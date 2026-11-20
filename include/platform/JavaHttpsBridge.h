#pragma once

#include <string>

// Executes an HTTPS GET through Android's HttpURLConnection and returns the
// JSON transport envelope produced by HSVJEngine. This is a C++-only bridge;
// callers must not expose it as a JNI ABI.
std::string callJavaHttpsGet(const std::string& url,
                             int connectTimeoutMs,
                             int readTimeoutMs);