// Static types describing a per-SoC frequency configuration plus the
// runtime device-selection API.
//
// Every supported SoC is compiled into the binary. Pick which one is
// active at runtime via SetActiveDevice / SetActiveDeviceByName; the
// FreqController and SetThreadAffinity APIs then operate against that
// device's tables.
//
// The generated header `freq_control/device_registry.h` (included at the
// bottom of this file) supplies the `Device` enum values and the device
// registry table. Re-run tools/gen_device_config.py to regenerate it.

#ifndef FREQ_CONTROL_DEVICE_CONFIG_H_
#define FREQ_CONTROL_DEVICE_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace freq_control {

// Predefined named frequency profiles. The integer values are stable.
enum class FrequencyMode : uint32_t {
  kPowerSave = 0,
  kBalanced = 1,
  kPerformance = 2,
  kBoost = 3,
  kMaximum = 4,
};

// Logical CPU clusters; per-SoC mapping to kernel CPU ids is in the
// device's CpuClusterMap table.
enum class CpuCluster : uint32_t {
  kLittle = 0,
  kMid = 1,
  kBig = 2,
  kPrime = 3,
};

// One constraint to apply: write `min_khz` to `min_path` and `max_khz` to
// `max_path`. A nullptr/empty path means that side is not constrained. A 0
// frequency means "do not change that side".
struct RailTarget {
  const char* min_path;
  const char* max_path;
  uint64_t min_khz;
  uint64_t max_khz;
};

// A named profile keyed by FrequencyMode.
struct ModeProfile {
  FrequencyMode mode;
  const char* name;
  const RailTarget* targets;
  size_t target_count;
};

// A profile keyed by an opaque uint32_t id. Use these for ad-hoc workloads
// (e.g. one-off ML burst settings) that do not fit a named mode.
struct CustomProfile {
  uint32_t id;
  const char* name;
  const RailTarget* targets;
  size_t target_count;
};

struct CpuClusterMap {
  CpuCluster cluster;
  const char* name;
  const uint32_t* cpu_ids;
  size_t cpu_count;
};

struct DeviceConfig {
  const char* soc_name;
  const ModeProfile* modes;
  size_t mode_count;
  const CustomProfile* custom_profiles;
  size_t custom_count;
  const CpuClusterMap* clusters;
  size_t cluster_count;
};

// Defined in the generated freq_control/device_registry.h. Declared up here
// so DeviceRegistryEntry and the API functions below can reference it.
enum class Device : uint32_t;

struct DeviceRegistryEntry {
  Device device;
  const char* soc_name;
  const DeviceConfig* config;
};

// Selects the active device. Subsequent FreqController and thread-affinity
// calls use this device's tables. Returns false if `device` is not
// registered (i.e. its SoC header was not compiled in).
bool SetActiveDevice(Device device);

// Same as SetActiveDevice but resolves by SoC name (e.g. "s5e9955").
// Useful at startup after reading `ro.soc.model`. Returns false if the
// name is not registered.
bool SetActiveDeviceByName(const char* soc_name);

// Returns the currently active device. Initially this is the first entry
// in the device registry.
Device GetActiveDevice();

// Returns the config for the currently active device.
const DeviceConfig& GetDeviceConfig();

// Returns the config for a specific device. Aborts if `device` is not
// registered (caller error: the enum value must come from the registry).
const DeviceConfig& GetDeviceConfig(Device device);

// Iteration over the compiled-in device list.
const DeviceRegistryEntry* DeviceRegistryBegin();
const DeviceRegistryEntry* DeviceRegistryEnd();

}  // namespace freq_control

// The generated registry header completes the `Device` enum and provides
// the table that the runtime API resolves against. Keeping the include at
// the bottom means users include only this single header to get
// everything (types + API + enum values).
#include "freq_control/device_registry.h"

#endif  // FREQ_CONTROL_DEVICE_CONFIG_H_
