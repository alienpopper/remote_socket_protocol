#include "os/os_fileops.hpp"

namespace rsp::os {

std::FILE* openFile(const std::string& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

}  // namespace rsp::os