#pragma once
// Cesbo Astra 4.4.18x (not modern make_stream JSON) migration.
// Parse ONLY Lua literal dvb_tune({...}) and make_channel({...}) calls.
// Never evaluate/execute Lua, globals, variables or Newcamd credentials.
#include "ExternalConfigTransfer.h"
#include <jsoncpp/json/json.h>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace dvbstreamer5::external_config::astra44 {

struct Token {
    enum Kind { Id, String, Number, Punct, End } kind=End;
    std::string text;
};

inline bool tokenize(const std::string& src, std::vector<Token>& out, std::string& error) {
    out.clear();
    if (src.size() > 4U*1024U*1024U) { error="Astra 4.4 file exceeds 4 MiB"; return false; }
    size_t i=0;
    while (i<src.size()) {
        if (out.size()>250000) {error="Too many tokens in Lua file";return false;}
        const unsigned char c=static_cast<unsigned char>(src[i]);
        if (std::isspace(c)) {++i;continue;}
        if (i+1<src.size() && src[i]=='-' && src[i+1]=='-') {
            i+=2;
            if (i+1<src.size() && src[i]=='[' && src[i+1]=='[') {
                i+=2;
                const auto end=src.find("]]",i);
                if (end==std::string::npos) {error="Unterminated Lua block comment";return false;}
                i=end+2;
            } else {
                const auto end=src.find('\n',i);
                i=end==std::string::npos?src.size():end;
            }
            continue;
        }
        if (std::isalpha(c) || c=='_') {
            size_t start=i++;
            while (i<src.size()) {
                const unsigned char k=static_cast<unsigned char>(src[i]);
                if (!std::isalnum(k) && k!='_') break;
                ++i;
            }
            out.push_back({Token::Id,src.substr(start,i-start)});
            continue;
        }
        if (std::isdigit(c) || (c=='-' && i+1<src.size() &&
                               std::isdigit(static_cast<unsigned char>(src[i+1])))) {
            size_t start=i++;
            while (i<src.size() && std::isdigit(static_cast<unsigned char>(src[i])))++i;
            out.push_back({Token::Number,src.substr(start,i-start)});
            continue;
        }
        if (c=='"' || c=='\'') {
            const char quote=src[i++];
            std::string value;
            bool done=false;
            while(i<src.size()) {
                const char ch=src[i++];
                if(ch==quote) {done=true;break;}
                if(ch=='\n' || ch=='\r') {error="Newline in short Lua string";return false;}
                if(ch=='\\') {
                    if(i>=src.size()) break;
                    const char esc=src[i++];
                    switch(esc) {
                    case 'n':value.push_back('\n');break;
                    case 'r':value.push_back('\r');break;
                    case 't':value.push_back('\t');break;
                    case '\\':value.push_back('\\');break;
                    case '"':value.push_back('"');break;
                    case '\'':value.push_back('\'');break;
                    case 'z':
                        while(i<src.size() && std::isspace(static_cast<unsigned char>(src[i])))++i;
                        break;
                    default:
                        if (esc>='0' && esc<='9') {
                            unsigned n=static_cast<unsigned>(esc-'0');
                            for(int j=0;j<2 && i<src.size() &&
                                std::isdigit(static_cast<unsigned char>(src[i]));++j)
                                n=n*10+static_cast<unsigned>(src[i++]-'0');
                            if(n>255){error="Invalid Lua numeric escape";return false;}
                            value.push_back(static_cast<char>(n));
                        } else { error="Unsupported Lua string escape";return false; }
                    }
                } else value.push_back(ch);
                if(value.size()>8192){error="Lua string too long";return false;}
            }
            if(!done){error="Unterminated Lua string";return false;}
            out.push_back({Token::String,std::move(value)});
            continue;
        }
        // Preserve punctuation to detect expressions, but never execute it.
        out.push_back({Token::Punct,std::string(1,src[i++])});
    }
    out.push_back({Token::End,{}});
    return true;
}

class Literals {
public:
    explicit Literals(const std::vector<Token>& tokens):t_(tokens){}
    bool value(size_t& i,Json::Value& out,unsigned depth=0) const {
        if(depth>12 || i>=t_.size())return false;
        const auto tok=t_[i++];
        if(tok.kind==Token::String) {out=tok.text;return true;}
        if(tok.kind==Token::Number) {
            try {
                size_t used=0;
                const auto n=std::stoll(tok.text,&used,10);
                if(used!=tok.text.size())return false;
                out=Json::Int64(n);return true;
            } catch(...) {return false;}
        }
        if(tok.kind==Token::Id && tok.text=="true"){out=true;return true;}
        if(tok.kind==Token::Id && tok.text=="false"){out=false;return true;}
        if(tok.kind==Token::Id && tok.text=="nil"){out=Json::Value();return true;}
        if(tok.text!="{")return false;
        Json::Value obj(Json::objectValue), arr(Json::arrayValue);
        bool hasFields=false,hasValues=false;
        size_t fields=0;
        while(i<t_.size() && t_[i].text!="}") {
            if(++fields>512)return false;
            if(t_[i].text=="," || t_[i].text==";") {++i;continue;}
            std::string key;
            if(t_[i].kind==Token::Id && i+1<t_.size() && t_[i+1].text=="=") {
                key=t_[i].text; i+=2;
            } else if(t_[i].text=="[" && i+3<t_.size() &&
                      t_[i+1].kind==Token::String &&
                      t_[i+2].text=="]" && t_[i+3].text=="=") {
                key=t_[i+1].text;i+=4;
            }
            Json::Value nested;
            if(!value(i,nested,depth+1))return false;
            if(!key.empty()){hasFields=true;obj[key]=std::move(nested);}
            else{hasValues=true;arr.append(std::move(nested));}
            if(i>=t_.size())return false;
            if(t_[i].text=="," || t_[i].text==";")++i;
            else if(t_[i].text!="}")return false;
        }
        if(i>=t_.size() || t_[i].text!="}" || (hasFields && hasValues))
            return false;
        ++i;out=hasFields?obj:arr;
        return true;
    }
private:
    const std::vector<Token>& t_;
};

inline bool intField(const Json::Value& value,const char* field,int& output,
                     int minimum,int maximum) {
    const auto& n=value[field];
    if(n.isNull())return false;
    int64_t val=0;
    if(n.isInt64())val=n.asInt64();
    else if(n.isString()) {
        const std::string text=n.asString();
        try{size_t used=0;val=std::stoll(text,&used);if(used!=text.size())return false;}
        catch(...){return false;}
    } else return false;
    if(val<minimum || val>maximum)return false;
    output=static_cast<int>(val);
    return true;
}
inline std::string strField(const Json::Value& v,const char* key) {
    return v[key].isString()?v[key].asString():std::string{};
}
inline bool isSafeUri(const std::string& uri) {
    return uri.size()<8192 && uri.find_first_of("\r\n\0",0,2)==std::string::npos;
}
inline std::map<std::string,std::string> params(std::string p) {
    std::map<std::string,std::string> r;
    size_t at=0;
    while(at<p.size()) {
        const size_t stop=p.find('&',at);
        const auto part=p.substr(at,stop==std::string::npos?stop:stop-at);
        const auto eq=part.find('=');
        if(eq!=std::string::npos)
            r[lower(part.substr(0,eq))]=part.substr(eq+1);
        else if(!part.empty())r[lower(part)]="";
        if(stop==std::string::npos)break;
        at=stop+1;
    }
    return r;
}
inline std::vector<std::string> split(std::string s,char delimiter) {
    std::vector<std::string> out;
    size_t start=0;
    do {
        const auto pos=s.find(delimiter,start);
        out.push_back(s.substr(start,pos==std::string::npos?pos:pos-start));
        if(pos==std::string::npos)break;
        start=pos+1;
    }while(out.size()<8);
    return out;
}
inline bool number(const std::string& s,int& n,int min,int max) {
    try {
        size_t used=0;long v=std::stol(s,&used);
        if(used!=s.size()||v<min||v>max)return false;
        n=static_cast<int>(v);return true;
    }catch(...){return false;}
}

inline std::string convertDvb(const std::string& reference,
                              const std::map<std::string,Json::Value>& tunes,
                              uint32_t& sid,std::vector<std::string>& warnings) {
    // Legacy format: dvb://<dvb_tune-variable>#pnr=SID
    std::string name=reference.substr(6);
    const auto hash=name.find('#');
    const auto options=hash==std::string::npos?std::map<std::string,std::string>{}:
                       params(name.substr(hash+1));
    if(hash!=std::string::npos)name.resize(hash);
    const auto tune=tunes.find(name);
    if(tune==tunes.end()) {
        warnings.push_back("Astra 4.4: DVB adapter reference not found: "+safeId(name));
        return {};
    }
    const auto& c=tune->second;
    int adapter=0,frontend=0;
    if(!intField(c,"adapter",adapter,0,31) ||
       (c.isMember("device") && !intField(c,"device",frontend,0,31))) {
        warnings.push_back("Astra 4.4: invalid DVB adapter/frontend");
        return {};
    }
    const std::string type=lower(strField(c,"type"));
    if(type!="s" && type!="s2" && type!="t" && type!="t2") {
        warnings.push_back("Astra 4.4: unsupported DVB tuner system "+type);
        return {};
    }
    const auto pnr=options.find("pnr");
    if(pnr!=options.end()) {
        int n=0;
        if(number(pnr->second,n,1,65535))sid=static_cast<uint32_t>(n);
        else {warnings.push_back("Astra 4.4: invalid PNR");return {};}
    }
    if(options.count("cam") || options.count("biss") || options.count("ecm_pid"))
        warnings.push_back("Astra 4.4: CA entitlement/key not migrated; assign CAM manually");
    std::ostringstream url;
    if(type=="s" || type=="s2") {
        auto tp=split(strField(c,"tp"),':');
        if(tp.size()!=3){warnings.push_back("Astra 4.4: invalid satellite TP");return {};}
        int mhz=0,sym=0;
        if(!number(tp[0],mhz,900,14000) || !number(tp[2],sym,100,60000) ||
           (tp[1]!="H" && tp[1]!="V" && tp[1]!="h" && tp[1]!="v")) {
            warnings.push_back("Astra 4.4: unsupported satellite TP values");
            return {};
        }
        if(c.isMember("uni_scr") || c.isMember("uni_frequency")) {
            warnings.push_back("Astra 4.4: Unicable requires manual review; DVB input skipped");
            return {};
        }
        url<<"dvb://satellite?adapter="<<adapter<<"&frontend="<<frontend
           <<"&frequency_khz="<<mhz*1000<<"&symbol_rate="<<sym
           <<"&polarity="<<static_cast<char>(std::toupper(tp[1][0]))
           <<"&delivery_system=dvb-"<<type;
        const auto lnb=split(strField(c,"lnb"),':');
        if(lnb.size()==3) {
            int l1=0,l2=0,switchFreq=0;
            if(number(lnb[0],l1,0,12000) && number(lnb[1],l2,0,12000) &&
               number(lnb[2],switchFreq,0,14000))
                url<<"&lnb_lof1_khz="<<l1*1000
                   <<"&lnb_lof2_khz="<<l2*1000
                   <<"&lnb_slof_khz="<<switchFreq*1000;
        } else if(c.isMember("lnb"))
            warnings.push_back("Astra 4.4: nonstandard LNB value skipped");
        int diseqc=0;
        if(intField(c,"diseqc",diseqc,0,15))url<<"&diseqc_source="<<diseqc;
        int streamId=-1;
        if(intField(c,"stream_id",streamId,0,255))url<<"&stream_id="<<streamId;
    } else {
        int mhz=0;
        if(!intField(c,"frequency",mhz,42,1002)) {
            warnings.push_back("Astra 4.4: invalid DVB-T/T2 frequency");return {};
        }
        int streamId=-1;
        (void)intField(c,"stream_id",streamId,0,255);
        url<<"dvb://terrestrial?adapter="<<adapter<<"&frontend="<<frontend
           <<"&frequency_khz="<<mhz*1000
           <<"&bandwidth_hz=8000000&delivery_system=dvb-"<<type
           <<"&modulation=auto&stream_id="<<streamId;
        if(c.isMember("bandwidth") || c.isMember("modulation"))
            warnings.push_back("Astra 4.4: DVB-T bandwidth/modulation need review");
    }
    return url.str();
}

inline bool importLegacy(const std::string& text,Conversion& result,
                         std::string& error) {
    result={};error.clear();
    std::vector<Token> tokens;
    if(!tokenize(text,tokens,error))return false;
    Literals literals(tokens);
    std::map<std::string,Json::Value> tunes;
    std::vector<Json::Value> channels;
    size_t skippedCalls=0;
    for(size_t i=0;i+2<tokens.size();++i) {
        const bool tune=tokens[i].text=="dvb_tune";
        const bool channel=tokens[i].text=="make_channel";
        if(!tune && !channel)continue;
        if(i>0 && (tokens[i-1].text=="." || tokens[i-1].text==":"))continue;
        if(tokens[i+1].text!="(" || tokens[i+2].text!="{")continue;
        size_t cursor=i+2;
        Json::Value options;
        if(!literals.value(cursor,options) || !options.isObject() ||
           cursor>=tokens.size() || tokens[cursor].text!=")") {
            ++skippedCalls;
            continue;
        }
        if(tune) {
            if(i>=2 && tokens[i-1].text=="=" &&
               tokens[i-2].kind==Token::Id)
                tunes[tokens[i-2].text]=std::move(options);
            else result.warnings.push_back(
                "Astra 4.4: unassigned dvb_tune ignored");
        } else channels.push_back(std::move(options));
        i=cursor;
        if(channels.size()>512 || tunes.size()>128) {
            error="Astra 4.4 configuration exceeds supported size";
            return false;
        }
    }
    if(skippedCalls)result.warnings.push_back(
        "Astra 4.4: dynamic/non-literal Lua calls ignored (never executed)");
    for(const auto& ch:channels) {
        if(!ch["name"].isString() || !ch["input"].isArray()) {
            ++result.ignored;result.warnings.push_back(
                "Astra 4.4: channel without name or literal input array skipped");
            continue;
        }
        StreamConfig stream=baseStream(ch["name"].asString());
        if(stream.name.empty()) {++result.ignored;continue;}
        if(ch.isMember("service_provider") && ch["service_provider"].isString())
            stream.serviceProvider=ch["service_provider"].asString();
        if(ch.isMember("service_name") && ch["service_name"].isString())
            stream.serviceName=ch["service_name"].asString();
        int serviceId=0;
        if(intField(ch,"set_pnr",serviceId,1,65535))stream.serviceId=serviceId;
        std::vector<std::string> sources;
        uint32_t sourceSid=0;
        for(const auto& val:ch["input"]) {
            if(!val.isString()) {result.warnings.push_back(
                "Astra 4.4: non-string input skipped");continue;}
            std::string uri=val.asString();
            if(!isSafeUri(uri)) {result.warnings.push_back(
                "Astra 4.4: malformed input URI");continue;}
            uint32_t sid=0;
            if(begins(lower(uri),"dvb://"))
                uri=convertDvb(uri,tunes,sid,result.warnings);
            else {
                const auto hash=uri.find('#');
                if(hash!=std::string::npos){
                    const auto p=params(uri.substr(hash+1));
                    const auto pnr=p.find("pnr");
                    if(pnr!=p.end()){
                        int n=0;
                        if(number(pnr->second,n,1,65535))sid=n;
                    }
                    if(p.count("biss") || p.count("cam"))
                        result.warnings.push_back(
                            "Astra 4.4: CA settings in input URI not migrated");
                    result.warnings.push_back(
                        "Astra 4.4: input URL fragment/options require manual review");
                    uri.resize(hash);
                }
                StreamConfig probe=stream;
                uri=sourceUrl(uri,probe);
                if(sources.empty())stream.inputMode=probe.inputMode;
            }
            if(uri.empty())continue;
            if(sources.empty())sourceSid=sid;
            sources.push_back(std::move(uri));
        }
        if(sources.empty()) {++result.ignored;continue;}
        stream.inputUri=sources.front();
        if(sourceSid)stream.inputServiceId=sourceSid;
        if(sources.size()>1) {
            stream.backupInputUri=sources[1];
            if(begins(sources[1],"file://")) {
                stream.backupInputType="file";
                stream.backupFileLoop=true;
                result.warnings.push_back(
                    "Astra 4.4: file fallback loop requires manual validation");
            }
        }
        if(sources.size()>2)result.warnings.push_back(
            "Astra 4.4: only two literal inputs migrated");
        std::vector<std::string> outputs;
        if(ch["output"].isArray()) {
            for(const auto& out:ch["output"]) {
                if(!out.isString())continue;
                std::string uri=out.asString();
                const auto hash=uri.find('#');
                if(hash!=std::string::npos){
                    uri.resize(hash);
                    result.warnings.push_back(
                        "Astra 4.4: output TTL/options not migrated");
                }
                outputs.push_back(uri);
            }
        }
        addOutputs(stream,outputs,result.warnings);
        if(ch.isMember("map") || ch.isMember("set_tsid") ||
           ch.isMember("http_keep_active"))
            result.warnings.push_back(
                "Astra 4.4: PID remap/TSID/HTTP keepalive require review");
        // Import always paused, independent of old enable=true.
        result.streams.push_back(std::move(stream));
    }
    if(result.streams.empty()) {
        error="No supported literal make_channel({...}) objects found in Astra 4.4 Lua";
        return false;
    }
    return true;
}

inline std::string luaQuote(const std::string& text) {
    std::string out="\"";
    for(unsigned char c:text) {
        switch(c) {
        case '\\':out+="\\\\";break;
        case '"':out+="\\\"";break;
        case '\n':out+="\\n";break;
        case '\r':out+="\\r";break;
        case '\t':out+="\\t";break;
        default:
            if(c<32 || c==127) {
                out+="\\"+std::to_string(static_cast<unsigned>(c));
                // Lua decimal escapes followed by digit may be ambiguous;
                // emit exactly three digits.
                const size_t digits=std::to_string(static_cast<unsigned>(c)).size();
                out.insert(out.size()-digits,3-digits,'0');
            } else out.push_back(static_cast<char>(c));
        }
    }
    return out+"\"";
}
inline std::string outputAddress(const std::string& type,const std::string& host,int port) {
    if((type!="udp-vbr" && type!="udp-cbr" && type!="rtp") ||
       host.empty() || port<1 || port>65535 ||
       host.find_first_of(";\r\n")!=std::string::npos) return {};
    return std::string(type=="rtp"?"rtp://":"udp://")+host+":"+std::to_string(port);
}

inline std::string exportLegacy(const std::vector<StreamConfig>& streams,
                                std::vector<std::string>& warnings) {
    std::ostringstream script;
    script<<"-- Cesbo Astra 4.4.18x Lua configuration generated by DVBStreamer5.\n"
          <<"-- Review adapter assignments and authorization before enabling.\n"
          <<"-- Never execute this file in DVBStreamer5; use it only as an Astra config.\n\n";
    std::map<std::string,std::string> exportedTunings;
    std::set<std::string> checkedAdapterTp;
    unsigned counter=0;
    for(const auto& s:streams) {
        if(s.name.empty() || s.inputUri.empty())continue;
        std::string primary=s.inputUri;
        if(begins(lower(primary),"dvb://")) {
            const auto mark=primary.find('?');
            const auto p=mark==std::string::npos?std::map<std::string,std::string>{}
                                                    :params(primary.substr(mark+1));
            const auto adapterIt=p.find("adapter"),freqIt=p.find("frequency_khz"),
                  rateIt=p.find("symbol_rate"),polIt=p.find("polarity");
            int adapter=0,front=0,freq=0,rate=0;
            if(adapterIt==p.end() || freqIt==p.end() || rateIt==p.end() ||
               polIt==p.end() ||
               !number(adapterIt->second,adapter,0,31) ||
               !number(freqIt->second,freq,900000,14000000) ||
               !number(rateIt->second,rate,100,60000)) {
                warnings.push_back("Astra 4.4 export: unrepresentable DVB input skipped");
                continue;
            }
            const auto feIt=p.find("frontend");
            if(feIt!=p.end() && !number(feIt->second,front,0,31))continue;
            const std::string delivery=p.count("delivery_system") ?
                lower(p.at("delivery_system")) : "dvb-s2";
            if(delivery!="dvb-s" && delivery!="dvb-s2") {
                warnings.push_back("Astra 4.4 export: DVB-T/T2 or other system requires manual adapter");
                continue;
            }
            if(polIt->second!="H" && polIt->second!="V")continue;
            const auto ub=p.find("unicable_mode");
            if(ub!=p.end() && ub->second!="none" && !ub->second.empty()){
                warnings.push_back("Astra 4.4 export: Unicable settings must be configured manually");
                continue;
            }
            const std::string tuneKey=std::to_string(adapter)+":"+std::to_string(front)+":"+
                std::to_string(freq)+":"+std::to_string(rate)+":"+polIt->second+":"+delivery;
            auto existing=exportedTunings.find(tuneKey);
            std::string var;
            if(existing==exportedTunings.end()) {
                var="dvb5_tune_"+std::to_string(++counter);
                exportedTunings.emplace(tuneKey,var);
                script<<var<<" = dvb_tune({\n  adapter = "<<adapter<<",\n  device = "<<front
                      <<",\n  type = "<<luaQuote(delivery=="dvb-s"?"S":"S2")
                      <<",\n  tp = "<<luaQuote(std::to_string(freq/1000)+":"+polIt->second+
                                                      ":"+std::to_string(rate))<<",\n";
                const auto diseqc=p.find("diseqc_source");
                if(diseqc!=p.end() && diseqc->second!="-1")
                    script<<"  diseqc = "<<luaQuote(diseqc->second)<<",\n";
                const auto lof1=p.find("lnb_lof1_khz"),lof2=p.find("lnb_lof2_khz"),
                           slof=p.find("lnb_slof_khz");
                if(lof1!=p.end() && lof2!=p.end() && slof!=p.end()) {
                    int l1=0,l2=0,ls=0;
                    if(number(lof1->second,l1,0,12000000) &&
                       number(lof2->second,l2,0,12000000) &&
                       number(slof->second,ls,0,14000000))
                        script<<"  lnb = "<<luaQuote(std::to_string(l1/1000)+":"+
                            std::to_string(l2/1000)+":"+std::to_string(ls/1000))<<",\n";
                }
                script<<"})\n\n";
            } else var=existing->second;
            primary="dvb://"+var;
            if(s.inputServiceId)primary+="#pnr="+std::to_string(s.inputServiceId);
            else if(s.serviceId)primary+="#pnr="+std::to_string(s.serviceId);
            warnings.push_back("Astra 4.4 export: verify DVB adapter/tuning and CAM assignment");
        }
        script<<"make_channel({\n  name = "<<luaQuote(s.name)<<",\n  enable = "
              <<(s.autoStart?"true":"false")<<",\n  input = {\n    "
              <<luaQuote(primary)<<",\n";
        if(!s.backupInputUri.empty()) {
            std::string backup=s.backupInputUri;
            if(s.backupInputType=="file" && s.backupFileLoop)
                backup+="#loop";
            script<<"    "<<luaQuote(backup)<<",\n";
        }
        script<<"  },\n  output = {\n";
        const std::string mainOut=outputAddress(s.outputType,s.outputHost,s.outputPort);
        if(!mainOut.empty())script<<"    "<<luaQuote(mainOut)<<",\n";
        else warnings.push_back("Astra 4.4 export: HTTP/HLS/SRT output needs manual setup");
        for(const auto& o:s.additionalOutputs) {
            const auto uri=outputAddress(o.outputType,o.outputHost,o.outputPort);
            if(!uri.empty())script<<"    "<<luaQuote(uri)<<",\n";
            else warnings.push_back("Astra 4.4 export: unsupported secondary output skipped");
        }
        script<<"  },\n";
        if(!s.serviceName.empty())script<<"  service_name = "<<luaQuote(s.serviceName)<<",\n";
        if(!s.serviceProvider.empty())script<<"  service_provider = "<<luaQuote(s.serviceProvider)<<",\n";
        if(s.transcodeEnabled || !s.conditionalAccessClient.empty())
            warnings.push_back("Astra 4.4: CAM/transcoding configuration not exported");
        script<<"})\n\n";
    }
    return script.str();
}
} // namespace dvbstreamer5::external_config::astra44
