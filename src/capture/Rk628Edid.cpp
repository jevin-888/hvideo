#include "capture/Rk628Edid.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <mutex>
#include <stdexcept>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace hsvj {
namespace {
using Bytes = std::vector<uint8_t>;
std::mutex edidMutex;
constexpr const char* stateName = "rk628-edid.json";

CaptureInputMode decode(const uint8_t* d) {
  const int clock = (d[0] | (d[1] << 8)) * 10000;
  if (!clock) return {};
  const int w = d[2] | ((d[4] & 0xf0) << 4);
  const int h = d[5] | ((d[7] & 0xf0) << 4);
  const int ht = w + (d[3] | ((d[4] & 0x0f) << 8));
  const int vt = h + (d[6] | ((d[7] & 0x0f) << 8));
  return {w, h, ht && vt ? static_cast<double>(clock) / ht / vt : 0};
}

std::vector<size_t> timingSlots(const Bytes& data) {
  std::vector<size_t> slots{54, 72, 90, 108};
  for (size_t block = 128; block < data.size(); block += 128) {
    if (data[block] != 2 || data[block + 2] < 4) continue;
    for (size_t pos = block + data[block + 2]; pos + 18 <= block + 127; pos += 18)
      slots.push_back(pos);
  }
  return slots;
}

bool sameTiming(const uint8_t* a, const uint8_t* b) {
  return std::equal(a, a + 12, b) && std::equal(a + 15, a + 18, b + 15);
}

std::string hex(const Bytes& data) {
  const char digits[] = "0123456789abcdef";
  std::string value;
  for (uint8_t byte : data) {
    value += digits[byte >> 4];
    value += digits[byte & 15];
  }
  return value;
}

Bytes unhex(const std::string& text) {
  if (text.size() < 256 || text.size() > 8192 || text.size() % 256)
    throw std::runtime_error("RK628 EDID 备份长度无效");
  Bytes data;
  auto digit = [](char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    throw std::runtime_error("RK628 EDID 备份内容无效");
  };
  for (size_t i = 0; i < text.size(); i += 2)
    data.push_back(static_cast<uint8_t>(digit(text[i]) * 16 + digit(text[i + 1])));
  Rk628Edid::validate(data);
  return data;
}

struct Saved {
  bool exists = false;
  bool custom = false;
  int width = 0, height = 0, refreshRate = 0;
  bool singleMode = false;
  Bytes original;
};

Saved load(const std::string& directory) {
  const auto path = std::filesystem::path(directory) / stateName;
  if (!std::filesystem::exists(path)) return {};
  if (std::filesystem::file_size(path) > 16384)
    throw std::runtime_error("RK628 EDID 配置文件过大");
  std::ifstream stream(path);
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string error;
  if (!stream || !Json::parseFromStream(builder, stream, &root, &error) ||
      !root.isObject() || !root["version"].isInt() || root["version"].asInt() != 1 ||
      !root["original"].isString() || !root["custom"].isBool())
    throw std::runtime_error("RK628 EDID 配置损坏，请保留备份后检查");
  Saved saved;
  saved.exists = true;
  saved.custom = root["custom"].asBool();
  saved.original = unhex(root["original"].asString());
  if (saved.custom) {
    if (!root["width"].isInt() || !root["height"].isInt() || !root["refreshRate"].isInt())
      throw std::runtime_error("RK628 EDID 自定义参数损坏");
    saved.width = root["width"].asInt();
    saved.height = root["height"].asInt();
    saved.refreshRate = root["refreshRate"].asInt();
    saved.singleMode = root["singleMode"].isBool() && root["singleMode"].asBool();
    Rk628Edid::customTiming(saved.width, saved.height, saved.refreshRate);
  }
  return saved;
}

void save(const std::string& directory, const Saved& state) {
  Json::Value root;
  root["version"] = 1;
  root["original"] = hex(state.original);
  root["custom"] = state.custom;
  if (state.custom) {
    root["width"] = state.width;
    root["height"] = state.height;
    root["refreshRate"] = state.refreshRate;
    root["singleMode"] = state.singleMode;
  }
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  const std::string content = Json::writeString(builder, root) + "\n";
#ifdef _WIN32
  (void)directory;
  (void)content;
  throw std::runtime_error("当前平台不支持 RK628 EDID");
#else
  std::string temp = (std::filesystem::path(directory) / "rk628-edid.tmp-XXXXXX").string();
  const int fd = mkstemp(temp.data());
  if (fd < 0) throw std::runtime_error("无法保存 RK628 EDID 配置");
  size_t offset = 0;
  while (offset < content.size()) {
    const auto count = ::write(fd, content.data() + offset, content.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<size_t>(count);
  }
  const bool flushed = offset == content.size() && fsync(fd) == 0;
  close(fd);
  const auto path = (std::filesystem::path(directory) / stateName).string();
  if (!flushed || rename(temp.c_str(), path.c_str()) != 0) {
    unlink(temp.c_str());
    throw std::runtime_error("无法原子保存 RK628 EDID 配置");
  }
  const int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir >= 0) { fsync(dir); close(dir); }
#endif
}

struct ConfigLock {
#ifndef _WIN32
  int fd = -1;
#endif
  explicit ConfigLock(const std::string& directory) {
#ifndef _WIN32
    std::filesystem::create_directories(directory);
    const auto path = (std::filesystem::path(directory) / "rk628-edid.lock").string();
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("无法锁定 RK628 EDID 配置");
    while (flock(fd, LOCK_EX) < 0) {
      if (errno == EINTR) continue;
      close(fd); fd = -1;
      throw std::runtime_error("无法锁定 RK628 EDID 配置");
    }
#else
    (void)directory;
#endif
  }
  ~ConfigLock() {
#ifndef _WIN32
    if (fd >= 0) close(fd);
#endif
  }
};

class LinuxDevice final : public CaptureEdidDevice {
  struct Fd {
    int value = -1;
    Fd() {
#ifndef _WIN32
      std::ifstream name("/sys/class/video4linux/v4l-subdev2/name");
      std::string text;
      std::getline(name, text);
      if (text.find("rk628") == std::string::npos)
        throw std::runtime_error("当前设备没有 RK628 采集芯片");
      value = open("/dev/v4l-subdev2", O_RDWR | O_CLOEXEC);
      if (value < 0) throw std::runtime_error("无法访问 RK628，请检查设备权限");
#else
      throw std::runtime_error("当前平台不支持 RK628 EDID");
#endif
    }
    ~Fd() {
#ifndef _WIN32
      if (value >= 0) close(value);
#endif
    }
  };
public:
  Bytes read() override {
    Fd fd;
#ifndef _WIN32
    Bytes data(4096);
    v4l2_edid edid{};
    edid.blocks = static_cast<uint32_t>(data.size() / 128);
    edid.edid = data.data();
    int result;
    do { result = ioctl(fd.value, VIDIOC_G_EDID, &edid); } while (result < 0 && errno == EINTR);
    if (result < 0 || !edid.blocks || edid.blocks > data.size() / 128)
      throw std::runtime_error("RK628 EDID 读取失败");
    data.resize(edid.blocks * 128);
    Rk628Edid::validate(data);
    return data;
#else
    return {};
#endif
  }
  void write(const Bytes& data) override {
    Rk628Edid::validate(data);
    Fd fd;
#ifndef _WIN32
    v4l2_edid edid{};
    edid.blocks = static_cast<uint32_t>(data.size() / 128);
    edid.edid = const_cast<uint8_t*>(data.data());
    int result;
    do { result = ioctl(fd.value, VIDIOC_S_EDID, &edid); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::runtime_error("RK628 拒绝 EDID 写入");
#endif
  }
  CaptureInputMode inputMode() override {
    Fd fd;
#ifndef _WIN32
    v4l2_control power{};
    power.id = V4L2_CID_DV_RX_POWER_PRESENT;
    if (ioctl(fd.value, VIDIOC_G_CTRL, &power) != 0 || !power.value) return {};
    v4l2_dv_timings timings{};
    if (ioctl(fd.value, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &timings) != 0) return {};
    const auto& bt = timings.bt;
    const uint64_t ht = static_cast<uint64_t>(bt.width) + bt.hfrontporch + bt.hsync + bt.hbackporch;
    const uint64_t vt = static_cast<uint64_t>(bt.height) + bt.vfrontporch + bt.vsync + bt.vbackporch;
    return {static_cast<int>(bt.width), static_cast<int>(bt.height),
            ht && vt ? static_cast<double>(bt.pixelclock) / ht / vt : 0};
#else
    return {};
#endif
  }
};

Bytes target(const Saved& state) {
  if (!state.custom) return state.original;
  return state.singleMode
      ? Rk628Edid::withSingleMode(state.original, state.width, state.height,
                                  state.refreshRate)
      : Rk628Edid::withPreferred(state.original, state.width, state.height,
                                 state.refreshRate);
}

CaptureEdidState status(const Saved& saved, const Bytes& current, CaptureEdidDevice& device) {
  CaptureEdidState state;
  state.custom = saved.custom;
  state.singleMode = saved.singleMode;
  if (saved.custom)
    state.requested = {saved.width, saved.height, static_cast<double>(saved.refreshRate)};
  state.applied = !saved.exists || current == target(saved);
  state.preferred = decode(current.data() + 54);
  state.input = device.inputMode();
  return state;
}

void writeVerified(CaptureEdidDevice& device, const Bytes& bytes) {
  device.write(bytes);
  if (device.read() != bytes) throw std::runtime_error("RK628 EDID 写入后回读不一致");
}
} // namespace

Rk628Edid::Rk628Edid(std::string configDirectory, std::shared_ptr<CaptureEdidDevice> device)
    : directory_(std::move(configDirectory)), device_(device ? std::move(device) : std::make_shared<LinuxDevice>()) {}

void Rk628Edid::validate(const Bytes& data) {
  const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
  if (data.size() < 128 || data.size() > 4096 || data.size() % 128 ||
      !std::equal(std::begin(header), std::end(header), data.begin()) ||
      data[126] + 1u != data.size() / 128 || data[18] != 1 || data[19] < 3)
    throw std::runtime_error("RK628 EDID 格式不支持或数据损坏");
  for (size_t block = 0; block < data.size(); block += 128) {
    unsigned sum = 0;
    for (size_t i = block; i < block + 128; ++i) sum += data[i];
    if (sum % 256) throw std::runtime_error("RK628 EDID 校验和错误");
    if (block && data[block] == 2 && data[block + 2] &&
        (data[block + 2] < 4 || data[block + 2] > 127))
      throw std::runtime_error("RK628 CTA 数据偏移无效");
  }
}

std::array<uint8_t, 18> Rk628Edid::customTiming(int width, int height, int refreshRate) {
  if (width < 320 || width > 4088 || width % 8 || height < 240 || height > 2160 ||
      refreshRate < 24 || refreshRate > 60)
    throw std::invalid_argument("宽度需为320–4088且为8的倍数，高度240–2160，刷新率24–60Hz整数");
  const int64_t period = (1000000000LL - 460000LL * refreshRate) / (height * refreshRate);
  const int sync = width * 3 == height * 4 ? 4 : width * 9 == height * 16 ? 5 :
                   width * 10 == height * 16 ? 6 :
                   (width * 4 == height * 5 || width * 9 == height * 15) ? 7 : 10;
  const int blank = std::max<int>(460000 / period + 1, 3 + sync + 6);
  const int64_t clock = (width + 160LL) * 1000000000LL / period / 250000 * 250000;
  if (clock > 297000000)
    throw std::invalid_argument("该模式像素时钟超过当前采集设置上限297MHz，请降低分辨率或刷新率");
  std::array<uint8_t, 18> d{};
  const int units = static_cast<int>(clock / 10000);
  d[0] = units & 255; d[1] = units >> 8;
  d[2] = width & 255; d[3] = 160; d[4] = (width >> 8) << 4;
  d[5] = height & 255; d[6] = blank & 255;
  d[7] = ((height >> 8) << 4) | (blank >> 8);
  d[8] = 48; d[9] = 32; d[10] = (3 << 4) | sync;
  d[17] = 0x1a;
  return d;
}

Bytes Rk628Edid::withPreferred(const Bytes& original, int width, int height, int refreshRate) {
  validate(original);
  const auto timing = customTiming(width, height, refreshRate);
  Bytes result = original;
  const auto slots = timingSlots(original);
  size_t destination = 0;
  if (original[54] || original[55]) {
    for (size_t pos : slots)
      if (sameTiming(timing.data(), original.data() + pos)) { destination = pos; break; }
    if (destination) {
      std::copy(original.begin() + 54, original.begin() + 72, result.begin() + destination);
    } else {
      // Reuse only an empty descriptor or a duplicate timing; keep identity,
      // range descriptors and every unique original timing for fallback.
      for (size_t pos : slots) {
        if (pos == 54) continue;
        if (std::all_of(original.begin() + pos, original.begin() + pos + 18,
                        [](uint8_t value) { return value == 0; })) { destination = pos; break; }
        if (!original[pos] && !original[pos + 1]) continue;
        for (size_t other : slots) {
          if (other != pos && sameTiming(original.data() + pos, original.data() + other)) {
            destination = pos; break;
          }
        }
        if (destination) break;
      }
      if (!destination) throw std::runtime_error("EDID 没有空闲或重复时序槽，保留原模式后无法添加");
      std::copy(original.begin() + 54, original.begin() + 72, result.begin() + destination);
    }
  } else {
    throw std::runtime_error("EDID 首个描述符不是时序，无法安全修改首选模式");
  }
  std::copy(timing.begin(), timing.end(), result.begin() + 54);
  result[24] |= 2;
  // Range descriptor must cover the advertised CVT-RB preferred timing.
  const auto mode = decode(timing.data());
  for (size_t pos : {54u, 72u, 90u, 108u}) {
    if (result[pos] || result[pos + 1] || result[pos + 3] != 0xfd) continue;
    result[pos + 5] = std::min<int>(result[pos + 5], std::floor(mode.refreshRate));
    result[pos + 6] = std::max<int>(result[pos + 6], std::ceil(mode.refreshRate));
    const int horizontal = (timing[0] | (timing[1] << 8)) * 10 / (width + 160);
    result[pos + 7] = std::min<int>(result[pos + 7], horizontal);
    result[pos + 8] = std::max<int>(result[pos + 8], horizontal + 1);
    result[pos + 9] = std::max<int>(result[pos + 9], ((timing[0] | (timing[1] << 8)) + 999) / 1000);
  }
  for (size_t block = 128; block < result.size(); block += 128) {
    if (result[block] != 2) continue;
    result[block + 3] &= 0xf0;
    const size_t end = result[block + 2] ? block + result[block + 2] : block + 127;
    for (size_t pos = block + 4; pos < end;) {
      const uint8_t header = result[pos++];
      const size_t length = header & 31;
      if (pos + length > end) throw std::runtime_error("CTA 数据块长度无效");
      if ((header >> 5) == 2)
        for (size_t i = pos; i < pos + length; ++i)
          if (result[i] >= 129 && result[i] <= 192) result[i] &= 127;
      pos += length;
    }
  }
  for (size_t block = 0; block < result.size(); block += 128) {
    unsigned sum = 0;
    for (size_t pos = block; pos < block + 127; ++pos) sum += result[pos];
    result[block + 127] = static_cast<uint8_t>(-sum);
  }
  validate(result);
  return result;
}

Bytes Rk628Edid::withSingleMode(const Bytes& original, int width, int height,
                                int refreshRate) {
  validate(original);
  const auto timing = customTiming(width, height, refreshRate);
  Bytes result = original;
  std::fill(result.begin() + 35, result.begin() + 54, 0);
  std::copy(timing.begin(), timing.end(), result.begin() + 54);
  for (size_t pos : {72u, 90u, 108u}) {
    if (result[pos + 3] == 0xfd || result[pos + 3] == 0xfc ||
        result[pos + 3] == 0xff || result[pos + 3] == 0xfe ||
        result[pos + 3] == 0xfb) {
      continue;
    }
    std::fill(result.begin() + pos, result.begin() + pos + 18, 0);
  }

  for (size_t block = 128; block < result.size(); block += 128) {
    if (result[block] != 2) continue;
    const size_t oldEnd = result[block + 2] ? block + result[block + 2] : block + 127;
    if (oldEnd < block + 4 || oldEnd > block + 127)
      throw std::runtime_error("CTA 数据偏移无效");
    Bytes dataBlocks;
    for (size_t pos = block + 4; pos < oldEnd;) {
      const uint8_t header = result[pos++];
      const size_t length = header & 31;
      if (pos + length > oldEnd) throw std::runtime_error("CTA 数据块长度无效");
      const uint8_t tag = header >> 5;
      const bool videoModeBlock = tag == 2 ||
          (tag == 7 && length > 0 &&
           (result[pos] == 0x0e || result[pos] == 0x0f));
      if (!videoModeBlock) {
        dataBlocks.push_back(header);
        dataBlocks.insert(dataBlocks.end(), result.begin() + pos,
                          result.begin() + pos + length);
      }
      pos += length;
    }
    if (dataBlocks.size() > 123)
      throw std::runtime_error("CTA 保留数据超出 EDID 容量");
    std::fill(result.begin() + block + 4, result.begin() + block + 127, 0);
    std::copy(dataBlocks.begin(), dataBlocks.end(), result.begin() + block + 4);
    // CTA offsets are relative to the start of this 128-byte extension block.
    result[block + 2] = static_cast<uint8_t>(4 + dataBlocks.size());
    result[block + 3] &= 0xf0;
  }

  for (size_t block = 0; block < result.size(); block += 128) {
    unsigned sum = 0;
    for (size_t pos = block; pos < block + 127; ++pos) sum += result[pos];
    result[block + 127] = static_cast<uint8_t>(-sum);
  }
  validate(result);
  return result;
}

CaptureEdidState Rk628Edid::get() {
  std::lock_guard<std::mutex> lock(edidMutex);
  // Serialize reads with another Android process' write/commit transaction.
  ConfigLock fileLock(directory_);
  const auto saved = load(directory_);
  auto current = device_->read();
  validate(current);
  return status(saved, current, *device_);
}

CaptureEdidState Rk628Edid::set(bool custom, int width, int height, int refreshRate,
                                bool singleMode) {
  // Validate before creating files or accessing hardware.
  if (custom) customTiming(width, height, refreshRate);
  std::lock_guard<std::mutex> lock(edidMutex);
  ConfigLock fileLock(directory_);
  auto saved = load(directory_);
  const auto current = device_->read();
  validate(current);
  if (!custom && !saved.exists) return status(saved, current, *device_);
  if (!saved.exists) { saved.exists = true; saved.original = current; }
  auto next = saved;
  next.custom = custom;
  next.singleMode = custom && singleMode;
  next.width = width; next.height = height; next.refreshRate = refreshRate;
  const auto desired = target(next);
  // A durable original backup must exist before changing receiver EDID.
  if (!std::filesystem::exists(std::filesystem::path(directory_) / stateName)) save(directory_, saved);
  try {
    if (current != desired) writeVerified(*device_, desired);
    save(directory_, next);
  } catch (const std::exception& error) {
    const std::string reason = error.what();
    try { writeVerified(*device_, current); }
    catch (...) { throw std::runtime_error(reason + "；恢复写入也失败，请使用保存的原始EDID检查"); }
    throw std::runtime_error(reason + "；已恢复此前EDID");
  }
  return status(next, desired, *device_);
}

void Rk628Edid::restoreSaved() {
  std::lock_guard<std::mutex> lock(edidMutex);
  if (!std::filesystem::exists(std::filesystem::path(directory_) / stateName)) return;
  ConfigLock fileLock(directory_);
  const auto saved = load(directory_);
  const auto desired = target(saved);
  const auto current = device_->read();
  if (current == desired) return;
  try { writeVerified(*device_, desired); }
  catch (...) {
    try { writeVerified(*device_, current); } catch (...) { /* Preserve original exception. */ }
    throw;
  }
}
} // namespace hsvj
