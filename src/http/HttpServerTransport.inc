#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

namespace {
// Linux zero-copy HLS media transfer. This function is only called after the
// ordinary per-request middleware check and the existing OnDemand guard.
// No URL or token is used to construct a filesystem path here.
bool sendHlsFileZeroCopy(tcp::socket& socket,const std::filesystem::path& path,
                         const http::response<http::string_body>& response,
                         unsigned version,bool keepAlive){
    const int source=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(source<0)return false;
    struct stat info{};
    if(::fstat(source,&info)<0||!S_ISREG(info.st_mode)||
       info.st_size<0||info.st_size>static_cast<off_t>(256U*1024U*1024U)){
        ::close(source);return false;
    }
    http::response<http::empty_body> headers{response.result(),version};
    for(const auto& field:response.base())
        headers.set(field.name(),field.value());
    headers.content_length(static_cast<std::uint64_t>(info.st_size));
    headers.keep_alive(keepAlive);
    struct timeval sendLimit{2,0};
    ::setsockopt(socket.native_handle(),SOL_SOCKET,SO_SNDTIMEO,
                 &sendLimit,sizeof(sendLimit));
    try{
        http::response_serializer<http::empty_body> writer{headers};
        http::write_header(socket,writer);
    }catch(...){::close(source);return false;}
    off_t offset=0;
    while(offset<info.st_size){
        const auto sent=::sendfile(socket.native_handle(),source,&offset,
            static_cast<std::size_t>(info.st_size-offset));
        if(sent>0)continue;
        if(sent<0&&errno==EINTR)continue;
        ::close(source);return false;
    }
    ::close(source);
    return true;
}
} // namespace

HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
    : ioContext(ioc), sessionPool(kHttpSessionWorkers), qualityHistoryTimer(ioc),
      configManager(cfg), streamManager(sm),
      streamAuthApiKeyDigest_(cfg.config.streamAuthApiKeySha256),
      streamAuthApiAllowedIps_(cfg.config.streamAuthApiAllowedIps) {
    std::cerr << "HTTP worker pool 202.47: workers=" << kHttpSessionWorkers
              << " max_queued=" << kMaxQueuedHttpSessions
              << " thread_per_request=off" << std::endl;
}

bool HttpServer::start() {
    if (!bindHttpPorts(configuredHttpPorts())) return false;

    // V10.8.115: quality history belongs to the server, not to the browser.
    // Restore persisted samples, record immediately, then continue every
    // 30 seconds even when no Web UI client is connected.
    if (!qualitySamplerStarted) {
        resetSystemLoadHistoryOnStartup();
        loadQualityHistoryFromDisk();
        recordAllQualitySamples();
        qualityLastPersist = 0;
        qualitySamplerStarted = true;
        scheduleQualitySampler();
    }
    return true;
}

void HttpServer::doAccept(std::shared_ptr<tcp::acceptor> listener, int port, uint64_t generation) {
    if (!listener || !listener->is_open()) {
        return;
    }

    listener->async_accept([this, listener, port, generation](boost::system::error_code ec, tcp::socket socket) {
        if (generation != acceptGeneration.load()) {
            return;
        }
        if (!ec) {
            const uint32_t queued = queuedHttpSessions.fetch_add(1, std::memory_order_relaxed) + 1;
            if (queued > kMaxQueuedHttpSessions) {
                queuedHttpSessions.fetch_sub(1, std::memory_order_relaxed);
                std::cerr << "HTTP worker pool overload: queued=" << queued
                          << " limit=" << kMaxQueuedHttpSessions << std::endl;
                boost::system::error_code closeEc;
                socket.close(closeEc);
            } else {
                try {
                    boost::asio::post(sessionPool,
                        [this, socket = std::move(socket)]() mutable {
                            struct PendingGuard {
                                std::atomic<uint32_t>& counter;
                                ~PendingGuard() { counter.fetch_sub(1, std::memory_order_relaxed); }
                            } guard{queuedHttpSessions};
                            handleSession(std::move(socket));
                        });
                } catch (const std::exception& ex) {
                    queuedHttpSessions.fetch_sub(1, std::memory_order_relaxed);
                    std::cerr << "HTTP session enqueue failed: " << ex.what() << std::endl;
                    boost::system::error_code closeEc;
                    socket.close(closeEc);
                }
            }
        } else if (ec != boost::asio::error::operation_aborted) {
            std::cerr << "HTTP accept failed on port " << port << ": " << ec.message() << std::endl;
        }
        if (listener->is_open()) {
            doAccept(listener, port, generation);
        }
    });
}

namespace {
// Parse only an exact cookie name, not an accidental substring in another key.
std::string panelCookieValue(const http::request<http::string_body>& req,
                             const std::string& name) {
    const auto field = req.find(http::field::cookie);
    if (field == req.end()) return {};
    std::istringstream cookies(std::string(field->value()));
    std::string token;
    while (std::getline(cookies, token, ';')) {
        boost::algorithm::trim(token);
        const auto equal = token.find('=');
        if (equal != std::string::npos && token.substr(0, equal) == name)
            return token.substr(equal + 1U);
    }
    return {};
}
} // namespace

void HttpServer::handleSession(tcp::socket socket) {
    handleSession(std::move(socket),std::make_shared<boost::beast::flat_buffer>(),0);
}

// HTTP/HLS requests still pass through the existing auth/session logic.
// Between keep-alive requests the TCP socket waits asynchronously in
// io_context, not in any of the 24 synchronous HTTP request workers.
void HttpServer::scheduleHlsKeepAlive(
    tcp::socket socket,std::shared_ptr<boost::beast::flat_buffer> buffered,
    unsigned requests) {
    if(!socket.is_open())return;
    auto live=std::make_shared<tcp::socket>(std::move(socket));
    auto timer=std::make_shared<boost::asio::steady_timer>(ioContext);
    auto done=std::make_shared<std::atomic<bool>>(false);
    auto resume=[this,live,buffered,requests,timer,done]() mutable {
        if(done->exchange(true,std::memory_order_acq_rel))return;
        boost::system::error_code ignored;
        timer->cancel();
        try {
            boost::asio::post(sessionPool,[this,live,buffered,requests]() mutable {
                handleSession(std::move(*live),buffered,requests);
            });
        } catch(...) {
            live->close(ignored);
        }
    };
    // A client may pipeline HLS requests; buffered data is already readable.
    if(buffered && buffered->size()>0){resume();return;}
    timer->expires_after(std::chrono::seconds(2));
    timer->async_wait([live,done](const boost::system::error_code& ec) {
        if(ec || done->exchange(true,std::memory_order_acq_rel))return;
        boost::system::error_code ignored;
        live->close(ignored);
    });
    live->async_wait(tcp::socket::wait_read,
        [resume,live,timer,done](const boost::system::error_code& ec) mutable {
            if(ec){
                if(!done->exchange(true,std::memory_order_acq_rel)){
                    boost::system::error_code ignored;
                    timer->cancel();
                    live->close(ignored);
                }
                return;
            }
            resume();
        });
}

void HttpServer::handleSession(
    tcp::socket socket,std::shared_ptr<boost::beast::flat_buffer> buffered,
    unsigned hlsRequests) {
    // Requests are processed by a bounded synchronous worker pool. The
    // TCP receive timeout applies only after epoll/asio signalled readability.
    if(hlsRequests>0){
        struct timeval partialRequestTimeout{1,0};
        ::setsockopt(socket.native_handle(),SOL_SOCKET,SO_RCVTIMEO,
                     &partialRequestTimeout,sizeof(partialRequestTimeout));
    }
    try {
        http::request_parser<http::string_body> parser;
        parser.body_limit(512ULL * 1024ULL * 1024ULL);
        http::read(socket,*buffered,parser);
        http::request<http::string_body> req = parser.release();
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "DVBStreamer5");
        res.set(http::field::content_type, "text/html; charset=UTF-8");
        res.set(http::field::cache_control, "no-store");
        res.set(http::field::pragma, "no-cache");
        // One accepted socket is handled for exactly one HTTP request in this
        // implementation. Advertising keep-alive while closing the socket after
        // the response makes HLS clients reuse a connection that no longer has a
        // request loop and can stall playlist/segment fetching after a few seconds.
        res.keep_alive(false);
        bool hlsKeepAlive=false;

        const std::string target(req.target());
        // Independent API identity: never accept session-management keys for
        // admin/UI/config/restart routes. A forged X-Forwarded-For is ignored:
        // peer IP comes exclusively from the connected TCP socket.
        std::string apiToken;
        bool apiKeyAttempt=false;
        const auto keyHeader=req.find("X-API-Key");
        if(keyHeader!=req.end()){
            apiKeyAttempt=true;
            apiToken=std::string(keyHeader->value());
        }
        const auto authorization=req.find(http::field::authorization);
        if(authorization!=req.end()){
            const std::string raw=std::string(authorization->value());
            if(raw.rfind("Bearer ",0)==0){
                const auto candidate=raw.substr(7);
                bool known=false;
                {
                    std::lock_guard<std::mutex> lock(streamAuthApiKeyMutex_);
                    known=dvbstreamer5::stream_auth::api_key::matches(
                        streamAuthApiKeyDigest_,candidate);
                }
                if(target.rfind("/api/",0)==0 || known){
                    if(apiKeyAttempt)apiToken.clear(); // ambiguous credentials deny
                    else apiToken=candidate;
                    apiKeyAttempt=true;
                }
            }
        }
        bool apiKeyGranted=false;
        if(apiKeyAttempt){
            boost::system::error_code peerError;
            const auto peer=socket.remote_endpoint(peerError);
            const auto method=req.method()==http::verb::get?"GET":
                              req.method()==http::verb::post?"POST":"OTHER";
            const bool inScope=dvbstreamer5::stream_auth::api_key::inScope(
                target,method);
            {
                std::lock_guard<std::mutex> lock(streamAuthApiKeyMutex_);
                apiKeyGranted=inScope && !peerError &&
                    dvbstreamer5::stream_auth::api_key::authorize(
                        streamAuthApiKeyDigest_,streamAuthApiAllowedIps_,apiToken,
                        peerError?"":peer.address().to_string());
            }
            if(!apiKeyGranted){
                res.result(http::status::forbidden);
                res.set(http::field::content_type,"application/json");
                res.body()=R"({"error":"Session API key forbidden"})";
                res.content_length(res.body().size());
                http::write(socket,res);
                return;
            }
        }

        // A logout guard cookie blocks the browser's cached Basic credentials.
        // Logout affects control-panel authentication only; IPTV media and
        // public playlists remain governed by their existing access rules.
        const bool loggedOut =
            panelCookieValue(req, "dvbstreamer5_panel_logged_out") == "1";
        if (target == "/logout" && req.method() == http::verb::get) {
            res.set(http::field::content_type, "text/html; charset=UTF-8");
            res.set(http::field::cache_control, "no-store");
            res.set("Content-Security-Policy",
                    "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'");
            res.body() = R"LOGOUT(<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVBStreamer5 — Logout</title><style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#10141c;color:#e7edf8;font:14px system-ui,Arial,sans-serif}
main{width:min(370px,calc(100% - 40px));padding:28px;background:#171f2b;border:1px solid #344255;border-radius:16px}
h1{font-size:20px;margin:0 0 8px}p{color:#acbbca}label{display:block;margin:14px 0 5px}
input{box-sizing:border-box;width:100%;padding:11px;background:#0e1723;color:#fff;border:1px solid #435168;border-radius:8px}
button{margin-top:18px;padding:11px;width:100%;border:0;border-radius:8px;background:#267bd7;color:#fff;cursor:pointer}
#error{color:#ffafa9;min-height:1em}
</style></head><body><main><h1>DVBStreamer5</h1><p id="hint">Вы вышли из панели. Войдите снова.</p>
<form id="loginForm"><label id="userLabel" for="login">Логин</label><input id="login" autocomplete="username" required>
<label id="passLabel" for="password">Пароль</label><input id="password" type="password" autocomplete="current-password" required>
<button id="submit" type="submit">Войти</button><p id="error" role="alert"></p></form></main>
<script>
const lang=localStorage.getItem('dvbstreamer5-language')||'ru';
const words={ru:['Вы вышли из панели. Войдите снова.','Логин','Пароль','Войти','Неверный логин или пароль.'],
en:['You have signed out. Sign in again.','Username','Password','Sign in','Invalid username or password.'],
hr:['Odjavili ste se. Prijavite se ponovno.','Korisničko ime','Lozinka','Prijava','Neispravno korisničko ime ili lozinka.']}[lang]||
['You have signed out. Sign in again.','Username','Password','Sign in','Invalid username or password.'];
['hint','userLabel','passLabel','submit'].forEach((id,i)=>document.getElementById(id).textContent=words[i]);
document.getElementById('loginForm').addEventListener('submit',async event=>{
 event.preventDefault();
 const button=document.getElementById('submit');button.disabled=true;
 const error=document.getElementById('error');error.textContent='';
 try{
  const response=await fetch('/api/panel/login',{method:'POST',credentials:'same-origin',
   headers:{'Content-Type':'application/json'},
   body:JSON.stringify({login:document.getElementById('login').value,password:document.getElementById('password').value})});
  if(!response.ok)throw Error(words[4]);
  window.location.replace('/');
 }catch(e){error.textContent=e.message||words[4];button.disabled=false;}
});
</script></body></html>)LOGOUT";
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }
        if (target == "/api/panel/login") {
            res.set(http::field::content_type, "application/json");
            bool authenticated = false;
            if (req.method() == http::verb::post && req.body().size() <= 4096U) {
                Json::Value body;
                Json::CharReaderBuilder reader;
                std::istringstream input(req.body());
                std::string parseErrors;
                if (Json::parseFromStream(reader, input, &body, &parseErrors) &&
                    body.isObject() && body["login"].isString() &&
                    body["password"].isString()) {
                    authenticated =
                        constantTimeEquals(body["login"].asString(), configManager.config.login) &&
                        constantTimeEquals(body["password"].asString(), configManager.config.password);
                }
            }
            if (authenticated) {
                const std::string token = createPanelSession();
                if (token.empty()) {
                    res.result(http::status::service_unavailable);
                    res.body() = R"({"error":"Unable to create login session"})";
                } else {
                    res.insert(http::field::set_cookie,
                        "dvbstreamer5_panel_logged_out=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
                    res.insert(http::field::set_cookie,
                        "dvbstreamer5_panel_session=" + token +
                        "; Path=/; Max-Age=43200; HttpOnly; SameSite=Lax");
                    res.body() = R"({"result":"ok"})";
                }
            } else {
                res.result(http::status::unauthorized);
                res.body() = R"({"error":"Invalid credentials"})";
            }
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }
        const bool panelProtected = requiresAuthentication(target);
        if (loggedOut && panelProtected && !apiKeyGranted) {
            if (req.method() == http::verb::get && target == "/") {
                res.result(http::status::see_other);
                res.set(http::field::location, "/logout");
            } else {
                // 403 (not 401): never trigger cached Basic Auth popup on logout.
                res.result(http::status::forbidden);
                res.set(http::field::content_type, "application/json");
                res.body() = R"({"error":"Panel session logged out"})";
            }
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }
        if (panelProtected && !apiKeyGranted && !isAuthorized(req) &&
            !isPanelSessionAuthorized(req)) {
            writeUnauthorized(res);
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }
        // V383: independent metadata-only auth settings. Existing streaming
        // workers and output sessions are NOT restarted on save.
        if (target == "/api/stream-auth" && req.method()==http::verb::get) {
            Json::Value data(Json::objectValue);
            data["enabled"]=configManager.config.streamAuthBackendEnabled;
            data["backend_url"]=configManager.config.streamAuthBackendUrl;
            data["timeout_ms"]=configManager.config.streamAuthBackendTimeoutMs;
            data["recheck_minutes"]=configManager.config.streamAuthRecheckMinutes;
            data["end_url"]=configManager.config.streamAuthEndUrl;
            data["ip_filter_effective"]=!configManager.config.streamAuthBackendEnabled;
            data["failure_policy"]="deny";
            data["protocols"]="http,hls,srt";
            {
                std::lock_guard<std::mutex> guard(streamAuthApiKeyMutex_);
                data["api_key_configured"]=!streamAuthApiKeyDigest_.empty();
                data["api_allowed_ips"]=streamAuthApiAllowedIps_;
            }
            Json::StreamWriterBuilder writer;
            res.set(http::field::content_type,"application/json; charset=UTF-8");
            res.body()=Json::writeString(writer,data);
            res.content_length(res.body().size());
            http::write(socket,res);
            return;
        }
        if (target == "/api/stream-auth/save" && req.method()==http::verb::post) {
            res.set(http::field::content_type,"application/json; charset=UTF-8");
            Json::Value reply(Json::objectValue),data;
            Json::CharReaderBuilder reader;
            std::string errors;
            std::istringstream payload(req.body());
            if (req.body().size()>8192 ||
                !Json::parseFromStream(reader,payload,&data,&errors) ||
                !data.isObject() || !data["enabled"].isBool() ||
                !data["backend_url"].isString()) {
                res.result(http::status::bad_request);
                reply["error"]="Invalid stream authentication settings";
            } else {
                const bool enabled=data["enabled"].asBool();
                const std::string url=data["backend_url"].asString();
                const int timeout=std::clamp(data.get("timeout_ms",1200).asInt(),100,3000);
                const int recheck=std::clamp(data.get("recheck_minutes",5).asInt(),1,60);
                const std::string endUrl=data.get("end_url","").asString();
                const std::string allowedIps=data.get("api_allowed_ips",
                    configManager.config.streamAuthApiAllowedIps).asString();
                const bool updateApiIps=data.isMember("api_allowed_ips");
                const auto requestedWith=req.find("X-Requested-With");
                const bool trustedSettingsUpdate=!updateApiIps ||
                    (requestedWith!=req.end() && 
                     requestedWith->value()=="DVBStreamer5");
                const auto validUrl=[](const std::string& value){
                    return value.size()<=2048 &&
                       value.find_first_of("\r\n#")==std::string::npos &&
                       (value.empty()||value.rfind("https://",0)==0||
                        value.rfind("http://",0)==0);
                };
                if (!trustedSettingsUpdate){
                    res.result(http::status::forbidden);
                    reply["error"]="Same-origin settings request required";
                } else if (!validUrl(url) || !validUrl(endUrl) || (enabled && url.empty()) ||
                           (updateApiIps &&
                            !dvbstreamer5::stream_auth::api_key::validAllowedIps(allowedIps))) {
                    res.result(http::status::bad_request);
                    reply["error"]="Invalid middleware URL or session API IP allowlist";
                } else {
                    const bool oldEnabled=configManager.config.streamAuthBackendEnabled;
                    const std::string oldUrl=configManager.config.streamAuthBackendUrl;
                    const int oldTimeout=configManager.config.streamAuthBackendTimeoutMs;
                    const int oldRecheck=configManager.config.streamAuthRecheckMinutes;
                    const auto oldEnd=configManager.config.streamAuthEndUrl;
                    const auto oldAllowed=configManager.config.streamAuthApiAllowedIps;
                    configManager.config.streamAuthBackendEnabled=enabled;
                    configManager.config.streamAuthBackendUrl=url;
                    configManager.config.streamAuthBackendTimeoutMs=timeout;
                    configManager.config.streamAuthRecheckMinutes=recheck;
                    configManager.config.streamAuthEndUrl=endUrl;
                    if(updateApiIps)
                        configManager.config.streamAuthApiAllowedIps=allowedIps;
                    if (!configManager.save()) {
                        configManager.config.streamAuthBackendEnabled=oldEnabled;
                        configManager.config.streamAuthBackendUrl=oldUrl;
                        configManager.config.streamAuthBackendTimeoutMs=oldTimeout;
                        configManager.config.streamAuthRecheckMinutes=oldRecheck;
                        configManager.config.streamAuthEndUrl=oldEnd;
                        configManager.config.streamAuthApiAllowedIps=oldAllowed;
                        res.result(http::status::internal_server_error);
                        reply["error"]="Failed to save stream authentication";
                    } else {
                        if(updateApiIps){
                            std::lock_guard<std::mutex> lock(streamAuthApiKeyMutex_);
                            streamAuthApiAllowedIps_=allowedIps;
                        }
                        reply["result"]="ok";
                        reply["enabled"]=enabled;
                        reply["ip_filter_effective"]=!enabled;
                        reply["failure_policy"]="deny";
                    }
                }
            }
            Json::StreamWriterBuilder writer;
            res.body()=Json::writeString(writer,reply);
            res.content_length(res.body().size());
            http::write(socket,res);
            return;
        }
        // Only an authenticated control-panel administrator can rotate the
        // 256-bit credential; a session-scoped API key cannot call this route.
        if(target=="/api/stream-auth/api-key/rotate" &&
           req.method()==http::verb::post){
            const auto requestedWith=req.find("X-Requested-With");
            Json::Value response(Json::objectValue);
            res.set(http::field::content_type,"application/json; charset=UTF-8");
            res.set(http::field::cache_control,"no-store");
            if(requestedWith==req.end()||
               requestedWith->value()!="DVBStreamer5"||
               req.body().size()>64U){
                res.result(http::status::forbidden);
                response["error"]="Same-origin admin request required";
            }else{
                const auto raw=dvbstreamer5::stream_auth::api_key::generate();
                const auto digest=dvbstreamer5::stream_auth::api_key::sha256(raw);
                if(raw.empty()||digest.empty()){
                    res.result(http::status::service_unavailable);
                    response["error"]="Secure random generator unavailable";
                }else{
                    std::lock_guard<std::mutex> lock(streamAuthApiKeyMutex_);
                    const auto old=configManager.config.streamAuthApiKeySha256;
                    configManager.config.streamAuthApiKeySha256=digest;
                    if(!configManager.save()){
                        configManager.config.streamAuthApiKeySha256=old;
                        res.result(http::status::internal_server_error);
                        response["error"]="Could not persist API key";
                    }else{
                        streamAuthApiKeyDigest_=digest;
                        response["result"]="ok";
                        response["api_key"]=raw; // shown exactly once
                    }
                }
            }
            Json::StreamWriterBuilder writer;
            res.body()=Json::writeString(writer,response);
            res.content_length(res.body().size());
            http::write(socket,res);
            return;
        }
        // Admin-only viewer management, or least-privilege API key.
        // Never return viewer token/credentials.
        if(target=="/api/stream-auth/sessions" && req.method()==http::verb::get){
            Json::Value data(Json::objectValue);
            data["result"]="ok";
            const bool includeBackendId=dvbstreamer5::stream_auth::api_key::backendIdVisible(
                configManager.config.streamAuthBackendEnabled,
                configManager.config.streamAuthApiKeySha256);
            data["sessions"]=dvbstreamer5::stream_auth::SessionRegistry::instance().listing(includeBackendId);
            Json::StreamWriterBuilder writer;
            res.set(http::field::content_type,"application/json; charset=UTF-8");
            res.body()=Json::writeString(writer,data);
            res.content_length(res.body().size());http::write(socket,res);return;
        }
        if(target=="/api/stream-auth/sessions/disconnect" &&
           req.method()==http::verb::post){
            Json::Value payload,reply(Json::objectValue);
            Json::CharReaderBuilder reader;std::string errors;
            std::istringstream body(req.body());
            if(req.body().size()>4096 ||
               !Json::parseFromStream(reader,body,&payload,&errors) ||
               !payload.isObject() || !payload["session_id"].isString() ||
               payload["session_id"].asString().size()>128){
                res.result(http::status::bad_request);
                reply["error"]="Invalid session ID";
            }else{
                const std::string sid=payload["session_id"].asString();
                const bool revoked=
                    dvbstreamer5::stream_auth::SessionRegistry::instance().revoke(sid);
                reply["result"]=revoked?"ok":"not_found";
                reply["http_disconnected"]=Json::UInt64(
                    revoked?streamManager.disconnectAuthSession(sid):0);
                if(!revoked)res.result(http::status::not_found);
            }
            Json::StreamWriterBuilder writer;
            res.set(http::field::content_type,"application/json; charset=UTF-8");
            res.body()=Json::writeString(writer,reply);
            res.content_length(res.body().size());http::write(socket,res);return;
        }
        if (target == "/api/panel/logout") {
            res.set(http::field::content_type, "application/json");
            const auto requestedWith = req.find("X-Requested-With");
            if (req.method() == http::verb::post &&
                requestedWith != req.end() &&
                requestedWith->value() == "DVBStreamer5") {
                revokePanelSession(req);
                res.insert(http::field::set_cookie,
                    "dvbstreamer5_panel_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
                res.insert(http::field::set_cookie,
                    "dvbstreamer5_panel_logged_out=1; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax");
                res.body() = R"({"result":"ok"})";
            } else {
                res.result(http::status::method_not_allowed);
                res.body() = R"({"error":"POST required"})";
            }
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }

        if (req.method() == http::verb::head) {
            // IPTV clients probe EPG/playlists with HEAD; do not synthesize
            // the potentially large XMLTV document for a header-only request.
            if (target == "/epg.xml" || target == "/epg.xml.gz" ||
                target == "/playlist.m3u") {
                res.set(http::field::content_type,
                    target == "/playlist.m3u" ? "audio/x-mpegurl; charset=UTF-8" :
                    (target == "/epg.xml.gz" ? "application/gzip" :
                     "application/xml; charset=UTF-8"));
#ifndef DVBSTREAMER5_HAVE_EPG_ZLIB
                if (target == "/epg.xml.gz")
                    res.result(http::status::not_implemented);
#endif
                res.set("Access-Control-Allow-Origin", "*");
                http::write(socket, res);
                return;
            }
            // Strict URL receivers (including NetUP Stream Processor) may probe
            // HTTP Progressive/HLS URLs with HEAD before opening the media GET.
            // Answer those probes without creating a playback session.
            std::string previewId;
            if (resolvePrivatePreviewTarget(target, previewId)) {
                if (!findStreamConfigById(configManager.config.streams, previewId) ||
                    !streamManager.isStreamActive(previewId)) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Preview stream not running";
                } else {
                    res.result(http::status::ok);
                    res.set(http::field::content_type, "video/MP2T");
                    res.set(http::field::cache_control, "no-store");
                }
                res.keep_alive(false);
                http::write(socket, res);
                return;
            }
            std::string httpMpegTsId;
            if (resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId)) {
                if (!isStreamClientAllowed(socket, target, req)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                } else {
                    res.result(http::status::ok);
                    res.set(http::field::content_type, "video/MP2T");
                    res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
                }
                res.keep_alive(false);
                http::write(socket, res);
                return;
            }

            std::string hlsId;
            std::string hlsFile;
            if (resolveHlsTarget(configManager.config.streams, target, hlsId, hlsFile)) {
                if (!isStreamClientAllowed(socket, target, req)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.keep_alive(false);
                    http::write(socket, res);
                    return;
                }
                const StreamConfig* hlsCfg = findStreamConfigById(configManager.config.streams, hlsId);
                const std::filesystem::path filePath = hlsCfg
                    ? hlsStorageDirectory(*hlsCfg) / hlsFile
                    : std::filesystem::path();
                int64_t archiveFrom = 0, archiveTo = 0; bool archiveEvent = false;
                const bool dynamicArchive = hlsCfg && hlsCfg->hlsArchiveEnabled &&
                    parseHlsArchivePlaylistRequest(hlsFile, archiveFrom, archiveTo, archiveEvent);
                if (!dynamicArchive && (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath))) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/plain");
                    res.keep_alive(false);
                    http::write(socket, res);
                    return;
                }
                res.result(http::status::ok);
                if (filePath.extension() == ".m3u8") res.set(http::field::content_type, "application/vnd.apple.mpegurl");
                else if (filePath.extension() == ".m4s") res.set(http::field::content_type, "video/iso.segment");
                else if (filePath.extension() == ".mp4") res.set(http::field::content_type, "video/mp4");
                else if (filePath.extension() == ".bin") res.set(http::field::content_type, "application/octet-stream");
                else res.set(http::field::content_type, "video/MP2T");
                res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
                res.set("Access-Control-Allow-Origin", "*");
                res.keep_alive(false);
                http::write(socket, res);
                return;
            }
        }

        if (req.method() == http::verb::get) {
          std::string previewId;
          if (resolvePrivatePreviewTarget(target, previewId)) {
            if (!findStreamConfigById(configManager.config.streams, previewId) ||
                !streamManager.isStreamActive(previewId)) {
              res.result(http::status::not_found);
              res.set(http::field::content_type, "text/plain; charset=UTF-8");
              res.body() = "Preview stream not running";
            } else {
              std::string previewToken;
              if (!privatePreviewTokenFromTarget(target, previewToken)) {
                res.result(http::status::bad_request);
                res.set(http::field::content_type, "text/plain; charset=UTF-8");
                res.body() = "Missing or invalid preview session";
              } else if (handleHttpStream(socket, target)) {
                return;
              } else {
                res.result(http::status::service_unavailable);
                res.body() = "Preview relay unavailable";
              }
            }
          } else {
          std::string httpMpegTsId;
          if (resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId)) {
            if (!isStreamClientAllowed(socket, target, req)) {
              res.result(http::status::forbidden);
              res.set(http::field::content_type, "text/plain");
              res.body() = "Stream access denied";
            } else if (handleHttpStream(socket, target)) {
              return;
            }
            } else if ([&] { std::string sid, file; return resolveHlsTarget(configManager.config.streams, target, sid, file); }()) {
                if (!isStreamClientAllowed(socket, target, req)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Stream access denied";
                } else {
                    std::filesystem::path directFile;
                    if(serveHlsFile(socket,target,res,&directFile)){
                        hlsKeepAlive=res.result()==http::status::ok &&
                            req.keep_alive() && hlsRequests<7;
                        if(!directFile.empty()){
                            if(!sendHlsFileZeroCopy(socket,directFile,res,req.version(),
                                                    hlsKeepAlive))return;
                            if(hlsKeepAlive)
                                scheduleHlsKeepAlive(std::move(socket),buffered,hlsRequests+1);
                            return;
                        }
                    }
                }
            } else if (target == "/epg.xml" || target == "/epg.xml.gz" ||
                       target == "/playlist.m3u") {
                // IPTV/OTT clients usually cannot send control-panel Basic Auth.
                // Only channels allowed for this client's IP are advertised.
                boost::system::error_code peerEc;
                const auto peer = socket.remote_endpoint(peerEc);
                if (peerEc) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Client address unavailable";
                } else {
                    const std::string clientIp = peer.address().to_string();
                    const EpgAllowed allowed = [this, &clientIp](
                        const std::string& id) {
                        return isClientAllowedForStream(id, clientIp);
                    };
                    res.set("Access-Control-Allow-Origin", "*");
                    res.set(http::field::cache_control, "private, max-age=15");
                    if (target == "/playlist.m3u") {
                        const auto hostIt = req.find(http::field::host);
                        const std::string authority = hostIt == req.end()
                            ? std::string{} : std::string(hostIt->value());
                        res.set(http::field::content_type,
                                "audio/x-mpegurl; charset=UTF-8");
                        // A trusted HTTPS reverse proxy can pass this header
                        // so XMLTV/HLS links retain the browser-visible scheme.
                        const auto protoIt = req.find("X-Forwarded-Proto");
                        const bool advertisedHttps = protoIt != req.end() &&
                            toLower(std::string(protoIt->value())) == "https";
                        res.body() = buildEpgIptvPlaylist(
                            configManager.config.streams, allowed,
                            epgHttpBase(authority, configManager.config.httpPort,
                                        advertisedHttps),
                            configManager.config.httpPort);
                    } else {
                        const std::string xml = buildEpgXmltv(
                            configManager.config.streams, streamManager, allowed);
                        if (target == "/epg.xml") {
                            res.set(http::field::content_type,
                                    "application/xml; charset=UTF-8");
                            res.body() = xml;
                        } else {
                            res.set(http::field::content_type, "application/gzip");
                            if (!gzipEpgXmltv(xml, res.body())) {
                                res.result(http::status::not_implemented);
                                res.set(http::field::content_type, "text/plain");
                                res.body() = "XMLTV gzip unavailable; use /epg.xml";
                            } else {
                                res.set(http::field::content_disposition,
                                        "attachment; filename=\"epg.xml.gz\"");
                            }
                        }
                    }
                }
            } else if (target == "/" || target == "/index.html") {
                res.body() = renderIndexPage();
            } else if (target == "/oscam-mini") {
                res.set(http::field::content_type, "text/html; charset=UTF-8");
                res.body() = OscamMiniManager::instance().renderPage();
            } else if (target == "/api/oscam-mini/status") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().statusJson();
            } else if (target == "/api/oscam-mini/settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().settingsJson();
            } else if (target == "/preview/mpegts.min.js" ||
                       target == "/preview/hls.min.js" ||
                       target == "/licenses/mpegts.js.txt" ||
                       target == "/licenses/hls.js.txt") {
                const std::string_view asset = dvbstreamer5::web::embeddedWebAsset(target);
                if (asset.empty()) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/plain; charset=UTF-8");
                    res.body() = "Embedded web asset is unavailable.";
                } else {
                    const bool isJavaScript = target.rfind("/preview/", 0) == 0;
                    res.set(http::field::content_type, isJavaScript
                        ? "application/javascript; charset=UTF-8"
                        : "text/plain; charset=UTF-8");
                    res.set(http::field::cache_control, "private, max-age=3600");
                    res.body().assign(asset.data(), asset.size());
                }
            } else if (target.rfind("/api/streams/", 0) == 0 &&
                       target.size() > std::string("/api/streams//preview").size() &&
                       target.compare(target.size() - std::string("/preview").size(),
                                      std::string("/preview").size(), "/preview") == 0) {
                res.set(http::field::content_type, "application/json; charset=UTF-8");
                res.set(http::field::cache_control, "no-store");
                const std::string encodedId = target.substr(std::string("/api/streams/").size(),
                    target.size() - std::string("/api/streams/").size() - std::string("/preview").size());
                const std::string streamId = urlDecode(encodedId);
                const StreamConfig* cfg = findStreamConfigById(configManager.config.streams, streamId);
                if (!cfg || streamId.empty() || streamId.find('/') != std::string::npos) {
                    res.result(http::status::not_found);
                    res.body() = R"({"error":"stream_not_found"})";
                } else {
                    Json::Value manifest(Json::objectValue);
                    manifest["stream_id"] = cfg->id;
                    manifest["name"] = cfg->name;
                    const bool active = streamManager.isStreamActive(cfg->id);
                    manifest["active"] = active;
                    manifest["input_mode"] = cfg->inputMode;
                    manifest["input_is_hls"] = toLower(cfg->inputMode) == "hls" ||
                        toLower(cfg->inputUri).find(".m3u8") != std::string::npos;
                    Json::Value sources(Json::arrayValue);
                    // A browser preview uses the existing HTTP relay when present,
                    // otherwise an isolated private MPEG-TS relay sharing this
                    // channel's already-running media chain. The configured outputs
                    // are not altered and the private URL requires admin auth.
                    const std::string cleanId = cleanPathToken(cfg->id);
                    const bool safeId = !cleanId.empty() && cleanId == cfg->id;
                    if (safeId && active) {
                        const bool configuredHttp = hasOutputType(*cfg, "http");
                        Json::Value source(Json::objectValue);
                        source["id"] = configuredHttp ? "configured-http" : "temporary-http";
                        source["label"] = configuredHttp ? "HTTP MPEG-TS" : "Временный HTTP MPEG-TS";
                        source["kind"] = "http";
                        source["preview_kind"] = "mpegts";
                        source["preview_url"] = configuredHttp
                            ? "/stream/" + cleanId + ".ts"
                            : "/api/streams/" + cleanId + "/preview.ts";
                        sources.append(std::move(source));
                        // If this channel also publishes HLS, offer the already
                        // running playlist as an alternate browser transport.
                        // No upstream URL, access token or satellite URI leaks.
                        if (hasOutputType(*cfg, "hls")) {
                            Json::Value hls(Json::objectValue);
                            hls["id"] = "configured-hls";
                            hls["label"] = "HLS";
                            hls["kind"] = "hls";
                            hls["preview_kind"] = "hls";
                            hls["preview_url"] = "/" + hlsPublicName(*cfg) + "/video.m3u8";
                            sources.append(std::move(hls));
                        }
                    }
                    manifest["sources"] = std::move(sources);
                    Json::StreamWriterBuilder writer;
                    res.body() = Json::writeString(writer, manifest);
                }
            } else if (target == "/api/tickers") {
                res.set(http::field::content_type, "application/json");
                Json::Value response(Json::objectValue);
                response["entries"] = configManager.tickerOverlaysSnapshot();
                response["storage"] = "ticker-overlays.json";
                response["fonts"] = dvbstreamer5::media::ticker::TickerOverlay::fontList();
                response["available"] = dvbstreamer5::media::ticker::TickerOverlay::available();
                Json::Value streams(Json::arrayValue);
                for (const auto& cfg : configManager.config.streams) {
                    Json::Value item(Json::objectValue);
                    item["id"] = cfg.id;
                    item["name"] = cfg.name;
                    item["transcode_enabled"] = cfg.transcodeEnabled;
                    item["video_codec"] = cfg.transcodeVideoCodec;
                    item["video_decoder"] = cfg.transcodeVideoDecoder;
                    streams.append(item);
                }
                response["streams"] = streams;
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer, response);
            } else if (target == "/api/interfaces") {
                res.set(http::field::content_type, "application/json");
                res.body() = listInterfaces();
            } else if (target == "/api/system-metrics") {
              res.set(http::field::content_type, "application/json");
              httpMetricsRequestCount.fetch_add(1, std::memory_order_relaxed);
              res.body() = systemMetrics();
            } else if (target.rfind("/api/system-load-history", 0) == 0 &&
                       (target.size() == 24U || target[24] == '?')) {
              // Protected by the existing /api/* control-panel authentication.
              // Never trust an unbounded client-supplied range.
              res.set(http::field::content_type, "application/json");
              uint64_t period = 3600;
              const std::string requested = queryValue(target, "period");
              if (requested == "360" || requested == "3600" || requested == "86400") {
                  period = static_cast<uint64_t>(std::stoull(requested));
              }
              res.body() = systemLoadHistory(period);
            } else if (target == "/api/mpts/status") {
                res.set(http::field::content_type, "application/json");
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer, streamManager.mptsSnapshot());
            } else if (target == "/api/backup-files") {
                res.set(http::field::content_type, "application/json");
                res.body() = listBackupFiles();
            } else if (target == "/api/xmltv/status") {
                res.set(http::field::content_type, "application/json");
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer,
                    streamManager.externalXmltvStatus());
            } else if (target == "/api/xmltv/channels") {
                res.set(http::field::content_type, "application/json");
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer,
                    streamManager.externalXmltvChannels());
            } else if (target.rfind("/api/media-tracks?stream_id=", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.set(http::field::cache_control, "no-store");
                std::string id=target.substr(
                    std::string("/api/media-tracks?stream_id=").size());
                const auto amp=id.find('&');
                if(amp!=std::string::npos)id.resize(amp);
                id=urlDecode(id);
                if(id.empty()||id.size()>256 ||
                   !findStreamConfigById(configManager.config.streams,id)) {
                    res.result(http::status::bad_request);
                    res.body()=R"({"error":"Unknown or invalid stream ID"})";
                } else {
                    Json::StreamWriterBuilder writer;
                    res.body()=Json::writeString(writer,streamManager.mediaTracks(id));
                }
            } else if (target.rfind("/api/dvb-subtitles?stream_id=", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                std::string streamId = target.substr(
                    std::string("/api/dvb-subtitles?stream_id=").size());
                const auto amp = streamId.find('&');
                if (amp != std::string::npos) streamId.resize(amp);
                if (streamId.empty() || streamId.size() > 256U) {
                    res.result(http::status::bad_request);
                    res.body() = R"({"error":"Invalid stream id"})";
                } else {
                    Json::StreamWriterBuilder writer;
                    res.body() = Json::writeString(
                        writer, streamManager.dvbSubtitleTracks(streamId));
                }
            } else if (target.rfind("/api/epg?stream_id=", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                std::string streamId = target.substr(
                    std::string("/api/epg?stream_id=").size());
                const auto amp = streamId.find('&');
                if (amp != std::string::npos) streamId.resize(amp);
                if (streamId.empty() || streamId.size() > 256) {
                    res.result(http::status::bad_request);
                    res.body() = R"({"error":"Invalid stream id"})";
                } else {
                    Json::StreamWriterBuilder writer;
                    res.body() = Json::writeString(
                        writer, streamManager.epgSnapshot(streamId));
                }
            } else if (target == "/api/update/status") {
                res.set(http::field::content_type, "application/json");
                // Only fixed phase words can enter the JSON response.
                std::ifstream statusFile("/var/lib/dvbstreamer5/update-status");
                std::string phase;
                if (!std::getline(statusFile, phase) ||
                    (phase != "downloading" && phase != "verifying" &&
                     phase != "installing" && phase != "restarting" &&
                     phase != "done" && phase != "error")) phase = "idle";
                res.set(http::field::cache_control, "no-store");
                res.body() = std::string(R"({"phase":")") + phase + R"("})";
            } else if (target == "/api/update/check") {
                // Fixed download host; never accept a URL from the browser.
                res.set(http::field::content_type, "application/json");
                res.set(http::field::cache_control, "no-store");
                const char* updaterPath = "/opt/DVBStreamer5/updater.sh";
                std::ifstream updaterFile(updaterPath);
                if (!updaterFile.good()) {
                    res.result(http::status::service_unavailable);
                    res.body() = R"({"result":"error","error":"Updater missing: install /opt/DVBStreamer5/updater.sh from the portable bundle"})";
                } else {
                    const std::string command = std::string("/bin/bash ") +
                        updaterPath + " check " + std::string(kProgramVersion) +
                        " 2>&1";
                    std::string output;
                    int exitCode = -1;
                    if (FILE* pipe = popen(command.c_str(), "r")) {
                        char buffer[1024];
                        while (output.size() < 4096 && fgets(buffer, sizeof(buffer), pipe))
                            output += buffer;
                        exitCode = pclose(pipe);
                    }
                    Json::Value parsed;
                    Json::CharReaderBuilder reader;
                    std::string parseErrors;
                    std::istringstream jsonInput(output);
                    if (!output.empty() && output.size() < 4096 &&
                        Json::parseFromStream(reader, jsonInput, &parsed, &parseErrors) &&
                        parsed.isObject() && parsed.isMember("result")) {
                        if (parsed["result"].asString() != "ok" || exitCode != 0)
                            res.result(http::status::service_unavailable);
                        res.body() = output;
                    } else {
                        // Return the actual updater error, JSON-escaped by JsonCpp.
                        // Previously 2>/dev/null hid all Bash, curl and permission
                        // diagnostics, leaving only an unhelpful generic message.
                        res.result(http::status::service_unavailable);
                        Json::Value failure;
                        failure["result"] = "error";
                        std::string detail = output;
                        const auto lastLine = detail.find_last_of("\\r\\n");
                        if (lastLine != std::string::npos && lastLine + 1 < detail.size())
                            detail = detail.substr(lastLine + 1);
                        if (detail.size() > 300)
                            detail = detail.substr(detail.size() - 300);
                        if (detail.empty())
                            detail = "Bash updater exited without output (check service user and server logs)";
                        failure["error"] = "Updater check failed: " + detail;
                        failure["exit_status"] = exitCode;
                        Json::StreamWriterBuilder writer;
                        res.body() = Json::writeString(writer, failure);
                    }
                }
            } else if (target == "/api/state") {
                res.set(http::field::content_type, "application/json");
                // 202.68: server-side A/B probe for the /api/state allocation path.
                // With DVBSTREAMER5_STATE_API_DIAG_BYPASS=1 the remote browser may remain
                // open and keep polling, but currentState() and its large JSON tree
                // are not built. Streaming/media paths are untouched.
                const char* stateBypassEnv = std::getenv("DVBSTREAMER5_STATE_API_DIAG_BYPASS");
                const bool stateBypass =
                    stateBypassEnv && *stateBypassEnv && std::strcmp(stateBypassEnv, "0") != 0;
                std::string stateBody;
                if (stateBypass) {
                    stateBody = std::string(R"({"diagnostic_state_bypass":true,"program_version":")") + kProgramVersion + R"("})";
                } else {
                    stateBody = currentState();
                }
                httpStateRequestCount.fetch_add(1, std::memory_order_relaxed);
                httpStateResponseBytes.fetch_add(
                    static_cast<uint64_t>(stateBody.size()), std::memory_order_relaxed);
                httpStateLastResponseBytes.store(
                    static_cast<uint64_t>(stateBody.size()), std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> diagLock(httpDiagMutex);
                    httpStateWorkerThreads.insert(std::this_thread::get_id());
                }
                res.body() = std::move(stateBody);
            } else if (target == "/api/dvb-adapters") {
                res.set(http::field::content_type, "application/json");
                res.body() = dvbAdapters();
            } else if (target == "/api/ca-manager") {
                res.set(http::field::content_type, "application/json");
                res.body() = caManagerStatus();
            } else if (target.rfind("/api/quality-history", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = qualityHistory(target);
            } else if (target == "/health") {
                res.set(http::field::content_type, "text/plain");
                res.body() = "Healthy";
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
          } // end normal GET routing; private preview is handled above
        } else if (req.method() == http::verb::post) {
            std::string closingPreviewId;
            if (resolvePrivatePreviewCloseTarget(target, closingPreviewId)) {
                res.set(http::field::content_type, "application/json");
                Json::Value payload;
                Json::CharReaderBuilder builder;
                std::string error;
                std::istringstream input(req.body());
                if (req.body().size() > 256 ||
                    !Json::parseFromStream(builder, input, &payload, &error) ||
                    !payload.isObject() || !payload["session"].isString() ||
                    !validPreviewSessionToken(payload["session"].asString()) ||
                    !findStreamConfigById(configManager.config.streams, closingPreviewId)) {
                    res.result(http::status::bad_request);
                    res.body() = R"({"error":"Invalid preview close request"})";
                } else {
                    const bool closed = streamManager.closePreviewSession(
                        closingPreviewId, payload["session"].asString());
                    res.body() = closed ? R"({"closed":true})" : R"({"closed":false})";
                }
            } else if (target == "/api/network/udp-input-filter") {
                res.set(http::field::content_type, "application/json");
                Json::Value payload;
                Json::CharReaderBuilder builder;
                std::string parseError;
                std::istringstream input(req.body());
                Json::Value reply(Json::objectValue);
                if (req.body().size() > 4096 ||
                    !Json::parseFromStream(builder, input, &payload, &parseError) ||
                    !payload.isObject() || !payload["address"].isString() ||
                    !payload["enabled"].isBool()) {
                    res.result(http::status::bad_request);
                    reply["result"] = "error";
                    reply["error"] = "Invalid UDP input filter request";
                } else {
                    const std::string address = payload["address"].asString();
                    const bool enabled = payload["enabled"].asBool();
                    const auto interfaces = enumerateNetworkInterfaces();
                    const auto iface = std::find_if(interfaces.begin(), interfaces.end(),
                        [&address](const NetworkInterface& candidate) {
                            return candidate.address == address && candidate.name != "lo" &&
                                   candidate.address.rfind("127.", 0) != 0;
                        });
                    if (iface == interfaces.end()) {
                        res.result(http::status::bad_request);
                        reply["result"] = "error";
                        reply["error"] = "Network interface is unavailable";
                    } else {
                        const auto previous = configManager.config.udpInputFilterInterfaces;
                        auto next = previous;
                        next.erase(std::remove(next.begin(), next.end(), address), next.end());
                        if (enabled) next.push_back(address);
                        std::sort(next.begin(), next.end());
                        next.erase(std::unique(next.begin(), next.end()), next.end());
                        configManager.config.udpInputFilterInterfaces = next;

                        std::string firewallError;
                        if (!dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, firewallError)) {
                            configManager.config.udpInputFilterInterfaces = previous;
                            std::string rollbackError;
                            (void)dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, rollbackError);
                            res.result(http::status::bad_request);
                            reply["result"] = "error";
                            reply["error"] = firewallError;
                        } else if (!configManager.save()) {
                            configManager.config.udpInputFilterInterfaces = previous;
                            std::string rollbackError;
                            (void)dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, rollbackError);
                            res.result(http::status::internal_server_error);
                            reply["result"] = "error";
                            reply["error"] = "Unable to save configuration";
                        } else {
                            reply["result"] = "ok";
                            reply["address"] = address;
                            reply["enabled"] = enabled;
                        }
                    }
                }
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                res.body() = Json::writeString(writer, reply);
            } else if (target == "/api/oscam-mini/save") {
                res.set(http::field::content_type, "application/json");
                const std::string oscamSaveResult = OscamMiniManager::instance().saveSettingsJson(req.body());
                res.body() = oscamSaveResult;

                Json::Value oscamSaveJson;
                Json::CharReaderBuilder oscamSaveReader;
                std::string oscamSaveErrors;
                std::istringstream oscamSaveInput(oscamSaveResult);
                if (Json::parseFromStream(oscamSaveReader, oscamSaveInput, &oscamSaveJson, &oscamSaveErrors) &&
                    oscamSaveJson.get("ok", false).asBool()) {
                    CardManager::instance().configure(configManager.config.camClients);
                }
            } else if (target == "/api/oscam-mini/action") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().serviceActionJson(req.body());
            } else if (target == "/api/xmltv/save" ||
                       target == "/api/xmltv/refresh") {
                res.set(http::field::content_type, "application/json");
                Json::Value result(Json::objectValue);
                Json::CharReaderBuilder parser;
                Json::Value payload;
                std::string parseError;
                std::istringstream input(req.body());
                if (req.body().size() > 128U * 1024U ||
                    !Json::parseFromStream(parser, input, &payload, &parseError)) {
                    res.result(http::status::bad_request);
                    result["result"] = "error";
                    result["error"] = "Invalid XMLTV request (128 KiB maximum)";
                } else if (target == "/api/xmltv/refresh") {
                    const std::string id = payload.get("id", "").asString();
                    if (!streamManager.refreshExternalXmltv(id)) {
                        res.result(http::status::not_found);
                        result["result"] = "error";
                        result["error"] = "Unknown XMLTV source";
                    } else {
                        result["result"] = "ok";
                        result["status"] = streamManager.externalXmltvStatus();
                    }
                } else if (!payload.isMember("sources") ||
                           !payload["sources"].isArray() ||
                           payload["sources"].size() > 32U) {
                    res.result(http::status::bad_request);
                    result["result"] = "error";
                    result["error"] = "Expected XMLTV sources array (up to 32)";
                } else {
                    std::set<std::string> ids;
                    std::vector<XmltvSourceConfig> sources;
                    std::string validationError;
                    for (const auto& entry : payload["sources"]) {
                        if (!entry.isObject()) {
                            validationError = "XMLTV source must be an object"; break;
                        }
                        XmltvSourceConfig cfg = XmltvSourceConfig::fromJson(entry);
                        if (cfg.id.empty() || cfg.id.size() > 64U ||
                            !std::all_of(cfg.id.begin(), cfg.id.end(), [](unsigned char c) {
                                return std::isalnum(c) || c == '-' || c == '_';
                            }) || !ids.insert(cfg.id).second) {
                            validationError = "Invalid or duplicate XMLTV source ID"; break;
                        }
                        if (cfg.url.size() > 2048U ||
                            (cfg.url.rfind("https://", 0) != 0 &&
                             cfg.url.rfind("http://", 0) != 0)) {
                            validationError = "XMLTV source URL must use HTTP(S)"; break;
                        }
                        sources.push_back(std::move(cfg));
                    }
                    if (!validationError.empty()) {
                        res.result(http::status::bad_request);
                        result["result"] = "error";
                        result["error"] = validationError;
                    } else {
                        const auto previous = configManager.config.xmltvSources;
                        configManager.config.xmltvSources = std::move(sources);
                        if (!configManager.save()) {
                            configManager.config.xmltvSources = previous;
                            res.result(http::status::internal_server_error);
                            result["result"] = "error";
                            result["error"] = "Failed to save XMLTV sources";
                        } else {
                            streamManager.configureExternalXmltvSources();
                            result["result"] = "ok";
                            result["status"] = streamManager.externalXmltvStatus();
                        }
                    }
                }
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer, result);
            } else if (target == "/api/config/export") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleConfigExport(req.body());
            } else if (target == "/api/config/import") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleConfigImport(req.body());
            } else if (target == "/api/tickers/import-docx") {
                res.set(http::field::content_type, "application/json");
                Json::Value response(Json::objectValue), payload;
                Json::CharReaderBuilder reader;
                std::string parseError;
                std::istringstream input(req.body());
                if (req.body().size() > 3U*1024U*1024U ||
                    !Json::parseFromStream(reader,input,&payload,&parseError) ||
                    !payload.isObject() || !payload["base64"].isString() ||
                    payload["base64"].asString().size() > 2800000U) {
                    res.result(http::status::bad_request);
                    response["error"] = "Invalid or oversized DOCX upload (2 MiB maximum)";
                } else {
                    const std::string bytes=base64Decode(payload["base64"].asString());
                    std::string imported,importError;
                    if(bytes.empty() || !tickerImportDocx(bytes,imported,importError)) {
                        res.result(http::status::unprocessable_entity);
                        response["error"] = importError.empty() ? "Cannot read DOCX" : importError;
                    } else {
                        response["result"] = "ok";
                        response["text"] = imported;
                    }
                }
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer,response);
            } else if (target == "/api/tickers/save") {
                res.set(http::field::content_type, "application/json");
                Json::Value response(Json::objectValue), payload;
                Json::CharReaderBuilder reader;
                std::string parseError;
                std::istringstream input(req.body());
                if (req.body().size() > 1024U*1024U ||
                    !Json::parseFromStream(reader, input, &payload, &parseError) ||
                    !payload.isObject() || !payload["entries"].isArray()) {
                    res.result(http::status::bad_request);
                    response["error"] = "Invalid ticker request";
                } else {
                    const auto& entries = payload["entries"];
                    std::string error;
                    if (!dvbstreamer5::media::ticker::TickerOverlay::validate(entries, error)) {
                        res.result(http::status::bad_request);
                        response["error"] = error;
                    } else {
                        // Previously saved tickers may reference a channel
                        // that was deleted/renamed after the initial save.
                        // Keep those records unchanged as archived settings;
                        // forbid creating or modifying unknown channel IDs.
                        const Json::Value savedEntries = configManager.tickerOverlaysSnapshot();
                        unsigned int archivedCount = 0U;
                        for (const auto& entry : entries) {
                            const std::string id = entry["stream_id"].asString();
                            bool found = false;
                            for (const auto& cfg : configManager.config.streams) {
                                if (cfg.id == id) {
                                    found = true;
                                    break;
                                }
                            }
                            if (found) continue;
                            bool preserved = false;
                            for (const auto& old : savedEntries) {
                                if (old["stream_id"].asString() == id && old == entry) {
                                    preserved = true;
                                    ++archivedCount;
                                    break;
                                }
                            }
                            if (!preserved) {
                                error = "Ticker references unknown stream: " + id;
                                break;
                            }
                        }
                        if (!error.empty()) {
                            res.result(http::status::bad_request);
                            response["error"] = error;
                        } else {
                            if (!configManager.saveTickerOverlays(entries,error)) {
                                res.result(http::status::internal_server_error);
                                response["error"] = "Cannot save ticker-overlays.json: " + error;
                            } else {
                                dvbstreamer5::media::ticker::TickerOverlay::instance().configure(entries);
                                response["result"] = "ok";
                                response["saved_file"] = "ticker-overlays.json";
                                response["archived_count"] = archivedCount;
                                response["note"] = "Ticker metadata applied live; a first-time switch from zero-copy to CPU-frame overlay may cause a short video decoder reinitialization without restarting the tile.";
                            }
                        }
                    }
                }
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer, response);
            } else if (target == "/api/save-config") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleSaveConfig(req.body());
            } else if (target == "/api/mpts/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleMptsSave(req.body());
            } else if (target == "/api/mpts/action") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleMptsAction(req.body());
            } else if (target == "/api/start-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStartStream(req.body());
            } else if (target == "/api/stop-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStopStream(req.body());
            } else if (target == "/api/update/install") {
                res.set(http::field::content_type, "application/json");
                // Accept the same administrator logins as the web panel:
                // session cookie or HTTP Basic. Never allow an API key to
                // authorize executable installation.
                const auto requestedWith = req.find("X-Requested-With");
                const bool panelAdminAuthenticated =
                    isPanelSessionAuthorized(req) || isAuthorized(req);
                if (apiKeyAttempt || !panelAdminAuthenticated ||
                    requestedWith == req.end() ||
                    requestedWith->value() != "DVBStreamer5") {
                    res.result(http::status::forbidden);
                    res.body() = R"({"result":"error","error":"Panel login and same-origin request required"})";
                } else {
                    // nohup does not escape the systemd service's process cgroup.
                    // A separate transient service survives systemctl stop of
                    // dvbstreamer5.service inside the bundle installer.
                    const auto updateRunId =
                        std::chrono::steady_clock::now().time_since_epoch().count();
                    const std::string command =
                        "/usr/bin/systemd-run --no-block --unit=dvbstreamer5-update-" +
                        std::to_string(updateRunId) +
                        " --description='DVBStreamer5 binary update' "
                        "/bin/bash -c 'exec /bin/bash /opt/DVBStreamer5/updater.sh install " +
                        std::string(kProgramVersion) +
                        " >>/var/log/dvbstreamer5-update.log 2>&1'";
                    const int rc = std::system(command.c_str());
                    if (rc != 0) {
                        res.result(http::status::service_unavailable);
                        res.body() = R"({"result":"error","error":"Unable to start independent systemd update worker"})";
                    } else {
                        res.body() = R"({"result":"started"})";
                    }
                }
            } else if (target == "/api/restart-program") {
                handleRestartProgram();
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"restarting\"}";
            } else if (target == "/api/delete-stream") {
              handleDeleteStream(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/save-subscribers") {
                handleSaveSubscribers(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target.rfind("/api/upload-backup-file", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = handleUploadBackupFile(target, req.body());
            } else if (target == "/api/delete-backup-file") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDeleteBackupFile(req.body());
            } else if (target == "/api/reset-subscriber") {
              handleResetSubscriber(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/block-stream-client") {
              handleBlockStreamClient(req.body(), true);
              res.set(http::field::content_type, "application/json");
              res.body() = R"json({"result":"ok"})json";
            } else if (target == "/api/unblock-stream-client") {
              handleBlockStreamClient(req.body(), false);
              res.set(http::field::content_type, "application/json");
              res.body() = R"json({"result":"ok"})json";
            } else if (target == "/api/cam-client-settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleCamClientSettings(req.body());
            } else if (target == "/api/dvb-signal") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbTune(req.body(), false);
            } else if (target == "/api/dvb-scan") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbTune(req.body(), true);
            } else if (target == "/api/dvb-add-channels") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbAddChannels(req.body());
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
        } else {
            res.result(http::status::method_not_allowed);
            res.body() = "Method Not Allowed";
        }

        res.content_length(res.body().size());
        res.keep_alive(hlsKeepAlive);
        http::write(socket, res);
        if(hlsKeepAlive)
            scheduleHlsKeepAlive(std::move(socket),buffered,hlsRequests+1);
        return;
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (message.find("bad method") != std::string::npos) {
            std::cerr << "HTTP non-HTTP request ignored on web port: " << message << std::endl;
        } else {
            std::cerr << "HTTP session failed: " << message << std::endl;
        }
        return;
    }
}

std::string HttpServer::createPanelSession() {
    std::array<unsigned char, 32U> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        return {};
    const char* digits = "0123456789abcdef";
    std::string token;
    token.reserve(random.size() * 2U);
    for (unsigned char value : random) {
        token.push_back(digits[value >> 4U]);
        token.push_back(digits[value & 15U]);
    }
    std::lock_guard<std::mutex> lock(panelSessionsMutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = panelSessions_.begin(); it != panelSessions_.end();) {
        if (it->second <= now) it = panelSessions_.erase(it);
        else ++it;
    }
    // Keep the session registry bounded under repeated logins.
    if (panelSessions_.size() >= 256U) panelSessions_.clear();
    panelSessions_[token] = now + std::chrono::hours(12);
    return token;
}
bool HttpServer::isPanelSessionAuthorized(
    const http::request<http::string_body>& req) const {
    const auto token = panelCookieValue(req, "dvbstreamer5_panel_session");
    if (token.size() != 64U || !std::all_of(token.begin(), token.end(),
           [](unsigned char c) { return (c >= '0' && c <= '9') ||
                                         (c >= 'a' && c <= 'f'); }))
        return false;
    std::lock_guard<std::mutex> lock(panelSessionsMutex_);
    const auto it = panelSessions_.find(token);
    return it != panelSessions_.end() &&
           std::chrono::steady_clock::now() < it->second;
}
void HttpServer::revokePanelSession(
    const http::request<http::string_body>& req) {
    const auto token = panelCookieValue(req, "dvbstreamer5_panel_session");
    if (token.empty()) return;
    std::lock_guard<std::mutex> lock(panelSessionsMutex_);
    panelSessions_.erase(token);
}

bool HttpServer::requiresAuthentication(const std::string& target) const {
    // XMLTV and output playlists are IPTV-client sidecars, not control APIs.
    // Individual channel visibility is checked against subscriber/IP filtering
    // when the sidecar response is built.
    if (target == "/epg.xml" || target == "/epg.xml.gz" ||
        target == "/playlist.m3u") return false;
    // Control-panel preview endpoints must never become public via a channel
    // whose HLS public name collides with the first URL segment ('api').
    if (target.rfind("/api/", 0) == 0) return true;
    std::string hlsId, hlsFile;
    const bool hlsTarget = resolveHlsTarget(configManager.config.streams, target, hlsId, hlsFile);
    std::string httpMpegTsId;
    const bool httpMpegTsTarget = resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId);
    return target != "/health" &&
           !httpMpegTsTarget &&
           !hlsTarget;
}

bool HttpServer::isAuthorized(const http::request<http::string_body>& req) const {
    const auto auth = req.find(http::field::authorization);
    if (auth == req.end()) {
        return false;
    }

    const std::string header(auth->value());
    const std::string prefix = "Basic ";
    if (header.size() <= prefix.size() || !boost::algorithm::istarts_with(header, prefix)) {
        return false;
    }

    const std::string decoded = base64Decode(header.substr(prefix.size()));
    const auto separator = decoded.find(':');
    if (separator == std::string::npos) {
        return false;
    }

    const std::string login = decoded.substr(0, separator);
    const std::string password = decoded.substr(separator + 1);
    return constantTimeEquals(login, configManager.config.login) &&
           constantTimeEquals(password, configManager.config.password);
}

bool HttpServer::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
  const std::string normalizedClientIp = normalizeIpAddress(clientIp);
  if (streamId.empty() || normalizedClientIp.empty()) {
    return false;
  }
  if (std::find(configManager.subscribers.blockedIps.begin(), configManager.subscribers.blockedIps.end(), normalizedClientIp) !=
      configManager.subscribers.blockedIps.end()) {
    return false;
  }
  if (!configManager.subscribers.filteringEnabled) {
    return true;
  }
  for (const auto& subscriber : configManager.subscribers.subscribers) {
    const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
    const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
    const bool ipMatches = subscriber.enabled &&
      (normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp));
    const bool streamMatches = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
    if (ipMatches && streamMatches) {
      return true;
    }
  }
  return false;
}

bool HttpServer::isStreamClientAllowed(const tcp::socket& socket, const std::string& target,
                                       const http::request<http::string_body>& request) const {
  boost::system::error_code ec;
  const std::string clientIp = socket.remote_endpoint(ec).address().to_string();
  if (ec) {
    return false;
  }
  std::string streamId;
  resolveHttpMpegTsTarget(configManager.config.streams, target, streamId);
  if (streamId.empty()) {
    std::string fileName;
    resolveHlsTarget(configManager.config.streams, target, streamId, fileName);
  }
  if (dvbstreamer5::stream_auth::enabled(configManager.config,streamId)) {
    const auto q=target.find('?');
    const auto agent=request.find(http::field::user_agent);
    const auto host=request.find(http::field::host);
    std::string hlsId,hlsFile;
    const std::string protocol=resolveHlsTarget(configManager.config.streams,
        target,hlsId,hlsFile)?"hls":"http";
    return dvbstreamer5::stream_auth::SessionRegistry::instance().check(
        configManager.config,streamId,clientIp,protocol,
        q==std::string::npos?target:target.substr(0,q),
        agent==request.end()?"":std::string(agent->value()),
        host==request.end()?"":std::string(host->value()),
        q==std::string::npos?"":target.substr(q+1));
  }
  // Backend mode overrides local allow/deny subscriber IP lists.
  return isClientAllowedForStream(streamId,clientIp);
}

void HttpServer::writeUnauthorized(http::response<http::string_body>& res) const {
    res.result(http::status::unauthorized);
    res.set(http::field::www_authenticate, "Basic realm=\"DVBStreamer5\"");
    res.set(http::field::content_type, "text/plain; charset=UTF-8");
    res.body() = "Unauthorized";
}

std::set<int> HttpServer::configuredHttpPorts() const {
    std::set<int> ports;
    if (configManager.config.httpPort > 0 && configManager.config.httpPort <= 65535) {
        ports.insert(configManager.config.httpPort);
    }
    const int defaultPort = configManager.config.httpPort;
    for (const auto& stream : configManager.config.streams) {
        for (const auto& output : streamOutputs(stream)) {
            const std::string type = normalizedOutputType(output);
            if (type == "http" || type == "hls") {
                const int port = streamHttpPort(output, defaultPort);
                if (port > 0 && port <= 65535) ports.insert(port);
            }
        }
    }
    if (ports.empty()) {
        ports.insert(9000);
    }
    return ports;
}

bool HttpServer::validateHttpPortsForConfig(const AppConfig& config, std::string& error) const {
    error.clear();
    struct Owner { std::string label; std::string protocol; };
    std::map<int, Owner> requested;
    if (config.httpPort > 0 && config.httpPort <= 65535) {
        requested.emplace(config.httpPort, Owner{"Control Panel", "HTTP"});
    }
    for (const auto& stream : config.streams) {
        for (const auto& output : streamOutputs(stream)) {
            const std::string type = normalizedOutputType(output);
            if (type != "http" && type != "hls") continue;
            const int port = streamHttpPort(output, config.httpPort);
            if (port <= 0 || port > 65535) continue;
            const std::string label = stream.name.empty() ? stream.id : stream.name;
            requested[port] = Owner{label, type == "hls" ? "HLS" : "HTTP"};
        }
    }
    for (const auto& [port, owner] : requested) {
        {
            std::lock_guard<std::mutex> lock(acceptorsMutex);
            const auto current = acceptors.find(port);
            if (current != acceptors.end() && current->second && current->second->is_open()) continue;
        }
        boost::system::error_code ec;
        tcp::acceptor probe(ioContext);
        tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
        probe.open(endpoint.protocol(), ec);
        if (!ec) probe.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (!ec) probe.bind(endpoint, ec);
        if (probe.is_open()) {
            boost::system::error_code closeEc;
            probe.close(closeEc);
        }
        if (ec) {
            error = owner.protocol + " port " + std::to_string(port) +
                " is unavailable for " + owner.label + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool HttpServer::bindHttpPorts(const std::set<int>& ports) {
    std::lock_guard<std::mutex> acceptorLock(acceptorsMutex);
    boost::system::error_code ec;
    const uint64_t generation = acceptGeneration.fetch_add(1) + 1;

    for (auto& [port, listener] : acceptors) {
        (void)port;
        if (listener && listener->is_open()) {
            listener->cancel(ec);
            listener->close(ec);
        }
    }

    std::unordered_map<int, std::shared_ptr<tcp::acceptor>> nextAcceptors;
    for (int port : ports) {
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid HTTP port: " << port << std::endl;
            continue;
        }

        auto listener = std::make_shared<tcp::acceptor>(ioContext);
        tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
        listener->open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "HTTP server failed to open port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "HTTP server failed to set reuse_address on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->bind(endpoint, ec);
        if (ec) {
            std::cerr << "HTTP server failed to bind port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "HTTP server failed to listen on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }

        doAccept(listener, port, generation);
        nextAcceptors[port] = std::move(listener);
        std::cerr << "HTTP server listening on port " << port << std::endl;
    }

    const bool primaryBound = nextAcceptors.count(configManager.config.httpPort) > 0;
    acceptors = std::move(nextAcceptors);
    if (!primaryBound) {
        std::cerr << "HTTP server primary port is not listening: " << configManager.config.httpPort << std::endl;
    }
    return primaryBound;
}

void HttpServer::refreshHttpPorts() {
    // V10.8.100: config saves are allowed to be partial and the web UI may
    // issue several of them in quick succession. Rebinding an unchanged
    // listener set closes healthy acceptors and can race the next save into
    // EADDRINUSE. Keep the existing listeners when every requested port is
    // already open; bindHttpPorts() remains the recovery/reconfiguration path
    // when the set really changed or a listener disappeared.
    const auto desiredPorts = configuredHttpPorts();
    boost::asio::post(ioContext, [this, desiredPorts]() {
        {
            std::lock_guard<std::mutex> acceptorLock(acceptorsMutex);
            bool unchanged = acceptors.size() == desiredPorts.size();
            if (unchanged) {
                for (int port : desiredPorts) {
                    const auto found = acceptors.find(port);
                    if (found == acceptors.end() || !found->second || !found->second->is_open()) {
                        unchanged = false;
                        break;
                    }
                }
            }
            if (unchanged) return;
        }
        bindHttpPorts(desiredPorts);
    });
}

