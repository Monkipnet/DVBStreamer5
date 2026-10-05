from pathlib import Path

p = Path('src/HttpServer.cpp')
s = p.read_text()
old = '''        for (int attempt = 0; attempt < 120 && !livePlaylistReady(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
'''
new = '''        for (int attempt = 0; attempt < 120 && !livePlaylistReady(); ++attempt) {
            // V10.8.92: this HTTP request itself is an active OnDemand viewer.
            // The decoder-safe first H.264/H.265 segment can legitimately take
            // longer than the normal 10 s idle window. Refresh OnDemand activity
            // while the request is blocked here so the monitor cannot stop the
            // stream underneath the pending playlist request. Once the request
            // completes, normal HLS playlist/segment requests keep activity alive
            // and the historical 10 s post-view idle shutdown remains unchanged.
            if ((attempt % 10) == 0) {
                std::string startupKeepaliveError;
                if (!streamManager.ensureOnDemandStream(
                        id, "hls-startup-wait", &startupKeepaliveError)) {
                    demandError = startupKeepaliveError;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
'''
if old not in s:
    raise SystemExit('V10.8.91 startup wait loop not found')
p.write_text(s.replace(old, new, 1))

vp = Path('src/AppVersion.h')
v = vp.read_text()
if 'kProgramVersion = "10.8.91"' not in v:
    raise SystemExit('version anchor 10.8.91 not found')
vp.write_text(v.replace('kProgramVersion = "10.8.91"', 'kProgramVersion = "10.8.92"', 1))
