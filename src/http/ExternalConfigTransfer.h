#pragma once
// V10.8.483: conservative Cesbo Astra / Flussonic stream-only migration.
// Foreign global settings, passwords, CAS, recording, template and auth
// semantics are intentionally NOT applied to DVBStreamer5.
#include "../ConfigManager.h"
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dvbstreamer5::external_config {
struct Conversion {
    std::vector<StreamConfig> streams;
    std::vector<std::string> warnings;
    size_t ignored = 0;
};

inline std::string trim(std::string s) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
    return s;
}
inline bool begins(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}
inline std::string safeId(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            r.push_back(static_cast<char>(c));
        else if (c == '.' || c == ' ') r.push_back('_');
        if (r.size() >= 44) break;
    }
    return r.empty() ? "channel" : r;
}
inline std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size()-2);
    return s;
}
inline std::string sourceUrl(std::string url, StreamConfig& stream) {
    url = unquote(url);
    const auto space = url.find_first_of(" \t\r\n"); // URL options are not URI.
    if (space != std::string::npos) url.resize(space);
    if (begins(url,"tshttp://")) return "http://" + url.substr(9);
    if (begins(url,"tshttps://")) return "https://" + url.substr(10);
    if (begins(url,"hls://")) {
        stream.inputMode = "hls";
        return "http://" + url.substr(6);
    }
    if (begins(url,"hlss://")) {
        stream.inputMode = "hls";
        return "https://" + url.substr(7);
    }
    static const char* allowed[] = {
        "udp://", "rtp://", "http://", "https://", "srt://",
        "rtsp://", "rtmp://", "file://"
    };
    for (const char* prefix : allowed) if (begins(lower(url), prefix)) return url;
    return {};
}
inline bool applyUdpOutput(const std::string& input, StreamConfig& cfg) {
    const std::string url = unquote(input);
    const std::string low = lower(url);
    if (!begins(low, "udp://") && !begins(low,"rtp://")) return false;
    const auto begin = url.find("://") + 3;
    const auto end = url.find_first_of("/? \t\r\n", begin);
    const std::string endpoint = url.substr(begin, end == std::string::npos
        ? std::string::npos : end-begin);
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 >= endpoint.size()) return false;
    std::string host = endpoint.substr(0,colon);
    const std::string port = endpoint.substr(colon+1);
    std::string bindIp;
    // Astra 4.4 supports udp://[interface@]multicast:port.
    const auto at = host.find('@');
    if (at != std::string::npos) {
        bindIp = host.substr(0,at);
        host = host.substr(at+1);
        // A Linux device name is not a portable source-interface address.
        // Never put it into an outgoing multicast host by accident.
        const auto octets = std::count(bindIp.begin(),bindIp.end(),'.');
        if (octets != 3 || bindIp.empty() || bindIp.size() > 15 ||
            bindIp.find_first_not_of("0123456789.") != std::string::npos)
            return false;
        std::istringstream parts(bindIp);
        std::string piece;
        unsigned count = 0;
        while (std::getline(parts,piece,'.')) {
            int n = -1;
            try {
                std::size_t used=0;
                n=std::stoi(piece,&used);
                if(used!=piece.size())return false;
            } catch(...) {return false;}
            if(n<0 || n>255) return false;
            ++count;
        }
        if(count!=4)return false;
    }
    if (host.empty() || host.size() > 255 || port.empty() ||
        !std::all_of(port.begin(),port.end(),[](unsigned char c){return std::isdigit(c);}))
        return false;
    int number = 0;
    try { number = std::stoi(port); } catch (...) { return false; }
    if (number < 1 || number > 65535) return false;
    cfg.outputType = begins(low,"rtp://") ? "rtp" : "udp-vbr";
    cfg.outputHost = host;
    cfg.outputPort = number;
    if (!bindIp.empty()) cfg.interfaceAddress = bindIp;
    cfg.outputMode = "caller";
    cfg.cbr = false;
    return true;
}
inline std::string firstStringUrl(const Json::Value& v) {
    if (v.isString()) return v.asString();
    if (v.isObject() && v["url"].isString()) return v["url"].asString();
    return {};
}
inline StreamConfig baseStream(std::string name) {
    StreamConfig cfg;
    cfg.name = std::move(name);
    cfg.outputType = "hls"; // No newly imported UDP listener surprises.
    cfg.outputMode = "listener";
    cfg.autoStart = false;  // Never start production streams on import.
    cfg.activationMode = "online";
    cfg.cbr = false;
    return cfg;
}
inline void addOutputs(StreamConfig& cfg, const std::vector<std::string>& urls,
                       std::vector<std::string>& warnings) {
    bool primary = false;
    for (const auto& url : urls) {
        StreamConfig copy = cfg;
        if (!applyUdpOutput(url, copy)) {
            if (!url.empty()) warnings.push_back("Output skipped (unsupported or invalid target)");
            continue;
        }
        if (!primary) {
            cfg.outputType=copy.outputType;
            cfg.outputHost=copy.outputHost;
            cfg.outputPort=copy.outputPort;
            cfg.interfaceAddress=copy.interfaceAddress;
            cfg.outputMode=copy.outputMode;
            cfg.cbr=copy.cbr;
            primary=true;
        } else {
            StreamOutputConfig extra;
            extra.outputType=copy.outputType;
            extra.outputMode=copy.outputMode;
            extra.outputHost=copy.outputHost;
            extra.outputPort=copy.outputPort;
            extra.interfaceAddress=copy.interfaceAddress;
            cfg.additionalOutputs.push_back(std::move(extra));
        }
    }
}

inline bool parseAstra(const std::string& data, Conversion& result,
                       std::string& error) {
    result={};
    if (data.size() > 4 * 1024 * 1024) { error="Astra file exceeds 4 MiB"; return false; }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream input(data);
    if (!Json::parseFromStream(builder,input,&root,&error) || !root.isObject() ||
        !root["make_stream"].isArray()) {
        error="Expected a Cesbo Astra JSON config with make_stream array";
        return false;
    }
    for (const auto& item: root["make_stream"]) {
        if (!item.isObject()) { ++result.ignored; continue; }
        const std::string type = lower(item.get("type","spts").asString());
        if (type != "spts") {
            ++result.ignored;
            result.warnings.push_back("MPTS stream skipped: manual service remapping required");
            continue;
        }
        const std::string name = item.get("name","").asString();
        const auto& inputs = item["input"];
        if (name.empty() || !inputs.isArray() || inputs.empty()) {
            ++result.ignored;
            result.warnings.push_back("Astra stream without name or inputs skipped");
            continue;
        }
        StreamConfig stream = baseStream(name);
        std::vector<std::string> sources;
        for (const auto& v: inputs) {
            StreamConfig inputProbe=stream;
            const auto uri=sourceUrl(firstStringUrl(v),inputProbe);
            if (!uri.empty()) {
                if (sources.empty()) stream.inputMode=inputProbe.inputMode;
                sources.push_back(uri);
            } else result.warnings.push_back("Unsupported Astra input skipped");
        }
        if (sources.empty()) {++result.ignored;continue;}
        stream.inputUri=sources[0];
        if (sources.size()>1) stream.backupInputUri=sources[1];
        if (sources.size()>2)
            result.warnings.push_back("Only the first two Astra inputs can migrate");
        std::vector<std::string> outputs;
        if (item["output"].isArray()) {
            for (const auto& v: item["output"]) outputs.push_back(firstStringUrl(v));
        }
        addOutputs(stream,outputs,result.warnings);
        result.streams.push_back(std::move(stream));
    }
    if (result.streams.empty()) {error="No supported Astra SPTS streams found";return false;}
    return true;
}

// Read only literal "stream <name> {...}" blocks. Ignore global listeners,
// auth, templates, VOD, DVR, includes and any unsupported directives.
// The scanner respects quotes and nested blocks, and does not interpret data.
inline bool parseFlussonic(const std::string& data, Conversion& result,
                           std::string& error) {
    result={};
    if (data.size() > 4 * 1024 * 1024) {error="Flussonic file exceeds 4 MiB";return false;}
    static const std::regex opener(R"(^\s*stream\s+([A-Za-z0-9_.-]+)\s*\{)");
    std::istringstream file(data);
    std::string line;
    bool inStream=false;
    int depth=0;
    StreamConfig current;
    std::vector<std::string> inputs, outputs;
    std::string buffer;
    auto finish=[&]() {
        if (current.name.empty())return;
        if (inputs.empty()) {
            ++result.ignored;
            result.warnings.push_back("Flussonic stream without supported input skipped");
        } else {
            current.inputUri=inputs[0];
            if (inputs.size()>1) current.backupInputUri=inputs[1];
            if (inputs.size()>2) result.warnings.push_back("Only first two Flussonic inputs migrated");
            addOutputs(current,outputs,result.warnings);
            result.streams.push_back(current);
        }
        current={};inputs.clear();outputs.clear();buffer.clear();
    };
    auto directive=[&](std::string text) {
        text=trim(text);
        if (text.empty()) return;
        const auto blank=text.find_first_of(" \t\n");
        if (blank==std::string::npos) return;
        const std::string key=lower(text.substr(0,blank));
        const std::string value=unquote(trim(text.substr(blank+1)));
        if (key=="input") {
            StreamConfig inputProbe=current;
            const auto url=sourceUrl(value,inputProbe);
            if (!url.empty()) {
                if(inputs.empty()) current.inputMode=inputProbe.inputMode;
                inputs.push_back(url);
            } else result.warnings.push_back("Unsupported Flussonic input skipped");
        } else if (key=="push") {
            if (begins(lower(value),"udp://") || begins(lower(value),"rtp://"))
                outputs.push_back(value);
            else result.warnings.push_back("Non-UDP/RTP Flussonic push skipped");
        }
    };
    for (int lineNo=0;std::getline(file,line);++lineNo) {
        if (lineNo>100000) {error="Flussonic file has too many lines";return false;}
        auto start=trim(line);
        if (start.empty() || begins(start,"#") || begins(start,"//")) continue;
        if (!inStream) {
            std::smatch match;
            if (!std::regex_search(line, match, opener)) continue;
            inStream=true;depth=1;current=baseStream(match[1].str());
            line=line.substr(static_cast<size_t>(match.position()+match.length()));
        }
        // Semicolon-delimited directives; nested options are skipped.
        bool quoted=false;char quote=0;
        for(char c:line) {
            if (c=='"' || c=='\'') {
                if (!quoted) {quoted=true;quote=c;}
                else if (quote==c) quoted=false;
            }
            if (!quoted && c=='{') {++depth;continue;}
            if (!quoted && c=='}') {
                --depth;
                if (depth==0) {finish();inStream=false;break;}
                continue;
            }
            if (depth != 1) continue;
            if (!quoted && c==';') {directive(buffer);buffer.clear();}
            else buffer.push_back(c);
        }
        if(inStream) buffer.push_back(' ');
    }
    if(inStream) {error="Unterminated Flussonic stream block";return false;}
    if(result.streams.empty()){error="No supported Flussonic stream blocks found";return false;}
    return true;
}
inline bool importedStreamMatches(const StreamConfig& a, const StreamConfig& b) {
    return lower(a.name)==lower(b.name) && a.inputUri==b.inputUri;
}
inline size_t mergeNewOnly(const Conversion& conversion,
                           std::vector<StreamConfig>& streams,
                           const std::string& kind, size_t& duplicateCount) {
    size_t added=0;
    duplicateCount=0;
    std::set<std::string> ids;
    for(const auto& s:streams) ids.insert(s.id);
    for(const auto& imported:conversion.streams) {
        if(std::any_of(streams.begin(),streams.end(),
            [&](const StreamConfig& previous){return importedStreamMatches(previous,imported);})) {
            ++duplicateCount;continue;
        }
        StreamConfig copy=imported;
        const std::string prefix="migration_"+kind+"_"+safeId(imported.name);
        std::string proposed=prefix;
        for(unsigned suffix=2;ids.count(proposed) && suffix<100000;++suffix)
            proposed=prefix+"_"+std::to_string(suffix);
        if(ids.count(proposed)) {++duplicateCount;continue;}
        ids.insert(proposed);
        copy.id=proposed;copy.autoStart=false;
        streams.push_back(std::move(copy));++added;
    }
    return added;
}
inline Json::Value exportAstra(const std::vector<StreamConfig>& streams,
                              std::vector<std::string>& warnings) {
    Json::Value root(Json::objectValue);
    root["make_stream"]=Json::Value(Json::arrayValue);
    for(const auto& s:streams){
        if(s.inputUri.empty() || s.name.empty()) continue;
        Json::Value item(Json::objectValue);
        item["id"]=safeId(s.id);
        item["name"]=s.name;
        item["type"]="spts";
        item["enable"]=s.autoStart;
        item["input"]=Json::Value(Json::arrayValue);
        item["input"].append(s.inputUri);
        if(!s.backupInputUri.empty() && s.backupInputType=="url")
            item["input"].append(s.backupInputUri);
        item["output"]=Json::Value(Json::arrayValue);
        const auto push=[&](const std::string& type,const std::string& host,int port){
            if((type=="udp-vbr" || type=="udp-cbr" || type=="rtp") &&
               !host.empty() && port>0 && port<=65535)
                item["output"].append(std::string(type=="rtp"?"rtp://":"udp://")+
                                      host+":"+std::to_string(port));
        };
        push(s.outputType,s.outputHost,s.outputPort);
        for(const auto& o:s.additionalOutputs)
            push(o.outputType,o.outputHost,o.outputPort);
        if(item["output"].empty()) warnings.push_back("Non-UDP/RTP outputs not exported to Astra");
        if(s.transcodeEnabled || !s.conditionalAccessClient.empty())
            warnings.push_back("Transcoding/CAM settings require manual configuration in Astra");
        root["make_stream"].append(std::move(item));
    }
    return root;
}
inline std::string exportFlussonic(const std::vector<StreamConfig>& streams,
                                   std::vector<std::string>& warnings) {
    std::ostringstream out;
    out<<"# Generated from DVBStreamer5 - review manually before deployment.\n";
    out<<"# Global listeners, authentication, DVR, CAM and transcoding are not exported.\n\n";
    std::set<std::string> names;
    for(const auto& s:streams){
        if(s.inputUri.empty() || s.name.empty()) continue;
        std::string name=safeId(s.name);
        const std::string base=name;
        for(unsigned index=2;names.count(name)&&index<100000;++index)
            name=base+"_"+std::to_string(index);
        names.insert(name);
        out<<"stream "<<name<<" {\n";
        auto writeInput=[&](std::string uri) {
            // Refuse to introduce extra Flussonic directives through a URL.
            if (uri.empty() || uri.find_first_of(";{}\r\n")!=std::string::npos) {
                warnings.push_back("Unsafe or empty input URI skipped in Flussonic export");
                return;
            }
            const bool hls=s.inputMode=="hls" ||
                           uri.find(".m3u8")!=std::string::npos;
            if(begins(lower(uri),"http://"))
                uri=std::string(hls?"hls://":"tshttp://")+uri.substr(7);
            else if(begins(lower(uri),"https://"))
                uri=std::string(hls?"hlss://":"tshttps://")+uri.substr(8);
            out<<"  input "<<uri<<";\n";
        };
        writeInput(s.inputUri);
        if(!s.backupInputUri.empty() && s.backupInputType=="url")
            writeInput(s.backupInputUri);
        auto push=[&](const std::string& type,const std::string& host,int port) {
            if((type=="udp-vbr" || type=="udp-cbr" || type=="rtp") &&
                !host.empty() && port>0 && port<=65535)
                out<<"  push "<<(type=="rtp"?"rtp://":"udp://")<<host<<":"<<port<<";\n";
        };
        push(s.outputType,s.outputHost,s.outputPort);
        for(const auto& o:s.additionalOutputs)
            push(o.outputType,o.outputHost,o.outputPort);
        if(s.outputType!="udp-vbr" && s.outputType!="udp-cbr" && s.outputType!="rtp")
            warnings.push_back("Flussonic generated with no equivalent primary push output");
        if(s.transcodeEnabled || !s.conditionalAccessClient.empty())
            warnings.push_back("Transcoding/CAM settings require manual configuration in Flussonic");
        out<<"}\n\n";
    }
    return out.str();
}
} // namespace dvbstreamer5::external_config
