#include "CaBackend.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <thread>
bool contains(const Json::Value& state, const std::string& id) {
    for (const auto& item : state["backends"]) if (item["id"].asString() == id && item["usable"].asBool()) return true;
    return false;
}
int main(int argc,char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin=argv[1];
    setenv("DVBSTREAMER5_CA_PLUGIN_DIR",plugin.parent_path().c_str(),1);
    setenv("DVBSTREAMER5_INSTANCE_ID","inventory-test",1);
    void* lib=dlopen(plugin.c_str(),RTLD_NOW|RTLD_LOCAL);
    assert(lib);
    auto entered=reinterpret_cast<bool(*)()>(dlsym(lib,"inventory_test_entered"));
    auto release=reinterpret_cast<void(*)()>(dlsym(lib,"inventory_test_release"));
    assert(entered && release);
    auto& manager=CaBackendManager::instance();
    assert(contains(manager.snapshot(),"inventory-test"));
    StreamConfig stream; stream.id="inventory-stream";
    CaBackendReaderBinding reader; reader.key="inventory-reader"; reader.backendId="inventory-test";
    bool started=false;
    std::thread worker([&]{started=manager.startService(stream,reader);});
    for(int i=0;i<2000&&!entered();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(entered());
    for(int i=0;i<20;++i) {
        const auto busy=manager.snapshot();
        assert(busy["busy"].asBool() && busy["backends_cached"].asBool());
        assert(contains(busy,"inventory-test"));
    }
    release(); worker.join(); assert(started);
    manager.stopService(stream.id);
    std::filesystem::rename(plugin,plugin.string()+".disabled");
    manager.configure({});
    assert(!contains(manager.snapshot(),"inventory-test"));
    std::filesystem::rename(plugin.string()+".disabled",plugin);
    dlclose(lib);
    std::cout<<"PASS CA inventory during blocked plugin open and after removal\n";
}
