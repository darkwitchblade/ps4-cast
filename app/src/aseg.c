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

static int resolve_host(void) {
    OrbisNetInAddr a;
    memset(&a, 0, sizeof(a));
    if (sceNetInetPton(ORBIS_NET_AF_INET, g_host, &a.s_addr) > 0) { g_addr = a.s_addr; return 0; }
    int pool = net_pool();
    if (pool < 0) return -1;
    OrbisNetId rid = sceNetResolverCreate("ps4cast_ares", pool, 0);
    if (rid < 0) return -2;
    int rc = sceNetResolverStartNtoa(rid, g_host, &a, 8 * 1000 * 1000, 3, 0);
    sceNetResolverDestroy(rid);
    if (rc < 0) return -3;
    g_addr = a.s_addr;
    return 0;
}

static int tcp_connect(void) {
    int s = sceNetSocket("ps4cast_aseg", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) return s;
    int tmo = 8 * 1000 * 1000;
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
    ps4_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.len = sizeof(sa); sa.family = ORBIS_NET_AF_INET;
    sa.port = sceNetHtons(g_port); sa.addr = g_addr;
    if (sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa)) < 0) {
        sceNetSocketClose(s);
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
static int do_request(int *status, char *loc, int loccap,
                      uint8_t *lead, int *leadLen) {
    conn_close();
    g_sock = tcp_connect();
    if (g_sock < 0) return -1;
    if (g_tlsmode) {
        g_tls = tls_open(g_sock, g_host);
        if (!g_tls) { conn_close(); return -2; }
    }

    char req[1600];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PS4Cast/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
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
    g_abort = 0;
    *outBuf = NULL; *outLen = 0;

    char cur[1400];
    strncpy(cur, url, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';

    uint8_t lead[4096]; int leadLen = 0;
    int opened = 0;
    for (int hop = 0; hop < 5 && !opened; hop++) {
        if (parse_url(cur) != 0) { conn_close(); return -1; }
        if (resolve_host() != 0) { conn_close(); return -2; }
        int status = 0; char loc[1400];
        int rc = do_request(&status, loc, sizeof(loc), lead, &leadLen);
        if (rc == 1 && loc[0]) { strncpy(cur, loc, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0'; continue; }
        if (rc != 0) { conn_close(); return -3; }
        if (status != 200 && status != 206) { conn_close(); return -4; }
        opened = 1;
    }
    if (!opened) { conn_close(); return -5; }

    // Read the whole body into a growing buffer (Connection: close => read to EOF).
    size_t cap = 256 * 1024, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { conn_close(); return -6; }
    if (leadLen > 0) { memcpy(buf, lead, leadLen); used = leadLen; }

    for (;;) {
        if (g_abort) { free(buf); conn_close(); return -9; }
        if (used + 64 * 1024 > cap) {
            size_t ncap = cap * 2;
            if (ncap > ASEG_FETCH_CAP) ncap = ASEG_FETCH_CAP;
            if (ncap <= cap) { free(buf); conn_close(); return -10; }
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { free(buf); conn_close(); return -7; }
            buf = nb; cap = ncap;
        }
        int r = conn_read(buf + used, (int)(cap - used));
        if (r > 0) { used += r; continue; }
        break;  // 0 = clean EOF, <0 = error/timeout (return what we have)
    }
    conn_close();

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
