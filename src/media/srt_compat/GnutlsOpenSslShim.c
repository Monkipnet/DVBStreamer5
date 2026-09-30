#include <stddef.h>
#include <limits.h>
#include <openssl/rand.h>

/*
 * Minimal ABI compatibility layer for the single GnuTLS primitive used by
 * libsrt-gnutls 1.5.x.  The implementation is backed by OpenSSL so deployed
 * DVBStreamer5 does not require the GnuTLS runtime.
 */
int gnutls_rnd(int level, void* data, size_t len) {
    (void)level;
    if ((!data && len != 0) || len > INT_MAX) return -1;
    if (len == 0) return 0;
    return RAND_bytes((unsigned char*)data, (int)len) == 1 ? 0 : -1;
}
