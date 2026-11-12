// Offline, side-effect-free DVBStreamer5 transcoder benchmark.
// Reads a local MPEG-TS sample; never contacts the running service.
#include "media/NativeTranscoderPipeline.h"
#include "media/NativeTsDemux.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
bool evenSize(int value) {
    return value >= 64 && value <= 8192 && value % 2 == 0;
}
}

int main(int argc, char** argv) {
    if (argc != 7 && argc != 8) {
        std::cerr << "Usage: dvbstreamer5_downscale_benchmark"
                  << " input.ts duration_seconds width height"
                  << " cpu|auto|vaapi|qsv|nvenc video_bitrate_bps [output.ts]\n";
        return 2;
    }
    try {
        const std::string path = argv[1];
        const double sourceDuration = std::stod(argv[2]);
        const int width = std::stoi(argv[3]);
        const int height = std::stoi(argv[4]);
        const std::string backend = argv[5];
        const std::uint64_t videoBitrate = std::stoull(argv[6]);
        // Optional diagnostic output is a local file; it is not used by
        // the production streaming process or network output.
        std::ofstream outputFile;
        if (argc == 8) {
            outputFile.open(argv[7], std::ios::binary | std::ios::trunc);
            if (!outputFile) {
                std::cerr << "ERROR: cannot open diagnostic output TS\n";
                return 3;
            }
        }
        if (sourceDuration < 5.0 || sourceDuration > 180.0 ||
            !evenSize(width) || !evenSize(height) ||
            videoBitrate < 250000 || videoBitrate > 80000000 ||
            (backend != "cpu" && backend != "auto" &&
             backend != "vaapi" && backend != "qsv" && backend != "nvenc")) {
            std::cerr << "ERROR: invalid benchmark arguments\n";
            return 2;
        }
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "ERROR: cannot open input sample\n";
            return 3;
        }
        const std::streamoff fileBytes = static_cast<std::streamoff>(file.tellg());
        if (fileBytes < 1024 * 1024 || fileBytes % 188 != 0) {
            std::cerr << "ERROR: invalid/incomplete MPEG-TS sample\n";
            return 3;
        }
        file.seekg(0, std::ios::beg);
        using namespace dvbstreamer5::media::transcode;
        NativeTranscoderConfig config;
        config.streamId = "offline-benchmark";
        config.videoCodec = "h264";
        config.videoDecoder = "ffmpeg"; // Deliberately software-only decode.
        config.videoEncoder = backend;
        config.audioCodec = "copy";    // No unrelated audio re-encode.
        config.width = width;
        config.height = height;
        config.lockOutputGeometry = true;
        config.fps = 25.0;
        config.videoBitrate = videoBitrate;
        config.preset = "fast";
        config.profile = "high";
        config.gopFrames = 50;
        config.refFrames = 1;
        config.deinterlace = true;
        config.deinterlaceMode = "legacy";
        config.videoPesPacerEnabled = true;
        NativeTranscoderPipeline transcoder;
        std::string error;
        if (!transcoder.initialize(config, error)) {
            std::cerr << "ERROR: native init: " << error << '\n';
            return 4;
        }

        constexpr std::size_t kChunk = 188U * 128U;
        std::array<std::uint8_t, kChunk> input{};
        std::vector<std::uint8_t> output;
        std::uint64_t readBytes = 0, emittedBytes = 0;
        std::uint64_t outputVideoFrames = 0;
        // Count the actual H.264 access units that reached the output TS,
        // not just 125-frame PERF windows. This is a measurement helper,
        // not part of DVBStreamer5's production encoder.
        dvbstreamer5::media::mpegts::NativeTsDemux outputDemux;
        outputDemux.setSampleCallback([&](dvbstreamer5::media::mpegts::DemuxSample&& sample) {
            if (sample.stream.kind == dvbstreamer5::media::mpegts::ElementaryKind::Video)
                ++outputVideoFrames;
        });
        const auto countOutput = [&](const std::vector<std::uint8_t>& ts) {
            if (ts.empty()) return true;
            if (outputFile.is_open()) {
                outputFile.write(reinterpret_cast<const char*>(ts.data()),
                                 static_cast<std::streamsize>(ts.size()));
                if (!outputFile) {
                    std::cerr << "ERROR: diagnostic TS write failed\n";
                    return false;
                }
            }
            std::string countError;
            if (outputDemux.push(ts.data(), ts.size(), countError)) return true;
            std::cerr << "ERROR: output TS counting failed: " << countError << '\n';
            return false;
        };
        const auto started = std::chrono::steady_clock::now();
        while (file) {
            file.read(reinterpret_cast<char*>(input.data()), input.size());
            const auto count = file.gcount();
            if (count <= 0) break;
            if (count % 188 != 0) {
                std::cerr << "ERROR: source TS has partial packets\n";
                return 5;
            }
            readBytes += static_cast<std::uint64_t>(count);
            // Pace the *single, non-looped* sample to its measured duration.
            // No timestamp reset or repeated GOP; same realtime clip as FFmpeg -re.
            const double dueSeconds = sourceDuration *
                static_cast<double>(readBytes) / static_cast<double>(fileBytes);
            const auto due = started + std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(dueSeconds));
            std::this_thread::sleep_until(due);
            if (!transcoder.process(input.data(), static_cast<std::size_t>(count),
                                    output, error)) {
                std::cerr << "ERROR: native process: " << error
                          << " status=" << transcoder.status() << '\n';
                return 6;
            }
            emittedBytes += output.size();
            if (!countOutput(output)) return 8;
        }
        if (!transcoder.flush(output, error)) {
            std::cerr << "ERROR: native flush: " << error
                      << " status=" << transcoder.status() << '\n';
            return 7;
        }
        emittedBytes += output.size();
        if (!countOutput(output)) return 8;
        outputDemux.flush();
        if (outputFile.is_open()) {
            outputFile.close();
            if (!outputFile) {
                std::cerr << "ERROR: failed closing diagnostic TS\n";
                return 8;
            }
        }
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (emittedBytes < 100000 || outputVideoFrames == 0) {
            std::cerr << "ERROR: no usable transcoded output\n";
            return 8;
        }
        std::cout << "NATIVE_BENCH_RESULT backend=" << backend
                  << " output=" << width << 'x' << height
                  << " input_bytes=" << readBytes
                  << " output_bytes=" << emittedBytes
                  << " output_video_samples=" << outputVideoFrames
                  << " elapsed_s=" << elapsed << " status=" << transcoder.status() << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: benchmark exception: " << ex.what() << '\n';
        return 9;
    }
}
