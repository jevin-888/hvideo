#ifndef HSVJ_CLOUD_MATERIAL_METADATA_H
#define HSVJ_CLOUD_MATERIAL_METADATA_H

#include <string>

namespace hsvj {
struct CloudMaterialMetadata {
    std::string id;
    std::string categoryId;
    std::string categoryName;
    std::string md5;
};
}

#endif
