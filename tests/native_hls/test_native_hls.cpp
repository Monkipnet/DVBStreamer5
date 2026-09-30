#include "media/NativeHlsInput.h"
#include "media/NativeHlsSegmenter.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using dvbstreamer5::media::mpegts::Packet;

Packet pcrPacket(std::uint64_t pcr90k, bool randomAccess) {
    Packet packet{};
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = 0x01;
    packet[2] = 0x00;
    packet[3] = 0x30;
    packet[4] = 7;
    packet[5] = static_cast<std::uint8_t>(0x10U | (randomAccess ? 0x40U : 0U));
    packet[6] = static_cast<std::uint8_t>((pcr90k >> 25) & 0xffU);
    packet[7] = static_cast<std::uint8_t>((pcr90k >> 17) & 0xffU);
    packet[8] = static_cast<std::uint8_t>((pcr90k >> 9) & 0xffU);
    packet[9] = static_cast<std::uint8_t>((pcr90k >> 1) & 0xffU);
    packet[10] = static_cast<std::uint8_t>((pcr90k & 1U) << 7);
    packet[11] = 0;
    return packet;
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool testInput() {
    httplib::Server server;
    std::atomic<int> mediaGets{0};
    const std::string auth = "Bearer native-stage4";
    auto authorized = [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Authorization") != auth) {
            res.status = 403;
            return false;
        }
        return true;
    };
    server.Get("/master.m3u8", [&](const auto& req, auto& res) {
        if (!authorized(req, res)) return;
        res.set_content("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000\nlow.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=2000000\nhigh.m3u8\n", "application/vnd.apple.mpegurl");
    });
    server.Get("/low.m3u8", [&](const auto& req, auto& res) {
        if (!authorized(req, res)) return;
        res.set_content("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nlow.ts\n#EXT-X-ENDLIST\n", "application/vnd.apple.mpegurl");
    });
    server.Get("/high.m3u8", [&](const auto& req, auto& res) {
        if (!authorized(req, res)) return;
        res.set_content("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:40\n#EXTINF:1,\na.ts\n#EXTINF:1,\nb.ts\n#EXTINF:1,\nc.ts\n#EXT-X-ENDLIST\n", "application/vnd.apple.mpegurl");
    });
    for (const char* name : {"/a.ts", "/b.ts", "/c.ts"}) {
        server.Get(name, [&](const auto& req, auto& res) {
            if (!authorized(req, res)) return;
            ++mediaGets;
            std::string body(188 * 7, '\0');
            for (std::size_t i = 0; i < body.size(); i += 188) body[i] = 0x47;
            res.set_content(std::move(body), "video/MP2T");
        });
    }
    server.Get("/low.ts", [&](const auto&, auto& res) { res.status = 500; });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) return false;
    std::thread http([&] { server.listen_after_bind(); });

    StreamConfig cfg;
    cfg.id = "hls-input-test";
    cfg.inputUri = "http://127.0.0.1:" + std::to_string(port) + "/master.m3u8";
    cfg.inputMode = "hls";
    cfg.targetBitrate = 2500000;
    cfg.hlsAccessKeyMode = "header";
    cfg.hlsAccessKeyName = "Authorization";
    cfg.hlsAccessKeyValue = auth;

    std::atomic<bool> finished{false};
    std::atomic<std::uint64_t> bytes{0};
    std::string finishError;
    dvbstreamer5::media::hls::NativeHlsInput input;
    std::string error;
    const bool started = input.start(cfg,
        [&](const std::uint8_t*, std::size_t size) { bytes += size; return true; },
        [&](const std::string& value) { finishError = value; finished = true; }, error);
    if (!started) {
        server.stop(); http.join();
        std::cerr << error << "\n";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!finished && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    input.stop();
    server.stop();
    http.join();
    return finishError.empty() && mediaGets == 3 && bytes == 3ULL * 188ULL * 7ULL;
}

bool testOutput() {
    const auto directory = std::filesystem::temp_directory_path() / "dvbstreamer5-native-hls-test";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    dvbstreamer5::media::hls::NativeHlsSegmenter segmenter;
    dvbstreamer5::media::hls::NativeHlsSegmenterConfig cfg;
    cfg.directory = directory;
    cfg.targetDurationSeconds = 1.0;
    cfg.liveWindowSegments = 4;
    std::string error;
    if (!segmenter.start(cfg, error)) return false;
    for (std::uint64_t second = 0; second <= 4; ++second) {
        const Packet packet = pcrPacket(second * 90000ULL, second != 0);
        if (!segmenter.push(packet.data(), packet.size())) return false;
    }
    segmenter.stop();
    const std::string playlist = readAll(directory / "video.m3u8");
    const bool ok = segmenter.segmentCount() >= 4 &&
        playlist.find("#EXT-X-TARGETDURATION:1") != std::string::npos &&
        playlist.find("#EXT-X-ENDLIST") != std::string::npos &&
        playlist.find("segment") != std::string::npos;
    std::filesystem::remove_all(directory, ec);
    return ok;
}

} // namespace

int main() {
    if (!testInput()) { std::cerr << "native HLS input test failed\n"; return 1; }
    if (!testOutput()) { std::cerr << "native HLS output test failed\n"; return 2; }
    std::cout << "PASS: native HLS input/output\n";
    return 0;
}
