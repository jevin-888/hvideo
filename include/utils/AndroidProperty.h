#ifndef HSVJ_ANDROID_PROPERTY_H
#define HSVJ_ANDROID_PROPERTY_H

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace hsvj {

inline int getAndroidIntProperty(const char *name, int fallback) {
#ifdef __ANDROID__
  if (!name) {
    return fallback;
  }
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
    return fallback;
  }
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || errno == ERANGE) {
    return fallback;
  }
  return static_cast<int>(parsed);
#else
  (void)name;
  return fallback;
#endif
}

inline bool getAndroidBooleanProperty(const char *name, bool fallback) {
#ifdef __ANDROID__
  if (!name) {
    return fallback;
  }
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
    return fallback;
  }
  return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0;
#else
  (void)name;
  return fallback;
#endif
}

} // namespace hsvj

#endif // HSVJ_ANDROID_PROPERTY_H
