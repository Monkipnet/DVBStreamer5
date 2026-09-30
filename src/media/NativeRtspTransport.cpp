#include "media/NativeRtspTransport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace dvbstreamer5::media::rtsp {
namespace {

using Clock = std::chrono::steady_clock;

std::string trim(std::string v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    return v;
}
std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return v;
}

struct Url {
    std::string scheme;
    std::string host;
    int port = 554;
    std::string path = "/";
    std::string user;
    std::string password;
};

bool parseUrl(const std::string& input, Url& out) {
    const auto sep = input.find("://");
    if (sep == std::string::npos) return false;
    Url u;
    u.scheme = lower(input.substr(0, sep));
    if (u.scheme != "rtsp" && u.scheme != "rtsps") return false;
    std::size_t authority = sep + 3;
    const auto pathPos = input.find('/', authority);
    std::string auth = input.substr(authority, pathPos == std::string::npos ? std::string::npos : pathPos - authority);
    u.path = pathPos == std::string::npos ? "/" : input.substr(pathPos);
    const auto at = auth.rfind('@');
    if (at != std::string::npos) {
        const std::string cred = auth.substr(0, at);
        auth.erase(0, at + 1);
        const auto colon = cred.find(':');
        u.user = cred.substr(0, colon);
        if (colon != std::string::npos) u.password = cred.substr(colon + 1);
    }
    if (!auth.empty() && auth.front() == '[') {
        const auto close = auth.find(']');
        if (close == std::string::npos) return false;
        u.host = auth.substr(1, close - 1);
        if (close + 1 < auth.size() && auth[close + 1] == ':') u.port = std::stoi(auth.substr(close + 2));
    } else {
        const auto colon = auth.rfind(':');
        if (colon != std::string::npos && auth.find(':') == colon) {
            u.host = auth.substr(0, colon);
            try { u.port = std::stoi(auth.substr(colon + 1)); } catch (...) { return false; }
        } else u.host = auth;
    }
    if (u.host.empty() || u.port <= 0 || u.port > 65535) return false;
    out = std::move(u);
    return true;
}

std::string base64(const std::string& value) {
    if (value.empty()) return {};
    std::string out(((value.size() + 2) / 3) * 4, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(value.data()),
                                  static_cast<int>(value.size()));
    if (n < 0) return {};
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::vector<std::uint8_t> decodeBase64(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c){ return std::isspace(c) != 0; }), value.end());
    if (value.empty()) return {};
    std::vector<std::uint8_t> out((value.size() / 4 + 1) * 3);
    const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
    if (n < 0) return {};
    std::size_t size = static_cast<std::size_t>(n);
    while (!value.empty() && value.back() == '=') { if (size) --size; value.pop_back(); }
    out.resize(size);
    return out;
}

int connectTcp(const Url& url, int timeoutMs, std::atomic<bool>* stopping, std::string& error) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &list);
    if (rc != 0) { error = std::string("RTSP DNS failed: ") + gai_strerror(rc); return -1; }
    int fd = -1;
    for (auto* ai = list; ai && fd < 0; ai = ai->ai_next) {
        int s = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (s < 0) continue;
        const int flags = ::fcntl(s, F_GETFL, 0);
        ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
        if (::connect(s, ai->ai_addr, ai->ai_addrlen) == 0) fd = s;
        else if (errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int left = timeoutMs;
            while (left > 0 && !(stopping && stopping->load())) {
                const int slice = std::min(left, 200);
                const int pr = ::poll(&p, 1, slice); left -= slice;
                if (pr > 0) {
                    int so = 0; socklen_t sl = sizeof(so); ::getsockopt(s, SOL_SOCKET, SO_ERROR, &so, &sl);
                    if (so == 0) fd = s;
                    break;
                }
                if (pr < 0 && errno != EINTR) break;
            }
        }
        if (fd < 0) ::close(s);
    }
    ::freeaddrinfo(list);
    if (fd < 0) { error = "RTSP TCP connect failed"; return -1; }
    const int flags = ::fcntl(fd, F_GETFL, 0); ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

bool sendAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    while (pos < size) {
        const ssize_t n = ::send(fd, data + pos, size - pos, MSG_NOSIGNAL);
        if (n > 0) { pos += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}
bool sendAll(int fd, const std::string& text) {
    return sendAll(fd, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

struct RtspResponse {
    int status = 0;
    std::map<std::string,std::string> headers;
    std::string body;
};

bool readResponse(int fd, std::string& pending, RtspResponse& response, int timeoutMs,
                  std::atomic<bool>* stopping, std::string& error) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto hdr = pending.find("\r\n\r\n");
        if (hdr != std::string::npos) {
            std::istringstream in(pending.substr(0, hdr));
            std::string line;
            if (!std::getline(in, line)) return false;
            line = trim(line);
            std::istringstream status(line); std::string version; status >> version >> response.status;
            std::size_t contentLength = 0;
            while (std::getline(in, line)) {
                line = trim(line); if (line.empty()) continue;
                const auto colon = line.find(':'); if (colon == std::string::npos) continue;
                const std::string name = lower(trim(line.substr(0, colon)));
                const std::string value = trim(line.substr(colon + 1));
                response.headers[name] = value;
                if (name == "content-length") { try { contentLength = std::stoul(value); } catch (...) {} }
            }
            const std::size_t total = hdr + 4 + contentLength;
            if (pending.size() >= total) {
                response.body = pending.substr(hdr + 4, contentLength);
                pending.erase(0, total);
                return true;
            }
        }
        if (stopping && stopping->load()) return false;
        const auto now = Clock::now(); if (now >= deadline) { error = "RTSP response timeout"; return false; }
        pollfd p{fd, POLLIN, 0};
        const int wait = std::min<int>(200, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));
        const int pr = ::poll(&p, 1, wait);
        if (pr == 0) continue;
        if (pr < 0) { if (errno == EINTR) continue; error = "RTSP poll failed"; return false; }
        char buffer[16384]; const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) { error = "RTSP server closed connection"; return false; }
        pending.append(buffer, static_cast<std::size_t>(n));
    }
}

bool request(int fd, const Url& url, int& cseq, const std::string& method, const std::string& target,
             const std::vector<std::pair<std::string,std::string>>& extra, const std::string& body,
             std::string& pending, RtspResponse& response, int timeoutMs,
             std::atomic<bool>* stopping, std::string& error) {
    std::ostringstream out;
    out << method << " " << target << " RTSP/1.0\r\nCSeq: " << cseq++ << "\r\nUser-Agent: DVBStreamer5/native-rtsp\r\n";
    if (!url.user.empty()) out << "Authorization: Basic " << base64(url.user + ":" + url.password) << "\r\n";
    for (const auto& [k,v] : extra) out << k << ": " << v << "\r\n";
    if (!body.empty()) out << "Content-Length: " << body.size() << "\r\nContent-Type: application/sdp\r\n";
    out << "\r\n" << body;
    if (!sendAll(fd, out.str())) { error = "RTSP request send failed"; return false; }
    response = {};
    if (!readResponse(fd, pending, response, timeoutMs, stopping, error)) return false;
    if (response.status < 200 || response.status >= 300) {
        error = method + " failed with RTSP status " + std::to_string(response.status);
        return false;
    }
    return true;
}

std::string absoluteControl(const Url& url, const std::string& base, const std::string& control) {
    if (control.empty() || control == "*") return base;
    if (lower(control).rfind("rtsp://", 0) == 0 || lower(control).rfind("rtsps://", 0) == 0) return control;
    std::string root = base;
    if (control.front() == '/') return url.scheme + "://" + url.host + ":" + std::to_string(url.port) + control;
    const auto slash = root.rfind('/');
    if (slash != std::string::npos && slash > root.find("://") + 2) root.erase(slash + 1);
    else if (!root.empty() && root.back() != '/') root.push_back('/');
    return root + control;
}

struct Track {
    enum class Codec { Mp2t, H264, H265, Aac, Unknown } codec = Codec::Unknown;
    std::string media;
    int payloadType = -1;
    int clockRate = 90000;
    int channels = 2;
    std::string control;
    std::string fmtp;
    int rtpChannel = -1;
    int rtcpChannel = -1;
    int rtpFd = -1;
    int rtcpFd = -1;
    std::uint32_t firstTimestamp = 0;
    bool haveTimestamp = false;
    std::vector<std::uint8_t> accessUnit;
    std::vector<std::uint8_t> parameterSets;
    bool fuActive = false;
};

std::vector<Track> parseSdp(const std::string& sdp) {
    std::vector<Track> tracks;
    Track* current = nullptr;
    std::istringstream in(sdp); std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("m=", 0) == 0) {
            std::istringstream m(line.substr(2)); Track t; std::string proto;
            m >> t.media; int port = 0; m >> port >> proto >> t.payloadType;
            tracks.push_back(t); current = &tracks.back();
        } else if (current && line.rfind("a=rtpmap:", 0) == 0) {
            const auto space = line.find(' '); if (space == std::string::npos) continue;
            int pt = -1; try { pt = std::stoi(line.substr(9, space - 9)); } catch (...) { continue; }
            if (pt != current->payloadType) continue;
            const std::string mapping = line.substr(space + 1);
            std::vector<std::string> parts; std::stringstream ss(mapping); std::string p;
            while (std::getline(ss, p, '/')) parts.push_back(p);
            const std::string codec = lower(parts.empty() ? "" : parts[0]);
            if (codec == "mp2t") current->codec = Track::Codec::Mp2t;
            else if (codec == "h264") current->codec = Track::Codec::H264;
            else if (codec == "h265" || codec == "hevc") current->codec = Track::Codec::H265;
            else if (codec == "mpeg4-generic" || codec == "mp4a-latm") current->codec = Track::Codec::Aac;
            if (parts.size() > 1) { try { current->clockRate = std::stoi(parts[1]); } catch (...) {} }
            if (parts.size() > 2) { try { current->channels = std::stoi(parts[2]); } catch (...) {} }
        } else if (current && line.rfind("a=control:", 0) == 0) current->control = trim(line.substr(10));
        else if (current && line.rfind("a=fmtp:", 0) == 0) {
            const auto space = line.find(' '); if (space != std::string::npos) current->fmtp = line.substr(space + 1);
        }
    }
    for (auto& t : tracks) {
        if (t.payloadType == 33 && t.codec == Track::Codec::Unknown) t.codec = Track::Codec::Mp2t;
        if (t.codec == Track::Codec::H264) {
            const auto p = t.fmtp.find("sprop-parameter-sets=");
            if (p != std::string::npos) {
                const auto start = p + 21; const auto end = t.fmtp.find(';', start);
                std::stringstream list(t.fmtp.substr(start, end == std::string::npos ? std::string::npos : end-start));
                std::string item;
                while (std::getline(list, item, ',')) {
                    auto nal = decodeBase64(trim(item)); if (nal.empty()) continue;
                    const std::array<std::uint8_t,4> sc{{0,0,0,1}};
                    t.parameterSets.insert(t.parameterSets.end(), sc.begin(), sc.end());
                    t.parameterSets.insert(t.parameterSets.end(), nal.begin(), nal.end());
                }
            }
        } else if (t.codec == Track::Codec::H265) {
            for (const char* name : {"sprop-vps=", "sprop-sps=", "sprop-pps="}) {
                const auto p = t.fmtp.find(name); if (p == std::string::npos) continue;
                const auto start = p + std::strlen(name); const auto end = t.fmtp.find(';', start);
                auto nal = decodeBase64(trim(t.fmtp.substr(start, end == std::string::npos ? std::string::npos : end-start)));
                if (nal.empty()) continue;
                const std::array<std::uint8_t,4> sc{{0,0,0,1}};
                t.parameterSets.insert(t.parameterSets.end(), sc.begin(), sc.end());
                t.parameterSets.insert(t.parameterSets.end(), nal.begin(), nal.end());
            }
        }
    }
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [](const Track& t){ return t.codec == Track::Codec::Unknown; }), tracks.end());
    return tracks;
}

bool bindUdp(const std::string& bindAddress, int& fd, int& port, std::string& error) {
    fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) { error = "RTSP UDP socket failed"; return false; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    if (!bindAddress.empty() && bindAddress != "0.0.0.0" && ::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        ::close(fd); fd = -1; error = "invalid RTSP bind address"; return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { error = "RTSP UDP bind failed"; ::close(fd); fd=-1; return false; }
    socklen_t len = sizeof(addr); ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len); port = ntohs(addr.sin_port); return true;
}

void appendStartCode(std::vector<std::uint8_t>& out) { out.insert(out.end(), {0,0,0,1}); }

bool makeAdts(int sampleRate, int channels, std::size_t payloadSize, std::array<std::uint8_t,7>& h) {
    static const int rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    int idx = -1; for (int i=0;i<13;++i) if (rates[i] == sampleRate) { idx=i; break; }
    if (idx < 0 || channels <= 0 || channels > 7 || payloadSize + 7 > 0x1fff) return false;
    const std::size_t len = payloadSize + 7;
    h[0]=0xff; h[1]=0xf1; h[2]=static_cast<std::uint8_t>((1<<6)|(idx<<2)|((channels>>2)&1));
    h[3]=static_cast<std::uint8_t>(((channels&3)<<6)|((len>>11)&3)); h[4]=static_cast<std::uint8_t>(len>>3);
    h[5]=static_cast<std::uint8_t>(((len&7)<<5)|0x1f); h[6]=0xfc; return true;
}

std::string peerIp(int fd) {
    sockaddr_storage ss{}; socklen_t sl=sizeof(ss); if (::getpeername(fd,reinterpret_cast<sockaddr*>(&ss),&sl)!=0) return {};
    char buf[INET6_ADDRSTRLEN]{};
    if (ss.ss_family==AF_INET) ::inet_ntop(AF_INET,&reinterpret_cast<sockaddr_in*>(&ss)->sin_addr,buf,sizeof(buf));
    else if (ss.ss_family==AF_INET6) ::inet_ntop(AF_INET6,&reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr,buf,sizeof(buf));
    return buf;
}

std::string randomSession() {
    static std::atomic<std::uint64_t> counter{1};
    return std::to_string(static_cast<unsigned long long>(Clock::now().time_since_epoch().count())) + "-" + std::to_string(counter++);
}

} // namespace

NativeRtspInput::~NativeRtspInput() { stop(); }

bool NativeRtspInput::start(const InputConfig& config, DataCallback data, StatusCallback status, std::string& error) {
    stop();
    Url url; if (!parseUrl(config.uri, url)) { error = "invalid RTSP URL"; return false; }
    if (url.scheme == "rtsps") { error = "RTSPS is not enabled in native RTSP yet; use RTSP or terminate TLS upstream"; return false; }
    config_ = config; data_ = std::move(data); status_ = std::move(status); receivedBytes_.store(0); stopping_.store(false); running_.store(true); setError({});
    try { worker_ = std::thread(&NativeRtspInput::run, this); }
    catch (const std::exception& ex) { running_.store(false); error = ex.what(); return false; }
    error.clear(); return true;
}

void NativeRtspInput::stop() noexcept { stopping_.store(true); if (worker_.joinable() && worker_.get_id()!=std::this_thread::get_id()) worker_.join(); running_.store(false); }
std::string NativeRtspInput::lastError() const { std::lock_guard<std::mutex> lock(errorMutex_); return lastError_; }
void NativeRtspInput::setError(const std::string& v) { std::lock_guard<std::mutex> lock(errorMutex_); lastError_=v; }

void NativeRtspInput::run() {
    Url url; std::string error; if (!parseUrl(config_.uri,url)) { setError("invalid RTSP URL"); running_.store(false); return; }
    int fd = connectTcp(url, config_.connectTimeoutMs, &stopping_, error); if (fd<0) { setError(error); if(status_)status_(error); running_.store(false); return; }
    auto closeAll = [&](std::vector<Track>& tracks){ for(auto& t:tracks){ if(t.rtpFd>=0)::close(t.rtpFd); if(t.rtcpFd>=0)::close(t.rtcpFd); t.rtpFd=t.rtcpFd=-1;} ::close(fd); };
    int cseq=1; std::string pending; RtspResponse resp;
    const std::string root = url.scheme + "://" + url.host + ":" + std::to_string(url.port) + url.path;
    if (!request(fd,url,cseq,"DESCRIBE",root,{{"Accept","application/sdp"}},"",pending,resp,config_.ioTimeoutMs,&stopping_,error)) { setError(error); std::vector<Track> none; closeAll(none); running_.store(false); return; }
    auto tracks = parseSdp(resp.body); if (tracks.empty()) { setError("RTSP SDP contains no supported MP2T/H264/H265/AAC track"); closeAll(tracks); running_.store(false); return; }
    std::string session;
    bool tcp = lower(config_.mode)!="udp";
    int channel=0;
    for (auto& track:tracks) {
        std::vector<std::pair<std::string,std::string>> headers;
        if (tcp) { track.rtpChannel=channel; track.rtcpChannel=channel+1; channel+=2; headers.push_back({"Transport","RTP/AVP/TCP;unicast;interleaved="+std::to_string(track.rtpChannel)+"-"+std::to_string(track.rtcpChannel)}); }
        else {
            int rtpPort=0,rtcpPort=0; if(!bindUdp(config_.bindAddress,track.rtpFd,rtpPort,error)||!bindUdp(config_.bindAddress,track.rtcpFd,rtcpPort,error)){ setError(error); closeAll(tracks); running_.store(false); return; }
            headers.push_back({"Transport","RTP/AVP;unicast;client_port="+std::to_string(rtpPort)+"-"+std::to_string(rtcpPort)});
        }
        if(!session.empty()) headers.push_back({"Session",session});
        RtspResponse setup;
        if(!request(fd,url,cseq,"SETUP",absoluteControl(url,root,track.control),headers,"",pending,setup,config_.ioTimeoutMs,&stopping_,error)){ setError(error); closeAll(tracks); running_.store(false); return; }
        if(session.empty()) { auto it=setup.headers.find("session"); if(it!=setup.headers.end()){ session=it->second; const auto semi=session.find(';'); if(semi!=std::string::npos)session.erase(semi); } }
    }
    std::vector<std::pair<std::string,std::string>> playHeaders; if(!session.empty()) playHeaders.push_back({"Session",session});
    if(!request(fd,url,cseq,"PLAY",root,playHeaders,"",pending,resp,config_.ioTimeoutMs,&stopping_,error)){ setError(error); closeAll(tracks); running_.store(false); return; }
    if(status_) status_("playing");

    mpegts::NativeMpegTsMux mux; mpegts::NativeMuxConfig mc; mc.serviceId=1; mc.targetBitrate=0; mc.serviceName="RTSP"; mc.serviceProvider="DVBStreamer5";
    mux.initialize(mc,error);
    bool needMux = std::none_of(tracks.begin(),tracks.end(),[](const Track&t){return t.codec==Track::Codec::Mp2t;});
    if(needMux){ for(const auto&t:tracks){ if(t.codec==Track::Codec::H264)mux.setCodec(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H264,error); else if(t.codec==Track::Codec::H265)mux.setCodec(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H265,error); else if(t.codec==Track::Codec::Aac)mux.setCodec(mpegts::ElementaryKind::Audio,mpegts::ElementaryCodec::AacAdts,error); } }

    auto emitElementary = [&](Track& t, const std::uint8_t* bytes, std::size_t count, std::uint32_t ts, bool marker) {
        rtp::PacketView r{}; (void)r;
        if(!t.haveTimestamp){t.firstTimestamp=ts;t.haveTimestamp=true;}
        std::uint64_t pts90k = t.clockRate>0 ? (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ts-t.firstTimestamp))*90000ULL/static_cast<std::uint64_t>(t.clockRate)) : 0;
        auto emitSample=[&](mpegts::ElementaryKind kind,mpegts::ElementaryCodec codec,const std::vector<std::uint8_t>& sample,bool key){ (void)codec; if(sample.empty())return; mpegts::ElementarySample es; es.data=sample.data();es.size=sample.size();es.hasPts=es.hasDts=true;es.pts90k=es.dts90k=pts90k;es.randomAccess=key;std::vector<mpegts::Packet> out;std::string e;if(mux.write(kind,es,out,e)){std::vector<std::uint8_t> raw;raw.reserve(out.size()*188);for(auto&p:out)raw.insert(raw.end(),p.begin(),p.end());if(!raw.empty()&&data_&&data_(raw.data(),raw.size()))receivedBytes_.fetch_add(raw.size());} };
        if(t.codec==Track::Codec::H264){
            if(count<1)return; const std::uint8_t nt=bytes[0]&0x1fU;
            if(nt>=1&&nt<=23){appendStartCode(t.accessUnit);t.accessUnit.insert(t.accessUnit.end(),bytes,bytes+count);} else if(nt==24){std::size_t p=1;while(p+2<=count){const std::size_t n=(bytes[p]<<8)|bytes[p+1];p+=2;if(p+n>count)break;appendStartCode(t.accessUnit);t.accessUnit.insert(t.accessUnit.end(),bytes+p,bytes+p+n);p+=n;}} else if(nt==28&&count>=2){const bool start=(bytes[1]&0x80)!=0,end=(bytes[1]&0x40)!=0;const std::uint8_t hdr=(bytes[0]&0xe0U)|(bytes[1]&0x1fU);if(start){appendStartCode(t.accessUnit);t.accessUnit.push_back(hdr);t.fuActive=true;}if(t.fuActive)t.accessUnit.insert(t.accessUnit.end(),bytes+2,bytes+count);if(end)t.fuActive=false;}
            if(marker&&!t.accessUnit.empty()){bool key=false;for(std::size_t i=0;i+4<t.accessUnit.size();++i)if(t.accessUnit[i]==0&&t.accessUnit[i+1]==0&&((t.accessUnit[i+2]==1&&(t.accessUnit[i+3]&0x1fU)==5)||(t.accessUnit[i+2]==0&&t.accessUnit[i+3]==1&&i+4<t.accessUnit.size()&&(t.accessUnit[i+4]&0x1fU)==5))){key=true;break;}if(key&&!t.parameterSets.empty())t.accessUnit.insert(t.accessUnit.begin(),t.parameterSets.begin(),t.parameterSets.end());emitSample(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H264,t.accessUnit,key);t.accessUnit.clear();}
        } else if(t.codec==Track::Codec::H265){
            if(count<2)return; const std::uint8_t nt=(bytes[0]>>1)&0x3fU;
            if(nt<48){appendStartCode(t.accessUnit);t.accessUnit.insert(t.accessUnit.end(),bytes,bytes+count);} else if(nt==48){std::size_t p=2;while(p+2<=count){const std::size_t n=(bytes[p]<<8)|bytes[p+1];p+=2;if(p+n>count)break;appendStartCode(t.accessUnit);t.accessUnit.insert(t.accessUnit.end(),bytes+p,bytes+p+n);p+=n;}} else if(nt==49&&count>=3){const bool start=(bytes[2]&0x80)!=0,end=(bytes[2]&0x40)!=0;const std::uint8_t orig=bytes[2]&0x3fU;if(start){appendStartCode(t.accessUnit);t.accessUnit.push_back(static_cast<std::uint8_t>((bytes[0]&0x81U)|(orig<<1)));t.accessUnit.push_back(bytes[1]);t.fuActive=true;}if(t.fuActive)t.accessUnit.insert(t.accessUnit.end(),bytes+3,bytes+count);if(end)t.fuActive=false;}
            if(marker&&!t.accessUnit.empty()){bool key=false;for(std::size_t i=0;i+5<t.accessUnit.size();++i){std::size_t n=0;if(t.accessUnit[i]==0&&t.accessUnit[i+1]==0&&t.accessUnit[i+2]==1)n=i+3;else if(t.accessUnit[i]==0&&t.accessUnit[i+1]==0&&t.accessUnit[i+2]==0&&t.accessUnit[i+3]==1)n=i+4;if(n){const auto ty=(t.accessUnit[n]>>1)&0x3fU;if(ty>=16&&ty<=21){key=true;break;}}}if(key&&!t.parameterSets.empty())t.accessUnit.insert(t.accessUnit.begin(),t.parameterSets.begin(),t.parameterSets.end());emitSample(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H265,t.accessUnit,key);t.accessUnit.clear();}
        } else if(t.codec==Track::Codec::Aac){
            if(count<4)return;const std::uint16_t bits=static_cast<std::uint16_t>((bytes[0]<<8)|bytes[1]);const std::size_t headerBytes=(bits+7)/8;if(2+headerBytes>count)return;std::size_t hp=2,dp=2+headerBytes;while(hp+2<=2+headerBytes){const std::size_t n=((bytes[hp]<<8)|bytes[hp+1])>>3;hp+=2;if(n==0||dp+n>count)break;std::array<std::uint8_t,7> adts{};if(!makeAdts(t.clockRate,t.channels,n,adts))break;std::vector<std::uint8_t> frame(adts.begin(),adts.end());frame.insert(frame.end(),bytes+dp,bytes+dp+n);emitSample(mpegts::ElementaryKind::Audio,mpegts::ElementaryCodec::AacAdts,frame,false);dp+=n;}
        }
    };

    auto handleRtp=[&](Track& track,const std::uint8_t* bytes,std::size_t count){rtp::PacketView r;if(!rtp::parsePacket(bytes,count,r))return;if(track.codec==Track::Codec::Mp2t){std::vector<mpegts::Packet> ps;if(!rtp::decodeMpegTsPayload(r,ps))return;std::vector<std::uint8_t> raw;raw.reserve(ps.size()*188);for(auto&p:ps)raw.insert(raw.end(),p.begin(),p.end());if(data_&&data_(raw.data(),raw.size()))receivedBytes_.fetch_add(raw.size());}else emitElementary(track,r.payload,r.payloadSize,r.timestamp,r.marker);};

    auto lastKeep=Clock::now(); std::array<std::uint8_t,65536> buffer{};
    while(!stopping_.load()){
        if(tcp){
            pollfd p{fd,POLLIN,0};const int pr=::poll(&p,1,200);if(pr<0&&errno!=EINTR){error="RTSP TCP poll failed";break;}if(pr>0){const ssize_t n=::recv(fd,buffer.data(),buffer.size(),0);if(n<=0){error="RTSP source disconnected";break;}pending.append(reinterpret_cast<char*>(buffer.data()),static_cast<std::size_t>(n));for(;;){const auto dollar=pending.find('$');if(dollar==std::string::npos){if(pending.size()>65536)pending.clear();break;}if(dollar>0)pending.erase(0,dollar);if(pending.size()<4)break;const int ch=static_cast<unsigned char>(pending[1]);const std::size_t len=(static_cast<unsigned char>(pending[2])<<8)|static_cast<unsigned char>(pending[3]);if(pending.size()<4+len)break;for(auto&t:tracks)if(t.rtpChannel==ch)handleRtp(t,reinterpret_cast<const std::uint8_t*>(pending.data()+4),len);pending.erase(0,4+len);}}
        }else{
            std::vector<pollfd> pf;std::vector<Track*> map;for(auto&t:tracks)if(t.rtpFd>=0){pf.push_back({t.rtpFd,POLLIN,0});map.push_back(&t);}if(::poll(pf.data(),pf.size(),200)>0)for(std::size_t i=0;i<pf.size();++i)if(pf[i].revents&POLLIN){const ssize_t n=::recv(pf[i].fd,buffer.data(),buffer.size(),0);if(n>0)handleRtp(*map[i],buffer.data(),static_cast<std::size_t>(n));}
        }
        if(Clock::now()-lastKeep>std::chrono::seconds(20)){RtspResponse keep;std::vector<std::pair<std::string,std::string>> h;if(!session.empty())h.push_back({"Session",session});std::string keepPending;if(!request(fd,url,cseq,"OPTIONS",root,h,"",keepPending,keep,config_.ioTimeoutMs,&stopping_,error))break;lastKeep=Clock::now();}
    }
    if(!session.empty()&&!stopping_.load()){RtspResponse bye;std::string p;request(fd,url,cseq,"TEARDOWN",root,{{"Session",session}},"",p,bye,1000,nullptr,error);}
    closeAll(tracks); if(!stopping_.load()&&!error.empty()){setError(error);if(status_)status_(error);} running_.store(false);
}

struct NativeRtspOutput::Client {
    int fd=-1; std::string ip; std::string session; std::atomic<bool> playing{false}; std::atomic<bool> stopping{false};
    bool tcpInterleaved=true; int rtpChannel=0; sockaddr_storage udpPeer{}; socklen_t udpPeerLen=0; int udpFd=-1; std::mutex writeMutex; std::thread thread;
    ~Client(){ if(fd>=0)::close(fd); if(udpFd>=0)::close(udpFd); }
};

NativeRtspOutput::NativeRtspOutput() : packetizer_(0x44564235U,0,33,7) {}
NativeRtspOutput::~NativeRtspOutput(){stop();}

bool NativeRtspOutput::start(const OutputConfig& config, AllowPeer allowPeer, PeerCallback onConnect, PeerCallback onDisconnect, std::string& error){
    stop();config_=config;allowPeer_=std::move(allowPeer);onConnect_=std::move(onConnect);onDisconnect_=std::move(onDisconnect);sentBytes_.store(0);stopping_.store(false);setError({});
    listenFd_=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listenFd_<0){error="RTSP listen socket failed";return false;}int yes=1;::setsockopt(listenFd_,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(static_cast<std::uint16_t>(config.port));a.sin_addr.s_addr=INADDR_ANY;if(!config.bindAddress.empty()&&config.bindAddress!="0.0.0.0"&&::inet_pton(AF_INET,config.bindAddress.c_str(),&a.sin_addr)!=1){error="invalid RTSP output bind address";::close(listenFd_);listenFd_=-1;return false;}if(::bind(listenFd_,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0||::listen(listenFd_,32)!=0){error=std::string("RTSP listen failed: ")+std::strerror(errno);::close(listenFd_);listenFd_=-1;return false;}running_.store(true);try{acceptThread_=std::thread(&NativeRtspOutput::acceptLoop,this);}catch(...){::close(listenFd_);listenFd_=-1;running_.store(false);error="RTSP accept thread failed";return false;}return true;
}

void NativeRtspOutput::stop() noexcept{stopping_.store(true);running_.store(false);if(listenFd_>=0){::shutdown(listenFd_,SHUT_RDWR);::close(listenFd_);listenFd_=-1;}if(acceptThread_.joinable()&&acceptThread_.get_id()!=std::this_thread::get_id())acceptThread_.join();std::vector<std::shared_ptr<Client>> clients;{std::lock_guard<std::mutex>l(clientsMutex_);clients.swap(clients_);}for(auto&c:clients){c->stopping.store(true);if(c->fd>=0)::shutdown(c->fd,SHUT_RDWR);}for(auto&c:clients)if(c->thread.joinable()&&c->thread.get_id()!=std::this_thread::get_id())c->thread.join();}
std::string NativeRtspOutput::lastError()const{std::lock_guard<std::mutex>l(errorMutex_);return lastError_;}void NativeRtspOutput::setError(const std::string&v){std::lock_guard<std::mutex>l(errorMutex_);lastError_=v;}

void NativeRtspOutput::acceptLoop(){while(!stopping_.load()){sockaddr_storage ss{};socklen_t sl=sizeof(ss);const int fd=::accept4(listenFd_,reinterpret_cast<sockaddr*>(&ss),&sl,SOCK_CLOEXEC);if(fd<0){if(stopping_.load())break;if(errno==EINTR)continue;std::this_thread::sleep_for(std::chrono::milliseconds(50));continue;}const std::string ip=peerIp(fd);if(allowPeer_&&!allowPeer_(ip)){::close(fd);continue;}auto c=std::make_shared<Client>();c->fd=fd;c->ip=ip;c->session=randomSession();{std::lock_guard<std::mutex>l(clientsMutex_);clients_.push_back(c);}if(onConnect_)onConnect_(ip);try{c->thread=std::thread(&NativeRtspOutput::clientLoop,this,c);}catch(...){removeClient(c);}}}

void NativeRtspOutput::removeClient(const std::shared_ptr<Client>&c){
    if (c->thread.joinable() && c->thread.get_id() == std::this_thread::get_id()) c->thread.detach();
    {std::lock_guard<std::mutex>l(clientsMutex_);clients_.erase(std::remove(clients_.begin(),clients_.end(),c),clients_.end());}
    if(onDisconnect_&&!c->ip.empty())onDisconnect_(c->ip);
}

void NativeRtspOutput::clientLoop(const std::shared_ptr<Client>&c){std::string pending;char buf[8192];auto reply=[&](int code,const std::string&reason,const std::string&cseq,const std::vector<std::pair<std::string,std::string>>&headers,const std::string&body){std::ostringstream o;o<<"RTSP/1.0 "<<code<<" "<<reason<<"\r\nCSeq: "<<cseq<<"\r\nServer: DVBStreamer5/native-rtsp\r\n";for(auto&[k,v]:headers)o<<k<<": "<<v<<"\r\n";if(!body.empty())o<<"Content-Type: application/sdp\r\nContent-Length: "<<body.size()<<"\r\n";o<<"\r\n"<<body;std::lock_guard<std::mutex>wl(c->writeMutex);return sendAll(c->fd,o.str());};while(!stopping_.load()&&!c->stopping.load()){pollfd p{c->fd,POLLIN,0};const int pr=::poll(&p,1,500);if(pr==0)continue;if(pr<0){if(errno==EINTR)continue;break;}const ssize_t n=::recv(c->fd,buf,sizeof(buf),0);if(n<=0)break;pending.append(buf,static_cast<std::size_t>(n));for(;;){const auto end=pending.find("\r\n\r\n");if(end==std::string::npos)break;std::string req=pending.substr(0,end);pending.erase(0,end+4);std::istringstream in(req);std::string line;std::getline(in,line);line=trim(line);std::istringstream rl(line);std::string method,target,ver;rl>>method>>target>>ver;std::map<std::string,std::string>h;while(std::getline(in,line)){line=trim(line);const auto col=line.find(':');if(col!=std::string::npos)h[lower(trim(line.substr(0,col)))]=trim(line.substr(col+1));}const std::string cs=h.count("cseq")?h["cseq"]:"1";method=lower(method);if(method=="options"){reply(200,"OK",cs,{{"Public","OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"}},"");}else if(method=="describe"){std::ostringstream sdp;sdp<<"v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=DVBStreamer5 "<<config_.streamName<<"\r\nt=0 0\r\na=control:*\r\nm=video 0 RTP/AVP 33\r\na=rtpmap:33 MP2T/90000\r\na=control:trackID=0\r\n";reply(200,"OK",cs,{{"Content-Base",target+(target.back()=='/'?"":"/")}},sdp.str());}else if(method=="setup"){const std::string transport=h["transport"];c->tcpInterleaved=lower(transport).find("rtp/avp/tcp")!=std::string::npos;if(c->tcpInterleaved){const auto q=lower(transport).find("interleaved=");if(q!=std::string::npos){try{c->rtpChannel=std::stoi(transport.substr(q+12));}catch(...){c->rtpChannel=0;}}reply(200,"OK",cs,{{"Session",c->session},{"Transport","RTP/AVP/TCP;unicast;interleaved="+std::to_string(c->rtpChannel)+"-"+std::to_string(c->rtpChannel+1)}},"");}else{const auto q=lower(transport).find("client_port=");int cp=0;if(q!=std::string::npos){try{cp=std::stoi(transport.substr(q+12));}catch(...){}}sockaddr_storage peer{};socklen_t pl=sizeof(peer);::getpeername(c->fd,reinterpret_cast<sockaddr*>(&peer),&pl);if(peer.ss_family==AF_INET)reinterpret_cast<sockaddr_in*>(&peer)->sin_port=htons(static_cast<std::uint16_t>(cp));else if(peer.ss_family==AF_INET6)reinterpret_cast<sockaddr_in6*>(&peer)->sin6_port=htons(static_cast<std::uint16_t>(cp));c->udpPeer=peer;c->udpPeerLen=pl;c->udpFd=::socket(peer.ss_family,SOCK_DGRAM|SOCK_CLOEXEC,0);reply(200,"OK",cs,{{"Session",c->session},{"Transport","RTP/AVP;unicast;client_port="+std::to_string(cp)+"-"+std::to_string(cp+1)}},"");}}else if(method=="play"){c->playing.store(true);reply(200,"OK",cs,{{"Session",c->session},{"RTP-Info","url="+target+";seq=0;rtptime=0"}},"");}else if(method=="get_parameter"){reply(200,"OK",cs,{{"Session",c->session}},"");}else if(method=="teardown"){reply(200,"OK",cs,{{"Session",c->session}},"");c->stopping.store(true);break;}else reply(405,"Method Not Allowed",cs,{},"");}}removeClient(c);}

bool NativeRtspOutput::push(const std::uint8_t*data,std::size_t size){if(!running_.load()||!data||!size)return false;std::vector<mpegts::Packet> packets;std::vector<std::vector<std::uint8_t>> dgs;{std::lock_guard<std::mutex>l(packetizerMutex_);framer_.push(data,size,packets);if(packets.empty())return true;const auto ticks=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();const std::uint32_t ts=static_cast<std::uint32_t>((ticks*90)/1000);if(!packetizer_.packetize(packets,ts,dgs))return false;}std::vector<std::shared_ptr<Client>> clients;{std::lock_guard<std::mutex>l(clientsMutex_);clients=clients_;}for(auto&c:clients){if(!c||!c->playing.load())continue;for(const auto&dg:dgs){ssize_t n=-1;if(c->tcpInterleaved){std::vector<std::uint8_t> frame(4+dg.size());frame[0]='$';frame[1]=static_cast<std::uint8_t>(c->rtpChannel);frame[2]=static_cast<std::uint8_t>(dg.size()>>8);frame[3]=static_cast<std::uint8_t>(dg.size());std::copy(dg.begin(),dg.end(),frame.begin()+4);std::lock_guard<std::mutex>wl(c->writeMutex);if(sendAll(c->fd,frame.data(),frame.size()))n=static_cast<ssize_t>(dg.size());}else if(c->udpFd>=0)n=::sendto(c->udpFd,dg.data(),dg.size(),MSG_NOSIGNAL,reinterpret_cast<const sockaddr*>(&c->udpPeer),c->udpPeerLen);if(n>0)sentBytes_.fetch_add(static_cast<std::uint64_t>(n));}}return true;}

} // namespace dvbstreamer5::media::rtsp
