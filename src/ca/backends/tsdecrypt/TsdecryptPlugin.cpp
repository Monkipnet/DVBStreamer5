// V10.8.457: Opt-in external TSDecrypt CA backend.
// Each selected DVBStreamer5 service owns an isolated, bounded stdin/stdout
// pipeline. The legacy newcamd backend remains the default.
#include "../../CaBackendPluginApi.h"
#include "SpawnFdIsolation.h"
#include "TsDecryptTelemetry.h"
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <pthread.h>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

namespace {
constexpr size_t kPacket = 188;
constexpr size_t kMaxQueueBytes = 4 * 1024 * 1024;

// Prefer the executable shipped beside DVBStreamer5. An explicit absolute
// override is useful for isolated A/B testing; host locations are fallbacks.
std::string locateTsdecrypt() {
    std::vector<std::string> paths;
    if (const char* overridePath = std::getenv("DVBSTREAMER5_TSDECRYPT_BINARY")) {
        if (overridePath[0] == '/') paths.emplace_back(overridePath);
    }
    std::array<char, 4096> executable{};
    const ssize_t len = ::readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (len > 0) {
        const std::string path(executable.data(), static_cast<size_t>(len));
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos) paths.push_back(path.substr(0, slash) + "/tsdecrypt");
    }
    paths.push_back("/opt/DVBStreamer5/bin/tsdecrypt");
    paths.push_back("/usr/local/bin/tsdecrypt");
    paths.push_back("/usr/bin/tsdecrypt");
    for (const auto& path : paths) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return {};
}
struct Reader {
    std::string host, user, pass, des, protocol = "NEWCAMD", caid = "098D";
    uint32_t ecmPid = 0;
};
struct Service {
    std::string id, readerKey;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> input;
    std::deque<std::array<uint8_t, kPacket>> output;
    size_t inputBytes = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> exited{false};
    std::atomic<uint64_t> inputDrops{0};
    std::atomic<uint64_t> outputDrops{0};
    // All reported values are metadata only; never retain raw CW/ECM payloads.
    std::atomic<uint64_t> ecmReceived{0}, ecmDuplicates{0}, ecmProcessed{0};
    std::atomic<uint64_t> ecmReportCount{0}, cwValidSignalEvents{0}, cwWarningEvents{0};
    std::atomic<uint64_t> inputScrambledPackets{0}, outputNullPackets{0};
    std::atomic<uint64_t> outputClearMediaPes{0}, outputClearOnScrambledPid{0};
    std::atomic<uint64_t> outputClearMediaPesOnScrambledPid{0};
    std::atomic<bool> cwValidReported{false};
    // Input/output are asynchronous, so classify decoded evidence by observed
    // encrypted PID rather than assuming the next output TS matches the input.
    std::array<bool, 8192> inputScrambledPids{};
    bool csaAltUnverified = false;
    uint64_t lastDiagSecond = 0;
    int inFd = -1, outFd = -1, logFd = -1;
    pid_t pid = -1;
    std::thread writer, reader, logReader;

    ~Service() { stop(); }

    static bool writeBytes(int fd, const uint8_t* bytes, size_t length, std::atomic<bool>& stop) {
        size_t done = 0;
        while (done < length && !stop.load()) {
            const ssize_t n = ::write(fd, bytes + done, length - done);
            if (n > 0) { done += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd event{fd, POLLOUT, 0};
                (void)::poll(&event, 1, 100);
                continue;
            }
            return false;
        }
        return done == length;
    }

    bool start(const Reader& config, uint32_t sid, std::string& error) {
        if (!sid || sid > 65535) { error = "valid DVB service_id required"; return false; }
        csaAltUnverified = config.caid == "098D";
        const std::string tsdecryptBinary = locateTsdecrypt();
        if (tsdecryptBinary.empty()) {
            error = "TSDecrypt executable missing (expected bin/tsdecrypt or /usr/bin/tsdecrypt)"; return false;
        }
        // Prepare all allocations before fork: the main host is multithreaded.
        // The child must call only async-signal-safe operations before exec.
        std::vector<std::string> args{
            "tsdecrypt", "-M", std::to_string(sid), "-c", "VIDEOGUARD",
            "-C", config.caid, "-A", config.protocol, "-s", config.host,
            "-U", config.user, "-P", config.pass,
            // -K suppresses raw ECM/CW dumps; -H emits numeric aggregate
            // ECM reports. The child stderr is parsed with an allowlist only.
            "-K", "-H", "10", "-J", "10", "-3", "-p"
        };
        if (config.protocol == "NEWCAMD") { args.push_back("-B"); args.push_back(config.des); }
        if (config.ecmPid) { args.push_back("-X"); args.push_back(std::to_string(config.ecmPid)); }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        int inputPipe[2]{-1,-1}, outputPipe[2]{-1,-1}, logPipe[2]{-1,-1};
        if (::pipe(inputPipe) != 0) { error = "input pipe failed"; return false; }
        if (::pipe(outputPipe) != 0) {
            ::close(inputPipe[0]); ::close(inputPipe[1]); error = "output pipe failed"; return false;
        }
        // Snapshot the limit before fork; the child must avoid getrlimit/sysconf.
        const unsigned int inheritedFdLimit =
            dvbstreamer5::ca::tsdecrypt_detail::inheritedFdLimit();
        if (::pipe(logPipe) != 0) {
            ::close(inputPipe[0]); ::close(inputPipe[1]);
            ::close(outputPipe[0]); ::close(outputPipe[1]);
            error = "TSDecrypt diagnostics pipe failed"; return false;
        }
        pid = ::fork();
        if (pid < 0) {
            ::close(inputPipe[0]); ::close(inputPipe[1]);
            ::close(outputPipe[0]); ::close(outputPipe[1]);
            ::close(logPipe[0]); ::close(logPipe[1]);
            error = "fork failed"; return false;
        }
        if (pid == 0) {
            if (::dup2(inputPipe[0], STDIN_FILENO) < 0 ||
                ::dup2(outputPipe[1], STDOUT_FILENO) < 0) _exit(127);
            if (::dup2(logPipe[1], STDERR_FILENO) < 0) _exit(127);
            // Do not keep HTTP listeners, accepted client sockets or other
            // parent resources alive in this external decoder. The sweep also
            // closes both pipe originals; only descriptors 0, 1, 2 survive.
            dvbstreamer5::ca::tsdecrypt_detail::closeInheritedFds(inheritedFdLimit);
            ::execv(tsdecryptBinary.c_str(), argv.data());
            _exit(127);
        }
        ::close(inputPipe[0]); ::close(outputPipe[1]); ::close(logPipe[1]);
        logFd = logPipe[0];
        inFd = inputPipe[1];
        outFd = outputPipe[0];
        ::fcntl(inFd, F_SETFL, ::fcntl(inFd, F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(outFd, F_SETFL, ::fcntl(outFd, F_GETFL, 0) | O_NONBLOCK);
        writer = std::thread([this] {
            // Prevent SIGPIPE from terminating DVBStreamer5 on tsdecrypt exit.
            sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGPIPE);
            (void)pthread_sigmask(SIG_BLOCK, &mask, nullptr);
            while (!stopping.load()) {
                std::vector<uint8_t> chunk;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                        return stopping.load() || !input.empty();
                    });
                    if (stopping.load()) break;
                    if (input.empty()) continue;
                    chunk = std::move(input.front()); input.pop_front();
                    inputBytes -= chunk.size();
                }
                if (!writeBytes(inFd, chunk.data(), chunk.size(), stopping)) break;
            }
            exited.store(true);
        });
        logReader = std::thread([this] {
            // Parse only fixed, numeric summaries. Never relay raw stderr:
            // upstream CLI may expose passwords, ECMs or control words.
            std::array<char, 4096> chunk{};
            std::string line;
            line.reserve(512);
            while (!stopping.load()) {
                pollfd event{logFd, POLLIN, 0};
                if (::poll(&event, 1, 100) <= 0) continue;
                const ssize_t n = ::read(logFd, chunk.data(), chunk.size());
                if (n == 0) break;
                if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
                for (ssize_t i = 0; i < n; ++i) {
                    if (chunk[i] != '\n' && chunk[i] != '\r') {
                        if (line.size() < 1024) line.push_back(chunk[i]);
                        continue;
                    }
                    const auto report = dvbstreamer5::ca::tsdecrypt_detail::parseEcmReport(line.c_str());
                    if (report.valid) {
                        ecmReceived.fetch_add(report.received);
                        ecmDuplicates.fetch_add(report.duplicates);
                        ecmProcessed.fetch_add(report.processed);
                        ecmReportCount.fetch_add(1);
                    }
                    if (dvbstreamer5::ca::tsdecrypt_detail::hasSafeLogEvent(
                            line.c_str(), "CW  | OK: Valid code word was received.")) {
                        cwValidSignalEvents.fetch_add(1);
                        cwValidReported.store(true);
                    } else if (dvbstreamer5::ca::tsdecrypt_detail::hasSafeLogEvent(
                            line.c_str(), "CW  | *ERR* No valid code word")) {
                        cwWarningEvents.fetch_add(1);
                        cwValidReported.store(false);
                    }
                    line.clear();
                }
            }
        });
        reader = std::thread([this] {
            std::array<uint8_t, 32 * 1024> temp{};
            std::vector<uint8_t> unaligned;
            while (!stopping.load()) {
                pollfd event{outFd, POLLIN, 0};
                if (::poll(&event, 1, 100) <= 0) continue;
                const ssize_t got = ::read(outFd, temp.data(), temp.size());
                if (got == 0) break;
                if (got < 0) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    break;
                }
                unaligned.insert(unaligned.end(), temp.begin(), temp.begin() + got);
                size_t position = 0;
                std::lock_guard<std::mutex> lock(mutex);
                while (position + kPacket <= unaligned.size()) {
                    if (unaligned[position] != 0x47) { ++position; continue; }
                    if (position + kPacket < unaligned.size() &&
                        unaligned[position + kPacket] != 0x47) { ++position; continue; }
                    std::array<uint8_t, kPacket> packet{};
                    std::memcpy(packet.data(), unaligned.data() + position, kPacket);
                    // Do not forward encrypted data as decoded content.
                    if ((packet[3] & 0xC0) != 0) {
                        packet.fill(0xFF);
                        packet[0] = 0x47; packet[1] = 0x1F; packet[2] = 0xFF; packet[3] = 0x10;
                    }
                    if (output.size() >= kMaxQueueBytes / kPacket) {
                        output.pop_front(); outputDrops.fetch_add(1);
                    }
                    output.push_back(std::move(packet));
                    position += kPacket;
                }
                if (position) unaligned.erase(unaligned.begin(), unaligned.begin() + position);
                if (unaligned.size() > 2 * kPacket) unaligned.clear();
            }
            exited.store(true);
        });
        return true;
    }

    void stop() {
        if (stopping.exchange(true)) return;
        cv.notify_all();
        if (pid > 0) ::kill(pid, SIGTERM);
        if (writer.joinable()) writer.join();
        if (reader.joinable()) reader.join();
        if (logReader.joinable()) logReader.join();
        if (inFd >= 0) { ::close(inFd); inFd = -1; }
        if (outFd >= 0) { ::close(outFd); outFd = -1; }
        if (logFd >= 0) { ::close(logFd); logFd = -1; }
        if (pid > 0) {
            int status = 0; bool done = false;
            for (int i=0; i<20; ++i) {
                pid_t r = ::waitpid(pid, &status, WNOHANG);
                if (r == pid || (r < 0 && errno == ECHILD)) { done = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!done) { ::kill(pid, SIGKILL); (void)::waitpid(pid, &status, 0); }
            pid = -1;
        }
    }
};
struct Backend {
    std::mutex mutex;
    std::map<std::string, Reader> readers;
    std::map<std::string, std::shared_ptr<Service>> services;
    std::string snapshot = "{}";
};
void err(char* dest, size_t length, const std::string& message) {
    if (!dest || !length) return;
    std::snprintf(dest, length, "%s", message.c_str());
}
bool parseReader(const char* raw, Reader& out, std::string& error) {
    Json::Value j;
    Json::CharReaderBuilder b; b["collectComments"] = false;
    std::string parseError;
    std::unique_ptr<Json::CharReader> parser(b.newCharReader());
    if (!raw || !parser->parse(raw, raw + std::strlen(raw), &j, &parseError) || !j.isObject()) {
        error = "invalid backend_config JSON"; return false;
    }
    const auto value = [&](const char* key) -> std::string {
        const auto& v = j[key]; return v.isString() ? v.asString() : std::string();
    };
    const int port = j.get("port", 0).asInt();
    const std::string host = value("host");
    out.user = value("user"); out.pass = value("pass"); out.des = value("des");
    out.protocol = value("protocol");
    if (out.protocol.empty()) out.protocol = "NEWCAMD";
    std::transform(out.protocol.begin(), out.protocol.end(), out.protocol.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    out.caid = value("caid");
    if (out.caid.empty()) out.caid = "098D";
    if (out.caid.size() != 4 ||
        !std::all_of(out.caid.begin(), out.caid.end(),
            [](unsigned char ch) { return std::isxdigit(ch); })) {
        error = "CAID must contain exactly four hexadecimal digits"; return false;
    }
    if (port < 1 || port > 65535 || host.empty() || host.find_first_of(" \r\n\t") != std::string::npos ||
        out.user.empty() || out.pass.empty() ||
        (out.protocol != "NEWCAMD" && out.protocol != "CS378X") ||
        (out.protocol == "NEWCAMD" &&
         (out.des.size() != 28 ||
          !std::all_of(out.des.begin(), out.des.end(), [](unsigned char c){ return std::isxdigit(c); })))) {
        error = "invalid CAMD host/port/protocol/credentials or DES key"; return false;
    }
    out.host = host + ":" + std::to_string(port);
    const int ecm = j.get("ecm_pid", 0).asInt();
    if (ecm < 0 || ecm > 8190) { error = "invalid ecm_pid"; return false; }
    out.ecmPid = static_cast<uint32_t>(ecm);
    return true;
}
void* create(const dvbstreamer5_ca_host_api_v1*) { return new Backend; }
void destroy(void* self) { delete static_cast<Backend*>(self); }
int openReader(void* self, const dvbstreamer5_ca_reader_info_v1* info,
               char* error, size_t errorLen) {
    if (!self || !info || !info->reader_key) return DVBSTREAMER5_CA_RESULT_ERROR;
    Reader config; std::string reason;
    if (!parseReader(info->backend_config_json, config, reason)) {
        err(error,errorLen,reason); return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    auto* backend = static_cast<Backend*>(self);
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->readers[info->reader_key] = std::move(config);
    return DVBSTREAMER5_CA_RESULT_OK;
}
void closeReader(void* self,const char* key) {
    if (!self || !key) return;
    auto* b = static_cast<Backend*>(self);
    std::lock_guard<std::mutex> lock(b->mutex);
    b->readers.erase(key);
}
int startService(void* self, const char* readerKey, const dvbstreamer5_ca_service_info_v1* info,
                 char* error, size_t errorLen) {
    if (!self || !readerKey || !info || !info->stream_id) return DVBSTREAMER5_CA_RESULT_ERROR;
    auto* b = static_cast<Backend*>(self);
    Reader config;
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        const auto it = b->readers.find(readerKey);
        if (it == b->readers.end()) { err(error,errorLen,"unknown TSDecrypt reader"); return DVBSTREAMER5_CA_RESULT_ERROR; }
        config = it->second;
        if (b->services.count(info->stream_id)) return DVBSTREAMER5_CA_RESULT_OK;
    }
    auto session = std::make_shared<Service>();
    session->id = info->stream_id; session->readerKey = readerKey;
    std::string reason;
    if (!session->start(config, info->service_id, reason)) {
        err(error,errorLen,reason); return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        if (b->services.count(session->id)) { session->stop(); return DVBSTREAMER5_CA_RESULT_OK; }
        b->services.emplace(session->id, std::move(session));
    }
    return DVBSTREAMER5_CA_RESULT_OK;
}
void stopService(void* self,const char* id) {
    if (!self || !id) return;
    auto* b = static_cast<Backend*>(self);
    std::shared_ptr<Service> removed;
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto it = b->services.find(id);
        if (it == b->services.end()) return;
        removed = std::move(it->second); b->services.erase(it);
    }
    removed->stop();
}
int processTs(void* self, const char* id, uint8_t* data, size_t size,
              dvbstreamer5_ca_ts_result_v1* result) {
    if (!self || !id || !data || (size % kPacket)) return DVBSTREAMER5_CA_RESULT_ERROR;
    auto* b = static_cast<Backend*>(self);
    std::shared_ptr<Service> service;
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto it = b->services.find(id);
        if (it == b->services.end()) return DVBSTREAMER5_CA_RESULT_ERROR;
        service = it->second;
    }
    uint64_t clear = 0, scrambled = 0, changed = 0;
    std::vector<uint8_t> original(data, data+size);
    {
        std::lock_guard<std::mutex> lock(service->mutex);
        if (!service->stopping.load()) {
            for (size_t i=0; i<size; i+=kPacket) {
                const auto facts = dvbstreamer5::ca::tsdecrypt_detail::inspectTsPacket(data+i, kPacket);
                if (facts.valid && facts.scrambled && facts.pid != 0x1fff) {
                    service->inputScrambledPids[facts.pid] = true;
                    service->inputScrambledPackets.fetch_add(1);
                }
            }
            if (service->inputBytes + size <= kMaxQueueBytes) {
                service->input.push_back(std::move(original)); service->inputBytes += size;
                service->cv.notify_one();
            } else service->inputDrops.fetch_add(1);
        }
        // Bounded asynchronous pipeline: fill missing decoded output with null TS,
        // never stall a production relay thread waiting for CAMD.
        for (size_t i=0; i<size; i+=kPacket) {
            if (!service->output.empty()) {
                auto packet = std::move(service->output.front());
                service->output.pop_front();
                std::memcpy(data+i,packet.data(),kPacket);
                const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
                if (pid != 0x1FFF) {
                    const auto facts = dvbstreamer5::ca::tsdecrypt_detail::inspectTsPacket(
                        packet.data(), packet.size());
                    if (facts.valid) {
                        if (!facts.scrambled) {
                            ++clear;
                            if (service->inputScrambledPids[pid]) {
                                ++changed; // clear output on an encrypted input PID; not proof of CW
                                service->outputClearOnScrambledPid.fetch_add(1);
                                if (facts.clearMediaPesStart)
                                    service->outputClearMediaPesOnScrambledPid.fetch_add(1);
                            }
                            if (facts.clearMediaPesStart)
                                service->outputClearMediaPes.fetch_add(1);
                        } else ++scrambled;
                    }
                } else service->outputNullPackets.fetch_add(1);
            } else {
                service->outputNullPackets.fetch_add(1);
                std::memset(data+i,0xFF,kPacket);
                data[i]=0x47; data[i+1]=0x1F; data[i+2]=0xFF; data[i+3]=0x10;
            }
        }
        // Rate-limit key-free counters; do not put child log lines in journal.
        const uint64_t nowSec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (nowSec >= service->lastDiagSecond + 15) {
            service->lastDiagSecond = nowSec;
            std::fprintf(stderr,
                "TSDECRYPT DIAG stream=%s ecm_received=%llu ecm_processed=%llu "
                "ecm_duplicates=%llu cw_valid_events=%llu cw_warnings=%llu "
                "input_scrambled=%llu output_clear_encrypted_pid=%llu "
                "clear_media_pes=%llu clear_media_pes_encrypted_pid=%llu "
                "output_null=%llu csa_alt_verified=no\n",
                service->id.c_str(),
                static_cast<unsigned long long>(service->ecmReceived.load()),
                static_cast<unsigned long long>(service->ecmProcessed.load()),
                static_cast<unsigned long long>(service->ecmDuplicates.load()),
                static_cast<unsigned long long>(service->cwValidSignalEvents.load()),
                static_cast<unsigned long long>(service->cwWarningEvents.load()),
                static_cast<unsigned long long>(service->inputScrambledPackets.load()),
                static_cast<unsigned long long>(service->outputClearOnScrambledPid.load()),
                static_cast<unsigned long long>(service->outputClearMediaPes.load()),
                static_cast<unsigned long long>(service->outputClearMediaPesOnScrambledPid.load()),
                static_cast<unsigned long long>(service->outputNullPackets.load()));
        }
    }
    if (result) {
        result->packets_seen = size / kPacket;
        result->packets_changed = changed;
        result->packets_clear = clear;
        result->packets_scrambled = scrambled;
        // A CW event or a cleared scrambling bit is NOT proof that CSA-ALT
        // decoded usable video. A valid clear PES header is stronger evidence,
        // but is still not a full media decode verification.
        const bool hasPesEvidence = service->outputClearMediaPesOnScrambledPid.load() > 0;
        const char* state = service->exited.load() ? "TSDECRYPT_EXITED" :
            hasPesEvidence ? "TSDECRYPT_PES_EVIDENCE" :
            service->cwValidReported.load() ? "TSDECRYPT_CW_SIGNAL_NO_PES" :
            service->ecmProcessed.load() ? "TSDECRYPT_ECM_NO_PES" :
            "TSDECRYPT_WAITING_ECM";
        std::snprintf(result->status,sizeof(result->status),"%s",state);
    }
    return DVBSTREAMER5_CA_RESULT_OK;
}
const char* statusJson(void* self) {
    if (!self) return "{}";
    auto* b = static_cast<Backend*>(self);
    std::lock_guard<std::mutex> lock(b->mutex);
    Json::Value root(Json::objectValue);
    root["id"] = "tsdecrypt";
    root["services"] = Json::UInt(b->services.size());
    Json::Value list(Json::arrayValue);
    for (const auto& entry : b->services) {
        Json::Value item(Json::objectValue);
        item["stream_id"] = entry.first;
        item["status"] = entry.second->exited.load() ? "EXITED" : "ACTIVE";
        item["input_drops"] = Json::UInt64(entry.second->inputDrops.load());
        item["output_drops"] = Json::UInt64(entry.second->outputDrops.load());
        item["ecm_received_reported"] = Json::UInt64(entry.second->ecmReceived.load());
        item["ecm_processed_reported"] = Json::UInt64(entry.second->ecmProcessed.load());
        item["ecm_duplicates_reported"] = Json::UInt64(entry.second->ecmDuplicates.load());
        item["ecm_reports"] = Json::UInt64(entry.second->ecmReportCount.load());
        item["cw_valid_signal_events"] = Json::UInt64(entry.second->cwValidSignalEvents.load());
        item["cw_warning_events"] = Json::UInt64(entry.second->cwWarningEvents.load());
        item["cw_count_available"] = false;
        item["input_scrambled_packets"] = Json::UInt64(entry.second->inputScrambledPackets.load());
        item["output_clear_packets_on_input_scrambled_pid"] =
            Json::UInt64(entry.second->outputClearOnScrambledPid.load());
        item["output_clear_media_pes_starts"] = Json::UInt64(entry.second->outputClearMediaPes.load());
        item["output_clear_media_pes_on_input_scrambled_pid"] =
            Json::UInt64(entry.second->outputClearMediaPesOnScrambledPid.load());
        item["output_null_packets"] = Json::UInt64(entry.second->outputNullPackets.load());
        item["decryption_verified"] = false;
        item["csa_alt_verified"] = false;
        item["note"] = "PES evidence is not confirmed video decode; iCAM CSA-ALT is unverified";
        list.append(std::move(item));
    }
    root["active"] = list;
    Json::StreamWriterBuilder fmt; fmt["indentation"] = "";
    b->snapshot = Json::writeString(fmt,root);
    return b->snapshot.c_str();
}
const dvbstreamer5_ca_backend_api_v1 kApi {
    DVBSTREAMER5_CA_BACKEND_ABI_V1,
    "tsdecrypt", "TSDecrypt (external, experimental)", "DVBStreamer5 / gfto",
    DVBSTREAMER5_CA_CAP_TS_INPLACE | DVBSTREAMER5_CA_CAP_MULTI_SERVICE,
    &create,&destroy,&openReader,&closeReader,&startService,&stopService,&processTs,&statusJson
};
} // namespace
extern "C" const dvbstreamer5_ca_backend_api_v1* dvbstreamer5_ca_backend_get_api_v1() {
    return &kApi;
}
