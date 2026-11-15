// V537: opt-in interserver failover for *new* HTTP/HLS viewers and
// least-privilege middleware route discovery. Media and old endpoints unchanged.
#include "http/ClusterViewerRouting.h"

Json::Value HttpServer::clusterChooseStream(const std::string& id,bool hls,
                                            const std::string& clientKey) {
    using dvbstreamer5::cluster::routing::Origin;
    Json::Value result(Json::objectValue);
    result["result"]="error";
    if(!dvbstreamer5::cluster::routing::safeChannelId(id)){
        result["error"]="Invalid channel ID";
        return result;
    }
    const auto* localCfg=findStreamConfigById(configManager.config.streams,id);
    if(!localCfg){
        result["error"]="Unknown local channel";
        return result;
    }
    dvbstreamer5::cluster::Config peers;
    {
        std::lock_guard<std::mutex> lock(clusterMutex_);
        if(!dvbstreamer5::cluster::load(configManager.configDirectory(),peers)){
            result["error"]="Cluster configuration unreadable";
            return result;
        }
    }
    if(!peers.autoFailover && !peers.balanceViewers){
        result["error"]="Cluster viewer routing disabled";
        return result;
    }
    std::vector<Origin> origins;
    const bool hasLocalOutput=hasOutputType(*localCfg,hls?"hls":"http");
    auto running=streamManager.snapshot();
    const auto it=running.find(id);
    const bool localLive=it!=running.end() && it->second &&
        it->second->active.load(std::memory_order_relaxed) &&
        it->second->outputBitrate.load(std::memory_order_relaxed)>0;
    if(hasLocalOutput && localLive) {
        std::string url;
        for(const auto& output:streamOutputs(*localCfg)){
            if(normalizedOutputType(output)==(hls?"hls":"http")){
                url=streamLink(output,configManager.config.httpPort);
                break;
            }
        }
        std::uint64_t viewers=0;
        for(const auto& session:streamManager.activeStreamSessions())
            if(session.streamId==id && session.connections>0)
                viewers+=session.connections;
        if(!url.empty())origins.push_back({configManager.config.serverName,url,viewers,true,true});
    }
    if(peers.autoFailover || peers.balanceViewers) {
        const std::string snapshot=clusterPeerStatus();
        Json::Value status;Json::CharReaderBuilder parser;
        std::string error;std::istringstream reader(snapshot);
        if(Json::parseFromStream(parser,reader,&status,&error) && status["peers"].isArray()){
            for(const auto& peer:status["peers"]){
                if(!peer.get("online",false).asBool() || !peer["channels"].isArray())
                    continue;
                std::string origin=peer.get("viewer_url","").asString();
                if(origin.empty())origin=peer.get("url","").asString();
                if(!dvbstreamer5::cluster::validOrigin(origin,!peer.get("verify_tls",true).asBool()))continue;
                const auto peerName=peer.get("name","").asString();
                for(const auto& ch:peer["channels"]){
                    if(!ch.isObject() || !ch.get("active",false).asBool() ||
                       !ch.get(hls?"hls":"http",false).asBool() ||
                       !ch["bitrate_out_kbps"].isNumeric() ||
                       ch["bitrate_out_kbps"].asUInt64()==0)continue;
                    const auto remoteId=ch.get("id","").asString();
                    const auto remoteTvgId=ch.get("tvg_id","").asString();
                    const auto remoteName=ch.get("name","").asString();
                    const bool bothHaveTvgIds=
                        !localCfg->tvgId.empty() && !remoteTvgId.empty();
                    // Conflicting explicit TVG IDs must never fall back to a
                    // coincidentally equal channel name or local ID.
                    const bool sameChannel=bothHaveTvgIds
                        ? localCfg->tvgId==remoteTvgId
                        : (remoteId==id ||
                           (!localCfg->name.empty() && localCfg->name==remoteName));
                    if(!sameChannel)continue;
                    const auto slug=ch.get("slug","").asString();
                    if(!dvbstreamer5::cluster::routing::safeChannelId(slug))continue;
                    const auto playlist=ch.get("hls_playlist","video.m3u8").asString();
                    if(hls && playlist!="video.m3u8" && playlist!="master.m3u8")continue;
                    const auto redirect=origin+"/"+slug+"/"+(hls?playlist:"mpegts");
                    const auto viewers=ch.get("viewers",Json::UInt64(0)).asUInt64();
                    origins.push_back({peerName,redirect,viewers,false,true});
                    break; // A peer contributes at most one matching output.
                }
            }
        }
    }
    // Opt-out of balancing keeps a healthy local stream; otherwise select
    // the least loaded candidate and sticky-tie-break by client identity.
    const auto selected=dvbstreamer5::cluster::routing::select(
        origins,peers.balanceViewers,clientKey);
    if(!selected.found){
        result["error"]="No healthy HTTP/HLS cluster origin";
        return result;
    }
    result["result"]="ok";
    result["stream_id"]=id;
    result["protocol"]=hls?"hls":"http";
    result["origin"]=selected.name;
    result["local"]=selected.local;
    result["url"]=selected.redirect;
    result["policy"]=peers.balanceViewers?"balance":"failover";
    result["authorization_required"]=dvbstreamer5::stream_auth::enabled(
        configManager.config,id);
    return result;
}

std::string HttpServer::clusterRoutingSettings(const std::string& body) {
    Json::Value input;
    Json::CharReaderBuilder parser;
    std::string errors;
    std::istringstream source(body);
    if(body.size()>1024 || !Json::parseFromStream(parser,source,&input,&errors) ||
       !input.isObject() || !input["auto_failover"].isBool() ||
       !input["balance_viewers"].isBool())
        return R"({"error":"Invalid cluster routing settings"})";
    std::lock_guard<std::mutex> lock(clusterMutex_);
    dvbstreamer5::cluster::Config config;
    if(!dvbstreamer5::cluster::ensure(configManager.configDirectory(),config))
        return R"({"error":"Cannot read cluster settings"})";
    config.autoFailover=input["auto_failover"].asBool() ||
                        input["balance_viewers"].asBool();
    config.balanceViewers=input["balance_viewers"].asBool();
    if(!dvbstreamer5::cluster::save(configManager.configDirectory(),config))
        return R"({"error":"Cannot save cluster routing settings"})";
    Json::Value result(Json::objectValue);
    result["result"]="ok";
    result["auto_failover"]=config.autoFailover;
    result["balance_viewers"]=config.balanceViewers;
    Json::StreamWriterBuilder writer;writer["indentation"]="";
    return Json::writeString(writer,result);
}

bool HttpServer::clusterViewerAllowed(const tcp::socket& socket,
                                    const http::request<http::string_body>& req,
                                    const std::string& id,bool hls) const {
    const std::string ip=clientIpForRequest(socket,req);
    if(ip.empty())return false;
    if(dvbstreamer5::stream_auth::enabled(configManager.config,id)){
        const std::string target(req.target());
        const auto q=target.find('?');
        const auto agent=req.find(http::field::user_agent);
        const auto host=req.find(http::field::host);
        return dvbstreamer5::stream_auth::SessionRegistry::instance().check(
            configManager.config,id,ip,hls?"hls":"http",
            q==std::string::npos?target:target.substr(0,q),
            agent==req.end()?"":std::string(agent->value()),
            host==req.end()?"":std::string(host->value()),
            q==std::string::npos?"":target.substr(q+1));
    }
    return isClientAllowedForStream(id,ip);
}
