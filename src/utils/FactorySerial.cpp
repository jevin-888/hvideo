#include "utils/FactorySerial.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hsvj {
namespace {
std::mutex serialMutex;

// Rockchip rkflash_vendor_storage.h ABI. Only SN_ID (1) is exposed.
struct VendorRequest {
    uint32_t tag = 0x56524551;
    uint16_t id = 1;
    uint16_t len = 1024;
    unsigned char data[1024] = {};
};
static_assert(sizeof(VendorRequest) == 1032, "Rockchip vendor request ABI");
constexpr unsigned long ReadSerial = _IOW('v', 0x01, uint32_t);
constexpr unsigned long WriteSerial = _IOW('v', 0x02, uint32_t);

struct VendorDevice {
    int fd = open("/dev/vendor_storage", O_RDWR | O_CLOEXEC);
    ~VendorDevice() { if (fd >= 0) close(fd); }
};

FactorySerialResult unavailable() {
    return {503, {}, "无法访问出厂写号接口，请确认 ROM 支持 /dev/vendor_storage 且播放器具有读写权限"};
}

FactorySerialResult readSerial(int fd) {
    VendorRequest request;
    if (ioctl(fd, ReadSerial, &request) < 0) {
        // The Rockchip eMMC driver returns -1 (EPERM) for an absent SN item.
        // Other failures must never be interpreted as an empty factory record.
        if (errno == EPERM) return {};
        return {500, {}, "读取出厂序列号失败"};
    }
    if (request.len > sizeof(request.data)) return {500, {}, "出厂序列号记录长度异常"};
    size_t length = 0;
    while (length < request.len && request.data[length] != 0) ++length;
    if (!std::all_of(request.data + length, request.data + request.len,
                     [](unsigned char ch) { return ch == 0; })) {
        return {500, {}, "出厂序列号记录格式异常，请使用工厂工具检查"};
    }
    return {200, std::string(reinterpret_cast<char*>(request.data), length), {}};
}
}

bool isValidFactorySerial(const std::string& serial) {
    const auto letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    return !serial.empty() && serial.size() <= 14 && letter(serial.front()) &&
        std::all_of(serial.begin(), serial.end(), [&](unsigned char c) {
            return letter(c) || (c >= '0' && c <= '9');
        });
}

FactorySerialResult readFactorySerial() {
    std::lock_guard<std::mutex> lock(serialMutex);
    VendorDevice device;
    return device.fd < 0 ? unavailable() : readSerial(device.fd);
}

FactorySerialResult writeFactorySerial(const std::string& serial,
                                       const std::string& expectedSerial) {
    if (!isValidFactorySerial(serial)) {
        return {400, {}, "序列号须为 1–14 位英文字母或数字，并以英文字母开头"};
    }
    std::lock_guard<std::mutex> lock(serialMutex);
    VendorDevice device;
    if (device.fd < 0) return unavailable();
    auto before = readSerial(device.fd);
    if (before.status != 200) return before;
    if (before.storedSerial != expectedSerial) {
        return {409, {}, "出厂序列号已变化，请重新读取后再写入"};
    }
    if (before.storedSerial == serial) return before;
    VendorRequest request;
    request.len = static_cast<uint16_t>(serial.size());
    std::memcpy(request.data, serial.data(), serial.size());
    if (ioctl(device.fd, WriteSerial, &request) < 0) {
        return {500, {}, "写入未确认成功，请重新读取出厂序列号后核对，勿直接重复提交"};
    }
    auto after = readSerial(device.fd);
    if (after.status != 200 || after.storedSerial != serial) {
        return {500, {}, "已提交写入但回读校验失败，请重新读取并核对设备，勿直接重复提交"};
    }
    return after;
}
}
