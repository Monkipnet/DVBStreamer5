#include "CamSettingsPolicy.h"
#include "http/DvbTransponderUriEditor.h"
#include <cmath>
std::string HttpServer::dvbAdapters() {
    Json::Value root = DvbSatellite::adapters();
    std::map<std::pair<int, int>, Json::Value> usage;
    std::map<std::pair<int, int>, std::set<std::string>> activeTps;

    // A DVB frontend is assigned as soon as a stream is configured for it.
    // OnDemand streams are intentionally included even while their runtime
    // state is stopped, so the scanner does not advertise an assigned
    // frontend as free merely because no subscriber is connected yet.
    for (const auto& config : configManager.config.streams) {
        DvbSatelliteParams params;
        std::string parseError;
        if (!DvbSatellite::parseUri(config.inputUri, params, parseError)) continue;
        const auto key = std::make_pair(params.adapter, params.frontend);
        auto& item = usage[key];
        item["configured_streams"] = item.get("configured_streams", 0).asUInt() + 1;
        if (toLower(config.activationMode) == "ondemand") {
            item["ondemand_streams"] = item.get("ondemand_streams", 0).asUInt() + 1;
        }
        // Preserve one configured tune even while an OnDemand frontend is idle.
        // An active runtime consumer below overrides these values with the tune
        // that is physically in use right now.
        if (!item.isMember("frequency_khz")) {
            item["frequency_khz"] = params.frequencyKHz;
            item["symbol_rate"] = params.symbolRateK;
            item["bandwidth_hz"] = params.bandwidthHz;
            item["polarity"] = params.polarity;
            item["delivery_system"] = params.deliverySystem;
            item["modulation"] = params.modulation;
            item["fec"] = params.fec;
            item["stream_id"] = params.streamId;
            item["diseqc_source"] = params.diseqcSource;
            item["lnb_lof1_khz"] = params.lnbLof1KHz;
            item["lnb_lof2_khz"] = params.lnbLof2KHz;
            item["lnb_slof_khz"] = params.lnbSlofKHz;
            item["unicable_mode"] = params.unicableMode;
            item["unicable_user_band"] = params.unicableUserBand;
            item["unicable_frequency_khz"] = params.unicableFrequencyKHz;
            item["unicable_position"] = params.unicablePosition;
            item["unicable_pin"] = params.unicablePin;
        }
        if (!item.isMember("streams")) item["streams"] = Json::Value(Json::arrayValue);
        Json::Value stream;
        stream["id"] = config.id;
        stream["name"] = config.name;
        stream["sid"] = config.inputServiceId;
        stream["activation_mode"] = config.activationMode;
        item["streams"].append(stream);
    }

    // Keep the old active-consumer accounting separately. This preserves
    // SHARED N as a runtime indicator instead of conflating configured
    // OnDemand assignments with currently tuned consumers.
    const auto snap = streamManager.snapshot();
    for (const auto& [id, state] : snap) {
        if (!state || !state->running.load() || !state->active.load() ||
            state->usingBackup) continue;
        DvbSatelliteParams params;
        std::string parseError;
        if (!DvbSatellite::parseUri(state->config.inputUri, params, parseError)) continue;
        const auto key = std::make_pair(params.adapter, params.frontend);
        auto& item = usage[key];
        item["consumers"] = item.get("consumers", 0).asUInt() + 1;
        // Track real tuned DVB transponders separately from configured
        // (possibly idle OnDemand) service assignments. A frontend may host
        // multiple services but only one tune at a time.
        const std::string tpKey =
            params.deliverySystem + "|" + std::to_string(params.frequencyKHz) +
            "|" + std::to_string(params.symbolRateK) + "|" + params.polarity +
            "|" + std::to_string(params.streamId) + "|" +
            std::to_string(params.diseqcSource) + "|" + params.unicableMode +
            "|" + std::to_string(params.unicableUserBand) + "|" +
            std::to_string(params.unicableFrequencyKHz);
        if (activeTps[key].insert(tpKey).second) {
            if (!item.isMember("active_transponders"))
                item["active_transponders"] = Json::Value(Json::arrayValue);
            Json::Value tp;
            tp["frequency_khz"] = params.frequencyKHz;
            tp["symbol_rate"] = params.symbolRateK;
            tp["polarity"] = params.polarity;
            tp["delivery_system"] = params.deliverySystem;
            tp["stream_id"] = params.streamId;
            tp["diseqc_source"] = params.diseqcSource;
            item["active_transponders"].append(tp);
        }
        item["frequency_khz"] = params.frequencyKHz;
        item["symbol_rate"] = params.symbolRateK;
            item["bandwidth_hz"] = params.bandwidthHz;
        item["polarity"] = params.polarity;
        item["delivery_system"] = params.deliverySystem;
        item["modulation"] = params.modulation;
        item["fec"] = params.fec;
        item["stream_id"] = params.streamId;
        item["diseqc_source"] = params.diseqcSource;
        item["lnb_lof1_khz"] = params.lnbLof1KHz;
        item["lnb_lof2_khz"] = params.lnbLof2KHz;
        item["lnb_slof_khz"] = params.lnbSlofKHz;
        item["unicable_mode"] = params.unicableMode;
        item["unicable_user_band"] = params.unicableUserBand;
        item["unicable_frequency_khz"] = params.unicableFrequencyKHz;
        item["unicable_position"] = params.unicablePosition;
        item["unicable_pin"] = params.unicablePin;
    }

    if (root.isMember("adapters") && root["adapters"].isArray()) {
        for (Json::ArrayIndex i = 0; i < root["adapters"].size(); ++i) {
            auto& adapter = root["adapters"][i];
            const auto key = std::make_pair(adapter.get("adapter", 0).asInt(), adapter.get("frontend", 0).asInt());
            const auto found = usage.find(key);
            // The source that actually owns the kernel frontend may be
            // in warm standby, with zero service consumers. This is still a
            // frontend in use by DVBStreamer5, not merely "assigned".
            const bool ownFrontend =
                dvbstreamer5::media::network::LinuxDvbInput::hasActiveFrontend(
                    key.first, key.second);
            adapter["frontend_active"] = ownFrontend;
            if (found == usage.end()) {
                const bool externallyBusy = adapter.get("external_in_use", false).asBool();
                adapter["in_use"] = externallyBusy || ownFrontend;
                adapter["usage_state"] = externallyBusy
                    ? (ownFrontend ? "conflict" : "external")
                    : (ownFrontend ? "active" : "free");
                adapter["consumers"] = 0;
                adapter["configured_streams"] = 0;
                adapter["ondemand_streams"] = 0;
                adapter["active_transponder_count"] = 0;
                adapter["active_transponders"] = Json::Value(Json::arrayValue);
            } else {
                const unsigned configured = found->second.get("configured_streams", 0).asUInt();
                const unsigned consumers = found->second.get("consumers", 0).asUInt();
                const bool externallyBusy = adapter.get("external_in_use", false).asBool();
                adapter["in_use"] = externallyBusy || ownFrontend ||
                    configured > 0 || consumers > 0;
                adapter["usage_state"] = externallyBusy
                    ? ((configured > 0 || consumers > 0 || ownFrontend)
                        ? "conflict" : "external")
                    : ((ownFrontend || consumers > 0) ? "active"
                        : (configured > 0 ? "assigned" : "free"));
                adapter["active_transponders"] = found->second.get(
                    "active_transponders", Json::Value(Json::arrayValue));
                adapter["active_transponder_count"] =
                    static_cast<unsigned>(adapter["active_transponders"].size());
                adapter["consumers"] = consumers;
                adapter["configured_streams"] = configured;
                adapter["ondemand_streams"] = found->second.get("ondemand_streams", 0);
                if (found->second.isMember("frequency_khz")) {
                    adapter["frequency_khz"] = found->second.get("frequency_khz", 0);
                    adapter["symbol_rate"] = found->second.get("symbol_rate", 0);
                    adapter["bandwidth_hz"] = found->second.get("bandwidth_hz", 8000000);
                    adapter["polarity"] = found->second.get("polarity", "");
                    adapter["delivery_system"] = found->second.get("delivery_system", "");
                    adapter["modulation"] = found->second.get("modulation", "auto");
                    adapter["fec"] = found->second.get("fec", "auto");
                    adapter["stream_id"] = found->second.get("stream_id", -1);
                    adapter["diseqc_source"] = found->second.get("diseqc_source", -1);
                    adapter["lnb_lof1_khz"] = found->second.get("lnb_lof1_khz", 9750000);
                    adapter["lnb_lof2_khz"] = found->second.get("lnb_lof2_khz", 10600000);
                    adapter["lnb_slof_khz"] = found->second.get("lnb_slof_khz", 11700000);
                    adapter["unicable_mode"] = found->second.get("unicable_mode", "none");
                    adapter["unicable_user_band"] = found->second.get("unicable_user_band", -1);
                    adapter["unicable_frequency_khz"] = found->second.get("unicable_frequency_khz", 0);
                    adapter["unicable_position"] = found->second.get("unicable_position", 0);
                    adapter["unicable_pin"] = found->second.get("unicable_pin", -1);
                }
                if (found->second.isMember("streams")) {
                    adapter["streams"] = found->second["streams"];
                }
            }
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::caManagerStatus() {
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, CardManager::instance().snapshot());
}

std::string HttpServer::handleCamClientSettings(const std::string& body) {
    Json::Value response;
    Json::Value request;
    Json::CharReaderBuilder readerBuilder;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["result"] = "error";
        response["error"] = "invalid CAM client settings: " + errors;
    } else {
        const auto previous = configManager.config.camClients;
        auto reject = [&](const std::string& message) {
            Json::Value rejected;
            rejected["result"]="error"; rejected["error"]=message;
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer,rejected);
        };
        auto parseClient = [&](const Json::Value& item, CamClientConfig& updated) {
            if (!item.isObject()) return false;
            if (item.isMember("id") && !item["id"].isString()) return false;
            if (!item.isMember("id") && !item["client_id"].isString()) return false;
            updated.id = item.get("id", item.get("client_id","")).asString();
            if (updated.id.empty()) return false;
            const auto old = std::find_if(previous.begin(),previous.end(),[&](const auto& c){return c.id==updated.id;});
            const bool exists=old!=previous.end();
            auto name=item.get("name",exists?old->name:updated.id);
            auto limit=item.get("max_services",exists?old->maxServices:10u);
            auto kind=item.get("backend_id",exists?old->backendId:"newcamd");
            if (!name.isString() || !kind.isString() || !camsettings::bounded(limit,1,CardManager::kMaxConfigurableServices)) return false;
            updated.name=name.asString();
            updated.maxServices=limit.asUInt();
            updated.backendId=kind.asString();
            if (updated.backendId.empty() || updated.backendId=="passthrough") return false;
            Json::Value oldConfig(Json::objectValue);
            if(exists && old->backendId==updated.backendId) camsettings::decode(old->backendConfig,oldConfig);
            Json::Value patch(Json::objectValue);
            if(item.isMember("backend_config")) patch=item["backend_config"];
            else for(const auto* key:{"host","port","user","pass","des","username","password","des_key","protocol","caid","ecm_pid"})
                if(item.isMember(key)) patch[key]=item[key];
            Json::Value merged;
            if(!camsettings::merge(updated.backendId,patch,oldConfig,merged)) return false;
            updated.backendConfig=camsettings::encode(merged);
            return true;
        };

        std::vector<CamClientConfig> nextClients;
        std::set<std::string> ids;
        if (request.isObject() && request.isMember("clients")) {
            if(!request["clients"].isArray()) return reject("invalid_cam_settings");
            for (const auto& item : request["clients"]) {
                CamClientConfig client;
                if(!parseClient(item,client) || !ids.insert(client.id).second) return reject("invalid_cam_settings");
                nextClients.push_back(std::move(client));
            }
        } else {
            CamClientConfig updated;
            if(!parseClient(request,updated)) return reject("invalid_cam_settings");
            nextClients=previous;
            auto old=std::find_if(nextClients.begin(),nextClients.end(),[&](const auto& c){return c.id==updated.id;});
            if(old==nextClients.end()) nextClients.push_back(std::move(updated));
            else *old=std::move(updated);
        }
        configManager.config.camClients=nextClients;
        if (!configManager.save()) {
            configManager.config.camClients=previous;
            response["result"] = "error";
            response["error"] = "failed to save CAM client settings";
        } else {
            CardManager::instance().configure(configManager.config.camClients);
            response = CardManager::instance().snapshot();
            response["result"] = "ok";
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleDvbTune(const std::string& body, bool scan) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid DVB request: " + errors;
    } else if (scan && request.get("discover_lnb", false).asBool()) {
        // Discovery must never retune a configured frontend, including
        // idle OnDemand streams. An SCR User Band is exclusive on a shared coax.
        const int adapter = request.get("adapter", -1).asInt();
        const int frontend = request.get("frontend", -1).asInt();
        const int userBand = request.get("unicable_user_band", -1).asInt();
        const std::string mode = request.get("unicable_mode", "none").asString();
        const int position = request.get("unicable_position", -1).asInt();
        if (adapter < 0 || adapter > 31 || frontend < 0 || frontend > 31 ||
            userBand < 0 || (mode != "en50494" && mode != "en50607") ||
            position < 0 || position >= (mode == "en50494" ? 2 : 4)) {
            response["error"] = "LNB discovery requires a valid Unicable frontend, UB and position 0..3";
        } else {
            bool present = false;
            const Json::Value available = DvbSatellite::adapters();
            for (const auto& item : available["adapters"]) {
                if (item.get("adapter", -1).asInt() == adapter &&
                    item.get("frontend", -1).asInt() == frontend) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                response["error"] = "Selected DVB frontend is not available";
            } else {
                std::string conflict;
                auto checkInput = [&](const std::string& uri) {
                    DvbSatelliteParams existing;
                    std::string parseError;
                    if (!conflict.empty() ||
                        !DvbSatellite::parseUri(uri, existing, parseError)) return;
                    if (existing.adapter == adapter && existing.frontend == frontend) {
                        conflict = "Frontend is assigned to streams; discovery cannot interrupt them";
                    } else if (existing.unicableMode != "none" &&
                               existing.unicableUserBand == userBand) {
                        conflict = "User Band is assigned to another stream; choose a free UB";
                    }
                };
                for (const auto& stream : configManager.config.streams)
                    checkInput(stream.inputUri);
                const auto snap = streamManager.snapshot();
                for (const auto& [id, state] : snap) {
                    if (state && state->running.load()) checkInput(state->config.inputUri);
                }
                if (!conflict.empty()) {
                    response["error"] = conflict;
                } else {
                    response = DvbSatellite::scan(request);
                    response["discover_lnb"] = true;
                }
            }
        }
    } else {
        response = scan ? DvbSatellite::scan(request) : DvbSatellite::signal(request);
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

// V539: only configured, stopped channels in the *same* physical transponder
// may be retuned as a group. No new channel objects, output/CAM settings,
// restart requests or other input URIs are changed by this endpoint.
std::string HttpServer::handleDvbTransponderEdit(const std::string& body) {
    Json::Value response(Json::objectValue);
    const auto fail=[&](const std::string& message) {
        response["result"]="error";response["error"]=message;
        Json::StreamWriterBuilder writer;writer["indentation"]="";
        return Json::writeString(writer,response);
    };
    if(body.size()>65536U)return fail("Transponder edit request too large");
    Json::CharReaderBuilder readerBuilder;Json::Value root;
    std::istringstream input(body);std::string parseError;
    if(!Json::parseFromStream(readerBuilder,input,&root,&parseError) ||
       !root.isObject()||!root["streams"].isArray() ||
       root["streams"].empty()||root["streams"].size()>256U ||
       !root["tune"].isObject())
        return fail("Invalid transponder edit payload");
    const auto& tune=root["tune"];
    const auto validNumber=[&](const char* name,double min,double max) {
        return tune.isMember(name)&&tune[name].isNumeric() &&
               std::isfinite(tune[name].asDouble()) &&
               tune[name].asDouble()>=min && tune[name].asDouble()<=max;
    };
    if(!tune["delivery_system"].isString())
        return fail("Missing DVB delivery system");
    const std::string system=tune["delivery_system"].asString();
    const bool terrestrial=system=="dvb-t" || system=="dvb-t2";
    const bool satellite=system=="dvb-s" || system=="dvb-s2";
    if(!terrestrial && !satellite)
        return fail("Unsupported DVB delivery system");
    const auto validBandwidth=[&]() {
        if(!validNumber("bandwidth_hz",1712000,10000000))return false;
        const int hz=tune["bandwidth_hz"].asInt();
        return hz==1712000 || hz==5000000 || hz==6000000 ||
               hz==7000000 || hz==8000000 || hz==10000000;
    };
    if(!validNumber("adapter",0,31) || !validNumber("frontend",0,31) ||
       !validNumber("frequency_mhz",terrestrial?42:900,terrestrial?1002:14000) ||
       (terrestrial?!validBandwidth():!validNumber("symbol_rate",100,60000)) ||
       !validNumber("diseqc_source",-1,3) ||
       !validNumber("stream_id",-1,255) ||
       !validNumber("lnb_lof1_mhz",0,14000) ||
       !validNumber("lnb_lof2_mhz",0,14000) ||
       !validNumber("lnb_slof_mhz",0,14000) ||
       !validNumber("unicable_user_band",-1,31) ||
       !validNumber("unicable_frequency_mhz",0,2150) ||
       !validNumber("unicable_position",0,63) ||
       !validNumber("unicable_pin",-1,255) ||
       !tune["delivery_system"].isString() ||
       !tune["polarity"].isString() ||
       !tune["modulation"].isString() ||
       !tune["fec"].isString() ||
       !tune["unicable_mode"].isString() ||
       !root.get("satellite_name","").isString())
        return fail("Invalid DVB tuning fields");
    const auto name=root.get("satellite_name","").asString();
    if(name.size()>80 || name.find_first_of("\r\n")!=std::string::npos)
        return fail("Invalid satellite display name");
    const int adapter=static_cast<int>(tune["adapter"].asInt());
    const int frontend=static_cast<int>(tune["frontend"].asInt());
    const auto mhzToKhz=[](const double mhz)->uint32_t {
        return static_cast<uint32_t>(std::llround(mhz*1000.0));
    };
    std::vector<std::pair<std::size_t,std::string>> updates;
    std::set<std::string> requested;
    DvbSatelliteParams reference;
    bool haveReference=false;
    for(const auto& item:root["streams"]) {
        if(!item.isObject()||!item["id"].isString()||
           !item["expected_input_uri"].isString())
            return fail("Missing stream ID or original URI");
        const std::string id=item["id"].asString();
        const std::string expected=item["expected_input_uri"].asString();
        if(id.empty()||id.size()>128U || !requested.insert(id).second ||
           expected.size()>4096U)
            return fail("Invalid or duplicate stream identity");
        auto found=std::find_if(configManager.config.streams.begin(),
            configManager.config.streams.end(),
            [&](const StreamConfig& cfg){return cfg.id==id;});
        if(found==configManager.config.streams.end() ||
           found->inputUri!=expected)
            return fail("Channel configuration changed; reload before editing: "+id);
        DvbSatelliteParams original;std::string error;
        if(!DvbSatellite::parseUri(found->inputUri,original,error) ||
           (terrestrial ?
             (original.deliverySystem!="dvb-t" && original.deliverySystem!="dvb-t2") :
             (original.deliverySystem!="dvb-s" && original.deliverySystem!="dvb-s2")) ||
           original.adapter!=adapter || original.frontend!=frontend)
            return fail("Selected channel is not on the expected DVB frontend: "+id);
        if(haveReference &&
           (original.frequencyKHz!=reference.frequencyKHz ||
            original.bandwidthHz!=reference.bandwidthHz ||
            original.symbolRateK!=reference.symbolRateK ||
            original.polarity!=reference.polarity ||
            original.deliverySystem!=reference.deliverySystem ||
            original.streamId!=reference.streamId ||
            original.diseqcSource!=reference.diseqcSource ||
            original.unicableMode!=reference.unicableMode ||
            original.unicablePosition!=reference.unicablePosition))
            return fail("Selected channels belong to different transponders/multiplexes");
        reference=original;haveReference=true;
        if(streamManager.isStreamActive(id)||streamManager.isStopPending(id))
            return fail("Stop all transponder channels before saving: "+id);
        DvbSatelliteParams updated=original;
        updated.frequencyKHz=mhzToKhz(tune["frequency_mhz"].asDouble());
        if(terrestrial)updated.bandwidthHz=tune["bandwidth_hz"].asUInt();
        else updated.symbolRateK=tune["symbol_rate"].asUInt();
        updated.polarity=tune["polarity"].asString();
        updated.deliverySystem=tune["delivery_system"].asString();
        updated.modulation=tune["modulation"].asString();
        updated.fec=tune["fec"].asString();
        updated.diseqcSource=tune["diseqc_source"].asInt();
        updated.streamId=tune["stream_id"].asInt();
        updated.lnbLof1KHz=mhzToKhz(tune["lnb_lof1_mhz"].asDouble());
        updated.lnbLof2KHz=mhzToKhz(tune["lnb_lof2_mhz"].asDouble());
        updated.lnbSlofKHz=mhzToKhz(tune["lnb_slof_mhz"].asDouble());
        updated.unicableMode=tune["unicable_mode"].asString();
        updated.unicableUserBand=tune["unicable_user_band"].asInt();
        updated.unicableFrequencyKHz=mhzToKhz(
            tune["unicable_frequency_mhz"].asDouble());
        updated.unicablePosition=tune["unicable_position"].asInt();
        updated.unicablePin=tune["unicable_pin"].asInt();
        // DVB-T/T2 is an RF multiplex, not an LNB. Keep its physical tuning
        // clean even when the shared editor still contains satellite fields.
        if(terrestrial){
            updated.symbolRateK=0;
            updated.polarity.clear();
            updated.diseqcSource=-1;
            updated.unicableMode="none";
            updated.unicableUserBand=-1;
            updated.unicableFrequencyKHz=0;
            updated.unicablePosition=0;
            updated.unicablePin=-1;
        }
        std::string validationError;
        DvbSatelliteParams check;
        if(!DvbSatellite::parseUri(DvbSatellite::buildUri(updated),
                                  check,validationError))
            return fail("Invalid updated transponder: "+validationError);
        const std::string uri=dvbstreamer5::dvb_tp_editor::editUri(
            expected,DvbSatellite::buildUri(check),name);
        DvbSatelliteParams checkedUri;
        if(uri.empty() ||
           !DvbSatellite::parseUri(uri,checkedUri,validationError) ||
           checkedUri.deliverySystem!=system ||
           checkedUri.frequencyKHz!=check.frequencyKHz ||
           (terrestrial && checkedUri.bandwidthHz!=check.bandwidthHz))
            return fail("Cannot preserve channel tuning URI: "+id);
        updates.emplace_back(static_cast<std::size_t>(std::distance(
            configManager.config.streams.begin(),found)),uri);
    }
    // Require *all* configured services on the old transponder. No
    // partial retune of services sharing one physical RF input.
    for(const auto& cfg:configManager.config.streams){
        DvbSatelliteParams other;std::string error;
        if(!DvbSatellite::parseUri(cfg.inputUri,other,error) ||
           other.adapter!=reference.adapter ||
           other.frontend!=reference.frontend ||
           other.frequencyKHz!=reference.frequencyKHz ||
           other.bandwidthHz!=reference.bandwidthHz ||
           other.symbolRateK!=reference.symbolRateK ||
           other.polarity!=reference.polarity ||
           other.streamId!=reference.streamId ||
           other.deliverySystem!=reference.deliverySystem ||
           other.diseqcSource!=reference.diseqcSource ||
           other.unicableMode!=reference.unicableMode ||
           other.unicablePosition!=reference.unicablePosition)continue;
        if(!requested.count(cfg.id))
            return fail("Not all services from this transponder were selected");
    }
    // Even a different TP on the same frontend must not be interrupted.
    const auto live=streamManager.snapshot();
    for(const auto& [id,state]:live) {
        if(!state || !state->running.load(std::memory_order_acquire))continue;
        DvbSatelliteParams params;std::string error;
        if(DvbSatellite::parseUri(state->config.inputUri,params,error) &&
           params.adapter==adapter && params.frontend==frontend)
            return fail("DVB frontend still has an active consumer");
    }
    if(dvbstreamer5::media::network::LinuxDvbInput::hasActiveFrontend(
        adapter,frontend))
        return fail("DVB frontend is still occupied");
    const auto before=configManager.config.streams;
    for(const auto& [index,uri]:updates)
        configManager.config.streams[index].inputUri=uri;
    if(!configManager.save()) {
        configManager.config.streams=before;
        return fail("Failed to save transponder changes");
    }
    streamManager.configureAutoStartRecovery();
    response["result"]="ok";
    response["changed"]=Json::UInt64(updates.size());
    Json::StreamWriterBuilder writer;writer["indentation"]="";
    return Json::writeString(writer,response);
}

std::string HttpServer::handleDvbAddChannels(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid channel selection: " + errors;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    if (!request["channels"].isArray() || request["channels"].size() == 0) {
        response["error"] = "No satellite channels selected";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    std::string outputType = toLower(request.get("output_type", "udp-vbr").asString());
    static const std::set<std::string> kSatelliteOutputTypes = {
        "udp-vbr", "udp-cbr", "rtp", "srt", "http", "hls", "rtsp", "rtmp", "youtube"
    };
    if (!kSatelliteOutputTypes.count(outputType)) {
        response["error"] = "Unsupported satellite output protocol: " + outputType;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    std::string outputMode = toLower(request.get("output_mode", "listener").asString());
    if (outputMode != "listener" && outputMode != "caller") outputMode = "listener";
    if (outputType != "srt") outputMode = "listener";

    std::string outputHost = request.get("output_host", "").asString();
    if (outputHost.empty()) {
        if (outputType == "udp-vbr" || outputType == "udp-cbr" || outputType == "rtp") {
            outputHost = "239.255.10.1";
        } else if (outputType == "srt" && outputMode == "listener") {
            outputHost = "0.0.0.0";
        } else {
            outputHost = "127.0.0.1";
        }
    }

    const int defaultPort =
        (outputType == "srt" ? 7001 :
         (outputType == "rtsp" ? 8554 :
          ((outputType == "rtmp" || outputType == "youtube") ? 1935 :
           ((outputType == "http" || outputType == "hls") ? configManager.config.httpPort : 5000))));
    int basePort = std::clamp(request.get("base_port", defaultPort).asInt(), 1, 65535);
    const std::string interfaceAddress = request.get("interface_address", "").asString();
    std::string activationMode = toLower(request.get("activation_mode", "online").asString());
    if (activationMode != "ondemand") activationMode = "online";
    const bool autoStart = request.get("auto_start", false).asBool();

    bool cbr = request.get("cbr", outputType == "udp-cbr").asBool();
    if (outputType == "udp-cbr") cbr = true;
    if (outputType == "udp-vbr" || outputType == "rtp" || outputType == "rtsp" ||
        outputType == "rtmp" || outputType == "youtube") {
        cbr = false;
    }

    const int srtLatencyMs = std::clamp(request.get("srt_latency_ms", 120).asInt(), 20, 60000);
    const std::string srtPassphrase = request.get("srt_passphrase", "").asString();
    const std::string srtStreamId = request.get("srt_streamid", "").asString();
    int srtPbKeyLen = request.get("srt_pbkeylen", 16).asInt();
    if (srtPbKeyLen != 16 && srtPbKeyLen != 24 && srtPbKeyLen != 32) srtPbKeyLen = 16;

    const bool sharedHttpPort = outputType == "http" || outputType == "hls";
    if (activationMode == "ondemand" && !sharedHttpPort) {
        response["error"] = "OnDemand requires HTTP or HLS output";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }
    const std::string conditionalAccessClient = request.get("conditional_access_client", "").asString();
    bool hasScrambledSelection = false;
    for (const auto& selected : request["channels"]) {
        if (selected.get("scrambled", false).asBool()) { hasScrambledSelection = true; break; }
    }
    if (hasScrambledSelection && (conditionalAccessClient.empty() || conditionalAccessClient == "auto")) {
        response["error"] = "Select a CAM client for scrambled channels";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }
    // Satellite label is presentation metadata in DVB URI, not a tune parameter.
    // Ignore it for duplicate detection so rescanning cannot clone existing SIDs.
    const auto tuneIdentity = [](const std::string& uri) {
        const auto question = uri.find('?');
        if (question == std::string::npos) return uri;
        const std::string base = uri.substr(0, question + 1);
        const std::string query = uri.substr(question + 1);
        std::string filtered;
        std::istringstream parts(query);
        std::string part;
        while (std::getline(parts, part, '&')) {
            if (part == "satellite_name" || part.rfind("satellite_name=", 0) == 0) continue;
            if (!filtered.empty()) filtered += '&';
            filtered += part;
        }
        return base + filtered;
    };
    const uint64_t targetBitrate = std::clamp<uint64_t>(
        request.get("target_bitrate_kbps", Json::UInt64(12000)).asUInt64(), 500, 100000) * 1000ULL;

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::set<int> occupiedPorts;
    for (const auto& existing : configManager.config.streams) {
        if (existing.outputHost == outputHost) occupiedPorts.insert(existing.outputPort);
        for (const auto& extra : existing.additionalOutputs) {
            if (extra.outputHost == outputHost) occupiedPorts.insert(extra.outputPort);
        }
    }
    int nextPort = basePort;
    size_t created = 0;
    size_t skipped = 0;
    Json::Value createdItems(Json::arrayValue);

    for (Json::ArrayIndex index = 0; index < request["channels"].size(); ++index) {
        const Json::Value& item = request["channels"][index];
        const uint32_t sid = item.get("service_id", 0).asUInt();
        const std::string inputUri = item.get("input_uri", "").asString();
        if (sid == 0 || inputUri.empty()) {
            ++skipped;
            continue;
        }

        DvbSatelliteParams params;
        std::string dvbError;
        if (!DvbSatellite::parseUri(inputUri, params, dvbError)) {
            ++skipped;
            continue;
        }

        const bool duplicate = std::any_of(
            configManager.config.streams.begin(), configManager.config.streams.end(),
            [&](const StreamConfig& existing) {
                return tuneIdentity(existing.inputUri) == tuneIdentity(inputUri) &&
                    existing.inputServiceId == sid;
            });
        if (duplicate) {
            ++skipped;
            continue;
        }

        StreamConfig config;
        config.id = "sat-" + std::to_string(now) + "-" + std::to_string(sid) + "-" + std::to_string(index);
        config.name = item.get("name", "").asString();
        if (config.name.empty()) config.name = "Satellite " + std::to_string(sid);
        config.inputUri = inputUri;
        config.inputMode = "auto";
        config.testPattern = false;
        config.activationMode = activationMode;
        config.autoStart = autoStart && activationMode == "online";
        config.outputType = outputType;
        config.outputMode = outputMode;
        config.outputHost = outputHost;
        if (sharedHttpPort) {
            // HTTP/HLS channels are separated by URL path, so every selected
            // service may share the same listener port.
            config.outputPort = basePort;
        } else {
            while (nextPort <= 65535 && occupiedPorts.count(nextPort)) ++nextPort;
            if (nextPort > 65535) {
                ++skipped;
                continue;
            }
            config.outputPort = nextPort;
            occupiedPorts.insert(nextPort);
            ++nextPort;
        }
        config.interfaceAddress = interfaceAddress;
        config.cbr = cbr;
        config.targetBitrate = targetBitrate;
        config.srtOutputLatencyMs = srtLatencyMs;
        config.srtOutputPassphrase = srtPassphrase;
        config.srtOutputStreamId = srtStreamId;
        config.srtOutputPbKeyLen = srtPbKeyLen;
        config.remapEnabled = false;
        config.transcodeEnabled = false;
        config.inputServiceId = sid;
        config.serviceId = sid;
        config.serviceName = config.name;
        config.serviceProvider = item.get("provider", "").asString();
        config.conditionalAccessClient = item.get("scrambled", false).asBool()
            ? conditionalAccessClient
            : "";
        config.videoPid = 0;
        config.audioPid = 0;

        configManager.config.streams.push_back(config);
        Json::Value createdItem = config.toJson();
        createdItems.append(createdItem);
        ++created;
    }

    if (created > 0) configManager.save();
    response["result"] = "ok";
    response["created"] = Json::UInt64(created);
    response["skipped"] = Json::UInt64(skipped);
    response["channels"] = createdItems;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

bool HttpServer::handleHttpStream(tcp::socket& socket, const std::string& target,
                                  const std::string& effectiveClientIp) {
    std::string id;
    const bool publicHttp = resolveHttpMpegTsTarget(configManager.config.streams, target, id);
    if (!publicHttp) {
        if (!resolvePrivatePreviewTarget(target, id) ||
            !findStreamConfigById(configManager.config.streams, id) ||
            !streamManager.isStreamActive(id)) return false;
    }
    if (id.empty()) return false;
    if (publicHttp) {
        std::string demandError;
        if (!streamManager.ensureOnDemandStreamParallel(id, "http", &demandError)) {
            const std::string body = "On-demand stream unavailable\n";
            const std::string crlf =
                std::string(1, static_cast<char>(13)) +
                std::string(1, static_cast<char>(10));
            const std::string unavailable =
                "HTTP/1.1 503 Service Unavailable" + crlf +
                "Server: DVBStreamer5" + crlf +
                "Content-Type: text/plain; charset=utf-8" + crlf +
                "Retry-After: 1" + crlf +
                "Connection: close" + crlf +
                "Content-Length: " + std::to_string(body.size()) + crlf + crlf + body;
            boost::asio::write(socket, boost::asio::buffer(unavailable));
            return true;
        }
    }

    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Server: DVBStreamer5\r\n"
        "Content-Type: video/MP2T\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    boost::asio::write(socket, boost::asio::buffer(header));
    boost::system::error_code endpointError;
    const auto address = socket.remote_endpoint(endpointError);
    const std::string clientIp = !effectiveClientIp.empty() ? effectiveClientIp :
        (endpointError ? std::string{} : address.address().to_string());
    int fd = socket.release();
    std::string privateId, previewToken;
    if (resolvePrivatePreviewTarget(target, privateId)) {
        // Validated by the GET handler before the response header is written.
        if (!privatePreviewTokenFromTarget(target, previewToken)) {
            ::close(fd);
            return false;
        }
    }
    std::string middlewareSession;
    if(publicHttp && !endpointError &&
       dvbstreamer5::stream_auth::enabled(configManager.config,id)){
        const auto q=target.find('?');
        middlewareSession=dvbstreamer5::stream_auth::SessionRegistry::idFor(
            "http",id,clientIp,q==std::string::npos?"":target.substr(q+1));
    }
    streamManager.addHttpClient(id, fd, endpointError ? "" : clientIp,
                                previewToken,middlewareSession);
    return true;
}

bool HttpServer::serveHlsFile(const tcp::socket& socket, const std::string& target,
                              http::response<http::string_body>& res,
                              std::filesystem::path* directFile,
                              const std::string& effectiveClientIp) {
    if(directFile)directFile->clear();
    std::string id;
    std::string fileName;
    if (!resolveHlsTarget(configManager.config.streams, target, id, fileName) ||
        fileName.find("..") != std::string::npos) {
        return false;
    }

    const StreamConfig* cfg = findStreamConfigById(configManager.config.streams, id);
    if (!cfg) return false;

    const bool wasActive = streamManager.isStreamActive(id);
    const auto hlsStartupBegin = std::chrono::steady_clock::now();
    std::string demandError;
    if (!streamManager.ensureOnDemandStreamParallel(id, "hls", &demandError)) {
        res.result(http::status::service_unavailable);
        res.set(http::field::content_type, "text/plain");
        res.set("Retry-After", "1");
        res.body() = "On-demand stream unavailable";
        return true;
    }
    const auto activationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hlsStartupBegin).count();

    const std::filesystem::path filePath = hlsStorageDirectory(*cfg) / fileName;
    const std::string archivePlaylist = buildHlsArchivePlaylist(*cfg, fileName);
    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {
        // An old OnDemand playlist can survive the previous generation until the
        // new segmenter clears its directory. A live manifest is coherent only
        // when its first advertised media URI also exists.
        const auto livePlaylistReady = [&filePath]() {
            std::error_code ec;
            if (!std::filesystem::exists(filePath, ec) || ec ||
                !std::filesystem::is_regular_file(filePath, ec) || ec) {
                return false;
            }

            std::ifstream probe(filePath, std::ios::binary);
            if (!probe.is_open()) return false;
            std::string line;
            while (std::getline(probe, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line.front() == '#') continue;
                const std::string relative = cleanHlsRelativePath(line);
                if (relative.empty()) return false;
                const auto mediaPath = filePath.parent_path() / relative;
                ec.clear();
                return std::filesystem::exists(mediaPath, ec) && !ec &&
                       std::filesystem::is_regular_file(mediaPath, ec) && !ec;
            }
            return false;
        };

        // Passthrough OnDemand keeps the short 3-s probe and 503/Retry-After
        // behavior, avoiding excessive HTTP worker occupancy for copy-mode.
        // V339: the first independent HLS segment of a transcoded OnDemand
        // service cannot be ready until decoder, encoder and an IDR-aligned
        // segment have all started. The old unconditional 3-s probe budget
        // returned 503 while a valid 4-s segment was still being assembled.
        // V530: the short-segment transcoded OnDemand profile now publishes
        // a playable playlist only after three COMPLETE segments (~10-14 s
        // for the default 4s target). Give this narrowly-scoped cold start
        // up to 20s, otherwise VLC may abandon the first 503 before its
        // initial playable buffer exists. Passthrough retains 3s and other
        // transcodes retain 12s. This does not alter online playlists.
        const bool prefilledOnDemandTranscode = cfg->transcodeEnabled &&
            toLower(cfg->transcodeVideoCodec) != "copy" &&
            (cfg->hlsSegmentDuration == 0U || cfg->hlsSegmentDuration <= 4U);
        const int maxStartupAttempts =
            prefilledOnDemandTranscode ? 200 : (cfg->transcodeEnabled ? 120 : 30);
        bool ready = livePlaylistReady();
        int startupAttempts = 0;
        for (; startupAttempts < maxStartupAttempts && !ready; ++startupAttempts) {
            if ((startupAttempts % 10) == 0) {
                std::string startupKeepaliveError;
                if (!streamManager.ensureOnDemandStreamParallel(
                        id, "hls-startup-wait", &startupKeepaliveError)) {
                    demandError = startupKeepaliveError;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ready = livePlaylistReady();
        }

        const auto coherentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - hlsStartupBegin).count();
        if (!ready) {
            res.result(http::status::service_unavailable);
            res.set(http::field::content_type, "text/plain");
            res.set("Retry-After", "1");
            res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
            res.set("Access-Control-Allow-Origin", "*");
            res.keep_alive(false);
            res.body() = "HLS starting";
            std::cerr << "HLS STARTUP PENDING stream=" << id
                      << " activation_ms=" << activationMs
                      << " coherent_wait_ms=" << coherentMs
                      << " attempts=" << startupAttempts
                      << " max_wait_ms=" << maxStartupAttempts * 100
                      << " transcode=" << (cfg->transcodeEnabled ? 1 : 0);
            if (!demandError.empty()) std::cerr << " error=" << demandError;
            std::cerr << std::endl;
            return true;
        }

        if (!wasActive || startupAttempts > 0) {
            std::cerr << "HLS STARTUP READY stream=" << id
                      << " activation_ms=" << activationMs
                      << " first_media_ms=" << coherentMs
                      << " attempts=" << startupAttempts
                      << std::endl;
        }
    }
    if (!archivePlaylist.empty()) {
        res.body() = archivePlaylist;
    } else {
        if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
            res.result(http::status::not_found);
            res.set(http::field::content_type, "text/plain");
            res.body() = "Not Found";
            return true;
        }
        std::error_code sizeError;
        const auto diskBytes=std::filesystem::file_size(filePath,sizeError);
        if(sizeError){
            res.result(http::status::not_found);
            res.body()="Not Found";
            return true;
        }
        if(directFile && filePath.extension()!=".m3u8" &&
           diskBytes>dvbstreamer5::media::hls::HlsFileCache::kMaxFileBytes){
            // Authorization/on-demand checks already completed above. The
            // actual bytes are sent with sendfile() after MIME headers.
            *directFile=filePath;
        }else if(!dvbstreamer5::media::hls::HlsFileCache::instance()
                     .readSmallFile(filePath,res.body())){
            // A replaced/truncated file is retried on the next HLS request.
            res.result(http::status::service_unavailable);
            res.set("Retry-After","1");
            res.body()="HLS file updating";
            return true;
        }
    }
    // Shared HLS segments remain unchanged; token is propagated into each
    // relative URI, including key and init-map resources.
    if (dvbstreamer5::stream_auth::enabled(configManager.config,id) &&
        filePath.extension()==".m3u8") {
        const auto q=target.find('?');
        if(q!=std::string::npos)
            dvbstreamer5::stream_auth::propagateHlsQuery(res.body(),target.substr(q+1));
    }
    boost::system::error_code endpointError;
    const auto address = socket.remote_endpoint(endpointError);
    const std::string clientIp = !effectiveClientIp.empty() ? effectiveClientIp :
        (endpointError ? std::string{} : address.address().to_string());
    // V509: HLS playlist activity is not playback. Record one logical
    // viewer only once a real media segment has been successfully served.
    // Covers transport stream, CMAF fragments, and init segments.
    const std::string segmentExt = filePath.extension().string();
    const bool mediaDelivered = res.result() == http::status::ok &&
        (segmentExt == ".ts" || segmentExt == ".m4s" ||
         segmentExt == ".mp4" || segmentExt == ".aac");
    if (!endpointError && !clientIp.empty() && mediaDelivered) {
        streamManager.addStreamSession(id, clientIp, "hls");
    }
    // V340: a successful media response (not merely a startup playlist)
    // proves HLS playback has reached its first real segment. End the cold
    // OnDemand grace; normal no-client inactivity applies from this point.
    // No effect on HTTP MPEG-TS, other protocols or online HLS.
    if (cfg->activationMode == "ondemand" &&
        (filePath.extension() == ".ts" || filePath.extension() == ".m4s")) {
        streamManager.markOnDemandHlsFirstMedia(id);
    }
    if (filePath.extension() == ".m3u8") res.set(http::field::content_type, "application/vnd.apple.mpegurl");
    else if (filePath.extension() == ".m4s") res.set(http::field::content_type, "video/iso.segment");
    else if (filePath.extension() == ".mp4") res.set(http::field::content_type, "video/mp4");
    else if (filePath.extension() == ".bin") res.set(http::field::content_type, "application/octet-stream");
    else res.set(http::field::content_type, "video/MP2T");
    res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
    res.set("Access-Control-Allow-Origin", "*");
    res.keep_alive(false);
    return true;
}


