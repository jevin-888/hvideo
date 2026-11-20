#ifndef HSVJ_CLOUD_MATERIAL_FILENAME_H
#define HSVJ_CLOUD_MATERIAL_FILENAME_H

#include "utils/FileUtils.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace hsvj {

inline bool containsUtf8ReplacementCharacter(const std::string& value) {
    return value.find("\xef\xbf\xbd") != std::string::npos;
}

    // URL path segments use percent encoding, not form encoding: '+' stays '+'.
    inline std::string decodeCloudPathSegment(const std::string& value) {
        auto hex = [](unsigned char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string decoded;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                const int high = hex(value[i + 1]), low = hex(value[i + 2]);
                if (high >= 0 && low >= 0) {
                    decoded += static_cast<char>((high << 4) | low);
                    i += 2;
                    continue;
                }
            }
            decoded += value[i];
        }
        return decoded;
    }

    inline std::string safeCloudFilename(std::string name) {
        for (char& c : name) {
            if (static_cast<unsigned char>(c) < 32 || c == 127 ||
                c == '/' || c == '\\') c = '_';
        }
        if (name == "." || name == "..") name.clear();
        // Keep complete UTF-8 characters within the filesystem component limit.
        if (name.size() > 240) {
            size_t end = 240;
            while ((static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
            name.resize(end);
        }
        return name;
    }

    inline std::string cloudMaterialFilename(const std::string& materialId,
                                      const std::string& name,
                                      const std::string& cdnUrl) {
        const std::string pureUrl = cdnUrl.substr(0, cdnUrl.find_first_of("?#"));
        const std::string urlName = safeCloudFilename(
            decodeCloudPathSegment(FileUtils::getFilename(pureUrl)));
        std::string filename = safeCloudFilename(name);
        if (containsUtf8ReplacementCharacter(filename)) filename.clear();
        if (filename.empty()) filename = urlName;
        if (filename.empty()) filename = safeCloudFilename(materialId);
        if (filename.empty()) filename = "material";
        // Cloud display names may omit the suffix; retain the real media type.
        std::string ext = FileUtils::getExtension(urlName);
        if (ext.empty() || ext.size() > 10 ||
            !std::all_of(ext.begin(), ext.end(), [](unsigned char c) { return std::isalnum(c); })) ext = "mp4";
        std::string nameExt = FileUtils::getExtension(filename);
        std::transform(nameExt.begin(), nameExt.end(), nameExt.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string supported = "|mp4|hvideo|mov|mkv|avi|wmv|flv|ts|webm|mpg|mpeg|m4v|mp3|wav|flac|aac|ogg|m4a|wma|jpg|jpeg|png|gif|bmp|webp|";
        if (nameExt.empty() || supported.find("|" + nameExt + "|") == std::string::npos) {
            filename += "." + ext;
        }
        return filename;
    }

} // namespace hsvj

#endif
