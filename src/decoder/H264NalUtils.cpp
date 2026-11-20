#include "decoder/H264NalUtils.h"

#include <cstddef>
#include <iterator>

#include <algorithm>

namespace hsvj {
namespace {

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

uint32_t readBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

bool startCodeAt(const uint8_t* data, size_t size, size_t offset,
                 size_t& codeSize) {
  codeSize = 0;
  if (!data || offset + 3 > size || data[offset] != 0 ||
      data[offset + 1] != 0) {
    return false;
  }
  if (data[offset + 2] == 1) {
    codeSize = 3;
    return true;
  }
  if (offset + 4 <= size && data[offset + 2] == 0 && data[offset + 3] == 1) {
    codeSize = 4;
    return true;
  }
  return false;
}

bool findStartCode(const uint8_t* data, size_t size, size_t from,
                   size_t& offset, size_t& codeSize) {
  for (size_t i = from; i + 3 <= size; ++i) {
    if (startCodeAt(data, size, i, codeSize)) {
      offset = i;
      return true;
    }
  }
  return false;
}

bool isPlausibleNalHeader(uint8_t value) {
  const uint8_t type = value & 0x1f;
  return (value & 0x80) == 0 && type >= 1 && type <= 23;
}

} // namespace

std::vector<uint8_t> normalizeH264AnnexB(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    return {};
  }

  size_t firstStart = 0;
  size_t firstStartSize = 0;
  if (findStartCode(data, size, 0, firstStart, firstStartSize) &&
      firstStart == 0) {
    return std::vector<uint8_t>(data, data + size);
  }

  std::vector<uint8_t> annexB;
  size_t offset = 0;
  size_t nalCount = 0;
  while (offset + 4 <= size) {
    const uint32_t nalSize = readBe32(data + offset);
    offset += 4;
    if (nalSize == 0 || nalSize > size - offset ||
        !isPlausibleNalHeader(data[offset])) {
      annexB.clear();
      break;
    }
    annexB.insert(annexB.end(), std::begin(kStartCode), std::end(kStartCode));
    annexB.insert(annexB.end(), data + offset, data + offset + nalSize);
    offset += nalSize;
    ++nalCount;
  }
  if (nalCount > 0 && offset == size && !annexB.empty()) {
    return annexB;
  }

  if (isPlausibleNalHeader(data[0])) {
    annexB.assign(std::begin(kStartCode), std::end(kStartCode));
    annexB.insert(annexB.end(), data, data + size);
    return annexB;
  }

  return std::vector<uint8_t>(data, data + size);
}

bool containsH264NalType(const uint8_t* data, size_t size, uint8_t nalType) {
  std::vector<uint8_t> normalized = normalizeH264AnnexB(data, size);
  size_t start = 0;
  size_t codeSize = 0;
  while (findStartCode(normalized.data(), normalized.size(), start, start,
                       codeSize)) {
    const size_t header = start + codeSize;
    if (header < normalized.size() &&
        (normalized[header] & 0x1f) == (nalType & 0x1f)) {
      return true;
    }
    start = header + 1;
  }
  return false;
}

bool containsH264Idr(const uint8_t* data, size_t size) {
  return containsH264NalType(data, size, 5);
}

bool extractH264NalUnit(const uint8_t* data, size_t size, uint8_t nalType,
                        std::vector<uint8_t>& out) {
  out.clear();
  std::vector<uint8_t> normalized = normalizeH264AnnexB(data, size);
  size_t search = 0;
  size_t start = 0;
  size_t codeSize = 0;
  while (findStartCode(normalized.data(), normalized.size(), search, start,
                       codeSize)) {
    const size_t header = start + codeSize;
    if (header >= normalized.size()) {
      return false;
    }
    size_t next = normalized.size();
    size_t nextCodeSize = 0;
    findStartCode(normalized.data(), normalized.size(), header + 1, next,
                  nextCodeSize);
    if ((normalized[header] & 0x1f) == (nalType & 0x1f)) {
      out.assign(std::begin(kStartCode), std::end(kStartCode));
      out.insert(out.end(), normalized.begin() + static_cast<std::ptrdiff_t>(header),
                 normalized.begin() + static_cast<std::ptrdiff_t>(next));
      return true;
    }
    search = next;
  }
  return false;
}

} // namespace hsvj
