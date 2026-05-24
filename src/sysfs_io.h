// Thin sysfs read/write helpers. Sysfs nodes hold a single ASCII value;
// these functions encapsulate parsing and writing decimal integers.

#ifndef FREQ_CONTROL_SRC_SYSFS_IO_H_
#define FREQ_CONTROL_SRC_SYSFS_IO_H_

#include <cstdint>
#include <string>

namespace freq_control {

// Reads a uint64 from `path`. On success returns true and writes the value to
// `*out`. Whitespace and a single trailing newline are tolerated.
bool ReadSysfsUint64(const char* path, uint64_t* out);

// Writes `value` (decimal, with trailing newline) to `path`. The file is
// opened with O_WRONLY and a single write() is issued so sysfs handlers see
// the whole value at once.
bool WriteSysfsUint64(const char* path, uint64_t value);

// Returns true iff `path` exists and is readable.
bool SysfsExists(const char* path);

}  // namespace freq_control

#endif  // FREQ_CONTROL_SRC_SYSFS_IO_H_
