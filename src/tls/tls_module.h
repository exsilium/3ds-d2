// The TLS module (tls.bin): mbedTLS and Sentry's root certificates, kept
// out of the app so that they take no memory during play. The app loads it
// only to send a problem report (issue #26), before the game starts or after
// it has ended, and frees it right afterwards: src/app/ctr_tls.cpp.
//
// The module is linked at address 0 (module.ld) and shipped as this header,
// its image and the offsets of the words that hold an absolute address
// (pack_module.py); the loader adds the load address to each. It shares no
// state with the app: everything it needs from outside (memory, formatting,
// the clock, random bytes, the connection) comes through TlsHost.
//
// Both sides are built by the same compiler with the same flags (hard-float
// ARMv6K), so the structures below are passed as they are. kTlsAbi changes
// whenever they do; a module of another version is refused.
#ifndef D2_TLS_MODULE_H
#define D2_TLS_MODULE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TLS_MODULE_MAGIC 0x444d4c54u   // "TLMD"
#define TLS_MODULE_ABI 1u

// The file: this header, `image_size` bytes of image, `reloc_count` u32
// offsets into the image. In memory the image is followed by `bss_size`
// zero bytes.
typedef struct TlsModuleFile {
    uint32_t magic, abi;
    uint32_t image_size, bss_size;
    uint32_t entry;   // offset of tls_module_entry
    uint32_t reloc_count;
} TlsModuleFile;

// What the module takes from the app.
typedef struct TlsHost {
    void *(*calloc)(size_t n, size_t size);
    void (*free)(void *p);
    int (*vsnprintf)(char *s, size_t n, const char *fmt, va_list ap);
    time_t (*time)(time_t *t);
    struct tm *(*gmtime_r)(const time_t *t, struct tm *out);
    int (*random)(unsigned char *out, size_t len);   // 0 on success
    // The connection: bytes sent or received (0 from recv: closed), or < 0
    // on an error or a timeout.
    int (*send)(void *io, const unsigned char *buf, size_t len);
    int (*recv)(void *io, unsigned char *buf, size_t len);
} TlsHost;

// One exchange over an open connection `io`: a TLS 1.2 handshake with
// `server` (its name, for SNI and the certificate check against the
// module's roots), the request (`head`, then `body`), and the answer, read
// until the server closes, up to `cap` bytes into `reply` (`*got`). 0 on
// success; otherwise an mbedTLS error, which `err` describes.
typedef int (*TlsExchange)(const TlsHost *host, void *io, const char *server, const unsigned char *head,
                           size_t head_len, const unsigned char *body, size_t body_len, unsigned char *reply,
                           size_t cap, size_t *got, char *err, size_t err_cap);

// The module's entry: its functions, or 0 if `abi` is not its own.
typedef struct TlsModule {
    TlsExchange exchange;
    const char *version;   // mbedTLS's
} TlsModule;
typedef int (*TlsModuleEntry)(uint32_t abi, TlsModule *out);

#ifdef __cplusplus
}
#endif

#endif
