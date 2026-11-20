#ifdef NDEBUG
#undef NDEBUG
#endif
#include "media/HlsTemporaryCleanup.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace dvbstreamer5::media::hls;
static void put(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "test";
}
int main() {
    std::error_code ec;
    const fs::path base=fs::temp_directory_path()/
        ("dvbstreamer5-hls-cleanup-v598-"+std::to_string(::getpid()));
    fs::remove_all(base,ec);
    const fs::path root=base/"live";
    const fs::path archive=base/"archive";
    fs::create_directories(root);
    const std::string id="sat-1794306945530-136-3";
    put(root/id/"video.m3u8");
    put(root/id/"segment00000001.ts");
    put(root/id/"abr"/"720p"/"segment00000002.ts");
    put(root/id/"master.m3u8");
    put(root/id/"segment00000003.m4s");
    put(root/id/"init00000001.mp4");
    put(root/"unrelated"/"segment00000010.ts");
    put(archive/id/"video.m3u8");
    assert(safeTemporaryHlsId(id));
    assert(!safeTemporaryHlsId("../bad"));
    assert(!safeTemporaryHlsId("foo/bar"));
    assert(!safeTemporaryHlsId(".."));
    assert(!safeTemporaryHlsId(""));
    assert(!safeTemporaryHlsId(std::string(181,'x')));
    auto trash=quarantineStoppedTemporaryHls(root,id,ec);
    assert(!ec && !trash.empty() && fs::exists(trash));
    assert(!fs::exists(root/id));
    // A new Start creates an independent generation at the original path.
    put(root/id/"segment99999999.ts");
    const auto removed=removeQuarantinedTemporaryHls(trash,ec);
    assert(!ec && removed >= 6);
    assert(!fs::exists(trash));
    assert(fs::exists(root/id/"segment99999999.ts"));
    assert(fs::exists(root/"unrelated"/"segment00000010.ts"));
    assert(fs::exists(archive/id/"video.m3u8"));
    // A symlink must never cause cleanup to traverse outside the HLS root.
    const fs::path outside=base/"outside";
    put(outside/"precious.txt");
    fs::remove_all(root/id,ec);
    fs::create_directory_symlink(outside,root/id);
    trash=quarantineStoppedTemporaryHls(root,id,ec);
    assert(trash.empty() && ec);
    assert(fs::exists(outside/"precious.txt"));
    fs::remove(root/id);
    const fs::path unsafe=base/"symlink-live";
    fs::create_directory_symlink(root,unsafe);
    trash=quarantineStoppedTemporaryHls(unsafe,id,ec);
    assert(trash.empty() && ec);
    trash=quarantineStoppedTemporaryHls(root,"../../outside",ec);
    assert(trash.empty() && ec);
    // Crash/legacy-tail sweep requires a completely ended playlist and
    // all files older than 24 h; fresh HLS is never eligible.
    const fs::path old=root/"sat-old-ended";
    put(old/"video.m3u8");
    put(old/"segment00000001.ts");
    {
        std::ofstream playlist(old/"video.m3u8",std::ios::trunc);
        playlist<<"#EXTM3U\n#EXTINF:4.0,\nsegment00000001.ts\n#EXT-X-ENDLIST\n";
    }
    assert(!isStaleCompletedTemporaryHls(old));
    const auto past=fs::file_time_type::clock::now()-std::chrono::hours(48);
    fs::last_write_time(old/"video.m3u8",past);
    fs::last_write_time(old/"segment00000001.ts",past);
    assert(isStaleCompletedTemporaryHls(old));
    put(old/"fresh.ts");
    assert(!isStaleCompletedTemporaryHls(old));
    fs::remove(old/"fresh.ts");
    {
        std::ofstream playlist(old/"video.m3u8",std::ios::trunc);
        playlist<<"#EXTM3U\n#EXTINF:4.0,\nsegment00000001.ts\n";
    }
    fs::last_write_time(old/"video.m3u8",past);
    assert(!isStaleCompletedTemporaryHls(old));
    fs::remove_all(base,ec);
    std::cout<<"PASS V598 HLS stop tail quarantine, ABR/CMAF, restart isolation, symlink safety\n";
}
