#include "os/os_random.hpp"

#include <cstddef>

#include <stdlib.h>

namespace rsp::os {

void randomFill(uint8_t* buffer, uint32_t length) {
    if (buffer == nullptr || length == 0) {
        return;
    }

    ::arc4random_buf(buffer, static_cast<size_t>(length));
}

}  // namespace rsp::os