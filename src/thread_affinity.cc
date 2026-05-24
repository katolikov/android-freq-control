#include "freq_control/thread_affinity.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace freq_control {

namespace {

// Maximum CPU id the syscall path supports without falling back to a heap
// allocation. SoCs with > 64 CPUs are not currently a concern; bump and
// switch to a byte buffer if that ever changes.
constexpr uint32_t kMaxSupportedCpuId = 63;

const CpuClusterMap* FindCluster(CpuCluster cluster) {
  const DeviceConfig& cfg = GetDeviceConfig();
  for (size_t i = 0; i < cfg.cluster_count; ++i) {
    if (cfg.clusters[i].cluster == cluster) {
      return &cfg.clusters[i];
    }
  }
  return nullptr;
}

bool BuildMaskForCluster(const CpuClusterMap& map, uint64_t* mask_out) {
  uint64_t mask = 0;
  for (size_t i = 0; i < map.cpu_count; ++i) {
    const uint32_t id = map.cpu_ids[i];
    if (id > kMaxSupportedCpuId) {
      std::fprintf(stderr, "freq_control: cpu id %u exceeds supported max %u\n", id,
                   kMaxSupportedCpuId);
      return false;
    }
    mask |= (uint64_t{1} << id);
  }
  *mask_out = mask;
  return true;
}

bool ApplyMask(pid_t tid, uint64_t mask) {
  // syscall(SYS_sched_setaffinity, pid, cpusetsize, mask)
  //   pid == 0  -> current thread
  //   cpusetsize is in bytes
  long rc = ::syscall(__NR_sched_setaffinity, static_cast<long>(tid),
                      static_cast<unsigned>(sizeof(mask)), &mask);
  if (rc != 0) {
    std::fprintf(stderr, "freq_control: sched_setaffinity(tid=%d) failed: %s\n", tid,
                 std::strerror(errno));
    return false;
  }
  return true;
}

}  // namespace

bool SetThreadAffinity(CpuCluster cluster, pid_t tid) {
  const CpuClusterMap* map = FindCluster(cluster);
  if (map == nullptr) {
    std::fprintf(stderr, "freq_control: cluster %u not declared in SoC '%s'\n",
                 static_cast<unsigned>(cluster), GetDeviceConfig().soc_name);
    return false;
  }
  uint64_t mask = 0;
  if (!BuildMaskForCluster(*map, &mask)) return false;
  return ApplyMask(tid, mask);
}

bool ResetThreadAffinity(pid_t tid) {
  const DeviceConfig& cfg = GetDeviceConfig();
  if (cfg.cluster_count == 0) {
    std::fprintf(stderr, "freq_control: no clusters declared in SoC '%s'\n", cfg.soc_name);
    return false;
  }
  uint64_t mask = 0;
  for (size_t i = 0; i < cfg.cluster_count; ++i) {
    uint64_t part = 0;
    if (!BuildMaskForCluster(cfg.clusters[i], &part)) return false;
    mask |= part;
  }
  return ApplyMask(tid, mask);
}

}  // namespace freq_control
