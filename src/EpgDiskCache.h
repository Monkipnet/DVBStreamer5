#pragma once

#include <jsoncpp/json/json.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace dvbstreamer5::epgcache {

// Disk cache lives outside /opt/DVBStreamer5 and survives portable upgrades,
// systemd restarts and server reboots. An admin can override the path.
std::filesystem::path pathFor(const std::string& category, const std::string& key);

class Writer {
public:
    explicit Writer(const std::filesystem::path& path);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    bool valid() const { return file_ != nullptr; }
    bool append(const Json::Value& item);
    bool finish();
private:
    std::filesystem::path finalPath_;
    std::filesystem::path temporaryPath_;
    gzFile file_ = nullptr;
    bool finished_ = false;
};

class Reader {
public:
    explicit Reader(const std::filesystem::path& path);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    bool valid() const { return file_ != nullptr; }
    // Reads next valid JSON record; false at EOF or on invalid/truncated data.
    // Call good() to distinguish clean end-of-file from corruption.
    bool next(Json::Value& item);
    bool good() const { return good_; }
private:
    gzFile file_ = nullptr;
    bool good_ = true;
    std::size_t records_ = 0;
};
} // namespace dvbstreamer5::epgcache
