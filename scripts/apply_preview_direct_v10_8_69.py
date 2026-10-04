from pathlib import Path

h = Path('src/StreamManager.h')
s = h.read_text()
old_init = '''    bool initialize(
        const dvbstreamer5::media::transcode::NativeTranscoderConfig& config,
        std::string& error) {
        reset();
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        // V10.8.69: restore V10.8.41 preview semantics. The dedicated
        // preview transcoder is initialized immediately and remains alive for
        // the stream lifetime instead of being torn down/recreated on every
        // preview idle interval. This avoids decoder state loss on live DVB.
        if (!pipeline_.initialize(config_, error)) {
            configured_ = false;
            active_ = false;
            return false;
        }
        configured_ = true;
        active_ = true;
        stopMonitor_ = false;
        lastActivity_ = std::chrono::steady_clock::now();
        error.clear();
        return true;
    }
'''
new_init = '''    bool initialize(
        const dvbstreamer5::media::transcode::NativeTranscoderConfig& config,
        std::string& error) {
        reset();
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        configured_ = true;
        error.clear();
        return true;
    }
'''
if old_init not in s:
    raise SystemExit('temporary eager wrapper anchor not found')
s = s.replace(old_init, new_init, 1)
old_field = '''    // Private browser preview is always H.264/AAC MPEG-TS. The lazy handle
    // starts its native workers only while preview.ts is actively consuming
    // transport and tears them down after the short idle grace period.
    LazyPreviewTranscoderHandle nativePreviewTranscoder;
'''
new_field = '''    // V10.8.69: browser preview uses the direct V10.8.41 pipeline again.
    // It stays initialized for the stream lifetime and is isolated from the
    // production transcoder. Output is fixed H.264/AAC 1280x720 square-pixel 16:9.
    std::unique_ptr<dvbstreamer5::media::transcode::NativeTranscoderPipeline> nativePreviewTranscoder;
'''
if old_field not in s:
    raise SystemExit('preview field anchor not found')
s = s.replace(old_field, new_field, 1)
h.write_text(s)
