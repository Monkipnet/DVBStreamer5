from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 occurrence, got {count}")
    return text.replace(old, new, 1)


p = Path("src/AppVersion.h")
s = p.read_text()
s = replace_once(
    s,
    'kProgramVersion = "10.8.108"',
    'kProgramVersion = "10.8.109"',
    "version",
)
p.write_text(s)

p = Path("src/HttpServer.cpp")
s = p.read_text()
s = replace_once(
    s,
    '#include "OscamMiniManager.h"\n',
    '#include "OscamMiniManager.h"\n#include "protocols/SrtVpsProfile.h"\n',
    "HttpServer profile include",
)
s = replace_once(
    s,
    '        "&latency=" + std::to_string(std::clamp(cfg.srtOutputLatencyMs, 20, 60000));',
    '        "&latency=" + std::to_string(dvbstreamer5::protocols::srt_vps::latencyMs(\n'
    '            cfg, std::clamp(cfg.srtOutputLatencyMs, 20, 60000)));',
    "SRT generated-link latency",
)
p.write_text(s)

p = Path("src/StreamManager.cpp")
s = p.read_text()
old_out = '''        srtConfig.latencyMs = std::clamp(spec.latency, 20, 60000);
        srtConfig.passphrase = spec.passphrase;
        srtConfig.streamId = spec.streamId;
        srtConfig.pbkeylen = spec.pbkeylen;
        srtConfig.bindAddress = cleanInterface(spec.iface);
        if (srtConfig.mode == "listener") srtConfig.host = "0.0.0.0";'''
new_out = '''        srtConfig.latencyMs = std::clamp(spec.latency, 20, 60000);
        srtConfig.passphrase = spec.passphrase;
        srtConfig.streamId = spec.streamId;
        srtConfig.pbkeylen = spec.pbkeylen;
        srtConfig.bindAddress = cleanInterface(spec.iface);
        // Apply VPS/VDS tuning last so the normal per-output latency cannot
        // overwrite the optimization profile after URI parsing.
        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);
        if (streamConfig.srtVpsVdsOptimization) {
            std::cerr << "SRT VPS/VDS effective OUT stream=" << streamConfig.id
                      << " mode=" << srtConfig.mode
                      << " latency_ms=" << srtConfig.latencyMs
                      << " rcvlatency_ms=" << srtConfig.receiveLatencyMs
                      << " peerlatency_ms=" << srtConfig.peerLatencyMs
                      << " rcvbuf=" << srtConfig.receiveBufferBytes
                      << " sndbuf=" << srtConfig.sendBufferBytes
                      << " fc=" << srtConfig.flightWindowPackets
                      << " payload=" << srtConfig.payloadSize
                      << " io_timeout_ms=" << srtConfig.ioTimeoutMs
                      << std::endl;
        }
        if (srtConfig.mode == "listener") srtConfig.host = "0.0.0.0";'''
s = replace_once(s, old_out, new_out, "SRT OUT final profile")

old_in = '''        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);
        if (!state->nativeSrtInput->start('''
new_in = '''        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);
        if (streamConfig.srtVpsVdsOptimization) {
            std::cerr << "SRT VPS/VDS effective IN stream=" << streamConfig.id
                      << " mode=" << srtConfig.mode
                      << " latency_ms=" << srtConfig.latencyMs
                      << " rcvlatency_ms=" << srtConfig.receiveLatencyMs
                      << " peerlatency_ms=" << srtConfig.peerLatencyMs
                      << " rcvbuf=" << srtConfig.receiveBufferBytes
                      << " sndbuf=" << srtConfig.sendBufferBytes
                      << " fc=" << srtConfig.flightWindowPackets
                      << " payload=" << srtConfig.payloadSize
                      << " io_timeout_ms=" << srtConfig.ioTimeoutMs
                      << std::endl;
        }
        if (!state->nativeSrtInput->start('''
s = replace_once(s, old_in, new_in, "SRT IN effective log")
p.write_text(s)
