#include "sysfs_io.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace freq_control {

namespace {

constexpr size_t kReadBufferSize = 64;

void LogSysfsError(const char* op, const char* path, int err) {
  std::fprintf(stderr, "freq_control: %s(%s) failed: %s\n", op, path, std::strerror(err));
}

}  // namespace

bool ReadSysfsUint64(const char* path, uint64_t* out) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    LogSysfsError("open", path, errno);
    return false;
  }
  char buf[kReadBufferSize];
  ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  int saved_errno = errno;
  ::close(fd);
  if (n <= 0) {
    LogSysfsError("read", path, saved_errno);
    return false;
  }
  buf[n] = '\0';
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(buf, &end, 10);
  if (end == buf) {
    std::fprintf(stderr, "freq_control: parse(%s) failed: %s\n", path, buf);
    return false;
  }
  *out = static_cast<uint64_t>(parsed);
  return true;
}

bool WriteSysfsUint64(const char* path, uint64_t value) {
  int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    LogSysfsError("open", path, errno);
    return false;
  }
  char buf[kReadBufferSize];
  int len = std::snprintf(buf, sizeof(buf), "%llu\n", static_cast<unsigned long long>(value));
  if (len <= 0 || static_cast<size_t>(len) >= sizeof(buf)) {
    ::close(fd);
    std::fprintf(stderr, "freq_control: format(%llu) failed\n",
                 static_cast<unsigned long long>(value));
    return false;
  }
  ssize_t written = ::write(fd, buf, static_cast<size_t>(len));
  int saved_errno = errno;
  ::close(fd);
  if (written != len) {
    LogSysfsError("write", path, saved_errno);
    return false;
  }
  return true;
}

bool SysfsExists(const char* path) {
  return path != nullptr && path[0] != '\0' && ::access(path, F_OK) == 0;
}

}  // namespace freq_control
