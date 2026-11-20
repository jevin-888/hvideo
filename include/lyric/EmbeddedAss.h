#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace hsvj {

// Publishes cumulative, parseable ASS snapshots while reading; the return value
// is the complete document, or empty on absence, cancellation or failure.
// onProgress runs on the reader thread and must not access render resources.
std::string readEmbeddedAss(const std::string& videoPath, const std::atomic<bool>& cancelled,
                           const std::function<void(std::string)>& onProgress);

}
