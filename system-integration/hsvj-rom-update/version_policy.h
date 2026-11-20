#pragma once

#include <string>

namespace hsvj_rom {
inline bool version_allowed(int current, int target, bool allow_downgrade) {
    return target > 0 && current > 0 && (target >= current || allow_downgrade);
}

inline bool upgrade_pending(const std::string& phase) {
    return phase == "armed" || phase == "installing" || phase == "installed";
}
}
