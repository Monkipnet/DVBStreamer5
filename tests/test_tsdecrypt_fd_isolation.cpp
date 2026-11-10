// Isolated regression for TSDecrypt child fd hygiene.
// No tuner, CAMD server or DVBStreamer5 service is required.
#include "../src/ca/backends/tsdecrypt/SpawnFdIsolation.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool probe(bool forceLegacy) {
    int sockets[2]{-1, -1};
    int output[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
    if (::pipe(output) != 0) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return false;
    }
    const unsigned int upper =
        dvbstreamer5::ca::tsdecrypt_detail::inheritedFdLimit();
    const pid_t child = ::fork();
    if (child == 0) {
        if (::dup2(output[1], STDOUT_FILENO) < 0) _exit(11);
        if (forceLegacy) {
            dvbstreamer5::ca::tsdecrypt_detail::closeInheritedFdsLegacy(upper);
        } else {
            dvbstreamer5::ca::tsdecrypt_detail::closeInheritedFds(upper);
        }
        if (::fcntl(sockets[0], F_GETFD) != -1 || errno != EBADF) _exit(12);
        if (::fcntl(sockets[1], F_GETFD) != -1 || errno != EBADF) _exit(13);
        if (::write(STDOUT_FILENO, "OK", 2) != 2) _exit(14);
        _exit(0);
    }
    ::close(output[1]);
    ::close(sockets[0]);
    ::close(sockets[1]);
    if (child < 0) {
        ::close(output[0]);
        return false;
    }
    char buffer[2]{};
    const ssize_t n = ::read(output[0], buffer, sizeof(buffer));
    ::close(output[0]);
    int status = 0;
    if (::waitpid(child, &status, 0) != child) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        n == 2 && std::memcmp(buffer, "OK", 2) == 0;
}

int main() {
    if (!probe(false)) {
        std::fprintf(stderr, "TSDecrypt fd isolation failed (normal path)\n");
        return 1;
    }
    if (!probe(true)) {
        std::fprintf(stderr, "TSDecrypt fd isolation failed (kernel 5.4 fallback)\n");
        return 1;
    }
    return 0;
}
