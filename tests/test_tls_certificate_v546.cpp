#include "http/TlsCertificateModules.h"
#include <cassert>
#include <iostream>
using namespace dvbstreamer5::tls;
int main(){
    assert(validDomain("blumen.zapto.org"));
    assert(validDomain("streamer.example.org"));
    for(const auto* bad:{"","localhost","foo.local","foo.internal","foo..example.org",
                          "-bad.example.org","bad-.example.org","bad/example.org",
                          "bad;touch.example.org","bad\r\nHost:evil.example.org",
                          "bad@example.org","http://example.org","example.org:9443"}){
        assert(!validDomain(bad));
    }
    assert(validEmail("admin@example.org"));
    for(const auto* bad:{"","a@b","bad@example.com;rm","foo bar@example.org",
                         "a@example.org\r\ncommand","a@example.org/"}){
        assert(!validEmail(bad));
    }
    assert(!status(9000).isMember("private_key"));
    assert(!status(9000).isMember("password"));
    auto bad=SelfSignedModule::generate("bad;domain",9000,9443);
    assert(bad["result"]=="error");
    bad=SelfSignedModule::generate("example.org",9000,9000);
    assert(bad["result"]=="error");
    bad=LetsEncryptModule::setEnabled(true,"example.org","evil;email",9000,9443);
    assert(bad["result"]=="error");
    bad=LetsEncryptModule::setEnabled(true,"example.org","admin@example.org",9000,80);
    assert(bad["result"]=="error");
    std::cout<<"PASS V546 TLS module input security, domain/email/ports and status privacy\n";
}
