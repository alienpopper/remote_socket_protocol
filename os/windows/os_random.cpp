#include "os/os_random.hpp"

#include <stdexcept>

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace rsp::os {

void randomFill(uint8_t* buffer, uint32_t length) {
    if (buffer == nullptr || length == 0) {
        return;
    }

    const NTSTATUS status = ::BCryptGenRandom(nullptr, buffer, length, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
}

}  // namespace rsp::os