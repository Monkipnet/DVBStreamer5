from pathlib import Path

p = Path('src/HttpServer.cpp')
s = p.read_text()
old = '''    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {
        // V10.8.90: decoder-safe H.264/H.265 segment boundaries may need to wait
        // for the next IDR/IRAP before the first live playlist can be published.
        // Keep the initial HTTP request open long enough for that safe boundary
        // instead of returning a premature 404 after the historical 3 seconds.
        for (int attempt = 0; attempt < 120 && !std::filesystem::exists(filePath); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
'''
new = '''    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {
        // V10.8.91: an old OnDemand playlist can survive the previous generation
        // until the new segmenter clears its directory.  Waiting only for
        // video.m3u8 to exist can therefore return that stale manifest while its
        // referenced segment0000000000.ts is already being deleted by the new
        // generation.  Treat a live playlist as ready only when the first media
        // URI it advertises exists alongside it.
        const auto livePlaylistReady = [&filePath]() {
            std::error_code ec;
            if (!std::filesystem::exists(filePath, ec) || ec ||
                !std::filesystem::is_regular_file(filePath, ec) || ec) {
                return false;
            }

            std::ifstream probe(filePath, std::ios::binary);
            if (!probe.is_open()) return false;
            std::string line;
            while (std::getline(probe, line)) {
                if (!line.empty() && line.back() == '\\r') line.pop_back();
                if (line.empty() || line.front() == '#') continue;
                const std::string relative = cleanHlsRelativePath(line);
                if (relative.empty()) return false;
                const auto mediaPath = filePath.parent_path() / relative;
                ec.clear();
                return std::filesystem::exists(mediaPath, ec) && !ec &&
                       std::filesystem::is_regular_file(mediaPath, ec) && !ec;
            }
            return false;
        };

        // Keep the V10.8.90 12-second startup budget, but spend it waiting for a
        // coherent playlist+first-media pair rather than for the manifest alone.
        for (int attempt = 0; attempt < 120 && !livePlaylistReady(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
'''
if old not in s:
    raise SystemExit('V10.8.90 HLS startup wait block not found')
p.write_text(s.replace(old, new, 1))

vp = Path('src/AppVersion.h')
v = vp.read_text()
if 'kProgramVersion = "10.8.90"' not in v:
    raise SystemExit('version anchor not found')
vp.write_text(v.replace('kProgramVersion = "10.8.90"', 'kProgramVersion = "10.8.91"', 1))
