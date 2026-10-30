#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace dvbstreamer5::ca::newcamd {

// No CW or ECM bytes are cached. Only a CHID that yielded a successful
// CW is retained for short service stop/start cycles (e.g. OnDemand).
class IrdetoChidPreferenceCache {
public:
    struct Key {
        std::string reader;
        std::string stream;
        std::uint16_t sid = 0;
        std::uint16_t caid = 0;
        std::uint32_t provid = 0;

        bool operator<(const Key& rhs) const noexcept {
            return std::tie(reader, stream, sid, caid, provid) <
                   std::tie(rhs.reader, rhs.stream, rhs.sid, rhs.caid, rhs.provid);
        }
    };

    static constexpr std::uint64_t kTtlMs = 10 * 60 * 1000;
    static constexpr std::size_t kMaxEntries = 128;
    static constexpr std::uint64_t kRestoreGraceMs = 4000;

    void remember(const Key& key, std::uint16_t chid, std::uint64_t nowMs) {
        if (chid == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[key] = {chid, nowMs};
        if (entries_.size() <= kMaxEntries) return;
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.updatedAtMs < oldest->second.updatedAtMs) oldest = it;
        }
        entries_.erase(oldest);
    }

    bool recover(const Key& key, std::uint64_t nowMs, std::uint16_t& chidOut) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        if (nowMs < it->second.updatedAtMs ||
            nowMs - it->second.updatedAtMs > kTtlMs) {
            entries_.erase(it);
            return false;
        }
        chidOut = it->second.chid;
        return true;
    }

    void forgetIfMatches(const Key& key, std::uint16_t chid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);
        if (it != entries_.end() && it->second.chid == chid) entries_.erase(it);
    }

private:
    struct Entry {
        std::uint16_t chid = 0;
        std::uint64_t updatedAtMs = 0;
    };
    std::mutex mutex_;
    std::map<Key, Entry> entries_;
};

} // namespace dvbstreamer5::ca::newcamd
