// Selects the per-SoC configuration at build time.
//
// Build with `-DFREQ_CONTROL_SOC=<soc>`, where <soc> matches the directory
// name and namespace inside config/soc/<soc>.h. For example:
//
//     -DFREQ_CONTROL_SOC=s5e9955
//
// causes this file to include config/soc/s5e9955.h and return
// `freq_control::s5e9955::kDeviceConfig`.

#include "freq_control/device_config.h"

#ifndef FREQ_CONTROL_SOC
#error "FREQ_CONTROL_SOC must be defined at build time, e.g. -DFREQ_CONTROL_SOC=s5e9955"
#endif

#define FREQ_CONTROL_STRINGIFY_(x) #x
#define FREQ_CONTROL_STRINGIFY(x) FREQ_CONTROL_STRINGIFY_(x)

#include FREQ_CONTROL_STRINGIFY(soc/FREQ_CONTROL_SOC.h)

namespace freq_control {

const DeviceConfig& GetDeviceConfig() {
  return FREQ_CONTROL_SOC::kDeviceConfig;
}

}  // namespace freq_control
