#include "../src/ca/backends/biss/Biss1Key.h"
#include "../src/ca/CaBackendPluginApi.h"
extern "C" {
#include <dvbcsa/dvbcsa.h>
}
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

// Exported by the separately compiled BISS-1 plugin.
extern "C" const dvbstreamer5_ca_backend_api_v1*
dvbstreamer5_ca_backend_get_api_v1();

using dvbstreamer5::ca::biss::parseBiss1Key;
using dvbstreamer5::ca::biss::tsPayloadOffset;

static bool expect(bool ok, const char* text) {
    if (!ok) std::cerr << "FAIL BISS-1: " << text << "\n";
    return ok;
}
int main() {
    bool ok = true;
    std::array<uint8_t,8> cw{};
    ok &= expect(parseBiss1Key("010203040506",cw), "12 digit accepted");
    const std::array<uint8_t,8> expected{{1,2,3,6,4,5,6,15}};
    ok &= expect(cw == expected, "checksum bytes inserted");
    std::array<uint8_t,8> full{};
    ok &= expect(parseBiss1Key("010203060405060F",full) && full == expected,
                 "16 digit full word accepted");
    ok &= expect(!parseBiss1Key("010203000405060F",full), "invalid checksum denied");
    ok &= expect(!parseBiss1Key("000000000000",full), "zero key denied");
    ok &= expect(!parseBiss1Key("123",full), "invalid length denied");
    ok &= expect(!parseBiss1Key("01020304050Z",full), "invalid hex denied");

    const auto* api = dvbstreamer5_ca_backend_get_api_v1();
    ok &= expect(api && std::string(api->backend_id) == "biss1", "plugin id");
    auto* instance = api->create(nullptr);
    dvbstreamer5_ca_reader_info_v1 reader{};
    reader.reader_key = "biss-authorized-test";
    reader.backend_config_json = R"({"key":"010203040506","mode":"biss1"})";
    char error[256]{};
    ok &= expect(api->open_reader(instance,&reader,error,sizeof(error)) ==
                 DVBSTREAMER5_CA_RESULT_OK,"reader open");
    dvbstreamer5_ca_service_info_v1 service{};
    service.stream_id = "stream-test";
    service.service_id = 105;
    ok &= expect(api->start_service(instance,reader.reader_key,&service,error,sizeof(error)) ==
                 DVBSTREAMER5_CA_RESULT_OK,"service start");

    auto* encryptKey = dvbcsa_key_alloc();
    dvbcsa_key_set(cw.data(),encryptKey);
    auto roundtrip = [&](uint8_t parity, uint8_t adaptationBytes) {
        std::array<uint8_t,188> packet{};
        packet[0]=0x47; packet[1]=0x00; packet[2]=0x65;
        packet[3]=static_cast<uint8_t>((parity==0 ? 0x80 : 0xC0) |
            (adaptationBytes ? 0x30 : 0x10));
        const size_t payload=adaptationBytes? 5 + adaptationBytes : 4;
        if(adaptationBytes){
            packet[4]=adaptationBytes;
            for(unsigned i=0;i<adaptationBytes;++i)packet[5+i]=0xff;
        }
        for(size_t i=payload;i<188;++i)
            packet[i]=static_cast<uint8_t>((i*7 + 0x12) & 0xff);
        auto original=packet;
        original[3] &= 0x3f; // expected output is clear TS
        dvbcsa_encrypt(encryptKey,packet.data()+payload,
                       static_cast<unsigned>(188-payload));
        dvbstreamer5_ca_ts_result_v1 result{};
        ok &= expect(api->process_ts(instance,"stream-test",packet.data(),
                    packet.size(),&result) == DVBSTREAMER5_CA_RESULT_OK,"packet process");
        ok &= expect(packet==original,"TS payload decrypted and scrambling bits cleared");
        ok &= expect(result.packets_changed==1 && result.packets_seen==1,
                     "changed packet counted");
    };
    roundtrip(0,0);
    roundtrip(1,0);
    roundtrip(0,12);

    // Invalid packet and adaptation-only packets must not be marked clear.
    std::array<uint8_t,188> bad{};
    bad[0]=0x47;bad[3]=0xA0;bad[4]=183;
    dvbstreamer5_ca_ts_result_v1 result{};
    ok &= expect(api->process_ts(instance,"stream-test",bad.data(),
                 bad.size(),&result)==DVBSTREAMER5_CA_RESULT_OK,
                 "adaptation-only packet tolerated");
    ok &= expect((bad[3]&0xc0)==0x80 && result.packets_changed==0,
                 "scrambling bits preserved without payload");
    ok &= expect(api->process_ts(instance,"stream-test",bad.data(),187,&result) ==
                 DVBSTREAMER5_CA_RESULT_ERROR,
                 "non-TS buffer rejected");

    api->stop_service(instance,"stream-test");
    api->close_reader(instance,reader.reader_key);
    api->destroy(instance);
    dvbcsa_key_free(encryptKey);
    cw.fill(0);
    if(ok) std::cout<<"PASS BISS-1 checksum/descrambling/invalid packet tests\n";
    return ok?0:1;
}
