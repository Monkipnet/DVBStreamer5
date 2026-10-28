#pragma once

// Passive DVB NIT (PID 0x0010) monitor for the existing full-MPTS reader.
// No frontend I/O, tuning, filtering or media-path modification is performed.
#include "media/DvbText.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dvbstreamer5::media::network {

struct DvbNitInfo {
    // NIT actual (table 0x40), network_name_descriptor 0x40.
    // Broadcast network names are NOT guaranteed to be satellite brand names.
    std::string networkName;
    // Orbital position from a satellite_delivery_system_descriptor 0x43
    // matching the frontend's tuned RF frequency and polarization.
    std::string orbitalPosition;
};

class DvbNitMonitor {
public:
    explicit DvbNitMonitor(std::uint32_t frequencyKHz = 0,
                           std::string polarity = {})
        : frequencyKHz_(frequencyKHz), polarity_(std::move(polarity)) {
        section_.reserve(1024);
    }

    DvbNitInfo snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }

    void observe(const std::uint8_t* data, std::size_t bytes) {
        if (!data || bytes < 188) return;
        // The Linux DVB DVR supplies full 188-byte TS packets. Resynchronizing
        // locally also tolerates a corrupt leading byte without copying data.
        std::size_t p = 0;
        while (p + 188 <= bytes) {
            if (data[p] != 0x47U) { ++p; continue; }
            const std::uint16_t pid = static_cast<std::uint16_t>(
                ((data[p+1] & 0x1fU) << 8) | data[p+2]);
            if (pid == 0x0010U) pushPacket(data+p);
            p += 188;
        }
    }

private:
    static constexpr std::size_t kMaximumSection = 1024;

    static bool crcValid(const std::vector<std::uint8_t>& bytes) {
        std::uint32_t crc = 0xffffffffU;
        for (const auto byte : bytes) {
            crc ^= static_cast<std::uint32_t>(byte) << 24U;
            for (unsigned n = 0; n < 8; ++n)
                crc = (crc & 0x80000000U) != 0U
                    ? (crc << 1U) ^ 0x04c11db7U : (crc << 1U);
        }
        return crc == 0;
    }

    static std::int64_t bcd(const std::uint8_t* bytes, std::size_t count) {
        std::int64_t result = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const unsigned high = (bytes[k] >> 4U) & 0x0fU;
            const unsigned low = bytes[k] & 0x0fU;
            if (high > 9U || low > 9U) return -1;
            result = result * 100 + high * 10 + low;
        }
        return result;
    }

    static std::string cleanName(const std::uint8_t* bytes, std::size_t count) {
        std::string value = dvbstreamer5::media::dvbtext::decode(bytes, count);
        for (char& c : value)
            if (static_cast<unsigned char>(c) < 0x20U) c = ' ';
        const auto start = value.find_first_not_of(' ');
        if (start == std::string::npos) return {};
        const auto end = value.find_last_not_of(' ');
        value = value.substr(start, end-start+1);
        if (value.size() > 80)
            value.resize(dvbstreamer5::media::dvbtext::utf8PrefixLength(value, 80));
        return value;
    }

    void parseSection(const std::vector<std::uint8_t>& section) {
        if (section.size() < 16 || section[0] != 0x40U ||
            (section[1] & 0x80U) == 0U ||
            (section[5] & 0x01U) == 0U ||
            3U + ((((section[1] & 0x0fU) << 8U) | section[2])) != section.size() ||
            !crcValid(section)) return;

        const std::size_t contentEnd = section.size() - 4; // CRC excluded
        const std::size_t networkBytes =
            static_cast<std::size_t>(((section[8] & 0x0fU) << 8U) | section[9]);
        const std::size_t networkEnd = 10 + networkBytes;
        if (networkEnd + 2 > contentEnd) return;

        DvbNitInfo parsed;
        for (std::size_t pos = 10; pos + 2 <= networkEnd;) {
            const auto length = section[pos+1];
            const std::size_t next = pos + 2 + length;
            if (next > networkEnd) return;
            if (section[pos] == 0x40U && parsed.networkName.empty())
                parsed.networkName = cleanName(section.data()+pos+2, length);
            pos = next;
        }

        const std::size_t transportBytes =
            static_cast<std::size_t>(
                ((section[networkEnd] & 0x0fU) << 8U) | section[networkEnd+1]);
        const std::size_t transportEnd = networkEnd + 2 + transportBytes;
        if (transportEnd > contentEnd) return;

        // A NIT can list many satellites. Do not use the first unrelated
        // satellite position; match the currently tuned DVB-S/S2 transponder.
        for (std::size_t transport = networkEnd + 2;
             transport + 6 <= transportEnd;) {
            const std::size_t descLen =
                static_cast<std::size_t>(
                    ((section[transport+4] & 0x0fU) << 8U) | section[transport+5]);
            const std::size_t descEnd = transport + 6 + descLen;
            if (descEnd > transportEnd) return;

            for (std::size_t d = transport+6; d+2 <= descEnd;) {
                const auto descriptorLength = section[d+1];
                const std::size_t next = d + 2 + descriptorLength;
                if (next > descEnd) return;
                if (section[d] == 0x43U && descriptorLength >= 11U) {
                    const auto* sat = section.data()+d+2;
                    const auto frequency = bcd(sat,4); // units of 10 kHz
                    const auto position = bcd(sat+4,2); // tenths of a degree
                    const unsigned pol = (sat[6] >> 5U) & 0x03U;
                    const bool frequencyMatch = frequency >= 0 &&
                        std::llabs(frequency*10 - static_cast<std::int64_t>(frequencyKHz_)) <= 3000;
                    const bool polarityMatch = polarity_.empty() ||
                        (pol == 0U && polarity_ == "H") ||
                        (pol == 1U && polarity_ == "V");
                    if (frequencyMatch && polarityMatch && position >= 0) {
                        parsed.orbitalPosition = std::to_string(position/10)+'.'+
                            std::to_string(position%10)+(sat[6] & 0x80U ? "°E" : "°W");
                    }
                }
                d = next;
            }
            transport = descEnd;
        }

        if (parsed.networkName.empty() && parsed.orbitalPosition.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!parsed.networkName.empty()) info_.networkName = std::move(parsed.networkName);
        if (!parsed.orbitalPosition.empty()) info_.orbitalPosition = std::move(parsed.orbitalPosition);
    }

    void reset() {
        section_.clear();
        expected_ = 0;
    }

    void append(const std::uint8_t* data, std::size_t bytes) {
        if (section_.empty() || !bytes) return;
        std::size_t pos = 0;
        if (expected_ == 0 && section_.size() < 3) {
            const auto take = std::min(bytes, 3 - section_.size());
            section_.insert(section_.end(),data,data+take);
            pos += take;
            if (section_.size() < 3) return;
            expected_ = 3U + ((((section_[1] & 0x0fU) << 8U) | section_[2]));
            if (expected_ < 16 || expected_ > kMaximumSection) {
                reset();
                return;
            }
        }
        if (section_.size() < expected_ && pos < bytes) {
            const auto take = std::min(bytes-pos, expected_-section_.size());
            section_.insert(section_.end(),data+pos,data+pos+take);
        }
        if (expected_ && section_.size() == expected_) {
            parseSection(section_);
            reset();
        }
    }

    void consume(const std::uint8_t* data, std::size_t bytes) {
        std::size_t pos = 0;
        while (pos < bytes) {
            if (data[pos] == 0xffU) return;
            const auto remaining = bytes-pos;
            if (remaining < 3) {
                section_.assign(data+pos,data+bytes);
                expected_=0;
                return;
            }
            const std::size_t total =
                3U + ((((data[pos+1] & 0x0fU) << 8U) | data[pos+2]));
            if (total < 16 || total > kMaximumSection) {
                reset();
                return;
            }
            if (remaining < total) {
                section_.assign(data+pos,data+bytes);
                expected_=total;
                return;
            }
            std::vector<std::uint8_t> complete(data+pos,data+pos+total);
            parseSection(complete);
            pos += total;
        }
    }

    void pushPacket(const std::uint8_t* packet) {
        if ((packet[1] & 0x80U) != 0U || (packet[3] & 0xc0U) != 0U) {
            reset();
            continuityValid_=false;
            return;
        }
        const unsigned adaptation = (packet[3] >> 4U) & 0x03U;
        if ((adaptation & 1U) == 0U) return;
        std::size_t offset = 4;
        if ((adaptation & 2U) != 0U) {
            if (packet[4] > 183U) { reset(); return; }
            if (packet[4] > 0 && (packet[5] & 0x80U) != 0U) reset();
            offset = 5 + packet[4];
            if (offset >= 188) return;
        }
        const unsigned cc = packet[3] & 0x0fU;
        if (continuityValid_) {
            if (cc == continuity_) return;
            if (cc != ((continuity_+1U) & 0x0fU)) reset();
        }
        continuity_ = static_cast<std::uint8_t>(cc);
        continuityValid_ = true;

        const bool start = (packet[1] & 0x40U) != 0U;
        const auto* payload = packet+offset;
        std::size_t size = 188-offset;
        if (!start) {
            if (!section_.empty()) append(payload,size);
            return;
        }
        if (!size) return;
        const auto pointer = payload[0];
        ++payload;--size;
        if (pointer > size) { reset();return; }
        if (!section_.empty() && pointer) append(payload,pointer);
        if (!section_.empty()) reset();
        consume(payload+pointer,size-pointer);
    }

    const std::uint32_t frequencyKHz_ = 0;
    const std::string polarity_;
    std::vector<std::uint8_t> section_;
    std::size_t expected_ = 0;
    bool continuityValid_ = false;
    std::uint8_t continuity_ = 0;
    mutable std::mutex mutex_;
    DvbNitInfo info_;
};

} // namespace dvbstreamer5::media::network
