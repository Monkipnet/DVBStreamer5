#include "StableUdpOutput.h"

#include "DvbSatellite.h"
#include "StreamManager.h"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace StableUdpOutput {
MemoryStats memoryStats() { return {}; }
}

namespace {

bool makeWarmDvbTune(const StreamConfig& cfg,
                     DvbSatelliteParams& params,
                     dvbstreamer5::media::network::LinuxDvbTuneConfig& tune,
                     std::string& error) {
    error.clear();
    if (!DvbSatellite::isDvbUri(cfg.inputUri)) {
        error = "not a DVB input";
        return false;
    }
    if (!DvbSatellite::parseUri(cfg.inputUri, params, error)) {
        if (error.empty()) error = "invalid DVB input";
        return false;
    }

    tune.adapter = params.adapter;
    tune.frontend = params.frontend;
    tune.frequencyKHz = params.frequencyKHz;
    tune.symbolRateK = params.symbolRateK;
    tune.polarity = params.polarity;
    tune.deliverySystem = params.deliverySystem;
    tune.modulation = params.modulation;
    tune.fec = params.fec;
    tune.diseqcSource = params.diseqcSource;
    tune.lnbLof1KHz = params.lnbLof1KHz;
    tune.lnbLof2KHz = params.lnbLof2KHz;
    tune.lnbSlofKHz = params.lnbSlofKHz;
    tune.streamId = params.streamId;
    tune.pids = "8192";
    return true;
}

std::string warmFrontendKey(const DvbSatelliteParams& params) {
    return std::to_string(params.adapter) + ":" +
           std::to_string(params.frontend);
}

} // namespace

// V10.8.179: HLS/HTTP OnDemand activation uses a lock per stream instead of
// holding the global OnDemand bookkeeping mutex while a tuner, CA client,
// decoder/transcoder and HLS pipeline are being started. Different tiles can
// therefore wake in parallel; duplicate requests for the same tile still share
// one serialized start.
bool StreamManager::ensureOnDemandStreamParallel(
    const std::string& id,
    const std::string& source,
    std::string* error) {
    if (error) error->clear();

    StreamConfig cfg;
    bool found = false;
    for (const auto& candidate : configManager.config.streams) {
        if (candidate.id == id) {
            cfg = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        if (error) *error = "stream is not configured";
        return false;
    }
    if (toLower(cfg.activationMode) != "ondemand") return true;

    std::shared_ptr<std::mutex> streamStartMutex;
    {
        std::lock_guard<std::mutex> demandLock(onDemandMutex);
        onDemandLastActivity[id] = std::chrono::steady_clock::now();
        auto& slot = onDemandStartMutexes[id];
        if (!slot) slot = std::make_shared<std::mutex>();
        streamStartMutex = slot;
    }

    std::lock_guard<std::mutex> streamStartLock(*streamStartMutex);
    {
        std::lock_guard<std::mutex> demandLock(onDemandMutex);
        onDemandLastActivity[id] = std::chrono::steady_clock::now();
    }
    if (isStreamActive(id)) return true;

    const auto begin = std::chrono::steady_clock::now();
    long long warmMs = -1;

    // For satellite OnDemand, establish/refresh the transponder-only warm bus
    // before constructing the service pipeline. warmStandby owns only the DVB
    // frontend + full-MPTS reader: there is deliberately no CA reservation,
    // service remap, decoder, transcoder or HLS segmenter in this state.
    if (DvbSatellite::isDvbUri(cfg.inputUri)) {
        DvbSatelliteParams params;
        dvbstreamer5::media::network::LinuxDvbTuneConfig tune;
        std::string warmError;
        if (!makeWarmDvbTune(cfg, params, tune, warmError)) {
            if (error) *error = warmError;
            std::cerr << "ONDEMAND ACTIVATE FAILED stream=" << id
                      << " source=" << source
                      << " stage=dvb-parse error=" << warmError << std::endl;
            return false;
        }

        const auto warmBegin = std::chrono::steady_clock::now();
        {
            auto tuneGuard = DvbSatellite::acquireFrontendTuneGuard(params);
            if (!sharedDvbInputs.warmStandby(tune, warmError)) {
                if (error) *error = warmError.empty()
                    ? "failed to establish DVB warm standby"
                    : warmError;
                std::cerr << "ONDEMAND ACTIVATE FAILED stream=" << id
                          << " source=" << source
                          << " stage=dvb-warm error="
                          << (warmError.empty() ? "unknown" : warmError)
                          << std::endl;
                return false;
            }
        }
        warmMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - warmBegin).count();
    }

    std::string startError;
    if (!startStream(cfg, &startError)) {
        // A manual control-panel start may have won after our pre-check.
        if (isStreamActive(id)) return true;
        if (error) *error = startError.empty()
            ? "failed to start on-demand stream"
            : startError;
        std::cerr << "ONDEMAND ACTIVATE FAILED stream=" << id
                  << " source=" << source
                  << " stage=service-pipeline error="
                  << (startError.empty() ? "unknown" : startError)
                  << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> demandLock(onDemandMutex);
        onDemandStartedStreams.insert(id);
        onDemandLastActivity[id] = std::chrono::steady_clock::now();
    }

    const auto startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    std::cerr << "ONDEMAND ACTIVATE stream=" << id
              << " source=" << source
              << " startup_ms=" << startupMs;
    if (warmMs >= 0) std::cerr << " dvb_warm_ms=" << warmMs;
    std::cerr << " ca="
              << (cfg.conditionalAccessClient.empty() ? "fta" : "managed")
              << std::endl;
    return true;
}

// Tune one OnDemand transponder per otherwise-unused physical frontend after
// HTTP startup. This is intentionally transponder-only: zero StreamState objects
// are created, so decoders/transcoders/CA/HLS stay completely off in standby.
void StreamManager::prewarmOnDemandDvbInputs() {
    struct WarmJob {
        std::string streamId;
        std::string frontend;
        DvbSatelliteParams params;
        dvbstreamer5::media::network::LinuxDvbTuneConfig tune;
    };

    // Only an online stream that is going to auto-start competes with the boot
    // prewarm worker. Merely configured/stopped online tiles must not prevent an
    // OnDemand frontend from being warm; a later manual start may retune an idle
    // warm bus safely because it has zero service subscribers.
    std::set<std::string> reservedByOnline;
    for (const auto& cfg : configManager.config.streams) {
        if (toLower(cfg.activationMode) == "ondemand" || !cfg.autoStart ||
            !DvbSatellite::isDvbUri(cfg.inputUri)) {
            continue;
        }
        DvbSatelliteParams params;
        dvbstreamer5::media::network::LinuxDvbTuneConfig tune;
        std::string ignored;
        if (makeWarmDvbTune(cfg, params, tune, ignored)) {
            reservedByOnline.insert(warmFrontendKey(params));
        }
    }

    std::map<std::string, WarmJob> selected;
    for (const auto& cfg : configManager.config.streams) {
        if (toLower(cfg.activationMode) != "ondemand" ||
            !DvbSatellite::isDvbUri(cfg.inputUri)) {
            continue;
        }

        DvbSatelliteParams params;
        dvbstreamer5::media::network::LinuxDvbTuneConfig tune;
        std::string parseError;
        if (!makeWarmDvbTune(cfg, params, tune, parseError)) {
            std::cerr << "ONDEMAND STANDBY prewarm skip stream=" << cfg.id
                      << " reason=" << parseError << std::endl;
            continue;
        }
        const std::string frontend = warmFrontendKey(params);
        if (reservedByOnline.count(frontend)) {
            std::cerr << "ONDEMAND STANDBY prewarm skip stream=" << cfg.id
                      << " frontend=" << frontend
                      << " reason=frontend-reserved-by-online-autostart" << std::endl;
            continue;
        }
        if (selected.count(frontend)) continue;
        selected.emplace(frontend, WarmJob{cfg.id, frontend, params, tune});
    }

    if (selected.empty()) {
        std::cerr << "ONDEMAND STANDBY prewarm frontends=0" << std::endl;
        return;
    }

    std::vector<WarmJob> jobs;
    jobs.reserve(selected.size());
    for (auto& [frontend, job] : selected) {
        (void)frontend;
        jobs.push_back(std::move(job));
    }

    const std::size_t workerCount = std::min<std::size_t>(4U, jobs.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    std::cerr << "ONDEMAND STANDBY prewarm frontends=" << jobs.size()
              << " workers=" << workerCount << std::endl;

    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([this, &jobs, &next, worker] {
            for (;;) {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= jobs.size()) break;
                const auto& job = jobs[index];
                const auto begin = std::chrono::steady_clock::now();
                std::string warmError;
                bool ok = false;
                {
                    auto tuneGuard = DvbSatellite::acquireFrontendTuneGuard(job.params);
                    ok = sharedDvbInputs.warmStandby(job.tune, warmError);
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin).count();
                if (ok) {
                    std::cerr << "ONDEMAND STANDBY prewarm ready stream="
                              << job.streamId
                              << " frontend=" << job.frontend
                              << " worker=" << worker
                              << " startup_ms=" << elapsed
                              << " decoder=off transcoder=off ca=off hls=off"
                              << std::endl;
                } else {
                    std::cerr << "ONDEMAND STANDBY prewarm failed stream="
                              << job.streamId
                              << " frontend=" << job.frontend
                              << " worker=" << worker
                              << " startup_ms=" << elapsed
                              << " error="
                              << (warmError.empty() ? "unknown" : warmError)
                              << std::endl;
                }
            }
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}
