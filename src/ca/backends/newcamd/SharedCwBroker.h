#pragma once
// V10.8.472: bounded, per-reader, exact-ECM control-word reuse.
// Never log keys or ECM payloads. CW lifetime intentionally short.
// This is not a universal channel key pool: mismatched CA/SID/PID/ECM
// contexts are NEVER interchangeable.
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

namespace dvbstreamer5::ca::newcamd {
class SharedCwBroker {
public:
    struct Key {
        std::string reader;
        uint16_t serviceId = 0;
        uint16_t caid = 0;
        uint32_t provid = 0;
        uint16_t ecmPid = 0;
        uint8_t ecmMode = 0;
        std::string ecmSignature; // exact, never logged

        bool operator<(const Key& rhs) const {
            return std::tie(reader, serviceId, caid, provid, ecmPid,
                            ecmMode, ecmSignature) <
                   std::tie(rhs.reader, rhs.serviceId, rhs.caid, rhs.provid,
                            rhs.ecmPid, rhs.ecmMode, rhs.ecmSignature);
        }
    };
    struct Pair {
        std::array<uint8_t, 8> even{};
        std::array<uint8_t, 8> odd{};
        void wipe() noexcept {
            volatile uint8_t* p = even.data();
            for (size_t i = 0; i < even.size(); ++i) p[i] = 0;
            p = odd.data();
            for (size_t i = 0; i < odd.size(); ++i) p[i] = 0;
        }
        ~Pair() { wipe(); }
        bool usable() const noexcept {
            uint8_t evenBits = 0, oddBits = 0;
            for (auto b : even) evenBits |= b;
            for (auto b : odd) oddBits |= b;
            return evenBits && oddBits;
        }
    };
    struct Stats {
        uint64_t hits = 0, misses = 0, stores = 0, expired = 0;
        uint64_t rejected = 0;
        size_t entries = 0;
    };
    static constexpr uint64_t kTtlMs = 1500;
    static constexpr size_t kMaxEntries = 128;

    ~SharedCwBroker() { clear(); }
    bool lookup(const Key& key, uint64_t nowMs, Pair& out) {
        std::lock_guard<std::mutex> guard(mutex_);
        expireLocked(nowMs);
        auto it = entries_.find(key);
        if (it == entries_.end()) { ++stats_.misses; return false; }
        out = it->second.value;
        ++stats_.hits;
        return true;
    }
    bool publish(const Key& key, uint64_t nowMs, const Pair& value) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!key.serviceId || !key.caid || !key.ecmPid ||
            key.reader.empty() || key.ecmSignature.empty() || !value.usable()) {
            ++stats_.rejected;
            return false;
        }
        expireLocked(nowMs);
        auto old = entries_.find(key);
        if (old != entries_.end()) {
            old->second.value.wipe();
            entries_.erase(old);
        }
        while (entries_.size() >= kMaxEntries) {
            auto earliest = entries_.begin();
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
                if (it->second.storedMs < earliest->second.storedMs) earliest = it;
            earliest->second.value.wipe();
            entries_.erase(earliest);
        }
        auto result = entries_.try_emplace(key);
        result.first->second.value = value;
        result.first->second.storedMs = nowMs;
        ++stats_.stores;
        return true;
    }
    void invalidateReader(const std::string& reader) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.reader != reader) { ++it; continue; }
            it->second.value.wipe();
            it = entries_.erase(it);
        }
    }
    Stats stats() const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto result = stats_;
        result.entries = entries_.size();
        return result;
    }
    void clear() {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& item : entries_) item.second.value.wipe();
        entries_.clear();
    }
private:
    struct Entry { Pair value; uint64_t storedMs = 0; };
    void expireLocked(uint64_t nowMs) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (nowMs < it->second.storedMs ||
                nowMs - it->second.storedMs >= kTtlMs) {
                it->second.value.wipe();
                it = entries_.erase(it);
                ++stats_.expired;
            } else ++it;
        }
    }
    mutable std::mutex mutex_;
    std::map<Key, Entry> entries_;
    Stats stats_;
};
} // namespace dvbstreamer5::ca::newcamd
