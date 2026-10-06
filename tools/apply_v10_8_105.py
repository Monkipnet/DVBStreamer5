from pathlib import Path

relay_path = Path("src/media/NativeUdpRelay.cpp")
text = relay_path.read_text()

start_marker = "    // observeTransport consumers such as SRT/RTSP/RTMP receive the already\n"
end_marker = "    std::array<std::uint8_t, 65536> datagram {};\n"
if text.count(start_marker) != 1 or text.count(end_marker) != 1:
    raise SystemExit("unexpected NativeUdpRelay observer block layout")
start = text.index(start_marker)
end = text.index(end_marker, start)

new_block = """    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.  The old
    // implementation only slept between already-existing TS packets, so when
    // payload bitrate was below target it remained VBR and CBR Out simply
    // followed the source.  Give these consumers their own CbrTsPacer: it emits
    // the configured number of TS packets per second and fills missing capacity
    // with PID 0x1fff NULL packets, exactly like native UDP-CBR.
    std::unique_ptr<dvbstreamer5::media::mpegts::CbrTsPacer> observedCbrPacer;
    std::mutex observedCbrMutex;
    std::condition_variable observedCbrCondition;
    bool observedCbrStop = false;
    std::thread observedCbrWorker;

    if (config_.paceObservedTransport && config_.targetBitrate > 0) {
        try {
            observedCbrPacer =
                std::make_unique<dvbstreamer5::media::mpegts::CbrTsPacer>(
                    config_.targetBitrate);
            observedCbrWorker = std::thread([&] {
                while (true) {
                    dvbstreamer5::media::mpegts::CbrDatagram cbrDatagram {};
                    bool ready = false;
                    {
                        std::unique_lock<std::mutex> lock(observedCbrMutex);
                        observedCbrCondition.wait(lock, [&] {
                            return observedCbrStop ||
                                (observedCbrPacer && observedCbrPacer->started());
                        });
                        if (observedCbrStop) break;

                        const auto deadline = observedCbrPacer->nextDeadline();
                        if (observedCbrCondition.wait_until(
                                lock, deadline, [&] { return observedCbrStop; })) {
                            break;
                        }
                        if (observedCbrStop) break;

                        ready = observedCbrPacer->nextDatagram(
                            std::chrono::steady_clock::now(), cbrDatagram);
                    }
                    if (!ready) continue;

                    std::array<std::uint8_t,
                        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *
                            dvbstreamer5::media::mpegts::kPacketSize> bytes {};
                    for (std::size_t index = 0; index < cbrDatagram.size(); ++index) {
                        std::memcpy(
                            bytes.data() +
                                index * dvbstreamer5::media::mpegts::kPacketSize,
                            cbrDatagram[index].data(),
                            dvbstreamer5::media::mpegts::kPacketSize);
                    }
                    config_.observeTransport(bytes.data(), bytes.size());
                }
            });
            std::cerr << "NATIVE OBSERVED CBR start target_kbps="
                      << (config_.targetBitrate / 1000ULL) << std::endl;
        } catch (const std::exception& exception) {
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = std::string("native observed CBR setup failed: ") +
                    exception.what();
            }
            running_.store(false, std::memory_order_release);
            httpQueueCondition_.notify_all();
            return;
        }
    }

    auto observePackets = [&](
        const std::vector<dvbstreamer5::media::mpegts::Packet>& observedPackets) -> bool {
        if (!config_.observeTransport || observedPackets.empty()) return true;

        if (observedCbrPacer) {
            {
                std::lock_guard<std::mutex> lock(observedCbrMutex);
                for (const auto& packet : observedPackets) {
                    if (!observedCbrPacer->enqueue(packet)) {
                        std::lock_guard<std::mutex> errorLock(errorMutex_);
                        lastError_ =
                            "observed CBR input exceeded the bounded 2 MiB pacing queue; "
                            "increase target bitrate";
                        running_.store(false, std::memory_order_release);
                        httpQueueCondition_.notify_all();
                        return false;
                    }
                }
            }
            observedCbrCondition.notify_one();
            return true;
        }

        constexpr std::size_t kObservedPacketsPerBatch = 7;
        for (std::size_t first = 0; first < observedPackets.size();
             first += kObservedPacketsPerBatch) {
            if (!running_.load(std::memory_order_acquire)) return false;
            const std::size_t count = (std::min)(
                kObservedPacketsPerBatch, observedPackets.size() - first);
            std::array<std::uint8_t,
                kObservedPacketsPerBatch * dvbstreamer5::media::mpegts::kPacketSize> bytes {};
            for (std::size_t index = 0; index < count; ++index) {
                std::memcpy(
                    bytes.data() + index * dvbstreamer5::media::mpegts::kPacketSize,
                    observedPackets[first + index].data(),
                    dvbstreamer5::media::mpegts::kPacketSize);
            }
            config_.observeTransport(
                bytes.data(), count * dvbstreamer5::media::mpegts::kPacketSize);
        }
        return true;
    };
"""
text = text[:start] + new_block + text[end:]

# Insert worker shutdown immediately before the final run() shutdown.  There are
# earlier error paths with the same running_.store() call, so use the last one.
shutdown_marker = "    running_.store(false, std::memory_order_release);\n    httpQueueCondition_.notify_all();\n"
shutdown_pos = text.rfind(shutdown_marker)
if shutdown_pos < end:
    raise SystemExit("unexpected NativeUdpRelay shutdown layout")
shutdown = """    {
        std::lock_guard<std::mutex> lock(observedCbrMutex);
        observedCbrStop = true;
    }
    observedCbrCondition.notify_all();
    if (observedCbrWorker.joinable()) {
        observedCbrWorker.join();
    }

"""
text = text[:shutdown_pos] + shutdown + text[shutdown_pos:]
relay_path.write_text(text)

manager_path = Path("src/StreamManager.cpp")
manager = manager_path.read_text()
old = "        relay.paceObservedTransport = false;\n"
new = """        // V10.8.105: keep paceObservedTransport enabled when CBR is selected.
        // Shared DVB only changes how input arrives; SRT/HTTP/HLS still require
        // the output CBR shaper to insert NULL packets up to targetBitrate.
"""
if manager.count(old) != 1:
    raise SystemExit("unexpected StreamManager shared DVB CBR layout")
manager_path.write_text(manager.replace(old, new, 1))

version_path = Path("src/AppVersion.h")
version = version_path.read_text()
old_version = 'inline constexpr const char* kProgramVersion = "10.8.104";'
new_version = 'inline constexpr const char* kProgramVersion = "10.8.105";'
if version.count(old_version) != 1:
    raise SystemExit("unexpected AppVersion")
version_path.write_text(version.replace(old_version, new_version, 1))
