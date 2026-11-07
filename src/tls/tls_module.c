// The TLS module (see tls_module.h): one TLS 1.2 exchange with mbedTLS.
// Built on its own (module.ld) and loaded by the app only to send a problem
// report; nothing here is in the app's memory during play.
//
// The C library functions mbedTLS calls that keep state or reach the system
// (memory, formatting, the clock) are defined here and passed to the app's;
// the few string functions it needs besides are in tls_libc.c. The module
// links neither newlib nor libctru, so it shares no state with the app.
#include "tls/tls_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include "tls/root_ca.inc"

static const TlsHost *g_host;

// ---- The C library, through the app -----------------------------------------------------

void *calloc(size_t n, size_t size) { return g_host->calloc(n, size); }

void free(void *p) { g_host->free(p); }

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) { return g_host->vsnprintf(s, n, fmt, ap); }

int snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int r = g_host->vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

time_t time(time_t *t) { return g_host->time(t); }

struct tm *gmtime_r(const time_t *t, struct tm *out) { return g_host->gmtime_r(t, out); }

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return g_host->gmtime_r(t, &tm);
}

// ---- The exchange --------------------------------------------------------------------------

static int entropy(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    return g_host->random(out, len) == 0 ? 0 : MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
}

static int bio_send(void *io, const unsigned char *buf, size_t len)
{
    const int r = g_host->send(io, buf, len);
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : r;
}

static int bio_recv(void *io, unsigned char *buf, size_t len)
{
    const int r = g_host->recv(io, buf, len);
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : r;
}

static int write_all(mbedtls_ssl_context *ssl, const unsigned char *p, size_t n)
{
    while (n) {
        const int r = mbedtls_ssl_write(ssl, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r < 0)
            return r;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int exchange(const TlsHost *host, void *io, const char *server, const unsigned char *head, size_t head_len,
                    const unsigned char *body, size_t body_len, unsigned char *reply, size_t cap, size_t *got,
                    char *err, size_t err_cap)
{
    g_host = host;
    *got = 0;
    if (err_cap)
        err[0] = 0;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&roots);
    mbedtls_ctr_drbg_init(&drbg);
    const char *step = "seeding the random generator";
    int r = mbedtls_ctr_drbg_seed(&drbg, entropy, NULL, (const unsigned char *)"3ds-d2 report", 13);
    for (size_t i = 0; !r && i < sizeof kSentryRootCas / sizeof kSentryRootCas[0]; i++) {
        step = "reading the root certificates";
        r = mbedtls_x509_crt_parse_der(&roots, kSentryRootCas[i].der, kSentryRootCas[i].size);
    }
    if (!r) {
        step = "setting up TLS";
        r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (!r) {
        mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);   // TLS 1.2
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &roots, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        r = mbedtls_ssl_setup(&ssl, &conf);
    }
    if (!r)
        r = mbedtls_ssl_set_hostname(&ssl, server);
    if (!r) {
        step = "the TLS handshake";
        mbedtls_ssl_set_bio(&ssl, io, bio_send, bio_recv, NULL);
        do
            r = mbedtls_ssl_handshake(&ssl);
        while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char why[160];
            mbedtls_x509_crt_verify_info(why, sizeof why, "", mbedtls_ssl_get_verify_result(&ssl));
            for (char *p = why; *p; p++)
                if (*p == '\n')
                    *p = ' ';
            snprintf(err, err_cap, "the certificate check: %s", why);
        }
    }
    if (!r) {
        step = "sending";
        r = write_all(&ssl, head, head_len);
        if (!r && body_len)
            r = write_all(&ssl, body, body_len);
    }
    if (!r) {
        // The answer, until the server closes the connection (the request
        // asks it to) or the buffer is full.
        step = "reading the answer";
        while (*got < cap) {
            const int n = mbedtls_ssl_read(&ssl, reply + *got, cap - *got);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                break;
            if (n < 0) {
                if (*got == 0)
                    r = n;
                break;   // an answer is there: a connection cut short after it does not matter
            }
            *got += (size_t)n;
        }
        mbedtls_ssl_close_notify(&ssl);
    }
    if (r && err_cap && !err[0]) {
        char what[96];
        mbedtls_strerror(r, what, sizeof what);
        snprintf(err, err_cap, "%s: %s (-0x%04x)", step, what, (unsigned)-r);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&roots);
    mbedtls_ctr_drbg_free(&drbg);
    g_host = NULL;
    return r;
}

__attribute__((used, section(".text.tls_module_entry"))) int tls_module_entry(uint32_t abi, TlsModule *out)
{
    if (abi != TLS_MODULE_ABI)
        return 0;
    out->exchange = exchange;
    out->version = MBEDTLS_VERSION_STRING;
    return 1;
}
