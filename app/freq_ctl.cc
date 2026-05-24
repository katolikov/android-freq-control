// Small CLI driver. Useful for manual verification on a device:
//
//     freq_ctl devices                                    # list compiled-in SoCs
//     freq_ctl [--device <name>] list                     # show profiles
//     freq_ctl [--device <name>] probe                    # check sysfs paths
//     freq_ctl [--device <name>] set      <mode|id>       # SetFrequencyMode/Id + SetClocks
//     freq_ctl [--device <name>] cycle    <mode|id> <secs>
//     freq_ctl [--device <name>] affinity <little|mid|big|prime>
//     freq_ctl [--device <name>] reset-affinity
//
// `--device` selects which compiled-in SoC the command operates on. If
// omitted, the first device in the registry is used.

#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "freq_control/device_config.h"
#include "freq_control/freq_controller.h"
#include "freq_control/thread_affinity.h"
#include "../src/sysfs_io.h"

namespace {

void PrintRail(const freq_control::RailTarget& t) {
  std::printf("    min[%s] = %llu kHz, max[%s] = %llu kHz\n",
              t.min_path ? t.min_path : "(none)",
              static_cast<unsigned long long>(t.min_khz),
              t.max_path ? t.max_path : "(none)",
              static_cast<unsigned long long>(t.max_khz));
}

void PrintTunable(const freq_control::Tunable& t) {
  if (t.sysfs_path) {
    std::printf("    tunable %s = %llu (sysfs: %s)\n", t.name,
                static_cast<unsigned long long>(t.value), t.sysfs_path);
  } else {
    std::printf("    tunable %s = %llu (app-readable)\n", t.name,
                static_cast<unsigned long long>(t.value));
  }
}

int CmdList() {
  const auto& cfg = freq_control::GetDeviceConfig();
  std::printf("soc: %s\n", cfg.soc_name);
  std::printf("modes:\n");
  for (size_t i = 0; i < cfg.mode_count; ++i) {
    const auto& m = cfg.modes[i];
    std::printf("  %s (mode=%u, %zu rails, %zu tunables)\n", m.name,
                static_cast<unsigned>(m.mode), m.target_count, m.tunable_count);
    for (size_t j = 0; j < m.target_count; ++j) PrintRail(m.targets[j]);
    for (size_t j = 0; j < m.tunable_count; ++j) PrintTunable(m.tunables[j]);
  }
  std::printf("custom:\n");
  for (size_t i = 0; i < cfg.custom_count; ++i) {
    const auto& c = cfg.custom_profiles[i];
    std::printf("  id=%u name=%s (%zu rails, %zu tunables)\n", c.id, c.name, c.target_count,
                c.tunable_count);
    for (size_t j = 0; j < c.target_count; ++j) PrintRail(c.targets[j]);
    for (size_t j = 0; j < c.tunable_count; ++j) PrintTunable(c.tunables[j]);
  }
  return 0;
}

int CmdProbe() {
  const auto& cfg = freq_control::GetDeviceConfig();
  int missing = 0;
  auto check = [&](const char* path) {
    if (!path || path[0] == '\0') return;
    bool ok = freq_control::SysfsExists(path);
    uint64_t v = 0;
    bool read_ok = ok && freq_control::ReadSysfsUint64(path, &v);
    std::printf("  %s  exists=%d  readable=%d  value=%llu\n", path, ok, read_ok,
                static_cast<unsigned long long>(v));
    if (!ok) ++missing;
  };
  for (size_t i = 0; i < cfg.mode_count; ++i) {
    const auto& m = cfg.modes[i];
    std::printf("[%s]\n", m.name);
    for (size_t j = 0; j < m.target_count; ++j) {
      check(m.targets[j].min_path);
      check(m.targets[j].max_path);
    }
    for (size_t j = 0; j < m.tunable_count; ++j) {
      if (m.tunables[j].sysfs_path != nullptr) check(m.tunables[j].sysfs_path);
    }
  }
  std::printf("missing paths: %d\n", missing);
  return missing == 0 ? 0 : 2;
}

bool ResolveProfile(const char* arg, freq_control::FreqController* ctl) {
  const auto& cfg = freq_control::GetDeviceConfig();
  for (size_t i = 0; i < cfg.mode_count; ++i) {
    if (std::strcmp(cfg.modes[i].name, arg) == 0) {
      return ctl->SetFrequencyMode(cfg.modes[i].mode);
    }
  }
  char* end = nullptr;
  unsigned long long id = std::strtoull(arg, &end, 10);
  if (end != arg && *end == '\0') {
    return ctl->SetFrequencyCustomId(static_cast<uint32_t>(id));
  }
  std::fprintf(stderr, "unknown profile '%s'\n", arg);
  return false;
}

int CmdSet(const char* arg) {
  freq_control::FreqController ctl;
  if (!ResolveProfile(arg, &ctl)) return 1;
  return ctl.SetClocks() ? 0 : 1;
}

int CmdCycle(const char* arg, unsigned secs) {
  freq_control::FreqController ctl;
  if (!ResolveProfile(arg, &ctl)) return 1;
  if (!ctl.SetClocks()) return 1;
  std::printf("applied '%s', sleeping %u s...\n", arg, secs);
  ::sleep(secs);
  return ctl.UnsetClocks() ? 0 : 1;
}

bool ParseCluster(const char* arg, freq_control::CpuCluster* out) {
  const auto& cfg = freq_control::GetDeviceConfig();
  for (size_t i = 0; i < cfg.cluster_count; ++i) {
    if (std::strcmp(cfg.clusters[i].name, arg) == 0) {
      *out = cfg.clusters[i].cluster;
      return true;
    }
  }
  return false;
}

int CmdAffinity(const char* arg) {
  freq_control::CpuCluster cluster;
  if (!ParseCluster(arg, &cluster)) {
    std::fprintf(stderr, "unknown cluster '%s'\n", arg);
    return 1;
  }
  // The pid==0 path means "current thread"; this lets us verify the syscall
  // without spawning a worker.
  if (!freq_control::SetThreadAffinity(cluster)) return 1;
  // Read back via /proc/self/status so the caller can see the mask landed.
  std::printf("affinity set to cluster '%s'; verifying with sched_getcpu()=%d\n", arg,
              ::sched_getcpu());
  return 0;
}

int CmdResetAffinity() {
  if (!freq_control::ResetThreadAffinity()) return 1;
  std::printf("affinity reset; sched_getcpu()=%d\n", ::sched_getcpu());
  return 0;
}

int CmdDevices() {
  std::printf("compiled-in devices (active: %s):\n",
              freq_control::GetDeviceConfig().soc_name);
  for (auto it = freq_control::DeviceRegistryBegin();
       it != freq_control::DeviceRegistryEnd(); ++it) {
    std::printf("  [%u] %s\n", static_cast<unsigned>(it->device), it->soc_name);
  }
  return 0;
}

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  freq_ctl devices\n"
               "  freq_ctl [--device <name>] list\n"
               "  freq_ctl [--device <name>] probe\n"
               "  freq_ctl [--device <name>] set            <mode_name|custom_id>\n"
               "  freq_ctl [--device <name>] cycle          <mode_name|custom_id> <seconds>\n"
               "  freq_ctl [--device <name>] affinity       <little|mid|big|prime>\n"
               "  freq_ctl [--device <name>] reset-affinity\n");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  int i = 1;
  if (i < argc && std::strcmp(argv[i], "--device") == 0) {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "--device requires a SoC name\n");
      return Usage();
    }
    if (!freq_control::SetActiveDeviceByName(argv[i + 1])) {
      return 1;
    }
    i += 2;
  }
  if (i >= argc) return Usage();

  std::string cmd = argv[i++];
  const int remaining = argc - i;

  if (cmd == "devices" && remaining == 0) return CmdDevices();
  if (cmd == "list" && remaining == 0) return CmdList();
  if (cmd == "probe" && remaining == 0) return CmdProbe();
  if (cmd == "set" && remaining == 1) return CmdSet(argv[i]);
  if (cmd == "cycle" && remaining == 2) {
    return CmdCycle(argv[i], static_cast<unsigned>(std::atoi(argv[i + 1])));
  }
  if (cmd == "affinity" && remaining == 1) return CmdAffinity(argv[i]);
  if (cmd == "reset-affinity" && remaining == 0) return CmdResetAffinity();
  return Usage();
}
