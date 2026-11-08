#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dvbstreamer5::media::hls {

// Immutable raw HLS assets: never cache per-viewer tokenized playlist bodies.
// File size and mtime are checked on every request so renamed/rebuilt live
// playlists cannot leak stale media. Large segments use kernel page cache and
// sendfile(2), not this user-space buffer.
class HlsFileCache {
public:
    static constexpr std::uintmax_t kMaxFileBytes=512U*1024U;
    static constexpr std::size_t kMaxCacheBytes=64U*1024U*1024U;
    static constexpr std::size_t kMaxEntries=1024;
    static HlsFileCache& instance() {
        static HlsFileCache cache;
        return cache;
    }
    bool readSmallFile(const std::filesystem::path& path,std::string& output) {
        std::error_code ec;
        const auto bytes=std::filesystem::file_size(path,ec);
        if(ec||bytes>kMaxFileBytes)return false;
        const auto stamp=std::filesystem::last_write_time(path,ec);
        if(ec)return false;
        const auto key=path.string();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it=entries_.find(key);
            if(it!=entries_.end()&&it->second.bytes==bytes&&
               it->second.modified==stamp){
                it->second.lastUsed=++generation_;
                output=*it->second.data;
                return true;
            }
        }
        std::ifstream file(path,std::ios::binary);
        if(!file)return false;
        std::string contents(static_cast<std::size_t>(bytes),'\0');
        if(bytes && !file.read(contents.data(),static_cast<std::streamsize>(bytes)))
            return false;
        std::error_code verifyError;
        const auto stampAfter=std::filesystem::last_write_time(path,verifyError);
        if(verifyError||stampAfter!=stamp)return false;
        const auto sizeAfter=std::filesystem::file_size(path,verifyError);
        if(verifyError||sizeAfter!=bytes)return false;
        auto immutable=std::make_shared<const std::string>(std::move(contents));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto existing=entries_.find(key);
            if(existing!=entries_.end()){
                cachedBytes_-=existing->second.bytes;
                entries_.erase(existing);
            }
            entries_.emplace(key,Entry{immutable,bytes,stamp,++generation_});
            cachedBytes_+=static_cast<std::size_t>(bytes);
            while(cachedBytes_>kMaxCacheBytes||entries_.size()>kMaxEntries){
                const auto stale=std::min_element(entries_.begin(),entries_.end(),
                    [](const auto& a,const auto& b){
                        return a.second.lastUsed<b.second.lastUsed;
                    });
                if(stale==entries_.end())break;
                cachedBytes_-=stale->second.bytes;
                entries_.erase(stale);
            }
        }
        output=*immutable;
        return true;
    }
    std::size_t entries()const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }
private:
    struct Entry {
        std::shared_ptr<const std::string> data;
        std::uintmax_t bytes=0;
        std::filesystem::file_time_type modified{};
        std::uint64_t lastUsed=0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string,Entry> entries_;
    std::size_t cachedBytes_=0;
    std::uint64_t generation_=0;
};
} // namespace dvbstreamer5::media::hls
