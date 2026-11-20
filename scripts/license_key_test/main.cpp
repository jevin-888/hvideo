#include "core/LicenseVerify.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

// Exercise the production verifier and the production CMake key definition.
// Tests only log pass/fail, never the key or the user's authorization contents.
hsvj::Logger::Level hsvj::Logger::getLevel() { return FATAL_LEVEL; }
void hsvj::Logger::log(Level, const char*, const char*, ...) {}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    static_assert(sizeof(HVIDEO_LICENSE_HMAC_KEY) > 1, "Missing test key");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    std::string encoded((std::istreambuf_iterator<char>(input)), {});
    auto decoded = hsvj::licenseBase64Decode(encoded);
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(decoded.data(), decoded.data() + decoded.size(), &root, &errors)) return 2;
    const std::string key = HVIDEO_LICENSE_HMAC_KEY;
    if (!hsvj::verifyLicenseSignature(root, key)) return 3;
    std::cout << "PASS original license with configured key\n";
    if (hsvj::verifyLicenseSignature(root, "")) return 4;
    std::cout << "PASS empty key rejected\n";
    auto modified = root;
    modified["expiry_time"] = Json::Int64(0);
    if (hsvj::verifyLicenseSignature(modified, key)) return 5;
    std::cout << "PASS modified payload rejected\n";
    modified = root;
    modified["signature"] = std::string(64, '0');
    if (hsvj::verifyLicenseSignature(modified, key)) return 6;
    std::cout << "PASS modified signature rejected\n";
    return 0;
}
