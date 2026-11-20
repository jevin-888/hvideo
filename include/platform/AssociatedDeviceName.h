#ifndef HSVJ_PLATFORM_ASSOCIATED_DEVICE_NAME_H
#define HSVJ_PLATFORM_ASSOCIATED_DEVICE_NAME_H

#include <string>

// Applies one authoritative display name to every Android runtime name target.
// On non-Android builds this function returns false with an explanatory error.
bool applyJavaAssociatedDeviceName(const std::string& name, std::string& error);

#endif
