#include "DvbEitOutput.h"
#include "media/DvbText.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

namespace {
using Packet = dvbstreamer5::media::mpegts::Packet;
constexpr std::uint16_t kEitPid = 0x0012U;
constexpr std::size_t kMaxScheduleEvents = 768;
constexpr std::size_t kMaxPacketBurst = 5;
constexpr auto kEmitInterval = std::chrono::milliseconds(200);
constexpr auto kRefreshInterval = std::chrono::seconds(20);
constexpr auto kPfInterval = std::chrono::seconds(3);

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8U) | p[1]);
}
void append16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8U));
    out.push_back(static_cast<std::uint8_t>(v));
}
std::uint32_t mpegCrc(const std::uint8_t* p, std::size_t len) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint32_t>(p[i]) << 24U;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x80000000U) != 0
                ? (crc << 1U) ^ 0x04c11db7U : crc << 1U;
    }
    return crc;
}
void appendCrc(std::vector<std::uint8_t>& section) {
    const auto crc = mpegCrc(section.data(), section.size());
    for (int shift : {24, 16, 8, 0})
        section.push_back(static_cast<std::uint8_t>(crc >> shift));
}
std::uint8_t bcd(unsigned x) {
    return static_cast<std::uint8_t>(((x / 10U) << 4U) | (x % 10U));
}
bool writeDvbTime(std::vector<std::uint8_t>& data, Json::Int64 seconds) {
    if (seconds < 0 || seconds > 4102444799LL) return false;
    // MJD of Unix epoch is 40587; timestamps in EIT are UTC (no XMLTV offset).
    const std::uint32_t mjd = 40587U +
        static_cast<std::uint32_t>(seconds / 86400);
    const unsigned daytime = static_cast<unsigned>(seconds % 86400);
    append16(data, static_cast<std::uint16_t>(mjd));
    data.push_back(bcd(daytime / 3600U));
    data.push_back(bcd((daytime / 60U) % 60U));
    data.push_back(bcd(daytime % 60U));
    return true;
}
std::vector<std::uint8_t> shortEvent(const Json::Value& event) {
    const std::string name = event.get("title", "").asString();
    if (name.empty()) return {};
    const std::string desc = event.get("description", "").asString();
    const auto title = dvbstreamer5::media::dvbtext::encode(name, 80);
    const auto summary = dvbstreamer5::media::dvbtext::encode(desc, 140);
    std::vector<std::uint8_t> result;
    result.reserve(7 + title.size() + summary.size());
    result.push_back(0x4d); // short_event_descriptor
    result.push_back(static_cast<std::uint8_t>(5U + title.size() + summary.size()));
    result.push_back('u'); result.push_back('n'); result.push_back('d');
    result.push_back(static_cast<std::uint8_t>(title.size()));
    result.insert(result.end(), title.begin(), title.end());
    result.push_back(static_cast<std::uint8_t>(summary.size()));
    result.insert(result.end(), summary.begin(), summary.end());
    return result;
}
struct Event {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    std::uint16_t id = 0;
    std::vector<std::uint8_t> descriptor;
};
std::vector<std::uint8_t> eventBytes(const Event& event, bool currentlyRunning = false) {
    const auto duration = event.stop - event.start;
    if (duration <= 0 || duration > 359999LL) return {};
    std::vector<std::uint8_t> bytes;
    bytes.reserve(12U + event.descriptor.size());
    append16(bytes, event.id);
    if (!writeDvbTime(bytes, event.start)) return {};
    const unsigned sec = static_cast<unsigned>(duration);
    bytes.push_back(bcd(sec / 3600U));
    bytes.push_back(bcd((sec / 60U) % 60U));
    bytes.push_back(bcd(sec % 60U));
    const std::size_t dl = event.descriptor.size();
    // 4 = running, 1 = not running; free_CA_mode=0, length high nibble.
    const auto running = currentlyRunning ? 4U : 1U;
    bytes.push_back(static_cast<std::uint8_t>((running << 5U) |
                    ((dl >> 8U) & 0x0fU)));
    bytes.push_back(static_cast<std::uint8_t>(dl));
    bytes.insert(bytes.end(), event.descriptor.begin(), event.descriptor.end());
    return bytes;
}
std::vector<std::uint8_t> makeEit(
    std::uint8_t tableId, std::uint16_t sid, std::uint16_t tsid,
    std::uint16_t onid, std::uint8_t version, std::uint8_t sectionNumber,
    std::uint8_t lastSection, std::uint8_t segmentLast,
    std::uint8_t lastTableId, const std::vector<std::uint8_t>& events) {
    const auto sectionLength = 11U + events.size() + 4U;
    if (sectionLength > 1021U) return {};
    std::vector<std::uint8_t> section;
    section.reserve(sectionLength + 3U);
    section.push_back(tableId);
    section.push_back(static_cast<std::uint8_t>(0xf0U | ((sectionLength >> 8U) & 0x0fU)));
    section.push_back(static_cast<std::uint8_t>(sectionLength));
    append16(section, sid);
    section.push_back(static_cast<std::uint8_t>(0xc1U | ((version & 31U) << 1U)));
    section.push_back(sectionNumber);
    section.push_back(lastSection);
    append16(section, tsid);
    append16(section, onid);
    section.push_back(segmentLast);
    section.push_back(lastTableId);
    section.insert(section.end(), events.begin(), events.end());
    appendCrc(section);
    return section;
}

// DVB EIT is emitted only for the single service in the outgoing PAT/SDT.
// This avoids assigning the wrong SID to an unremapped multi-service input.
bool singleServicePat(const std::uint8_t* sec, std::size_t len,
                      std::uint16_t& sid) {
    if (len < 16U || sec[0] != 0x00U) return false;
    const auto size = 3U + (((sec[1] & 0x0fU) << 8U) | sec[2]);
    if (size > len || size < 16U) return false;
    std::uint16_t only = 0;
    for (std::size_t offset = 8U; offset + 4U <= size - 4U; offset += 4U) {
        const auto id = be16(sec + offset);
        if (id == 0) continue;
        if (only != 0 && only != id) return false;
        only = id;
    }
    if (only == 0) return false;
    sid = only;
    return true;
}
} // namespace

DvbEitOutput::DvbEitOutput(Snapshot snapshot)
    : snapshot_(std::move(snapshot)) {}

void DvbEitOutput::inspectAndUpdateTables(Packet& packet) {
    if (packet[0] != 0x47U || (packet[1] & 0x40U) == 0U) return;
    const auto pid = static_cast<std::uint16_t>(((packet[1] & 31U) << 8U) | packet[2]);
    if (pid != 0x00U && pid != 0x11U) return;
    const auto control = (packet[3] >> 4U) & 3U;
    if ((control & 1U) == 0U) return;
    std::size_t p = 4U;
    if ((control & 2U) != 0U) {
        p = 5U + packet[4];
        if (p >= packet.size()) return;
    }
    const std::size_t pointer = packet[p++];
    if (p + pointer >= packet.size()) return;
    p += pointer;
    if (p + 15U > packet.size()) return;
    std::uint8_t* sec = packet.data() + p;
    const std::size_t available = packet.size() - p;
    const std::size_t length = 3U + (((sec[1] & 0x0fU) << 8U) | sec[2]);
    if (length > available || length < 16U) return;
    if (pid == 0U && sec[0] == 0x00U) {
        std::uint16_t single = 0;
        if (!singleServicePat(sec, available, single)) return;
        const auto transport = be16(sec + 3U);
        if (single != serviceId_ || transport != tsid_) {
            serviceId_ = single;
            tsid_ = transport;
            onid_ = 0U; // Wait for matching output SDT before publishing EIT.
            schedule_.clear();
            presentFollowing_.clear();
            nextRefresh_ = {};
        }
        return;
    }
    if (pid != 0x11U || sec[0] != 0x42U || tsid_ == 0U ||
        be16(sec + 3U) != tsid_) return;
    const auto network = be16(sec + 8U);
    if (network == 0U || serviceId_ == 0U) return;
    const std::size_t end = length - 4U;
    bool seen = false, edited = false;
    for (std::size_t offset = 11U; offset + 5U <= end;) {
        const auto sid = be16(sec + offset);
        const std::size_t descriptors = ((sec[offset + 3U] & 0x0fU) << 8U) |
            sec[offset + 4U];
        if (sid == serviceId_) {
            seen = true;
            if ((sec[offset + 2U] & 0x03U) != 0x03U) {
                sec[offset + 2U] |= 0x03U; // EIT_schedule + EIT_pf flags.
                edited = true;
            }
        }
        offset += 5U + descriptors;
    }
    if (!seen) return;
    if (edited) {
        const std::uint32_t crc = mpegCrc(sec, length - 4U);
        for (unsigned i = 0; i < 4; ++i)
            sec[length - 4U + i] =
                static_cast<std::uint8_t>(crc >> (24U - i * 8U));
    }
    if (onid_ != network) {
        onid_ = network;
        nextRefresh_ = {};
    }
}

void DvbEitOutput::enqueueSection(
    const std::vector<std::uint8_t>& section, std::deque<Packet>& queue) {
    if (section.empty()) return;
    std::size_t offset = 0;
    while (offset < section.size()) {
        Packet packet{};
        packet.fill(0xffU);
        packet[0] = 0x47U;
        packet[1] = static_cast<std::uint8_t>(
            (offset == 0U ? 0x40U : 0x00U) | ((kEitPid >> 8U) & 0x1fU));
        packet[2] = static_cast<std::uint8_t>(kEitPid);
        packet[3] = 0x10U; // Assigned at SEND time, across both EIT section streams.
        std::size_t pos = 4U;
        if (offset == 0U) packet[pos++] = 0U; // PSI pointer_field
        const std::size_t payload = std::min(packet.size() - pos, section.size() - offset);
        std::memcpy(packet.data() + pos, section.data() + offset, payload);
        queue.push_back(packet);
        offset += payload;
    }
}

void DvbEitOutput::buildSchedule() {
    nextRefresh_ = std::chrono::steady_clock::now() + kRefreshInterval;
    if (!serviceId_ || !tsid_ || !onid_ || !snapshot_) return;
    const auto root = snapshot_();
    if (!root.isMember("events") || !root["events"].isArray()) return;

    std::vector<Event> events;
    events.reserve(std::min<std::size_t>(root["events"].size(), kMaxScheduleEvents));
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    std::uint64_t fingerprint = 1469598103934665603ULL;
    const auto fingerprintByte = [&fingerprint](std::uint8_t v) {
        fingerprint = (fingerprint ^ v) * 1099511628211ULL;
    };
    for (const auto& source : root["events"]) {
        const std::int64_t start = source.get("start_utc", 0).asInt64();
        const std::int64_t stop = source.get("end_utc", 0).asInt64();
        if (start < now - 6 * 3600 || start > now + 7 * 86400 ||
            stop <= start || events.size() >= kMaxScheduleEvents)
            continue;
        Event event;
        event.start = start; event.stop = stop;
        event.id = source.get("event_id", 0).asUInt();
        if (event.id == 0U) event.id = static_cast<std::uint16_t>(
            ((start / 60) ^ (start >> 16)) & 0xffffU);
        event.descriptor = shortEvent(source);
        if (event.descriptor.empty()) continue;
        for (int shift : {0, 8, 16, 24, 32, 40, 48, 56})
            fingerprintByte(static_cast<std::uint8_t>(start >> shift));
        for (int shift : {0, 8, 16, 24, 32, 40, 48, 56})
            fingerprintByte(static_cast<std::uint8_t>(stop >> shift));
        for (std::uint8_t ch : event.descriptor) fingerprintByte(ch);
        events.push_back(std::move(event));
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.start < b.start;
    });
    if (fingerprint != lastFingerprint_) {
        version_ = static_cast<std::uint8_t>((version_ + 1U) & 31U);
        lastFingerprint_ = fingerprint;
    }
    schedule_.clear();
    presentFollowing_.clear();
    if (events.empty()) return;

    // EIT present/following actual (EN 300 468): section 0 carries the
    // present event, section 1 carries the following event. Do not combine
    // them in section 0: DVB receivers such as WISI require section numbering.
    const Event* current = nullptr;
    const Event* upcoming = nullptr;
    for (const auto& event : events) {
        if (!current && event.start <= now && now < event.stop) current = &event;
        if (!upcoming && event.start > now) upcoming = &event;
    }
    for (const auto sectionNumber : {0U, 1U}) {
        const Event* selected = sectionNumber == 0 ? current : upcoming;
        if (!selected) continue;
        const auto bytes = eventBytes(*selected, sectionNumber == 0U);
        if (bytes.empty()) continue;
        const auto sec = makeEit(0x4eU, serviceId_, tsid_, onid_, version_,
            static_cast<std::uint8_t>(sectionNumber),
            1U, 1U, 0x4eU, bytes);
        std::deque<Packet> temp;
        enqueueSection(sec, temp);
        presentFollowing_.insert(presentFollowing_.end(), temp.begin(), temp.end());
    }

    // Schedule actual: each section holds one event, with valid 8-section
    // segmentation and 0x50..0x5f table IDs. Delay emission across relay ticks
    // rather than creating a burst that would overflow the bounded CBR queue.
    const std::size_t count = events.size();
    const auto lastTable = static_cast<std::uint8_t>(0x50U + (count - 1U) / 256U);
    for (std::size_t i = 0; i < count; ++i) {
        const auto data = eventBytes(events[i]);
        if (data.empty()) continue;
        const auto table = static_cast<std::uint8_t>(0x50U + i / 256U);
        const auto section = static_cast<std::uint8_t>(i % 256U);
        const auto last = static_cast<std::uint8_t>(
            std::min<std::size_t>(255U, count - (i / 256U) * 256U - 1U));
        const auto segLast = static_cast<std::uint8_t>(
            std::min<std::size_t>(last, (section | 7U)));
        enqueueSection(makeEit(table, serviceId_, tsid_, onid_, version_,
                               section, last, segLast, lastTable, data),
                       schedule_);
    }
    nextPresentFollowing_ = {};
}

void DvbEitOutput::process(std::vector<Packet>& packets) {
    // Opt-in EIT owns PID 0x12. Avoid merging broadcaster and generated
    // continuity counters; all other PIDs, PCR and payload stay unchanged.
    packets.erase(std::remove_if(packets.begin(), packets.end(), [this](Packet& p) {
        if (p[0] != 0x47U) return false;
        const auto pid = static_cast<std::uint16_t>(((p[1] & 31U) << 8U) | p[2]);
        if (pid == kEitPid) return true;
        inspectAndUpdateTables(p);
        return false;
    }), packets.end());
    if (serviceId_ == 0U || tsid_ == 0U || onid_ == 0U) return;
    const auto now = std::chrono::steady_clock::now();
    if (nextRefresh_ == std::chrono::steady_clock::time_point{} || now >= nextRefresh_)
        buildSchedule();
    if (now < nextEmission_) return;
    nextEmission_ = now + kEmitInterval;
    if (!presentFollowing_.empty() &&
        (nextPresentFollowing_ == std::chrono::steady_clock::time_point{} ||
         now >= nextPresentFollowing_)) {
        for (Packet packet : presentFollowing_) {
            packet[3] = static_cast<std::uint8_t>(0x10U | (continuity_++ & 0x0fU));
            packets.push_back(std::move(packet));
        }
        nextPresentFollowing_ = now + kPfInterval;
    }
    for (std::size_t n = 0; n < kMaxPacketBurst && !schedule_.empty(); ++n) {
        Packet packet = schedule_.front();
        schedule_.pop_front();
        packet[3] = static_cast<std::uint8_t>(0x10U | (continuity_++ & 0x0fU));
        packets.push_back(std::move(packet));
    }
}
