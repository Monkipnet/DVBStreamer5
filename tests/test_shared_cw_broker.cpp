#include "../src/ca/backends/newcamd/SharedCwBroker.h"
#include <iostream>
#include <initializer_list>
using dvbstreamer5::ca::newcamd::SharedCwBroker;
int main() {
    bool ok = true;
    auto check = [&](bool pass, const char* label) {
        if (!pass) { std::cerr << "FAIL: " << label << "\n"; ok = false; }
    };
    SharedCwBroker cache;
    SharedCwBroker::Key key{"cam-1", 105, 0x98d, 0, 7181, 4, "80abcdef"};
    SharedCwBroker::Pair in, out;
    for (unsigned i=0; i<8; ++i) {
        in.even[i]=static_cast<uint8_t>(i+1);
        in.odd[i]=static_cast<uint8_t>(i+9);
    }
    check(cache.publish(key, 1000, in), "store valid pair");
    check(cache.lookup(key, 1100, out), "same ECM hit");
    check(in.even == out.even && in.odd == out.odd, "same pair");
    auto otherReader=key; otherReader.reader="cam-2";
    auto otherSid=key; otherSid.serviceId=108;
    auto otherEcm=key; otherEcm.ecmSignature="81abcdef";
    auto otherPid=key; otherPid.ecmPid=7183;
    auto otherMode=key; otherMode.ecmMode=0;
    auto otherCaid=key; otherCaid.caid=0x98c;
    auto otherProvid=key; otherProvid.provid=1;
    for (const auto& mismatch : {otherReader,otherSid,otherEcm,
             otherPid,otherMode,otherCaid,otherProvid})
        check(!cache.lookup(mismatch, 1100, out),
              "different CA/ECM/service context must miss");
    check(!cache.lookup(key, 2500, out), "expired after 1.5 seconds");
    check(cache.stats().expired == 1, "expiry counted");
    check(cache.publish(key, 3000, in), "store after expiry");
    cache.invalidateReader("cam-1");
    check(!cache.lookup(key, 3001, out), "reader close removes CW");
    SharedCwBroker::Pair empty;
    check(!cache.publish(key, 3002, empty), "reject empty CW");
    check(cache.stats().entries == 0, "no entries remain");
    for (unsigned i=0; i<SharedCwBroker::kMaxEntries+5; ++i) {
        auto k=key; k.ecmSignature=std::to_string(i);
        check(cache.publish(k, 10000, in), "bounded insert");
    }
    check(cache.stats().entries == SharedCwBroker::kMaxEntries,
          "capacity bounded");
    cache.clear();
    check(cache.stats().entries == 0, "CW cleared");
    in.wipe(); out.wipe();
    if (ok) std::cout << "PASS exact-ECM CW broker lifetime, context and bound\n";
    return ok ? 0 : 1;
}
