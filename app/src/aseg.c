#include "aseg.h"
#include "tls.h"
#include "urlopt.h"
#include "native_http.h"

// main.c: pet the freeze watchdog during a legitimately-progressing blocking op.
// Safe here because every fetch is bounded by ASEG_FETCH_BUDGET_US, so this can
// never mask a true freeze — it only stops a SLOW-but-bounded playlist/segment
// fetch from being mistaken for one (the "HANG watchdog stale=35s" fail-close
// seen while zapping past several unreachable channels in a row).
extern void watchdog_kick(void);
extern const char *watchdog_note(const char *w);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

// Minimal "download a whole small resource" client for the HLS audio rendition.
// Audio-only segments are tiny (~tens-to-hundreds of KB), so a blocking
// fetch-into-RAM per segment is plenty and keeps this completely separate from
// the video read-ahead reader (httpsrc) — the two never share a socket.

typedef struct {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;   // network byte order
    uint32_t addr;   // network byte order
    uint8_t  zero[8];
} ps4_sockaddr_in;

#define ORBIS_NET_SOL_SOCKET   0xffff
#define ORBIS_NET_SO_SNDTIMEO  0x1005
#define ORBIS_NET_SO_RCVTIMEO  0x1006
#define ASEG_FETCH_CAP         (16 * 1024 * 1024)

static int                 g_pool = -1;
static int                 g_sock = -1;
static tls_ctx            *g_tls  = NULL;
static volatile int        g_abort = 0;
static OrbisPthreadMutex   g_fetchMtx;
static int                 g_fetchMtxInit = 0;

#define ASEG_BUDGET_SEGMENT_US (25ULL * 1000 * 1000)
#define ASEG_BUDGET_PLAYLIST_US (6ULL * 1000 * 1000)
static volatile uint64_t g_fetchBudgetUs = ASEG_BUDGET_SEGMENT_US;
// Start of the current fetch, so do_request's header loop is bounded by the SAME
// budget as the body loops. Written under g_fetchMtx (one fetch at a time).
static uint64_t g_fetchT0 = 0;
static int  g_lastStatus = -1;      // last parsed HTTP status (0 = unparseable)
static char g_lastLine[28] = "";    // first bytes of the last status line
static int  g_badReuse = -1, g_badHop = -1;   // was the failing request on a REUSED socket, and which redirect hop
static char g_badLine[28] = "";              // status line OF THE FAILING request
static char g_badPath[64] = "";              // request path we sent
static int  g_badPathLen = -1;               // full length before truncation into g_badPath
static char g_badStage[16] = "";             // where the failing request died: dns/connect/tls/req/hdr
static char g_nativeHost[256] = "";          // host whose CDN rejected BearSSL but accepted SceHttp
#define ASEG_FETCH_BUDGET_US (g_fetchBudgetUs)

static char     g_host[256];
static char     g_path[1024];
static uint16_t g_port = 80;
static uint32_t g_addr = 0;
static int      g_tlsmode = 0;

// HTTP keep-alive: live HLS hits the same CDN host for every segment, so reusing
// the TLS connection (no per-segment handshake) is the difference between fetch
// at ~realtime (can't build a buffer) and ~2.5x realtime (buffer fills, smooth).
static int      g_kaAlive = 0;     // g_sock/g_tls are open + reusable
static char     g_kaHost[256] = "";
static uint16_t g_kaPort = 0;
static int      g_kaTlsmode = 0;

static const char *ci_strstr(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == nl) return hay;
    }
    return NULL;
}

static int net_pool(void) {
    if (g_pool < 0) g_pool = sceNetPoolCreate("ps4cast_aseg", 16 * 1024, 0);
    return g_pool;
}

static void conn_close(void) {
    if (g_tls) { tls_close(g_tls); g_tls = NULL; }
    if (g_sock >= 0) { sceNetSocketClose(g_sock); g_sock = -1; }
}

static int parse_url(const char *url) {
    if (strncmp(url, "http://", 7) == 0)       { g_tlsmode = 0; url += 7; g_port = 80; }
    else if (strncmp(url, "https://", 8) == 0) { g_tlsmode = 1; url += 8; g_port = 443; }
    else return -1;

    const char *slash = strchr(url, '/');
    const char *hostend = slash ? slash : url + strlen(url);
    char hostport[300];
    int hl = (int)(hostend - url);
    if (hl <= 0 || hl >= (int)sizeof(hostport)) return -2;
    memcpy(hostport, url, hl); hostport[hl] = '\0';

    char *colon = strchr(hostport, ':');
    if (colon) { *colon = '\0'; int p = atoi(colon + 1); if (p > 0) g_port = (uint16_t)p; }
    strncpy(g_host, hostport, sizeof(g_host) - 1); g_host[sizeof(g_host) - 1] = '\0';

    if (slash) { strncpy(g_path, slash, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = '\0'; }
    else strcpy(g_path, "/");
    return 0;
}

// 1-entry DNS cache: live HLS hits the same CDN host for every segment, so
// resolving each time added 100s of ms of variable latency per fetch (a big part
// of the segment-fetch time that kept the prefetcher from getting ahead).
static char     g_dnsHost[256] = "";
static uint32_t g_dnsAddr = 0;

static int resolve_host(void) {
    OrbisNetInAddr a;
    memset(&a, 0, sizeof(a));
    if (sceNetInetPton(ORBIS_NET_AF_INET, g_host, &a.s_addr) > 0) { g_addr = a.s_addr; return 0; }
    if (g_dnsAddr && strcmp(g_dnsHost, g_host) == 0) { g_addr = g_dnsAddr; return 0; }  // cache hit
    int pool = net_pool();
    if (pool < 0) return -1;
    OrbisNetId rid = sceNetResolverCreate("ps4cast_ares", pool, 0);
    if (rid < 0) return -2;
    // 4s x2 (max 8s), not the original 8s x3 (24s) and not 3s x1. 24s ran before
    // the fetch budget could bail and blew the channel-switch watchdog grace, but
    // a single 3s try was too strict the other way: it failed on hosts that
    // resolve fine (a channel whose server answers in 0.4s from a PC reported
    // "hls fetch failed" on console). Two tries with a sane timeout covers a
    // transient DNS hiccup while staying well inside the grace.
    watchdog_kick();
    const char *pv_dns = watchdog_note("dns");   // unabortable; the one blocking call we cannot pet through
    int rc = sceNetResolverStartNtoa(rid, g_host, &a, 4 * 1000 * 1000, 2, 0);
    watchdog_note(pv_dns);
    watchdog_kick();
    sceNetResolverDestroy(rid);
    if (rc < 0) return -3;
    g_addr = a.s_addr;
    strncpy(g_dnsHost, g_host, sizeof(g_dnsHost) - 1); g_dnsHost[sizeof(g_dnsHost) - 1] = '\0';
    g_dnsAddr = a.s_addr;                                                                // cache it
    return 0;
}

#define ORBIS_NET_SO_NBIO 0x1200   // non-blocking I/O socket option

static int tcp_connect(void) {
    if (g_abort) return -1;                  // already tearing down: don't begin a fresh connect
    int s = sceNetSocket("ps4cast_aseg", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) return s;
    int tmo = 2 * 1000 * 1000;   // 2s: caps how long an in-flight segment recv blocks before the teardown abort check breaks it
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
    ps4_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.len = sizeof(sa); sa.family = ORBIS_NET_AF_INET;
    sa.port = sceNetHtons(g_port); sa.addr = g_addr;

    // Non-blocking connect with a bounded budget. A blocking connect to a slow/dead
    // segment server can't be aborted (sceNetSocketAbort doesn't interrupt it), so it
    // wedged a channel switch's player_stop for ~13s. Here we start the connect
    // non-blocking, then poll a zero-length send (writable == connected) up to ~4s,
    // bailing immediately on teardown (g_abort), then restore blocking I/O.
    int nb = 1; sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_NBIO, &nb, sizeof(nb));
    g_sock = s;
    sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa));   // returns in-progress
    int connected = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    uint64_t connectBudgetUs = g_fetchBudgetUs == ASEG_BUDGET_SEGMENT_US
                             ? 6000ULL * 1000 : 2500ULL * 1000;
    if (g_fetchT0) {
        uint64_t elapsed = t0 - g_fetchT0;
        if (elapsed >= g_fetchBudgetUs) connectBudgetUs = 0;
        else if (connectBudgetUs > g_fetchBudgetUs - elapsed)
            connectBudgetUs = g_fetchBudgetUs - elapsed;
    }
    sceKernelUsleep(15000);                                        // let the handshake start before probing
    while (sceKernelGetProcessTime() - t0 < connectBudgetUs) {
        if (g_abort) break;
        if (sceNetSend(s, "", 0, 0) >= 0) { connected = 1; break; } // writable -> connected
        sceKernelUsleep(20000);
    }
    // Stay NON-BLOCKING for request/read. SO_RCVTIMEO is not reliably honored on
    // this stack -- a single recv was measured blocking ~36s with a 2s timeout set
    // ("HANG stale=36s at=eof/read"), which no budget check around the call can
    // bound. conn_read/conn_write below poll with their own deadline instead,
    // exactly like this connect does.
    if (!connected) { sceNetSocketClose(s); g_sock = -1; return -1; }
    return s;
}

// No-progress ceiling for a single read/write on a non-blocking aseg socket.
#define ASEG_STALL_US (4ULL * 1000 * 1000)

static int aseg_past_budget(void) {
    return g_fetchT0 && (sceKernelGetProcessTime() - g_fetchT0) > ASEG_FETCH_BUDGET_US;
}

static int conn_read(uint8_t *buf, int len) {
    if (g_tls) return tls_read(g_tls, buf, len);
    uint64_t t0 = sceKernelGetProcessTime();
    int spins = 0;
    for (;;) {
        if (g_abort) return -1;
        int n = sceNetRecv(g_sock, buf, len, 0);
        if (n > 0) return n;
        if (n == 0) return 0;                                   // peer closed: clean EOF
        if (sceKernelGetProcessTime() - t0 > ASEG_STALL_US) return -1;
        if (aseg_past_budget()) return -1;
        watchdog_kick();
        // Adaptive: a flat 3ms sleep per not-ready read throttled throughput badly
        // (TLS reassembles a record over many small reads). Spin first, then back off.
        if (spins < 64) { spins++; sceKernelUsleep(200); }
        else sceKernelUsleep(2000);
    }
}
static int conn_write(const uint8_t *buf, int len) {
    if (g_tls) return tls_write(g_tls, buf, len);
    int sent = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    while (sent < len) {
        if (g_abort) return -1;
        int n = sceNetSend(g_sock, buf + sent, len - sent, 0);
        if (n > 0) { sent += n; t0 = sceKernelGetProcessTime(); continue; }
        if (sceKernelGetProcessTime() - t0 > ASEG_STALL_US) return -1;
        if (aseg_past_budget()) return -1;
        watchdog_kick();
        sceKernelUsleep(3000);
    }
    return 0;
}

void aseg_abort(void) {
    g_abort = 1;
    // An aborted socket must never be reused. aseg keeps connections alive across
    // channel switches, and a burst fires many aborts, so the NEXT tune could pick
    // up a socket that had sceNetSocketAbort() called on it -- where SO_RCVTIMEO
    // is not reliably honored and recv can block indefinitely. That matches the
    // failure exactly: only after a burst, only on the first tune.
    g_kaAlive = 0;
    if (g_sock >= 0) sceNetSocketAbort(g_sock, 0);
    native_http_abort();
}

// Clear a STALE abort at the start of a new stream. player_stop() raises g_abort
// to unblock in-flight fetches; that flag then survived into the next channel's
// FIRST playlist fetch, which bailed immediately with rc=-9 ("hls fetch failed").
// The flag self-clears on that failed fetch, so the next attempt succeeded — which
// is why the failure looked random (measured 8/15 channel tunes failing, always
// rc=-9, on sources answering 200 in 0.3s from a PC).
//
// NOTE: this does NOT make the flag sticky. aseg_fetch keeps its own per-fetch
// clear; an earlier attempt to remove that in favour of this call wedged fetching
// entirely, because aseg_abort() is also raised from live-playback paths.
void aseg_resume(void) { g_abort = 0; }

void aseg_clear_error(void) {
    g_lastStatus = -1; g_lastLine[0] = '\0';
    g_badReuse = g_badHop = g_badPathLen = -1;
    g_badLine[0] = g_badPath[0] = '\0';
    g_badStage[0] = '\0';
}

static int preferred_native_url(const char *url) {
    if (!g_nativeHost[0] || !url) return 0;
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    size_t n = strlen(g_nativeHost);
    return strncasecmp(p, g_nativeHost, n) == 0 &&
           (p[n] == '/' || p[n] == ':' || p[n] == '\0');
}

// One request: connect, GET (Connection: close), parse headers. On 3xx returns
// 1 and copies Location into `loc`. On 2xx returns 0 and leaves the connection
// positioned at the first body byte, with header-adjacent body bytes in *lead.
static int do_request(int reuse, int *status, char *loc, int loccap,
                      uint8_t *lead, int *leadLen, long *clen, int *chunked) {
    if (!reuse) {
        conn_close();
        watchdog_note("aseg/connect");
        g_sock = tcp_connect();
        if (g_sock < 0) { snprintf(g_badStage, sizeof(g_badStage), "connect"); return -1; }
        if (g_tlsmode) {
            const char *pv_tls = watchdog_note("tls");   // handshake to a half-open host can block for seconds
            g_tls = tls_open_bounded(g_sock, g_host, g_fetchT0 + ASEG_FETCH_BUDGET_US);
            watchdog_note(pv_tls); watchdog_kick();
            if (!g_tls) { conn_close(); snprintf(g_badStage, sizeof(g_badStage), "tls"); return -2; }
        }
    }

    char req[1600];
    const char *xh = urlopt_headers();
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s"
        "%s"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        g_path, g_host, xh,
        strstr(xh, "User-Agent:") ? "" : "User-Agent: PS4Cast/1.0\r\n");
    if (n <= 0 || n >= (int)sizeof(req)) { conn_close(); snprintf(g_badStage, sizeof(g_badStage), "req"); return -3; }
    // Re-arm every request: a kept-alive TLS context outlives the fetch that
    // created it, so a deadline set at handshake time would already be in the
    // past on the next fetch and fail every read instantly.
    if (g_tls) tls_set_read_deadline(g_tls, g_fetchT0 + ASEG_FETCH_BUDGET_US);
    watchdog_note("aseg/req-send");
    if (conn_write((const uint8_t *)req, n) != 0) { conn_close(); snprintf(g_badStage, sizeof(g_badStage), "req"); return -3; }
    watchdog_note("aseg/hdr-read");

    char hdr[4096];
    int hl = 0, end = -1;
    while (hl < (int)sizeof(hdr) - 1) {
        if (g_abort) { conn_close(); snprintf(g_badStage, sizeof(g_badStage), "hdr"); return -9; }
        // Budget + watchdog, same as the body loops. Without them a server that
        // TRICKLES its response headers blocks the main thread indefinitely: each
        // conn_read can sit for the 2s RCVTIMEO and still return a byte, so r<=0
        // never fires, and this runs up to 4095 iterations -- twice per hop, five
        // hops. Nothing here petted the watchdog, which is the
        // "HANG stale=35s stage=source-open at=open/load-variant" fail-close.
        if (sceKernelGetProcessTime() - g_fetchT0 > ASEG_FETCH_BUDGET_US) {
            conn_close(); snprintf(g_badStage, sizeof(g_badStage), "hdr"); return -4;
        }
        watchdog_kick();
        int r = conn_read((uint8_t *)hdr + hl, (int)sizeof(hdr) - 1 - hl);
        if (r <= 0) break;
        hl += r; hdr[hl] = '\0';
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { end = (int)(e - hdr) + 4; break; }
    }
    if (end < 0) { conn_close(); snprintf(g_badStage, sizeof(g_badStage), "hdr"); return -4; }

    int st = 0;
    if (strncmp(hdr, "HTTP/", 5) == 0) { const char *sp = strchr(hdr, ' '); if (sp) st = atoi(sp + 1); }
    if (status) *status = st;
    { int k = 0; while (k < (int)sizeof(g_lastLine) - 1 && k < hl && hdr[k] != '\r' && hdr[k] != '\n') { char ch = hdr[k]; g_lastLine[k] = (ch >= 32 && ch < 127) ? ch : '.'; k++; } g_lastLine[k] = '\0'; }

    if (clen) {
        *clen = -1;
        const char *cl = ci_strstr(hdr, "Content-Length:");
        if (cl) *clen = atol(cl + (int)strlen("Content-Length:"));
    }
    if (chunked) {
        const char *te = ci_strstr(hdr, "Transfer-Encoding:");
        const char *eol = te ? strstr(te, "\r\n") : NULL;
        const char *coding = te ? ci_strstr(te, "chunked") : NULL;
        *chunked = coding && (!eol || coding < eol);
        if (*chunked && clen) *clen = -1;  // Transfer-Encoding wins over Content-Length.
    }

    if (st >= 300 && st < 400 && loc && loccap) {
        loc[0] = '\0';
        const char *l = ci_strstr(hdr, "Location:");
        if (l) {
            l += 9; while (*l == ' ' || *l == '\t') l++;
            const char *eol = strstr(l, "\r\n");
            int ln = eol ? (int)(eol - l) : (int)strlen(l);
            if (ln >= loccap) ln = loccap - 1;
            memcpy(loc, l, ln); loc[ln] = '\0';
        }
        return 1;  // redirect
    }

    int avail = hl - end;
    if (avail > 0) { memcpy(lead, hdr + end, avail); }
    *leadLen = avail > 0 ? avail : 0;
    return 0;
}

typedef struct {
    const uint8_t *lead;
    int leadLen;
    int leadPos;
} ChunkReader;

static int chunk_read_some(ChunkReader *r, uint8_t *out, int len) {
    if (r->leadPos < r->leadLen) {
        int n = r->leadLen - r->leadPos;
        if (n > len) n = len;
        memcpy(out, r->lead + r->leadPos, (size_t)n);
        r->leadPos += n;
        return n;
    }
    return conn_read(out, len);
}

static int chunk_read_exact(ChunkReader *r, uint8_t *out, size_t len) {
    size_t done = 0;
    while (done < len) {
        if (g_abort || sceKernelGetProcessTime() - g_fetchT0 > ASEG_FETCH_BUDGET_US) return -1;
        watchdog_kick();
        int want = (len - done) > INT32_MAX ? INT32_MAX : (int)(len - done);
        int got = chunk_read_some(r, out + done, want);
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 0;
}

static int chunk_read_line(ChunkReader *r, char *line, int cap) {
    int n = 0;
    for (;;) {
        uint8_t c;
        if (chunk_read_exact(r, &c, 1) != 0) return -1;
        if (c == '\n') {
            if (n > 0 && line[n - 1] == '\r') n--;
            line[n] = '\0';
            return n;
        }
        if (n >= cap - 1) return -1;
        line[n++] = (char)c;
    }
}

// Decode RFC 7230 chunk framing while reading, so a chunk-size line can never
// leak into the HLS parser as a fake segment URI. The terminating chunk and all
// trailers are consumed, leaving same-host connections safe for keep-alive.
static int read_chunked_body(const uint8_t *lead, int leadLen, uint8_t **outBuf, int *outLen) {
    ChunkReader reader = { lead, leadLen, 0 };
    size_t cap = 256 * 1024, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return -6;

    for (;;) {
        char line[128];
        int lineLen = chunk_read_line(&reader, line, sizeof(line));
        if (lineLen < 0) { free(buf); return -11; }

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        unsigned long long chunk = strtoull(p, &end, 16);
        if (end == p) { free(buf); return -13; }
        while (*end == ' ' || *end == '\t') end++;
        if (*end && *end != ';') { free(buf); return -13; }

        if (chunk == 0) {
            // Optional trailer fields end at the first blank line.
            do {
                lineLen = chunk_read_line(&reader, line, sizeof(line));
                if (lineLen < 0) { free(buf); return -11; }
            } while (lineLen != 0);
            break;
        }
        if (chunk > ASEG_FETCH_CAP || used > ASEG_FETCH_CAP - (size_t)chunk) {
            free(buf); return -10;
        }

        size_t need = used + (size_t)chunk;
        if (need > cap) {
            size_t next = cap;
            while (next < need && next < ASEG_FETCH_CAP) {
                size_t grown = next * 2;
                next = grown > ASEG_FETCH_CAP ? ASEG_FETCH_CAP : grown;
            }
            if (next < need) { free(buf); return -10; }
            uint8_t *larger = realloc(buf, next);
            if (!larger) { free(buf); return -7; }
            buf = larger; cap = next;
        }
        if (chunk_read_exact(&reader, buf + used, (size_t)chunk) != 0) {
            free(buf); return -11;
        }
        used += (size_t)chunk;

        uint8_t crlf[2];
        if (chunk_read_exact(&reader, crlf, sizeof(crlf)) != 0 || crlf[0] != '\r' || crlf[1] != '\n') {
            free(buf); return -13;
        }
    }

    if (used == 0) { free(buf); return -8; }
    *outBuf = buf;
    *outLen = (int)used;
    return 0;
}

// Hard ceiling on ONE fetch (all redirect hops + the body read). Without it a
// dead host could burn ~2.5s connect + 3s read per hop across 5 hops, and
// hls_open does TWO fetches (master + variant) — enough to block the main thread
// past the 35s channel-switch watchdog grace and fail-close the app. Observed as
// "HANG watchdog stale=36002ms" while zapping through unreachable channels.
// Default budget for a SEGMENT fetch. Segments are large and a slow-but-working
// CDN legitimately needs many seconds — an over-tight cap here makes every slow
// segment fail and the player rebuffer continuously (observed after this was set
// to a flat 9s for everything). Playlists are small, so hls_open lowers it around
// the master/variant fetches, where the real risk is blocking a channel switch.

void aseg_set_playlist_budget(int on) {
    g_fetchBudgetUs = on ? ASEG_BUDGET_PLAYLIST_US : ASEG_BUDGET_SEGMENT_US;
}

static int aseg_fetch_inner(const char *url, uint8_t **outBuf, int *outLen) {
    uint64_t budget0 = sceKernelGetProcessTime();
    g_fetchT0 = budget0;
    // Abort is STICKY until aseg_resume(). It must not self-clear here.
    //
    // Every fetch is serialized on g_fetchMtx. Clearing the flag on entry meant a
    // teardown's abort was consumed by whichever fetch ran next, so a thread
    // already QUEUED on the mutex (past its own stop-flag check, unable to see it)
    // then started a brand-new full-budget fetch. hls_close()'s unbounded
    // scePthreadJoin waited for it: ~25s segment budget on top of the fetch in
    // flight ~= the 36s that fail-closed the app on every
    // "HANG stale=36s stage=source-open" since v03.81.
    //
    // v03.65 removed this clear WITHOUT adding a resume point and wedged fetching
    // permanently. The resume calls in hls.c (after hls_close, and at
    // prefetch_start/apref_start) are what make stickiness safe.
    if (g_abort) { *outBuf = NULL; *outLen = 0; return -9; }
    *outBuf = NULL; *outLen = 0;

    // Some Cloudflare configurations reject BearSSL's TLS fingerprint even when
    // the URL and browser compatibility headers are valid. Once SceHttp proves
    // itself for a host, keep using the PS4's native TLS stack for that host.
    if (preferred_native_url(url)) {
        int status = 0;
        int rc = native_http_fetch(url, outBuf, outLen, &status, ASEG_FETCH_BUDGET_US);
        if (rc == 0 && (status == 200 || status == 206)) {
            aseg_clear_error();
            g_lastStatus = status;
            snprintf(g_lastLine, sizeof(g_lastLine), "native HTTP");
            return 0;
        }
        if (*outBuf) { free(*outBuf); *outBuf = NULL; *outLen = 0; }
        g_nativeHost[0] = '\0';
    }

    char cur[1400];
    strncpy(cur, url, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';

    uint8_t lead[4096]; int leadLen = 0; long clen = -1; int chunked = 0;
    int opened = 0;
    for (int hop = 0; hop < 5 && !opened; hop++) {
        if (g_abort) { conn_close(); g_kaAlive = 0; return -9; }   // bail between redirect hops on teardown
        if (sceKernelGetProcessTime() - budget0 > ASEG_FETCH_BUDGET_US) {
            conn_close(); g_kaAlive = 0; return -12;               // dead/slow host: fail fast
        }
        watchdog_kick();
        if (parse_url(cur) != 0) { conn_close(); g_kaAlive = 0; return -1; }
        if (resolve_host() != 0) { conn_close(); g_kaAlive = 0; snprintf(g_badStage, sizeof(g_badStage), "dns"); return -2; }
        // Reuse the kept-alive socket if it's to the same host:port:tls.
        // This was disabled in v04.12 to test a response-desync theory. That theory
        // was disproved (the wrong-playlist symptom turned out to be the main thread
        // stuck in a retry loop), and leaving it off costs a full TCP+TLS handshake
        // on EVERY segment -- measured open=1.6-3.4s against 0.3s from a PC.
        int reuse = (g_kaAlive && g_sock >= 0 && g_kaPort == g_port &&
                     g_kaTlsmode == g_tlsmode && strcmp(g_kaHost, g_host) == 0);
        int status = 0; char loc[1400];
        int rc = do_request(reuse, &status, loc, sizeof(loc), lead, &leadLen, &clen, &chunked);
        if (rc != 0 && reuse) {            // stale keep-alive socket -> fresh connect once
            conn_close(); g_kaAlive = 0;
            rc = do_request(0, &status, loc, sizeof(loc), lead, &leadLen, &clen, &chunked);
        }
        if (rc == 1 && loc[0]) { strncpy(cur, loc, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0'; g_kaAlive = 0; continue; }
        if (rc != 0) {
            conn_close(); g_kaAlive = 0;
            // A hard failure BEFORE any HTTP status — connect refused/timeout,
            // TLS handshake stall or rejection, request write stall — on an
            // HTTPS origin gets one bounded retry through the PS4's native
            // SceHttp stack. Cloudflare edges were observed hanging BearSSL
            // handshakes instead of answering 403, which left this fetch with
            // no fallback at all (rc=-3 len=0 st=-1). Same pinning rule as the
            // 403 path: success keeps using SceHttp for exactly this host.
            if (!g_abort && g_tlsmode) {
                int nstatus = 0;
                int nrc = native_http_fetch(cur, outBuf, outLen, &nstatus,
                                            ASEG_FETCH_BUDGET_US);
                if (nrc == 0 && (nstatus == 200 || nstatus == 206)) {
                    snprintf(g_nativeHost, sizeof(g_nativeHost), "%s", g_host);
                    aseg_clear_error();
                    g_lastStatus = nstatus;
                    snprintf(g_lastLine, sizeof(g_lastLine), "native HTTP");
                    return 0;
                }
                if (*outBuf) { free(*outBuf); *outBuf = NULL; *outLen = 0; }
            }
            return -3;
        }
        if (status == 403 && urlopt_has_page_headers()) {
            // Browser extensions can observe the outer page before the exact
            // manifest request headers arrive. Some CDNs reject that incorrect
            // context but intentionally accept no Referer/Origin. Retry clean
            // once, retaining UA/Cookie, and keep the winning policy for all
            // variant and segment requests in this stream.
            conn_close(); g_kaAlive = 0;
            urlopt_set_page_headers_enabled(0);
            leadLen = 0; clen = -1; chunked = 0;
            int clean_status = 0;
            int clean_rc = do_request(0, &clean_status, loc, sizeof(loc),
                                      lead, &leadLen, &clen, &chunked);
            if (clean_rc == 0 && (clean_status == 200 || clean_status == 206)) {
                status = clean_status;
                aseg_clear_error();
                g_lastStatus = status;
                snprintf(g_lastLine, sizeof(g_lastLine), "clean-header HTTP");
            } else {
                urlopt_set_page_headers_enabled(1);
                status = clean_status ? clean_status : status;
            }
        }
        if (status != 200 && status != 206) {
            g_lastStatus = status;           // capture HERE: a later OK fetch must not overwrite it
            g_badReuse = reuse; g_badHop = hop;
            snprintf(g_badLine, sizeof(g_badLine), "%s", g_lastLine);
            g_badPathLen = (int)strlen(g_path);
            snprintf(g_badPath, sizeof(g_badPath), "%s", g_path);
            conn_close(); g_kaAlive = 0;
            // Retry an HTTPS 403 once through the native PS4 HTTP/TLS stack. A
            // successful retry pins only this host to SceHttp; all other origins
            // stay on the established BearSSL path.
            if (g_tlsmode && status == 403) {
                int native_status = 0;
                int native_rc = native_http_fetch(cur, outBuf, outLen, &native_status,
                                                  ASEG_FETCH_BUDGET_US);
                if (native_rc == 0 && (native_status == 200 || native_status == 206)) {
                    snprintf(g_nativeHost, sizeof(g_nativeHost), "%s", g_host);
                    aseg_clear_error();
                    g_lastStatus = native_status;
                    snprintf(g_lastLine, sizeof(g_lastLine), "native HTTP");
                    return 0;
                }
                if (*outBuf) { free(*outBuf); *outBuf = NULL; *outLen = 0; }
            }
            return -4;
        }
        opened = 1;
    }
    if (!opened) { conn_close(); g_kaAlive = 0; return -5; }

    if (chunked) {
        watchdog_note("aseg/body-chunked");
        int rc = read_chunked_body(lead, leadLen, outBuf, outLen);
        if (rc != 0) { conn_close(); g_kaAlive = 0; return rc; }
        g_kaAlive = 1; g_kaPort = g_port; g_kaTlsmode = g_tlsmode;
        strncpy(g_kaHost, g_host, sizeof(g_kaHost) - 1); g_kaHost[sizeof(g_kaHost) - 1] = '\0';
        return 0;
    }

    watchdog_note("aseg/alloc");
    size_t cap = 256 * 1024, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { conn_close(); g_kaAlive = 0; return -6; }
    if (leadLen > 0) { memcpy(buf, lead, leadLen); used = leadLen; }

    if (clen >= 0) {
        // Known length: read exactly Content-Length bytes and KEEP the socket open
        // for the next same-host segment (skips the TLS handshake — the big win).
        watchdog_note("aseg/body-known");
        size_t need = (size_t)clen;
        if (need > ASEG_FETCH_CAP) { free(buf); conn_close(); g_kaAlive = 0; return -10; }
        while (used < need) {
            if (g_abort) { free(buf); conn_close(); g_kaAlive = 0; return -9; }
            // Budget + watchdog, exactly as the unknown-length loop below has.
            // Their absence here was the long-standing zap crash: a server that
            // sends Content-Length and then TRICKLES the body keeps conn_read
            // returning >0, so the 2s RCVTIMEO never fires, the loop never exits,
            // and NOTHING pets the watchdog -> an unbounded block inside
            // aseg_fetch_inner, past DNS and TLS ("HANG stale=35s
            // stage=source-open at=open/load-variant"). It showed up only on the
            // first tune AFTER a burst because aseg keeps sockets alive across
            // channel switches, so a burst leaves a degraded keep-alive
            // connection that then trickles.
            if (sceKernelGetProcessTime() - budget0 > ASEG_FETCH_BUDGET_US) {
                free(buf); conn_close(); g_kaAlive = 0; return -12;
            }
            watchdog_kick();
            if (used + 64 * 1024 > cap) {
                size_t ncap = cap * 2; if (ncap < need) ncap = need;
                if (ncap > ASEG_FETCH_CAP) ncap = ASEG_FETCH_CAP;
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) { free(buf); conn_close(); g_kaAlive = 0; return -7; }
                buf = nb; cap = ncap;
            }
            int want = (int)(need - used); if (want > (int)(cap - used)) want = (int)(cap - used);
            int r = conn_read(buf + used, want);
            if (r <= 0) { free(buf); conn_close(); g_kaAlive = 0; return -11; }  // short read -> fail+reconnect
            used += r;
        }
        g_kaAlive = 1; g_kaPort = g_port; g_kaTlsmode = g_tlsmode;   // reusable next time
        strncpy(g_kaHost, g_host, sizeof(g_kaHost) - 1); g_kaHost[sizeof(g_kaHost) - 1] = '\0';
    } else {
        watchdog_note("aseg/body-eof");
        // Unknown length (no Content-Length): read to EOF, then close (no reuse).
        for (;;) {
            if (g_abort) { free(buf); conn_close(); g_kaAlive = 0; return -9; }
            if (sceKernelGetProcessTime() - budget0 > ASEG_FETCH_BUDGET_US) {
                free(buf); conn_close(); g_kaAlive = 0; return -12;
            }
            watchdog_kick();
            if (used + 64 * 1024 > cap) {
                watchdog_note("eof/grow");
                size_t ncap = cap * 2;
                if (ncap > ASEG_FETCH_CAP) ncap = ASEG_FETCH_CAP;
                if (ncap <= cap) { free(buf); conn_close(); g_kaAlive = 0; return -10; }
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) { free(buf); conn_close(); g_kaAlive = 0; return -7; }
                buf = nb; cap = ncap;
            }
            watchdog_note("eof/read");
            int r = conn_read(buf + used, (int)(cap - used));
            if (r > 0) { used += r; continue; }
            break;
        }
        conn_close(); g_kaAlive = 0;
    }

    if (used == 0) { free(buf); return -8; }
    *outBuf = buf; *outLen = (int)used;
    return 0;
}

// Create the fetch lock ONCE, from main(), before any thread can fetch.
//
// This used to be a lazy "if (!inited) { init(); inited = 1; }" inside
// aseg_fetch. Three threads call aseg_fetch (main, ps4cast_hlsp, ps4cast_hlsap),
// and a channel-switch burst restarts both prefetch threads repeatedly, so two
// could enter that window together and BOTH run scePthreadMutexInit over the same
// storage. The second init replaces the handle the first thread is locking, so
// that thread's unlock releases a different object than it acquired and the lock
// stays held forever -- every later aseg_fetch then blocks indefinitely.
//
// That is the long-standing zap crash: "HANG stale=36s stage=source-open
// at=open/load-variant", always on the FIRST tune after a burst, never during it.
int aseg_last_status(void) { return g_lastStatus; }
int aseg_bad_reuse(void) { return g_badReuse; }
int aseg_bad_hop(void) { return g_badHop; }
const char *aseg_last_line(void) { return g_badLine[0] ? g_badLine : g_lastLine; }
const char *aseg_bad_path(void) { return g_badPath; }
int aseg_bad_pathlen(void) { return g_badPathLen; }
const char *aseg_bad_stage(void) { return g_badStage; }
const char *aseg_native_debug(void) { return native_http_debug(); }

void aseg_init(void) {
    if (g_fetchMtxInit) return;
    scePthreadMutexInit(&g_fetchMtx, NULL, "ps4cast_aseg_m");
    g_fetchMtxInit = 1;
}

int aseg_fetch(const char *url, uint8_t **outBuf, int *outLen) {
    if (!g_fetchMtxInit) return -1;     // aseg_init() not run: fail, never race an init
    const char *pv = watchdog_note("aseg/lock");
    scePthreadMutexLock(&g_fetchMtx);
    watchdog_note(pv);
    int rc = aseg_fetch_inner(url, outBuf, outLen);
    scePthreadMutexUnlock(&g_fetchMtx);
    return rc;
}
