#pragma once

#include <cstdio>
#include <string>

namespace rsp::os {

std::FILE* openFile(const std::string& path, const char* mode);

}  // namespace rsp::os