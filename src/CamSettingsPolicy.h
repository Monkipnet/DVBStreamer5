#pragma once
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>
namespace camsettings {
inline bool decode(const Json::Value& raw, Json::Value& out) {
    if (raw.isObject()) { out=raw; return true; }
    if (!raw.isString()) return false;
    Json::CharReaderBuilder reader; std::string errors; std::istringstream in(raw.asString());
    return Json::parseFromStream(reader,in,&out,&errors) && out.isObject();
}
inline std::string encode(const Json::Value& value) {
    Json::StreamWriterBuilder writer; writer["indentation"]="";
    return Json::writeString(writer,value);
}
inline void aliases(Json::Value& v) {
    for (const auto& names : {std::pair<const char*,const char*>{"user","username"},{"pass","password"},{"des","des_key"}}) {
        if (!v.isMember(names.first) && v.isMember(names.second)) v[names.first]=v[names.second];
        v.removeMember(names.second);
    }
}
inline bool hex(const std::string& value, size_t length) {
    return value.size()==length && std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isxdigit(c)!=0;});
}
inline bool bounded(const Json::Value& v,int lo,int hi) {
    return v.isInt() && v.asInt()>=lo && v.asInt()<=hi;
}
inline bool merge(const std::string& backend,const Json::Value& raw,const Json::Value& previous,Json::Value& out) {
    Json::Value patch;
    if (!decode(raw,patch)) return false;
    const bool network=backend=="newcamd" || backend=="tsdecrypt";
    if(network) aliases(patch);
    out=previous.isObject()?previous:Json::Value(Json::objectValue);
    if(network) aliases(out);
    for(const auto& key:patch.getMemberNames()) {
        if(network && key=="pass" && patch[key].isString() && patch[key].asString().empty() &&
           out.isMember("pass") && out["pass"].isString() && !out["pass"].asString().empty()) continue;
        out[key]=patch[key];
    }
    if(!network) return true;
    for(const auto* key:{"host","user","pass"}) if(!out[key].isString()||out[key].asString().empty()) return false;
    if(out["host"].asString().find_first_of(" \r\n\t")!=std::string::npos || !bounded(out["port"],1,65535)) return false;
    if(backend=="tsdecrypt") {
        if(!out.isMember("protocol")) out["protocol"]="NEWCAMD";
        if(!out["protocol"].isString()) return false;
        std::string protocol=out["protocol"].asString();
        std::transform(protocol.begin(),protocol.end(),protocol.begin(),[](unsigned char c){return std::toupper(c);});
        if(protocol!="NEWCAMD"&&protocol!="CS378X") return false;
        out["protocol"]=protocol;
        if(!out.isMember("caid")) out["caid"]="098D";
        if(!out["caid"].isString()||!hex(out["caid"].asString(),4)) return false;
        if(!out.isMember("ecm_pid")) out["ecm_pid"]=0;
        if(!bounded(out["ecm_pid"],0,8190)) return false;
        if(protocol=="CS378X") return true;
    }
    return out["des"].isString()&&hex(out["des"].asString(),28);
}
inline bool readerChanged(const std::string& oldBackend,const std::string& oldConfig,const std::string& newBackend,const std::string& newConfig) {
    if(oldBackend!=newBackend) return true;
    Json::Value a,b;
    if(decode(oldConfig,a)&&decode(newConfig,b)) return a!=b;
    return oldConfig!=newConfig;
}
}
