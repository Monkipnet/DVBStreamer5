#pragma once
// V539: update an existing DVB-S/S2 transponder without dropping any
// service-specific URI keys, PNR/PID filters or hand-edited metadata.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dvbstreamer5::dvb_tp_editor {
inline std::map<std::string,std::string> queryParts(const std::string& uri) {
    std::map<std::string,std::string> result;
    const auto q=uri.find('?');
    if(q==std::string::npos)return result;
    std::size_t pos=q+1;
    while(pos<uri.size()){
        const auto end=uri.find('&',pos);
        const auto part=uri.substr(pos,end==std::string::npos?std::string::npos:end-pos);
        const auto eq=part.find('=');
        if(eq!=std::string::npos && eq>0) result[part.substr(0,eq)]=part.substr(eq+1);
        if(end==std::string::npos)break;
        pos=end+1;
    }
    return result;
}
inline std::string encode(const std::string& raw) {
    std::string result;
    const char hex[]="0123456789ABCDEF";
    for(unsigned char c:raw){
        if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')
            result.push_back(static_cast<char>(c));
        else {
            result.push_back('%');result.push_back(hex[c>>4]);result.push_back(hex[c&15]);
        }
    }
    return result;
}
inline std::string editUri(const std::string& original,
                           const std::string& tunedUri,
                           const std::string& satelliteName) {
    const auto question=original.find('?');
    if(question==std::string::npos)return {};
    std::string base=original.substr(0,question);
    auto query=queryParts(original);
    const auto replacement=queryParts(tunedUri);
    // Do not replace pids/jitter_ms, or unknown per-channel extensions.
    static constexpr const char* keys[]={
      "frequency_khz","symbol_rate","polarity","delivery_system",
      "modulation","fec","diseqc_source","lnb_lof1_khz","lnb_lof2_khz",
      "lnb_slof_khz","unicable_mode","unicable_user_band",
      "unicable_frequency_khz","unicable_position","unicable_pin","stream_id"
    };
    for(const char* key:keys){
        const auto it=replacement.find(key);
        if(it!=replacement.end())query[key]=it->second;
    }
    if(satelliteName.empty())query.erase("satellite_name");
    else query["satellite_name"]=encode(satelliteName);
    std::ostringstream result;
    result<<base<<"?";
    bool first=true;
    for(const auto& pair:query){
        if(!first)result<<"&";
        result<<pair.first<<"="<<pair.second;
        first=false;
    }
    return result.str();
}
} // namespace dvbstreamer5::dvb_tp_editor
