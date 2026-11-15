// V545: deterministic malformed-input security policy tests without live streams.
#include "http/HttpSecurityPolicy.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
using namespace dvbstreamer5::http_security;
int main(){
    assert(effectiveClientIp("198.51.100.8","203.0.113.5",false)=="198.51.100.8");
    assert(effectiveClientIp("198.51.100.8","203.0.113.5",true)=="198.51.100.8");
    assert(effectiveClientIp("127.0.0.1","203.0.113.5",true)=="203.0.113.5");
    assert(effectiveClientIp("::1","2001:db8::7",true)=="2001:db8::7");
    for(const std::string bad : {"","garbage","1.2.3.4,5.6.7.8","1.2.3.4\r\nHost:evil",
                                   "0.0.0.0","224.0.0.1","[2001:db8::1]"}) {
        assert(effectiveClientIp("127.0.0.1",bad,true)=="127.0.0.1");
    }
    assert(maxRequestBodyBytes("/api/state","GET")==16U*1024U);
    assert(maxRequestBodyBytes("/api/panel/login","POST")==4096U);
    assert(maxRequestBodyBytes("/api/save-config","POST")==1024U*1024U);
    assert(maxRequestBodyBytes("/api/config/import","POST")==4096U);
    assert(maxRequestBodyBytes("/api/config/import","POST",true)==16U*1024U*1024U);
    assert(maxRequestBodyBytes("/api/upload-backup-file?filename=x","POST")==4096U);
    assert(maxRequestBodyBytes("/api/upload-backup-file?filename=x","POST",true)==512U*1024U*1024U);
    assert(validSameOrigin("https://blumen.zapto.org:9443","blumen.zapto.org:9443"));
    assert(validSameOrigin("http://192.168.1.22:9000","192.168.1.22:9000"));
    assert(!validSameOrigin("https://blumen.zapto.org.evil:9443","blumen.zapto.org:9443"));
    assert(!validSameOrigin("null","blumen.zapto.org:9443"));
    assert(!validSameOrigin("http://blumen.zapto.org:9000/evil","blumen.zapto.org:9000"));
    assert(!validSameOrigin("https://blumen.zapto.org:9443@evil","blumen.zapto.org:9443"));
    assert(allowStateChangingBrowserRequest("https://foo.test:9443","foo.test:9443","same-origin","",true));
    assert(!allowStateChangingBrowserRequest("https://evil.test","foo.test:9443","cross-site","",true));
    assert(!allowStateChangingBrowserRequest("","foo.test:9443","cross-site","DVBStreamer5",true));
    assert(!allowStateChangingBrowserRequest("","foo.test:9443","","",true));
    assert(allowStateChangingBrowserRequest("","foo.test:9443","","DVBStreamer5",true));
    assert(allowStateChangingBrowserRequest("","foo.test:9443","","",false));
    LoginThrottle limiter;
    const auto now=LoginThrottle::Clock::now();
    for(int i=0;i<8;++i){
        assert(limiter.allowed("198.51.100.3",now));
        limiter.record("198.51.100.3",false,now);
    }
    assert(!limiter.allowed("198.51.100.3",now));
    assert(limiter.allowed("198.51.100.4",now));
    assert(limiter.allowed("198.51.100.3",now+std::chrono::minutes(6)));
    limiter.record("198.51.100.3",false,now+std::chrono::minutes(6));
    limiter.record("198.51.100.3",true,now+std::chrono::minutes(6));
    assert(limiter.allowed("198.51.100.3",now+std::chrono::minutes(6)));
    std::cout<<"PASS HTTP V545 size limits, proxy trust, CSRF and login throttling\n";
}
