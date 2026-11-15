#pragma once

// V536: cluster control-plane configuration, separate from programme and CAM
// settings. Never put peer secrets in /api/state, exported presets or logs.
// Stored only in cluster-peers.json (mode 0600); no cross-node stream control.
#include <jsoncpp/json/json.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dvbstreamer5::cluster {

struct Peer {
    std::string name;
    std::string url;   // An origin, without path/query/credentials.
    std::string token; // Inbound token generated on the remote node.
};

struct Config {
    std::string nodeToken;
    std::vector<Peer> peers;
};

inline bool validToken(const std::string& token) {
    if (token.size() != 64) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline std::string randomToken() {
    std::array<unsigned char,32> bytes{};
    if (RAND_bytes(bytes.data(),static_cast<int>(bytes.size())) != 1) return {};
    static constexpr char hex[]="0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (auto byte:bytes) {token.push_back(hex[byte>>4]);token.push_back(hex[byte&15]);}
    return token;
}

inline bool tokenMatches(const std::string& actual,const std::string& received) {
    return validToken(actual) && validToken(received) &&
        CRYPTO_memcmp(actual.data(),received.data(),64)==0;
}

// Do not allow a remotely configured peer URL to query arbitrary services.
// HTTP is accepted ONLY for private RFC1918 IPv4 nodes (typically WireGuard/LAN).
// For domain names or public networks HTTPS is mandatory and TLS verified.
// The port is optional. No username, path, query, fragment or redirects.
inline bool validOrigin(const std::string& url) {
    if(url.size()>250 || url.find_first_of("\r\n\t@?#")!=std::string::npos)return false;
    static const std::regex origin(
        R"(^(https?)://([A-Za-z0-9][A-Za-z0-9.-]{0,190}|\[[0-9a-fA-F:]+\])(?::([0-9]{1,5}))?/?$)");
    std::smatch parts;
    if(!std::regex_match(url,parts,origin))return false;
    if(parts[3].matched){
        int port=0;
        try{port=std::stoi(parts[3].str());}catch(...){return false;}
        if(port<1 || port>65535)return false;
    }
    if(parts[1]=="https")return true;
    const std::string host=parts[2].str();
    static const std::regex ipv4(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
    std::smatch ip;
    if(!std::regex_match(host,ip,ipv4))return false;
    std::array<int,4> octets{};
    for(int i=0;i<4;++i){
        octets[i]=std::stoi(ip[i+1].str());
        if(octets[i]>255)return false;
    }
    return octets[0]==10 || (octets[0]==172 && octets[1]>=16 && octets[1]<=31) ||
        (octets[0]==192 && octets[1]==168);
}

inline bool validPeer(const Peer& peer) {
    return !peer.name.empty() && peer.name.size()<=80 &&
           peer.name.find_first_of("\r\n\t") == std::string::npos &&
           validOrigin(peer.url) && validToken(peer.token);
}
inline std::filesystem::path filePath(const std::filesystem::path& configDirectory) {
    return configDirectory/"cluster-peers.json";
}
inline bool load(const std::filesystem::path& configDirectory,Config& result) {
    result={};
    const auto file=filePath(configDirectory);
    std::error_code ec;
    if(!std::filesystem::exists(file,ec))return true;
    // Do not follow a symlink or read a world-readable secret file.
    const int fd=::open(file.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return false;
    struct stat st{};
    if(::fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0 ||
        st.st_size>65536 || (st.st_mode&0077)!=0){
        ::close(fd);return false;
    }
    std::string json(static_cast<std::size_t>(st.st_size),'\0');
    std::size_t done=0;
    while(done<json.size()){
        const auto n=::read(fd,json.data()+done,json.size()-done);
        if(n<=0){::close(fd);return false;}
        done+=static_cast<std::size_t>(n);
    }
    ::close(fd);
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string error;
    std::istringstream input(json);
    if(!Json::parseFromStream(reader,input,&root,&error)||!root.isObject())return false;
    result.nodeToken=root.get("node_token","").asString();
    if(!validToken(result.nodeToken))return false;
    if(root.isMember("peers")&&!root["peers"].isArray())return false;
    std::set<std::string> addresses;
    for(const auto& item:root["peers"]){
        if(!item.isObject()||result.peers.size()>=8)return false;
        Peer peer{item.get("name","").asString(),item.get("url","").asString(),
                  item.get("token","").asString()};
        if(!validPeer(peer)||!addresses.insert(peer.url).second)return false;
        result.peers.push_back(std::move(peer));
    }
    return true;
}
inline bool save(const std::filesystem::path& directory,const Config& config) {
    if(!validToken(config.nodeToken)||config.peers.size()>8)return false;
    Json::Value root(Json::objectValue),peers(Json::arrayValue);
    root["node_token"]=config.nodeToken;
    std::set<std::string> urls;
    for(const auto& peer:config.peers){
        if(!validPeer(peer)||!urls.insert(peer.url).second)return false;
        Json::Value item(Json::objectValue);
        item["name"]=peer.name;item["url"]=peer.url;item["token"]=peer.token;
        peers.append(item);
    }
    root["peers"]=peers;
    Json::StreamWriterBuilder writer;
    writer["indentation"]="  ";
    const std::string data=Json::writeString(writer,root);
    const auto target=filePath(directory);
    std::string templ=target.string()+".tmp-XXXXXX";
    std::vector<char> name(templ.begin(),templ.end());
    name.push_back('\0');
    const int fd=::mkstemp(name.data());
    if(fd<0)return false;
    const std::filesystem::path temp(name.data());
    bool ok=::fchmod(fd,0600)==0;
    std::size_t done=0;
    while(ok && done<data.size()){
        const auto n=::write(fd,data.data()+done,data.size()-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){ok=false;break;}
        done+=static_cast<std::size_t>(n);
    }
    if(ok)ok=::fsync(fd)==0;
    if(::close(fd)!=0)ok=false;
    if(ok)ok=::rename(temp.c_str(),target.c_str())==0;
    if(!ok)::unlink(temp.c_str());
    return ok;
}
inline bool ensure(const std::filesystem::path& dir,Config& result) {
    if(!load(dir,result))return false;
    if(validToken(result.nodeToken))return true;
    result.nodeToken=randomToken();
    return !result.nodeToken.empty()&&save(dir,result);
}
inline Json::Value publicPeers(const Config& config,bool includeLocalToken) {
    Json::Value result(Json::objectValue),peers(Json::arrayValue);
    if(includeLocalToken)result["node_token"]=config.nodeToken;
    for(const auto& peer:config.peers){
        Json::Value item(Json::objectValue);
        item["name"]=peer.name;
        item["url"]=peer.url; // No stored remote token in browser responses.
        peers.append(std::move(item));
    }
    result["peers"]=peers;
    return result;
}
} // namespace dvbstreamer5::cluster
