#include "media/HlsFileCache.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
int main(){
    using dvbstreamer5::media::hls::HlsFileCache;
    char path[]="/tmp/dvbstreamer5-hls-cache-XXXXXX";
    const int fd=::mkstemp(path);
    assert(fd>=0);
    ::close(fd);
    const std::filesystem::path file(path);
    {
        std::ofstream output(file);
        output<<"#EXTM3U\nsegment1.ts\n";
    }
    std::string first,second;
    assert(HlsFileCache::instance().readSmallFile(file,first));
    assert(first=="#EXTM3U\nsegment1.ts\n");
    assert(HlsFileCache::instance().readSmallFile(file,second));
    assert(first==second);
    {
        std::ofstream output(file,std::ios::trunc);
        output<<"#EXTM3U\nsegment2.ts\n";
    }
    std::error_code ec;
    const auto stamp=std::filesystem::last_write_time(file,ec);
    assert(!ec);
    std::filesystem::last_write_time(file,stamp+std::chrono::seconds(2),ec);
    assert(!ec);
    assert(HlsFileCache::instance().readSmallFile(file,second));
    assert(second=="#EXTM3U\nsegment2.ts\n");
    assert(HlsFileCache::instance().entries()<=HlsFileCache::kMaxEntries);
    std::filesystem::remove(file);
    std::cout<<"HLS bounded file cache: OK\n";
}
