#include "CardManager.h"

#include "CaBackend.h"
#include "OscamMiniManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace {

Json::Value parseBackendConfig(const std::string& value) {
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(value.empty() ? "{}" : value);
    if (!Json::parseFromStream(builder, input, &parsed, &errors)) return Json::Value(Json::objectValue);
    return parsed;
}

std::string endpointText(const Json::Value& config) {
    const std::string host = config.get("host", "").asString();
    const int port = config.get("port", 0).asInt();
    if (host.empty() || port <= 0) return {};
    return host + ":" + std::to_string(port);
}

std::string trimLocal(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    return value;
}

bool isLoopbackNewcamdHost(std::string host) {
    host = trimLocal(std::move(host));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return host == "127.0.0.1" || host == "localhost" ||
           host == "::1" || host == "[::1]";
}

bool procNetHasListener(const char* path, int port) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    std::getline(file, line); // header
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string slot;
        std::string localAddress;
        std::string remoteAddress;
        std::string state;
        if (!(row >> slot >> localAddress >> remoteAddress >> state)) continue;
        if (state != "0A") continue; // TCP_LISTEN

        const auto colon = localAddress.rfind(':');
        if (colon == std::string::npos || colon + 1 >= localAddress.size()) continue;
        try {
            const int listedPort = static_cast<int>(
                std::stoul(localAddress.substr(colon + 1), nullptr, 16));
            if (listedPort == port) return true;
        } catch (...) {
        }
    }
    return false;
}

bool newcamdPortListening(int port) {
    return procNetHasListener("/proc/net/tcp", port) ||
           procNetHasListener("/proc/net/tcp6", port);
}

std::vector<int> localNewcamdPorts(const std::vector<CamClientConfig>& clients) {
    std::set<int> uniquePorts;
    for (const auto& client : clients) {
        const std::string backendId = client.backendId.empty() ? "newcamd" : client.backendId;
        if (backendId != "newcamd") continue;

        const Json::Value config = parseBackendConfig(client.backendConfig);
        const std::string host = config.get("host", "").asString();
        const int port = config.get("port", 0).asInt();
        if (!isLoopbackNewcamdHost(host) || port < 1 || port > 65535) continue;
        uniquePorts.insert(port);
    }
    return std::vector<int>(uniquePorts.begin(), uniquePorts.end());
}

std::string portsText(const std::vector<int>& ports) {
    std::ostringstream out;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (i != 0) out << ',';
        out << ports[i];
    }
    return out.str();
}

void waitForLocalNewcamdListeners(const std::vector<CamClientConfig>& clients) {
    const std::vector<int> ports = localNewcamdPorts(clients);
    if (ports.empty()) return;

    constexpr auto kPollInterval = std::chrono::milliseconds(25);
    constexpr auto kTimeout = std::chrono::seconds(5);
    const auto started = std::chrono::steady_clock::now();

    for (;;) {
        const bool ready = std::all_of(ports.begin(), ports.end(), [](int port) {
            return newcamdPortListening(port);
        });
        const auto now = std::chrono::steady_clock::now();
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - started);

        if (ready) {
            std::cerr << "OSCam-mini Newcamd readiness ready ports=" << portsText(ports)
                      << " wait_ms=" << waited.count() << std::endl;
            return;
        }
        if (now - started >= kTimeout) {
            std::cerr << "OSCam-mini Newcamd readiness timeout ports=" << portsText(ports)
                      << " timeout_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout).count()
                      << std::endl;
            return; // Fail open: preserve the pre-V183 startup behavior on OSCam failure.
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace

CardManager& CardManager::instance() {
    static CardManager manager;
    return manager;
}

void CardManager::configure(const std::vector<CamClientConfig>& clients) {
    std::vector<CamClientConfig> normalized;
    normalized.reserve(clients.size());
    for (auto client : clients) {
        if (client.id.empty()) continue;
        client.maxServices = std::clamp(client.maxServices, 1u, kMaxConfigurableServices);
        if (client.name.empty()) client.name = client.id;
        if (client.backendId.empty() || client.backendId == "passthrough") client.backendId = "newcamd";
        if (client.backendConfig.empty()) client.backendConfig = "{}";
        normalized.push_back(std::move(client));
    }
    std::vector<CamClientConfig> configured;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        clients_ = std::move(normalized);
        configured = clients_;
    }
    std::string oscamSyncError;
    const bool oscamSyncOk =
        OscamMiniManager::instance().synchronizeCamClients(configured, oscamSyncError);
    if (!oscamSyncOk) {
        std::cerr << "OSCam-mini CAM listener synchronization failed: " << oscamSyncError << std::endl;
    } else {
        // synchronizeCamClients() can restart an already-active OSCam service.
        // systemctl restart returns when Type=simple has started the process,
        // a few milliseconds before OSCam opens its Newcamd sockets. Do not let
        // parallel satellite auto-start race that listener initialization.
        waitForLocalNewcamdListeners(configured);
    }
    CaBackendManager::instance().configure(configured);
}

std::string CardManager::isoTime(const std::chrono::system_clock::time_point& value) {
    if (value.time_since_epoch().count() == 0) return {};
    const std::time_t time = std::chrono::system_clock::to_time_t(value);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string CardManager::displayName(const CamClientConfig& client) {
    return client.name.empty() ? client.id : client.name;
}

CamClientConfig CardManager::findClient(const std::string& requested) const {
    if (requested.empty()) return {};
    std::lock_guard<std::mutex> lock(configMutex_);
    for (const auto& client : clients_) {
        if (client.id == requested || client.name == requested) return client;
    }
    return {};
}

Json::Value CardManager::clientStatusJson(const CamClientConfig& client) {
    Json::Value item;
    item["id"] = client.id;
    item["name"] = displayName(client);
    item["max_services"] = std::clamp(client.maxServices, 1u, kMaxConfigurableServices);
    item["unlimited_services"] = false;
    item["backend_id"] = client.backendId.empty() ? "newcamd" : client.backendId;
    const Json::Value backendConfig = parseBackendConfig(client.backendConfig);
    item["configured"] = !client.id.empty();
    item["endpoint"] = endpointText(backendConfig);
    item["host"] = backendConfig.get("host", "").asString();
    item["port"] = backendConfig.get("port", 0).asInt();
    item["user"] = backendConfig.get("user", "").asString();
    item["des_configured"] = !backendConfig.get("des", "").asString().empty();
    item["status"] = item["endpoint"].asString().empty() ? "NOT_CONFIGURED" : "CONFIGURED";
    return item;
}

bool CardManager::reserveService(const StreamConfig& config, std::string* error) {
    if (error) error->clear();
    if (config.conditionalAccessClient.empty()) return true;
    if (config.conditionalAccessClient == "auto") {
        if (error) *error = "CAM auto routing is disabled: select a CAM client for this channel";
        return false;
    }

    CamClientConfig client = findClient(config.conditionalAccessClient);
    if (client.id.empty()) {
        if (error) *error = "CAM client is not configured: " + config.conditionalAccessClient;
        return false;
    }
    client.maxServices = std::clamp(client.maxServices, 1u, kMaxConfigurableServices);
    if (client.backendId.empty()) client.backendId = "newcamd";
    if (client.backendConfig.empty()) client.backendConfig = "{}";

    std::lock_guard<std::mutex> lock(mutex_);
    if (slotsByStream_.count(config.id)) return true;

    const auto used = static_cast<unsigned>(std::count_if(
        slotsByStream_.begin(), slotsByStream_.end(), [&](const auto& entry) {
            return entry.second.clientId == client.id;
        }));
    if (used >= client.maxServices) {
        if (error) {
            *error = "CAM client service limit reached for " + client.id + ": " +
                     std::to_string(used) + "/" + std::to_string(client.maxServices);
        }
        return false;
    }

    CaBackendReaderBinding backendClient;
    backendClient.key = client.id;
    backendClient.displayName = displayName(client);
    backendClient.maxServices = client.maxServices;
    backendClient.backendId = client.backendId;
    backendClient.backendConfig = client.backendConfig;

    std::string backendError;
    if (!CaBackendManager::instance().startService(config, backendClient, &backendError)) {
        if (error) *error = backendError.empty() ? "failed to reserve CAM backend service" : backendError;
        std::cerr << "CAM manager: backend reservation rejected client=" << client.id
                  << " stream=" << config.id
                  << " backend=" << client.backendId
                  << " error=" << backendError << std::endl;
        return false;
    }

    ServiceSlot slot;
    slot.streamId = config.id;
    slot.streamName = config.name;
    slot.serviceId = config.inputServiceId ? config.inputServiceId : config.serviceId;
    slot.serviceProvider = config.serviceProvider;
    slot.requestedClient = config.conditionalAccessClient;
    slot.clientId = client.id;
    slot.clientName = displayName(client);
    slot.backendId = client.backendId;
    slot.lastStatus = "RESERVED";
    slotsByStream_[config.id] = std::move(slot);

    std::cerr << "CAM manager: reserved client=" << client.id
              << " stream=" << config.id
              << " sid=" << (config.inputServiceId ? config.inputServiceId : config.serviceId)
              << " slots=" << (used + 1) << "/" << client.maxServices
              << " backend=" << client.backendId << std::endl;
    return true;
}

void CardManager::activateService(const std::string& streamId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = slotsByStream_.find(streamId);
        if (found == slotsByStream_.end()) return;
        found->second.active = true;
        if (found->second.lastStatus.empty() || found->second.lastStatus == "RESERVED") {
            found->second.lastStatus = "ACTIVE";
        }
        std::cerr << "CAM manager: active client=" << found->second.clientId
                  << " stream=" << found->second.streamId
                  << " sid=" << found->second.serviceId << std::endl;
    }
    CaBackendManager::instance().markServiceActive(streamId);
}

void CardManager::releaseService(const std::string& streamId) {
    bool existed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = slotsByStream_.find(streamId);
        if (found == slotsByStream_.end()) return;
        std::cerr << "CAM manager: released client=" << found->second.clientId
                  << " stream=" << found->second.streamId
                  << " sid=" << found->second.serviceId << std::endl;
        slotsByStream_.erase(found);
        existed = true;
    }
    if (existed) CaBackendManager::instance().stopService(streamId);
}

void CardManager::releaseAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slotsByStream_.clear();
    }
    CaBackendManager::instance().stopAll();
}


void CardManager::recordTransaction(const std::string& streamId, uint64_t latencyMs,
                                    bool success, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slotsByStream_.find(streamId);
    if (found == slotsByStream_.end()) return;
    ServiceSlot& slot = found->second;
    ++slot.transactions;
    if (success) ++slot.successfulTransactions;
    else ++slot.failedTransactions;
    slot.lastLatencyMs = latencyMs;
    slot.maxLatencyMs = std::max(slot.maxLatencyMs, latencyMs);
    slot.averageLatencyMs += (static_cast<double>(latencyMs) - slot.averageLatencyMs) /
                             static_cast<double>(slot.transactions);
    slot.lastTransactionAt = std::chrono::system_clock::now();
    if (!status.empty()) slot.lastStatus = status;
}

Json::Value CardManager::slotToJson(const ServiceSlot& slot) const {
    Json::Value item;
    item["stream_id"] = slot.streamId;
    item["stream_name"] = slot.streamName;
    item["service_id"] = slot.serviceId;
    item["service_provider"] = slot.serviceProvider;
    item["client"] = slot.clientId;
    item["client_name"] = slot.clientName;
    item["requested_client"] = slot.requestedClient;
    item["active"] = slot.active;
    item["status"] = slot.lastStatus;
    item["reserved_at"] = isoTime(slot.reservedAt);
    item["transactions"] = Json::UInt64(slot.transactions);
    item["transactions_ok"] = Json::UInt64(slot.successfulTransactions);
    item["transactions_failed"] = Json::UInt64(slot.failedTransactions);
    item["last_latency_ms"] = Json::UInt64(slot.lastLatencyMs);
    item["max_latency_ms"] = Json::UInt64(slot.maxLatencyMs);
    item["average_latency_ms"] = slot.averageLatencyMs;
    item["last_transaction_at"] = isoTime(slot.lastTransactionAt);
    item["backend_id"] = slot.backendId;
    return item;
}

Json::Value CardManager::snapshot() const {
    std::vector<CamClientConfig> clients;
    std::vector<ServiceSlot> slots;
    {
        std::lock_guard<std::mutex> configLock(configMutex_);
        clients = clients_;
    }
    bool cardManagerBusy = false;
    {
        // Never wait behind a slow CAM start/stop while serving /api/state.
        // A busy snapshot returns the configured clients and marks runtime
        // slot telemetry as temporarily unavailable instead of blocking HTTP.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            slots.reserve(slotsByStream_.size());
            for (const auto& [streamId, slot] : slotsByStream_) {
                (void)streamId;
                slots.push_back(slot);
            }
        } else {
            cardManagerBusy = true;
        }
    }

    Json::Value root;
    root["mode"] = "cam-client+plugin-backend";
    root["ca_backend"] = CaBackendManager::instance().snapshot();
    root["network_ca_server"] = false;
    root["external_key_export"] = false;
    root["default_max_services"] = kDefaultMaxServices;
    root["max_configurable_services"] = kMaxConfigurableServices;
    root["service_limit_enabled"] = true;
    root["reserved_services"] = Json::UInt(slots.size());
    root["busy"] = cardManagerBusy;
    if (cardManagerBusy) root["status"] = "CARD_MANAGER_BUSY";

    Json::Value clientList(Json::arrayValue);
    for (const auto& client : clients) {
        Json::Value item = clientStatusJson(client);
        Json::Value services(Json::arrayValue);
        unsigned active = 0;
        for (const auto& slot : slots) {
            if (slot.clientId != client.id) continue;
            Json::Value service = slotToJson(slot);
            service["backend"] = CaBackendManager::instance().streamState(slot.streamId);
            services.append(service);
            if (slot.active) ++active;
        }
        const unsigned maxServices = item.get("max_services", kDefaultMaxServices).asUInt();
        const unsigned used = services.size();
        item["services"] = services;
        item["services_used"] = used;
        item["services_active"] = active;
        item["services_free"] = maxServices > used ? maxServices - used : 0;
        item["unlimited_services"] = false;
        clientList.append(item);
    }
    root["clients"] = clientList;
    return root;
}

Json::Value CardManager::streamState(const std::string& streamId) const {
    ServiceSlot slot;
    bool foundSlot = false;
    bool cardManagerBusy = false;
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            cardManagerBusy = true;
        } else {
            const auto found = slotsByStream_.find(streamId);
            if (found != slotsByStream_.end()) {
                slot = found->second;
                foundSlot = true;
            }
        }
    }

    // Query the backend only after CardManager::mutex_ is released. The CA
    // manager uses a non-blocking snapshot path, so a backend teardown cannot
    // make the dashboard disappear.
    const Json::Value backendState = CaBackendManager::instance().streamState(streamId);
    if (cardManagerBusy) {
        Json::Value result;
        result["managed"] = true;
        result["busy"] = true;
        result["status"] = "CARD_MANAGER_BUSY";
        result["backend"] = backendState;
        result["native_card_backend"] = backendState.get("native_plugin", false).asBool();
        return result;
    }
    if (!foundSlot) {
        Json::Value result;
        result["managed"] = false;
        result["status"] = "UNBOUND";
        result["backend"] = backendState;
        result["native_card_backend"] = false;
        return result;
    }

    Json::Value result = slotToJson(slot);
    result["managed"] = true;
    result["backend"] = backendState;
    result["native_card_backend"] = backendState.get("native_plugin", false).asBool();
    result["client_display_name"] = slot.clientName;
    result["backend_id"] = slot.backendId;
    return result;
}
