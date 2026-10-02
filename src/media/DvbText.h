#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::media::dvbtext {

inline void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7fU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0x10ffffU) {
        out.push_back(static_cast<char>(0xf0U | (cp >> 18)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    }
}

inline bool validUtf8(const std::uint8_t* data, std::size_t size) noexcept {
    if (!data && size != 0) return false;
    std::size_t i = 0;
    while (i < size) {
        const std::uint8_t c = data[i++];
        if (c < 0x80U) continue;

        unsigned continuation = 0;
        std::uint32_t codepoint = 0;
        if ((c & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = c & 0x1fU;
            if (codepoint < 2U) return false;
        } else if ((c & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = c & 0x0fU;
        } else if ((c & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = c & 0x07U;
        } else {
            return false;
        }

        if (i + continuation > size) return false;
        for (unsigned j = 0; j < continuation; ++j) {
            const std::uint8_t cc = data[i++];
            if ((cc & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (cc & 0x3fU);
        }

        if ((continuation == 2 && codepoint < 0x800U) ||
            (continuation == 3 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            return false;
        }
    }
    return true;
}

inline std::string decodeLatin1(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t c = data[i];
        if (c < 0x20U) continue;
        appendUtf8(out, c);
    }
    return out;
}

inline std::string decodeIso88595(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t c = data[i];
        if (c < 0x20U) continue;

        std::uint32_t cp = c;
        if (c == 0xa0U) cp = 0x00a0U;
        else if (c >= 0xa1U && c <= 0xacU) cp = 0x0401U + (c - 0xa1U);
        else if (c == 0xadU) cp = 0x00adU;
        else if (c >= 0xaeU && c <= 0xafU) cp = 0x040eU + (c - 0xaeU);
        else if (c >= 0xb0U && c <= 0xcfU) cp = 0x0410U + (c - 0xb0U);
        else if (c >= 0xd0U && c <= 0xefU) cp = 0x0430U + (c - 0xd0U);
        else if (c == 0xf0U) cp = 0x2116U;
        else if (c >= 0xf1U && c <= 0xfcU) cp = 0x0451U + (c - 0xf1U);
        else if (c == 0xfdU) cp = 0x00a7U;
        else if (c >= 0xfeU) cp = 0x045eU + (c - 0xfeU);

        appendUtf8(out, cp);
    }
    return out;
}

inline std::string decodeIso88599(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t c = data[i];
        if (c < 0x20U) continue;
        std::uint32_t cp = c;
        switch (c) {
            case 0xd0U: cp = 0x011eU; break; // Ğ
            case 0xddU: cp = 0x0130U; break; // İ
            case 0xdeU: cp = 0x015eU; break; // Ş
            case 0xf0U: cp = 0x011fU; break; // ğ
            case 0xfdU: cp = 0x0131U; break; // ı
            case 0xfeU: cp = 0x015fU; break; // ş
            default: break;
        }
        appendUtf8(out, cp);
    }
    return out;
}

inline std::string decodeUcs2Be(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        const std::uint32_t cp =
            (static_cast<std::uint32_t>(data[i]) << 8) |
            static_cast<std::uint32_t>(data[i + 1]);
        if (cp == 0U || (cp >= 0xd800U && cp <= 0xdfffU)) continue;
        appendUtf8(out, cp);
    }
    return out;
}

// Decode DVB SI text according to the most common ETSI EN 300 468 Annex A
// selectors used by satellite operators. In particular, selector 0x01 is
// ISO-8859-5 (Cyrillic), which is used on TürkmenÄlem/Express multiplexes.
// 0x15 is UTF-8 and 0x11 is UCS-2 big-endian.
inline std::string decode(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) return {};

    const std::uint8_t selector = data[0];
    if (selector >= 0x20U) {
        if (validUtf8(data, size)) {
            return std::string(reinterpret_cast<const char*>(data), size);
        }
        return decodeLatin1(data, size);
    }

    if (selector == 0x15U) {
        ++data;
        --size;
        return validUtf8(data, size)
            ? std::string(reinterpret_cast<const char*>(data), size)
            : decodeLatin1(data, size);
    }

    if (selector == 0x11U) {
        ++data;
        --size;
        return decodeUcs2Be(data, size);
    }

    unsigned iso8859Table = 0;
    std::size_t skip = 1;
    if (selector >= 0x01U && selector <= 0x07U) {
        iso8859Table = static_cast<unsigned>(selector) + 4U;
    } else if (selector >= 0x09U && selector <= 0x0bU) {
        // 0x08 is reserved; 0x09..0x0b select ISO-8859-13..15.
        iso8859Table = static_cast<unsigned>(selector) + 4U;
    } else if (selector == 0x10U && size >= 3 && data[1] == 0x00U) {
        iso8859Table = data[2];
        skip = 3;
    }

    if (skip > size) return {};
    data += skip;
    size -= skip;

    if (iso8859Table == 5U) return decodeIso88595(data, size);
    if (iso8859Table == 9U) return decodeIso88599(data, size);

    // Unknown/reserved table: preserve the text as UTF-8 when the broadcaster
    // actually sends UTF-8 behind a non-standard selector; otherwise retain
    // the previous single-byte behavior instead of emitting invalid JSON.
    if (validUtf8(data, size)) {
        return std::string(reinterpret_cast<const char*>(data), size);
    }
    return decodeLatin1(data, size);
}

inline bool isAsciiPrintable(const std::string& text) noexcept {
    for (unsigned char ch : text) {
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    return true;
}

inline std::size_t utf8PrefixLength(const std::string& text, std::size_t maximum) noexcept {
    std::size_t i = 0;
    std::size_t last = 0;
    while (i < text.size() && i < maximum) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t width = 1;
        if ((c & 0x80U) == 0) width = 1;
        else if ((c & 0xe0U) == 0xc0U) width = 2;
        else if ((c & 0xf0U) == 0xe0U) width = 3;
        else if ((c & 0xf8U) == 0xf0U) width = 4;
        else { ++i; last = i; continue; }
        if (i + width > text.size() || i + width > maximum) break;
        bool valid = true;
        for (std::size_t j = 1; j < width; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0U) != 0x80U) {
                valid = false;
                break;
            }
        }
        if (!valid) { ++i; last = i; continue; }
        i += width;
        last = i;
    }
    return last;
}

// ETSI EN 300 468 Annex A: character table selection byte 0x15 selects UTF-8.
// Plain printable ASCII is kept unchanged for maximum receiver compatibility.
inline std::vector<std::uint8_t> encode(const std::string& text, std::size_t maximumBytes) {
    std::vector<std::uint8_t> out;
    if (maximumBytes == 0 || text.empty()) return out;
    if (isAsciiPrintable(text)) {
        const std::size_t n = text.size() < maximumBytes ? text.size() : maximumBytes;
        out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(n));
        return out;
    }
    if (maximumBytes < 2) return out;
    out.push_back(0x15); // UTF-8 selector for DVB text fields.
    const std::size_t n = utf8PrefixLength(text, maximumBytes - 1);
    out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

} // namespace dvbstreamer5::media::dvbtext
