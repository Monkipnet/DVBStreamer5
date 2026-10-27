#include "EpgDiskCache.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace dvbstreamer5::epgcache {
namespace {
constexpr std::size_t kMaximumLine = 16U * 1024U;
constexpr std::size_t kMaximumRecords = 400000U;
std::string hashed(const std::string& value) {
    // Cache paths are safe even for arbitrary channel names, URLs and IDs.
    std::uint64_t n = 14695981039346656037ULL;
    for (unsigned char c : value) n = (n ^ c) * 1099511628211ULL;
    const char* hex = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[static_cast<std::size_t>(i)] = hex[n & 15U];
        n >>= 4U;
    }
    return result;
}
std::string jsonCompact(const Json::Value& value) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["commentStyle"] = "None";
    return Json::writeString(w, value);
}
} // namespace

std::filesystem::path pathFor(const std::string& category,
                              const std::string& key) {
    const char* overridePath = std::getenv("DVBSTREAMER5_EPG_CACHE_DIR");
    const std::filesystem::path root =
        overridePath && *overridePath ? overridePath
                                     : "/var/lib/dvbstreamer5/epg";
    // Only compiled-in categories, not arbitrary path components.
    const std::string prefix = category == "xmltv" ? "xmltv-" : "dvb-";
    return root / (prefix + hashed(category + ":" + key) + ".jsonl.gz");
}

Writer::Writer(const std::filesystem::path& path): finalPath_(path) {
    std::error_code ec;
    std::filesystem::create_directories(finalPath_.parent_path(), ec);
    if (ec) {
        std::clog << "EPG CACHE mkdir failed: " << ec.message() << '\n';
        return;
    }
    std::string pattern = finalPath_.string() + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) {
        std::clog << "EPG CACHE temp create failed: " << std::strerror(errno) << '\n';
        return;
    }
    temporaryPath_ = name.data();
    (void)::fchmod(fd, 0600);
    file_ = ::gzdopen(fd, "wb3");
    if (!file_) {
        ::close(fd);
        std::filesystem::remove(temporaryPath_, ec);
        temporaryPath_.clear();
    }
}
Writer::~Writer() {
    if (file_) gzclose(file_);
    if (!finished_ && !temporaryPath_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath_, ignored);
    }
}
bool Writer::append(const Json::Value& item) {
    if (!file_) return false;
    const auto line = jsonCompact(item);
    if (line.size() > kMaximumLine - 2U) return false;
    if (gzwrite(file_, line.data(), static_cast<unsigned>(line.size())) !=
        static_cast<int>(line.size())) return false;
    return gzwrite(file_, "\n", 1) == 1;
}
bool Writer::finish() {
    if (!file_ || finished_) return false;
    const int result = gzclose(file_);
    file_ = nullptr;
    if (result != Z_OK) return false;
    std::error_code ec;
    std::filesystem::rename(temporaryPath_, finalPath_, ec);
    if (ec) {
        std::clog << "EPG CACHE atomic rename failed: " << ec.message() << '\n';
        return false;
    }
    finished_ = true;
    return true;
}
Reader::Reader(const std::filesystem::path& path) {
    file_ = gzopen(path.c_str(), "rb");
}
Reader::~Reader() {
    if (file_) gzclose(file_);
}
bool Reader::next(Json::Value& item) {
    if (!file_ || !good_) return false;
    if (++records_ > kMaximumRecords) {
        good_ = false;
        return false;
    }
    std::array<char,kMaximumLine> buf{};
    if (!gzgets(file_,buf.data(),static_cast<int>(buf.size()))) {
        int status = Z_OK;
        (void)gzerror(file_, &status);
        if (status != Z_OK && status != Z_STREAM_END) good_ = false;
        return false;
    }
    std::string line(buf.data());
    if (line.empty() || line.back() != '\n') { good_ = false; return false; }
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> parser(builder.newCharReader());
    if (!parser->parse(line.data(),line.data()+line.size(),&item,&errors)) {
        good_ = false;
        return false;
    }
    return true;
}
} // namespace dvbstreamer5::epgcache
