#include "freq_control/freq_controller.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sysfs_io.h"

namespace freq_control {

namespace {

// One saved rail value, captured at SetClocks() time so UnsetClocks() can
// restore it.
struct SavedRail {
  const char* min_path;
  const char* max_path;
  uint64_t prev_min;  // ignored if min_path is empty
  uint64_t prev_max;  // ignored if max_path is empty
  bool has_min;
  bool has_max;
};

// One saved tunable value, for sysfs-bound tunables.
struct SavedTunable {
  const char* sysfs_path;
  uint64_t prev_value;
};

bool PathPresent(const char* p) {
  return p != nullptr && p[0] != '\0';
}

// Applies (min, max) to a rail, ordering writes so neither side temporarily
// violates the kernel's `min <= max` invariant. Either side may be skipped by
// passing a 0 frequency for it.
bool ApplyRail(const RailTarget& target) {
  const bool want_min = target.min_khz != 0 && PathPresent(target.min_path);
  const bool want_max = target.max_khz != 0 && PathPresent(target.max_path);
  if (!want_min && !want_max) {
    return true;
  }
  if (want_max && PathPresent(target.max_path)) {
    uint64_t current_min = 0;
    if (PathPresent(target.min_path) && ReadSysfsUint64(target.min_path, &current_min) &&
        target.max_khz < current_min) {
      if (!WriteSysfsUint64(target.min_path, target.min_khz != 0 ? target.min_khz : target.max_khz)) {
        return false;
      }
      return WriteSysfsUint64(target.max_path, target.max_khz);
    }
  }
  if (want_max && !WriteSysfsUint64(target.max_path, target.max_khz)) {
    return false;
  }
  if (want_min && !WriteSysfsUint64(target.min_path, target.min_khz)) {
    return false;
  }
  return true;
}

}  // namespace

struct FreqController::Impl {
  const RailTarget* selected_targets = nullptr;
  size_t selected_count = 0;
  const Tunable* selected_tunables = nullptr;
  size_t selected_tunable_count = 0;
  const char* selected_name = nullptr;
  std::vector<SavedRail> saved_rails;
  std::vector<SavedTunable> saved_tunables;

  void ClearSelection() {
    selected_targets = nullptr;
    selected_count = 0;
    selected_tunables = nullptr;
    selected_tunable_count = 0;
    selected_name = nullptr;
  }

  bool HasSelection() const { return selected_targets != nullptr || selected_tunables != nullptr; }
};

FreqController::FreqController() : impl_(std::make_unique<Impl>()) {}

FreqController::~FreqController() {
  if (clocks_applied()) {
    UnsetClocks();
  }
}

bool FreqController::SetFrequencyMode(FrequencyMode mode) {
  const DeviceConfig& cfg = GetDeviceConfig();
  for (size_t i = 0; i < cfg.mode_count; ++i) {
    if (cfg.modes[i].mode == mode) {
      impl_->selected_targets = cfg.modes[i].targets;
      impl_->selected_count = cfg.modes[i].target_count;
      impl_->selected_tunables = cfg.modes[i].tunables;
      impl_->selected_tunable_count = cfg.modes[i].tunable_count;
      impl_->selected_name = cfg.modes[i].name;
      return true;
    }
  }
  std::fprintf(stderr, "freq_control: mode %u not declared in SoC config '%s'\n",
               static_cast<unsigned>(mode), cfg.soc_name);
  return false;
}

bool FreqController::SetFrequencyCustomId(uint32_t id) {
  const DeviceConfig& cfg = GetDeviceConfig();
  for (size_t i = 0; i < cfg.custom_count; ++i) {
    if (cfg.custom_profiles[i].id == id) {
      impl_->selected_targets = cfg.custom_profiles[i].targets;
      impl_->selected_count = cfg.custom_profiles[i].target_count;
      impl_->selected_tunables = cfg.custom_profiles[i].tunables;
      impl_->selected_tunable_count = cfg.custom_profiles[i].tunable_count;
      impl_->selected_name = cfg.custom_profiles[i].name;
      return true;
    }
  }
  std::fprintf(stderr, "freq_control: custom id %u not declared in SoC config '%s'\n", id,
               cfg.soc_name);
  return false;
}

bool FreqController::SetClocks() {
  if (!impl_->HasSelection()) {
    std::fprintf(stderr, "freq_control: SetClocks called without a profile selected\n");
    return false;
  }
  std::vector<SavedRail> saved_rails;
  saved_rails.reserve(impl_->selected_count);
  for (size_t i = 0; i < impl_->selected_count; ++i) {
    const RailTarget& t = impl_->selected_targets[i];
    SavedRail s{};
    s.min_path = t.min_path;
    s.max_path = t.max_path;
    if (t.min_khz != 0 && PathPresent(t.min_path)) {
      s.has_min = ReadSysfsUint64(t.min_path, &s.prev_min);
      if (!s.has_min) return false;
    }
    if (t.max_khz != 0 && PathPresent(t.max_path)) {
      s.has_max = ReadSysfsUint64(t.max_path, &s.prev_max);
      if (!s.has_max) return false;
    }
    saved_rails.push_back(s);
  }

  std::vector<SavedTunable> saved_tunables;
  saved_tunables.reserve(impl_->selected_tunable_count);
  for (size_t i = 0; i < impl_->selected_tunable_count; ++i) {
    const Tunable& t = impl_->selected_tunables[i];
    if (!PathPresent(t.sysfs_path)) continue;
    SavedTunable st{};
    st.sysfs_path = t.sysfs_path;
    if (!ReadSysfsUint64(t.sysfs_path, &st.prev_value)) return false;
    saved_tunables.push_back(st);
  }

  for (size_t i = 0; i < impl_->selected_count; ++i) {
    if (!ApplyRail(impl_->selected_targets[i])) {
      impl_->saved_rails = std::move(saved_rails);
      impl_->saved_tunables = std::move(saved_tunables);
      return false;
    }
  }
  for (size_t i = 0; i < impl_->selected_tunable_count; ++i) {
    const Tunable& t = impl_->selected_tunables[i];
    if (!PathPresent(t.sysfs_path)) continue;
    if (!WriteSysfsUint64(t.sysfs_path, t.value)) {
      impl_->saved_rails = std::move(saved_rails);
      impl_->saved_tunables = std::move(saved_tunables);
      return false;
    }
  }

  impl_->saved_rails = std::move(saved_rails);
  impl_->saved_tunables = std::move(saved_tunables);
  return true;
}

bool FreqController::UnsetClocks() {
  if (impl_->saved_rails.empty() && impl_->saved_tunables.empty()) {
    std::fprintf(stderr, "freq_control: UnsetClocks called with no saved state\n");
    return false;
  }
  bool ok = true;
  for (const SavedRail& s : impl_->saved_rails) {
    RailTarget restore{};
    restore.min_path = s.min_path;
    restore.max_path = s.max_path;
    restore.min_khz = s.has_min ? s.prev_min : 0;
    restore.max_khz = s.has_max ? s.prev_max : 0;
    if (!ApplyRail(restore)) ok = false;
  }
  for (const SavedTunable& s : impl_->saved_tunables) {
    if (!WriteSysfsUint64(s.sysfs_path, s.prev_value)) ok = false;
  }
  impl_->saved_rails.clear();
  impl_->saved_tunables.clear();
  return ok;
}

bool FreqController::clocks_applied() const {
  return !impl_->saved_rails.empty() || !impl_->saved_tunables.empty();
}

bool FreqController::HasTunable(const char* name) const {
  if (name == nullptr || impl_->selected_tunables == nullptr) return false;
  for (size_t i = 0; i < impl_->selected_tunable_count; ++i) {
    if (std::strcmp(impl_->selected_tunables[i].name, name) == 0) return true;
  }
  return false;
}

uint64_t FreqController::GetTunable(const char* name, uint64_t fallback) const {
  if (name == nullptr || impl_->selected_tunables == nullptr) return fallback;
  for (size_t i = 0; i < impl_->selected_tunable_count; ++i) {
    if (std::strcmp(impl_->selected_tunables[i].name, name) == 0) {
      return impl_->selected_tunables[i].value;
    }
  }
  return fallback;
}

}  // namespace freq_control
