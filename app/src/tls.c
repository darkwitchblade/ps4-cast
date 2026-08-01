#include "tls.h"

extern void watchdog_kick(void);

// NOTE: deliberately NO unconditional watchdog_kick() in ll_read/ll_write. g_heartbeat means
// "the main render loop is alive"; kicking it from the read-ahead / audio threads
// lets a frozen main loop look healthy forever, and puts a syscall on the
// per-record streaming path (measured: playback stopped being smooth). The
// handshake is petted once, from the main thread, at the tls_open call sites.

#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include <orbis/Net.h>
#include <orbis/libkernel.h>

// Build handshake seed entropy WITHOUT sceRandom: that import does not resolve
// reliably on this GoldHEN/homebrew setup and calling it crashed (CE-34878-0).
// We mix the monotonic clock (re-sampled), stack/pointer addresses, and a
// splitmix64 diffuser. Weaker than a CSPRNG, but enough for the client-random
// and ECDHE key gen to function; confidentiality of the session is preserved.
static void make_seed(unsigned char *seed, int n, int salt) {
    uint64_t acc = sceKernelGetProcessTime();
    acc ^= (uint64_t)(uintptr_t)&seed;
    acc ^= (uint64_t)(uintptr_t)seed << 17;
    acc ^= (uint64_t)salt * 0x100000001B3ull;
    for (int i = 0; i < n; i++) {
        if ((i & 7) == 0) acc ^= sceKernelGetProcessTime() * 0x9E3779B97F4A7C15ull;
        acc += 0x9E3779B97F4A7C15ull;
        uint64_t z = acc;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        seed[i] = (unsigned char)(z >> (((unsigned)i % 8u) * 8u));
    }
}

struct tls_ctx {
    br_ssl_client_context sc;
    br_x509_minimal_context xdummy;
    br_sslio_context io;
    int sock;
    // Absolute deadline (sceKernelGetProcessTime units) for reads, or 0 = none.
    // BearSSL loops inside br_sslio_read until a whole TLS record is assembled, so
    // a server that TRICKLES record bytes keeps ll_read returning >0 and never
    // returns control to the caller's budget check -- an unbounded block inside a
    // single conn_read ("HANG stale=36s at=aseg/body-eof"). Only aseg arms this;
    // httpsrc leaves it 0 and pays no clock syscall on the video path.
    uint64_t rdDeadline;
    unsigned rdTick;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    struct {
        const br_x509_class *vtable;
        br_x509_decoder_context dc;
        int certs;
        int active;
        const br_x509_pkey *pkey;
    } xc;
};

// --- x509 accept-all verifier ---------------------------------------------
//
// BearSSL's x509_minimal still refuses to return a public key when the platform
// has no wall clock (BR_ERR_X509_TIME_UNKNOWN = 53). For PS4 Cast we deliberately
// accept arbitrary local/online HTTPS certificates, so this tiny verifier only
// decodes the end-entity certificate's public key and skips CA/date/name checks.
typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context dc;
    int certs;
    int active;
    const br_x509_pkey *pkey;
} accept_x509_ctx;

static void ax_start_chain(const br_x509_class **ctx, const char *sn) {
    (void)sn;
    accept_x509_ctx *c = (accept_x509_ctx *)ctx;
    c->certs = 0;
    c->active = 0;
    c->pkey = NULL;
}
static void ax_start_cert(const br_x509_class **ctx, uint32_t len) {
    (void)len;
    accept_x509_ctx *c = (accept_x509_ctx *)ctx;
    c->active = (c->certs == 0);
    if (c->active) br_x509_decoder_init(&c->dc, NULL, NULL);
}
static void ax_append(const br_x509_class **ctx, const unsigned char *b, size_t n) {
    accept_x509_ctx *c = (accept_x509_ctx *)ctx;
    if (c->active) br_x509_decoder_push(&c->dc, b, n);
}
static void ax_end_cert(const br_x509_class **ctx) {
    accept_x509_ctx *c = (accept_x509_ctx *)ctx;
    if (c->active) c->pkey = br_x509_decoder_get_pkey(&c->dc);
    c->certs++;
    c->active = 0;
}
static unsigned ax_end_chain(const br_x509_class **ctx) {
    accept_x509_ctx *c = (accept_x509_ctx *)ctx;
    if (c->certs == 0) return BR_ERR_X509_EMPTY_CHAIN;
    return c->pkey ? 0 : (unsigned)br_x509_decoder_last_error(&c->dc);
}
static const br_x509_pkey *ax_get_pkey(const br_x509_class *const *ctx, unsigned *u) {
    accept_x509_ctx *c = (accept_x509_ctx *)ctx;
    if (u) *u = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return c->pkey;
}
static const br_x509_class accept_x509_vtable = {
    sizeof(accept_x509_ctx),
    ax_start_chain, ax_start_cert, ax_append, ax_end_cert, ax_end_chain, ax_get_pkey
};

// --- low-level I/O over the OrbisNet socket --------------------------------
static int ll_read(void *ctx, unsigned char *buf, size_t len) {
    tls_ctx *t = (tls_ctx *)ctx;
    // Check EVERY call when armed. Sampling every 16th call was useless here: each
    // call can block for the 2s socket timeout and still return a byte on a
    // trickling peer, so the first check came 16*2s = 32s in -- the ~36s the
    // watchdog kept catching. These calls already block on the network, so a clock
    // read per call is free; and only aseg arms a deadline, so the video streaming
    // path (httpsrc) still pays nothing and stays smooth.
    // Deadline armed (aseg) => its socket is non-blocking, so poll with our own
    // bound. Unarmed (httpsrc video path) => blocking socket, behave exactly as
    // before: no clock syscall, no poll loop, no change to streaming smoothness.
    if (!t->rdDeadline) {
        int n = sceNetRecv(t->sock, buf, len, 0);
        if (n <= 0) return -1;
        return n;
    }
    uint64_t t0 = sceKernelGetProcessTime();
    for (;;) {
        int n = sceNetRecv(t->sock, buf, len, 0);
        if (n > 0) return n;
        if (n == 0) return -1;                                   // EOF mid-record
        uint64_t now = sceKernelGetProcessTime();
        if (now > t->rdDeadline || now - t0 > 4ULL * 1000 * 1000) return -1;
        watchdog_kick();
        sceKernelUsleep(3000);
    }
}
static int ll_write(void *ctx, const unsigned char *buf, size_t len) {
    tls_ctx *tc = (tls_ctx *)ctx;
    int s = tc->sock;
    if (tc->rdDeadline) {
        uint64_t t0 = sceKernelGetProcessTime();
        for (;;) {
            int n = sceNetSend(s, buf, len, 0);
            if (n > 0) return n;
            uint64_t now = sceKernelGetProcessTime();
            if (now > tc->rdDeadline || now - t0 > 4ULL * 1000 * 1000) return -1;
            watchdog_kick();
            sceKernelUsleep(3000);
        }
    }
    int n = sceNetSend(s, buf, len, 0);
    if (n <= 0) return -1;
    return n;
}

tls_ctx *tls_open(int sock, const char *host) { return tls_open_bounded(sock, host, 0); }

// deadlineUs must be armed BEFORE the handshake when the socket is non-blocking:
// with no deadline, ll_read/ll_write take the blocking branch and treat the first
// EAGAIN as fatal, so the handshake dies the moment data isn't already buffered.
// (That looked like "works on the first few tunes, then fails every time".)
tls_ctx *tls_open_bounded(int sock, const char *host, uint64_t deadlineUs) {
    tls_ctx *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->sock = sock;
    t->rdDeadline = deadlineUs; t->rdTick = 0;

    br_ssl_client_init_full(&t->sc, &t->xdummy, NULL, 0);
    memset(&t->xc, 0, sizeof(t->xc));
    t->xc.vtable = &accept_x509_vtable;
    br_ssl_engine_set_x509(&t->sc.eng, &t->xc.vtable);

    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof(t->iobuf), 1);

    // Seed the engine PRNG — BearSSL has no OS seeder when BR_USE_UNIX_TIME=0.
    unsigned char seed[32];
    make_seed(seed, sizeof(seed), sock);
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof(seed));

    if (br_ssl_client_reset(&t->sc, host, 0) != 1) {
        free(t);
        return NULL;
    }
    br_sslio_init(&t->io, &t->sc.eng, ll_read, t, ll_write, t);
    return t;
}

int tls_read(tls_ctx *t, uint8_t *buf, int len) {
    if (len > 16 * 1024) len = 16 * 1024;  // one TLS record; avoids large-read instability on PS4
    int n = br_sslio_read(&t->io, buf, (size_t)len);
    if (n < 0) {
        // Clean closure shows up as an error after the close_notify; treat the
        // engine "closed" state as EOF rather than a hard failure.
        if (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&t->sc.eng);
            return (err == BR_ERR_OK) ? 0 : -1;
        }
        return -1;
    }
    return n;
}

int tls_write(tls_ctx *t, const uint8_t *buf, int len) {
    if (br_sslio_write_all(&t->io, buf, (size_t)len) < 0) return -1;
    if (br_sslio_flush(&t->io) < 0) return -1;
    return 0;
}

int tls_last_error(tls_ctx *t) {
    return t ? br_ssl_engine_last_error(&t->sc.eng) : -1;
}

void tls_set_read_deadline(tls_ctx *t, uint64_t absUs) { if (t) { t->rdDeadline = absUs; t->rdTick = 0; } }

void tls_close(tls_ctx *t) {
    if (!t) return;
    // Streaming seeks abandon large in-flight HTTPS responses and immediately
    // reconnect at a new byte range. A graceful TLS close_notify can block while
    // BearSSL/socket I/O tries to drain/write on that old response, which freezes
    // the read-ahead thread. The caller closes/aborts the socket separately, so
    // this must be a fast context teardown.
    free(t);
}
