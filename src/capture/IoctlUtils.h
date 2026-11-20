#pragma once

#include <cerrno>
#include <sys/ioctl.h>

namespace hsvj {

inline int captureIoctl(int fd, unsigned long request, void *arg) {
  // EAGAIN belongs to the poll/recovery loop. Bound EINTR as well so shutdown
  // can regain control even if a device or signal source keeps interrupting.
  for (int attempt = 0; attempt < 8; ++attempt) {
    int result = ::ioctl(fd, request, arg);
    if (result != -1 || errno != EINTR) return result;
  }
  return -1;
}

} // namespace hsvj
