from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.126";',
    'inline constexpr const char* kProgramVersion = "10.8.127";',
)

# V10.8.127: when a stream already has a native UDP-CBR output at the exact
# configured bitrate, that output's generated 1316-byte datagrams are already
# the desired CBR transport for HLS/HTTP/SRT/RTSP/RTMP observers.  Dispatch the
# same datagrams asynchronously through four shared shards instead of running a
# second CbrTsPacer + timer thread for every channel.  Per-stream ordering is
# preserved because a token is permanently assigned to one shard.  A bounded
# 2 MiB per-stream queue preserves the previous observed-pacer overload guard.
dispatcher = r'''
constexpr std::size_t kObservedCbrDatagramBytes =
    dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *
    dvbstreamer5::media::mpegts::kPacketSize;
constexpr std::size_t kObservedCbrDispatchMaximumQueuedBytes = 2 * 1024 * 1024;
constexpr std::size_t kObservedCbrDispatchShardCount = 4;

static_assert(
    sizeof(dvbstreamer5::media::mpegts::CbrDatagram) == kObservedCbrDatagramBytes,
    "CBR datagram packet storage must be contiguous");

struct ObservedCbrDispatchToken {
    explicit ObservedCbrDispatchToken(
        const NativeTransportObserver& transportObserver,
        std::size_t shardIndex)
        : observer(transportObserver), shard(shardIndex) {}

    NativeTransportObserver observer;
    const std::size_t shard;
    std::mutex mutex;
    std::condition_variable condition;
    bool active = true;
    std::size_t queuedBytes = 0;
    std::size_t inFlight = 0;
};

class ObservedCbrDispatcher {
public:
    static ObservedCbrDispatcher& instance() {
        static ObservedCbrDispatcher dispatcher;
        return dispatcher;
    }

    std::shared_ptr<ObservedCbrDispatchToken> createToken(
        const NativeTransportObserver& observer) {
        const std::size_t shard =
            nextShard_.fetch_add(1, std::memory_order_relaxed) %
            kObservedCbrDispatchShardCount;
        return std::make_shared<ObservedCbrDispatchToken>(observer, shard);
    }

    bool enqueue(
        const std::shared_ptr<ObservedCbrDispatchToken>& token,
        const dvbstreamer5::media::mpegts::CbrDatagram& datagram) {
        if (!token) return false;
        {
            std::lock_guard<std::mutex> tokenLock(token->mutex);
            if (!token->active ||
                token->queuedBytes + kObservedCbrDatagramBytes >
                    kObservedCbrDispatchMaximumQueuedBytes) {
                return false;
            }
            token->queuedBytes += kObservedCbrDatagramBytes;
        }

        Shard& shard = shards_[token->shard];
        bool notify = false;
        {
            std::lock_guard<std::mutex> queueLock(shard.mutex);
            if (shard.stopping) {
                std::lock_guard<std::mutex> tokenLock(token->mutex);
                token->queuedBytes -= kObservedCbrDatagramBytes;
                return false;
            }
            notify = shard.queue.empty();
            Task task;
            task.token = token;
            task.datagram = datagram;
            shard.queue.push_back(std::move(task));
        }
        if (notify) shard.condition.notify_one();
        return true;
    }

    void deactivateAndWait(
        const std::shared_ptr<ObservedCbrDispatchToken>& token) noexcept {
        if (!token) return;
        std::unique_lock<std::mutex> lock(token->mutex);
        token->active = false;
        token->condition.wait(lock, [&] { return token->inFlight == 0; });
    }

    static constexpr std::size_t shardCount() noexcept {
        return kObservedCbrDispatchShardCount;
    }

private:
    struct Task {
        std::shared_ptr<ObservedCbrDispatchToken> token;
        dvbstreamer5::media::mpegts::CbrDatagram datagram {};
    };

    struct Shard {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<Task> queue;
        bool stopping = false;
        std::thread worker;
    };

    ObservedCbrDispatcher() {
        try {
            for (std::size_t index = 0; index < shards_.size(); ++index) {
                shards_[index].worker = std::thread([this, index] {
                    runShard(index);
                });
            }
        } catch (...) {
            for (auto& shard : shards_) {
                {
                    std::lock_guard<std::mutex> lock(shard.mutex);
                    shard.stopping = true;
                }
                shard.condition.notify_all();
            }
            for (auto& shard : shards_) {
                if (shard.worker.joinable()) shard.worker.join();
            }
            throw;
        }
    }

    ~ObservedCbrDispatcher() {
        for (auto& shard : shards_) {
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                shard.stopping = true;
            }
            shard.condition.notify_all();
        }
        for (auto& shard : shards_) {
            if (shard.worker.joinable()) shard.worker.join();
        }
    }

    void runShard(std::size_t index) {
        Shard& shard = shards_[index];
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(shard.mutex);
                shard.condition.wait(lock, [&] {
                    return shard.stopping || !shard.queue.empty();
                });
                if (shard.stopping) return;
                task = std::move(shard.queue.front());
                shard.queue.pop_front();
            }

            bool invoke = false;
            {
                std::lock_guard<std::mutex> tokenLock(task.token->mutex);
                if (task.token->queuedBytes >= kObservedCbrDatagramBytes) {
                    task.token->queuedBytes -= kObservedCbrDatagramBytes;
                } else {
                    task.token->queuedBytes = 0;
                }
                if (task.token->active) {
                    ++task.token->inFlight;
                    invoke = true;
                }
            }

            if (!invoke) continue;

            task.token->observer(
                reinterpret_cast<const std::uint8_t*>(task.datagram.data()),
                kObservedCbrDatagramBytes);

            {
                std::lock_guard<std::mutex> tokenLock(task.token->mutex);
                if (task.token->inFlight != 0) --task.token->inFlight;
                if (!task.token->active && task.token->inFlight == 0) {
                    task.token->condition.notify_all();
                }
            }
        }
    }

    std::array<Shard, kObservedCbrDispatchShardCount> shards_;
    std::atomic<std::size_t> nextShard_ {0};
};

struct ObservedCbrDispatchLease {
    std::shared_ptr<ObservedCbrDispatchToken> token;

    ~ObservedCbrDispatchLease() {
        if (token) {
            ObservedCbrDispatcher::instance().deactivateAndWait(token);
        }
    }
};

'''
replace_once(
    "src/media/NativeUdpRelay.cpp",
    "\nstd::string inputInterfaceFor(const NativeUdpRelayConfig& config, bool multicastOrWildcard) {",
    "\n" + dispatcher + "std::string inputInterfaceFor(const NativeUdpRelayConfig& config, bool multicastOrWildcard) {",
)

# Pick the first native UDP-CBR output as the byte-identical observer clock.
# If dispatcher setup is unavailable, leave the token empty and retain the old
# independent observed CBR pacer below as a transparent fallback.
setup = r'''    std::size_t sharedObservedCbrOutputIndex = outputs.size();
    ObservedCbrDispatchLease sharedObservedCbrLease;
    if (config_.paceObservedTransport && config_.targetBitrate > 0 &&
        config_.observeTransport) {
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            if (!outputs[index].cbrPacer) continue;
            try {
                sharedObservedCbrLease.token =
                    ObservedCbrDispatcher::instance().createToken(
                        config_.observeTransport);
                sharedObservedCbrOutputIndex = index;
                std::cerr << "NATIVE OBSERVED CBR reuse_udp_dispatch output_index="
                          << outputs[index].index
                          << " target_kbps=" << (config_.targetBitrate / 1000ULL)
                          << " shards=" << ObservedCbrDispatcher::shardCount()
                          << std::endl;
            } catch (const std::exception& exception) {
                sharedObservedCbrLease.token.reset();
                sharedObservedCbrOutputIndex = outputs.size();
                std::cerr << "NATIVE OBSERVED CBR dispatcher unavailable; "
                             "using dedicated pacer: "
                          << exception.what() << std::endl;
            }
            break;
        }
    }

'''
replace_once(
    "src/media/NativeUdpRelay.cpp",
    "        outputs.push_back(std::move(output));\n        ++seed;\n    }\n\n    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.",
    "        outputs.push_back(std::move(output));\n        ++seed;\n    }\n\n" + setup + "    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "    if (config_.paceObservedTransport && config_.targetBitrate > 0) {\n        try {\n            observedCbrPacer =",
    "    if (config_.paceObservedTransport && config_.targetBitrate > 0 &&\n        !sharedObservedCbrLease.token) {\n        try {\n            observedCbrPacer =",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "        if (!config_.observeTransport || observedPackets.empty()) return true;\n\n        if (observedCbrPacer) {",
    "        if (!config_.observeTransport || observedPackets.empty()) return true;\n\n        // V10.8.127: the primary UDP-CBR datagram is dispatched after send, so\n        // do not feed a second per-stream CBR pacer with the same TS packets.\n        if (sharedObservedCbrLease.token) return true;\n\n        if (observedCbrPacer) {",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    '''                if (!sendCbrDatagram(\n                        cbrDatagram, *output.socket, outputBytes_, error)) {\n                    std::lock_guard<std::mutex> lock(errorMutex_);\n                    lastError_ = error.empty()\n                        ? "UDP CBR output send failed" : error;\n                    break;\n                }\n                ++output.datagramsSent;\n''',
    '''                if (!sendCbrDatagram(\n                        cbrDatagram, *output.socket, outputBytes_, error)) {\n                    std::lock_guard<std::mutex> lock(errorMutex_);\n                    lastError_ = error.empty()\n                        ? "UDP CBR output send failed" : error;\n                    break;\n                }\n\n                // V10.8.127: reuse this already-paced, already-NULL-stuffed\n                // datagram for observers without running another timer clock.\n                // Dispatch is asynchronous, so HLS/HTTP/SRT work cannot delay\n                // the UDP sender itself.\n                if (sharedObservedCbrLease.token &&\n                    output.index == sharedObservedCbrOutputIndex &&\n                    !ObservedCbrDispatcher::instance().enqueue(\n                        sharedObservedCbrLease.token, cbrDatagram)) {\n                    error =\n                        "observed CBR dispatcher exceeded the bounded 2 MiB queue";\n                    std::lock_guard<std::mutex> lock(errorMutex_);\n                    lastError_ = error;\n                    break;\n                }\n                ++output.datagramsSent;\n''',
)

print("V10.8.127 shared observed CBR dispatcher applied")
