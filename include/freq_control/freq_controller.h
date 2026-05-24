// Public C++ API for applying and reverting CPU/GPU/devfreq frequency
// constraints on Android devices.
//
// Typical usage:
//
//     freq_control::FreqController ctl;
//     ctl.SetFrequencyMode(freq_control::FrequencyMode::kPerformance);
//     ctl.SetClocks();      // writes the profile, saving previous values
//     /* ... do work ... */
//     ctl.UnsetClocks();    // restores previous values
//
// All sysfs writes must be performed by a process that has write access to
// the target nodes (typically `root` or `system`). Calls return false on
// the first I/O failure; previously-applied writes are *not* rolled back
// automatically, but UnsetClocks() can be called to restore whatever was
// successfully saved.

#ifndef FREQ_CONTROL_FREQ_CONTROLLER_H_
#define FREQ_CONTROL_FREQ_CONTROLLER_H_

#include <cstdint>
#include <memory>

#include "freq_control/device_config.h"

namespace freq_control {

class FreqController {
 public:
  FreqController();
  ~FreqController();

  FreqController(const FreqController&) = delete;
  FreqController& operator=(const FreqController&) = delete;

  // Selects a named profile. Returns false if the SoC config does not
  // declare a profile for `mode`.
  bool SetFrequencyMode(FrequencyMode mode);

  // Selects a custom-id profile. Returns false if no profile with this id
  // exists in the SoC config.
  bool SetFrequencyCustomId(uint32_t id);

  // Saves the current min/max values of every rail in the selected profile,
  // then writes the profile values. Returns false if no profile has been
  // selected, or on the first I/O failure encountered.
  bool SetClocks();

  // Writes the values saved by the most-recent SetClocks() call. After a
  // successful UnsetClocks(), the saved state is cleared. Returns false if
  // there is no saved state to restore, or on I/O failure.
  bool UnsetClocks();

  // Whether SetClocks() has run and not been followed by UnsetClocks().
  bool clocks_applied() const;

  // Returns true iff the currently-selected profile declares a Tunable
  // with this name (regardless of whether it is sysfs-bound).
  bool HasTunable(const char* name) const;

  // Returns the value of the named Tunable in the currently-selected
  // profile, or `fallback` if no profile is selected or no such Tunable
  // is declared. Comparison is case-sensitive.
  uint64_t GetTunable(const char* name, uint64_t fallback = 0) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace freq_control

#endif  // FREQ_CONTROL_FREQ_CONTROLLER_H_
