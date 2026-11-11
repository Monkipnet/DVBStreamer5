#include "../src/ca/backends/tsdecrypt/TsDecryptPidStore.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace dvbstreamer5::ca::tsdecrypt_detail;

int main() {
    char dir[] = "/tmp/dvb5-tsdecrypt-pid-store-XXXXXX";
    assert(::mkdtemp(dir));
    const std::string path = std::string(dir) + "/cache.json";
    const std::string key = pidStoreKey("sat-test-142", 142,
        "reader-one", "127.0.0.1:33000", "098d", "NEWCAMD");
    assert(key.size() == 16);
    assert(key == pidStoreKey("sat-test-142", 142,
        "reader-one", "127.0.0.1:33000", "098D", "NEWCAMD"));
    assert(key != pidStoreKey("sat-test-132", 132,
        "reader-one", "127.0.0.1:33000", "098D", "NEWCAMD"));
    {
        PidStore store(path);
        assert(store.lookup(key) == 0);
        store.remember(key, 7169);
        assert(store.lookup(key) == 7169);
        store.remember(key, 8191); // reserved null PID
        assert(store.lookup(key) == 7169);
    }
    {
        PidStore restored(path);
        assert(restored.lookup(key) == 7169);
        restored.forget(key, 7254); // cannot erase a newer candidate
        assert(restored.lookup(key) == 7169);
        restored.remember(key, 7254);
        assert(restored.lookup(key) == 7254);
        restored.forget(key, 7254);
        assert(restored.lookup(key) == 0);
    }
    {
        PidStore empty(path);
        assert(empty.lookup(key) == 0);
    }
    struct stat st{};
    assert(::stat(path.c_str(), &st) == 0);
    assert((st.st_mode & 077) == 0); // owner-only cache file
    std::ifstream input(path);
    std::string persisted((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    assert(persisted.find("reader-one") == std::string::npos);
    assert(persisted.find("127.0.0.1") == std::string::npos);
    ::unlink(path.c_str());
    ::rmdir(dir);
    std::puts("PASS TSDecrypt PID store persistence and privacy");
}
