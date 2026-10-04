from pathlib import Path

p = Path('src/StreamManager.cpp')
s = p.read_text()
s = s.replace(
    'low-bitrate hardware-auto pipeline whose output is used only by preview.ts.\n    // NVDEC/NVENC or QSV decode/VPP/encode is preferred for broadcast Main/HEVC;\n    // compatible streams can still fall back to the native CPU codecs.',
    'low-bitrate CPU pipeline whose output is used only by preview.ts.\n    // V10.8.69 restores the proven V10.8.41 preview path; production transcoding\n    // remains independent and may still use hardware acceleration.',
    1)
if 'previewTc.videoEncoder = "auto";' not in s:
    raise SystemExit('preview encoder anchor not found')
s = s.replace('previewTc.videoEncoder = "auto";', 'previewTc.videoEncoder = "cpu";', 1)
s = s.replace('encoder=auto video_kbps=1800 audio_kbps=128',
              'encoder=cpu video_kbps=1800 audio_kbps=128', 1)
p.write_text(s)

h = Path('src/StreamManager.h')
s = h.read_text()
old = '''    bool initialize(
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
new = '''    bool initialize(
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
if old not in s:
    raise SystemExit('LazyPreviewTranscoder initialize anchor not found')
h.write_text(s.replace(old, new, 1))

v = Path('src/AppVersion.h')
s = v.read_text()
if '"10.8.68"' not in s:
    raise SystemExit('version anchor not found')
v.write_text(s.replace('"10.8.68"', '"10.8.69"', 1))
