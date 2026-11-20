#pragma once

#include <string>

namespace hsvj {

// CAVE and matrix correction share one license; fusion uses its own module.
inline bool isCaveLicenseAction(const std::string& action) {
  return action == "get_projection_correction" ||
         action == "set_projection_correction" ||
         action == "save_projection_correction_config" ||
         action == "initialize_cave_wall" ||
         action == "get_region_geometry_correction" ||
         action == "set_region_geometry_correction" ||
         action == "get_cave_wall_config" ||
         action == "set_cave_wall_config";
}

} // namespace hsvj
