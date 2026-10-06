from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def regex_replace_once(text: str, pattern: str, replacement: str, label: str) -> str:
    updated, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return updated


# Version bump.
p = Path("src/AppVersion.h")
s = p.read_text()
s = replace_once(s, 'kProgramVersion = "10.8.111"', 'kProgramVersion = "10.8.112"', "version")
p.write_text(s)

# Remove obsolete manual provider-PCR settings from the persistent runtime model.
p = Path("src/ConfigManager.h")
s = p.read_text()
s = regex_replace_once(
    s,
    r'    // Manual per-stream provider-PCR rate clock \(203\.36\)\. Existing streams default off\.\n'
    r'    bool hlsSlowPcrAssist = false;\n'
    r'    // 203\.40: manual HLS pre-buffered provider-PCR interval pacing\. The existing\n'
    r'    // JSON key is retained for compatibility, but the mode is now feed-forward:\n'
    r'    // adjacent PCR anchors are known before packets enter the token sender\. Off by default\.\n'
    r'    bool hlsPcrPhasePacing = false;\n',
    '',
    "obsolete HLS PCR config fields")
# Module overview: config is the durable schema boundary; unknown legacy JSON keys are ignored.
s = replace_once(
    s,
    '#include <vector>\n\n',
    '#include <vector>\n\n'
    '// Configuration model and JSON persistence boundary. Keep runtime-only state out of\n'
    '// these structures; fromJson() deliberately tolerates unknown legacy keys so old\n'
    '// configuration files can be upgraded without carrying dead options forward.\n\n',
    "ConfigManager.h overview")
p.write_text(s)

p = Path("src/ConfigManager.cpp")
s = p.read_text()
s = regex_replace_once(
    s,
    r'    config\.hlsSlowPcrAssist = root\.get\("hls_slow_pcr_assist", false\)\.asBool\(\);\n'
    r'    config\.hlsPcrPhasePacing = root\.get\("hls_pcr_phase_pacing", false\)\.asBool\(\);\n'
    r'    // The two manual HLS timing modes are mutually exclusive\. Pre-buffered PCR\n'
    r'    // interval pacing wins if an old API client accidentally submits both flags\.\n'
    r'    if \(config\.hlsPcrPhasePacing\) config\.hlsSlowPcrAssist = false;\n',
    '',
    "obsolete HLS PCR JSON input")
s = replace_once(s, '    root["hls_slow_pcr_assist"] = hlsSlowPcrAssist;\n', '', "obsolete slow PCR JSON output")
s = replace_once(s, '    root["hls_pcr_phase_pacing"] = hlsPcrPhasePacing;\n', '', "obsolete phase pacing JSON output")
s = replace_once(
    s,
    '#include <openssl/rand.h>\n\n\nnamespace {',
    '#include <openssl/rand.h>\n\n'
    '// Persistent configuration handling: normalize endpoints on input, validate bounded\n'
    '// numeric fields, protect UI credentials, and serialize only options consumed by the\n'
    '// current runtime. Legacy/unknown JSON members are intentionally ignored.\n\n'
    'namespace {',
    "ConfigManager.cpp overview")
p.write_text(s)

# Remove the two obsolete PCR controls and simplify the input-mode UI handler.
p = Path("src/HttpServer.cpp")
s = p.read_text()
s = regex_replace_once(
    s,
    r"\n  \['HLS синхронизация', 'HLS synchronization'\],.*?(?=\n  \['Резерв / файл замены', 'Backup / replacement file'\],)",
    '',
    "obsolete PCR UI translations")
s = replace_once(
    s,
    "hls_user_agent:'Mozilla/5.0 DVBStreamer5', hls_slow_pcr_assist:false, hls_pcr_phase_pacing:false, conditional_access_client:''",
    "hls_user_agent:'Mozilla/5.0 DVBStreamer5', conditional_access_client:''",
    "new-stream obsolete PCR defaults")
s = regex_replace_once(
    s,
    r'\n        <div class=\\"form-row full\\" id=\\"streamHlsSynchronizationRow\\".*?Не включать вместе с Provider PCR clock\.</small></div>',
    '',
    "obsolete PCR controls row")
s = replace_once(
    s,
    "    hls_slow_pcr_assist: selectedInputMode === 'hls' && document.getElementById('streamHlsSlowPcrAssist')?.checked === true,\n",
    '',
    "PCR save field 1")
s = replace_once(
    s,
    "    hls_pcr_phase_pacing: selectedInputMode === 'hls' && document.getElementById('streamHlsPcrPhasePacing')?.checked === true,\n",
    '',
    "PCR save field 2")
s = regex_replace_once(
    s,
    r"function updateHlsSynchronizationVisibility\(\) \{\n"
    r"  const mode = document\.getElementById\('streamInputMode'\);\n"
    r"  const row = document\.getElementById\('streamHlsSynchronizationRow'\);\n"
    r"  const isHls = mode\?\.value === 'hls';\n"
    r"  if \(row\) row\.style\.display = isHls \? '' : 'none';\n"
    r"  const providerClock = document\.getElementById\('streamHlsSlowPcrAssist'\);\n"
    r"  const deadlineShaper = document\.getElementById\('streamHlsPcrPhasePacing'\);\n"
    r"  if \(providerClock\) providerClock\.disabled = !isHls;\n"
    r"  if \(deadlineShaper\) deadlineShaper\.disabled = !isHls;\n",
    "function updateInputModeVisibility() {\n  const mode = document.getElementById('streamInputMode');\n",
    "input mode UI simplification")
s = s.replace('updateHlsSynchronizationVisibility()', 'updateInputModeVisibility()')
s = replace_once(
    s,
    '#if defined(__GLIBC__)\n#include <malloc.h>\n#endif\n\nnamespace {',
    '#if defined(__GLIBC__)\n#include <malloc.h>\n#endif\n\n'
    '// HTTP control plane and embedded web UI. This module translates API requests into\n'
    '// validated StreamConfig changes and publishes runtime state; media packet processing\n'
    '// remains in StreamManager/media modules so UI code cannot become a transport path.\n\n'
    'namespace {',
    "HttpServer overview")
p.write_text(s)

# Add concise architecture comments to the primary runtime modules. No media behavior changes.
module_comments = {
    "src/main.cpp": (
        '#include "AppVersion.h"\n\n',
        '#include "AppVersion.h"\n\n'
        '// Process composition: load durable configuration, apply fail-open host networking,\n'
        '// initialize CA/notifications/stream orchestration, start the HTTP control plane,\n'
        '// auto-start eligible channels, then keep the shared Asio event loop alive.\n\n'),
    "src/StreamManager.cpp": (
        '#include <unistd.h>\n\nnamespace {',
        '#include <unistd.h>\n\n'
        '// Stream orchestration layer. It owns per-channel runtime state, selects native input\n'
        '// and output transports, wires CA/remap/transcode/HLS observers, and contains failures\n'
        '// to one stream. Protocol/socket details stay in src/media and src/protocols.\n\n'
        'namespace {'),
    "src/media/NativeHlsInput.cpp": (
        '#include <vector>\n\nnamespace {',
        '#include <vector>\n\n'
        '// Native HLS acquisition: resolve playlists, select/prefetch bounded segments, handle\n'
        '// encryption, and feed MPEG-TS/CMAF media downstream. Output-rate shaping is not done\n'
        '// here; the current runtime uses the common CBR/output transport path instead.\n\n'
        'namespace {'),
    "src/media/NativeUdpRelay.cpp": (
        '#endif\n\nnamespace dvbstreamer5::media::network {',
        '#endif\n\n'
        '// Native transport relay: acquire UDP/RTP/HTTP/file/DVB/external input, apply the\n'
        '// configured remap/CA/transcode callbacks once, then fan out the processed MPEG-TS to\n'
        '// UDP/RTP sockets and observers used by SRT/HTTP/HLS/RTSP/RTMP.\n\n'
        'namespace dvbstreamer5::media::network {'),
    "src/media/CbrTsPacer.cpp": (
        '#include <stdexcept>\n\nnamespace dvbstreamer5::media::mpegts {',
        '#include <stdexcept>\n\n'
        '// Fixed-rate MPEG-TS sender clock. It emits seven-packet datagrams on a monotonic\n'
        '// deadline and fills unused capacity with NULL packets. It deliberately has no\n'
        '// provider-PCR feedback/PLL; PCR generation/rewriting belongs to the mux layer.\n\n'
        'namespace dvbstreamer5::media::mpegts {'),
}
for filename, (old, new) in module_comments.items():
    path = Path(filename)
    text = path.read_text()
    text = replace_once(text, old, new, f"module comment {filename}")
    path.write_text(text)

# Prevent ad-hoc historical stage notes from accumulating again.
p = Path('.gitignore')
s = p.read_text()
if '*STAGE*.md\n' not in s:
    s += '*STAGE*.md\n'
p.write_text(s)

# Remove all historical stage Markdown notes (history remains available in Git).
for path in list(Path('.').rglob('*.md')):
    if 'STAGE' in path.name.upper():
        path.unlink()

# This large duplicate predates the compiled StreamManager.cpp and is not part of CMake.
legacy = Path('src/StreamManagerImpl.inc')
if not legacy.exists():
    raise SystemExit('legacy StreamManagerImpl.inc is unexpectedly absent')
legacy.unlink()

print('V10.8.112 cleanup transform applied')
