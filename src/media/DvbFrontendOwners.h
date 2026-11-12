#pragma once
// Read-only Linux DVB frontend owner diagnostics; no device opens, retunes or kills.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif
namespace dvbstreamer5::media::network {
struct ExternalDvbFrontendOwner { int pid=0; std::string name; bool writer=false; };
inline std::vector<ExternalDvbFrontendOwner> externalDvbFrontendOwners(
    int adapter, int frontend, const std::filesystem::path& procRoot="/proc",
    int ownPid=
#ifdef __linux__
    static_cast<int>(::getpid())
#else
    -1
#endif
) {
    std::vector<ExternalDvbFrontendOwner> owners;
#ifdef __linux__
    if (adapter < 0 || adapter > 255 || frontend < 0 || frontend > 255) return owners;
    const std::string expected = "/dev/dvb/adapter"+std::to_string(adapter)+
                                 "/frontend"+std::to_string(frontend);
    std::error_code ec;
    std::filesystem::directory_iterator procs(procRoot,
        std::filesystem::directory_options::skip_permission_denied,ec);
    if (ec) return owners;
    for (const auto& entry : procs) {
        const std::string pidText=entry.path().filename().string();
        if (pidText.empty() || pidText.size()>10 ||
            !std::all_of(pidText.begin(),pidText.end(),
                [](unsigned char c){return std::isdigit(c)!=0;})) continue;
        int pid=0;
        try {
            const auto parsed=std::stoull(pidText);
            if (parsed==0 || parsed>static_cast<unsigned long long>(
                std::numeric_limits<int>::max())) continue;
            pid=static_cast<int>(parsed);
        } catch (...) {continue;}
        if (pid==ownPid) continue;
        std::error_code fdEc;
        std::filesystem::directory_iterator fds(entry.path()/"fd",
            std::filesystem::directory_options::skip_permission_denied,fdEc);
        if (fdEc) continue;
        bool matched=false, writer=false;
        for (const auto& fd : fds) {
            std::error_code linkEc;
            const auto link=std::filesystem::read_symlink(fd.path(),linkEc);
            if (linkEc || link.string()!=expected) continue;
            matched=true;
            std::ifstream meta(entry.path()/"fdinfo"/fd.path().filename());
            std::string line;
            while (std::getline(meta,line)) {
                if (line.compare(0,6,"flags:")!=0) continue;
                try {
                    const auto access=std::stoul(line.substr(6),nullptr,8)&O_ACCMODE;
                    writer=writer || access==O_WRONLY || access==O_RDWR;
                } catch (...) {}
                break;
            }
        }
        if (!matched) continue;
        std::string name;
        std::ifstream comm(entry.path()/"comm");
        std::getline(comm,name);
        if (name.empty()) name="unknown";
        if (name.size()>48) name.resize(48);
        for (char& c:name) if (static_cast<unsigned char>(c)<32 ||
                               static_cast<unsigned char>(c)==127) c='_';
        owners.push_back({pid,name,writer});
        if (owners.size()>=64) break;
    }
    std::sort(owners.begin(),owners.end(),
        [](const auto& a,const auto& b){return a.pid<b.pid;});
#else
    (void)adapter;(void)frontend;(void)procRoot;(void)ownPid;
#endif
    return owners;
}
} // namespace dvbstreamer5::media::network
