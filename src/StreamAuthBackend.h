#pragma once
#include "ConfigManager.h"
#include "NativeHttpClient.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
namespace dvbstreamer5::stream_auth {
inline bool enabled(const AppConfig& c) noexcept { return c.streamAuthBackendEnabled; }
inline bool enabled(const AppConfig& c, const std::string& channel) {
    for(const auto& stream:c.streams){
        if(stream.id!=channel)continue;
        if(stream.streamAuthMode=="on")return true;
        if(stream.streamAuthMode=="off")return false;
        break;
    }
    return c.streamAuthBackendEnabled;
}
inline bool safe(const std::string& x) {
    return x.size()<=2048 && x.find_first_of("\r\n")==std::string::npos;
}
inline std::string sessionId(const std::string& channel,const std::string& ip,
                             const std::string& query) {
    return std::to_string(static_cast<std::uint64_t>(
        std::hash<std::string>{}(channel+"\n"+ip+"\n"+query)));
}
inline bool authorize(const AppConfig& c,const std::string& channel,
                      const std::string& ip,const std::string& path,
                      const std::string& agent,const std::string& host,
                      const std::string& query={}) {
    if(!enabled(c,channel))return true;
    const auto& backend=c.streamAuthBackendUrl;
    if(backend.empty() || backend.size()>2048 ||
       (backend.rfind("https://",0)!=0 && backend.rfind("http://",0)!=0) ||
       backend.find('#')!=std::string::npos ||
       !safe(backend)||!safe(channel)||!safe(ip)||!safe(path)||
       !safe(agent)||!safe(host)||!safe(query)||
       query.find('#')!=std::string::npos)return false;
    std::string url=backend;
    if(!query.empty()){
        if(url.back()!='?' && url.back()!='&')
            url+=url.find('?')==std::string::npos?"?":"&";
        url+=query;
    }
    dvbstreamer5::http::RequestOptions opt;
    opt.connectTimeoutMs=std::clamp(c.streamAuthBackendTimeoutMs,100,3000);
    opt.readTimeoutMs=opt.connectTimeoutMs;
    opt.writeTimeoutMs=opt.connectTimeoutMs;
    opt.totalTimeoutMs=opt.connectTimeoutMs;
    opt.maxRedirects=0;opt.maxBodyBytes=4096;
    opt.userAgent="DVBStreamer5/10.8.379";
    opt.headers={{"X-Session-ID",sessionId(channel,ip,query)},
                 {"X-Channel-ID",channel},{"X-Real-IP",ip},
                 {"X-Real-Path",path},{"X-Real-UA",agent},
                 {"X-Real-Host",host}};
    dvbstreamer5::http::Response result;std::string error;
    return dvbstreamer5::http::get(url,opt,result,error) && result.status==200;
}
// The SRT listener supplies the peer's streamid. Forward its original
// streamid and common Cesbo-style token= fields to HTTP middleware.
inline std::string srtQuery(const std::string& streamid) {
    std::string query = "streamid=" +
        dvbstreamer5::http::encodeQueryComponent(streamid);
    auto text = streamid;
    if (text.rfind("#!::",0)==0) text.erase(0,4);
    if (!text.empty() && text[0]=='?') text.erase(0,1);
    std::size_t offset=0;
    while(offset<text.size()) {
        const auto end=text.find_first_of(",&",offset);
        const auto field=text.substr(offset,end==std::string::npos?
            std::string::npos:end-offset);
        const auto eq=field.find('=');
        if(eq!=std::string::npos) {
            const auto name=field.substr(0,eq);
            if(name=="token" || name=="access_token" || name=="auth")
                query += "&" + name + "=" +
                    dvbstreamer5::http::encodeQueryComponent(field.substr(eq+1));
        }
        if(end==std::string::npos)break;
        offset=end+1;
    }
    return query;
}
inline void propagateHlsQuery(std::string& playlist,const std::string& query){
    if(query.empty()||query.size()>2048)return;
    auto withQuery=[&](const std::string& uri){
        if(uri.empty()||uri[0]=='/'||uri[0]=='#'||
           uri.find("://")!=std::string::npos||uri.rfind("//",0)==0)
            return uri;
        return uri+(uri.find('?')==std::string::npos?"?":"&")+query;
    };
    std::string result;result.reserve(playlist.size()+256);
    std::size_t pos=0;
    while(pos<playlist.size()){
        const auto end=playlist.find('\n',pos);
        const bool nl=end!=std::string::npos;
        std::string line=playlist.substr(pos,nl?end-pos:std::string::npos);
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(!line.empty()&&line[0]!='#')line=withQuery(line);
        else if(line.rfind("#EXT-X-KEY:",0)==0||
                line.rfind("#EXT-X-SESSION-KEY:",0)==0||
                line.rfind("#EXT-X-MAP:",0)==0){
            const auto at=line.find("URI=\"");
            if(at!=std::string::npos){
                const auto begin=at+5,endUri=line.find('"',begin);
                if(endUri!=std::string::npos)
                    line.replace(begin,endUri-begin,withQuery(line.substr(begin,endUri-begin)));
            }
        }
        result+=line;if(nl)result+='\n';if(!nl)break;pos=end+1;
    }
    playlist.swap(result);
}
} // namespace dvbstreamer5::stream_auth
