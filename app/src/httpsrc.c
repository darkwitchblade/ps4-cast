#include "httpsrc.h"
#include "tls.h"

extern void watchdog_kick(void);
extern const char *watchdog_note(const char *w);   // see aseg.c: bounded slow I/O must not look like a freeze

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

// Streaming HTTP(S) reader for ffmpeg's custom AVIO.
//
// One persistent connection per playback (or per seek): we issue a single
// ranged GET from the requested offset and then stream the body sequentially.
// ffmpeg reads mostly sequentially, so this is one TLS handshake per playback
// instead of one per read. A seek to a non-sequential offset transparently
// reconnects with a new Range. Plain http and https share this path; https adds
// a TLS layer (tls.c / BearSSL) over the same socket.

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
#define ORBIS_NET_SO_NBIO      0x1200

static char     g_host[256];
static char     g_path[1024];
static uint16_t g_port = 80;
static uint32_t g_addr = 0;
static int      g_tlsmode = 0;     // 1 = https

static int      g_sock = -1;
static tls_ctx *g_tls = NULL;
static int      g_pool = -1;

static int      g_ready = 0;
static uint64_t g_total = 0;
static char     g_dbg[200] = "idle";

// leftover body bytes captured while reading the response header block
static uint8_t  g_lead[4096];
static int      g_leadOff = 0, g_leadLen = 0;

// ---- read-ahead buffer thread ---------------------------------------------
// A background thread drains the network at full speed into a ring buffer; the
// consumer (ffmpeg's AVIO, on the decode thread) serves from the ring. This
// decouples network reads from decode/pacing, so a CDN stall or slow decode
// never blocks the decode loop (no freeze / no UI wedge) and playback rides
// through jitter. Seeks flush the ring and reposition the network connection.
#define RING_CAP   (48 * 1024 * 1024)
#define RING_CHUNK (256 * 1024)
// History kept behind the play position. Sized so a back-seek (L1 = -60s) is
// served from cache instead of forcing a reconnect + rebuffer: 12MB was only
// ~36s at a typical ~330KB/s stream, so every -60s seek missed. 24MB covers
// ~72s there, and still leaves ~24MB (~72s) of forward read-ahead in RING_CAP.
#define RING_KEEP_BEHIND (24 * 1024 * 1024)

static uint8_t           *g_ring;
static size_t             g_ringHead, g_ringFill;
static uint64_t           g_cacheStart; // abs offset of ring front
static uint64_t           g_servePos;   // next byte requested/served by consumer
static uint64_t           g_rawPos;     // abs offset reader has fetched to
static OrbisPthread       g_rdThread;
static OrbisPthreadMutex  g_mtx;
static OrbisPthreadCond   g_condData, g_condSpace;
static int                g_threadUp = 0;
static volatile int       g_stop = 0, g_eof = 0;
static volatile int       g_seekReq = 0;
static volatile uint64_t  g_seekTo = 0;
static volatile int       g_abort = 0;   // interrupt a blocked read (stop/cast)
static long               g_waits = 0, g_stalls = 0, g_reconnects = 0, g_seeks = 0;
static int                g_lastWaitMs = 0;

// HTTP keep-alive: reuse one TLS/socket connection across back-to-back same-host
// requests (e.g. HLS segments) so each segment boundary skips a fresh handshake.
static int       g_keepSock = -1;
static tls_ctx  *g_keepTls = NULL;
static char      g_keepHost[256] = "";
static uint16_t  g_keepPort = 0;
static int       g_keepTlsMode = 0;
static volatile int g_cleanEof = 0;     // last response ended exactly at its length
static long      g_kalive = 0;          // count of reused connections (telemetry)

const char *httpsrc_debug(void) {
    static char b[320];
    size_t fill = 0;
    uint64_t start = g_cacheStart, serve = g_servePos, raw = g_rawPos;
    int eof = g_eof, aborting = g_abort;
    if (g_threadUp) {
        scePthreadMutexLock(&g_mtx);
        fill = g_ringFill;
        start = g_cacheStart;
        serve = g_servePos;
        raw = g_rawPos;
        eof = g_eof;
        aborting = g_abort;
        scePthreadMutexUnlock(&g_mtx);
    }
    snprintf(b, sizeof(b),
             "%s buf=%u%% fill=%uKB cache=%llu-%llu serve=%llu wait=%ld stall=%ld last=%dms rec=%ld seek=%ld eof=%d abort=%d",
             g_dbg, (unsigned)(fill * 100 / RING_CAP), (unsigned)(fill / 1024),
             (unsigned long long)start, (unsigned long long)raw, (unsigned long long)serve,
             g_waits, g_stalls, g_lastWaitMs, g_reconnects, g_seeks, eof, aborting);
    return b;
}
uint64_t    httpsrc_size(void)  { return g_total; }
uint64_t    httpsrc_rx_total(void) { return g_rawPos; }   // bytes fetched from network

// Forward read-ahead in bytes (buffered ahead of the consumer).
uint64_t httpsrc_ahead_bytes(void) {
    if (!g_threadUp) return 0;
    return (g_rawPos > g_servePos) ? (g_rawPos - g_servePos) : 0;  // unlocked, advisory
}

// 1 if the current host resolved to a private/LAN address (smaller buffer is
// fine); 0 for public/CDN hosts (want a bigger cushion).
int httpsrc_is_lan(void) {
    uint32_t a = g_addr;                 // network byte order: bytes [o1,o2,..]
    unsigned o1 = a & 0xFF, o2 = (a >> 8) & 0xFF;
    if (o1 == 10 || o1 == 127) return 1;
    if (o1 == 192 && o2 == 168) return 1;
    if (o1 == 172 && o2 >= 16 && o2 <= 31) return 1;
    return 0;
}

// Forward read-ahead as a percent of the ring, for the on-screen buffer gauge.
int httpsrc_fill_pct(void) {
    uint64_t ahead = httpsrc_ahead_bytes();
    if (ahead > RING_CAP) ahead = RING_CAP;
    return (int)(ahead * 100 / RING_CAP);
}

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
    if (g_pool < 0) g_pool = sceNetPoolCreate("ps4cast_pool", 16 * 1024, 0);
    return g_pool;
}

static void conn_close(void) {
    if (g_tls) { tls_close(g_tls); g_tls = NULL; }
    if (g_sock >= 0) { sceNetSocketClose(g_sock); g_sock = -1; }
    g_leadOff = g_leadLen = 0;
}

void httpsrc_close(void) {
    if (g_threadUp) {
        scePthreadMutexLock(&g_mtx);
        g_stop = 1;
        scePthreadCondSignal(&g_condData);
        scePthreadCondSignal(&g_condSpace);
        scePthreadMutexUnlock(&g_mtx);
        // Only abort the socket if the reader might be stuck; aborting a cleanly
        // finished response would kill a connection we could keep alive.
        if (g_sock >= 0 && !g_cleanEof) sceNetSocketAbort(g_sock, 0);
        scePthreadJoin(g_rdThread, NULL);
        scePthreadCondDestroy(&g_condData);
        scePthreadCondDestroy(&g_condSpace);
        scePthreadMutexDestroy(&g_mtx);
        g_threadUp = 0;
    }
    // Keep-alive: if the response ended exactly at its length, stash the idle
    // socket so the next same-host request can reuse it (no fresh handshake).
    if (g_cleanEof && g_sock >= 0 && !g_abort) {
        if (g_keepSock >= 0) { if (g_keepTls) tls_close(g_keepTls); sceNetSocketClose(g_keepSock); }
        g_keepSock = g_sock; g_keepTls = g_tls;
        g_keepTlsMode = g_tlsmode; g_keepPort = g_port;
        strncpy(g_keepHost, g_host, sizeof(g_keepHost) - 1); g_keepHost[sizeof(g_keepHost)-1] = '\0';
        g_sock = -1; g_tls = NULL; g_leadOff = g_leadLen = 0;
    }
    conn_close();
    if (g_ring) { free(g_ring); g_ring = NULL; }
    g_ready = 0;
    g_total = 0;
    g_cacheStart = g_servePos = g_rawPos = 0;
    g_ringHead = g_ringFill = 0;
    g_stop = g_eof = g_seekReq = g_abort = 0;
    g_cleanEof = 0;   // consumed by the stash decision above; clear for next time
    g_waits = g_stalls = g_reconnects = g_seeks = 0;
    g_lastWaitMs = 0;
}

// Interrupt a blocked read immediately (called when the user hits Stop / casts a
// new URL) so a buffer-underrun stall can never trap the app. The next read
// returns EOF, the player unwinds, and the pending control request runs.
void httpsrc_abort(void) {
    g_abort = 1;
    if (g_sock >= 0) sceNetSocketAbort(g_sock, 0);  // unblock a stuck recv
    if (g_threadUp) {
        scePthreadMutexLock(&g_mtx);
        scePthreadCondSignal(&g_condData);
        scePthreadCondSignal(&g_condSpace);
        scePthreadMutexUnlock(&g_mtx);
    }
}

static char     g_dnsHost[256] = "";   // 1-entry DNS cache so back-to-back HLS
static uint32_t g_dnsAddr = 0;         // segments to the same host skip the lookup

static int resolve_host(void) {
    OrbisNetInAddr a;
    memset(&a, 0, sizeof(a));
    if (sceNetInetPton(ORBIS_NET_AF_INET, g_host, &a.s_addr) > 0) { g_addr = a.s_addr; return 0; }
    if (g_dnsAddr && strcmp(g_dnsHost, g_host) == 0) { g_addr = g_dnsAddr; return 0; }
    int pool = net_pool();
    if (pool < 0) return -1;
    OrbisNetId rid = sceNetResolverCreate("ps4cast_res", pool, 0);
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
    strncpy(g_dnsHost, g_host, sizeof(g_dnsHost) - 1); g_dnsHost[sizeof(g_dnsHost)-1] = '\0';
    g_dnsAddr = a.s_addr;
    return 0;
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

static int tcp_connect(void) {
    int s = sceNetSocket("ps4cast_http", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) return s;
    // Shorter timeout so a stalled read fails fast and we reconnect-resume
    // instead of the picture freezing for a long time.
    int tmo = 5 * 1000 * 1000;
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
    ps4_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.len = sizeof(sa); sa.family = ORBIS_NET_AF_INET;
    sa.port = sceNetHtons(g_port); sa.addr = g_addr;
    // Non-blocking connect with a bounded budget — a plain blocking sceNetConnect
    // to a dead host can be held by the OS for ~30s and is NOT interruptible, so
    // opening several unreachable channels in a row blocked the main thread past
    // the 35s channel-switch watchdog grace and fail-closed the app
    // ("HANG watchdog stale=35-36s" while zapping past dead entries). Same shape
    // as aseg's connect: start it non-blocking, poll a zero-length send until
    // writable, pet the watchdog while waiting, then restore blocking I/O.
    int nb = 1; sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_NBIO, &nb, sizeof(nb));
    sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa));   // returns in-progress
    int connected = 0;
    uint64_t ct0 = sceKernelGetProcessTime();
    sceKernelUsleep(15000);
    while (sceKernelGetProcessTime() - ct0 < 2500ULL * 1000) {     // ~2.5s connect budget
        if (sceNetSend(s, "", 0, 0) >= 0) { connected = 1; break; }
        watchdog_kick();
        sceKernelUsleep(20000);
    }
    nb = 0; sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_NBIO, &nb, sizeof(nb));
    if (!connected) { sceNetSocketClose(s); return -1; }
    return s;
}

// Read raw bytes from the active connection (TLS or plain). >0/0/<0.
static int conn_read(uint8_t *buf, int len) {
    if (g_tls) return tls_read(g_tls, buf, len);
    int n = sceNetRecv(g_sock, buf, len, 0);
    if (n < 0) return -1;
    return n; // 0 = EOF
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

// Open a connection and issue a ranged GET from `pos`. Parses the response
// headers; on the initial open also captures total size + follows redirects.
// `want_meta` non-zero => fill *status/*total/loc (used by open). Leaves the
// connection positioned at the first body byte (extra bytes buffered in g_lead).
static int request_from_ex(uint64_t pos, int *status, int64_t *total, char *loc, int loccap, int reuse) {
    if (!reuse) {
        conn_close();
        g_sock = tcp_connect();
        if (g_sock < 0) { snprintf(g_dbg, sizeof(g_dbg), "connect failed"); return -1; }
        if (g_tlsmode) {
            const char *pv_tls = watchdog_note("tls");   // handshake to a half-open host can block for seconds
            g_tls = tls_open(g_sock, g_host);
            watchdog_note(pv_tls); watchdog_kick();
            if (!g_tls) { conn_close(); snprintf(g_dbg, sizeof(g_dbg), "tls handshake failed (%s)", g_host); return -2; }
        }
    } else {
        g_leadOff = g_leadLen = 0;   // reusing an idle keep-alive socket
    }
    g_cleanEof = 0;

    char req[1800];
    // Do not send Range on the initial byte-0 open. Some HTTPS CDNs accept the
    // 206 header then deliver zero body bytes on a first ranged request; a plain
    // GET is the most compatible start. Real seeks still send Range below.
    int n;
    if (pos == 0) {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: PS4Cast/1.0\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            g_path, g_host);
    } else {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: PS4Cast/1.0\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Range: bytes=%llu-\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            g_path, g_host, (unsigned long long)pos);
    }
    if (conn_write((const uint8_t *)req, n) != 0) {
        int te = g_tls ? tls_last_error(g_tls) : 0;
        conn_close();
        snprintf(g_dbg, sizeof(g_dbg), g_tlsmode ? "tls/send failed e=%d" : "send failed", te);
        return -3;
    }

    // Read response headers into a buffer; keep any trailing body bytes.
    char hdr[4096];
    int hl = 0, end = -1;
    while (hl < (int)sizeof(hdr) - 1) {
        int r = conn_read((uint8_t *)hdr + hl, (int)sizeof(hdr) - 1 - hl);
        if (r <= 0) break;
        hl += r; hdr[hl] = '\0';
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { end = (int)(e - hdr) + 4; break; }
    }
    if (end < 0) { conn_close(); snprintf(g_dbg, sizeof(g_dbg), "no http header"); return -4; }

    int st = 0;
    if (strncmp(hdr, "HTTP/", 5) == 0) { const char *sp = strchr(hdr, ' '); if (sp) st = atoi(sp + 1); }
    if (status) *status = st;

    if (loc && loccap) {
        loc[0] = '\0';
        const char *l = ci_strstr(hdr, "Location:");
        if (l) {
            l += 9; while (*l == ' ' || *l == '\t') l++;
            const char *eol = strstr(l, "\r\n");
            int ln = eol ? (int)(eol - l) : (int)strlen(l);
            if (ln >= loccap) ln = loccap - 1;
            memcpy(loc, l, ln); loc[ln] = '\0';
        }
    }
    if (total) {
        *total = -1;
        const char *cr = ci_strstr(hdr, "Content-Range:");
        if (cr) { const char *sl = strchr(cr, '/'); const char *eol = strstr(cr, "\r\n");
            if (sl && (!eol || sl < eol)) *total = (int64_t)strtoull(sl + 1, NULL, 10); }
        if (*total < 0) { const char *cl = ci_strstr(hdr, "Content-Length:");
            if (cl) *total = (int64_t)strtoull(cl + (int)strlen("Content-Length:"), NULL, 10); }
    }
    if (ci_strstr(hdr, "Transfer-Encoding:") && ci_strstr(hdr, "chunked")) {
        snprintf(g_dbg, sizeof(g_dbg), "chunked unsupported (%s)", g_host);
        conn_close();
        return -5;
    }

    // Stash trailing body bytes that arrived with the header block.
    int avail = hl - end;
    if (avail > 0) { memcpy(g_lead, hdr + end, avail); g_leadLen = avail; }
    g_leadOff = 0;

    // If the server ignored our Range (200 instead of 206), the body starts at
    // byte 0, so discard down to the requested offset. This is the http
    // frames=0 regression fix: without it, every seeked read returned byte-0
    // data and the decoder got garbage.
    if (st == 200 && pos > 0) {
        uint64_t discard = pos;
        while (discard > 0) {
            if (g_leadOff < g_leadLen) { g_leadOff++; discard--; continue; }
            uint8_t tmp[2048];
            int want = discard > sizeof(tmp) ? (int)sizeof(tmp) : (int)discard;
            int r = conn_read(tmp, want);
            if (r <= 0) break;
            discard -= (uint32_t)r;
        }
    }
    // A failed reconnect used to remain in g_dbg forever even after the reader
    // recovered and playback continued, making /status report a dead network
    // path while bytes and frames were advancing normally.
    if (!status && !total && !loc)
        snprintf(g_dbg, sizeof(g_dbg), "%s recovered %s:%u at=%llu st=%d",
                 g_tlsmode ? "https" : "http", g_host, g_port,
                 (unsigned long long)pos, st);
    return 0;
}

static int request_from(uint64_t pos, int *status, int64_t *total, char *loc, int loccap) {
    return request_from_ex(pos, status, total, loc, loccap, 0);
}

// Sequential read from the current connection (reader thread only): serve
// header-adjacent bytes, then conn_read, reconnecting from g_rawPos on a stall.
static int raw_seq_read(uint8_t *buf, int len) {
    int got = 0, reconnects = 0;
    while (got < len) {
        if (g_stop || g_abort) break;   // bail fast on stop/cast
        if (g_total && g_rawPos + (uint64_t)got >= g_total) break;
        if (g_leadOff < g_leadLen) {
            int avail = g_leadLen - g_leadOff, take = len - got;
            if (g_total && g_rawPos + (uint64_t)got + (uint64_t)take > g_total)
                take = (int)(g_total - (g_rawPos + (uint64_t)got));
            if (take > avail) take = avail;
            memcpy(buf + got, g_lead + g_leadOff, take);
            g_leadOff += take; got += take;
            continue;
        }
        int want = len - got;
        if (g_total && g_rawPos + (uint64_t)got + (uint64_t)want > g_total)
            want = (int)(g_total - (g_rawPos + (uint64_t)got));
        if (want <= 0) break;
        int r = conn_read(buf + got, want);
        if (r > 0) { got += r; reconnects = 0; continue; }   // progress resets backoff
        if (g_stop || g_abort) break;
        uint64_t curpos = g_rawPos + got;
        if ((g_total && curpos >= g_total) || reconnects >= 10) break;
        // Exponential backoff so a flaky CDN recovers instead of being hammered:
        // 100ms, 200, 400, 800 ... capped at ~3s. Resume at the exact byte range.
        unsigned backoff = 100000u << (reconnects < 5 ? reconnects : 5);
        if (backoff > 3000000u) backoff = 3000000u;
        sceKernelUsleep(backoff);
        if (g_stop || g_abort) break;
        reconnects++;
        g_reconnects++;
        if (request_from(curpos, NULL, NULL, NULL, 0) != 0) break;
    }
    return got;
}

static void ring_append(const uint8_t *src, int n) {
    size_t tail = (g_ringHead + g_ringFill) % RING_CAP;
    int first = n;
    if (tail + first > RING_CAP) first = (int)(RING_CAP - tail);
    memcpy(g_ring + tail, src, first);
    if (n > first) memcpy(g_ring, src + first, n - first);
    g_ringFill += n;
}

static void ring_evict_locked(size_t need) {
    if (!g_ringFill) return;
    uint64_t keepFrom = (g_servePos > RING_KEEP_BEHIND) ? (g_servePos - RING_KEEP_BEHIND) : 0;
    if (keepFrom > g_cacheStart) {
        uint64_t canDrop64 = keepFrom - g_cacheStart;
        if (canDrop64 > g_ringFill) canDrop64 = g_ringFill;
        size_t drop = (size_t)canDrop64;
        if (drop > 0) {
            g_ringHead = (g_ringHead + drop) % RING_CAP;
            g_ringFill -= drop;
            g_cacheStart += drop;
        }
    }
    if (g_ringFill + need > RING_CAP && g_cacheStart < g_servePos) {
        size_t over = g_ringFill + need - RING_CAP;
        uint64_t canDrop64 = g_servePos - g_cacheStart;
        if (canDrop64 > g_ringFill) canDrop64 = g_ringFill;
        size_t canDrop = (size_t)canDrop64;
        if (over > canDrop) over = canDrop;
        if (over > 0) {
            g_ringHead = (g_ringHead + over) % RING_CAP;
            g_ringFill -= over;
            g_cacheStart += over;
        }
    }
}

static void *reader_main(void *arg) {
    (void)arg;
    static uint8_t tmp[RING_CHUNK];
    for (;;) {
        scePthreadMutexLock(&g_mtx);
        ring_evict_locked(RING_CHUNK);
        while (!g_stop && !g_seekReq && (g_ringFill > RING_CAP - RING_CHUNK || g_eof)) {
            scePthreadCondWait(&g_condSpace, &g_mtx);
            ring_evict_locked(RING_CHUNK);
        }
        if (g_stop) { scePthreadMutexUnlock(&g_mtx); break; }
        if (g_seekReq) {
            uint64_t sp = g_seekTo; g_seekReq = 0;
            g_ringHead = 0; g_ringFill = 0; g_cacheStart = sp; g_servePos = sp; g_rawPos = sp; g_eof = 0;
            scePthreadMutexUnlock(&g_mtx);
            request_from(sp, NULL, NULL, NULL, 0);     // network, outside the lock
            scePthreadMutexLock(&g_mtx);
            scePthreadCondSignal(&g_condData);          // wake consumer (repositioned)
            scePthreadMutexUnlock(&g_mtx);
            continue;
        }
        scePthreadMutexUnlock(&g_mtx);

        int n = raw_seq_read(tmp, RING_CHUNK);          // network, outside the lock

        scePthreadMutexLock(&g_mtx);
        if (g_seekReq) { scePthreadMutexUnlock(&g_mtx); continue; } // stale read; seek pending
        if (n <= 0) {
            g_eof = 1;
            g_cleanEof = (g_total && g_rawPos >= g_total) ? 1 : 0;  // socket at a clean boundary
            scePthreadCondSignal(&g_condData); scePthreadMutexUnlock(&g_mtx); continue;
        }
        ring_evict_locked((size_t)n);
        if (g_ringFill + (size_t)n <= RING_CAP) ring_append(tmp, n);
        g_rawPos += n;
        if (g_total && g_rawPos >= g_total) {
            g_eof = 1;
            g_cleanEof = 1;
        }
        scePthreadCondSignal(&g_condData);
        scePthreadMutexUnlock(&g_mtx);
    }
    return NULL;
}

int httpsrc_open(const char *url) {
    httpsrc_close();

    char cur[1400];
    strncpy(cur, url, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';

    int opened = 0;
    for (int hop = 0; hop < 6 && !opened; hop++) {
        // Pet the watchdog EVERY hop. This loop had no kick at all while calling a
        // blocking ~8s DNS resolve per hop; six hops was ~48s of silence, past the
        // 35s channel-switch grace -> "HANG stale=35-36s stage=source-open" while
        // zapping. aseg's equivalent loop already kicks per hop; this one did not.
        watchdog_kick();
        if (parse_url(cur) != 0) { snprintf(g_dbg, sizeof(g_dbg), "bad url"); return -1; }
        if (resolve_host() != 0) { snprintf(g_dbg, sizeof(g_dbg), "resolve failed (%s)", g_host); return -2; }

        // Keep-alive: reuse a stashed same-host connection (skip the handshake);
        // close it if it's for a different host.
        int reuse = 0;
        if (g_keepSock >= 0) {
            if (g_keepTlsMode == g_tlsmode && g_keepPort == g_port && strcmp(g_keepHost, g_host) == 0) {
                g_sock = g_keepSock; g_tls = g_keepTls; g_keepSock = -1; g_keepTls = NULL; reuse = 1; g_kalive++;
            } else {
                if (g_keepTls) tls_close(g_keepTls); sceNetSocketClose(g_keepSock); g_keepSock = -1; g_keepTls = NULL;
            }
        }

        int status = 0; int64_t total = -1; char loc[1400];
        int rrc = request_from_ex(0, &status, &total, loc, sizeof(loc), reuse);
        if (rrc != 0 && reuse) {        // stale keep-alive socket -> fresh connect
            conn_close();
            rrc = request_from_ex(0, &status, &total, loc, sizeof(loc), 0);
        }
        if (rrc != 0) return -3;

        if (status >= 300 && status < 400 && loc[0]) {
            strncpy(cur, loc, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';
            continue; // follow redirect
        }
        if (status != 200 && status != 206) {
            snprintf(g_dbg, sizeof(g_dbg), "http %d (%s)", status, g_host);
            conn_close(); return -4;
        }
        g_total = (total > 0) ? (uint64_t)total : 0;
        snprintf(g_dbg, sizeof(g_dbg), "%s %s:%u sz=%llu st=%d",
                 g_tlsmode ? "https" : "http", g_host, g_port,
                 (unsigned long long)g_total, status);
        opened = 1;
    }
    if (!opened) { snprintf(g_dbg, sizeof(g_dbg), "too many redirects"); return -5; }

    // Spin up the read-ahead thread (connection is positioned at byte 0).
    g_ring = malloc(RING_CAP);
    if (!g_ring) { snprintf(g_dbg, sizeof(g_dbg), "oom ring"); conn_close(); return -6; }
    g_ringHead = g_ringFill = 0;
    g_cacheStart = g_servePos = g_rawPos = 0;
    g_stop = g_eof = g_seekReq = 0;
    g_abort = 0;
    g_waits = g_stalls = g_reconnects = g_seeks = 0;
    g_lastWaitMs = 0;
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_rd_mtx");
    scePthreadCondInit(&g_condData, NULL, "ps4cast_rd_data");
    scePthreadCondInit(&g_condSpace, NULL, "ps4cast_rd_space");
    if (scePthreadCreate(&g_rdThread, NULL, reader_main, NULL, "ps4cast_rd") != 0) {
        // Fall back to no read-ahead is not supported here; fail cleanly.
        scePthreadCondDestroy(&g_condData); scePthreadCondDestroy(&g_condSpace);
        scePthreadMutexDestroy(&g_mtx);
        free(g_ring); g_ring = NULL; conn_close();
        snprintf(g_dbg, sizeof(g_dbg), "reader thread failed");
        return -7;
    }
    g_threadUp = 1;
    g_ready = 1;
    return 0;
}

int httpsrc_read(uint8_t *buf, uint64_t pos, uint32_t len) {
    if (!g_ready || !g_ring || len == 0) return -1;
    if (g_total && pos >= g_total) return 0; // EOF

    scePthreadMutexLock(&g_mtx);

    // If the requested byte is outside the rolling cache, ask the reader to
    // reconnect at that range. Small MP4 demuxer back-seeks are usually served
    // directly from cache, avoiding TLS reconnect stalls.
    if (pos < g_cacheStart || pos > g_rawPos) {
        g_seekTo = pos; g_seekReq = 1;
        g_seeks++;
        scePthreadCondSignal(&g_condSpace);
        while (!g_stop && !g_abort && (g_seekReq || pos < g_cacheStart || pos > g_rawPos))
            scePthreadCondTimedwait(&g_condData, &g_mtx, 200 * 1000);
    }

    // Wait for data. Bounded + abortable: a buffer underrun holds here and
    // auto-resumes when the reader refills; Stop/cast sets g_abort to break out;
    // a truly dead stream gives up after ~30s so playback ends instead of hanging.
    int waitedMs = 0;
    while (!g_stop && !g_abort && pos >= g_rawPos && !g_eof && waitedMs < 30000) {
        scePthreadCondTimedwait(&g_condData, &g_mtx, 200 * 1000);
        waitedMs += 200;
        g_waits++;
    }
    g_lastWaitMs = waitedMs;
    if (waitedMs >= 1000) g_stalls++;
    if (g_abort) { scePthreadMutexUnlock(&g_mtx); return 0; }

    uint32_t take = 0;
    if (pos >= g_cacheStart && pos < g_rawPos) {
        uint64_t avail64 = g_rawPos - pos;
        take = (avail64 < len) ? (uint32_t)avail64 : len;
        size_t off = (size_t)(pos - g_cacheStart);
        size_t idx = (g_ringHead + off) % RING_CAP;
        size_t first = take;
        if (idx + first > RING_CAP) first = RING_CAP - idx;
        memcpy(buf, g_ring + idx, first);
        if (take > first) memcpy(buf + first, g_ring, take - first);
        g_servePos = pos + take;
        ring_evict_locked(0);
        scePthreadCondSignal(&g_condSpace);
    }
    scePthreadMutexUnlock(&g_mtx);
    return (int)take;  // 0 = EOF
}
