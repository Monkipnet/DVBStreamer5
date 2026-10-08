from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.136";',
    'inline constexpr const char* kProgramVersion = "10.8.137";',
)

replace_once(
    "src/media/NativeUdpRelay.h",
    "    bool externallyFedInput = false;\n    bool dvbInputSource = false;",
    "    bool externallyFedInput = false;\n    // V10.8.137: set only when an external producer already emits contiguous,\n    // structurally validated 188-byte MPEG-TS packets (shared-DVB prefilter).\n    // Other external inputs retain the legacy full PacketFramer validation.\n    bool trustedAlignedExternalInput = false;\n    bool dvbInputSource = false;",
)

replace_once(
    "src/StreamManager.cpp",
    "        relay.remapEnabled = false;\n        std::cerr << \"SHARED DVB service prefilter enabled stream=\"",
    "        relay.remapEnabled = false;\n        // V10.8.137: SharedDvbServicePrefilter emits a contiguous Packet[188]\n        // array after selected-packet structural validation/remapping. The relay\n        // may therefore use the trusted aligned framer fast path; a sync-grid\n        // break still falls back to the legacy byte-resync/full-validation path.\n        relay.trustedAlignedExternalInput = true;\n        std::cerr << \"SHARED DVB service prefilter enabled stream=\"",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "                inputBytes_.fetch_add(received, std::memory_order_relaxed);\n                inputFramer.push(datagram.data(), received, packets);",
    "                inputBytes_.fetch_add(received, std::memory_order_relaxed);\n                if (config_.trustedAlignedExternalInput) {\n                    inputFramer.pushTrustedAligned(datagram.data(), received, packets);\n                } else {\n                    inputFramer.push(datagram.data(), received, packets);\n                }",
)
