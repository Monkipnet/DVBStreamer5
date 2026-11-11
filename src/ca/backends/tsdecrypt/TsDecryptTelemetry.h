#pragma once
// TSDecrypt V10.8.460 non-secret transport telemetry.
// A cleared scrambling bit is not proof of successful decryption.
// Only a plausible clear A/V PES start on an input-scrambled PID is evidence.
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace dvbstreamer5::ca::tsdecrypt_detail {
struct TsPacketFacts {
    uint16_t pid = 0x1fff;
    bool valid = false;
    bool scrambled = false;
    bool clearMediaPesStart = false;
    bool clearVideoPesStart = false;
    bool clearAudioPesStart = false;
};

inline TsPacketFacts inspectTsPacket(const uint8_t* p, size_t size) noexcept {
    TsPacketFacts facts;
    if (!p || size != 188 || p[0] != 0x47 || (p[1] & 0x80)) return facts;
    const uint8_t afc = static_cast<uint8_t>((p[3] >> 4) & 3);
    if (afc == 0) return facts;
    facts.valid = true;
    facts.pid = static_cast<uint16_t>(((p[1] & 0x1f) << 8) | p[2]);
    facts.scrambled = (p[3] & 0xc0) != 0;
    if (facts.scrambled || !(p[1] & 0x40) || afc == 2 ||
        facts.pid == 0x1fff) return facts;
    size_t start = 4;
    if (afc == 3) {
        const size_t adaptLen = p[4];
        start = 5 + adaptLen;
    }
    if (start + 4 > 188) return facts;
    const uint8_t id = p[start + 3];
    const bool validPesPrefix =
        p[start] == 0 && p[start + 1] == 0 && p[start + 2] == 1;
    facts.clearVideoPesStart = validPesPrefix && id >= 0xe0 && id <= 0xef;
    facts.clearAudioPesStart = validPesPrefix && id >= 0xc0 && id <= 0xdf;
    facts.clearMediaPesStart = facts.clearVideoPesStart ||
        facts.clearAudioPesStart || (validPesPrefix && id == 0xbd);
    return facts;
}

struct EcmReport {
    uint64_t received = 0;
    uint64_t duplicates = 0;
    uint64_t processed = 0;
    bool valid = false;
};
// Accept only the numeric summary TSDecrypt emits with -H. Never parse or
// forward arbitrary child stderr lines, which could contain sensitive data.
inline EcmReport parseEcmReport(const char* line) noexcept {
    EcmReport result;
    if (!line) return result;
    static constexpr char needle[] = "ECM | Received ";
    const char* start = line;
    while (*start) {
        const char* a = start;
        const char* b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) { start = a; break; }
        ++start;
    }
    if (!*start) return result;
    unsigned long long seen = 0, dup = 0, done = 0, seconds = 0;
    // Strict format and safe bounds; no ECM payload or CW material.
    if (std::sscanf(start, "%llu (%llu dup) and processed %llu in %llu seconds.",
                    &seen, &dup, &done, &seconds) == 4 &&
        seconds >= 1 && seconds <= 86400) {
        // Duplicates are counted per repeated ECM section, independently of
        // unique requests (seen); they can legitimately exceed seen.
        // Processed requests can cross a reporting interval boundary, too.
        result.received = static_cast<uint64_t>(seen);
        result.duplicates = static_cast<uint64_t>(dup);
        result.processed = static_cast<uint64_t>(done);
        result.valid = true;
    }
    return result;
}

inline bool hasSafeLogEvent(const char* line, const char* needle) noexcept {
    if (!line || !needle) return false;
    // This helper checks a fixed allowlisted marker, never copies log content.
    for (const char* p = line; *p; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

struct EcmPidReport {
    uint16_t pid = 0;
    bool valid = false;
    bool switched = false;
};

// Accept only the current ECM PID selection (not the list of alternative
// PIDs printed with "idx:"). Parse hexadecimal digits; never log any
// raw TSDecrypt stderr which might contain ECM/CW or credentials.
inline EcmPidReport parseEcmPidReport(const char* line) noexcept {
    EcmPidReport result;
    if (!line) return result;
    const char* start = std::strstr(line, "ECM | Switching ECM pid to 0x");
    if (start) {
        result.switched = true;
        start += sizeof("ECM | Switching ECM pid to 0x") - 1;
    } else {
        start = std::strstr(line, "--- | ECM pid : 0x");
        if (!start || std::strstr(line, "idx:")) return result;
        start += sizeof("--- | ECM pid : 0x") - 1;
    }
    uint32_t pid = 0;
    unsigned digits = 0;
    while (digits < 4) {
        const unsigned char ch = static_cast<unsigned char>(*start);
        int value = -1;
        if (ch >= '0' && ch <= '9') value = ch - '0';
        else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
        if (value < 0) break;
        pid = (pid << 4) | static_cast<unsigned>(value);
        ++digits;
        ++start;
    }
    const unsigned char next = static_cast<unsigned char>(*start);
    const bool moreHex = (next >= '0' && next <= '9') ||
        (next >= 'a' && next <= 'f') || (next >= 'A' && next <= 'F');
    if (digits == 0 || moreHex || pid == 0 || pid >= 0x1fff) return result;
    result.valid = true;
    result.pid = static_cast<uint16_t>(pid);
    return result;
}

inline bool isEcmPidHoldEvent(const char* line) noexcept {
    return hasSafeLogEvent(line,
        "ECM | Holding recently valid ECM pid after transient card denial.");
}

enum class CamdEvent : uint8_t {
    None = 0, Connecting, Connected, ConnectionError,
    ReconnectRequested, NoCodeWord, EcmQueueOverflow, ProtocolError,
    NewcamdReadError, NewcamdBadMsgId, NewcamdCardDenied,
    NewcamdChecksumError, NewcamdUnexpectedResponse, NewcamdLoginError
};

// Classify only fixed TSDecrypt log markers. The real line can include
// credentials, endpoints, ECM bytes or CW; NEVER copy it into telemetry.
inline CamdEvent classifyCamdEvent(const char* line) noexcept {
    if (!line) return CamdEvent::None;
    if (hasSafeLogEvent(line, "ERR | Error sending ecm packet, reconnecting to camd."))
        return CamdEvent::ReconnectRequested;
    if (hasSafeLogEvent(line, "ERR | No code word has been received (ret = -1)"))
        return CamdEvent::NoCodeWord;
    // Subtypes are intentionally checked before the aggregate fallback.
    // No protocol error text is forwarded; it may contain sensitive data.
    if (hasSafeLogEvent(line, "ERR | [newcamd] Failed to read message."))
        return CamdEvent::NewcamdReadError;
    if (hasSafeLogEvent(line, "ERR | [newcamd] Bad msg_id"))
        return CamdEvent::NewcamdBadMsgId;
    if (hasSafeLogEvent(line, "ERR | [newcamd] Card was not able to decode the channel."))
        return CamdEvent::NewcamdCardDenied;
    if (hasSafeLogEvent(line, "ERR | [newcamd] Checksum error."))
        return CamdEvent::NewcamdChecksumError;
    if (hasSafeLogEvent(line, "ERR | [newcamd] Unexpected CAMD server response"))
        return CamdEvent::NewcamdUnexpectedResponse;
    if (hasSafeLogEvent(line, "ERR | [newcamd] Login failed."))
        return CamdEvent::NewcamdLoginError;
    if (hasSafeLogEvent(line, "ERR | [newcamd]"))
        return CamdEvent::ProtocolError;
    if (hasSafeLogEvent(line, "in ECM queue, dropping the oldest."))
        return CamdEvent::EcmQueueOverflow;
    if (hasSafeLogEvent(line, "CAM | Error connecting to server ") ||
        hasSafeLogEvent(line, "CAM | Could not create socket:") ||
        hasSafeLogEvent(line, "CAM | ERROR: getaddrinfo("))
        return CamdEvent::ConnectionError;
    if (hasSafeLogEvent(line, "CAM | Connected to server "))
        return CamdEvent::Connected;
    if (hasSafeLogEvent(line, "CAM | Connecting to server "))
        return CamdEvent::Connecting;
    return CamdEvent::None;
}

} // namespace dvbstreamer5::ca::tsdecrypt_detail
