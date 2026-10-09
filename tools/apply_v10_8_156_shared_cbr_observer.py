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
    'inline constexpr const char* kProgramVersion = "10.8.155";',
    'inline constexpr const char* kProgramVersion = "10.8.156";',
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "        outputs.push_back(std::move(output));\n        ++seed;\n    }\n\n    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.",
    "        outputs.push_back(std::move(output));\n        ++seed;\n    }\n\n"
    "    // V10.8.156: when a native UDP-CBR output already exists, reuse its\n"
    "    // paced datagrams for observeTransport instead of running a second\n"
    "    // independent CBR pacer/thread for the same stream. This keeps public\n"
    "    // HTTP/SRT/HLS/RTSP/RTMP/statistics on the exact emitted CBR timeline\n"
    "    // and removes one high-frequency pacing thread per UDP-CBR channel.\n"
    "    std::size_t observedCbrOutputIndex = outputs.size();\n"
    "    if (config_.paceObservedTransport && config_.observeTransport) {\n"
    "        for (std::size_t index = 0; index < outputs.size(); ++index) {\n"
    "            if (outputs[index].cbrPacer) {\n"
    "                observedCbrOutputIndex = index;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    const bool shareObservedCbrWithUdpOutput =\n"
    "        observedCbrOutputIndex < outputs.size();\n"
    "    if (shareObservedCbrWithUdpOutput) {\n"
    "        std::cerr << \"NATIVE OBSERVED CBR share output_index=\"\n"
    "                  << observedCbrOutputIndex\n"
    "                  << \" target_kbps=\" << (config_.targetBitrate / 1000ULL)\n"
    "                  << std::endl;\n"
    "    }\n\n"
    "    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "    if (config_.paceObservedTransport && config_.targetBitrate > 0) {",
    "    if (config_.paceObservedTransport && config_.targetBitrate > 0 &&\n"
    "        !shareObservedCbrWithUdpOutput) {",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "        if (!config_.observeTransport || observedPackets.empty()) return true;\n\n        if (observedCbrPacer) {",
    "        if (!config_.observeTransport || observedPackets.empty()) return true;\n\n"
    "        // The first UDP-CBR pacer will publish its already-shaped 1316-byte\n"
    "        // datagrams below. Do not enqueue the same TS into a second pacer.\n"
    "        if (shareObservedCbrWithUdpOutput) return true;\n\n"
    "        if (observedCbrPacer) {",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "                if (!sendCbrDatagram(\n                        cbrDatagram, *output.socket, outputBytes_, error)) {\n                    std::lock_guard<std::mutex> lock(errorMutex_);\n                    lastError_ = error.empty()\n                        ? \"UDP CBR output send failed\" : error;\n                    break;\n                }\n                ++output.datagramsSent;",
    "                if (!sendCbrDatagram(\n                        cbrDatagram, *output.socket, outputBytes_, error)) {\n                    std::lock_guard<std::mutex> lock(errorMutex_);\n                    lastError_ = error.empty()\n                        ? \"UDP CBR output send failed\" : error;\n                    break;\n                }\n                if (shareObservedCbrWithUdpOutput &&\n                    output.index == observedCbrOutputIndex && config_.observeTransport) {\n                    config_.observeTransport(\n                        reinterpret_cast<const std::uint8_t*>(cbrDatagram.data()),\n                        sizeof(cbrDatagram));\n                }\n                ++output.datagramsSent;",
)

print("V10.8.156 patch applied")
