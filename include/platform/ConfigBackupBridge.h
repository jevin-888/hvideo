#pragma once
#include <string>

// Returns a JSON result. Java owns ZIP validation, staging and rollback.
std::string callJavaConfigBackup(const std::string& operation,
                                const std::string& root,
                                const std::string& work);
