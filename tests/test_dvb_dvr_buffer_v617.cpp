#include "media/DvbDvrBuffer.h"
#include <cassert>
#include <cerrno>
#include <iostream>

int main() {
    using namespace dvbstreamer5::media::network;
    int calls = 0;
    // Emulate all four independent adapters active on home2.
    for (int adapter = 0; adapter < 4; ++adapter) {
        const int dvrFd = 100 + adapter;
        const auto result = configureDvbDvrBuffer(
            dvrFd, [&](int fd, unsigned long request, unsigned long bytes) {
                ++calls;
                assert(fd == dvrFd);
                assert(request == static_cast<unsigned long>(DMX_SET_BUFFER_SIZE));
                assert(bytes == kDvbDvrBufferBytes);
                return 0;
            });
        assert(result.success);
        assert(result.error == 0);
    }
    assert(calls == 4);

    // An unsupported ioctl must be reported, not terminate a working DVB tap.
    errno = 0;
    const auto unsupported = configureDvbDvrBuffer(
        200, [&](int fd, unsigned long request, unsigned long bytes) {
            ++calls;
            assert(fd == 200);
            assert(request == static_cast<unsigned long>(DMX_SET_BUFFER_SIZE));
            assert(bytes == kDvbDvrBufferBytes);
            errno = ENOTTY;
            return -1;
        });
    assert(!unsupported.success && unsupported.error == ENOTTY);
    assert(calls == 5);

    const auto invalid = configureDvbDvrBuffer(
        -1, [&](int, unsigned long, unsigned long) {
            ++calls;
            return 0;
        });
    assert(!invalid.success && invalid.error == EBADF);
    assert(calls == 5);

    std::cout << "PASS: V617 4 DVB DVR buffers, ioctl error fallback and invalid fd\n";
}
