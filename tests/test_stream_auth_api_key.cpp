#include "StreamAuthApiKey.h"
#include <cassert>
#include <iostream>
#include <string>
using namespace dvbstreamer5::stream_auth;
int main(){
    auto old=api_key::generate();
    auto next=api_key::generate();
    assert(old.size()==64 && next.size()==64 && old!=next);
    const auto hash=api_key::sha256(old);
    assert(hash.size()==64 && hash!=old);
    assert(api_key::matches(hash,old));
    assert(!api_key::matches(hash,next));
    assert(!api_key::matches("",old));
    assert(!api_key::matches(hash,"short"));
    const std::string peers="127.0.0.1,::1;192.168.200.9";
    assert(api_key::validAllowedIps(peers));
    assert(api_key::isPeerAllowed(peers,"127.0.0.1"));
    assert(api_key::isPeerAllowed(peers,"::ffff:127.0.0.1"));
    assert(api_key::isPeerAllowed(peers,"192.168.200.9"));
    assert(!api_key::isPeerAllowed(peers,"192.168.200.10"));
    assert(!api_key::isPeerAllowed(peers,"bad-host"));
    assert(!api_key::validAllowedIps("127.0.0.1,invalid-ip"));
    assert(!api_key::validAllowedIps(""));
    assert(api_key::authorize(hash,peers,old,"127.0.0.1"));
    assert(!api_key::authorize(hash,peers,old,"192.168.200.10"));
    assert(!api_key::authorize(api_key::sha256(next),peers,old,"127.0.0.1"));
    assert(api_key::inScope("/api/stream-auth/sessions","GET"));
    assert(api_key::inScope("/api/stream-auth/sessions/disconnect","POST"));
    assert(!api_key::inScope("/api/state","GET"));
    assert(!api_key::inScope("/api/stream-auth/save","POST"));
    assert(!api_key::inScope("/api/stream-auth/api-key/rotate","POST"));
    assert(!api_key::inScope("/api/stream-auth/sessions","POST"));
    assert(!api_key::inScope("/api/stream-auth/sessions/disconnect","GET"));
    assert(!api_key::inScope("/api/stream-auth/sessions?x=1","GET"));
    std::cout<<"Session API least-privilege key: OK\n";
}
