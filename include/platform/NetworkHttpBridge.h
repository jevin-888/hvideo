#pragma once
#include <string>

// Android per-Network HTTP(S) transport. Returns JSON {status, body}; empty on bridge failure.
std::string callJavaHttpRequestAcrossNetworks(const std::string& method, const std::string& url,
                                              const std::string& body, const std::string& token,
                                              int timeoutMs);

// Android per-Network streaming download. Returns a non-empty result only after
// the file is completely written by one of the available networks.
std::string callJavaFileDownloadAcrossNetworks(const std::string& url, const std::string& filePath,
                                               int timeoutMs);
