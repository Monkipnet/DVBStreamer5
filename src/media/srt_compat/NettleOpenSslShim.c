#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <openssl/aes.h>
#include <openssl/evp.h>

typedef void nettle_cipher_func(const void*, size_t, uint8_t*, const uint8_t*);

/*
 * Debian/Ubuntu libsrt-gnutls 1.5.x uses the legacy Nettle AES context ABI.
 * OpenSSL 3's AES_KEY has the same 244-byte storage footprint on Linux x86_64,
 * which lets this compatibility module keep the caller-owned context opaque.
 */
_Static_assert(sizeof(AES_KEY) == 244, "unsupported OpenSSL AES_KEY ABI");

void nettle_aes_set_encrypt_key(void* ctx, size_t length, const uint8_t* key) {
    if (!ctx || !key || (length != 16 && length != 24 && length != 32)) return;
    (void)AES_set_encrypt_key(key, (int)(length * 8), (AES_KEY*)ctx);
}

void nettle_aes_set_decrypt_key(void* ctx, size_t length, const uint8_t* key) {
    if (!ctx || !key || (length != 16 && length != 24 && length != 32)) return;
    (void)AES_set_decrypt_key(key, (int)(length * 8), (AES_KEY*)ctx);
}

void nettle_aes_encrypt(const void* ctx, size_t length, uint8_t* dst, const uint8_t* src) {
    if (!ctx || !dst || !src) return;
    while (length >= AES_BLOCK_SIZE) {
        AES_encrypt(src, dst, (const AES_KEY*)ctx);
        src += AES_BLOCK_SIZE;
        dst += AES_BLOCK_SIZE;
        length -= AES_BLOCK_SIZE;
    }
}

void nettle_aes_decrypt(const void* ctx, size_t length, uint8_t* dst, const uint8_t* src) {
    if (!ctx || !dst || !src) return;
    while (length >= AES_BLOCK_SIZE) {
        AES_decrypt(src, dst, (const AES_KEY*)ctx);
        src += AES_BLOCK_SIZE;
        dst += AES_BLOCK_SIZE;
        length -= AES_BLOCK_SIZE;
    }
}

static void increment_counter(uint8_t* counter, size_t size) {
    for (size_t i = size; i > 0; --i) {
        if (++counter[i - 1] != 0) break;
    }
}

void nettle_ctr_crypt(const void* ctx, nettle_cipher_func* cipher,
                      size_t block_size, uint8_t* counter,
                      size_t length, uint8_t* dst, const uint8_t* src) {
    uint8_t stream[64];
    if (!ctx || !cipher || !counter || !dst || !src || block_size == 0 || block_size > sizeof(stream)) return;
    while (length != 0) {
        cipher(ctx, block_size, stream, counter);
        const size_t chunk = length < block_size ? length : block_size;
        for (size_t i = 0; i < chunk; ++i) dst[i] = src[i] ^ stream[i];
        increment_counter(counter, block_size);
        dst += chunk;
        src += chunk;
        length -= chunk;
    }
}

void nettle_pbkdf2_hmac_sha1(size_t key_length, const uint8_t* key,
                              unsigned iterations,
                              size_t salt_length, const uint8_t* salt,
                              size_t length, uint8_t* dst) {
    if (!key || !salt || !dst || iterations == 0 ||
        key_length > INT_MAX || salt_length > INT_MAX || length > INT_MAX) return;
    (void)PKCS5_PBKDF2_HMAC_SHA1((const char*)key, (int)key_length,
                                 salt, (int)salt_length,
                                 (int)iterations, (int)length, dst);
}
