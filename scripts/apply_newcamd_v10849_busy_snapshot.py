#!/usr/bin/env python3
from pathlib import Path

p = Path("src/CaBackend.cpp")
text = p.read_text()
old = '''    if (!lock.owns_lock()) {
        root["abi_version"] = Json::UInt(DVBSTREAMER5_CA_BACKEND_ABI_V1);
        root["plugin_directory"] = directory;
        root["network_ca_server"] = false;
        root["external_key_export"] = false;
        root["raw_control_word_api"] = false;
        root["busy"] = true;
        root["status"] = "BACKEND_BUSY";
        root["backends"] = Json::Value(Json::arrayValue);
        return root;
    }
'''
new = '''    if (!lock.owns_lock()) {
        root["abi_version"] = Json::UInt(DVBSTREAMER5_CA_BACKEND_ABI_V1);
        root["plugin_directory"] = directory;
        root["network_ca_server"] = false;
        root["external_key_export"] = false;
        root["raw_control_word_api"] = false;
        root["busy"] = true;
        root["status"] = "BACKEND_BUSY";

        // A remote Newcamd connect/login may hold the CA manager lock for a
        // short time.  Returning an empty backend list made the web UI replace
        // the compiled-in Newcamd option with "not loaded", so the module
        // appeared to disappear exactly while it was authenticating.  The two
        // built-in backends are immutable for the lifetime of the process and
        // can therefore be advertised safely without touching backends_.
        Json::Value busyBackends(Json::arrayValue);
        Json::Value passthrough;
        passthrough["id"] = "passthrough";
        passthrough["display_name"] = "Passthrough (без декодирования)";
        passthrough["vendor"] = "DVBStreamer5";
        passthrough["path"] = "builtin";
        passthrough["builtin"] = true;
        passthrough["usable"] = true;
        passthrough["capabilities"] = Json::UInt(0);
        passthrough["backend_kind"] = "passthrough";
        busyBackends.append(passthrough);

        if (const auto* api = dvbstreamer5_ca_backend_get_api_v1()) {
            Json::Value newcamd;
            newcamd["id"] = safeString(api->backend_id);
            newcamd["display_name"] = safeString(api->display_name);
            newcamd["vendor"] = safeString(api->vendor);
            newcamd["path"] = "builtin:newcamd";
            newcamd["builtin"] = true;
            newcamd["usable"] = true;
            newcamd["capabilities"] = Json::UInt(api->capabilities);
            newcamd["ts_inplace"] =
                (api->capabilities & DVBSTREAMER5_CA_CAP_TS_INPLACE) != 0;
            newcamd["multi_service"] =
                (api->capabilities & DVBSTREAMER5_CA_CAP_MULTI_SERVICE) != 0;
            newcamd["emm_managed"] =
                (api->capabilities & DVBSTREAMER5_CA_CAP_EMM_MANAGED) != 0;
            newcamd["backend_kind"] = "builtin";
            busyBackends.append(newcamd);
        }
        root["backends"] = busyBackends;
        return root;
    }
'''
if new in text:
    print("busy snapshot patch already applied")
    raise SystemExit(0)
if old not in text:
    raise SystemExit("busy snapshot anchor not found")
p.write_text(text.replace(old, new, 1))
print("busy snapshot patch applied")
