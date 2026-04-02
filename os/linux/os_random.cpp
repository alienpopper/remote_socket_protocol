#include "os/os_random.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

namespace rsp::os {

void randomFill(uint8_t* buffer, uint32_t length) {
    if (buffer == nullptr) {
        return;
    }

    size_t filled = 0;

    while (filled < length) {
        const ssize_t count = ::getrandom(buffer + filled, static_cast<size_t>(length) - filled, 0);
        if (count > 0) {
            filled += static_cast<size_t>(count);
            continue;
        }

        if (count == -1 && errno == EINTR) {
            continue;
        }

        break;
    }

    if (filled == length) {
        return;
    }

    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd != -1) {
        while (filled < length) {
            const ssize_t count = ::read(fd, buffer + filled, static_cast<size_t>(length) - filled);
            if (count > 0) {
                filled += static_cast<size_t>(count);
                continue;
            }

            if (count == -1 && errno == EINTR) {
                continue;
            }

            break;
        }

        ::close(fd);
    }

    if (filled < length) {
        std::fill(buffer + filled, buffer + length, static_cast<uint8_t>(0));
    }
}

}  // namespace rsp::os