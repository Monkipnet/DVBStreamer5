#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::media::dvbtext {

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
