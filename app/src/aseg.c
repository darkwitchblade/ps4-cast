#include "aseg.h"
#include "tls.h"

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
    int rc = sceNetResolverStartNtoa(rid, g_host, &a, 8 * 1000 * 1000, 3, 0);
    sceNetResolverDestroy(rid);
    if (rc < 0) return -3;
    g_addr = a.s_addr;
    strncpy(g_dnsHost, g_host, sizeof(g_dnsHost) - 1); g_dnsHost[sizeof(g_dnsHost) - 1] = '\0';
    g_dnsAddr = a.s_addr;                                                                // cache it
    return 0;
}

static int tcp_connect(void) {
    if (g_abort) return -1;                  // already tearing down: don't begin a fresh blocking connect
    int s = sceNetSocket("ps4cast_aseg", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) return s;
    int tmo = 3 * 1000 * 1000;   // 3s (was 8s): a stalled segment read fails fast so a channel switch's player_stop teardown isn't blocked waiting out a long socket timeout
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
    ps4_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.len = sizeof(sa); sa.family = ORBIS_NET_AF_INET;
    sa.port = sceNetHtons(g_port); sa.addr = g_addr;
    // Publish the socket BEFORE the blocking connect so aseg_abort()'s
    // sceNetSocketAbort(g_sock) can interrupt a slow connect during teardown
    // (otherwise the connecting socket is invisible to the aborter and a dead
    // segment server wedges player_stop for ~13s).
    g_sock = s;
    if (sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa)) < 0) {
        sceNetSocketClose(s); g_sock = -1;
        return -1;
    }
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

static int aseg_fetch_inner(const char *url, uint8_t **outBuf, int *outLen) {
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
