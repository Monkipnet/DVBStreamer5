#pragma once
// V10.8.532: bounded, non-executing Cesbo Astra JSON tuner references.
// Uses the same native DVB URI model as the 4.4.18x Lua converter.
// No CAM secrets, foreign panel/users or system settings are imported.
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iconv.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dvbstreamer5::external_config::astra_json {
inline void warnOnce(std::vector<std::string>& warnings, const std::string& message) {
    if (std::find(warnings.begin(), warnings.end(), message) == warnings.end())
        warnings.push_back(message);
}

inline bool validUtf8(const std::string& s) {
    for (size_t i=0; i<s.size();) {
        const unsigned char a=static_cast<unsigned char>(s[i]);
        if (a<0x80U) {++i;continue;}
        const size_t n=a>=0xC2U&&a<=0xDFU ? 2U :
                       a>=0xE0U&&a<=0xEFU ? 3U :
                       a>=0xF0U&&a<=0xF4U ? 4U : 0U;
        if(n==0U || i+n>s.size())return false;
        for(size_t k=1;k<n;++k)
            if((static_cast<unsigned char>(s[i+k])&0xC0U)!=0x80U)return false;
        const auto b=static_cast<unsigned char>(s[i+1]);
        if((a==0xE0U&&b<0xA0U)||(a==0xEDU&&b>=0xA0U)||
           (a==0xF0U&&b<0x90U)||(a==0xF4U&&b>=0x90U))return false;
        i+=n;
    }
    return true;
}

inline bool jsonUtf8(const std::string& source, std::string& output,
                     std::vector<std::string>& warnings) {
    if(validUtf8(source)) {output=source;return true;}
    // Astra 4.x installations commonly export Windows-1251 JSON containing
    // literal, non-UTF8 Cyrillic. iconv is libc on supported Linux targets.
    // Never execute or call a shell command to repair imported configuration.
    iconv_t cv=iconv_open("UTF-8","WINDOWS-1251");
    if(cv==reinterpret_cast<iconv_t>(-1))return false;
    output.resize(source.size()*4U+4U);
    char* input=const_cast<char*>(source.data());
    size_t inputLeft=source.size(),outputLeft=output.size();
    char* out=&output[0];
    const size_t rc=iconv(cv,&input,&inputLeft,&out,&outputLeft);
    iconv_close(cv);
    if(rc==static_cast<size_t>(-1)||inputLeft!=0)return false;
    output.resize(output.size()-outputLeft);
    warnOnce(warnings,"Astra JSON source decoded from Windows-1251 to UTF-8");
    return true;
}

inline bool real(const Json::Value& obj, const char* key,
                 double& n, double low, double high) {
    const auto& v=obj[key];
    if(v.isNull()||v.isBool())return false;
    try {
        if(v.isDouble() || v.isInt64() || v.isUInt64()) n=v.asDouble();
        else if(v.isString()) {
            const auto s=v.asString();
            if(s.empty()||s.size()>40)return false;
            size_t used=0;
            n=std::stod(s,&used);
            if(used!=s.size())return false;
        }else return false;
    }catch(...){return false;}
    return std::isfinite(n)&&n>=low&&n<=high;
}

inline bool integer(const Json::Value& obj, const char* key,
                    int& n, int low, int high) {
    double x=0;
    if(!real(obj,key,x,static_cast<double>(low),static_cast<double>(high))||
       std::floor(x)!=x)return false;
    n=static_cast<int>(x);
    return true;
}

inline std::string string(const Json::Value& obj,const char* key) {
    return obj[key].isString() ? obj[key].asString() : std::string{};
}

inline bool validReference(const std::string& id) {
    return !id.empty() && id.size()<=64U &&
       std::all_of(id.begin(),id.end(),[](unsigned char c){
           return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                  (c>='0'&&c<='9')||c=='_'||c=='-';
       });
}

inline std::map<std::string, Json::Value> tunerIndex(
    const Json::Value& root, std::vector<std::string>& warnings) {
    std::map<std::string,Json::Value> tunes;
    const auto& a=root["dvb_tune"];
    if(a.isNull())return tunes;
    if(!a.isArray()) {
        warnOnce(warnings,"Astra JSON dvb_tune is not an array; DVB references skipped");
        return tunes;
    }
    for(const auto& t:a) {
        if(tunes.size()>=128U){
            warnOnce(warnings,"Astra JSON tuner limit 128 reached");
            break;
        }
        if(!t.isObject()||!t["id"].isString()||
           !validReference(t["id"].asString())) {
            warnOnce(warnings,"Astra JSON invalid tuner ID skipped");
            continue;
        }
        const std::string id=t["id"].asString();
        if(!tunes.emplace(id,t).second)
            warnOnce(warnings,"Astra JSON duplicate tuner ID skipped");
    }
    return tunes;
}

inline std::map<std::string,std::string> fragmentOptions(const std::string& s) {
    std::map<std::string,std::string> out;
    size_t start=0;
    while(start<s.size()){
        const auto end=s.find('&',start);
        const std::string part=s.substr(start,
            end==std::string::npos?std::string::npos:end-start);
        const auto eq=part.find('=');
        if(eq!=std::string::npos)
            out[part.substr(0,eq)]=part.substr(eq+1);
        else if(!part.empty())out[part]="";
        if(end==std::string::npos)break;
        start=end+1;
    }
    return out;
}

inline std::string convertDvb(const std::string& source,
    const std::map<std::string,Json::Value>& tunes, uint32_t& pnr,
    std::vector<std::string>& warnings) {
    pnr=0;
    if(source.size()>8192U || (source.rfind("dvb://",0)!=0 && source.rfind("t2mi://",0)!=0))return {};
    std::string reference=source.substr(6);
    const auto hash=reference.find('#');
    std::map<std::string,std::string> options;
    if(hash!=std::string::npos){
        options=fragmentOptions(reference.substr(hash+1));
        reference.resize(hash);
    }
    if(!validReference(reference)){
        warnOnce(warnings,"Astra JSON malformed DVB tuner reference skipped");
        return {};
    }
    const auto it=tunes.find(reference);
    if(it==tunes.end()){
        warnOnce(warnings,"Astra JSON DVB tuner reference missing: "+reference);
        return {};
    }
    if(const auto pi=options.find("pnr");pi!=options.end()){
        try {
            size_t used=0;const auto n=std::stoul(pi->second,&used);
            if(used!=pi->second.size()||n<1U||n>65535U)throw 1;
            pnr=static_cast<uint32_t>(n);
        }catch(...){
            warnOnce(warnings,"Astra JSON invalid DVB PNR skipped");
            return {};
        }
    }
    if(options.count("cam")||options.count("biss")||options.count("ecm_pid"))
        warnOnce(warnings,"Astra JSON CA/CAM references not transferred; configure CA manually");

    const Json::Value& t=it->second;
    int adapter=0, frontend=0;
    if(!integer(t,"adapter",adapter,0,31) ||
       (t.isMember("device")&&!integer(t,"device",frontend,0,31))){
        warnOnce(warnings,"Astra JSON invalid DVB adapter/frontend skipped");
        return {};
    }
    std::string type=string(t,"type");
    std::transform(type.begin(),type.end(),type.begin(),
        [](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(type!="s"&&type!="s2"&&type!="t"&&type!="t2"){
        warnOnce(warnings,"Astra JSON unsupported DVB system skipped");
        return {};
    }
    double mhz=0;
    const bool satellite=type=="s"||type=="s2";
    if(!real(t,"frequency",mhz,satellite?900.0:42.0,
                                 satellite?14000.0:1002.0)){
        warnOnce(warnings,"Astra JSON invalid DVB frequency skipped");
        return {};
    }
    const auto freq=static_cast<uint32_t>(std::llround(mhz*1000.0));
    std::ostringstream uri;
    if(satellite){
        double symbol=0;
        if(!real(t,"symbolrate",symbol,100.0,60000.0)){
            warnOnce(warnings,"Astra JSON invalid satellite symbol rate skipped");
            return {};
        }
        const std::string polarity=string(t,"polarization");
        if(polarity.empty()||(polarity!="H"&&polarity!="V"&&
                              polarity!="h"&&polarity!="v")){
            warnOnce(warnings,"Astra JSON invalid satellite polarity skipped");
            return {};
        }
        // A guessed user band can disturb every service on a shared
        // Unicable line. Require an explicit operator-configured mapping.
        for(const char* field:{"uni_scr","uni_frequency","uni_pos"}){
            if(t.isMember(field)&&!t[field].isNull()&&
               !(t[field].isString()&&t[field].asString().empty())){
                warnOnce(warnings,"Astra JSON nonempty Unicable setting needs manual mapping; tuner skipped");
                return {};
            }
        }
        const int sym=static_cast<int>(std::llround(symbol));
        uri<<"dvb://satellite?adapter="<<adapter<<"&frontend="<<frontend
           <<"&frequency_khz="<<freq<<"&symbol_rate="<<sym
           <<"&polarity="<<static_cast<char>(std::toupper(static_cast<unsigned char>(polarity[0])))
           <<"&delivery_system=dvb-"<<type;
        for(const char* field:{"lof1","lof2","slof"}){
            if(!t.isMember(field))continue;
            double val=0;
            if(!real(t,field,val,0.0,14000.0)){
                warnOnce(warnings,"Astra JSON invalid LNB value; tuner skipped");
                return {};
            }
            const std::string param=std::string(field)=="slof"
                ?"lnb_slof_khz":(std::string("lnb_")+field+"_khz");
            uri<<"&"<<param<<"="<<static_cast<int>(std::llround(val*1000.0));
        }
        if(t.isMember("diseqc")){
            int diseqc=0;
            if(!integer(t,"diseqc",diseqc,0,15)){
                warnOnce(warnings,"Astra JSON invalid DiSEqC value; tuner skipped");
                return {};
            }
            uri<<"&diseqc_source="<<diseqc;
        }
        if(t.isMember("stream_id")){
            int isi=0;
            if(!integer(t,"stream_id",isi,0,255)){
                warnOnce(warnings,"Astra JSON invalid MIS/ISI value; tuner skipped");
                return {};
            }
            uri<<"&stream_id="<<isi;
        }
    }else{
        uint32_t bandwidth=8000000U;
        if(t.isMember("bandwidth")){
            std::string b=string(t,"bandwidth");
            std::transform(b.begin(),b.end(),b.begin(),
                [](unsigned char c){return static_cast<char>(std::tolower(c));});
            if(b=="8mhz"||b=="8")bandwidth=8000000U;
            else if(b=="7mhz"||b=="7")bandwidth=7000000U;
            else if(b=="6mhz"||b=="6")bandwidth=6000000U;
            else if(b=="5mhz"||b=="5")bandwidth=5000000U;
            else if(b=="10mhz"||b=="10")bandwidth=10000000U;
            else {
                warnOnce(warnings,"Astra JSON unsupported DVB-T/T2 bandwidth skipped");
                return {};
            }
        }
        int plp=-1;
        if(t.isMember("stream_id")||t.isMember("plp")){
            if(!integer(t,t.isMember("plp")?"plp":"stream_id",plp,0,255)||
               type=="t"){
                warnOnce(warnings,"Astra JSON invalid DVB-T/T2 PLP skipped");
                return {};
            }
        }
        uri<<"dvb://terrestrial?adapter="<<adapter<<"&frontend="<<frontend
           <<"&frequency_khz="<<freq<<"&bandwidth_hz="<<bandwidth
           <<"&delivery_system=dvb-"<<type
           <<"&modulation=auto&stream_id="<<plp;
    }
    const bool t2miSource=source.rfind("t2mi://",0)==0 ||
        options.count("t2mi_pid") || options.count("t2mi");
    if(t2miSource){
        if(!satellite){
            warnOnce(warnings,"Astra T2-MI virtual adapter must use a satellite DVB source");
            return {};
        }
        auto readOpt=[&](const char* key,int fallback,int min,int max,int& value)->bool{
            value=fallback;
            const auto it=options.find(key);
            if(it==options.end())return true;
            try {
                size_t used=0;
                const auto n=std::stol(it->second,&used);
                if(used!=it->second.size()||n<min||n>max)return false;
                value=static_cast<int>(n);
                return true;
            }catch(...){return false;}
        };
        int pid=4096,stream=0,plp=-1;
        if(!readOpt("t2mi_pid",4096,0,8190,pid)||
           !readOpt("t2mi",0,0,7,stream)||
           !readOpt("plp",-1,0,255,plp)){
            warnOnce(warnings,"Astra T2-MI PID, stream or PLP is invalid");
            return {};
        }
        uri<<"&t2mi_pid="<<pid<<"&t2mi_stream="<<stream;
        if(plp>=0)uri<<"&t2mi_plp="<<plp;
    }
    return uri.str();
}
} // namespace dvbstreamer5::external_config::astra_json
