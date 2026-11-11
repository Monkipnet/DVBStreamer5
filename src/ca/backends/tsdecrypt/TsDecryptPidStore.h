#pragma once
// A bounded, non-secret cache of video-PES-confirmed ECM PIDs.
// No CW/ECM packets, CAMD login, DES key or user password is ever persisted.
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cctype>
#include <memory>
#include <utility>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace dvbstreamer5::ca::tsdecrypt_detail {

inline std::string pidStoreKey(const std::string& streamId, uint32_t sid,
                               const std::string& readerKey,
                               const std::string& server,
                               const std::string& caid,
                               const std::string& protocol) {
    // A fingerprint, not the reader address or credentials, is written to disk.
    // Length-prefixed fields keep the tuple unambiguous.
    uint64_t hash = 14695981039346656037ULL;
    const auto part = [&hash](const std::string& value) {
        const std::string framed = std::to_string(value.size()) + ":" + value;
        for (unsigned char ch : framed) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
    };
    part(streamId);
    part(std::to_string(sid));
    part(readerKey);
    part(server);
    std::string upperCaid = caid;
    std::transform(upperCaid.begin(), upperCaid.end(), upperCaid.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    part(upperCaid);
    part(protocol);
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

class PidStore final {
public:
    explicit PidStore(std::string filename =
        "/var/lib/dvbstreamer5/tsdecrypt-ecm-pids.json")
        : filename_(std::move(filename)) {
        load();
    }

    uint32_t lookup(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return 0;
        if (!fresh(it->second.time)) return 0;
        return it->second.pid;
    }

    void remember(const std::string& key, uint32_t pid) {
        if (pid < 1 || pid > 8190 || key.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::time_t now = std::time(nullptr);
        const auto it = entries_.find(key);
        // No high-frequency writes while the stream is decoded.
        if (it != entries_.end() && it->second.pid == pid &&
            now >= it->second.time && now - it->second.time < 12 * 3600)
            return;
        entries_[key] = {pid, now};
        prune();
        save();
    }

    void forget(const std::string& key, uint32_t suspectedPid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end() || it->second.pid != suspectedPid) return;
        entries_.erase(it);
        save();
    }

private:
    struct Entry { uint32_t pid; std::time_t time; };
    static constexpr std::size_t kMaxEntries = 256;
    static constexpr std::time_t kMaxAgeSeconds = 14 * 24 * 3600;
    std::string filename_;
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;

    static bool fresh(std::time_t when) {
        const std::time_t now = std::time(nullptr);
        return when > 0 && now >= when && now - when <= kMaxAgeSeconds;
    }

    void prune() {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!fresh(it->second.time)) it = entries_.erase(it);
            else ++it;
        }
        while (entries_.size() > kMaxEntries) {
            auto oldest = std::min_element(entries_.begin(), entries_.end(),
                [](const auto& a, const auto& b) { return a.second.time < b.second.time; });
            entries_.erase(oldest);
        }
    }

    void load() {
        std::ifstream input(filename_, std::ios::binary | std::ios::ate);
        if (!input) return;
        const auto size = input.tellg();
        if (size <= 0 || size > 128 * 1024) return;
        input.seekg(0);
        std::string payload(static_cast<std::size_t>(size), '\0');
        if (!input.read(&payload[0], static_cast<std::streamsize>(payload.size()))) return;
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string error;
        std::unique_ptr<Json::CharReader> parser(builder.newCharReader());
        if (!parser->parse(payload.data(), payload.data() + payload.size(),
                           &root, &error) || !root.isObject() ||
            root.get("version", 0).asInt() != 1) return;
        const auto& items = root["entries"];
        if (!items.isObject()) return;
        const auto names = items.getMemberNames();
        if (names.size() > kMaxEntries) return;
        for (const auto& key : names) {
            if (key.size() != 16 || !items[key].isObject()) continue;
            const auto& item = items[key];
            if (!item["pid"].isUInt() || !item["time"].isIntegral()) continue;
            const uint32_t pid = item["pid"].asUInt();
            const std::time_t when = static_cast<std::time_t>(item["time"].asInt64());
            if (pid > 0 && pid <= 8190 && fresh(when))
                entries_[key] = {pid, when};
        }
    }

    void save() {
        if (filename_ == "/var/lib/dvbstreamer5/tsdecrypt-ecm-pids.json" &&
            ::mkdir("/var/lib/dvbstreamer5", 0750) != 0 && errno != EEXIST)
            return;
        Json::Value root(Json::objectValue);
        root["version"] = 1;
        Json::Value items(Json::objectValue);
        for (const auto& pair : entries_) {
            Json::Value item(Json::objectValue);
            item["pid"] = Json::UInt(pair.second.pid);
            item["time"] = Json::Int64(pair.second.time);
            items[pair.first] = std::move(item);
        }
        root["entries"] = std::move(items);
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const std::string payload = Json::writeString(builder, root);
        const std::string tmp = filename_ + ".tmp";
        const int fd = ::open(tmp.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return; // Runtime in-memory cache still works.
        (void)::fchmod(fd, 0600);
        std::size_t offset = 0;
        bool ok = true;
        while (offset < payload.size()) {
            const ssize_t wrote = ::write(fd, payload.data() + offset,
                                          payload.size() - offset);
            if (wrote > 0) offset += static_cast<std::size_t>(wrote);
            else if (wrote < 0 && errno == EINTR) continue;
            else { ok = false; break; }
        }
        if (ok && ::fsync(fd) != 0) ok = false;
        if (::close(fd) != 0) ok = false;
        if (ok && ::rename(tmp.c_str(), filename_.c_str()) == 0) return;
        (void)::unlink(tmp.c_str());
    }
};

} // namespace dvbstreamer5::ca::tsdecrypt_detail
