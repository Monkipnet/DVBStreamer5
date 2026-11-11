#pragma once
// EBU Tech 3292 v3 (BISS2-1/E); ETSI TS 103 127 section 6 (DVB-CISSA).
// BISS2-CA (ECM/EMM and receiver entitlement) is intentionally NOT provided.
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <openssl/evp.h>
#include <openssl/crypto.h>

namespace dvbstreamer5::ca::biss2 {

inline int nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool parseHex128(const std::string& input,
                        std::array<uint8_t, 16>& out) noexcept {
    out.fill(0);
    // Accept separators to support broadcast operator key formatting.
    std::size_t idx = 0;
    if (input.size() >= 2 && input[0] == '0' &&
        (input[1] == 'x' || input[1] == 'X')) idx = 2;
    std::size_t n = 0;
    int high = -1;
    for (; idx < input.size(); ++idx) {
        const unsigned char c = static_cast<unsigned char>(input[idx]);
        if (std::isspace(c) || c == ':' || c == '-') continue;
        const int v = nibble(static_cast<char>(c));
        if (v < 0 || n >= 16) { out.fill(0); return false; }
        if (high < 0) high = v;
        else {
            out[n++] = static_cast<uint8_t>((high << 4) | v);
            high = -1;
        }
    }
    if (n != 16 || high >= 0) { out.fill(0); return false; }
    return true;
}

inline bool nonzeroKey(const std::array<uint8_t, 16>& key) noexcept {
    uint8_t nonzero = 0;
    for (const uint8_t v : key) nonzero |= v;
    return nonzero != 0;
}

inline bool decryptEsw(const std::array<uint8_t,16>& encrypted,
                       const std::array<uint8_t,16>& receiverId,
                       std::array<uint8_t,16>& sessionWord) noexcept {
    sessionWord.fill(0);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int n = 0, tail = 0;
    const bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                           receiverId.data(), nullptr) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_DecryptUpdate(ctx, sessionWord.data(), &n,
                          encrypted.data(), 16) == 1 &&
        n == 16 &&
        EVP_DecryptFinal_ex(ctx, sessionWord.data() + n, &tail) == 1 &&
        tail == 0;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) sessionWord.fill(0);
    return ok;
}

inline constexpr std::array<uint8_t, 16> kCissaIv{{
    0x44, 0x56, 0x42, 0x54, 0x4d, 0x43, 0x50, 0x54,
    0x41, 0x45, 0x53, 0x43, 0x49, 0x53, 0x53, 0x41
}};

// CTS or AES-CTR is NOT CISSA. The trailing partial block remains clear.
// Decrypt to an isolated scratch buffer; never corrupt the original TS
// packet or clear its scrambling bits if OpenSSL rejects a block.
inline bool decryptTsPayload(EVP_CIPHER_CTX* ctx,
                             const std::array<uint8_t,16>& sessionWord,
                             uint8_t* packet) noexcept {
    if (!ctx || !packet || packet[0] != 0x47 || (packet[1] & 0x80))
        return false;
    const unsigned scrambling = (packet[3] >> 6) & 3;
    if (scrambling != 2 && scrambling != 3) return false;
    const unsigned afc = (packet[3] >> 4) & 3;
    if (afc != 1 && afc != 3) return false;
    std::size_t payload = 4;
    if (afc == 3) {
        const unsigned length = packet[4];
        if (length > 182) return false;
        payload = 5 + length;
    }
    if (payload >= 188) return false;
    const std::size_t count = (188 - payload) & ~std::size_t(15);
    if (count == 0) {
        // Sub-16-byte CISSA payload is explicitly in the clear.
        packet[3] &= 0x3f;
        return true;
    }
    std::array<uint8_t, 184> decoded{};
    int n = 0, tail = 0;
    const bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                           sessionWord.data(), kCissaIv.data()) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_DecryptUpdate(ctx, decoded.data(), &n,
                          packet + payload, static_cast<int>(count)) == 1 &&
        n == static_cast<int>(count) &&
        EVP_DecryptFinal_ex(ctx, decoded.data() + n, &tail) == 1 &&
        tail == 0;
    if (ok) {
        std::memcpy(packet + payload, decoded.data(), count);
        packet[3] &= 0x3f;
    }
    OPENSSL_cleanse(decoded.data(), decoded.size());
    return ok;
}
} // namespace dvbstreamer5::ca::biss2
