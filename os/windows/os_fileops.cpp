#include "os/os_fileops.hpp"

namespace rsp::os {

std::FILE* openFile(const std::string& path, const char* mode) {
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), mode) != 0) {
        return nullptr;
    }

    return file;
}

}  // namespace rsp::os