#pragma once

// Linux DVB exposes independent demux-filter and DVR-output ring buffers.
// DMX_OUT_TS_TAP feeds the DVR ring, so increasing a filter buffer alone
// does not protect the read(/dev/dvb/adapterN/dvr0) path from EOVERFLOW.
#ifdef __linux__
#include <cerrno>
#include <linux/dvb/dmx.h>
#include <sys/ioctl.h>

namespace dvbstreamer5::media::network {

inline constexpr unsigned long kDvbDvrBufferBytes = 8UL * 1024UL * 1024UL;

struct DvbDvrBufferResizeResult {
    bool success;
    int error;
};

// Injectable ioctl adapter makes the exact kernel request, target fd, and
// unsupported-driver fallback testable without opening physical DVB devices.
template <typename IoctlFn>
inline DvbDvrBufferResizeResult configureDvbDvrBuffer(
    int dvrFd, IoctlFn&& ioctlFn) {
    if (dvrFd < 0) return {false, EBADF};
    if (ioctlFn(dvrFd, static_cast<unsigned long>(DMX_SET_BUFFER_SIZE),
                kDvbDvrBufferBytes) == 0) {
        return {true, 0};
    }
    return {false, errno};
}

} // namespace dvbstreamer5::media::network
#endif
