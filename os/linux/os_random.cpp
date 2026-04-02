#include "os/os_random.hpp"

#include <algorithm>

namespace rsp::os {

void randomFill(uint8_t* buffer, uint32_t length) {
    if (buffer == nullptr) {
        return;
    }

    std::fill(buffer, buffer + length, static_cast<uint8_t>(0));
}

}  // namespace rsp::os