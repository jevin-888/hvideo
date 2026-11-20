#ifndef HSVJ_PLATFORM_GROUP_CLOCK_BRIDGE_H
#define HSVJ_PLATFORM_GROUP_CLOCK_BRIDGE_H

#include <cstdint>

// (0, 0) revokes the current leader sample; other values pair UTC milliseconds
// with the equivalent local CLOCK_MONOTONIC nanoseconds.
extern "C" void callJavaOfferGroupLeaderTime(std::int64_t utcMs,
                                            std::int64_t localMonoNs);

#endif
