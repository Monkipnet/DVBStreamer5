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
    'inline constexpr const char* kProgramVersion = "10.8.116";',
    'inline constexpr const char* kProgramVersion = "10.8.117";',
)

replace_once(
    "src/StreamManager.cpp",
    '#include "media/NativeSampleAes.h"\n#include "media/NativeTsDemux.h"\n',
    '#include "media/NativeSampleAes.h"\n#include "media/MpegTsRemapper.h"\n#include "media/NativeTsDemux.h"\n',
)

helper = r'''
class SharedDvbServicePrefilter {
public:
    bool initialize(
        const std::string& streamId,
        const dvbstreamer5::media::mpegts::RemapConfig& config,
        bool dropSourceNullPackets,
        std::string& error) {
        streamId_ = streamId;
        dropSourceNullPackets_ = dropSourceNullPackets;
        inputPackets_.reserve(384);
        remappedPackets_.reserve(64);
        outputBytes_.reserve(64 * 1024);
        return remapper_.initialize(config, error);
    }

    bool push(
        dvbstreamer5::media::network::NativeUdpRelay* relay,
        const std::uint8_t* data,
        std::size_t size) {
        if (!relay || !data || size == 0) return false;

        inputPackets_.clear();
        framer_.push(data, size, inputPackets_);
        outputBytes_.clear();

        for (const auto& packet : inputPackets_) {
            if (dropSourceNullPackets_) {
                dvbstreamer5::media::mpegts::PacketInfo info;
                if (dvbstreamer5::media::mpegts::inspectPacket(
                        packet.data(), packet.size(), info) &&
                    info.pid == dvbstreamer5::media::mpegts::kNullPid) {
                    continue;
                }
            }

            remappedPackets_.clear();
            std::string remapError;
            if (!remapper_.process(packet, remappedPackets_, remapError)) {
                const std::string message = remapError.empty()
                    ? "shared DVB service prefilter failed"
                    : "shared DVB service prefilter failed: " + remapError;
                std::cerr << "SHARED DVB service prefilter failed stream="
                          << streamId_ << " error=" << message << std::endl;
                relay->finishInput(message);
                return false;
            }

            for (const auto& filtered : remappedPackets_) {
                outputBytes_.insert(
                    outputBytes_.end(), filtered.begin(), filtered.end());
            }
        }

        if (outputBytes_.empty()) return true;
        return relay->pushInput(outputBytes_.data(), outputBytes_.size());
    }

private:
    std::string streamId_;
    bool dropSourceNullPackets_ = false;
    dvbstreamer5::media::mpegts::PacketFramer framer_;
    dvbstreamer5::media::mpegts::Remapper remapper_;
    std::vector<dvbstreamer5::media::mpegts::Packet> inputPackets_;
    std::vector<dvbstreamer5::media::mpegts::Packet> remappedPackets_;
    std::vector<std::uint8_t> outputBytes_;
};

'''

replace_once(
    "src/StreamManager.cpp",
    '    bool resolvedLogged_ = false;\n};\n\nstruct AbrProfile {\n',
    '    bool resolvedLogged_ = false;\n};\n\n' + helper + 'struct AbrProfile {\n',
)

replace_once(
    "src/StreamManager.cpp",
    '''        // The shared reader always takes the complete multiplex.  Per-channel
        // PAT/PMT/remap/CA filtering remains independent in each NativeUdpRelay.
        relay.dvbTuneConfig.pids = "8192";
''',
    '''        // The shared reader still owns one full-MPTS demux for the frontend.
        // V10.8.117 moves only per-service PAT/PMT filtering ahead of the relay
        // queue, so tuner ownership/tuning and the shared bus remain unchanged.
        relay.dvbTuneConfig.pids = "8192";
''',
)

anchor = '''    if (!streamConfig.conditionalAccessClient.empty()) {
'''
insert = '''    std::shared_ptr<SharedDvbServicePrefilter> sharedDvbPrefilter;
    if (sharedDvbInputSource && relay.remapEnabled) {
        sharedDvbPrefilter = std::make_shared<SharedDvbServicePrefilter>();
        const bool dropSourceNullPackets =
            streamConfig.cbr && streamConfig.targetBitrate > 0;
        std::string prefilterError;
        if (!sharedDvbPrefilter->initialize(
                streamConfig.id,
                relay.remapConfig,
                dropSourceNullPackets,
                prefilterError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) {
                *error = prefilterError.empty()
                    ? "shared DVB service prefilter initialization failed"
                    : prefilterError;
            }
            return false;
        }

        // The same proven Remapper now runs in the subscriber thread before
        // NativeUdpRelay's bounded external-input queue.  Do not remap twice.
        relay.remapEnabled = false;
        std::cerr << "SHARED DVB service prefilter enabled stream="
                  << streamConfig.id
                  << " sid=" << streamConfig.inputServiceId
                  << " drop_source_null=" << (dropSourceNullPackets ? 1 : 0)
                  << std::endl;
    }

'''
replace_once(
    "src/StreamManager.cpp",
    anchor,
    insert + anchor,
)

replace_once(
    "src/StreamManager.cpp",
    '''                    [relayPtr](const std::uint8_t* data, std::size_t size) {
                        return relayPtr->pushInput(data, size);
                    },
''',
    '''                    [relayPtr, sharedDvbPrefilter](
                        const std::uint8_t* data, std::size_t size) {
                        if (sharedDvbPrefilter) {
                            return sharedDvbPrefilter->push(relayPtr, data, size);
                        }
                        return relayPtr->pushInput(data, size);
                    },
''',
)

print("V10.8.117 shared DVB prefilter applied")
