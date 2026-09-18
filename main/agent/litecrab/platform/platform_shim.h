#ifndef LITECRAB_PLATFORM_SHIM_H
#define LITECRAB_PLATFORM_SHIM_H

/*
 * Platform shim for compiling LiteCrab C sources under ESP-IDF.
 *
 * LiteCrab's original code uses raw BSD sockets + OpenSSL for HTTPS, libopus
 * for audio codec, and POSIX libc functions (getenv, clock_gettime, etc.).
 * On ESP32 these are replaced as follows:
 *
 *   OpenSSL  → mbedtls (already linked via esp-tls/mbedtls component)
 *   BSD socket → lwip (available via ESP-IDF lwip component)
 *   libopus   → esp_audio_codec (esp_opus_dec/enc, already linked)
 *   getenv    → stubbed (config via NVS, not env vars)
 *   clock_gettime → esp_timer_get_time
 *   getpid    → 0 (no process IDs on MCU)
 *   usleep    → vTaskDelay(pdMS_TO_TICKS(ms))
 *   memmem/strcasestr/strncasecmp → provided below (lwip libc lacks them)
 *   _Thread_local → removed (single agent task, no TLS needed)
 *
 * This header is included by every LiteCrab .c file that previously included
 * <arpa/inet.h>, <netdb.h>, <sys/socket.h>, <sys/time.h>, <unistd.h>,
 * <opus/opus.h>, or declared OpenSSL ABI externs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

/* ---- PATH_MAX: ESP32 has tiny flash; 256 is more than enough ---- */
#ifndef PATH_MAX
#define PATH_MAX 256
#endif
/* Override any later #define PATH_MAX in LiteCrab headers */
#ifdef PATH_MAX
#undef PATH_MAX
#endif
#define PATH_MAX 256

/* ---- EXT_RAM_BSS_ATTR: use ESP-IDF's definition from esp_attr.h ---- */
#include <esp_attr.h>

/* ---- lwip socket compatibility ---- */
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif

/* close() on lwip sockets uses lwip_close */
#ifndef close
#define close(fd) lwip_close(fd)
#endif

/* SO_RCVTIMEO / SO_SNDTIMEO work on lwip sockets */
/* send/recv map to lwip_send/lwip_recv implicitly */

/* ---- memmem / strcasestr / strncasecmp (not in newlib/picolibc) ---- */
#ifndef HAVE_MEMMEM
static inline void* lc_memmem(const void* haystack, size_t haystacklen,
                              const void* needle, size_t needlelen) {
    if (!needlelen) return (void*)haystack;
    if (needlelen > haystacklen) return NULL;
    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && !memcmp(h + i, n, needlelen))
            return (void*)(h + i);
    }
    return NULL;
}
#define memmem lc_memmem
#endif

#ifndef HAVE_STRCASESTR
static inline char* lc_strcasestr(const char* haystack, const char* needle) {
    size_t nz = strlen(needle);
    if (!nz) return (char*)haystack;
    for (; *haystack; haystack++) {
        size_t i = 0;
        while (i < nz && haystack[i]) {
            char a = haystack[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == nz) return (char*)haystack;
    }
    return NULL;
}
#define strcasestr lc_strcasestr
#endif

#ifndef HAVE_STRNCASECMP
static inline int lc_strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        if (!ca) return 0;
    }
    return 0;
}
#define strncasecmp lc_strncasecmp
#endif

/* ---- clock_gettime → esp_timer ---- */
#include <esp_timer.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 0
#endif
static inline int lc_clock_gettime(int clk, struct timespec* ts) {
    (void)clk;
    int64_t us = esp_timer_get_time();
    ts->tv_sec = (time_t)(us / 1000000);
    ts->tv_nsec = (long)((us % 1000000) * 1000);
    return 0;
}
#define clock_gettime lc_clock_gettime

/* ---- getpid stub ---- */
#ifndef getpid
static inline int lc_getpid(void) { return 0; }
#define getpid lc_getpid
#endif

/* ---- usleep → vTaskDelay ---- */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#ifndef usleep
#define usleep(us) vTaskDelay(pdMS_TO_TICKS((us) / 1000 + (((us) % 1000) ? 1 : 0)))
#endif

/* ---- getenv stub (ESP32 has no environment variables; config via NVS) ---- */
static inline char* lc_getenv(const char* name) { (void)name; return NULL; }
#define getenv lc_getenv

/* ---- setenv/unsetenv stubs ---- */
static inline int lc_setenv(const char* name, const char* value, int overwrite) {
    (void)name; (void)value; (void)overwrite; return 0;
}
#define setenv lc_setenv
static inline void lc_unsetenv(const char* name) { (void)name; }
#define unsetenv lc_unsetenv

/* ---- _Thread_local → empty (single-task agent) ---- */
#define _Thread_local

/* ---- OpenSSL → mbedtls shim ----
 *
 * LiteCrab declares OpenSSL ABI externs inline. We redirect them to a
 * mbedtls-based HTTPS client. The key insight: all four files (asr_sf,
 * tts_sf, llm, ws_codec) use the SAME pattern:
 *   1. SSL_CTX_new / SSL_new / SSL_connect → establish TLS
 *   2. SSL_write / SSL_read → send/receive over TLS
 *   3. SSL_shutdown / SSL_free / SSL_CTX_free → close
 *
 * We map SSL_CTX/SSL to esp_tls context and SSL_* to esp_tls_*. The actual
 * TLS connection uses esp-ml307's NetworkInterface::CreateSsl() under the
 * hood when we go through the Http class — but for the raw-socket path
 * (asr_sf/tts_sf/llm), we use esp_tls directly.
 */
#include <esp_tls.h>

/* Opaque types matching LiteCrab's declarations */
typedef struct ssl_ctx_st { int dummy; } SSL_CTX;
typedef struct ssl_st {
    esp_tls_t* tls;
    int fd;
    int useTls;
} SSL;
typedef struct ssl_method_st { int dummy; } SSL_METHOD;
typedef struct x509_verify_param_st { int dummy; } X509_VERIFY_PARAM;

/* Stubs that satisfy the link but redirect to esp_tls */
static inline const SSL_METHOD* TLS_client_method(void) { return (const SSL_METHOD*)1; }
static inline SSL_CTX* SSL_CTX_new(const SSL_METHOD* m) { (void)m; return (SSL_CTX*)1; }
static inline void SSL_CTX_free(SSL_CTX* c) { (void)c; }
static inline int SSL_CTX_set_default_verify_paths(SSL_CTX* c) { (void)c; return 1; }
static inline void SSL_CTX_set_verify(SSL_CTX* c, int m, void* f) { (void)c; (void)m; (void)f; }
static inline SSL* SSL_new(SSL_CTX* c) { (void)c; return NULL; /* real alloc in conn_open */ }
static inline void SSL_free(SSL* s) { if (s && s->tls) esp_tls_conn_destroy(s->tls); }
static inline int SSL_set_fd(SSL* s, int fd) { if (s) s->fd = fd; return 1; }
static inline long SSL_ctrl(SSL* s, int cmd, long arg, void* ptr) { (void)s; (void)cmd; (void)arg; (void)ptr; return 1; }
static inline X509_VERIFY_PARAM* SSL_get0_param(SSL* s) { (void)s; return (X509_VERIFY_PARAM*)1; }
static inline int X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM* p, const char* h, size_t l) { (void)p; (void)h; (void)l; return 1; }
static inline int SSL_connect(SSL* s) { (void)s; return 1; /* real connect in conn_open */ }
static inline long SSL_get_verify_result(const SSL* s) { (void)s; return 0; }
static inline int SSL_shutdown(SSL* s) { (void)s; return 1; }
static inline int SSL_write(SSL* s, const void* buf, int len) {
    if (!s || !s->tls) return -1;
    int written = esp_tls_conn_write(s->tls, (const unsigned char*)buf, (size_t)len);
    return written;
}
static inline int SSL_read(SSL* s, void* buf, int len) {
    if (!s || !s->tls) return -1;
    int read = esp_tls_conn_read(s->tls, (unsigned char*)buf, (size_t)len);
    if (read == 0) return -1; /* ESP_ERR_ESP_TLS_CONNECTION_CLOSED */
    return read;
}
#define SSL_VERIFY_PEER 0x01
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSEXT_NAMETYPE_host_name 0

/* ---- EVP (SHA-1 + Base64) → mbedtls ---- */
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

typedef struct evp_md_ctx_st { mbedtls_md_context_t ctx; } EVP_MD_CTX;
typedef struct evp_md_st { int dummy; } EVP_MD;

static inline EVP_MD_CTX* EVP_MD_CTX_new(void) {
    EVP_MD_CTX* c = (EVP_MD_CTX*)calloc(1, sizeof(EVP_MD_CTX));
    if (c) mbedtls_md_init(&c->ctx);
    return c;
}
static inline void EVP_MD_CTX_free(EVP_MD_CTX* c) {
    if (c) { mbedtls_md_free(&c->ctx); free(c); }
}
static inline const EVP_MD* EVP_sha1(void) { return (const EVP_MD*)1; }
static inline int EVP_DigestInit_ex(EVP_MD_CTX* c, const EVP_MD* md, void* impl) {
    (void)md; (void)impl;
    if (!c) return 0;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_setup(&c->ctx, info, 0);
    mbedtls_md_starts(&c->ctx);
    return 1;
}
static inline int EVP_DigestUpdate(EVP_MD_CTX* c, const void* data, size_t len) {
    if (!c) return 0;
    mbedtls_md_update(&c->ctx, (const unsigned char*)data, len);
    return 1;
}
static inline int EVP_DigestFinal_ex(EVP_MD_CTX* c, unsigned char* md, unsigned int* s) {
    if (!c) return 0;
    mbedtls_md_finish(&c->ctx, md);
    if (s) *s = 20;
    return 1;
}
static inline int EVP_EncodeBlock(unsigned char* dst, const unsigned char* src, int n) {
    size_t olen = 0;
    mbedtls_base64_encode(dst, 64, &olen, src, (size_t)n);
    return (int)olen;
}

/* ---- libopus → esp_audio_codec ---- */
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_audio_enc.h"
#include "esp_audio_dec.h"

/* Type aliases so LiteCrab code compiles without modification */
#define OpusDecoder void
#define OpusEncoder void
#define OPUS_OK 0
#define OPUS_APPLICATION_AUDIO ESP_OPUS_ENC_APPLICATION_AUDIO

/* opus_int16 is int16_t on ESP32 */
#include <stdint.h>
typedef int16_t opus_int16;
typedef int32_t opus_int32;

/* Map libopus API → esp_audio_codec API */
#define opus_decoder_create(sr, ch, err) lc_opus_decoder_create(sr, ch, err)
#define opus_decoder_destroy(d) lc_opus_decoder_destroy(d)
#define opus_decode(d, data, len, pcm, max, fec) lc_opus_decode(d, data, len, pcm, max, fec)
#define opus_encoder_create(sr, ch, app, err) lc_opus_encoder_create(sr, ch, app, err)
#define opus_encoder_destroy(e) lc_opus_encoder_destroy(e)
#define opus_encode(e, pcm, len, out, max) lc_opus_encode(e, pcm, len, out, max)

/* Wrappers */
static inline void* lc_opus_decoder_create(int sampleRate, int channels, int* err) {
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate = (uint32_t)sampleRate;
    cfg.channel = (uint8_t)channels;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    cfg.self_delimited = false;
    void* h = NULL;
    int rc = esp_opus_dec_open(&cfg, sizeof(cfg), &h);
    if (err) *err = rc;
    return h;
}
static inline void lc_opus_decoder_destroy(void* d) {
    if (d) esp_opus_dec_close(d);
}
static inline int lc_opus_decode(void* d, const unsigned char* data, int len,
                                 opus_int16* pcm, int maxSamples, int fec) {
    (void)fec;
    esp_audio_dec_in_raw_t in = {};
    in.buffer = (uint8_t*)data;
    in.len = (size_t)len;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = (uint8_t*)pcm;
    out.len = (size_t)(maxSamples * sizeof(opus_int16));
    esp_audio_dec_info_t info = {};
    int rc = esp_opus_dec_decode(d, &in, &out, &info);
    if (rc != ESP_AUDIO_ERR_OK) return -1;
    return (int)(out.decoded_size / sizeof(opus_int16));
}
static inline void* lc_opus_encoder_create(int sampleRate, int channels, int app, int* err) {
    (void)app;
    esp_opus_enc_config_t cfg = {};
    cfg.sample_rate = sampleRate;
    cfg.channel = channels;
    cfg.bits_per_sample = 16;
    cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    cfg.complexity = 5;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;
    cfg.enable_vbr = true;
    void* h = NULL;
    int rc = esp_opus_enc_open(&cfg, sizeof(cfg), &h);
    if (err) *err = rc;
    return h;
}
static inline void lc_opus_encoder_destroy(void* e) {
    if (e) esp_opus_enc_close(e);
}
static inline int lc_opus_encode(void* e, const opus_int16* pcm, int samples,
                                  unsigned char* out, int maxOut) {
    esp_audio_enc_in_frame_t in = {};
    in.buffer = (uint8_t*)pcm;
    in.len = (size_t)(samples * sizeof(opus_int16));
    esp_audio_enc_out_frame_t o = {};
    o.buffer = out;
    o.len = (size_t)maxOut;
    int rc = esp_opus_enc_process(e, &in, &o);
    if (rc != ESP_AUDIO_ERR_OK) return -1;
    return (int)o.encoded_bytes;
}

#endif /* LITECRAB_PLATFORM_SHIM_H */
