#pragma once
#include "StreamAuthBackend.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// V388: one logical viewer session per (channel, IP, protocol, credentials).
// Nothing here copies media, creates a transcoder or proxies outgoing traffic.
// Backend requests never execute while a media-fanout lock is held.
namespace dvbstreamer5::stream_auth {
class SessionRegistry {
    using Clock=std::chrono::steady_clock;
    struct Entry {
        std::string id,backendId,channel,ip,protocol,path,query,agent,host;
        std::string backendUrl,endUrl;
        int timeoutMs=1200,recheckMinutes=5;
        Clock::time_point created=Clock::now(),lastSeen=created,nextCheck=created;
        std::chrono::system_clock::time_point createdWall=std::chrono::system_clock::now();
        std::size_t connections=0;
        bool checking=false,revoked=false,endQueued=false;
    };
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<std::string,Entry> sessions_;
    std::deque<Entry> finished_;
    std::vector<std::thread> workers_;
    static constexpr std::size_t kMaximumSessions=16384;
    static constexpr std::size_t kMaximumEndEvents=4096;
    static constexpr auto kIdle=std::chrono::minutes(2);
    static std::string make(const std::string& proto,const std::string& channel,
                            const std::string& ip,const std::string& query) {
        return proto+"-"+sessionId(channel,ip,query);
    }
    static bool checkBackend(const Entry& e) {
        AppConfig cfg;
        cfg.streamAuthBackendEnabled=true;
        cfg.streamAuthBackendUrl=e.backendUrl;
        cfg.streamAuthBackendTimeoutMs=e.timeoutMs;
        return authorize(cfg,e.channel,e.ip,e.path,e.agent,e.host,e.query);
    }
    static void sendEnd(const Entry& e) {
        if(e.endUrl.empty() || e.endUrl.size()>2048 ||
           (e.endUrl.rfind("https://",0)!=0 && e.endUrl.rfind("http://",0)!=0) ||
           e.endUrl.find_first_of("\r\n#")!=std::string::npos)return;
        std::string url=e.endUrl;
        if(url.back()!='?' && url.back()!='&')
            url+=url.find('?')==std::string::npos?"?":"&";
        url+="event=stop&session_id="+dvbstreamer5::http::encodeQueryComponent(e.backendId);
        dvbstreamer5::http::RequestOptions opt;
        opt.maxRedirects=0;
        opt.maxBodyBytes=2048;
        opt.connectTimeoutMs=e.timeoutMs;
        opt.readTimeoutMs=e.timeoutMs;
        opt.writeTimeoutMs=e.timeoutMs;
        opt.totalTimeoutMs=e.timeoutMs;
        opt.headers={{"X-Session-ID",e.backendId},{"X-Channel-ID",e.channel},
                     {"X-Real-IP",e.ip},{"X-Real-Path",e.path},
                     {"X-Session-Event","stop"}};
        dvbstreamer5::http::Response res;std::string error;
        (void)dvbstreamer5::http::get(url,opt,res,error);
    }
    void queueEndLocked(Entry& entry) {
        if(entry.endQueued)return;
        entry.endQueued=true;
        if(!entry.endUrl.empty() && finished_.size()<kMaximumEndEvents)
            finished_.push_back(entry);
        wake_.notify_one();
    }
    void cleanupLocked(Clock::time_point now) {
        for(auto it=sessions_.begin();it!=sessions_.end();) {
            auto& e=it->second;
            if(e.connections==0 && !e.checking && now-e.lastSeen>kIdle) {
                queueEndLocked(e);
                it=sessions_.erase(it);
            } else ++it;
        }
    }
    void worker() {
        for(;;) {
            Entry task;
            bool endpoint=false,refresh=false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cleanupLocked(Clock::now());
                if(!finished_.empty()){
                    task=std::move(finished_.front());finished_.pop_front();
                    endpoint=true;
                }else{
                    const auto now=Clock::now();
                    for(auto& item:sessions_){
                        auto& e=item.second;
                        if(!e.revoked && !e.checking && e.nextCheck<=now &&
                           (e.connections>0 || now-e.lastSeen<=kIdle)){
                            e.checking=true;task=e;refresh=true;break;
                        }
                    }
                }
                if(!endpoint && !refresh){
                    wake_.wait_for(lock,std::chrono::milliseconds(500));
                    continue;
                }
            }
            if(endpoint){sendEnd(task);continue;}
            const bool okay=checkBackend(task);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it=sessions_.find(task.id);
                if(it!=sessions_.end() && it->second.checking){
                    auto& e=it->second;
                    e.checking=false;
                    if(okay && !e.revoked)
                        e.nextCheck=Clock::now()+std::chrono::minutes(e.recheckMinutes);
                    else{
                        e.revoked=true;
                        queueEndLocked(e);
                    }
                }
            }
            wake_.notify_all();
        }
    }
    SessionRegistry() {
        // Bounded revalidation concurrency: the streaming I/O threads never
        // wait for the revalidation HTTP requests.
        for(int i=0;i<4;++i)workers_.emplace_back([this]{worker();});
    }
public:
    SessionRegistry(const SessionRegistry&)=delete;
    SessionRegistry& operator=(const SessionRegistry&)=delete;
    static SessionRegistry& instance() {
        // Intentionally process-lifetime: streaming clients can hold detached
        // writers at shutdown; no destructor/thread ordering race.
        static SessionRegistry* registry=new SessionRegistry;
        return *registry;
    }
    static std::string idFor(const std::string& protocol,
                             const std::string& channel,const std::string& ip,
                             const std::string& query) {
        return make(protocol,channel,ip,query);
    }
    bool check(const AppConfig& cfg,const std::string& channel,
               const std::string& ip,const std::string& protocol,
               const std::string& path,const std::string& agent,
               const std::string& host,const std::string& query) {
        if(!enabled(cfg,channel))return true;
        if(channel.empty()||ip.empty()||protocol.empty()||query.size()>2048)
            return false;
        const auto id=make(protocol,channel,ip,query);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto it=sessions_.find(id);
            if(it!=sessions_.end()){
                if(it->second.revoked)return false;
                if(it->second.checking) {
                    if(!wake_.wait_for(lock,
                        std::chrono::milliseconds(std::clamp(cfg.streamAuthBackendTimeoutMs,100,3000)+100),
                        [&]{auto x=sessions_.find(id);return x==sessions_.end() || !x->second.checking;}))
                        return false;
                    it=sessions_.find(id);
                    if(it!=sessions_.end() && it->second.revoked)return false;
                }
                if(it!=sessions_.end() && Clock::now()<it->second.nextCheck){
                    it->second.lastSeen=Clock::now();
                    return true;
                }
                if(it!=sessions_.end())it->second.checking=true;
            }else if(sessions_.size()>=kMaximumSessions){
                return false; // bounded memory: deny, never bypass
            }else{
                Entry entry;
                entry.id=id;entry.backendId=sessionId(channel,ip,query);
                entry.channel=channel;entry.ip=ip;entry.protocol=protocol;
                entry.path=path;entry.query=query;entry.agent=agent;entry.host=host;
                entry.backendUrl=cfg.streamAuthBackendUrl;
                entry.endUrl=cfg.streamAuthEndUrl;
                entry.timeoutMs=std::clamp(cfg.streamAuthBackendTimeoutMs,100,3000);
                entry.recheckMinutes=std::clamp(cfg.streamAuthRecheckMinutes,1,60);
                entry.checking=true;
                sessions_.emplace(id,std::move(entry));
            }
        }
        // First authorization and expired cache entries fail closed.
        const bool okay=authorize(cfg,channel,ip,path,agent,host,query);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it=sessions_.find(id);
            if(it==sessions_.end())return false;
            auto& e=it->second;
            e.checking=false;
            if(!okay || e.revoked) {
                e.revoked=true;
                if(e.connections>0)queueEndLocked(e);
                else sessions_.erase(it);
            }else {
                e.lastSeen=Clock::now();
                e.nextCheck=Clock::now()+std::chrono::minutes(
                    std::clamp(cfg.streamAuthRecheckMinutes,1,60));
                e.backendUrl=cfg.streamAuthBackendUrl;
                e.endUrl=cfg.streamAuthEndUrl;
                e.timeoutMs=std::clamp(cfg.streamAuthBackendTimeoutMs,100,3000);
                e.recheckMinutes=std::clamp(cfg.streamAuthRecheckMinutes,1,60);
            }
        }
        wake_.notify_all();
        return okay;
    }
    bool exists(const std::string& id) const {
        if(id.empty())return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.find(id)!=sessions_.end();
    }
    bool isAllowed(const std::string& id) const {
        if(id.empty())return true;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it=sessions_.find(id);
        return it!=sessions_.end() && !it->second.revoked;
    }
    void touch(const std::string& id) {
        if(id.empty())return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=sessions_.find(id);
        if(it!=sessions_.end()&&!it->second.revoked)
            it->second.lastSeen=Clock::now();
    }
    void attach(const std::string& id) {
        if(id.empty())return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=sessions_.find(id);
        if(it!=sessions_.end()&&!it->second.revoked){
            ++it->second.connections;
            it->second.lastSeen=Clock::now();
        }
    }
    void detach(const std::string& id) {
        if(id.empty())return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=sessions_.find(id);
        if(it!=sessions_.end()){
            if(it->second.connections) --it->second.connections;
            if(it->second.connections==0 && it->second.protocol!="hls"){
                queueEndLocked(it->second);
                sessions_.erase(it);
            }
        }
    }
    bool revoke(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=sessions_.find(id);
        if(it==sessions_.end())return false;
        it->second.revoked=true;
        queueEndLocked(it->second);
        return true;
    }
    Json::Value listing(bool includeBackendId = false) const {
        Json::Value out(Json::arrayValue);
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& item:sessions_) {
            const auto& e=item.second;
            if(e.revoked)continue;
            Json::Value row(Json::objectValue);
            row["session_id"]=e.id;row["channel_id"]=e.channel;
            // Same opaque identifier sent to the middleware as X-Session-ID.
            // Never publish the viewer token or URL query credentials.
            if(includeBackendId)row["backend_id"]=e.backendId;
            row["protocol"]=e.protocol;row["ip"]=e.ip;row["viewer"]=e.ip;
            row["connections"]=Json::UInt64(e.connections);
            row["started_at"]=Json::Int64(std::chrono::duration_cast<
                std::chrono::seconds>(e.createdWall.time_since_epoch()).count());
            row["idle_seconds"]=Json::Int64(std::chrono::duration_cast<
                std::chrono::seconds>(Clock::now()-e.lastSeen).count());
            out.append(row);
        }
        return out;
    }
};
} // namespace dvbstreamer5::stream_auth
