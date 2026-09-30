#include <dvbcsa/dvbcsa.h>
#include <jsoncpp/json/json.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

bool testJsonCpp() {
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string error;
    std::istringstream input(R"({"product":"DVBStreamer5","version":1})");
    if (!Json::parseFromStream(builder, input, &root, &error)) {
        std::cerr << "JsonCpp parse failed: " << error << '\n';
        return false;
    }
    return root["product"].asString() == "DVBStreamer5" && root["version"].asInt() == 1;
}

bool testDvbCsa() {
    const dvbcsa_cw_t controlWord = {0x07, 0x12, 0x34, 0x4d, 0x89, 0xab, 0xcd, 0x01};
    std::array<std::uint8_t, 184> payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xffU);
    }
    const auto original = payload;

    using Key = std::unique_ptr<dvbcsa_key_t, decltype(&dvbcsa_key_free)>;
    Key key(dvbcsa_key_alloc(), &dvbcsa_key_free);
    if (!key) {
        std::cerr << "libdvbcsa key allocation failed\n";
        return false;
    }
    dvbcsa_key_set(controlWord, key.get());
    dvbcsa_encrypt(key.get(), payload.data(), static_cast<unsigned int>(payload.size()));
    if (payload == original) {
        std::cerr << "libdvbcsa encryption did not modify the payload\n";
        return false;
    }
    dvbcsa_decrypt(key.get(), payload.data(), static_cast<unsigned int>(payload.size()));
    return payload == original;
}

} // namespace

int main() {
    if (!testJsonCpp()) return 1;
    if (!testDvbCsa()) return 2;
    std::cout << "PASS: vendored JsonCpp and libdvbcsa\n";
    return 0;
}
