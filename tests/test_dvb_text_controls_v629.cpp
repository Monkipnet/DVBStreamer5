#include "media/DvbText.h"

#include <cassert>
#include <cstdint>
#include <string>

int main() {
    using dvbstreamer5::media::dvbtext::decode;

    // EN 300 468 Annex A.1: emphasis on/off around part of a SKY name.
    const std::uint8_t sky[] = {
        'S', 'k', 'y', ' ', 0x86, 'S', 'h', 'o', 'w', 'c', 'a', 's', 'e', 0x87
    };
    assert(decode(sky, sizeof(sky)) == "Sky Showcase");

    const std::uint8_t cartoon[] = {
        0x86, 'C', 'a', 'r', 't', 'o', 'o', 'n', ' ', 'N', 'e', 't', 'w', 'o', 'r', 'k', 0x87
    };
    assert(decode(cartoon, sizeof(cartoon)) == "Cartoon Network");

    // CR/LF in a channel label is whitespace, not a visible square.
    const std::uint8_t dazn[] = {
        'D', 'A', 'Z', 'N', 0x8a, '1'
    };
    assert(decode(dazn, sizeof(dazn)) == "DAZN 1");

    // Control codes can also be conveyed as UTF-8 in a 0x15-selected field.
    const std::uint8_t utf8Controls[] = {
        0x15, 'N', 'e', 'w', 0xc2, 0x86, 's', 0xc2, 0x87
    };
    assert(decode(utf8Controls, sizeof(utf8Controls)) == "News");

    // Two-byte DVB tables may encode controls in private-use U+E080..U+E09F.
    const std::uint8_t ucs2Controls[] = {
        0x11, 0x00, 'A', 0xe0, 0x86, 0x00, 'B'
    };
    assert(decode(ucs2Controls, sizeof(ucs2Controls)) == "AB");

    // ISO-8859-5 Cyrillic decoding remains intact when controls are present.
    const std::uint8_t cyrillic[] = {
        0x01, 0xb0, 0x86, 0xd0
    };
    assert(decode(cyrillic, sizeof(cyrillic)) == "\u0410\u0430");

    // UTF-8 continuation byte 0x86 is not an isolated DVB control!
    const std::uint8_t utf8Cyrillic[] = {
        0x15, 0xd0, 0x86
    };
    assert(decode(utf8Cyrillic, sizeof(utf8Cyrillic)) == "\u0406");

    const std::uint8_t ordinary[] = {
        'W', 'a', 'r', 'n', 'e', 'r', ' ', 'T', 'V'
    };
    assert(decode(ordinary, sizeof(ordinary)) == "Warner TV");
    return 0;
}
