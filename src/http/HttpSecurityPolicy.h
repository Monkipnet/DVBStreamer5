#pragma once
// V545: pure policy helpers. No user-supplied HTTP header can authorize itself.
#include <boost/asio/ip/address.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dvbstreamer5::http_security {

inline std::string lowerAscii(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){
        return c>='A' && c<='Z' ? char(c+('a'-'A')) : char(c);
    });
    return value;
}
inline bool isLoopback(const std::string& address) {
    boost::system::error_code ec;
    const auto ip=boost::asio::ip::make_address(address,ec);
    return !ec && ip.is_loopback();
}
inline std::string effectiveClientIp(const std::string& socketIp,
                                     const std::string& realIpHeader,
                                     bool trustLoopbackProxy) {
    if(!trustLoopbackProxy || !isLoopback(socketIp) ||
       realIpHeader.empty() || realIpHeader.size()>64U ||
       realIpHeader.find_first_of(",; \t\r\n")!=std::string::npos)
        return socketIp;
    boost::system::error_code ec;
    const auto remote=boost::asio::ip::make_address(realIpHeader,ec);
    if(ec || remote.is_unspecified() || remote.is_multicast())
        return socketIp;
    return remote.to_string();
}
inline std::size_t maxRequestBodyBytes(const std::string& target,
                                       const std::string& method,
                                       bool panelAuthenticated=false) {
    if(method!="POST" && method!="PUT")return 16U*1024U;
    if(target=="/api/panel/login")return 4096U;
    // Preserve existing 512 MiB backup-upload functionality for authenticated
    // administrators. Unauthenticated callers cannot allocate this body.
    if(target.rfind("/api/upload-backup-file",0)==0)
        return panelAuthenticated?512U*1024U*1024U:4096U;
    if(target=="/api/config/import")return panelAuthenticated?16U*1024U*1024U:4096U;
    if(target=="/api/tickers/import-docx")return panelAuthenticated?3U*1024U*1024U:4096U;
    return 1024U*1024U;
}
inline bool validSameOrigin(const std::string& origin,
                            const std::string& host) {
    // Exact scheme + authority comparison (host includes its optional port).
    if(origin.empty() || host.empty() || origin.size()>300 || host.size()>250 ||
       origin.find_first_of("\r\n\t@/?# ",8)!=std::string::npos)
        return false;
    const std::string o=lowerAscii(origin), h=lowerAscii(host);
    if(h.find_first_of("\r\n\t@/?# ")!=std::string::npos)return false;
    return o=="https://"+h || o=="http://"+h;
}
inline bool allowStateChangingBrowserRequest(
    const std::string& origin,const std::string& host,
    const std::string& fetchSite,const std::string& requestedWith,
    bool cookieAuthenticated) {
    // Reject explicitly cross-site requests even if other headers are spoofed.
    if(fetchSite=="cross-site" || fetchSite=="same-site")return false;
    if(!origin.empty())return validSameOrigin(origin,host);
    // Scripted older clients may omit Origin, but a cross-site HTML form
    // cannot add this non-safelisted header without a CORS preflight.
    return !cookieAuthenticated || requestedWith=="DVBStreamer5";
}

class LoginThrottle {
public:
    using Clock=std::chrono::steady_clock;
    bool allowed(const std::string& ip,Clock::time_point now=Clock::now()) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it=entries_.find(ip);
        if(it==entries_.end())return true;
        if(now>=it->second.until){entries_.erase(it);return true;}
        return it->second.failures<8;
    }
    void record(const std::string& ip,bool success,
                Clock::time_point now=Clock::now()) {
        std::lock_guard<std::mutex> lock(mu_);
        if(success){entries_.erase(ip);return;}
        if(entries_.size()>2048){
            for(auto it=entries_.begin();it!=entries_.end();){
                if(now>=it->second.until)it=entries_.erase(it);
                else ++it;
            }
            if(entries_.size()>2048)entries_.clear();
        }
        auto& entry=entries_[ip];
        if(now>=entry.until)entry.failures=0;
        ++entry.failures;
        entry.until=now+std::chrono::minutes(entry.failures>=8?5:15);
    }
private:
    struct Entry {
        int failures=0;
        Clock::time_point until{};
    };
    std::mutex mu_;
    std::unordered_map<std::string,Entry> entries_;
};
} // namespace dvbstreamer5::http_security
