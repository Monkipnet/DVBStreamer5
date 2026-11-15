// V536: opt-in, isolated read-only DVBStreamer5 cluster peering.
// This file is included from HttpServer.cpp after HttpServerState.inc.
#include "http/ClusterPeerStore.h"
#include "NativeHttpClient.h"
#include "http/ClusterViewerRouting.h"
#include <map>
#include <unordered_set>

bool HttpServer::clusterTokenAuthorized(const std::string& token) {
    std::lock_guard<std::mutex> lock(clusterMutex_);
    dvbstreamer5::cluster::Config config;
    if(!dvbstreamer5::cluster::load(configManager.configDirectory(),config))
        return false;
    return dvbstreamer5::cluster::tokenMatches(config.nodeToken,token);
}

std::string HttpServer::clusterNodeSnapshot() {
    Json::Value root(Json::objectValue),channels(Json::arrayValue);
    root["server_name"]=configManager.config.serverName;
    root["version"]=kProgramVersion;
    auto snapshot=streamManager.snapshot();
    std::map<std::string,std::uint64_t> viewerCounts;
    for(const auto& session:streamManager.activeStreamSessions()){
        if(!session.streamId.empty() && session.connections>0)
            viewerCounts[session.streamId]+=session.connections;
    }
    unsigned count=0,active=0;
    for(const auto& stream:configManager.config.streams) {
        if(++count>512U)break;
        Json::Value channel(Json::objectValue);
        channel["id"]=stream.id;
        channel["name"]=stream.name;
        channel["tvg_id"]=stream.tvgId;
        channel["slug"]=hlsPublicName(stream);
        channel["http"]=hasOutputType(stream,"http");
        channel["hls"]=hasOutputType(stream,"hls");
        channel["hls_playlist"]=stream.transcodeEnabled && stream.transcodeMultibitrateEnabled ? "master.m3u8" : "video.m3u8";
        channel["viewers"]=Json::UInt64(viewerCounts[stream.id]);
        const auto found=snapshot.find(stream.id);
        const bool running=found!=snapshot.end()&&found->second &&
            found->second->active.load(std::memory_order_relaxed);
        channel["active"]=running;
        if(running){
            ++active;
            channel["using_backup"]=found->second->usingBackup;
            channel["bitrate_in_kbps"]=Json::UInt64(
                found->second->inputBitrate.load(std::memory_order_relaxed)/1000U);
            channel["bitrate_out_kbps"]=Json::UInt64(
                found->second->outputBitrate.load(std::memory_order_relaxed)/1000U);
        }else{
            channel["using_backup"]=false;
            channel["bitrate_in_kbps"]=Json::UInt64(0);
            channel["bitrate_out_kbps"]=Json::UInt64(0);
        }
        channels.append(std::move(channel));
    }
    root["channels"]=std::move(channels);
    root["total"]=count>512U?512U:count;
    root["active"]=active;
    Json::StreamWriterBuilder writer;writer["indentation"]="";
    return Json::writeString(writer,root);
}

std::string HttpServer::clusterPeerConfiguration() {
    std::lock_guard<std::mutex> lock(clusterMutex_);
    dvbstreamer5::cluster::Config config;
    if(!dvbstreamer5::cluster::ensure(configManager.configDirectory(),config))
        return R"({"error":"Cannot read or create private cluster configuration"})";
    Json::Value result=dvbstreamer5::cluster::publicPeers(config,true);
    result["result"]="ok";
    Json::StreamWriterBuilder writer;writer["indentation"]="";
    return Json::writeString(writer,result);
}

std::string HttpServer::clusterPeerChange(const std::string& body,bool remove) {
    Json::Value result(Json::objectValue),input;
    Json::CharReaderBuilder reader;std::string errors;
    std::istringstream source(body);
    if(body.size()>2048U||!Json::parseFromStream(reader,source,&input,&errors)||
       !input.isObject()||!input["url"].isString()){
        return R"({"error":"Invalid cluster peer request"})";
    }
    std::string url=input["url"].asString();
    if(!url.empty()&&url.back()=='/')url.pop_back();
    const bool verifyTls=input.get("verify_tls",true).asBool();
    if(!dvbstreamer5::cluster::validOrigin(url,!verifyTls))
        return R"({"error":"Use HTTPS or disable TLS verification for HTTP public DNS, without a path"})";
    std::lock_guard<std::mutex> lock(clusterMutex_);
    dvbstreamer5::cluster::Config config;
    if(!dvbstreamer5::cluster::ensure(configManager.configDirectory(),config))
        return R"({"error":"Cannot read private cluster configuration"})";
    // V541: editing an existing node uses its original URL as immutable
    // identity. A changed URL must not silently append a second node.
    const std::string originalUrl=input.get("original_url","").asString();
    if(!originalUrl.empty() && !dvbstreamer5::cluster::validOrigin(originalUrl,true))
        return R"({"error":"Invalid original cluster peer address"})";
    auto found=std::find_if(config.peers.begin(),config.peers.end(),
        [&](const auto& peer){return peer.url==(originalUrl.empty()?url:originalUrl);});
    if(remove){
        if(found==config.peers.end())return R"({"error":"Unknown peer"})";
        config.peers.erase(found);
    }else{
        if(!input["name"].isString() ||
           (!input["token"].isString() && originalUrl.empty()))
            return R"({"error":"Peer name and access token are required"})";
        if(!originalUrl.empty() && found==config.peers.end())
            return R"({"error":"Unknown peer"})";
        const auto duplicate=std::find_if(config.peers.begin(),config.peers.end(),
            [&](const auto& candidate){return candidate.url==url;});
        if(duplicate!=config.peers.end() && duplicate!=found)
            return R"({"error":"Cluster peer address already exists"})";
        std::string token=input.get("token","").asString();
        if(token.empty() && found!=config.peers.end())token=found->token;
        dvbstreamer5::cluster::Peer peer{
            input["name"].asString(),url,token,
            input.get("viewer_url","").asString(),
            verifyTls};
        if(!peer.viewerUrl.empty() && peer.viewerUrl.back()=='/')
            peer.viewerUrl.pop_back();
        if(!dvbstreamer5::cluster::validPeer(peer))
            return R"({"error":"Invalid peer name, origin or 64-digit token"})";
        if(found!=config.peers.end())*found=std::move(peer);
        else {
            if(config.peers.size()>=8U)return R"({"error":"Maximum of 8 peers"})";
            config.peers.push_back(std::move(peer));
        }
    }
    if(!dvbstreamer5::cluster::save(configManager.configDirectory(),config))
        return R"({"error":"Failed to write private cluster configuration"})";
    {
        std::lock_guard<std::mutex> statusLock(clusterStatusMutex_);
        clusterStatusValid_=false;
    }
    result=dvbstreamer5::cluster::publicPeers(config,false);
    result["result"]="ok";
    Json::StreamWriterBuilder writer;writer["indentation"]="";
    return Json::writeString(writer,result);
}

std::string HttpServer::clusterPeerStatus() {
    dvbstreamer5::cluster::Config config;
    {
        std::lock_guard<std::mutex> lock(clusterMutex_);
        if(!dvbstreamer5::cluster::load(configManager.configDirectory(),config))
            return R"({"error":"Cluster peer configuration unreadable"})";
    }
    std::lock_guard<std::mutex> cacheLock(clusterStatusMutex_);
    const auto now=std::chrono::steady_clock::now();
    if(clusterStatusValid_ &&
       now-clusterStatusAt_<std::chrono::seconds(5)){
        Json::StreamWriterBuilder writer;writer["indentation"]="";
        return Json::writeString(writer,clusterStatusCache_);
    }
    Json::Value root(Json::objectValue),peers(Json::arrayValue);
    root["result"]="ok";
    for(const auto& peer:config.peers) {
        Json::Value item(Json::objectValue);
        item["name"]=peer.name;
        item["url"]=peer.url;
        item["verify_tls"]=peer.verifyTlsPeer;
        if(!peer.viewerUrl.empty())item["viewer_url"]=peer.viewerUrl;
        item["online"]=false;
        item["channels"]=Json::Value(Json::arrayValue);
        dvbstreamer5::http::RequestOptions options;
        options.connectTimeoutMs=600;
        options.readTimeoutMs=900;
        options.writeTimeoutMs=500;
        options.totalTimeoutMs=1500;
        options.maxRedirects=0; // Never forward tokens across origins.
        options.maxBodyBytes=256U*1024U;
        options.verifyTlsPeer=peer.verifyTlsPeer;
        options.headers.emplace_back("X-DVBStreamer5-Cluster-Key",peer.token);
        dvbstreamer5::http::Response response;std::string error;
        if(dvbstreamer5::http::get(peer.url+"/api/cluster/node",options,response,error) &&
           response.status==200 && response.body.size()<=256U*1024U){
            Json::CharReaderBuilder reader;Json::Value remote;std::string parseError;
            std::string raw(response.body.begin(),response.body.end());
            std::istringstream source(raw);
            if(Json::parseFromStream(reader,source,&remote,&parseError) &&
               remote.isObject() && remote["channels"].isArray() &&
               remote["channels"].size()<=512U &&
               remote["server_name"].isString()){
                item["online"]=true;
                item["remote_name"]=remote["server_name"];
                item["version"]=remote.get("version","").asString();
                item["channels"]=remote["channels"];
                item["total"]=remote["channels"].size();
                item["active"]=remote.get("active",0);
            }
        }
        peers.append(std::move(item));
    }
    root["peers"]=std::move(peers);
    clusterStatusCache_=root;
    clusterStatusAt_=std::chrono::steady_clock::now();
    clusterStatusValid_=true;
    Json::StreamWriterBuilder writer;writer["indentation"]="";
    return Json::writeString(writer,root);
}
