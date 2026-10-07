from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.128";',
    'inline constexpr const char* kProgramVersion = "10.8.129";',
)

replace_once(
    "src/media/TransportStream.h",
    '''    void pushTrustedAligned(\n        const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);\n    void reset() noexcept;''',
    '''    void pushTrustedAligned(\n        const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);\n\n    // V10.8.129: zero-copy visitor for trusted Linux DVB TS-tap input.\n    // The common aligned path invokes visitor directly on the source packet;\n    // partial/misaligned/corrupt input falls back to push() and preserves the\n    // exact legacy byte-resync and structural-validation behavior.\n    using TrustedPacketVisitor = bool (*)(void*, const std::uint8_t*);\n    bool visitTrustedAligned(\n        const std::uint8_t* data, std::size_t size,\n        void* context, TrustedPacketVisitor visitor);\n    void reset() noexcept;''',
)

anchor = '''void PacketFramer::reset() noexcept {\n    pending_.clear();\n}\n'''
visitor_impl = r'''bool PacketFramer::visitTrustedAligned(
    const std::uint8_t* data, std::size_t size,
    void* context, TrustedPacketVisitor visitor) {
    if (!data || size == 0 || !visitor) return true;

    auto visitFramed = [&](const std::uint8_t* bytes, std::size_t bytesSize) {
        std::vector<Packet> fallbackPackets;
        push(bytes, bytesSize, fallbackPackets);
        for (const auto& packet : fallbackPackets) {
            if (!visitor(context, packet.data())) return false;
        }
        return true;
    };

    if (!pending_.empty()) {
        return visitFramed(data, size);
    }

    std::size_t direct = 0;
    while (size - direct >= kPacketSize) {
        if (data[direct] != kSyncByte) break;
        const std::size_t remaining = size - direct;
        if (remaining >= 2 * kPacketSize &&
            data[direct + kPacketSize] != kSyncByte) {
            break;
        }
        if (remaining >= 3 * kPacketSize &&
            data[direct + 2 * kPacketSize] != kSyncByte) {
            break;
        }

        if (!visitor(context, data + direct)) return false;
        direct += kPacketSize;
    }

    if (direct == size) return true;
    if (direct > 0) {
        data += direct;
        size -= direct;
    }

    return visitFramed(data, size);
}

'''
replace_once(
    "src/media/TransportStream.cpp",
    anchor,
    visitor_impl + anchor,
)

replace_once(
    "src/StreamManager.cpp",
    '''        dropSourceNullPackets_ = dropSourceNullPackets;\n        inputPackets_.reserve(384);\n        filteredPackets_.reserve(128);''',
    '''        dropSourceNullPackets_ = dropSourceNullPackets;\n        filteredPackets_.reserve(128);''',
)

old_push = r'''        inputPackets_.clear();
        // V10.8.125: bytes originate from the shared Linux DVB TS tap. Avoid
        // full inspectPacket() work for every foreign-service MPTS packet; the
        // selected packets are validated below and again by Remapper::process().
        framer_.pushTrustedAligned(data, size, inputPackets_);
        filteredPackets_.clear();

        // V10.8.118: keep the V10.8.117 filtering semantics but avoid doing a
        // full inspectPacket() before Remapper::process() for every non-null
        // packet. For the NULL fast path the three-byte TS header is enough;
        // malformed packets still reach Remapper and retain its validation.
        std::string remapError;
        for (const auto& packet : inputPackets_) {
            if (packet[0] == dvbstreamer5::media::mpegts::kSyncByte) {
                const std::uint16_t pid = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) |
                    packet[2]);
                if (dropSourceNullPackets_ &&
                    pid == dvbstreamer5::media::mpegts::kNullPid) {
                    continue;
                }

                // V10.8.120: once PAT/PMT state is known, reject PIDs belonging
                // to other services before inspectPacket(), PSI assembly and the
                // per-packet steady-clock check inside Remapper::process().
                if (!remapper_.wantsInputPid(pid)) continue;
            }

            // The trusted framer intentionally skipped full header validation.
            // Preserve the old behavior for malformed selected-service packets:
            // drop them here instead of turning a recoverable damaged TS packet
            // into a fatal remapper error. Foreign-service packets never pay this
            // parsing cost because wantsInputPid() rejected them above.
            dvbstreamer5::media::mpegts::PacketInfo selectedInfo;
            if (!dvbstreamer5::media::mpegts::inspectPacket(
                    packet.data(), packet.size(), selectedInfo)) {
                continue;
            }

            // Remapper appends to the supplied packet vector. Accumulate the
            // whole selected-service block directly instead of creating a
            // temporary vector for each input packet and copying every 188-byte
            // packet again into a byte vector.
            if (!remapper_.process(packet, filteredPackets_, remapError)) {
                const std::string message = remapError.empty()
                    ? "shared DVB service prefilter failed"
                    : "shared DVB service prefilter failed: " + remapError;
                std::cerr << "SHARED DVB service prefilter failed stream="
                          << streamId_ << " error=" << message << std::endl;
                relay->finishInput(message);
                return false;
            }
        }
'''
new_push = r'''        filteredPackets_.clear();
        std::string remapError;
        VisitContext visitContext {this, relay, &remapError};

        // V10.8.129: process trusted aligned DVB packets in source order without
        // first copying the whole MPTS into inputPackets_. This preserves the
        // dynamic PAT -> PMT -> ES admission sequence inside a single input
        // chunk: wantsInputPid() observes remapper state updated by every prior
        // admitted packet. Only packets belonging to the selected service are
        // copied into a Packet for full structural validation/remapping.
        if (!framer_.visitTrustedAligned(
                data, size, &visitContext, &SharedDvbServicePrefilter::visitPacket)) {
            return false;
        }
'''
replace_once("src/StreamManager.cpp", old_push, new_push)

old_private = r'''private:
    std::string streamId_;
    bool dropSourceNullPackets_ = false;
    dvbstreamer5::media::mpegts::PacketFramer framer_;
    dvbstreamer5::media::mpegts::Remapper remapper_;
    std::vector<dvbstreamer5::media::mpegts::Packet> inputPackets_;
    std::vector<dvbstreamer5::media::mpegts::Packet> filteredPackets_;
};'''
new_private = r'''private:
    struct VisitContext {
        SharedDvbServicePrefilter* self = nullptr;
        dvbstreamer5::media::network::NativeUdpRelay* relay = nullptr;
        std::string* remapError = nullptr;
    };

    static bool visitPacket(void* opaque, const std::uint8_t* bytes) {
        auto* context = static_cast<VisitContext*>(opaque);
        if (!context || !context->self || !context->relay ||
            !context->remapError || !bytes) {
            return false;
        }

        auto* self = context->self;
        if (bytes[0] == dvbstreamer5::media::mpegts::kSyncByte) {
            const std::uint16_t pid = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[1] & 0x1fU) << 8) |
                bytes[2]);
            if (self->dropSourceNullPackets_ &&
                pid == dvbstreamer5::media::mpegts::kNullPid) {
                return true;
            }
            if (!self->remapper_.wantsInputPid(pid)) return true;
        }

        dvbstreamer5::media::mpegts::Packet packet {};
        std::memcpy(packet.data(), bytes, packet.size());

        dvbstreamer5::media::mpegts::PacketInfo selectedInfo;
        if (!dvbstreamer5::media::mpegts::inspectPacket(
                packet.data(), packet.size(), selectedInfo)) {
            return true;
        }

        if (!self->remapper_.process(
                packet, self->filteredPackets_, *context->remapError)) {
            const std::string message = context->remapError->empty()
                ? "shared DVB service prefilter failed"
                : "shared DVB service prefilter failed: " + *context->remapError;
            std::cerr << "SHARED DVB service prefilter failed stream="
                      << self->streamId_ << " error=" << message << std::endl;
            context->relay->finishInput(message);
            return false;
        }
        return true;
    }

    std::string streamId_;
    bool dropSourceNullPackets_ = false;
    dvbstreamer5::media::mpegts::PacketFramer framer_;
    dvbstreamer5::media::mpegts::Remapper remapper_;
    std::vector<dvbstreamer5::media::mpegts::Packet> filteredPackets_;
};'''
replace_once("src/StreamManager.cpp", old_private, new_private)
