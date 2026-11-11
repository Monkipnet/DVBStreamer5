#pragma once
// BISS-1 fixed session-word parsing, isolated from Newcamd/iCAM.
// Accept 6 significant CW bytes (12 hex digits) or 8 bytes (16 hex
// digits including the two BISS checksum bytes).
#include <array>
#include <cctype>
#include <cstdint>
#include <string>

namespace dvbstreamer5::ca::biss {
inline int hexNibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
inline bool parseBiss1Key(const std::string& text, std::array<uint8_t,8>& cw) {
    cw.fill(0);
    std::string clean;
    clean.reserve(16);
    size_t index = 0;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        index = 2;
    for (; index < text.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (std::isspace(ch) || ch == ':' || ch == '-') continue;
        if (hexNibble(static_cast<char>(ch)) < 0 || clean.size() >= 16) return false;
        clean.push_back(static_cast<char>(ch));
    }
    if (clean.size() != 12 && clean.size() != 16) return false;
    std::array<uint8_t,8> parsed{};
    for (size_t i=0, j=0; i<clean.size(); i+=2, ++j)
        parsed[j] = static_cast<uint8_t>(
            (hexNibble(clean[i]) << 4) | hexNibble(clean[i+1]));
    if (clean.size() == 12) {
        cw[0] = parsed[0]; cw[1] = parsed[1]; cw[2] = parsed[2];
        cw[3] = static_cast<uint8_t>((cw[0]+cw[1]+cw[2]) & 0xff);
        cw[4] = parsed[3]; cw[5] = parsed[4]; cw[6] = parsed[5];
        cw[7] = static_cast<uint8_t>((cw[4]+cw[5]+cw[6]) & 0xff);
    } else {
        cw = parsed;
        if (cw[3] != static_cast<uint8_t>((cw[0]+cw[1]+cw[2]) & 0xff) ||
            cw[7] != static_cast<uint8_t>((cw[4]+cw[5]+cw[6]) & 0xff)) {
            cw.fill(0); return false;
        }
    }
    // Do not accept the all-zero CW. No accidental "decoded" status.
    uint8_t combined = 0;
    for (uint8_t b : cw) combined |= b;
    if (!combined) { cw.fill(0); return false; }
    return true;
}
inline size_t tsPayloadOffset(const uint8_t* packet) noexcept {
    if (!packet || packet[0] != 0x47) return 188;
    const uint8_t afc = (packet[3] >> 4) & 3;
    if (afc == 1) return 4;
    if (afc != 3) return 188;
    const size_t offset = 5U + packet[4];
    return offset < 188 ? offset : 188;
}
} // namespace dvbstreamer5::ca::biss
