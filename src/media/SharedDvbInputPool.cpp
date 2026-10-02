#include "media/SharedDvbInputPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace dvbstreamer5::media::network {
namespace {

constexpr std::size_t kReadChunkBytes = 64 * 1024;
constexpr std::size_t kSubscriberQueueBytes = 8 * 1024 * 1024;

std::string frontendKey(const LinuxDvbTuneConfig& config) {
    return std::to_string(config.adapter) + ":" + std::to_string(config.frontend);
}

std::string transponderKey(const LinuxDvbTuneConfig& config) {
    std::ostringstream out;
    out
        << config.adapter << ':' << config.frontend
        << "|rf=" << config.frequencyKHz
        << "|sr=" << config.symbolRateK
        << "|pol=" << config.polarity
        << "|sys=" << config.deliverySystem
        << "|mod=" << config.modulation
        << "|fec=" << config.fec
        << "|diseqc=" << config.diseqcSource
        << "|lof1=" << config.lnbLof1KHz
        << "|lof2=" << config.lnbLof2KHz
        << "|slof=" << config.lnbSlofKHz
        << "|isi=" << config.streamId;
    return out.str();
}

bool logCounter(std::uint64_t value) {
    return value <= 3 || (value != 0 && (value & (value - 1)) == 0);
}

} // namespace

struct SharedDvbInputPool::Impl {
    struct Subscriber {
        std::string streamId;
        DataCallback onData;
        FinishCallback onFinish;

        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::vector<std::uint8_t>> queue;
        std::size_t queuedBytes = 0;
        std::atomic<bool> stop{false};
        bool sourceFinished = false;
        std::string finishError;
        std::uint64_t droppedChunks = 0;
        std::thread worker;
    };

    struct Bus {
        std::string key;
        std::string frontend;
        LinuxDvbTuneConfig tune;
        LinuxDvbInput input;

        std::mutex mutex;
        std::map<std::string, std::shared_ptr<Subscriber>> subscribers;
        std::atomic<bool> stop{false};
        std::atomic<bool> running{false};
        std::string sourceError;
        std::thread reader;
    };

    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Bus>> buses;
    std::map<std::string, std::string> streamBus;
    std::map<std::string, std::string> frontendOwner;

    static void runSubscriber(const std::shared_ptr<Subscriber>& subscriber) {
        for (;;) {
            std::vector<std::uint8_t> chunk;
            bool finished = false;
            std::string finishError;
            {
                std::unique_lock<std::mutex> lock(subscriber->mutex);
                subscriber->condition.wait(lock, [&] {
                    return subscriber->stop.load(std::memory_order_acquire) ||
                        !subscriber->queue.empty() ||
                        subscriber->sourceFinished;
                });

                if (subscriber->stop.load(std::memory_order_acquire)) break;

                if (!subscriber->queue.empty()) {
                    chunk = std::move(subscriber->queue.front());
                    subscriber->queue.pop_front();
                    subscriber->queuedBytes -= chunk.size();
                } else if (subscriber->sourceFinished) {
                    finished = true;
                    finishError = subscriber->finishError;
                }
            }

            if (!chunk.empty()) {
                bool accepted = false;
                try {
                    accepted = subscriber->onData &&
                        subscriber->onData(chunk.data(), chunk.size());
                } catch (const std::exception& ex) {
                    std::cerr << "SHARED DVB subscriber callback failed stream="
                              << subscriber->streamId
                              << " error=" << ex.what() << std::endl;
                } catch (...) {
                    std::cerr << "SHARED DVB subscriber callback failed stream="
                              << subscriber->streamId
                              << " error=unknown exception" << std::endl;
                }
                if (!accepted) break;
                continue;
            }

            if (finished) {
                try {
                    if (subscriber->onFinish) subscriber->onFinish(finishError);
                } catch (...) {
                }
                break;
            }
        }

        std::lock_guard<std::mutex> lock(subscriber->mutex);
        subscriber->queue.clear();
        subscriber->queuedBytes = 0;
    }

    static void enqueue(
        const std::shared_ptr<Subscriber>& subscriber,
        const std::uint8_t* data,
        std::size_t size) {
        if (!subscriber || !data || size == 0 ||
            subscriber->stop.load(std::memory_order_acquire)) {
            return;
        }

        std::vector<std::uint8_t> chunk(data, data + size);
        bool dropped = false;
        std::uint64_t droppedCount = 0;
        {
            std::lock_guard<std::mutex> lock(subscriber->mutex);
            if (subscriber->stop.load(std::memory_order_acquire) ||
                subscriber->sourceFinished) {
                return;
            }

            while (!subscriber->queue.empty() &&
                   subscriber->queuedBytes + chunk.size() > kSubscriberQueueBytes) {
                subscriber->queuedBytes -= subscriber->queue.front().size();
                subscriber->queue.pop_front();
                ++subscriber->droppedChunks;
                dropped = true;
            }

            if (chunk.size() > kSubscriberQueueBytes) {
                ++subscriber->droppedChunks;
                dropped = true;
            } else {
                subscriber->queuedBytes += chunk.size();
                subscriber->queue.push_back(std::move(chunk));
            }
            droppedCount = subscriber->droppedChunks;
        }

        if (dropped && logCounter(droppedCount)) {
            std::cerr << "SHARED DVB subscriber queue drop stream="
                      << subscriber->streamId
                      << " count=" << droppedCount
                      << " max_bytes=" << kSubscriberQueueBytes
                      << std::endl;
        }
        subscriber->condition.notify_one();
    }

    static void finishSource(
        const std::shared_ptr<Bus>& bus,
        const std::string& error) {
        std::vector<std::shared_ptr<Subscriber>> subscribers;
        {
            std::lock_guard<std::mutex> lock(bus->mutex);
            bus->sourceError = error;
            for (const auto& [id, subscriber] : bus->subscribers) {
                (void)id;
                subscribers.push_back(subscriber);
            }
        }

        for (const auto& subscriber : subscribers) {
            {
                std::lock_guard<std::mutex> lock(subscriber->mutex);
                subscriber->sourceFinished = true;
                subscriber->finishError = error;
            }
            subscriber->condition.notify_one();
        }
    }

    static void runBus(const std::shared_ptr<Bus>& bus) {
        std::array<std::uint8_t, kReadChunkBytes> buffer {};
        std::string error;

        std::cerr << "SHARED DVB SOURCE running frontend=" << bus->frontend
                  << " key=" << bus->key << std::endl;

        while (!bus->stop.load(std::memory_order_acquire)) {
            std::size_t received = 0;
            if (!bus->input.read(
                    buffer.data(), buffer.size(), received, 250, error)) {
                if (bus->stop.load(std::memory_order_acquire)) break;
                if (error.empty()) error = "shared DVB input read failed";
                bus->running.store(false, std::memory_order_release);
                std::cerr << "SHARED DVB SOURCE failed frontend="
                          << bus->frontend
                          << " error=" << error << std::endl;
                finishSource(bus, error);
                break;
            }
            if (received == 0) continue;

            std::vector<std::shared_ptr<Subscriber>> subscribers;
            {
                std::lock_guard<std::mutex> lock(bus->mutex);
                subscribers.reserve(bus->subscribers.size());
                for (const auto& [id, subscriber] : bus->subscribers) {
                    (void)id;
                    subscribers.push_back(subscriber);
                }
            }
            for (const auto& subscriber : subscribers) {
                enqueue(subscriber, buffer.data(), received);
            }
        }

        bus->input.close();
        bus->running.store(false, std::memory_order_release);
        std::cerr << "SHARED DVB SOURCE stopped frontend="
                  << bus->frontend << std::endl;
    }

    static void stopSubscriber(
        const std::shared_ptr<Subscriber>& subscriber) noexcept {
        if (!subscriber) return;
        subscriber->stop.store(true, std::memory_order_release);
        subscriber->condition.notify_all();
        if (subscriber->worker.joinable()) {
            try {
                subscriber->worker.join();
            } catch (...) {
            }
        }
    }

    static void stopBus(const std::shared_ptr<Bus>& bus) noexcept {
        if (!bus) return;
        bus->stop.store(true, std::memory_order_release);
        if (bus->reader.joinable()) {
            try {
                bus->reader.join();
            } catch (...) {
            }
        }
        bus->input.close();
        bus->running.store(false, std::memory_order_release);
    }

    bool subscribe(
        const std::string& streamId,
        const LinuxDvbTuneConfig& requested,
        DataCallback onData,
        FinishCallback onFinish,
        std::string& error) {
        error.clear();
        if (streamId.empty() || !onData) {
            error = "shared DVB subscription requires stream id and data callback";
            return false;
        }

        LinuxDvbTuneConfig tune = requested;
        tune.pids = "8192";
        const std::string key = transponderKey(tune);
        const std::string frontend = frontendKey(tune);

        std::shared_ptr<Bus> bus;
        bool createdBus = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (streamBus.count(streamId)) {
                error = "stream already has a shared DVB subscription";
                return false;
            }

            const auto owner = frontendOwner.find(frontend);
            if (owner != frontendOwner.end() && owner->second != key) {
                error =
                    "DVB frontend adapter" + std::to_string(tune.adapter) +
                    "/frontend" + std::to_string(tune.frontend) +
                    " is already tuned to another transponder";
                return false;
            }

            const auto existing = buses.find(key);
            if (existing != buses.end()) {
                bus = existing->second;
                if (!bus || !bus->running.load(std::memory_order_acquire)) {
                    if (bus) {
                        std::lock_guard<std::mutex> busLock(bus->mutex);
                        error = !bus->sourceError.empty()
                            ? bus->sourceError
                            : "shared DVB source is not running";
                    } else {
                        error = "shared DVB source is not running";
                    }
                    return false;
                }
            } else {
                bus = std::make_shared<Bus>();
                bus->key = key;
                bus->frontend = frontend;
                bus->tune = tune;

                if (!bus->input.open(tune, error)) {
                    return false;
                }

                bus->running.store(true, std::memory_order_release);
                try {
                    bus->reader = std::thread([bus] { runBus(bus); });
                } catch (const std::exception& ex) {
                    bus->running.store(false, std::memory_order_release);
                    bus->input.close();
                    error = std::string("cannot start shared DVB source thread: ") + ex.what();
                    return false;
                }

                buses[key] = bus;
                frontendOwner[frontend] = key;
                createdBus = true;
            }

            auto subscriber = std::make_shared<Subscriber>();
            subscriber->streamId = streamId;
            subscriber->onData = std::move(onData);
            subscriber->onFinish = std::move(onFinish);

            {
                std::lock_guard<std::mutex> busLock(bus->mutex);
                bus->subscribers[streamId] = subscriber;
                if (!bus->running.load(std::memory_order_acquire)) {
                    subscriber->sourceFinished = true;
                    subscriber->finishError = bus->sourceError.empty()
                        ? "shared DVB source stopped"
                        : bus->sourceError;
                }
            }

            try {
                subscriber->worker = std::thread([subscriber] {
                    runSubscriber(subscriber);
                });
            } catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> busLock(bus->mutex);
                    bus->subscribers.erase(streamId);
                }
                if (createdBus) {
                    buses.erase(key);
                    frontendOwner.erase(frontend);
                }
                if (createdBus) stopBus(bus);
                error = std::string("cannot start shared DVB subscriber thread: ") + ex.what();
                return false;
            }

            streamBus[streamId] = key;

            std::cerr << "SHARED DVB subscriber added stream=" << streamId
                      << " frontend=" << frontend
                      << " subscribers=";
            {
                std::lock_guard<std::mutex> busLock(bus->mutex);
                std::cerr << bus->subscribers.size();
            }
            std::cerr << std::endl;
        }

        return true;
    }

    void unsubscribe(const std::string& streamId) noexcept {
        std::shared_ptr<Bus> bus;
        std::shared_ptr<Subscriber> subscriber;
        bool stopSource = false;
        std::string key;
        std::string frontend;

        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto stream = streamBus.find(streamId);
            if (stream == streamBus.end()) return;

            key = stream->second;
            streamBus.erase(stream);

            const auto found = buses.find(key);
            if (found == buses.end()) return;
            bus = found->second;
            frontend = bus->frontend;

            {
                std::lock_guard<std::mutex> busLock(bus->mutex);
                const auto item = bus->subscribers.find(streamId);
                if (item != bus->subscribers.end()) {
                    subscriber = item->second;
                    bus->subscribers.erase(item);
                }
                stopSource = bus->subscribers.empty();
            }

            if (stopSource) {
                buses.erase(key);
                const auto owner = frontendOwner.find(frontend);
                if (owner != frontendOwner.end() && owner->second == key) {
                    frontendOwner.erase(owner);
                }
            }
        }

        stopSubscriber(subscriber);

        std::size_t remaining = 0;
        if (bus) {
            std::lock_guard<std::mutex> busLock(bus->mutex);
            remaining = bus->subscribers.size();
        }
        std::cerr << "SHARED DVB subscriber removed stream=" << streamId
                  << " frontend=" << frontend
                  << " subscribers=" << remaining
                  << std::endl;

        if (stopSource) stopBus(bus);
    }

    void stopAll() noexcept {
        std::vector<std::shared_ptr<Bus>> allBuses;
        std::vector<std::shared_ptr<Subscriber>> allSubscribers;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& [key, bus] : buses) {
                (void)key;
                allBuses.push_back(bus);
                std::lock_guard<std::mutex> busLock(bus->mutex);
                for (const auto& [id, subscriber] : bus->subscribers) {
                    (void)id;
                    allSubscribers.push_back(subscriber);
                }
                bus->subscribers.clear();
            }
            buses.clear();
            streamBus.clear();
            frontendOwner.clear();
        }

        for (const auto& subscriber : allSubscribers) {
            stopSubscriber(subscriber);
        }
        for (const auto& bus : allBuses) {
            stopBus(bus);
        }
    }
};

SharedDvbInputPool::SharedDvbInputPool()
    : impl_(std::make_unique<Impl>()) {}

SharedDvbInputPool::~SharedDvbInputPool() {
    stopAll();
}

bool SharedDvbInputPool::subscribe(
    const std::string& streamId,
    const LinuxDvbTuneConfig& config,
    DataCallback onData,
    FinishCallback onFinish,
    std::string& error) {
    return impl_->subscribe(
        streamId, config, std::move(onData), std::move(onFinish), error);
}

void SharedDvbInputPool::unsubscribe(const std::string& streamId) noexcept {
    impl_->unsubscribe(streamId);
}

void SharedDvbInputPool::stopAll() noexcept {
    if (impl_) impl_->stopAll();
}

} // namespace dvbstreamer5::media::network
