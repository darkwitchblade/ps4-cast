#include "aseg.h"
#include "tls.h"

// main.c: pet the freeze watchdog during a legitimately-progressing blocking op.
// Safe here because every fetch is bounded by ASEG_FETCH_BUDGET_US, so this can
// never mask a true freeze — it only stops a SLOW-but-bounded playlist/segment
// fetch from being mistaken for one (the "HANG watchdog stale=35s" fail-close
// seen while zapping past several unreachable channels in a row).
extern void watchdog_kick(void);

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
    int rc = sceNetResolverStartNtoa(rid, g_host, &a, 4 * 1000 * 1000, 2, 0);
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
    sceKernelUsleep(15000);                                        // let the handshake start before probing
    while (sceKernelGetProcessTime() - t0 < 2500ULL * 1000) {       // ~2.5s connect budget
        if (g_abort) break;
        if (sceNetSend(s, "", 0, 0) >= 0) { connected = 1; break; } // writable -> connected
        sceKernelUsleep(20000);
    }
    nb = 0; sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_NBIO, &nb, sizeof(nb)); // restore blocking for request/read
    if (!connected) { sceNetSocketClose(s); g_sock = -1; return -1; }
    return s;
}

static int conn_read(uint8_t *buf, int len) {
    if (g_tls) return tls_read(g_tls, buf, len);
    int n = sceNetRecv(g_sock, buf, len, 0);
    if (n < 0) return -1;
    return n;
}
static int conn_write(const uint8_t *buf, int len) {
    if (g_tls) return tls_write(g_tls, buf, len);
    int sent = 0;
    while (sent < len) {
        int n = sceNetSend(g_sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

void aseg_abort(void) {
    g_abort = 1;
    if (g_sock >= 0) sceNetSocketAbort(g_sock, 0);
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

// One request: connect, GET (Connection: close), parse headers. On 3xx returns
// 1 and copies Location into `loc`. On 2xx returns 0 and leaves the connection
// positioned at the first body byte, with header-adjacent body bytes in *lead.
static int do_request(int reuse, int *status, char *loc, int loccap,
                      uint8_t *lead, int *leadLen, long *clen) {
    if (!reuse) {
        conn_close();
        g_sock = tcp_connect();
        if (g_sock < 0) return -1;
        if (g_tlsmode) {
            g_tls = tls_open(g_sock, g_host);
            if (!g_tls) { conn_close(); return -2; }
        }
    }

    char req[1600];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PS4Cast/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        g_path, g_host);
    if (conn_write((const uint8_t *)req, n) != 0) { conn_close(); return -3; }

    char hdr[4096];
    int hl = 0, end = -1;
    while (hl < (int)sizeof(hdr) - 1) {
        if (g_abort) { conn_close(); return -9; }
        int r = conn_read((uint8_t *)hdr + hl, (int)sizeof(hdr) - 1 - hl);
        if (r <= 0) break;
        hl += r; hdr[hl] = '\0';
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { end = (int)(e - hdr) + 4; break; }
    }
    if (end < 0) { conn_close(); return -4; }

    int st = 0;
    if (strncmp(hdr, "HTTP/", 5) == 0) { const char *sp = strchr(hdr, ' '); if (sp) st = atoi(sp + 1); }
    if (status) *status = st;

    if (clen) {
        *clen = -1;
        const char *cl = ci_strstr(hdr, "Content-Length:");
        if (cl) *clen = atol(cl + (int)strlen("Content-Length:"));
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
#define ASEG_BUDGET_SEGMENT_US (25ULL * 1000 * 1000)
#define ASEG_BUDGET_PLAYLIST_US (6ULL * 1000 * 1000)
static volatile uint64_t g_fetchBudgetUs = ASEG_BUDGET_SEGMENT_US;
#define ASEG_FETCH_BUDGET_US (g_fetchBudgetUs)

void aseg_set_playlist_budget(int on) {
    g_fetchBudgetUs = on ? ASEG_BUDGET_PLAYLIST_US : ASEG_BUDGET_SEGMENT_US;
}

static int aseg_fetch_inner(const char *url, uint8_t **outBuf, int *outLen) {
    uint64_t budget0 = sceKernelGetProcessTime();
    // If an abort was issued (Stop/teardown) in the race window just before this
    // fetch started, honor it and bail — do NOT clear it and begin a blocking
    // resolve/connect the abort can no longer interrupt (no socket open yet).
    // This was the lost-abort path that let a fetch thread wedge at teardown.
    if (g_abort) { *outBuf = NULL; *outLen = 0; g_abort = 0; return -9; }
    g_abort = 0;
    *outBuf = NULL; *outLen = 0;

    char cur[1400];
    strncpy(cur, url, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';

    uint8_t lead[4096]; int leadLen = 0; long clen = -1;
    int opened = 0;
    for (int hop = 0; hop < 5 && !opened; hop++) {
        if (g_abort) { conn_close(); g_kaAlive = 0; return -9; }   // bail between redirect hops on teardown
        if (sceKernelGetProcessTime() - budget0 > ASEG_FETCH_BUDGET_US) {
            conn_close(); g_kaAlive = 0; return -12;               // dead/slow host: fail fast
        }
        watchdog_kick();
        if (parse_url(cur) != 0) { conn_close(); g_kaAlive = 0; return -1; }
        if (resolve_host() != 0) { conn_close(); g_kaAlive = 0; return -2; }
        // Reuse the kept-alive socket if it's to the same host:port:tls.
        int reuse = (g_kaAlive && g_sock >= 0 && g_kaPort == g_port &&
                     g_kaTlsmode == g_tlsmode && strcmp(g_kaHost, g_host) == 0);
        int status = 0; char loc[1400];
        int rc = do_request(reuse, &status, loc, sizeof(loc), lead, &leadLen, &clen);
        if (rc != 0 && reuse) {            // stale keep-alive socket -> fresh connect once
            conn_close(); g_kaAlive = 0;
            rc = do_request(0, &status, loc, sizeof(loc), lead, &leadLen, &clen);
        }
        if (rc == 1 && loc[0]) { strncpy(cur, loc, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0'; g_kaAlive = 0; continue; }
        if (rc != 0) { conn_close(); g_kaAlive = 0; return -3; }
        if (status != 200 && status != 206) { conn_close(); g_kaAlive = 0; return -4; }
        opened = 1;
    }
    if (!opened) { conn_close(); g_kaAlive = 0; return -5; }

    size_t cap = 256 * 1024, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { conn_close(); g_kaAlive = 0; return -6; }
    if (leadLen > 0) { memcpy(buf, lead, leadLen); used = leadLen; }

    if (clen >= 0) {
        // Known length: read exactly Content-Length bytes and KEEP the socket open
        // for the next same-host segment (skips the TLS handshake — the big win).
        size_t need = (size_t)clen;
        if (need > ASEG_FETCH_CAP) { free(buf); conn_close(); g_kaAlive = 0; return -10; }
        while (used < need) {
            if (g_abort) { free(buf); conn_close(); g_kaAlive = 0; return -9; }
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
        // Unknown length (no Content-Length): read to EOF, then close (no reuse).
        for (;;) {
            if (g_abort) { free(buf); conn_close(); g_kaAlive = 0; return -9; }
            if (sceKernelGetProcessTime() - budget0 > ASEG_FETCH_BUDGET_US) {
                free(buf); conn_close(); g_kaAlive = 0; return -12;
            }
            watchdog_kick();
            if (used + 64 * 1024 > cap) {
                size_t ncap = cap * 2;
                if (ncap > ASEG_FETCH_CAP) ncap = ASEG_FETCH_CAP;
                if (ncap <= cap) { free(buf); conn_close(); g_kaAlive = 0; return -10; }
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) { free(buf); conn_close(); g_kaAlive = 0; return -7; }
                buf = nb; cap = ncap;
            }
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

int aseg_fetch(const char *url, uint8_t **outBuf, int *outLen) {
    if (!g_fetchMtxInit) {
        scePthreadMutexInit(&g_fetchMtx, NULL, "ps4cast_aseg_m");
        g_fetchMtxInit = 1;
    }
    scePthreadMutexLock(&g_fetchMtx);
    int rc = aseg_fetch_inner(url, outBuf, outLen);
    scePthreadMutexUnlock(&g_fetchMtx);
    return rc;
}
