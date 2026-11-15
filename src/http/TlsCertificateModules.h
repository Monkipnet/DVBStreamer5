#pragma once
// DVBStreamer5 V546: TLS certificate control through isolated systemd jobs.
// No shell interprets admin-supplied arguments; no network calls on an HTTP worker.
#include <jsoncpp/json/json.h>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <vector>
extern char **environ;

namespace dvbstreamer5::tls {
inline Json::Value fail(const std::string& reason) {
    Json::Value value(Json::objectValue);
    value["result"]="error";value["error"]=reason;
    return value;
}
inline bool validDomain(const std::string& domain) {
    if(domain.size()<4 || domain.size()>253 ||
       domain.find('.')==std::string::npos ||
       domain.find("..")!=std::string::npos ||
       domain.front()=='.' || domain.back()=='.' ||
       std::all_of(domain.begin(),domain.end(),[](unsigned char c){
           return (c>='0'&&c<='9')||c=='.';
       }) ||
       domain.size()>=6 && domain.compare(domain.size()-6,6,".local")==0 ||
       domain.size()>=9 && domain.compare(domain.size()-9,9,".internal")==0)
        return false;
    static const std::regex label(R"(^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$)");
    std::size_t begin=0;
    while(begin<domain.size()){
        const auto end=domain.find('.',begin);
        const auto part=domain.substr(begin,end==std::string::npos?
            std::string::npos:end-begin);
        if(part.empty()||!std::regex_match(part,label))return false;
        if(end==std::string::npos)return true;
        begin=end+1;
    }
    return false;
}
inline bool validEmail(const std::string& email){
    static const std::regex safe(
        R"(^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$)");
    return email.size()>=6 && email.size()<=254 &&
        std::regex_match(email,safe);
}
inline std::string scriptPath() {
    std::error_code ec;
    const auto portable=std::filesystem::current_path(ec)/"tls/manage.sh";
    if(!ec && std::filesystem::is_regular_file(portable,ec))
        return portable.string();
    if(std::filesystem::is_regular_file("/opt/DVBStreamer5/tls/manage.sh",ec))
        return "/opt/DVBStreamer5/tls/manage.sh";
    return {};
}
inline Json::Value status(int backendPort){
    Json::Value result(Json::objectValue);
    result["phase"]="idle";result["mode"]="none";
    result["auto_renew"]=false;
    result["backend_port"]=backendPort;
    result["module_installed"]=!scriptPath().empty();
    const std::filesystem::path path("/etc/dvbstreamer5/tls/state.json");
    std::error_code ec;
    if(!std::filesystem::is_regular_file(path,ec) ||
       std::filesystem::file_size(path,ec)>8192 || ec)return result;
    std::ifstream file(path);
    Json::Value saved;Json::CharReaderBuilder reader;std::string errors;
    if(file && Json::parseFromStream(reader,file,&saved,&errors) && saved.isObject()){
        for(const char* field:{"phase","mode","domain","message","auto_renew","issuer","expires"}){
            if(saved.isMember(field))result[field]=saved[field];
        }
    }
    return result;
}
inline Json::Value launch(const std::vector<std::string>& parameters){
    const std::string script=scriptPath();
    if(script.empty())return fail("TLS modules are not installed; install V546 portable bundle");
    if(!std::filesystem::exists("/usr/bin/systemd-run"))
        return fail("systemd-run is required for background TLS operations");
    const auto id=std::chrono::steady_clock::now().time_since_epoch().count();
    std::vector<std::string> arguments{
        "/usr/bin/systemd-run","--no-block","--collect","--quiet",
        "--unit=dvbstreamer5-tls-"+std::to_string(id),
        "/bin/bash",script
    };
    arguments.insert(arguments.end(),parameters.begin(),parameters.end());
    std::vector<char*> argv;
    for(auto& arg:arguments)argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid=-1;
    const int rc=::posix_spawn(&pid,"/usr/bin/systemd-run",
                               nullptr,nullptr,argv.data(),::environ);
    if(rc!=0 || pid<=0)return fail("Cannot launch TLS systemd worker");
    int code=0;
    if(::waitpid(pid,&code,0)!=pid || !WIFEXITED(code) || WEXITSTATUS(code)!=0)
        return fail("systemd refused TLS worker; check systemd journal");
    Json::Value value(Json::objectValue);
    value["result"]="started";
    value["detail"]="TLS operation is running in an independent systemd service";
    return value;
}
struct SelfSignedModule {
    static Json::Value generate(const std::string& domain,int backend,int https){
        if(!validDomain(domain))return fail("Invalid certificate domain");
        if(backend<1||backend>65535||https<1||https>65535||
           backend==https||https==80)return fail("Invalid proxy port");
        return launch({"selfsigned",domain,std::to_string(backend),std::to_string(https)});
    }
};
struct LetsEncryptModule {
    static Json::Value setEnabled(bool enabled,const std::string& domain,
                                  const std::string& email,int backend,int https){
        if(!enabled)return launch({"le-disable"});
        if(!validDomain(domain)||!validEmail(email))
            return fail("Valid public domain and email are required for Let's Encrypt");
        if(backend<1||backend>65535||https<1||https>65535||
           backend==https||https==80)return fail("Invalid proxy port");
        return launch({"le-enable",domain,email,std::to_string(backend),
                       std::to_string(https)});
    }
};
} // namespace dvbstreamer5::tls
