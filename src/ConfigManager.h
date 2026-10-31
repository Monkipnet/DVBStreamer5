#pragma once

#include <jsoncpp/json/json.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Configuration model and JSON persistence boundary. Keep runtime-only state out of
// these structures; fromJson() deliberately tolerates unknown legacy keys so old
// configuration files can be upgraded without carrying dead options forward.

struct StreamOutputConfig {
    std::string outputType = "udp-cbr";
    std::string outputMode = "listener";
    std::string outputHost = "127.0.0.1";
    int outputPort = 1234;
    std::string interfaceAddress;
    int srtLatencyMs = 120;
    std::string srtPassphrase;
    std::string srtStreamId;
    int srtPbKeyLen = 16;

    Json::Value toJson() const;
    static StreamOutputConfig fromJson(const Json::Value& root);
};

struct MptsServiceConfig {
    std::string streamId;
    // 0 = allocate from the MPTS output service_id_base.
    uint32_t serviceId = 0;

    Json::Value toJson() const;
    static MptsServiceConfig fromJson(const Json::Value& root);
};

struct MptsOutputConfig {
    std::string id;
    std::string name;
    std::string outputHost = "239.255.20.1";
    int outputPort = 5000;
    std::string interfaceAddress;
    bool autoStart = false;
    uint32_t transportStreamId = 1;
    uint32_t originalNetworkId = 1;
    uint32_t serviceIdBase = 1;
    uint32_t pmtPidBase = 0x1000;
    std::vector<MptsServiceConfig> services;

    Json::Value toJson() const;
    static MptsOutputConfig fromJson(const Json::Value& root);
};

struct StreamConfig {
    std::string id;
    std::string name;
    std::string inputUri;
    std::string backupInputUri;
    std::string backupInputType = "url";
    bool backupFileLoop = false;
    std::string outputType = "udp-cbr";
    std::string outputMode = "listener";
    std::string outputHost = "127.0.0.1";
    int outputPort = 1234;
    std::string interfaceAddress;
    std::string inputInterfaceAddress;
    bool inputInterfaceAddressConfigured = false;
    std::string inputMode = "auto";
    int srtInputLatencyMs = 120;
    std::string srtInputPassphrase;
    std::string srtInputStreamId;
    int srtInputPbKeyLen = 16;
    int srtOutputLatencyMs = 120;
    std::string srtOutputPassphrase;
    std::string srtOutputStreamId;
    int srtOutputPbKeyLen = 16;
    // Per-stream HTTP access credentials for HLS manifests, segments and EXT-X-KEY URIs.
    // mode: none | header | query. The value is stored per stream and never shared.
    std::string hlsAccessKeyMode = "none";
    std::string hlsAccessKeyName = "Authorization";
    std::string hlsAccessKeyValue;
    std::string hlsUserAgent = "Mozilla/5.0 DVBStreamer5";
    bool testPattern = false;
    // V10.8.71: channel activation policy. Online keeps the full media/CA
    // pipeline running; OnDemand starts it only for an observable client.
    std::string activationMode = "online"; // online | ondemand
    bool autoStart = false;
    bool remapEnabled = false;
    bool cbr = true;
    uint64_t targetBitrate = 2000000;
    bool transcodeEnabled = false;
    std::string transcodeResolution = "1920x1080";
    std::string transcodeVideoCodec = "h264"; // h264 | hevc | copy
    std::string transcodeVideoEncoder = "auto"; // auto | cpu | nvenc | qsv | vaapi
    std::string transcodeVideoDecoder = "auto"; // auto | nvidia_hw | intel_hw | ittiam | openh264 (H.264 input only)
    uint64_t transcodeVideoBitrate = 6000000;
    double transcodeFps = 25.0;
    bool transcodeDeinterlace = true;
    // Common video encoder controls. Backends apply the supported subset and
    // log any safety clamp/unsupported option instead of silently failing.
    std::string transcodePreset = "balanced"; // fastest | fast | balanced | quality | best
    std::string transcodeProfile = "auto"; // auto | main | high
    std::string transcodeLevel = "auto"; // auto | 3.1 | 4.0 | 4.1 | 5.0 | 5.1
    uint32_t transcodeGopFrames = 50;
    uint32_t transcodeBFrames = 0; // live default: zero-reorder
    uint32_t transcodeRefFrames = 0; // 0 = encoder default
    bool transcodeOpenGop = false;
    uint32_t transcodeAqStrength = 8;
    // 203.75: adaptive HLS is opt-in. The configured rendition remains the
    // primary/highest profile; lower renditions are generated automatically.
    bool transcodeMultibitrateEnabled = false;
    // Runtime-only rendition directory. Empty means the primary rendition.
    std::string hlsVariantName;
    // HLS DVR archive. When enabled, live HLS segments are retained on disk and
    // exposed through Flussonic-compatible archive/timeshift playlist URLs.
    bool hlsArchiveEnabled = false;
    uint32_t hlsArchiveHours = 24;
    std::string hlsArchivePath = "/var/lib/dvbstreamer5/archive";
    // Native HLS container/security: mpegts | cmaf and none | aes-128 | sample-aes.
    // 0 = legacy Auto: MPEG-TS uses 4s/8, CMAF uses 2s/6.
    // Explicit values override the legacy minimum only for this stream.
    std::uint32_t hlsSegmentDuration = 0; // seconds: 0 (Auto) or 1..10
    std::uint32_t hlsSegmentCount = 0;    // live manifest: 0 (Auto) or 3..30
    std::string hlsContainer = "mpegts";
    std::string hlsEncryption = "none";
    std::string hlsEncryptionKeyUri = "key.bin";
    std::string hlsEncryptionKeyHex;
    std::string transcodeAudioCodec = "aac";
    uint64_t transcodeAudioBitrate = 192000;
    uint32_t audioPid = 0;
    uint32_t videoPid = 0;
    uint32_t inputServiceId = 0;
    uint32_t serviceId = 1;
    std::string serviceName;
    std::string serviceProvider;
    bool epgEnabled = false;
    uint32_t epgDays = 3;
    bool epgEitOutput = false; // Opt-in DVB EIT PID 0x12 on finished MPEG-TS
    bool dvbSubtitlesEnabled = false; // Opt-in DVB bitmap subtitles on transcoded MPEG-TS
    std::string dvbSubtitlesLanguage = "auto"; // auto = all languages; otherwise ISO 639-2 code
    std::string dvbSubtitlesTrack = "auto"; // Real PMT track ID PID:language:composition_page_id
    std::string epgMode = "auto"; // auto (DVB, then XMLTV) | dvb | xmltv
    std::string xmltvSourceId;   // empty = search all sources by channel ID
    std::string xmltvChannelId;
    std::string conditionalAccessClient;
    std::vector<StreamOutputConfig> additionalOutputs;
    // 203.67 runtime-inherited global mode; not serialized per stream.
    bool srtVpsVdsOptimization = false;

    Json::Value toJson() const;
    static StreamConfig fromJson(const Json::Value& root);
};


struct CamClientConfig {
    std::string id;
    std::string name;
    unsigned maxServices = 10;
    std::string backendId = "newcamd";
    std::string backendConfig = "{}";

    Json::Value toJson() const;
    static CamClientConfig fromJson(const Json::Value& root);
};

struct XmltvSourceConfig {
    std::string id;
    std::string name;
    std::string url;
    bool enabled = true;
    // Daily refresh in server local time, 24h HH:MM.
    std::string refreshTime = "03:00";
    std::string refreshPeriod = "daily"; // daily | weekly
    int refreshWeekday = 1; // Sun=0, Mon=1, ... Sat=6

    Json::Value toJson() const;
    static XmltvSourceConfig fromJson(const Json::Value& root);
};

struct StreamRestartSchedule {
    bool enabled = false;
    // ISO weekday numbers: Mon=1 ... Sun=7, server-local clock.
    std::vector<int> weekdays = {1,2,3,4,5,6,7};
    std::vector<int> hours = {5};
    int minute = 0;
    Json::Value toJson() const;
    static StreamRestartSchedule fromJson(const Json::Value& root);
};

struct AppConfig {
    std::string login = "admin";
    std::string password = "admin";
    std::string serverName = "DVBStreamer5";
    int httpPort = 9000;
    std::string language = "en";
    std::string telegramToken;
    std::string telegramChatId;
    // Opt-in SRT profile for VPS/VDS/container hosts.
    bool srtVpsVdsOptimization = false;
    // V10.8.110: IPv4 addresses of interfaces where UDP/RTP ingress filtering is enabled.
    // Empty by default: upgrades cannot unexpectedly alter host packet filtering.
    std::vector<std::string> udpInputFilterInterfaces;
    StreamRestartSchedule restartSchedule; // all active stream tiles, never manually stopped ones
    std::vector<StreamConfig> streams;
    std::vector<MptsOutputConfig> mptsOutputs;
    std::vector<XmltvSourceConfig> xmltvSources;
    std::vector<CamClientConfig> camClients;

    Json::Value toJson() const;
    static AppConfig fromJson(const Json::Value& root);
};

struct SubscriberConfig {
    std::string name;
    std::string primaryIp;
    std::string backupIp;
    std::string addedAt;
    bool enabled = true;
    std::vector<std::string> streamIds;

    Json::Value toJson() const;
    static SubscriberConfig fromJson(const Json::Value& root);
};

struct SubscriberListConfig {
    bool filteringEnabled = false;
    std::vector<SubscriberConfig> subscribers;
    std::vector<std::string> blockedIps;

    Json::Value toJson() const;
    static SubscriberListConfig fromJson(const Json::Value& root);
};

class ConfigManager {
public:
    ConfigManager();
    bool load();
    bool save();
    bool loadSubscribers();
    bool saveSubscribers();

    AppConfig config;
    SubscriberListConfig subscribers;

private:
    std::filesystem::path configPath;
    std::mutex fileMutex;
};
