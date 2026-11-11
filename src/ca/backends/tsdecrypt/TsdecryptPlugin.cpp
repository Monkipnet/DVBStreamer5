#pragma once
// TSDecrypt V10.8.460 non-secret transport telemetry.
// A cleared scrambling bit is not proof of successful decryption.
// Only a plausible clear A/V PES start on an input-scrambled PID is evidence.
#include <cstddef>
#include <cstdio>
#include <cstdint>

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
