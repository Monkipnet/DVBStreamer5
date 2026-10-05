from pathlib import Path

p = Path('src/HttpServer.cpp')
s = p.read_text()
old = '''    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {
        for (int attempt = 0; attempt < 30 && !std::filesystem::exists(filePath); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
'''
new = '''    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {
        // V10.8.90: decoder-safe H.264/H.265 segment boundaries may need to wait
        // for the next IDR/IRAP before the first live playlist can be published.
        // Keep the initial HTTP request open long enough for that safe boundary
        // instead of returning a premature 404 after the historical 3 seconds.
        for (int attempt = 0; attempt < 120 && !std::filesystem::exists(filePath); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
'''
if old not in s:
    raise SystemExit('HLS startup wait block not found')
p.write_text(s.replace(old, new, 1))

vp = Path('src/AppVersion.h')
v = vp.read_text()
if 'kProgramVersion = "10.8.89"' not in v:
    raise SystemExit('version anchor not found')
vp.write_text(v.replace('kProgramVersion = "10.8.89"', 'kProgramVersion = "10.8.90"', 1))
