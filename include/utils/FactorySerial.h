#pragma once

#include <string>

namespace hsvj {
struct FactorySerialResult {
    int status = 200;
    std::string storedSerial;
    std::string error;
};

bool isValidFactorySerial(const std::string& serial);
FactorySerialResult readFactorySerial();
FactorySerialResult writeFactorySerial(const std::string& serial,
                                       const std::string& expectedSerial);
}
