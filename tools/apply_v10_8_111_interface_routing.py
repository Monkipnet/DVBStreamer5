from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def regex_replace_once(text: str, pattern: str, replacement: str, label: str) -> str:
    text2, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one regex match, got {count}")
    return text2


# Version
p = Path("src/AppVersion.h")
s = p.read_text()
s = replace_once(s, 'kProgramVersion = "10.8.110"', 'kProgramVersion = "10.8.111"', "version")
p.write_text(s)

# StreamManager: preserve each output's own configured interface through RTSP and RTMP/YouTube.
p = Path("src/StreamManager.cpp")
s = p.read_text()
s = replace_once(
    s,
    '    struct RtspOutputSpec { std::string host; int port=8554; };\n    std::vector<RtspOutputSpec> rtspOutputSpecs;\n    struct RtmpOutputSpec { std::string uri; };',
    '    struct RtspOutputSpec { std::string host; int port=8554; std::string iface; };\n    std::vector<RtspOutputSpec> rtspOutputSpecs;\n    struct RtmpOutputSpec { std::string uri; std::string iface; };',
    "output spec interface fields")
s = replace_once(
    s,
    'if (type == "rtsp") { rtspOutputSpecs.push_back({host, port > 0 ? port : 8554}); return true; }',
    'if (type == "rtsp") { rtspOutputSpecs.push_back({host, port > 0 ? port : 8554, cleanInterface(iface)}); return true; }',
    "RTSP output spec")
s = replace_once(
    s,
    'rtmpOutputSpecs.push_back({uri}); return true;',
    'rtmpOutputSpecs.push_back({uri, cleanInterface(iface)}); return true;',
    "RTMP output spec")
s = replace_once(
    s,
    'dvbstreamer5::media::rtsp::OutputConfig cfg; cfg.bindAddress=cleanInterface(spec.host); if(cfg.bindAddress.empty())cfg.bindAddress="0.0.0.0"; cfg.port=spec.port; cfg.streamName=streamConfig.id;',
    'dvbstreamer5::media::rtsp::OutputConfig cfg; cfg.bindAddress=cleanInterface(spec.iface); if(cfg.bindAddress.empty())cfg.bindAddress=cleanInterface(spec.host); if(cfg.bindAddress.empty())cfg.bindAddress="0.0.0.0"; cfg.port=spec.port; cfg.streamName=streamConfig.id;',
    "RTSP output bind")
s = replace_once(
    s,
    'dvbstreamer5::media::rtmp::EndpointConfig cfg; cfg.uri=spec.uri; cfg.bindAddress=cleanInterface(streamConfig.interfaceAddress);',
    'dvbstreamer5::media::rtmp::EndpointConfig cfg; cfg.uri=spec.uri; cfg.bindAddress=cleanInterface(spec.iface);',
    "RTMP output bind")
s = replace_once(
    s,
    '        state->nativeRtspOutputs.push_back(std::move(output));',
    '        std::cerr << "RTSP OUT effective stream=" << streamConfig.id\n                  << " local_bind=" << cfg.bindAddress\n                  << " port=" << cfg.port << std::endl;\n        state->nativeRtspOutputs.push_back(std::move(output));',
    "RTSP effective log")
s = replace_once(
    s,
    '        state->nativeRtmpOutputs.push_back(std::move(output));',
    '        std::cerr << "RTMP OUT effective stream=" << streamConfig.id\n                  << " local_bind=" << (cfg.bindAddress.empty() ? std::string("auto") : cfg.bindAddress)\n                  << std::endl;\n        state->nativeRtmpOutputs.push_back(std::move(output));',
    "RTMP effective log")
s = replace_once(
    s,
    '        // Apply VPS/VDS tuning last so the normal per-output latency cannot\n        // overwrite the optimization profile after URI parsing.\n        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);\n        if (streamConfig.srtVpsVdsOptimization) {',
    '        // Apply VPS/VDS tuning last so the normal per-output latency cannot\n        // overwrite the optimization profile after URI parsing.\n        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);\n        std::cerr << "SRT OUT route stream=" << streamConfig.id\n                  << " mode=" << srtConfig.mode\n                  << " local_bind=" << (srtConfig.bindAddress.empty() ? std::string("auto") : srtConfig.bindAddress)\n                  << " host=" << srtConfig.host\n                  << " port=" << srtConfig.port << std::endl;\n        if (streamConfig.srtVpsVdsOptimization) {',
    "SRT route log")
p.write_text(s)

# RTMP: EndpointConfig::bindAddress existed but the socket layer ignored it.
p = Path("src/media/NativeRtmpTransport.cpp")
s = p.read_text()
old_pattern = r'''    bool connect\(const Url& u,int timeout,std::atomic<bool>\*stop,std::string&error\)\{.*?        return true;\n    \}\n    void close'''
new_connect = r'''    bool connect(const Url& u, const std::string& bindAddress, int timeout,
                 std::atomic<bool>* stop, std::string& error) {
        close();
        addrinfo h{}; h.ai_socktype = SOCK_STREAM; h.ai_family = AF_UNSPEC;
        addrinfo* l = nullptr;
        const int rc = ::getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &h, &l);
        if (rc) { error = gai_strerror(rc); return false; }

        sockaddr_storage local{};
        socklen_t localLen = 0;
        int bindFamily = AF_UNSPEC;
        const bool useBind = !bindAddress.empty() && bindAddress != "0.0.0.0" && bindAddress != "::";
        if (useBind) {
            sockaddr_in v4{}; v4.sin_family = AF_INET; v4.sin_port = 0;
            sockaddr_in6 v6{}; v6.sin6_family = AF_INET6; v6.sin6_port = 0;
            if (::inet_pton(AF_INET, bindAddress.c_str(), &v4.sin_addr) == 1) {
                std::memcpy(&local, &v4, sizeof(v4)); localLen = sizeof(v4); bindFamily = AF_INET;
            } else if (::inet_pton(AF_INET6, bindAddress.c_str(), &v6.sin6_addr) == 1) {
                std::memcpy(&local, &v6, sizeof(v6)); localLen = sizeof(v6); bindFamily = AF_INET6;
            } else {
                ::freeaddrinfo(l); error = "invalid RTMP local bind address"; return false;
            }
        }

        bool matchingFamily = !useBind;
        bool bindSucceeded = !useBind;
        int bindError = 0;
        for (auto* ai = l; ai && fd_ < 0; ai = ai->ai_next) {
            if (useBind && ai->ai_family != bindFamily) continue;
            matchingFamily = true;
            int sock = ::socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, ai->ai_protocol);
            if (sock < 0) continue;
            if (useBind) {
                if (::bind(sock, reinterpret_cast<const sockaddr*>(&local), localLen) != 0) {
                    bindError = errno; ::close(sock); continue;
                }
                bindSucceeded = true;
            }
            const int f = ::fcntl(sock, F_GETFL, 0);
            ::fcntl(sock, F_SETFL, f | O_NONBLOCK);
            if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) fd_ = sock;
            else if (errno == EINPROGRESS) {
                int left = timeout;
                while (left > 0 && !(stop && stop->load())) {
                    pollfd poll{sock, POLLOUT, 0};
                    const int slice = std::min(left, 200);
                    const int pr = ::poll(&poll, 1, slice); left -= slice;
                    if (pr > 0) {
                        int so = 0; socklen_t sl = sizeof(so);
                        ::getsockopt(sock, SOL_SOCKET, SO_ERROR, &so, &sl);
                        if (!so) fd_ = sock;
                        break;
                    }
                    if (pr < 0 && errno != EINTR) break;
                }
            }
            if (fd_ < 0) ::close(sock);
        }
        ::freeaddrinfo(l);
        if (fd_ < 0) {
            if (useBind && !matchingFamily) error = "RTMP local bind address family does not match destination";
            else if (useBind && !bindSucceeded && bindError) error = std::string("RTMP local bind failed: ") + std::strerror(bindError);
            else error = "RTMP connect failed";
            return false;
        }
        const int f = ::fcntl(fd_, F_GETFL, 0); ::fcntl(fd_, F_SETFL, f & ~O_NONBLOCK);
        if (u.tls) {
            ctx_ = SSL_CTX_new(TLS_client_method());
            if (!ctx_) { error = "RTMPS SSL_CTX failed"; close(); return false; }
            SSL_CTX_set_default_verify_paths(ctx_); ssl_ = SSL_new(ctx_); SSL_set_fd(ssl_, fd_);
            SSL_set_tlsext_host_name(ssl_, u.host.c_str());
            if (SSL_connect(ssl_) != 1) { error = "RTMPS TLS handshake failed"; close(); return false; }
        }
        return true;
    }
    void close'''
s = regex_replace_once(s, old_pattern, new_connect, "RTMP Io::connect")
s = replace_once(
    s,
    'impl_->io.connect(u,config.connectTimeoutMs,&impl_->stopping,error)',
    'impl_->io.connect(u,config.bindAddress,config.connectTimeoutMs,&impl_->stopping,error)',
    "RTMP output connect bind")
s = replace_once(
    s,
    'io.connect(u,config_.connectTimeoutMs,&stopping_,error)',
    'io.connect(u,config_.bindAddress,config_.connectTimeoutMs,&stopping_,error)',
    "RTMP input connect bind")
p.write_text(s)

# RTSP input: bind the TCP control connection to inputInterfaceAddress as well.
p = Path("src/media/NativeRtspTransport.cpp")
s = p.read_text()
old_pattern = r'''int connectTcp\(const Url& url, int timeoutMs, std::atomic<bool>\* stopping, std::string& error\) \{.*?    return fd;\n\}'''
new_connect = r'''int connectTcp(const Url& url, const std::string& bindAddress, int timeoutMs,
               std::atomic<bool>* stopping, std::string& error) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &list);
    if (rc != 0) { error = std::string("RTSP DNS failed: ") + gai_strerror(rc); return -1; }

    sockaddr_storage local{};
    socklen_t localLen = 0;
    int bindFamily = AF_UNSPEC;
    const bool useBind = !bindAddress.empty() && bindAddress != "0.0.0.0" && bindAddress != "::";
    if (useBind) {
        sockaddr_in v4{}; v4.sin_family = AF_INET; v4.sin_port = 0;
        sockaddr_in6 v6{}; v6.sin6_family = AF_INET6; v6.sin6_port = 0;
        if (::inet_pton(AF_INET, bindAddress.c_str(), &v4.sin_addr) == 1) {
            std::memcpy(&local, &v4, sizeof(v4)); localLen = sizeof(v4); bindFamily = AF_INET;
        } else if (::inet_pton(AF_INET6, bindAddress.c_str(), &v6.sin6_addr) == 1) {
            std::memcpy(&local, &v6, sizeof(v6)); localLen = sizeof(v6); bindFamily = AF_INET6;
        } else {
            ::freeaddrinfo(list); error = "invalid RTSP local bind address"; return -1;
        }
    }

    int fd = -1;
    bool matchingFamily = !useBind;
    bool bindSucceeded = !useBind;
    int bindError = 0;
    for (auto* ai = list; ai && fd < 0; ai = ai->ai_next) {
        if (useBind && ai->ai_family != bindFamily) continue;
        matchingFamily = true;
        int sock = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (sock < 0) continue;
        if (useBind) {
            if (::bind(sock, reinterpret_cast<const sockaddr*>(&local), localLen) != 0) {
                bindError = errno; ::close(sock); continue;
            }
            bindSucceeded = true;
        }
        const int flags = ::fcntl(sock, F_GETFL, 0);
        ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) fd = sock;
        else if (errno == EINPROGRESS) {
            pollfd poll{sock, POLLOUT, 0};
            int left = timeoutMs;
            while (left > 0 && !(stopping && stopping->load())) {
                const int slice = std::min(left, 200);
                const int pr = ::poll(&poll, 1, slice); left -= slice;
                if (pr > 0) {
                    int so = 0; socklen_t sl = sizeof(so);
                    ::getsockopt(sock, SOL_SOCKET, SO_ERROR, &so, &sl);
                    if (so == 0) fd = sock;
                    break;
                }
                if (pr < 0 && errno != EINTR) break;
            }
        }
        if (fd < 0) ::close(sock);
    }
    ::freeaddrinfo(list);
    if (fd < 0) {
        if (useBind && !matchingFamily) error = "RTSP local bind address family does not match destination";
        else if (useBind && !bindSucceeded && bindError) error = std::string("RTSP local bind failed: ") + std::strerror(bindError);
        else error = "RTSP TCP connect failed";
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0); ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}'''
s = regex_replace_once(s, old_pattern, new_connect, "RTSP connectTcp")
s = replace_once(
    s,
    'int fd = connectTcp(url, config_.connectTimeoutMs, &stopping_, error);',
    'int fd = connectTcp(url, config_.bindAddress, config_.connectTimeoutMs, &stopping_, error);',
    "RTSP input control bind")
p.write_text(s)

print("V10.8.111 interface routing transform applied")
