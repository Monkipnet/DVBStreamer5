#!/usr/bin/env python3
from pathlib import Path


def replace_exact(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"anchor not found in {path}: {old[:120]!r}")
    text = text.replace(old, new, 1)
    p.write_text(text)

# Built-in means statically linked, not passthrough. Only the explicit
# passthrough backend may bypass the CA lifecycle and TS processing hooks.
path = "src/CaBackend.cpp"
replace_exact(path,
'''bool CaBackendManager::ensureReaderOpenLocked(LoadedBackend& backend,
                                               const CaBackendReaderBinding& reader,
                                               std::string& error) {
    if (backend.builtin) {
        ++backend.readerRefs[reader.key];
        return true;
    }
    auto refs = backend.readerRefs.find(reader.key);
''',
'''bool CaBackendManager::ensureReaderOpenLocked(LoadedBackend& backend,
                                               const CaBackendReaderBinding& reader,
                                               std::string& error) {
    if (backend.id == "passthrough") {
        ++backend.readerRefs[reader.key];
        return true;
    }
    auto refs = backend.readerRefs.find(reader.key);
''')
replace_exact(path,
'''    if (!backend.builtin && backend.api && backend.api->close_reader) {
        backend.api->close_reader(backend.instance, readerKey.c_str());
    }
''',
'''    if (backend.id != "passthrough" && backend.api && backend.api->close_reader) {
        backend.api->close_reader(backend.instance, readerKey.c_str());
    }
''')
replace_exact(path,
'''    session.backendId = backend->id;
    session.passthrough = backend->builtin || !(backend->capabilities & DVBSTREAMER5_CA_CAP_TS_INPLACE);
    session.status = session.passthrough ? "PASSTHROUGH_NO_DECODE" : "BACKEND_RESERVED";

    if (!backend->builtin) {
''',
'''    session.backendId = backend->id;
    const bool passthroughBackend = backend->id == "passthrough";
    session.passthrough = passthroughBackend || !(backend->capabilities & DVBSTREAMER5_CA_CAP_TS_INPLACE);
    session.status = session.passthrough ? "PASSTHROUGH_NO_DECODE" : "BACKEND_RESERVED";

    if (!passthroughBackend) {
''')
replace_exact(path,
'''    std::cerr << "CA backend service reserved: stream=" << stream.id
              << " client=" << reader.key
              << " backend=" << backend->id
              << " mode=" << (backend->builtin ? "passthrough" : "plugin") << std::endl;
''',
'''    std::cerr << "CA backend service reserved: stream=" << stream.id
              << " client=" << reader.key
              << " backend=" << backend->id
              << " mode=" << (passthroughBackend ? "passthrough" :
                              (backend->builtin ? "builtin" : "plugin")) << std::endl;
''')
replace_exact(path,
'''    bool builtin = true;
    bool closeReader = false;
''',
'''    bool passthroughBackend = true;
    bool closeReader = false;
''')
replace_exact(path,
'''        builtin = backend->builtin;
        api = backend->api;
''',
'''        passthroughBackend = backend->id == "passthrough";
        api = backend->api;
''')
replace_exact(path,
'''    if (!builtin && api && api->stop_service) {
        api->stop_service(instance, streamId.c_str());
    }
    if (closeReader && !builtin && api && api->close_reader) {
        api->close_reader(instance, session.readerKey.c_str());
    }
''',
'''    if (!passthroughBackend && api && api->stop_service) {
        api->stop_service(instance, streamId.c_str());
    }
    if (closeReader && !passthroughBackend && api && api->close_reader) {
        api->close_reader(instance, session.readerKey.c_str());
    }
''')
replace_exact(path,
'''    LoadedBackend* backend = findBackendLocked(session.backendId);
    if (!backend || backend->builtin || !backend->api || !backend->api->process_ts) {
        session.status = "PASSTHROUGH_NO_DECODE";
        return true;
    }
''',
'''    LoadedBackend* backend = findBackendLocked(session.backendId);
    if (!backend || backend->id == "passthrough" || !backend->api || !backend->api->process_ts) {
        session.status = "PASSTHROUGH_NO_DECODE";
        return true;
    }
''')
replace_exact(path,
'''    item["plugin_status_deferred"] = !backend.builtin && backend.api && backend.api->status_json;
''',
'''    item["plugin_status_deferred"] = backend.id != "passthrough" && backend.api && backend.api->status_json;
    item["backend_kind"] = backend.id == "passthrough" ? "passthrough" :
                           (backend.builtin ? "builtin" : "plugin");
''')
replace_exact(path,
'''    result["native_plugin"] = backend && !backend->builtin &&
                              (backend->capabilities & DVBSTREAMER5_CA_CAP_TS_INPLACE) != 0 &&
                              !session.passthrough;
''',
'''    result["native_plugin"] = backend && backend->id != "passthrough" &&
                              (backend->capabilities & DVBSTREAMER5_CA_CAP_TS_INPLACE) != 0 &&
                              !session.passthrough;
    result["builtin"] = backend && backend->builtin;
''')

# Match OSCam's Newcamd client message-id sequence exactly:
# LOGIN is id=0, CARD_DATA_REQ is the first incremented id (=1).
path = "src/ca/backends/newcamd/NewcamdClient.cpp"
replace_exact(path,
'''bool NewcamdClient::login() {
    if (!socket_ || !socket_->is_open()) {
''',
'''bool NewcamdClient::login() {
    msg_id_ = 0;
    authenticated_ = false;
    if (!socket_ || !socket_->is_open()) {
''')
replace_exact(path,
'''        if (!send_message(std::move(loginPayload), kClientId, 0, 0, true)) return false;
''',
'''        // OSCam Newcamd sends the login packet with message-id 0.
        if (!send_message(std::move(loginPayload), kClientId, 0, 0, false)) return false;
''')
replace_exact(path,
'''        std::vector<uint8_t> cardReq{ kMsgCardDataReq, 0, 0 };
        if (!send_message(std::move(cardReq), 0, 0, 0, false)) return false;
        Message cardData;
        if (!receive_message(cardData, false) || cardData.payload.empty()) return false;
''',
'''        std::vector<uint8_t> cardReq{ kMsgCardDataReq, 0, 0 };
        uint16_t cardReqId = 0;
        // After LOGIN_ACK OSCam increments the client message id for CARD_DATA_REQ.
        if (!send_message(std::move(cardReq), 0, 0, 0, true, &cardReqId)) return false;
        Message cardData;
        if (!receive_message(cardData, true) || cardData.payload.empty()) return false;
''')
replace_exact(path,
'''        authenticated_ = true;
        set_error({});
        return true;
''',
'''        authenticated_ = true;
        std::cerr << "NEWCAMD LOGIN OK host=" << host_
                  << " port=" << port_
                  << " user=" << user_
                  << " card_caid=0x" << std::hex << card_caid_ << std::dec
                  << " providers=" << providers_.size()
                  << " au=" << (au_enabled_ ? 1 : 0)
                  << std::endl;
        set_error({});
        return true;
''')
replace_exact(path,
'''void NewcamdClient::disconnect() {
    running_ = false;
    authenticated_ = false;
    pending_ecms_ = 0;
''',
'''void NewcamdClient::disconnect() {
    running_ = false;
    authenticated_ = false;
    pending_ecms_ = 0;
    msg_id_ = 0;
    session_key_ready_ = false;
''')

# Version bump.
path = "src/AppVersion.h"
p = Path(path)
text = p.read_text()
old = 'inline constexpr const char* kProgramVersion = "10.8.48";'
new = 'inline constexpr const char* kProgramVersion = "10.8.49";'
if old not in text:
    raise SystemExit("version anchor not found")
p.write_text(text.replace(old, new, 1))

print("V10.8.49 Newcamd patch applied")
