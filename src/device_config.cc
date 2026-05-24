// Runtime device selection. The registry table is provided by the
// generated header freq_control/device_registry.h; this file implements
// the active-device state and the lookup API declared in
// freq_control/device_config.h.

#include "freq_control/device_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace freq_control {

namespace {

// The active device. Initialised to the first entry in the registry so
// SetActiveDevice is optional for single-device builds. Plain uint32_t
// (via the enum's underlying type) so concurrent reads on arm64 are
// trivially atomic; callers that retarget mid-flight should serialise.
Device g_active_device =
    (std::size(kDeviceRegistry) > 0) ? kDeviceRegistry[0].device : Device{};

const DeviceRegistryEntry* FindEntry(Device device) {
  for (const auto& e : kDeviceRegistry) {
    if (e.device == device) return &e;
  }
  return nullptr;
}

}  // namespace

bool SetActiveDevice(Device device) {
  if (FindEntry(device) == nullptr) {
    std::fprintf(stderr, "freq_control: device %u not registered\n",
                 static_cast<unsigned>(device));
    return false;
  }
  g_active_device = device;
  return true;
}

bool SetActiveDeviceByName(const char* soc_name) {
  if (soc_name == nullptr) return false;
  for (const auto& e : kDeviceRegistry) {
    if (std::strcmp(e.soc_name, soc_name) == 0) {
      g_active_device = e.device;
      return true;
    }
  }
  std::fprintf(stderr, "freq_control: no device named '%s'\n", soc_name);
  return false;
}

Device GetActiveDevice() { return g_active_device; }

const DeviceConfig& GetDeviceConfig() {
  return GetDeviceConfig(g_active_device);
}

const DeviceConfig& GetDeviceConfig(Device device) {
  const DeviceRegistryEntry* e = FindEntry(device);
  if (e == nullptr) {
    std::fprintf(stderr, "freq_control: device %u not in registry; aborting\n",
                 static_cast<unsigned>(device));
    std::abort();
  }
  return *e->config;
}

const DeviceRegistryEntry* DeviceRegistryBegin() {
  return std::begin(kDeviceRegistry);
}

const DeviceRegistryEntry* DeviceRegistryEnd() {
  return std::end(kDeviceRegistry);
}

}  // namespace freq_control
