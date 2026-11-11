#include "../src/ca/backends/biss/Biss2Key.h"
#include "../src/ca/CaBackendPluginApi.h"
#include <openssl/evp.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

extern "C" const dvbstreamer5_ca_backend_api_v1*
dvbstreamer5_ca_backend_get_api_v1();

using namespace dvbstreamer5::ca::biss2;
static bool expect(bool value, const char* message) {
    if (!value) std::cerr << "FAIL BISS2: " << message << "\n";
    return value;
}

static bool encryptPacket(const std::array<uint8_t,16>& sw,
                          std::array<uint8_t,188>& packet) {
    const unsigned afc = (packet[3] >> 4) & 3;
    const size_t pos = afc == 3 ? 5 + packet[4] : 4;
    const size_t n = (188-pos) & ~size_t(15);
    if (!n) return true;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int written=0, tail=0;
    const bool ok = EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,
                              sw.data(),kCissaIv.data())==1 &&
        EVP_CIPHER_CTX_set_padding(ctx,0)==1 &&
        EVP_EncryptUpdate(ctx,packet.data()+pos,&written,
                          packet.data()+pos,static_cast<int>(n))==1 &&
        written==static_cast<int>(n) &&
        EVP_EncryptFinal_ex(ctx,packet.data()+pos+written,&tail)==1 &&
        tail==0;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

int main() {
    bool ok = true;
    std::array<uint8_t,16> key{}, esw{}, id{}, sw{};
    ok &= expect(parseHex128("00112233445566778899aabbccddeeff",key),"32 hex key");
    ok &= expect(parseHex128("69c4e0d86a7b0430d8cdb78070b4c55a",esw),"official ESW");
    ok &= expect(parseHex128("000102030405060708090a0b0c0d0e0f",id),"official receiver ID");
    ok &= expect(decryptEsw(esw,id,sw) && key==sw,
        "EBU Tech 3292 Annex A AES-128 ECB official test vector");
    std::array<uint8_t,16> invalid{};
    ok &= expect(!parseHex128("1234",invalid),"reject short key");
    ok &= expect(!parseHex128("zz112233445566778899aabbccddeeff",invalid),"reject non-hex");
    ok &= expect(parseHex128("0011:2233-4455 6677 8899 aabb ccdd eeff",invalid) &&
                 invalid==key, "formatting separators");

    // ETSI TS 103 127 Annex B, Test Case 1: ciphertext first AES block.
    // This is an independent standards vector, NOT a ciphertext generated
    // by the same implementation being tested.
    std::array<uint8_t,188> standard{};
    standard[0]=0x47; standard[1]=0x60; standard[2]=0x80; standard[3]=0x91;
    const uint8_t cipherFirstBlock[16] = {
        0x15,0xce,0x67,0xe0,0xcb,0x01,0xb5,0x3c,
        0xe7,0x60,0x54,0xe5,0x7a,0x4a,0xd1,0x20};
    std::memcpy(standard.data()+4,cipherFirstBlock,16);
    EVP_CIPHER_CTX* ctx=EVP_CIPHER_CTX_new();
    ok &= expect(ctx && decryptTsPayload(ctx,key,standard.data()),
                 "DVB-CISSA known ciphertext packet");
    EVP_CIPHER_CTX_free(ctx);
    const uint8_t clearFirstBlock[16] = {
        0x54,0x68,0x69,0x73,0x20,0x69,0x73,0x20,
        0x74,0x68,0x65,0x20,0x70,0x61,0x79,0x6c};
    ok &= expect(std::memcmp(standard.data()+4,clearFirstBlock,16)==0,
        "ETSI 103127 Annex B first block matches");
    ok &= expect((standard[3]&0xc0)==0,"TS scrambling flags reset");

    const auto* api=dvbstreamer5_ca_backend_get_api_v1();
    ok &= expect(api && std::string(api->backend_id)=="biss2","plugin backend ID");
    void* p=api->create(nullptr);
    char error[256]{};
    dvbstreamer5_ca_reader_info_v1 reader{};
    reader.reader_key="biss2-test";
    reader.backend_config_json=R"({"mode":"biss2-1","key":"00112233445566778899aabbccddeeff"})";
    ok &= expect(api->open_reader(p,&reader,error,sizeof(error))==DVBSTREAMER5_CA_RESULT_OK,
                 "BISS2 Mode1 reader opened");
    dvbstreamer5_ca_service_info_v1 service{};
    service.stream_id="biss2-stream"; service.service_id=142;
    ok &= expect(api->start_service(p,reader.reader_key,&service,error,sizeof(error))==
                 DVBSTREAMER5_CA_RESULT_OK,"Mode1 service startup");

    auto verifyPacket = [&](int parity,int adaptationLength) {
        std::array<uint8_t,188> packet{};
        packet[0]=0x47; packet[1]=0x40; packet[2]=0x8e;
        packet[3]=static_cast<uint8_t>((parity==2?0x80:0xc0) |
                    (adaptationLength >= 0?0x30:0x10));
        const std::size_t pos=adaptationLength>=0?5+adaptationLength:4;
        if(adaptationLength>=0) {
            packet[4]=static_cast<uint8_t>(adaptationLength);
            for(size_t i=5;i<pos;i++)packet[i]=0xff;
        }
        for(size_t i=pos;i<188;i++)packet[i]=static_cast<uint8_t>(i ^ 0x75);
        auto original=packet;
        original[3]&=0x3f;
        ok &= expect(encryptPacket(key,packet),"independent AES-CBC encryption");
        dvbstreamer5_ca_ts_result_v1 result{};
        ok &= expect(api->process_ts(p,service.stream_id,packet.data(),
                    packet.size(),&result)==DVBSTREAMER5_CA_RESULT_OK,
                    "backend decrypt call");
        ok &= expect(packet==original,"CISSA payload restored including clear tail");
        ok &= expect(result.packets_changed==1 && result.packets_seen==1,
                    "descrambled packet counted");
    };
    verifyPacket(2,-1); // even, 176 encrypted + 8 clear
    verifyPacket(3,12); // odd, 160 encrypted + 11 clear
    verifyPacket(2,168); // payload <16 bytes: entirely clear
    api->stop_service(p,service.stream_id);
    api->close_reader(p,reader.reader_key);

    reader.backend_config_json=R"({"mode":"biss2-e","esw":"69c4e0d86a7b0430d8cdb78070b4c55a","injected_id":"000102030405060708090a0b0c0d0e0f"})";
    ok &= expect(api->open_reader(p,&reader,error,sizeof(error))==
                 DVBSTREAMER5_CA_RESULT_OK,"BISS2 Mode E official ESW");
    ok &= expect(api->start_service(p,reader.reader_key,&service,error,sizeof(error))==
                 DVBSTREAMER5_CA_RESULT_OK,"Mode E service startup");
    verifyPacket(2,0);
    api->stop_service(p,service.stream_id);
    api->close_reader(p,reader.reader_key);

    reader.backend_config_json=R"({"mode":"biss2-ca","key":"00112233445566778899aabbccddeeff"})";
    ok &= expect(api->open_reader(p,&reader,error,sizeof(error))==
                 DVBSTREAMER5_CA_RESULT_ERROR,"reject unsupported BISS-CA");
    reader.backend_config_json=R"({"mode":"biss2-1","key":"00112233"})";
    ok &= expect(api->open_reader(p,&reader,error,sizeof(error))==
                 DVBSTREAMER5_CA_RESULT_ERROR,"reject short SW");
    ok &= expect(std::string(api->status_json(p)).find(R"("key_material_exposed":false)") != std::string::npos,
                 "status excludes session key");
    api->destroy(p);
    if(ok)std::cout<<"PASS BISS2 Mode1/E EBU+ETSI vectors and packet roundtrips\n";
    return ok?0:1;
}
