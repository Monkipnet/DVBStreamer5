#pragma once

// DVBStreamer5 Conditional-Access backend plugin ABI.
//
// The host deliberately does not define or expose control-word APIs.  A backend
// is an opaque, local transport processor intended for an operator/manufacturer
// supplied implementation (for example an official smart-card SDK).  The host
// provides reader/service identity plus MPEG-TS buffers; the plugin owns all
// protocol-specific card/session details internally.
//
// This header is C ABI compatible so plugins can be written in C, C++ or any
// language that can export a C function table.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DVBSTREAMER5_CA_BACKEND_ABI_V1 0x00010000u
#define DVBSTREAMER5_CA_BACKEND_ENTRY_V1 "dvbstreamer5_ca_backend_get_api_v1"

// Capability flags advertised by a backend.
enum dvbstreamer5_ca_backend_capability_v1 {
    DVBSTREAMER5_CA_CAP_TS_INPLACE          = 1u << 0, // process MPEG-TS in place, size unchanged
    DVBSTREAMER5_CA_CAP_MULTI_SERVICE       = 1u << 1, // several services can share one reader
    DVBSTREAMER5_CA_CAP_EMM_MANAGED         = 1u << 2, // backend handles subscription/EMM lifecycle internally
    DVBSTREAMER5_CA_CAP_ENTITLEMENT_STATUS  = 1u << 3, // status_json can report entitlement state
    DVBSTREAMER5_CA_CAP_READER_RECONNECT    = 1u << 4  // backend can recover/reopen reader sessions
};

enum dvbstreamer5_ca_backend_result_v1 {
    DVBSTREAMER5_CA_RESULT_OK          = 0,
    DVBSTREAMER5_CA_RESULT_PASSTHROUGH = 1,
    DVBSTREAMER5_CA_RESULT_RETRY       = 2,
    DVBSTREAMER5_CA_RESULT_ERROR       = -1
};

enum dvbstreamer5_ca_log_level_v1 {
    DVBSTREAMER5_CA_LOG_DEBUG = 0,
    DVBSTREAMER5_CA_LOG_INFO  = 1,
    DVBSTREAMER5_CA_LOG_WARN  = 2,
    DVBSTREAMER5_CA_LOG_ERROR = 3
};

struct dvbstreamer5_ca_host_api_v1 {
    uint32_t abi_version;
    void (*log)(int level, const char* backend_id, const char* message);
    uint64_t (*monotonic_ms)(void);
};

struct dvbstreamer5_ca_reader_info_v1 {
    const char* reader_key;          // stable /dev/serial/by-id path when available
    const char* device;              // current tty device, e.g. /dev/ttyUSB0
    const char* serial;              // USB serial identifier
    const char* display_name;        // user-visible reader name
    const char* card_system;         // detected/known card system label
    const char* caid;                // hexadecimal text, e.g. "0652"
    const char* provider;            // hexadecimal/text provider identity
    const char* backend_config_json; // opaque per-reader JSON supplied by user/admin
    uint32_t max_services;
};

struct dvbstreamer5_ca_service_info_v1 {
    const char* stream_id;
    const char* stream_name;
    uint32_t service_id;
    const char* service_name;
    const char* service_provider;
    const char* input_uri;       // original user-visible URI
    const char* service_pids;    // saved DVB PID list when present (colon-separated)
};

struct dvbstreamer5_ca_ts_result_v1 {
    uint64_t packets_seen;
    uint64_t packets_changed;
    uint64_t packets_clear;
    uint64_t packets_scrambled;
    // Optional short, NUL-terminated backend status. The host treats this only
    // as telemetry and never as key material.
    char status[128];
};

struct dvbstreamer5_ca_backend_api_v1 {
    uint32_t abi_version;
    const char* backend_id;      // stable identifier persisted in config
    const char* display_name;
    const char* vendor;
    uint32_t capabilities;

    void* (*create)(const struct dvbstreamer5_ca_host_api_v1* host);
    void (*destroy)(void* instance);

    int (*open_reader)(void* instance,
                       const struct dvbstreamer5_ca_reader_info_v1* reader,
                       char* error, size_t error_size);
    void (*close_reader)(void* instance, const char* reader_key);

    int (*start_service)(void* instance,
                         const char* reader_key,
                         const struct dvbstreamer5_ca_service_info_v1* service,
                         char* error, size_t error_size);
    void (*stop_service)(void* instance, const char* stream_id);

    // Called with complete MPEG-TS packet groups. A backend advertising
    // DVBSTREAMER5_CA_CAP_TS_INPLACE may modify bytes in-place but MUST preserve the
    // buffer length and 188-byte packet framing. A backend that has nothing to
    // do should return DVBSTREAMER5_CA_RESULT_PASSTHROUGH.
    int (*process_ts)(void* instance,
                      const char* stream_id,
                      uint8_t* data, size_t size,
                      struct dvbstreamer5_ca_ts_result_v1* result);

    // Optional JSON snapshot owned by the plugin and valid until the next API
    // call on the same instance. May be NULL.
    const char* (*status_json)(void* instance);
};

typedef const struct dvbstreamer5_ca_backend_api_v1* (*dvbstreamer5_ca_backend_get_api_v1_fn)(void);

#ifdef __cplusplus
} // extern "C"
#endif
