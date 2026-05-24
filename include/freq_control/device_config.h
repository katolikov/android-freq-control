// Public types describing a per-SoC frequency configuration.
//
// A device configuration is a flat table of "rails" (CPU clusters, GPU, MIF,
// etc.) each with optional sysfs paths for the min-frequency and max-frequency
// constraint nodes. Profiles describe, for each rail, the (min_khz, max_khz)
// pair to apply when the profile is selected.
//
// Configurations are normally produced by tools/gen_device_config.py.

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

// One constraint to apply: write `min_khz` to `min_path` and `max_khz` to
// `max_path`. A nullptr/empty path means that side is not constrained. A 0
// frequency means "do not change that side".
struct RailTarget {
  const char* min_path;
  const char* max_path;
  uint64_t min_khz;
  uint64_t max_khz;
};

// A named profile: a list of rail targets keyed by FrequencyMode.
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

// One CPU cluster: maps a logical name to the kernel CPU ids that belong to
// it. Used by SetThreadAffinity to translate `kBig` etc. into a CPU mask.
enum class CpuCluster : uint32_t {
  kLittle = 0,
  kMid = 1,
  kBig = 2,
  kPrime = 3,
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

// Returns the compiled-in device configuration for the SoC selected at build
// time via -DFREQ_CONTROL_SOC=<name>. There is exactly one config per binary.
const DeviceConfig& GetDeviceConfig();

}  // namespace freq_control

#endif  // FREQ_CONTROL_DEVICE_CONFIG_H_
