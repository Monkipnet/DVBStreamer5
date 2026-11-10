#pragma once
// V10.8.459: close every inherited descriptor except stdin/stdout/stderr
// in the TSDecrypt child before exec. Works on both new and older kernels.
#include <climits>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace dvbstreamer5::ca::tsdecrypt_detail {

inline unsigned int inheritedFdLimit() noexcept {
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
        limit.rlim_cur != RLIM_INFINITY) {
        return limit.rlim_cur > static_cast<rlim_t>(INT_MAX)
            ? static_cast<unsigned int>(INT_MAX)
            : static_cast<unsigned int>(limit.rlim_cur);
    }
    const long limitFromSysconf = ::sysconf(_SC_OPEN_MAX);
    return limitFromSysconf > 0
        ? static_cast<unsigned int>(limitFromSysconf > INT_MAX ? INT_MAX : limitFromSysconf)
        : 65536U;
}

// Only async-signal-safe syscalls are used after fork of a multithreaded host.
inline void closeInheritedFdsLegacy(unsigned int upperExclusive) noexcept {
    for (unsigned int fd = 3; fd < upperExclusive; ++fd) {
        (void)::close(static_cast<int>(fd));
    }
}

inline void closeInheritedFds(unsigned int upperExclusive) noexcept {
#if defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
    // Ubuntu 18.04 / kernel 5.4 do not provide close_range(2).
    closeInheritedFdsLegacy(upperExclusive);
}

} // namespace dvbstreamer5::ca::tsdecrypt_detail
