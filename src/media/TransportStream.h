#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dvbstreamer5::media::mpegts {

inline constexpr std::size_t kPacketSize = 188;
inline constexpr std::uint8_t kSyncByte = 0x47;
inline constexpr std::uint16_t kNullPid = 0x1fff;
using Packet = std::array<std::uint8_t, kPacketSize>;

struct PacketInfo {
    std::uint16_t pid = 0;
    std::uint8_t continuityCounter = 0;
    std::uint8_t adaptationFieldControl = 0;
    std::size_t payloadOffset = kPacketSize;
    bool transportError = false;
    bool payloadUnitStart = false;
    bool scrambled = false;
    bool hasAdaptationField = false;
    bool hasPayload = false;
    bool discontinuity = false;
    bool randomAccess = false;
    bool hasPcr = false;
    std::uint64_t pcrBase90k = 0;
};

bool inspectPacket(const std::uint8_t* data, std::size_t size, PacketInfo& info) noexcept;
bool rewritePid(std::uint8_t* data, std::size_t size, std::uint16_t pid) noexcept;

class PacketFramer {
public:
    void push(const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);
    // Fast path for trusted Linux DVB TS-tap input. It verifies the 188-byte
    // sync grid but intentionally defers full packet-header validation to the
    // selected-service remapper. Any alignment break falls back to push().
    void pushTrustedAligned(
        const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);

    // V10.8.129: zero-copy visitor for trusted Linux DVB TS-tap input.
    // The common aligned path invokes visitor directly on the source packet;
    // partial/misaligned/corrupt input falls back to push() and preserves the
    // exact legacy byte-resync and structural-validation behavior.
    using TrustedPacketVisitor = bool (*)(void*, const std::uint8_t*);
    bool visitTrustedAligned(
        const std::uint8_t* data, std::size_t size,
        void* context, TrustedPacketVisitor visitor);
    void reset() noexcept;

private:
    std::vector<std::uint8_t> pending_;
};

enum class ContinuityStatus {
    InOrder,
    Gap,
    Duplicate,
    Discontinuity,
    NullPacket,
    TransportError,
    InvalidPacket
};

class ContinuityTracker {
public:
    ContinuityStatus observe(const std::uint8_t* data, std::size_t size) noexcept;
    void reset() noexcept;

private:
    struct State {
        std::uint8_t counter = 0;
        bool initialized = false;
        bool previousHadPayload = false;
    };

    std::array<State, 8192> states_ {};
};

} // namespace dvbstreamer5::media::mpegts
