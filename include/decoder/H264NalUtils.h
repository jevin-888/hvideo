#ifndef HSVJ_H264_NAL_UTILS_H
#define HSVJ_H264_NAL_UTILS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hsvj {

// Converts Annex-B, 4-byte AVCC, or a single raw H.264 NAL unit to Annex-B.
std::vector<uint8_t> normalizeH264AnnexB(const uint8_t* data, size_t size);

bool containsH264NalType(const uint8_t* data, size_t size, uint8_t nalType);
bool containsH264Idr(const uint8_t* data, size_t size);

// Returns the first complete Annex-B NAL unit of the requested type.
bool extractH264NalUnit(const uint8_t* data, size_t size, uint8_t nalType,
                        std::vector<uint8_t>& out);

} // namespace hsvj

#endif // HSVJ_H264_NAL_UTILS_H
