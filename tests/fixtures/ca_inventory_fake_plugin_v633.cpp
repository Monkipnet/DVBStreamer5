#include "ca/CaBackendPluginApi.h"
#include <atomic>
#include <thread>
#include <chrono>

namespace {
std::atomic<bool> entered{false}, release{false};
void* create(const dvbstreamer5_ca_host_api_v1*) { return &entered; }
void destroy(void*) {}
int open(void*, const dvbstreamer5_ca_reader_info_v1*, char*, size_t) {
    entered.store(true);
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return DVBSTREAMER5_CA_RESULT_OK;
}
void close(void*, const char*) {}
int start(void*, const char*, const dvbstreamer5_ca_service_info_v1*, char*, size_t) { return 0; }
int process(void*, const char*, uint8_t*, size_t, dvbstreamer5_ca_ts_result_v1*) { return 1; }
const dvbstreamer5_ca_backend_api_v1 api{DVBSTREAMER5_CA_BACKEND_ABI_V1,"inventory-test","Inventory test","test",0,create,destroy,open,close,start,close,process,nullptr};
}
extern "C" const dvbstreamer5_ca_backend_api_v1* dvbstreamer5_ca_backend_get_api_v1() { return &api; }
extern "C" bool inventory_test_entered() { return entered.load(); }
extern "C" void inventory_test_release() { release.store(true); }
