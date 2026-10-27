#include "media/DvbEpgCollector.h"

#include "media/DvbText.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <utility>

namespace dvbstreamer5::media::epg {
namespace {

constexpr std::size_t kTsPacketSize = 188;
constexpr std::uint16_t kEitPid = 0x0012;
constexpr std::size_t kMaximumSectionSize = 4096;
constexpr std::size_t kMaximumStoredEvents = 512;

int bcdByte(std::uint8_t value) {
    const int high = (value >> 4) & 0x0f;
    const int low = value & 0x0f;
    return high <= 9 && low <= 9 ? high * 10 + low : -1;
}

bool parseDvbUtc(const std::uint8_t* data, std::int64_t& utc) {
    if (!data) return false;
    if (data[0] == 0xff && data[1] == 0xff) return false;

    const std::uint16_t mjd =
        static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    const int hour = bcdByte(data[2]);
    const int minute = bcdByte(data[3]);
    const int second = bcdByte(data[4]);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }

    constexpr std::int64_t kUnixEpochMjd = 40587;
    const std::int64_t days =
        static_cast<std::int64_t>(mjd) - kUnixEpochMjd;
    utc = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    return true;
}

bool parseDuration(const std::uint8_t* data, std::uint32_t& seconds) {
    const int hour = bcdByte(data[0]);
    const int minute = bcdByte(data[1]);
    const int second = bcdByte(data[2]);
    if (hour < 0 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }
    seconds = static_cast<std::uint32_t>(
        hour * 3600 + minute * 60 + second);
    return true;
}

std::string compactWhitespace(std::string text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(static_cast<char>(ch));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void appendDescription(std::string& target, std::string value) {
    value = compactWhitespace(std::move(value));
    if (value.empty()) return;
    if (!target.empty()) target.push_back(' ');
    target += value;
}

void parseEventDescriptors(const std::uint8_t* data, std::size_t size,
                           std::string& title, std::string& description) {
    std::size_t offset = 0;
    while (offset + 2 <= size) {
        const std::uint8_t tag = data[offset++];
        const std::size_t length = data[offset++];
        if (offset + length > size) break;
        const std::uint8_t* body = data + offset;

        if (tag == 0x4d && length >= 5) {
            // short_event_descriptor:
            // ISO_639_language_code[3], event_name_length, event_name,
            // text_length, text.
            std::size_t pos = 3;
            if (pos < length) {
                const std::size_t nameLength = body[pos++];
                if (pos + nameLength <= length) {
                    if (title.empty()) {
                        title = compactWhitespace(
                            dvbtext::decode(body + pos, nameLength));
                    }
                    pos += nameLength;
                    if (pos < length) {
                        const std::size_t textLength = body[pos++];
                        if (pos + textLength <= length) {
                            appendDescription(
                                description,
                                dvbtext::decode(body + pos, textLength));
                        }
                    }
                }
            }
        } else if (tag == 0x4e && length >= 6) {
            // extended_event_descriptor. Items are intentionally skipped here;
            // the free-form text that follows them is the useful programme
            // description for the dashboard/schedule.
            std::size_t pos = 4; // descriptor number + language
            if (pos < length) {
                const std::size_t itemsLength = body[pos++];
                if (pos + itemsLength <= length) {
                    pos += itemsLength;
                    if (pos < length) {
                        const std::size_t textLength = body[pos++];
                        if (pos + textLength <= length) {
                            appendDescription(
                                description,
                                dvbtext::decode(body + pos, textLength));
                        }
                    }
                }
            }
        }
        offset += length;
    }
}

std::int64_t systemNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

DvbEpgCollector::DvbEpgCollector(std::uint16_t serviceId,
                                 unsigned scheduleDays)
    : configuredServiceId_(serviceId),
      resolvedServiceId_(serviceId),
      scheduleDays_(std::max(1U, std::min(scheduleDays, 7U))) {
    sectionBytes_.reserve(kMaximumSectionSize);
}

void DvbEpgCollector::resetSection() noexcept {
    sectionBytes_.clear();
    sectionExpected_ = 0;
}

void DvbEpgCollector::appendSectionBytes(
    const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0 || sectionBytes_.empty()) return;

    std::size_t offset = 0;
    if (sectionExpected_ == 0 && sectionBytes_.size() < 3) {
        const std::size_t take =
            std::min<std::size_t>(3 - sectionBytes_.size(), size);
        sectionBytes_.insert(
            sectionBytes_.end(), data, data + take);
        offset += take;
        if (sectionBytes_.size() < 3) return;

        const std::size_t sectionLength =
            static_cast<std::size_t>(
                ((sectionBytes_[1] & 0x0fU) << 8) | sectionBytes_[2]);
        sectionExpected_ = 3 + sectionLength;
        if (sectionExpected_ < 3 ||
            sectionExpected_ > kMaximumSectionSize) {
            resetSection();
            return;
        }
    }

    if (sectionExpected_ > sectionBytes_.size() && offset < size) {
        const std::size_t take = std::min(
            sectionExpected_ - sectionBytes_.size(), size - offset);
        sectionBytes_.insert(
            sectionBytes_.end(), data + offset, data + offset + take);
    }

    if (sectionExpected_ != 0 &&
        sectionBytes_.size() == sectionExpected_) {
        const auto complete = sectionBytes_;
        resetSection();
        parseSection(complete);
    }
}

void DvbEpgCollector::consumePayload(
    const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        if (data[offset] == 0xffU) break;

        if (size - offset < 3) {
            sectionBytes_.assign(data + offset, data + size);
            sectionExpected_ = 0;
            return;
        }

        const std::size_t sectionLength =
            static_cast<std::size_t>(
                ((data[offset + 1] & 0x0fU) << 8) | data[offset + 2]);
        const std::size_t total = 3 + sectionLength;
        if (total < 3 || total > kMaximumSectionSize) {
            resetSection();
            return;
        }

        if (size - offset >= total) {
            parseSection(std::vector<std::uint8_t>(
                data + offset, data + offset + total));
            offset += total;
            continue;
        }

        sectionBytes_.assign(data + offset, data + size);
        sectionExpected_ = total;
        return;
    }
}

void DvbEpgCollector::pushPacket(const std::uint8_t* packet) {
    if (!packet || packet[0] != 0x47U) return;

    const std::uint16_t pid = static_cast<std::uint16_t>(
        ((packet[1] & 0x1fU) << 8) | packet[2]);
    if (pid != kEitPid) return;

    if ((packet[1] & 0x80U) != 0) {
        resetSection();
        continuityValid_ = false;
        return;
    }

    const std::uint8_t adaptationControl =
        static_cast<std::uint8_t>((packet[3] >> 4) & 0x03U);
    if ((adaptationControl & 0x01U) == 0) return;

    const std::uint8_t continuity =
        static_cast<std::uint8_t>(packet[3] & 0x0fU);
    const bool discontinuity =
        (adaptationControl & 0x02U) != 0 &&
        packet[4] > 0 && (packet[5] & 0x80U) != 0;
    if (discontinuity) {
        resetSection();
    } else if (continuityValid_) {
        const std::uint8_t expected =
            static_cast<std::uint8_t>((continuity_ + 1U) & 0x0fU);
        if (continuity == continuity_) return;
        if (continuity != expected) resetSection();
    }
    continuity_ = continuity;
    continuityValid_ = true;

    std::size_t payloadOffset = 4;
    if ((adaptationControl & 0x02U) != 0) {
        payloadOffset = 5U + packet[4];
        if (payloadOffset >= kTsPacketSize) return;
    }

    const bool payloadStart = (packet[1] & 0x40U) != 0;
    const std::uint8_t* payload = packet + payloadOffset;
    std::size_t payloadSize = kTsPacketSize - payloadOffset;

    if (!payloadStart) {
        if (!sectionBytes_.empty()) appendSectionBytes(payload, payloadSize);
        return;
    }

    if (payloadSize == 0) return;
    const std::size_t pointer = payload[0];
    ++payload;
    --payloadSize;
    if (pointer > payloadSize) {
        resetSection();
        return;
    }

    if (!sectionBytes_.empty() && pointer > 0) {
        appendSectionBytes(payload, pointer);
    }
    // A payload-unit start defines a new section boundary even if a malformed
    // prior section failed to complete.
    if (!sectionBytes_.empty()) resetSection();

    payload += pointer;
    payloadSize -= pointer;
    consumePayload(payload, payloadSize);
}

void DvbEpgCollector::push(const std::uint8_t* data, std::size_t size) {
    if (!data || size < kTsPacketSize) return;

    std::size_t offset = 0;
    // Native relay source observers are packet-aligned. Shared DVB reads are
    // normally aligned as well; this small resync also tolerates a damaged
    // leading byte without allocating/copying the full transponder.
    while (offset + kTsPacketSize <= size) {
        if (data[offset] != 0x47U) {
            ++offset;
            continue;
        }
        pushPacket(data + offset);
        offset += kTsPacketSize;
    }
}

void DvbEpgCollector::parseSection(
    const std::vector<std::uint8_t>& section) {
    if (section.size() < 18) return;

    const std::uint8_t tableId = section[0];
    const bool presentFollowingActual = tableId == 0x4eU;
    const bool scheduleActual = tableId >= 0x50U && tableId <= 0x5fU;
    if (!presentFollowingActual && !scheduleActual) return;

    const std::size_t declared =
        3U + static_cast<std::size_t>(
            ((section[1] & 0x0fU) << 8) | section[2]);
    if (declared != section.size() || declared < 18) return;

    const std::uint16_t serviceId = static_cast<std::uint16_t>(
        (section[3] << 8) | section[4]);
    if (configuredServiceId_ != 0 &&
        serviceId != configuredServiceId_) {
        return;
    }
    if (configuredServiceId_ == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (resolvedServiceId_ == 0) resolvedServiceId_ = serviceId;
        if (serviceId != resolvedServiceId_) return;
    }

    const std::size_t eventsEnd = section.size() - 4U;
    std::size_t offset = 14U;
    std::vector<DvbEpgEvent> parsed;

    while (offset + 12U <= eventsEnd) {
        const std::size_t descriptorLength =
            static_cast<std::size_t>(
                ((section[offset + 10] & 0x0fU) << 8) |
                section[offset + 11]);
        if (offset + 12U + descriptorLength > eventsEnd) break;

        DvbEpgEvent event;
        event.serviceId = serviceId;
        event.eventId = static_cast<std::uint16_t>(
            (section[offset] << 8) | section[offset + 1]);
        if (!parseDvbUtc(section.data() + offset + 2, event.startUtc) ||
            !parseDuration(
                section.data() + offset + 7, event.durationSeconds)) {
            offset += 12U + descriptorLength;
            continue;
        }
        event.runningStatus =
            static_cast<std::uint8_t>((section[offset + 10] >> 5) & 0x07U);
        event.freeCa = (section[offset + 10] & 0x10U) != 0;
        parseEventDescriptors(
            section.data() + offset + 12,
            descriptorLength,
            event.title,
            event.description);
        parsed.push_back(std::move(event));
        offset += 12U + descriptorLength;
    }

    if (!parsed.empty()) commitEvents(parsed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++sectionsParsed_;
    }
}

void DvbEpgCollector::commitEvents(
    std::vector<DvbEpgEvent>& incoming) {
    const std::int64_t now = systemNowSeconds();
    const std::int64_t oldest = now - 6 * 3600LL;
    const std::int64_t newest =
        now + static_cast<std::int64_t>(scheduleDays_) * 86400LL;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& event : incoming) {
        if (event.startUtc + event.durationSeconds < oldest ||
            event.startUtc > newest) {
            continue;
        }
        auto existing = std::find_if(
            events_.begin(), events_.end(),
            [&event](const DvbEpgEvent& candidate) {
                return candidate.eventId == event.eventId &&
                    candidate.serviceId == event.serviceId &&
                    candidate.startUtc == event.startUtc;
            });
        if (existing == events_.end()) {
            events_.push_back(std::move(event));
        } else {
            *existing = std::move(event);
        }
    }

    events_.erase(
        std::remove_if(
            events_.begin(), events_.end(),
            [oldest, newest](const DvbEpgEvent& event) {
                return event.startUtc + event.durationSeconds < oldest ||
                    event.startUtc > newest;
            }),
        events_.end());

    std::sort(
        events_.begin(), events_.end(),
        [](const DvbEpgEvent& left, const DvbEpgEvent& right) {
            if (left.startUtc != right.startUtc)
                return left.startUtc < right.startUtc;
            return left.eventId < right.eventId;
        });
    if (events_.size() > kMaximumStoredEvents) {
        events_.erase(
            events_.begin(),
            events_.begin() +
                static_cast<std::ptrdiff_t>(
                    events_.size() - kMaximumStoredEvents));
    }
}

DvbEpgSnapshot DvbEpgCollector::snapshot(
    std::size_t maximumEvents) const {
    DvbEpgSnapshot result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.serviceId =
        configuredServiceId_ != 0 ? configuredServiceId_ : resolvedServiceId_;
    result.sectionsParsed = sectionsParsed_;

    const std::size_t count =
        std::min(maximumEvents, events_.size());
    result.events.insert(
        result.events.end(),
        events_.begin(),
        events_.begin() + static_cast<std::ptrdiff_t>(count));
    return result;
}

} // namespace dvbstreamer5::media::epg
