#pragma once
// V593: private OSCam-mini control for ONE existing reader.
// No HTTP port, credential in argv, shell invocation or daemon restart.
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>

namespace dvbstreamer5::oscam_reader_control {
inline constexpr const char* kSocketPath =
    "/run/dvbstreamer5-oscam-mini-reader.sock";
inline bool validLabel(const std::string& label) {
    if (label.empty() || label.size() > 64U) return false;
    // Match OSCam-mini's ASCII-only dvbstreamer5_reader_label_is_safe().
    // Locale-dependent std::isalnum() can admit names the daemon rejects.
    for (unsigned char c : label)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == '@'))
            return false;
    return true;
}
inline bool restartReader(const std::string& label, std::string& error,
                          const char* path = kSocketPath) {
    error.clear();
    if (!validLabel(label)) {
        error = "Invalid OSCam reader label";
        return false;
    }
    if (!path || !*path || std::strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        error = "Invalid OSCam reader control socket";
        return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = "Cannot open OSCam reader control socket";
        return false;
    }
    struct CloseSocket {
        int fd;
        ~CloseSocket() { if (fd >= 0) ::close(fd); }
    } closer{fd};
    timeval timeout{2,0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "OSCam-mini reader control unavailable (check service and installed version)";
        return false;
    }
    // Exactly one datagram/packet: peer performs its own root UID check.
    const std::string command = "restart " + label;
    if (::send(fd, command.data(), command.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(command.size())) {
        error = "OSCam reader control request failed";
        return false;
    }
    char reply[160]{};
    const auto received = ::recv(fd, reply, sizeof(reply)-1, 0);
    if (received <= 0) {
        error = "OSCam reader control response timed out";
        return false;
    }
    const std::string answer(reply, static_cast<std::size_t>(received));
    if (answer == "OK queued") return true;
    error = answer.rfind("ERROR ",0)==0 ? answer.substr(6) :
        "OSCam reader refused restart";
    return false;
}
}  // namespace dvbstreamer5::oscam_reader_control
