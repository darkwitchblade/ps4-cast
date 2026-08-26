#include "httpd.h"
#include "httpd_channels.h"
#include "web_ui.h"
#include "player.h"
#include "goldhen.h"
#include "ssdp.h"
#include "pad_diag.h"
#include "sys_diag.h"
#include "trace.h"
#include "notify.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

// IPv4 sockaddr (16 bytes) — OpenOrbis has no OrbisNetSockaddrIn, so we lay it
// out by hand (matches FreeBSD / OrbisNetSockaddr size).
typedef struct {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;       // network byte order
    uint32_t addr;       // network byte order
    uint8_t  zero[8];
} ps4_sockaddr_in;

#define SOL_SOCKET_PS4   0xffff
#define SO_REUSEADDR_PS4 0x0004
#define SO_SNDTIMEO_PS4  0x1005
#define SO_RCVTIMEO_PS4  0x1006
#define SO_NBIO_PS4      0x1200

static OrbisNetId        g_listen = -1;
static OrbisPthread      g_thread;
static OrbisPthread      g_event_thread;
static OrbisPthreadMutex g_mtx;
static int               g_started = 0;
static int               g_event_thread_up = 0;

static char g_pending_url[2048];
static int  g_player_pending = 0;
static int  g_play_pending = 0;
static int  g_stop_pending = 0;
static int  g_quit_pending = 0;

#define AVT_SUBS 4
typedef struct {
    int used;
    char sid[80];
    char host[64];
    char path[256];
    uint16_t port;
    uint32_t seq;
    uint64_t expires_at;
} AvtSubscription;
static AvtSubscription g_avt_subs[AVT_SUBS];
static volatile int g_avt_event_dirty = 1;
static void *event_main(void *arg);

// ---- recents / play-next queue / favorites --------------------------------
#define URL_MAX    1024
#define MAX_RECENT 12
#define MAX_QUEUE  16
#define MAX_FAV    32
#define FAV_PATH   "/data/ps4cast_favs.txt"
static char g_recent[MAX_RECENT][URL_MAX]; static int g_recentN = 0;
static char g_queue[MAX_QUEUE][URL_MAX];   static int g_queueHead = 0, g_queueN = 0;
static char g_fav[MAX_FAV][URL_MAX];       static int g_favN = 0;

// The channel list itself lives in httpd_channels.c.
// The most recently cast URL (HUD title); declared here so the channel-store
// helpers above the request handlers can update it.
static char g_last_push[1024];
const char *httpd_last_push(void) { return g_last_push; }

static void recent_add(const char *url) {     // most-recent-first, deduped
    scePthreadMutexLock(&g_mtx);
    if (g_recentN == 0 || strcmp(g_recent[0], url) != 0) {
        int existing = -1;
        for (int i = 0; i < g_recentN; i++) if (strcmp(g_recent[i], url) == 0) { existing = i; break; }
        int top = existing >= 0 ? existing : (g_recentN < MAX_RECENT ? g_recentN++ : MAX_RECENT - 1);
        for (int i = top; i > 0; i--) strncpy(g_recent[i], g_recent[i-1], URL_MAX - 1);
        strncpy(g_recent[0], url, URL_MAX - 1); g_recent[0][URL_MAX-1] = '\0';
    }
    scePthreadMutexUnlock(&g_mtx);
}
static void queue_push(const char *url) {
    scePthreadMutexLock(&g_mtx);
    if (g_queueN < MAX_QUEUE) {
        strncpy(g_queue[(g_queueHead + g_queueN) % MAX_QUEUE], url, URL_MAX - 1);
        g_queue[(g_queueHead + g_queueN) % MAX_QUEUE][URL_MAX-1] = '\0';
        g_queueN++;
    }
    scePthreadMutexUnlock(&g_mtx);
}

static void favs_save(void) {
    int fd = sceKernelOpen(FAV_PATH, 0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd < 0) return;
    for (int i = 0; i < g_favN; i++) { sceKernelWrite(fd, g_fav[i], strlen(g_fav[i])); sceKernelWrite(fd, "\n", 1); }
    sceKernelClose(fd);
}
static void favs_load(void) {
    int fd = sceKernelOpen(FAV_PATH, 0 /*O_RDONLY*/, 0);
    if (fd < 0) return;
    static char buf[MAX_FAV * URL_MAX];
    int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    g_favN = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_favN < MAX_FAV; line = strtok_r(NULL, "\n", &save)) {
        for (int i = (int)strlen(line) - 1; i >= 0 && (line[i]=='\r'||line[i]==' '); i--) line[i] = '\0';
        if (line[0]) { strncpy(g_fav[g_favN], line, URL_MAX - 1); g_fav[g_favN][URL_MAX-1]='\0'; g_favN++; }
    }
}
static const char *ci_strstr(const char *hay, const char *needle);  // defined below
static int g_cfgPair = 1;              // require the pairing token on mutations
// ---- pairing token ---------------------------------------------------------
// The receiver accepts commands from anyone on the LAN. A per-install token,
// shown on the TV (URL text + QR), gates every state-changing endpoint. Exempt
// by design: DLNA/UPnP clients (they cannot carry a token through SSDP
// discovery) and read-only /status + /trace, which the dev pipeline polls.
#define TOKEN_PATH "/data/ps4cast_token.txt"
static char g_token[9] = "";        // 8 chars + NUL

static void token_generate(void);
static void token_load_or_create(void) {
    int fd = sceKernelOpen(TOKEN_PATH, 0 /*O_RDONLY*/, 0);
    if (fd >= 0) {
        char buf[16] = {0};
        int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
        sceKernelClose(fd);
        if (n == 8) { memcpy(g_token, buf, 8); g_token[8] = '\0'; return; }
    }
    token_generate();
}

// Mint a fresh token and persist it. Shared by first run and POST /token/regen.
// 8 unambiguous chars (no O/0/I/1) from a high-resolution clock stir; the PS4 has
// no /dev/urandom in homebrew, and this only needs to be unique per install.
static void token_generate(void) {
    static const char cs[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    uint64_t t = sceKernelGetProcessTime() ^ (uint64_t)(uintptr_t)&g_token;
    for (int i = 0; i < 8; i++) { g_token[i] = cs[t & 31]; t ^= t >> 7; t *= 0x9E3779B97F4A7C15ULL; t >>= 9; }
    g_token[8] = '\0';
    int fd = sceKernelOpen(TOKEN_PATH, 0x0201 | 0x0400, 0666);
    if (fd >= 0) { sceKernelWrite(fd, g_token, 8); sceKernelClose(fd); }
}

const char *httpd_token(void) { return g_token; }
int httpd_pairing_required(void) { return g_cfgPair; }

// 1 if this request may proceed. token comes from ?t= on the path or the
// X-PS4Cast-Token header. Constant-shape compare; this is a LAN convenience,
// not a crypto boundary.
static int token_ok(const char *path, const char *headers) {
    if (!g_cfgPair || !g_token[0]) return 1;
    const char *q = strchr(path, '?');
    if (q) {
        char t[16] = {0};
        const char *tp = strstr(q, "t=");
        if (tp && (tp == q + 1 || tp[-1] == '&')) {
            int k = 0;
            tp += 2;
            while (tp[k] && tp[k] != '&' && k < 8) { t[k] = tp[k]; k++; }
            t[k] = '\0';
            if (k == 8 && strcmp(t, g_token) == 0) return 1;
        }
    }
    const char *h = ci_strstr(headers, "x-ps4cast-token:");
    if (h) {
        h += 16;
        while (*h == ' ') h++;
        int k = 0;
        while (h[k] && h[k] != '\r' && h[k] != '\n' && k < 8) k++;
        return k == 8 && strncmp(h, g_token, 8) == 0;
    }
    return 0;
}

// Paths that must work without a token: UPnP/DLNA machinery (SSDP-discovered,
// tokenless by protocol) and read-only diagnostics the dev pipeline polls.
static int token_exempt(const char *path) {
    static const char *const exempt[] = {
        "/description.xml", "/AVTransport.xml", "/RenderingControl.xml",
        "/ConnectionManager.xml", "/status", "/trace", "/crashlog", "/token", 0
    };
    for (int i = 0; exempt[i]; i++) {
        int l = (int)strlen(exempt[i]);
        if (strncmp(path, exempt[i], l) == 0 &&
            (path[l] == '\0' || path[l] == '?')) return 1;
    }
    return strncmp(path, "/upnp/", 6) == 0;
}

// ---- persisted settings (debug toasts on/off) ----------------------------
#define CFG_PATH "/data/ps4cast_cfg.txt"
static void cfg_save(void) {
    int fd = sceKernelOpen(CFG_PATH, 0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd < 0) return;
    char line[64];
    // avsync is user-tuned for their TV/soundbar; losing it on every relaunch
    // (while channels/recent/resume persisted) was an inconsistency.
    int n = snprintf(line, sizeof(line), "debug=%d\navsync=%d\npair=%d\n",
                     notify_get_debug(), player_get_avsync(), g_cfgPair);
    sceKernelWrite(fd, line, n);
    sceKernelClose(fd);
}
static void cfg_load(void) {
    int fd = sceKernelOpen(CFG_PATH, 0 /*O_RDONLY*/, 0);
    if (fd < 0) return;
    char buf[128];
    int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    const char *d = strstr(buf, "debug=");
    if (d) notify_set_debug(atoi(d + 6));
    const char *a = strstr(buf, "avsync=");
    if (a) player_set_avsync(atoi(a + 7));
    const char *p = strstr(buf, "pair=");
    if (p) g_cfgPair = atoi(p + 5) ? 1 : 0;
}

// ---- resume positions: remember where each VOD was stopped, resume on replay.
// Kept most-recent-first so the web "Continue Watching" row is ordered.
#define MAX_RESUME  64
#define RESUME_PATH "/data/ps4cast_resume.txt"
static char g_resUrl[MAX_RESUME][URL_MAX];
static int  g_resPos[MAX_RESUME];   // seconds into the title
static int  g_resDur[MAX_RESUME];   // title duration (seconds)
static int  g_resN = 0;

static void resume_save_file(void) {
    int fd = sceKernelOpen(RESUME_PATH, 0x0201 | 0x0400, 0666);
    if (fd < 0) return;
    char line[URL_MAX + 48];
    for (int i = 0; i < g_resN; i++) {
        int n = snprintf(line, sizeof(line), "%d\t%d\t%s\n", g_resPos[i], g_resDur[i], g_resUrl[i]);
        sceKernelWrite(fd, line, n);
    }
    sceKernelClose(fd);
}
static void resume_load(void) {
    int fd = sceKernelOpen(RESUME_PATH, 0, 0);
    if (fd < 0) return;
    static char buf[MAX_RESUME * (URL_MAX + 48)];
    int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    g_resN = 0;
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln && g_resN < MAX_RESUME; ln = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(ln, '\t'); if (!t1) continue; *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = '\0';
        const char *url = t2 + 1;
        if (!url[0]) continue;
        g_resPos[g_resN] = atoi(ln);
        g_resDur[g_resN] = atoi(t1 + 1);
        strncpy(g_resUrl[g_resN], url, URL_MAX - 1); g_resUrl[g_resN][URL_MAX - 1] = '\0';
        g_resN++;
    }
}
static void res_remove(int idx) {
    for (int i = idx; i < g_resN - 1; i++) {
        strcpy(g_resUrl[i], g_resUrl[i+1]); g_resPos[i] = g_resPos[i+1]; g_resDur[i] = g_resDur[i+1];
    }
    g_resN--;
}
// Record a stop position for a URL (cleared when finished); persists to /data.
void httpd_resume_save(const char *url, int pos, int dur) {
    if (!url || !url[0] || dur <= 0) return;
    scePthreadMutexLock(&g_mtx);
    int idx = -1;
    for (int i = 0; i < g_resN; i++) if (strcmp(g_resUrl[i], url) == 0) { idx = i; break; }
    if (idx >= 0) res_remove(idx);             // pull it out; re-insert at front (or drop)
    if (pos >= 5 && pos < dur - 15) {          // meaningful midpoint -> keep, most-recent first
        if (g_resN >= MAX_RESUME) g_resN = MAX_RESUME - 1;   // drop the oldest
        for (int i = g_resN; i > 0; i--) {
            strcpy(g_resUrl[i], g_resUrl[i-1]); g_resPos[i] = g_resPos[i-1]; g_resDur[i] = g_resDur[i-1];
        }
        g_resN++;
        strncpy(g_resUrl[0], url, URL_MAX - 1); g_resUrl[0][URL_MAX - 1] = '\0';
        g_resPos[0] = pos; g_resDur[0] = dur;
    }
    resume_save_file();
    scePthreadMutexUnlock(&g_mtx);
}
// Saved resume position for a URL in seconds, or 0 if none.
int httpd_resume_get(const char *url) {
    int pos = 0;
    if (!url || !url[0]) return 0;
    scePthreadMutexLock(&g_mtx);
    for (int i = 0; i < g_resN; i++) if (strcmp(g_resUrl[i], url) == 0) { pos = g_resPos[i]; break; }
    scePthreadMutexUnlock(&g_mtx);
    return pos;
}

// ---- channel list persistence: survive an app restart without re-loading ----
// The console can't type a URL, so re-loading an IPTV playlist from the phone
// after every relaunch is painful. Persist the parsed channels to /data and
// restore them on boot (no network needed). Callers hold g_mtx.
#define CHAN_PATH "/data/ps4cast_channels.txt"

static void fav_toggle(const char *url) {
    scePthreadMutexLock(&g_mtx);
    int idx = -1;
    for (int i = 0; i < g_favN; i++) if (strcmp(g_fav[i], url) == 0) { idx = i; break; }
    if (idx >= 0) { for (int i = idx; i < g_favN - 1; i++) strncpy(g_fav[i], g_fav[i+1], URL_MAX - 1); g_favN--; }
    else if (g_favN < MAX_FAV) { strncpy(g_fav[g_favN], url, URL_MAX - 1); g_fav[g_favN][URL_MAX-1]='\0'; g_favN++; }
    favs_save();
    scePthreadMutexUnlock(&g_mtx);
}

// Build a JSON array of strings into out. Minimal escaping of " and \.
static int json_list(char *out, int cap, char arr[][URL_MAX], int n) {
    int o = 0; o += snprintf(out + o, cap - o, "[");
    for (int i = 0; i < n && o < cap - 4; i++) {
        if (i) o += snprintf(out + o, cap - o, ",");
        o += snprintf(out + o, cap - o, "\"");
        for (const char *p = arr[i]; *p && o < cap - 8; p++) {
            if (*p == '"' || *p == '\\') out[o++] = '\\';
            out[o++] = *p;
        }
        o += snprintf(out + o, cap - o, "\"");
    }
    o += snprintf(out + o, cap - o, "]");
    return o;
}

// ---- M3U / IPTV playlist -------------------------------------------------
// Fetch an .m3u/.m3u8 playlist URL (via the shared aseg HTTP client) and turn
// it into JSON [{"n":"name","u":"url"},...] so the web UI can show a channel /
// movie picker. IPTV channel lists use a "#EXTINF:<dur> <attrs>,<Display Name>"
// line followed by a media URL. A genuine HLS stream (segment or master
// playlist, identified by #EXT-X- tags) is NOT a channel list — for those we
// return a single entry that points at the original URL so it can be cast.
#define PLAYLIST_MAX_ENTRIES 200
extern int aseg_fetch(const char *url, uint8_t **buf, int *len);
extern void aseg_resume(void);

// Append a JSON-escaped, length-capped string (with surrounding quotes).
static void json_str(char *out, int cap, int *po, const char *s, int maxchars) {
    int o = *po, n = 0;
    if (o < cap - 2) out[o++] = '"';
    for (const char *p = s; *p && o < cap - 8 && n < maxchars; p++, n++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = ch; }
        else if (ch < 0x20)          { out[o++] = ' '; }   // strip control chars
        else                         { out[o++] = ch; }
    }
    if (o < cap - 1) out[o++] = '"';
    *po = o;
}

// Derive a readable name from a URL: last path segment without the query/hash.
static void name_from_url(const char *url, char *out, int cap) {
    const char *q = strpbrk(url, "?#");
    const char *end = q ? q : url + strlen(url);
    const char *slash = end;
    while (slash > url && slash[-1] != '/') slash--;
    int n = (int)(end - slash);
    if (n <= 0 || n >= cap) { strncpy(out, "stream", cap - 1); out[cap - 1] = '\0'; return; }
    memcpy(out, slash, n); out[n] = '\0';
}

int httpd_take_next(char *out, int len) {
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_queueN > 0) {
        strncpy(out, g_queue[g_queueHead], len - 1); out[len-1] = '\0';
        g_queueHead = (g_queueHead + 1) % MAX_QUEUE; g_queueN--;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

// HUD title follows the tuned channel; the store lives in httpd_channels.c now.
static void chan_tuned_push_cb(const char *url) {
    strncpy(g_last_push, url, sizeof(g_last_push) - 1);
    g_last_push[sizeof(g_last_push) - 1] = '\0';
}
static void chan_save_file(void) { httpd_channels_save(); }

static const char DEVICE_XML[] =
"<?xml version=\"1.0\"?>"
"<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<device>"
"<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
"<friendlyName>PS4 Cast</friendlyName>"
"<manufacturer>Sony Interactive Entertainment</manufacturer>"
"<manufacturerURL>https://github.com/</manufacturerURL>"
"<modelName>PS4 Cast Receiver</modelName>"
"<modelDescription>PS4 Cast DLNA Renderer</modelDescription>"
"<modelNumber>1</modelNumber>"
"<serialNumber>PCST00001</serialNumber>"
"<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
"<UDN>uuid:7b2f63a8-2530-4e47-9f3a-0000000c5701</UDN>"
"<presentationURL>/</presentationURL>"
"<serviceList>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
"<SCPDURL>/AVTransport.xml</SCPDURL>"
"<controlURL>/upnp/control/AVTransport</controlURL>"
"<eventSubURL>/upnp/event/AVTransport</eventSubURL>"
"</service>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
"<SCPDURL>/RenderingControl.xml</SCPDURL>"
"<controlURL>/upnp/control/RenderingControl</controlURL>"
"<eventSubURL>/upnp/event/RenderingControl</eventSubURL>"
"</service>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
"<SCPDURL>/ConnectionManager.xml</SCPDURL>"
"<controlURL>/upnp/control/ConnectionManager</controlURL>"
"<eventSubURL>/upnp/event/ConnectionManager</eventSubURL>"
"</service>"
"</serviceList>"
"</device>"
"</root>";

static void send_response(OrbisNetId c, const char *status, const char *ctype,
                          const char *body, int bodylen);

static const char AVTRANSPORT_XML[] =
"<?xml version=\"1.0\"?>"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>SetAVTransportURI</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
"<argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Play</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Stop</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Pause</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Seek</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>"
"<argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetTransportInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
"<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
"<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetPositionInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>"
"<argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>"
"<argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>"
"<argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>"
"<argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>"
"<argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>"
"<argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>"
"<argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetMediaInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
"<argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
"<argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
"<argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
"<argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
"<argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
"<argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument>"
"<argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument>"
"<argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentTransportActions</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Actions</name><direction>out</direction><relatedStateVariable>CurrentTransportActions</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType><allowedValueList><allowedValue>ABS_TIME</allowedValue><allowedValue>REL_TIME</allowedValue><allowedValue>TRACK_NR</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>TransportState</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>LastChange</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>TransportStatus</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>PlaybackStorageMedium</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RecordStorageMedium</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RecordMediumWriteStatus</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTransportActions</name><dataType>string</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static const char RENDERING_XML[] =
"<?xml version=\"1.0\"?>"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>GetVolume</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>SetVolume</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetMute</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>SetMute</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType><allowedValueList><allowedValue>Master</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>Volume</name><dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>Mute</name><dataType>boolean</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static const char CONNECTION_XML[] =
"<?xml version=\"1.0\"?>"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>GetProtocolInfo</name><argumentList>"
"<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
"<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentConnectionIDs</name><argumentList>"
"<argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentConnectionInfo</name><argumentList>"
"<argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>"
"<argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>"
"<argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>"
"<argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>"
"<argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>"
"<argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType><allowedValueList><allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue><allowedValue>InsufficientBandwidth</allowedValue><allowedValue>UnreliableChannel</allowedValue><allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType><allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static char g_dlna_uri[1024];
static int  g_dlna_started;

static void send_response(OrbisNetId c, const char *status, const char *ctype,
                          const char *body, int bodylen);

static void xml_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
        else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
        else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
        else { *w++ = *r++; }
    }
    *w = '\0';
}

static int extract_tag(const char *body, const char *tag, char *out, int outlen) {
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(body, open);
    if (!s) return 0;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return 0;
    int n = (int)(e - s);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    xml_unescape(out);
    return 1;
}

static void xml_escape(const char *src, char *dst, int cap) {
    int n = 0;
    if (cap <= 0) return;
    while (*src && n < cap - 1) {
        const char *rep = NULL;
        if (*src == '&') rep = "&amp;";
        else if (*src == '<') rep = "&lt;";
        else if (*src == '>') rep = "&gt;";
        else if (*src == '"') rep = "&quot;";
        else if (*src == '\'') rep = "&apos;";
        if (rep) {
            int rn = (int)strlen(rep);
            if (n + rn >= cap) break;
            memcpy(dst + n, rep, rn);
            n += rn;
        } else {
            dst[n++] = *src;
        }
        src++;
    }
    dst[n] = '\0';
}

static int parse_upnp_time(const char *s, double *seconds) {
    char *end;
    long h = strtol(s, &end, 10);
    if (end == s || *end != ':' || h < 0) return 0;
    s = end + 1;
    long m = strtol(s, &end, 10);
    if (end == s || *end != ':' || m < 0 || m > 59) return 0;
    s = end + 1;
    double sec = strtod(s, &end);
    if (end == s || *end != '\0' || sec < 0 || sec >= 60) return 0;
    *seconds = (double)h * 3600.0 + (double)m * 60.0 + sec;
    return 1;
}

static void format_upnp_time(double seconds, char *out, int cap) {
    if (seconds < 0) seconds = 0;
    uint64_t total = (uint64_t)(seconds + 0.5);
    snprintf(out, cap, "%llu:%02llu:%02llu",
             (unsigned long long)(total / 3600),
             (unsigned long long)((total / 60) % 60),
             (unsigned long long)(total % 60));
}

static void set_pending_play(const char *url) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_play_pending = 1;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(url);
    player_interrupt();   // unblock a stuck read so the new cast is processed
}

static void set_pending_player_named(const char *url, const char *recentUrl) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_player_pending = 1;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(recentUrl ? recentUrl : url);
    player_interrupt();
}

static void set_pending_player(const char *url) { set_pending_player_named(url, url); }

static void set_pending_channel(const char *url) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_player_pending = 2;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(url);
    player_interrupt();
}

static void set_pending_local_file(const char *displayName) {
    // Local uploads are intentionally not added to Recents: the fixed file is
    // replaced by every upload, so a historical entry would be misleading.
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, PLAYER_LOCAL_UPLOAD_URL, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_player_pending = 1;
    strncpy(g_last_push, displayName, sizeof(g_last_push) - 1);
    g_last_push[sizeof(g_last_push) - 1] = '\0';
    httpd_channels_tune(-1, NULL, 0);   // nothing tuned after a manual cast
    scePthreadMutexUnlock(&g_mtx);
    g_avt_event_dirty = 1;
    player_interrupt();
}

static int form_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int form_value(const char *body, const char *key, char *out, int cap) {
    int klen = (int)strlen(key);
    if (cap <= 0) return 0;
    out[0] = '\0';
    for (const char *p = body; p && *p; ) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && eq - p == klen && memcmp(p, key, (size_t)klen) == 0) {
            int o = 0;
            for (const char *s = eq + 1; s < end && o < cap - 1; s++) {
                unsigned char c = (unsigned char)*s;
                if (c == '+') c = ' ';
                else if (c == '%' && s + 2 < end) {
                    int hi = form_hex(s[1]), lo = form_hex(s[2]);
                    if (hi >= 0 && lo >= 0) { c = (unsigned char)((hi << 4) | lo); s += 2; }
                }
                if (c == 0 || c == '\r' || c == '\n' || c < 0x20 || c == 0x7f) continue;
                out[o++] = (char)c;
            }
            out[o] = '\0';
            return o;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static int append_cast_option(char *dst, int cap, const char *name, const char *value) {
    if (!value || !value[0]) return 1;
    int used = (int)strlen(dst);
    int n = snprintf(dst + used, (size_t)(cap - used), "%c%s=", strchr(dst, '|') ? '&' : '|', name);
    if (n < 0 || used + n >= cap) return 0;
    used += n;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        int escape = c == '%' || c == '&' || c == '\r' || c == '\n' || c < 0x20 || c == 0x7f;
        if (used + (escape ? 3 : 1) >= cap) return 0;
        if (escape) {
            dst[used++] = '%'; dst[used++] = hex[c >> 4]; dst[used++] = hex[c & 15];
        } else dst[used++] = (char)c;
    }
    dst[used] = '\0';
    return 1;
}

static void send_soap_ok(OrbisNetId c, const char *action, const char *inner) {
    // The HTTP server is single-threaded. Keep the large SOAP scratch buffer
    // off its stack: nesting it under handle_client's request/position buffers
    // exhausted the default Orbis pthread stack in v04.22.
    static char body[8192];
    int n = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">%s</u:%sResponse></s:Body>"
        "</s:Envelope>", action, inner ? inner : "", action);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;
    send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body, n);
}

static void send_soap_fault(OrbisNetId c, int code, const char *description) {
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
        "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
        "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
        "</UPnPError></detail></s:Fault></s:Body></s:Envelope>", code, description);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;
    send_response(c, "500 Internal Server Error", "text/xml; charset=\"utf-8\"", body, n);
}

static void send_all(OrbisNetId c, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = sceNetSend(c, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

static void send_response(OrbisNetId c, const char *status, const char *ctype,
                          const char *body, int bodylen) {
    char hdr[256];
    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, bodylen);
    send_all(c, hdr, h);
    if (bodylen > 0)
        send_all(c, body, bodylen);
}

// Case-insensitive strstr (HTTP header names vary in case across clients).
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

static int header_value(const char *req, const char *name, char *out, int cap) {
    const char *p = ci_strstr(req, name);
    if (!p || cap <= 0) return 0;
    p += strlen(name);
    while (*p == ' ' || *p == '\t') p++;
    const char *e = strstr(p, "\r\n");
    if (!e) return 0;
    int n = (int)(e - p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return n > 0;
}

static int parse_callback(const char *value, char *host, int hostcap,
                          uint16_t *port, char *path, int pathcap) {
    const char *p = value;
    if (*p == '<') p++;
    if (strncmp(p, "http://", 7) != 0) return 0;
    p += 7;
    const char *end = strchr(p, '>');
    if (!end) end = p + strlen(p);
    const char *slash = memchr(p, '/', (size_t)(end - p));
    const char *hostend = slash ? slash : end;
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    int hn = (int)((colon ? colon : hostend) - p);
    if (hn <= 0 || hn >= hostcap) return 0;
    memcpy(host, p, hn); host[hn] = '\0';
    int pn = colon ? atoi(colon + 1) : 80;
    if (pn <= 0 || pn > 65535) return 0;
    *port = (uint16_t)pn;
    if (slash) {
        int n = (int)(end - slash);
        if (n >= pathcap) n = pathcap - 1;
        memcpy(path, slash, n); path[n] = '\0';
    } else {
        strncpy(path, "/", pathcap);
        path[pathcap - 1] = '\0';
    }
    return 1;
}

static int subscription_timeout(const char *req) {
    char value[64];
    if (!header_value(req, "TIMEOUT:", value, sizeof(value))) return 300;
    const char *p = ci_strstr(value, "Second-");
    if (!p) return 300;
    int sec = atoi(p + 7);
    if (sec < 60) sec = 60;
    if (sec > 1800) sec = 1800;
    return sec;
}

static void send_subscription_response(OrbisNetId c, const char *sid, int timeout) {
    char hdr[320];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "SID: %s\r\n"
        "TIMEOUT: Second-%d\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n", sid, timeout);
    send_all(c, hdr, n);
}

static int handle_avt_subscription(OrbisNetId c, const char *method, const char *req) {
    if (strcmp(method, "UNSUBSCRIBE") == 0) {
        char sid[80];
        if (!header_value(req, "SID:", sid, sizeof(sid))) {
            send_response(c, "412 Precondition Failed", "text/plain", "missing sid", 11);
            return 1;
        }
        scePthreadMutexLock(&g_mtx);
        for (int i = 0; i < AVT_SUBS; i++)
            if (g_avt_subs[i].used && strcmp(g_avt_subs[i].sid, sid) == 0)
                memset(&g_avt_subs[i], 0, sizeof(g_avt_subs[i]));
        scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "text/plain", "", 0);
        return 1;
    }
    if (strcmp(method, "SUBSCRIBE") != 0) return 0;
    if (!g_event_thread_up) {
        send_response(c, "503 Service Unavailable", "text/plain", "event thread unavailable", 24);
        return 1;
    }

    int timeout = subscription_timeout(req);
    uint64_t expires = sceKernelGetProcessTime() + (uint64_t)timeout * 1000000ULL;
    char sid[80];
    if (header_value(req, "SID:", sid, sizeof(sid))) {
        int found = 0;
        scePthreadMutexLock(&g_mtx);
        for (int i = 0; i < AVT_SUBS; i++) {
            if (g_avt_subs[i].used && strcmp(g_avt_subs[i].sid, sid) == 0) {
                g_avt_subs[i].expires_at = expires;
                found = 1;
                break;
            }
        }
        scePthreadMutexUnlock(&g_mtx);
        if (!found) send_response(c, "412 Precondition Failed", "text/plain", "unknown sid", 11);
        else send_subscription_response(c, sid, timeout);
        return 1;
    }

    char callback[384], host[64], path[256];
    uint16_t port;
    if (!header_value(req, "CALLBACK:", callback, sizeof(callback)) ||
        !parse_callback(callback, host, sizeof(host), &port, path, sizeof(path))) {
        send_response(c, "412 Precondition Failed", "text/plain", "bad callback", 12);
        return 1;
    }

    int slot = -1;
    scePthreadMutexLock(&g_mtx);
    uint64_t now = sceKernelGetProcessTime();
    for (int i = 0; i < AVT_SUBS; i++) {
        if (g_avt_subs[i].used && g_avt_subs[i].expires_at <= now)
            memset(&g_avt_subs[i], 0, sizeof(g_avt_subs[i]));
        if (slot < 0 && !g_avt_subs[i].used) slot = i;
    }
    if (slot >= 0) {
        AvtSubscription *s = &g_avt_subs[slot];
        memset(s, 0, sizeof(*s));
        s->used = 1; s->port = port; s->expires_at = expires;
        snprintf(s->sid, sizeof(s->sid), "uuid:ps4cast-%08x-%08x",
                 (unsigned)(now >> 32), (unsigned)now ^ (unsigned)slot);
        strncpy(s->host, host, sizeof(s->host) - 1);
        strncpy(s->path, path, sizeof(s->path) - 1);
        strncpy(sid, s->sid, sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0';
        g_avt_event_dirty = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    if (slot < 0) send_response(c, "503 Service Unavailable", "text/plain", "subscriber limit", 16);
    else send_subscription_response(c, sid, timeout);
    return 1;
}

static int notify_connect(const AvtSubscription *sub) {
    OrbisNetInAddr in;
    if (sceNetInetPton(ORBIS_NET_AF_INET, sub->host, &in.s_addr) <= 0) return -1;
    OrbisNetId s = sceNetSocket("ps4cast_evt", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) return -1;
    int tmo = 1000 * 1000;
    sceNetSetsockopt(s, SOL_SOCKET_PS4, SO_RCVTIMEO_PS4, &tmo, sizeof(tmo));
    sceNetSetsockopt(s, SOL_SOCKET_PS4, SO_SNDTIMEO_PS4, &tmo, sizeof(tmo));
    ps4_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.len = sizeof(sa); sa.family = ORBIS_NET_AF_INET;
    sa.port = sceNetHtons(sub->port); sa.addr = in.s_addr;
    int nb = 1;
    sceNetSetsockopt(s, SOL_SOCKET_PS4, SO_NBIO_PS4, &nb, sizeof(nb));
    sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa));
    int connected = 0;
    uint64_t start = sceKernelGetProcessTime();
    sceKernelUsleep(10000);
    while (sceKernelGetProcessTime() - start < 1000000ULL) {
        if (sceNetSend(s, "", 0, 0) >= 0) { connected = 1; break; }
        sceKernelUsleep(20000);
    }
    nb = 0;
    sceNetSetsockopt(s, SOL_SOCKET_PS4, SO_NBIO_PS4, &nb, sizeof(nb));
    if (!connected) { sceNetSocketClose(s); return -1; }
    return s;
}

static void notify_avt(const AvtSubscription *sub, const char *state,
                       const char *uri, double duration, int canSeek) {
    static char uriEsc[6144], inner[7168], lastChange[14336], body[15360], req[16384];
    char dur[32];
    format_upnp_time(duration, dur, sizeof(dur));
    xml_escape(uri, uriEsc, sizeof(uriEsc));
    int in = snprintf(inner, sizeof(inner),
        "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
        "<InstanceID val=\"0\"><TransportState val=\"%s\"/>"
        "<TransportStatus val=\"OK\"/><TransportPlaySpeed val=\"1\"/>"
        "<CurrentTrack val=\"%d\"/><CurrentTrackDuration val=\"%s\"/>"
        "<AVTransportURI val=\"%s\"/><CurrentTrackURI val=\"%s\"/>"
        "<CurrentTransportActions val=\"%s\"/></InstanceID></Event>",
        state, uri[0] ? 1 : 0, dur, uriEsc, uriEsc,
        canSeek ? "Play,Stop,Pause,Seek" : "Play,Stop,Pause");
    if (in < 0 || in >= (int)sizeof(inner)) return;
    xml_escape(inner, lastChange, sizeof(lastChange));
    int bn = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
        "<e:property><LastChange>%s</LastChange></e:property></e:propertyset>",
        lastChange);
    if (bn < 0 || bn >= (int)sizeof(body)) return;
    int rn = snprintf(req, sizeof(req),
        "NOTIFY %s HTTP/1.1\r\nHost: %s:%u\r\n"
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "NT: upnp:event\r\nNTS: upnp:propchange\r\nSID: %s\r\nSEQ: %u\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        sub->path, sub->host, sub->port, sub->sid, sub->seq, bn, body);
    if (rn < 0 || rn >= (int)sizeof(req)) return;
    OrbisNetId s = notify_connect(sub);
    if (s < 0) { trace_mark("upnp event connect fail sid=%s host=%s", sub->sid, sub->host); return; }
    send_all(s, req, rn);
    sceNetSocketClose(s);
    trace_mark("upnp event state=%s seq=%u sid=%s", state, sub->seq, sub->sid);
}

static void *event_main(void *arg) {
    (void)arg;
    char lastState[24] = "";
    char lastUri[1024] = "";
    int lastSeek = -1, lastDur = -1;
    for (;;) {
        const char *stateNow = player_started()
            ? (player_is_paused() ? "PAUSED_PLAYBACK" : "PLAYING") : "STOPPED";
        char state[24], uri[1024];
        strncpy(state, stateNow, sizeof(state) - 1); state[sizeof(state) - 1] = '\0';
        scePthreadMutexLock(&g_mtx);
        strncpy(uri, g_dlna_uri, sizeof(uri) - 1); uri[sizeof(uri) - 1] = '\0';
        scePthreadMutexUnlock(&g_mtx);
        double duration = 0;
        player_progress(NULL, &duration);
        int canSeek = player_can_seek();
        int durSec = (int)(duration + 0.5);
        if (strcmp(state, lastState) != 0 || strcmp(uri, lastUri) != 0 ||
            canSeek != lastSeek || durSec != lastDur) g_avt_event_dirty = 1;

        AvtSubscription pending[AVT_SUBS];
        int count = 0;
        if (g_avt_event_dirty) {
            uint64_t now = sceKernelGetProcessTime();
            scePthreadMutexLock(&g_mtx);
            g_avt_event_dirty = 0;
            for (int i = 0; i < AVT_SUBS; i++) {
                AvtSubscription *s = &g_avt_subs[i];
                if (s->used && s->expires_at <= now) memset(s, 0, sizeof(*s));
                if (s->used) { pending[count++] = *s; s->seq++; }
            }
            scePthreadMutexUnlock(&g_mtx);
            for (int i = 0; i < count; i++) notify_avt(&pending[i], state, uri, duration, canSeek);
            strncpy(lastState, state, sizeof(lastState) - 1); lastState[sizeof(lastState) - 1] = '\0';
            strncpy(lastUri, uri, sizeof(lastUri) - 1); lastUri[sizeof(lastUri) - 1] = '\0';
            lastSeek = canSeek; lastDur = durSec;
        }
        sceKernelUsleep(250000);
    }
    return NULL;
}

#define UPLOAD_TMP_PATH "/data/ps4cast_upload.part"
#define UPLOAD_NAME_PATH "/data/ps4cast_upload.name"
#define UPLOAD_MAX_BYTES (32ULL * 1024ULL * 1024ULL * 1024ULL)

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void decode_upload_name(const char *encoded, char *out, int cap) {
    int n = 0;
    for (int i = 0; encoded && encoded[i] && n < cap - 1; i++) {
        unsigned char ch = (unsigned char)encoded[i];
        if (ch == '%' && encoded[i + 1] && encoded[i + 2]) {
            int hi = hex_value(encoded[i + 1]), lo = hex_value(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) { ch = (unsigned char)((hi << 4) | lo); i += 2; }
        }
        // Keep valid UTF-8 bytes for Arabic/accented titles, while preventing
        // control characters or path/JSON punctuation from changing the HUD.
        if (ch < 0x20 || ch == 0x7f) ch = ' ';
        if (ch == '/' || ch == '\\' || ch == '"' || ch == '?' || ch == '#') ch = '_';
        out[n++] = (char)ch;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    if (!n) snprintf(out, cap, "Local file");
}

static int write_file_all(int fd, const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    int done = 0;
    while (done < len) {
        ssize_t wrote = (ssize_t)sceKernelWrite(fd, p + done, (size_t)(len - done));
        if (wrote <= 0 || wrote > len - done) return -1;
        done += (int)wrote;
    }
    return 0;
}

static uint64_t upload_file_size(void) {
    int fd = sceKernelOpen(PLAYER_LOCAL_UPLOAD_PATH, 0 /*O_RDONLY*/, 0);
    if (fd < 0) return 0;
    off_t end = sceKernelLseek(fd, 0, 2 /*SEEK_END*/);
    sceKernelClose(fd);
    return end > 0 ? (uint64_t)end : 0;
}

static void upload_name_save(const char *name) {
    int fd = sceKernelOpen(UPLOAD_NAME_PATH,
                           0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd < 0) return;
    write_file_all(fd, name, (int)strlen(name));
    sceKernelFsync(fd);
    sceKernelClose(fd);
}

static void upload_name_load(char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    int fd = sceKernelOpen(UPLOAD_NAME_PATH, 0 /*O_RDONLY*/, 0);
    if (fd >= 0) {
        int n = (int)sceKernelRead(fd, out, (size_t)(cap - 1));
        sceKernelClose(fd);
        if (n > 0) out[n] = '\0';
    }
    if (!out[0]) snprintf(out, cap, "Uploaded video");
}

static void handle_local_upload(OrbisNetId c, const char *req, int reqLen,
                                const char *hdrend, uint64_t contentLen) {
    if (contentLen == 0) {
        send_response(c, "400 Bad Request", "text/plain", "empty file", 10);
        return;
    }
    if (contentLen > UPLOAD_MAX_BYTES) {
        send_response(c, "413 Payload Too Large", "text/plain", "file too large", 14);
        return;
    }
    if (ci_strstr(req, "Transfer-Encoding: chunked")) {
        send_response(c, "411 Length Required", "text/plain", "content length required", 23);
        return;
    }

    char encodedName[768], displayName[256];
    encodedName[0] = '\0';
    header_value(req, "X-PS4Cast-Filename:", encodedName, sizeof(encodedName));
    decode_upload_name(encodedName, displayName, sizeof(displayName));

    // A client using Expect must receive the interim response before it sends
    // the body. Browsers normally skip this, but supporting it keeps curl useful.
    if (ci_strstr(req, "Expect: 100-continue"))
        send_all(c, "HTTP/1.1 100 Continue\r\n\r\n", 25);

    sceKernelUnlink(UPLOAD_TMP_PATH);
    int fd = sceKernelOpen(UPLOAD_TMP_PATH,
                           0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd < 0) {
        send_response(c, "507 Insufficient Storage", "text/plain", "cannot create upload", 20);
        return;
    }

    const char *body = hdrend + 4;
    int already = reqLen - (int)(body - req);
    if (already < 0) already = 0;
    if ((uint64_t)already > contentLen) already = (int)contentLen;
    uint64_t received = 0;
    int failed = 0;
    if (already > 0) {
        failed = write_file_all(fd, body, already) != 0;
        received = (uint64_t)already;
    }

    static uint8_t uploadBuf[64 * 1024];
    while (!failed && received < contentLen) {
        uint64_t left = contentLen - received;
        int want = left < sizeof(uploadBuf) ? (int)left : (int)sizeof(uploadBuf);
        int got = sceNetRecv(c, uploadBuf, want, 0);
        if (got <= 0 || write_file_all(fd, uploadBuf, got) != 0) { failed = 1; break; }
        received += (uint64_t)got;
    }
    if (!failed) sceKernelFsync(fd);
    sceKernelClose(fd);

    if (failed || received != contentLen) {
        sceKernelUnlink(UPLOAD_TMP_PATH);
        trace_mark("local upload failed name=%s bytes=%llu/%llu", displayName,
                   (unsigned long long)received, (unsigned long long)contentLen);
        send_response(c, "400 Bad Request", "text/plain", "upload interrupted", 18);
        return;
    }
    if (sceKernelRename(UPLOAD_TMP_PATH, PLAYER_LOCAL_UPLOAD_PATH) != 0) {
        sceKernelUnlink(UPLOAD_TMP_PATH);
        send_response(c, "500 Internal Server Error", "text/plain", "cannot finalize upload", 22);
        return;
    }

    upload_name_save(displayName);
    trace_mark("local upload complete name=%s bytes=%llu", displayName,
               (unsigned long long)contentLen);
    set_pending_local_file(displayName);
    send_response(c, "200 OK", "text/plain", "ok", 2);
}

static void handle_client(OrbisNetId c) {
    // One request is handled at a time by server_main, so static storage is safe
    // and avoids spending 8 KB of the HTTP thread stack before dispatch begins.
    static char req[8192];
    int n = sceNetRecv(c, req, sizeof(req) - 1, 0);
    if (n <= 0)
        return;
    req[n] = '\0';

    // A single recv may not contain the whole request: the POST body (the cast
    // URL) often arrives in a later TCP segment, which previously left body=""
    // and surfaced as "bad url" on intermittent casts. Keep reading until we have
    // the header terminator AND the full Content-Length body (bounded by req[]).
    char *hdrend = strstr(req, "\r\n\r\n");
    while (!hdrend && n < (int)sizeof(req) - 1) {           // headers not complete yet
        int r = sceNetRecv(c, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += r; req[n] = '\0';
        hdrend = strstr(req, "\r\n\r\n");
    }
    if (!hdrend) {
        send_response(c, "400 Bad Request", "text/plain", "headers too large", 17);
        return;
    }

    // Parse the route as soon as headers are complete. /upload owns its body as
    // a byte stream; all normal command bodies remain bounded by req[].
    char method[16] = {0}, target[256] = {0}, path[256] = {0};
    sscanf(req, "%15s %255s", method, target);
    snprintf(path, sizeof(path), "%s", target);
    char *query = strchr(path, '?');
    if (query) *query = '\0';
    uint64_t contentLen = 0;
    {
        const char *cl = ci_strstr(req, "Content-Length:");
        if (cl) contentLen = strtoull(cl + strlen("Content-Length:"), NULL, 10);
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/upload") == 0) {
        handle_local_upload(c, req, n, hdrend, contentLen);
        return;
    }

    {
        int clen = contentLen > INT32_MAX ? INT32_MAX : (int)contentLen;
        int have = n - (int)((hdrend + 4) - req);           // body bytes already read
        while (clen > have && n < (int)sizeof(req) - 1) {    // wait for the rest of the body
            int r = sceNetRecv(c, req + n, sizeof(req) - 1 - n, 0);
            if (r <= 0) break;
            n += r; req[n] = '\0';
            have = n - (int)((hdrend + 4) - req);
        }
    }

    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (strcmp(path, "/upnp/event/AVTransport") == 0 &&
        handle_avt_subscription(c, method, req)) return;

    // Pairing gate: mutations and UI pages need the token shown on the TV.
    // DLNA/UPnP and read-only /status + /trace stay open (token_exempt).
    if (!token_exempt(path) && !token_ok(target, req)) {
        send_response(c, "401 Unauthorized", "text/plain",
                      "missing pairing token (see the TV screen)", 41);
        return;
    }

    // Channel store endpoints (GET /channels, POST /channel/*).
    if (httpd_channels_handle(c, method, path, body, send_response)) return;

    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/") == 0 || strncmp(path, "/index", 6) == 0 ||
         strcmp(path, "/handoff") == 0 || strcmp(path, "/setup") == 0)) {
        send_response(c, "200 OK", "text/html; charset=utf-8",
                      WEB_UI_HTML, (int)sizeof(WEB_UI_HTML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/description.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", DEVICE_XML, (int)sizeof(DEVICE_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/AVTransport.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", AVTRANSPORT_XML, (int)sizeof(AVTRANSPORT_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/RenderingControl.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", RENDERING_XML, (int)sizeof(RENDERING_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ConnectionManager.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", CONNECTION_XML, (int)sizeof(CONNECTION_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/upload/status") == 0) {
        char name[256], json[512];
        uint64_t size = upload_file_size();
        upload_name_load(name, sizeof(name));
        int o = snprintf(json, sizeof(json), "{\"exists\":%d,\"size\":%llu,\"name\":",
                         size > 0, (unsigned long long)size);
        json_str(json, sizeof(json), &o, name, 255);
        if (o < (int)sizeof(json) - 2) json[o++] = '}';
        json[o] = '\0';
        send_response(c, "200 OK", "application/json", json, o);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upload/replay") == 0) {
        if (!upload_file_size()) {
            const char *m = "no uploaded file";
            send_response(c, "404 Not Found", "text/plain", m, (int)strlen(m));
            return;
        }
        char name[256];
        upload_name_load(name, sizeof(name));
        set_pending_local_file(name);
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upload/delete") == 0) {
        int activeLocal = player_is_local();
        int pendingLocal = 0;
        scePthreadMutexLock(&g_mtx);
        if (g_player_pending && strcmp(g_pending_url, PLAYER_LOCAL_UPLOAD_URL) == 0) {
            g_player_pending = 0;
            pendingLocal = 1;
        }
        if (activeLocal) g_stop_pending = 1;
        if (activeLocal || pendingLocal) g_last_push[0] = '\0';
        scePthreadMutexUnlock(&g_mtx);
        if (activeLocal) player_interrupt();
        sceKernelUnlink(PLAYER_LOCAL_UPLOAD_PATH);
        sceKernelUnlink(UPLOAD_NAME_PATH);
        sceKernelUnlink(UPLOAD_TMP_PATH);
        player_clear_error();
        trace_mark("local upload deleted active=%d pending=%d", activeLocal, pendingLocal);
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/error/clear") == 0) {
        player_clear_error();
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    if (strcmp(path, "/status") == 0) {
        char dbg[512];
        player_debug(dbg, sizeof(dbg));
        // Static because long URLs can nearly double when JSON-escaped; keeping
        // this off the HTTP thread's stack also leaves headroom for diagnostics.
        static char json[6144];
        int active = player_is_active();
        double cur = 0, dur = 0;
        player_progress(&cur, &dur);
        int o = 0, cap = (int)sizeof(json);
#define JAPP(...) do { \
            if (o < cap - 1) { \
                int _n = snprintf(json + o, cap - o, __VA_ARGS__); \
                if (_n < 0) _n = 0; \
                o = _n >= cap - o ? cap - 1 : o + _n; \
            } \
        } while (0)
        JAPP("{\"ver\":"); json_str(json, cap, &o, APP_VER, 16);
        JAPP(",\"goldhen\":"); json_str(json, cap, &o, goldhen_status(), 96);
        JAPP(",\"status\":"); json_str(json, cap, &o, player_status(), 159);
        JAPP(",\"ssdp\":"); json_str(json, cap, &o, ssdp_status(), 159);
        JAPP(",\"active\":%d,\"paused\":%d,\"cur\":%d,\"dur\":%d,\"last_push\":",
             active, player_is_paused(), (int)(cur + 0.5), (int)(dur + 0.5));
        json_str(json, cap, &o, g_last_push, 1023);
        JAPP(",\"diag\":"); json_str(json, cap, &o, dbg, 511);
        JAPP(",\"pad\":"); json_str(json, cap, &o, pad_diag_get(), 159);
        JAPP(",\"hw_enabled\":%d,\"debug\":%d,\"pair\":%d,\"token\":\"%s\",\"chan_n\":%d,\"chan_cur\":%d,\"buf\":%d,\"rx\":%llu,\"sys\":",
             player_hw_enabled(), notify_get_debug(), g_cfgPair, g_token,
             httpd_chan_count(), httpd_chan_current(),
             player_buffer_pct(), (unsigned long long)player_rx_total());
        json_str(json, cap, &o, sys_diag_get(), 159);
        JAPP(",\"fps\":%d,\"avsync\":%d,\"error_code\":", sys_get_fps(), player_get_avsync());
        json_str(json, cap, &o, player_error_code(), 31);
        JAPP(",\"error_message\":"); json_str(json, cap, &o, player_error_message(), 191);
        JAPP("}");
#undef JAPP
        json[o] = '\0';
        send_response(c, "200 OK", "application/json", json, o);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/trace") == 0) {
        int fd = sceKernelOpen(trace_path(), 0 /*O_RDONLY*/, 0);
        if (fd < 0) { send_response(c, "200 OK", "text/plain", "(no trace yet)", 14); return; }
        static char tb[8192];
        off_t end = sceKernelLseek(fd, 0, 2 /*SEEK_END*/);
        if (end > (off_t)sizeof(tb) - 1) sceKernelLseek(fd, end - ((off_t)sizeof(tb) - 1), 0 /*SEEK_SET*/);
        else sceKernelLseek(fd, 0, 0 /*SEEK_SET*/);
        int tn = (int)sceKernelRead(fd, tb, sizeof(tb) - 1);
        sceKernelClose(fd);
        if (tn < 0) tn = 0;
        tb[tn] = '\0';
        send_response(c, "200 OK", "text/plain", tb, tn);
        return;
    }

    // Toggle the hardware-decode fast path at runtime (A/B testing vs software).
    // POST /hwdecode body "0"/"1"; takes effect on the next cast.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/avsync") == 0) {
        player_set_avsync(atoi(body));
        cfg_save();
        char m[48]; int n = snprintf(m, sizeof(m), "avsync %dms", player_get_avsync());
        send_response(c, "200 OK", "text/plain", m, n);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/hwdecode") == 0) {
        int on = (body[0] != '0');
        player_set_hw(on);
        send_response(c, "200 OK", "text/plain", on ? "hw on" : "hw off", on ? 5 : 6);
        return;
    }

    // Toggle on-screen debug toasts (Settings). POST /debug body "0"/"1".
    if (strcmp(method, "POST") == 0 && strcmp(path, "/debug") == 0) {
        int on = (body[0] != '0');
        notify_set_debug(on);
        cfg_save();
        send_response(c, "200 OK", "text/plain", on ? "debug on" : "debug off", on ? 8 : 9);
        return;
    }

    // Settings toggle: require the pairing token on state-changing requests.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/pairing") == 0) {
        g_cfgPair = body[0] != '0';
        cfg_save();
        send_response(c, "200 OK", "text/plain", g_cfgPair ? "pairing on" : "pairing off",
                      g_cfgPair ? 10 : 11);
        return;
    }

    // The token itself, so an already-paired phone can show it in Settings and
    // the Chrome extension popup can copy it. Read-only, token-gated above.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/token") == 0) {
        send_response(c, "200 OK", "text/plain", g_token, 8);
        return;
    }

    // Roll the pairing token. Deliberately NOT token-exempt: proving you already
    // hold the current token is what stops anyone on the LAN rolling it and
    // locking the owner out. Paired clients that now 401 simply re-pair.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/token/regen") == 0) {
        token_generate();
        send_response(c, "200 OK", "text/plain", g_token, 8);
        return;
    }

    // Last fatal-signal capture (signal + fault address), persisted by the crash
    // handler before clean-exit, so we can diagnose what crashed after a reopen.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/crashlog") == 0) {
        int fd = sceKernelOpen("/data/ps4cast_crash.log", 0 /*O_RDONLY*/, 0);
        if (fd < 0) { send_response(c, "200 OK", "text/plain", "(no crash logged)", 17); return; }
        // Read the WHOLE log (it is capped at CRASHLOG_MAX=4096 on the write side).
        // This used to read only the first 512 bytes, which silently truncated the
        // NEWEST entry -- the one that matters -- mid-line: "HANG v03.98 stale=... up"
        // with the at= marker cut off.
        static char cb[4100];
        int cn = (int)sceKernelRead(fd, cb, sizeof(cb) - 1);
        sceKernelClose(fd);
        if (cn < 0) cn = 0;
        cb[cn] = '\0';
        send_response(c, "200 OK", "text/plain", cb, cn);
        return;
    }

    // Transport controls. /pause body: "1"/"0"/empty(toggle). /seek body: seconds.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/pause") == 0) {
        int want;
        if (body[0] == '1')      want = 1;
        else if (body[0] == '0') want = 0;
        else                     want = !player_is_paused();   // toggle
        player_pause(want);
        g_avt_event_dirty = 1;
        send_response(c, "200 OK", "text/plain", want ? "paused" : "playing", want ? 6 : 7);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/seek") == 0) {
        // Body is an absolute position in seconds (may be fractional).
        double sec = 0; int whole = 0, ok = 0;
        for (const char *p = body; *p >= '0' && *p <= '9'; p++) { whole = whole * 10 + (*p - '0'); ok = 1; }
        sec = whole;
        if (ok) {
            player_seek(sec);
            g_avt_event_dirty = 1;
            send_response(c, "200 OK", "text/plain", "ok", 2);
        } else {
            send_response(c, "400 Bad Request", "text/plain", "bad seconds", 11);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/goldhen") == 0) {
        char resp[420];
        goldhen_probe(resp, sizeof(resp));
        send_response(c, "200 OK", "text/plain", resp, (int)strlen(resp));
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/goldhen/restore") == 0) {
        char resp[420];
        goldhen_restore(resp, sizeof(resp));
        send_response(c, "200 OK", "text/plain", resp, (int)strlen(resp));
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/play") == 0) {
        char trimmed[1024];
        strncpy(trimmed, body, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        // trim trailing whitespace/newlines
        for (int i = (int)strlen(trimmed) - 1; i >= 0; i--) {
            char ch = trimmed[i];
            if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') trimmed[i] = '\0';
            else break;
        }
        set_pending_play(trimmed);
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/avplay") == 0) {
        char trimmed[1024];
        strncpy(trimmed, body, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        for (int i = (int)strlen(trimmed) - 1; i >= 0; i--) {
            char ch = trimmed[i];
            if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') trimmed[i] = '\0';
            else break;
        }
        strncpy(g_last_push, trimmed, sizeof(g_last_push) - 1);
        g_last_push[sizeof(g_last_push) - 1] = '\0';
        set_pending_player(trimmed);
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    // Structured browser-extension handoff. Only non-secret compatibility
    // headers are accepted; cookies/authorization are intentionally excluded.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/cast") == 0) {
        char media[1500], referer[500], origin[256], ua[300], kind[16], spec[2048];
        form_value(body, "url", media, sizeof(media));
        form_value(body, "referer", referer, sizeof(referer));
        form_value(body, "origin", origin, sizeof(origin));
        form_value(body, "ua", ua, sizeof(ua));
        form_value(body, "kind", kind, sizeof(kind));
        if ((strncmp(media, "http://", 7) != 0 && strncmp(media, "https://", 8) != 0) ||
            strchr(media, '|')) {
            const char *json = "{\"ok\":false,\"error\":\"bad media url\"}";
            send_response(c, "400 Bad Request", "application/json", json, (int)strlen(json));
            return;
        }
        snprintf(spec, sizeof(spec), "%s", media);
        if (!append_cast_option(spec, sizeof(spec), "Referer", referer) ||
            !append_cast_option(spec, sizeof(spec), "Origin", origin) ||
            !append_cast_option(spec, sizeof(spec), "User-Agent", ua) ||
            ((strcmp(kind, "hls") == 0 || strcmp(kind, "file") == 0) &&
             !append_cast_option(spec, sizeof(spec), "Type", kind))) {
            const char *json = "{\"ok\":false,\"error\":\"cast request too large\"}";
            send_response(c, "413 Payload Too Large", "application/json", json, (int)strlen(json));
            return;
        }
        strncpy(g_last_push, media, sizeof(g_last_push) - 1);
        g_last_push[sizeof(g_last_push) - 1] = '\0';
        set_pending_player_named(spec, media);
        {
            const char *json = "{\"ok\":true}";
            send_response(c, "200 OK", "application/json", json, (int)strlen(json));
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/AVTransport") == 0) {
        if (strstr(req, "SetAVTransportURI")) {
            char uri[1024];
            if (!extract_tag(body, "CurrentURI", uri, sizeof(uri)) || !uri[0]) {
                send_soap_fault(c, 402, "Invalid Args");
                return;
            }
            scePthreadMutexLock(&g_mtx);
            int changed = strcmp(g_dlna_uri, uri) != 0;
            strncpy(g_dlna_uri, uri, sizeof(g_dlna_uri) - 1);
            g_dlna_uri[sizeof(g_dlna_uri) - 1] = '\0';
            scePthreadMutexUnlock(&g_mtx);
            strncpy(g_last_push, uri, sizeof(g_last_push) - 1);
            g_last_push[sizeof(g_last_push) - 1] = '\0';
            // SetAVTransportURI loads a resource; Play starts it. Starting here
            // as well made Castify's normal SetURI->Play sequence open the same
            // media twice and reset the audio clock.
            if (changed) {
                g_dlna_started = 0;
                if (player_started()) {
                    scePthreadMutexLock(&g_mtx);
                    g_stop_pending = 1;
                    scePthreadMutexUnlock(&g_mtx);
                    player_interrupt();
                }
            }
            g_avt_event_dirty = 1;
            trace_mark("dlna seturi changed=%d len=%d", changed, (int)strlen(uri));
            send_soap_ok(c, "SetAVTransportURI", "");
            return;
        }
        if (strstr(req, "Play")) {
            if (!g_dlna_uri[0]) {
                send_soap_fault(c, 716, "Resource not found");
                return;
            }
            if (g_dlna_started && player_started()) {
                player_pause(0);              // true resume; do not reopen media
            } else {
                set_pending_player(g_dlna_uri);
                g_dlna_started = 1;
            }
            g_avt_event_dirty = 1;
            trace_mark("dlna play resume=%d", player_started() ? 1 : 0);
            send_soap_ok(c, "Play", "");
            return;
        }
        if (strstr(req, "Stop") || strstr(req, "Pause")) {
            if (strstr(req, "Pause")) {
                if (!player_started()) {
                    send_soap_fault(c, 701, "Transition not available");
                    return;
                }
                player_pause(1);
                g_avt_event_dirty = 1;
                trace_mark("dlna pause");
                send_soap_ok(c, "Pause", "");
            } else {
                scePthreadMutexLock(&g_mtx);
                g_stop_pending = 1;
                scePthreadMutexUnlock(&g_mtx);
                g_dlna_started = 0;
                player_interrupt();
                g_avt_event_dirty = 1;
                trace_mark("dlna stop");
                send_soap_ok(c, "Stop", "");
            }
            return;
        }
        if (strstr(req, "Seek")) {
            char unit[32], target[64];
            double seconds = 0, duration = 0;
            player_progress(NULL, &duration);
            if (!extract_tag(body, "Unit", unit, sizeof(unit)) ||
                !extract_tag(body, "Target", target, sizeof(target))) {
                send_soap_fault(c, 402, "Invalid Args");
                return;
            }
            if (strcmp(unit, "ABS_TIME") != 0 && strcmp(unit, "REL_TIME") != 0) {
                send_soap_fault(c, 710, "Seek mode not supported");
                return;
            }
            if (!player_can_seek()) {
                send_soap_fault(c, 710, "Seek mode not supported");
                return;
            }
            if (!parse_upnp_time(target, &seconds) || (duration > 0 && seconds > duration + 0.5)) {
                send_soap_fault(c, 711, "Illegal seek target");
                return;
            }
            player_seek(seconds);
            g_avt_event_dirty = 1;
            trace_mark("dlna seek %.3f", seconds);
            send_soap_ok(c, "Seek", "");
            return;
        }
        if (strstr(req, "GetTransportInfo")) {
            const char *state = player_started() ? (player_is_paused() ? "PAUSED_PLAYBACK" : "PLAYING")
                                                 : "STOPPED";
            char inner[220];
            snprintf(inner, sizeof(inner),
                         "<CurrentTransportState>%s</CurrentTransportState>"
                         "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                         "<CurrentSpeed>1</CurrentSpeed>", state);
            send_soap_ok(c, "GetTransportInfo",
                         inner);
            return;
        }
        if (strstr(req, "GetPositionInfo")) {
            double current = 0, duration = 0;
            char cur[32], dur[32];
            static char uri[6144], inner[6656];
            player_progress(&current, &duration);
            format_upnp_time(current, cur, sizeof(cur));
            format_upnp_time(duration, dur, sizeof(dur));
            xml_escape(g_dlna_uri, uri, sizeof(uri));
            snprintf(inner, sizeof(inner),
                     "<Track>%d</Track><TrackDuration>%s</TrackDuration>"
                     "<TrackMetaData></TrackMetaData><TrackURI>%s</TrackURI>"
                     "<RelTime>%s</RelTime><AbsTime>%s</AbsTime>"
                     "<RelCount>0</RelCount><AbsCount>0</AbsCount>",
                     g_dlna_uri[0] ? 1 : 0, dur, uri, cur, cur);
            send_soap_ok(c, "GetPositionInfo", inner);
            return;
        }
        if (strstr(req, "GetMediaInfo")) {
            double duration = 0;
            char dur[32];
            static char uri[6144], inner[6656];
            player_progress(NULL, &duration);
            format_upnp_time(duration, dur, sizeof(dur));
            xml_escape(g_dlna_uri, uri, sizeof(uri));
            snprintf(inner, sizeof(inner),
                     "<NrTracks>%d</NrTracks><MediaDuration>%s</MediaDuration>"
                     "<CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
                     "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                     "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                     "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
                     g_dlna_uri[0] ? 1 : 0, dur, uri);
            send_soap_ok(c, "GetMediaInfo", inner);
            return;
        }
        if (strstr(req, "GetCurrentTransportActions")) {
            send_soap_ok(c, "GetCurrentTransportActions",
                         player_can_seek() ? "<Actions>Play,Stop,Pause,Seek</Actions>"
                                           : "<Actions>Play,Stop,Pause</Actions>");
            return;
        }
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/RenderingControl") == 0) {
        if (strstr(req, "GetVolume")) {
            const char *body_ok =
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:GetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                "<CurrentVolume>50</CurrentVolume></u:GetVolumeResponse></s:Body></s:Envelope>";
            send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body_ok, (int)strlen(body_ok));
        } else if (strstr(req, "GetMute")) {
            const char *body_ok =
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:GetMuteResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                "<CurrentMute>0</CurrentMute></u:GetMuteResponse></s:Body></s:Envelope>";
            send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body_ok, (int)strlen(body_ok));
        } else {
            const char *rc_ok = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body/></s:Envelope>";
            send_response(c, "200 OK", "text/xml; charset=\"utf-8\"",
                          rc_ok, (int)strlen(rc_ok));
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/ConnectionManager") == 0) {
        const char *resp;
        if (strstr(req, "GetProtocolInfo")) {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                   "<s:Body><u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                   "<Source></Source><Sink>http-get:*:video/mp4:*,http-get:*:video/x-matroska:*,http-get:*:video/*:*</Sink>"
                   "</u:GetProtocolInfoResponse></s:Body></s:Envelope>";
        } else if (strstr(req, "GetCurrentConnectionIDs")) {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                   "<s:Body><u:GetCurrentConnectionIDsResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                   "<ConnectionIDs>0</ConnectionIDs></u:GetCurrentConnectionIDsResponse></s:Body></s:Envelope>";
        } else if (strstr(req, "GetCurrentConnectionInfo")) {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                   "<s:Body><u:GetCurrentConnectionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                   "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
                   "<ProtocolInfo>http-get:*:video/mp4:*</ProtocolInfo>"
                   "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                   "<Direction>Input</Direction><Status>OK</Status>"
                   "</u:GetCurrentConnectionInfoResponse></s:Body></s:Envelope>";
        } else {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body/></s:Envelope>";
        }
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", resp, (int)strlen(resp));
        return;
    }

    if (strcmp(path, "/stop") == 0) {
        scePthreadMutexLock(&g_mtx);
        g_stop_pending = 1;
        scePthreadMutexUnlock(&g_mtx);
        player_interrupt();   // break a stuck read so Stop takes effect now
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    // ---- recents / queue / favorites -------------------------------------
    if (strcmp(method, "GET") == 0 && strcmp(path, "/recent") == 0) {
        static char j[MAX_RECENT * URL_MAX + 64];
        scePthreadMutexLock(&g_mtx); int n = json_list(j, sizeof(j), g_recent, g_recentN); scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, n); return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/favs") == 0) {
        static char j[MAX_FAV * URL_MAX + 64];
        scePthreadMutexLock(&g_mtx); int n = json_list(j, sizeof(j), g_fav, g_favN); scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, n); return;
    }

    // Continue Watching: saved resume positions [{u,p,d},...], most recent first.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/resume") == 0) {
        static char j[MAX_RESUME * (URL_MAX + 32) + 16];
        scePthreadMutexLock(&g_mtx);
        int o = 0; j[o++] = '[';
        for (int i = 0; i < g_resN && o < (int)sizeof(j) - 1200; i++) {
            if (i) j[o++] = ',';
            j[o++] = '{'; o += snprintf(j + o, sizeof(j) - o, "\"u\":"); json_str(j, sizeof(j), &o, g_resUrl[i], 1000);
            o += snprintf(j + o, sizeof(j) - o, ",\"p\":%d,\"d\":%d}", g_resPos[i], g_resDur[i]);
        }
        j[o++] = ']';
        scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, o); return;
    }
    // Remove a Continue-Watching entry (body = URL; empty body clears all).
    if (strcmp(method, "POST") == 0 && strcmp(path, "/resume/clear") == 0) {
        char u[URL_MAX]; strncpy(u, body, sizeof(u) - 1); u[sizeof(u) - 1] = '\0';
        for (int i = (int)strlen(u) - 1; i >= 0 && (u[i]=='\r'||u[i]=='\n'||u[i]==' '||u[i]=='\t'); i--) u[i] = '\0';
        scePthreadMutexLock(&g_mtx);
        if (!u[0]) g_resN = 0;
        else for (int i = 0; i < g_resN; i++) if (strcmp(g_resUrl[i], u) == 0) { res_remove(i); break; }
        resume_save_file();
        scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
    }
    // Remove a Recent entry (body = URL; empty body clears all).
    if (strcmp(method, "POST") == 0 && strcmp(path, "/recent/clear") == 0) {
        char u[URL_MAX]; strncpy(u, body, sizeof(u) - 1); u[sizeof(u) - 1] = '\0';
        for (int i = (int)strlen(u) - 1; i >= 0 && (u[i]=='\r'||u[i]=='\n'||u[i]==' '||u[i]=='\t'); i--) u[i] = '\0';
        scePthreadMutexLock(&g_mtx);
        if (!u[0]) g_recentN = 0;
        else for (int i = 0; i < g_recentN; i++) if (strcmp(g_recent[i], u) == 0) {
            for (int k = i; k < g_recentN - 1; k++) strncpy(g_recent[k], g_recent[k+1], URL_MAX - 1);
            g_recentN--; break;
        }
        scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/queue") == 0) {
        static char tmp[MAX_QUEUE][URL_MAX]; static char j[MAX_QUEUE * URL_MAX + 64]; int n;
        scePthreadMutexLock(&g_mtx);
        n = g_queueN;
        for (int i = 0; i < n; i++) { strncpy(tmp[i], g_queue[(g_queueHead + i) % MAX_QUEUE], URL_MAX - 1); tmp[i][URL_MAX-1] = '\0'; }
        scePthreadMutexUnlock(&g_mtx);
        int jn = json_list(j, sizeof(j), tmp, n);
        send_response(c, "200 OK", "application/json", j, jn); return;
    }
    if (strcmp(method, "POST") == 0 && (strcmp(path, "/queue") == 0 || strcmp(path, "/fav") == 0)) {
        char u[URL_MAX]; strncpy(u, body, sizeof(u) - 1); u[sizeof(u)-1] = '\0';
        for (int i = (int)strlen(u) - 1; i >= 0 && (u[i]=='\r'||u[i]=='\n'||u[i]==' '||u[i]=='\t'); i--) u[i] = '\0';
        if (u[0]) { if (path[1] == 'q') queue_push(u); else fav_toggle(u); }
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
    }

    // Expand an M3U / IPTV playlist link into a JSON channel list. Body = URL.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/playlist") == 0) {
        char url[URL_MAX];
        strncpy(url, body, sizeof(url) - 1); url[sizeof(url) - 1] = '\0';
        for (int i = (int)strlen(url) - 1; i >= 0 && (url[i]=='\r'||url[i]=='\n'||url[i]==' '||url[i]=='\t'); i--) url[i] = '\0';
        const int CAP = 512 * 1024;
        char *out = malloc(CAP);
        if (!out) { send_response(c, "200 OK", "application/json", "[]", 2); return; }
        int n = 0;
        if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
            uint8_t *buf = NULL; int len = 0;
            aseg_resume();   // sticky abort from a previous Stop must not block a web-UI playlist load
            if (aseg_fetch(url, &buf, &len) == 0 && buf && len > 0) {
                uint8_t *txt = realloc(buf, (size_t)len + 1);
                if (txt) {
                    buf = txt; buf[len] = '\0';
                    n = httpd_channels_load_playlist((const char *)buf, url, out, CAP);
                }
                free(buf);
            }
        }
        if (n <= 0) { out[0] = '['; out[1] = ']'; n = 2; }
        send_response(c, "200 OK", "application/json", out, n);
        free(out);
        return;
    }

    // Tune a channel from the loaded playlist by index. Body = index. Drives the
    // in-app player (same path as /avplay) and syncs the shared current-channel.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/chan") == 0) {
        char curl[URL_MAX]; curl[0] = '\0';
        if (httpd_channels_tune(atoi(body), curl, sizeof(curl))) {
            set_pending_channel(curl);
            send_response(c, "200 OK", "text/plain", "ok", 2);
        } else {
            send_response(c, "400 Bad Request", "text/plain", "bad channel", 11);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/quit") == 0) {
        scePthreadMutexLock(&g_mtx);
        g_quit_pending = 1;
        scePthreadMutexUnlock(&g_mtx);
        player_interrupt();
        send_response(c, "200 OK", "text/plain", "closing", 7);
        return;
    }

    send_response(c, "404 Not Found", "text/plain", "not found", 9);
}

static void *server_main(void *arg) {
    (void)arg;
    for (;;) {
        OrbisNetId c = sceNetAccept(g_listen, NULL, NULL);
        if (c < 0)
            continue;
        // A vanished phone must not leave the single HTTP worker blocked on an
        // unfinished upload forever. This is microseconds on Orbis/BSD sockets.
        int tmo = 30 * 1000 * 1000;
        sceNetSetsockopt(c, SOL_SOCKET_PS4, SO_RCVTIMEO_PS4, &tmo, sizeof(tmo));
        sceNetSetsockopt(c, SOL_SOCKET_PS4, SO_SNDTIMEO_PS4, &tmo, sizeof(tmo));
        handle_client(c);
        sceNetSocketClose(c);
    }
    return NULL;
}

int httpd_start(int port) {
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_mtx");
    sceKernelUnlink(UPLOAD_TMP_PATH); // discard a partial upload left by power loss/crash
    favs_load();    // restore saved favorites from /data
    cfg_load();     // restore persisted settings (debug toasts)
    token_load_or_create(); // pairing token for state-changing requests
    resume_load();  // restore saved per-URL resume positions
    httpd_channels_init(); // channel store mutex + restore the last-loaded list
    httpd_channels_set_push_cb(chan_tuned_push_cb);

    g_listen = sceNetSocket("ps4cast", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (g_listen < 0)
        return -1;

    int on = 1;
    sceNetSetsockopt(g_listen, SOL_SOCKET_PS4, SO_REUSEADDR_PS4, &on, sizeof(on));

    ps4_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.len    = sizeof(addr);
    addr.family = ORBIS_NET_AF_INET;
    addr.port   = sceNetHtons((uint16_t)port);
    addr.addr   = 0;  // INADDR_ANY

    if (sceNetBind(g_listen, (const OrbisNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -2;
    }
    if (sceNetListen(g_listen, 8) < 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -3;
    }

    // Explicit headroom for request parsing and future SOAP actions. The large
    // current scratch buffers are static, but relying on the small platform
    // default again would make this thread fragile as handlers evolve.
    OrbisPthreadAttr attr;
    OrbisPthreadAttr *pattr = NULL;
    int attrInit = scePthreadAttrInit(&attr) == 0;
    if (attrInit) {
        if (scePthreadAttrSetstacksize(&attr, 256 * 1024) == 0) pattr = &attr;
    }
    int trc = scePthreadCreate(&g_thread, pattr, server_main, NULL, "ps4cast_httpd");
    if (attrInit) scePthreadAttrDestroy(&attr);
    if (trc != 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -4;
    }
    g_started = 1;
    if (scePthreadCreate(&g_event_thread, NULL, event_main, NULL, "ps4cast_event") == 0)
        g_event_thread_up = 1;
    return 0;
}

void httpd_poll(void) {
    // Nothing to do here; the accept loop runs on its own thread. Kept so the
    // main loop can call it if we later move to a single-threaded model.
}

int httpd_take_play_request(char *out, int len) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_play_pending) {
        strncpy(out, g_pending_url, len - 1);
        out[len - 1] = '\0';
        g_play_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int httpd_take_player_request(char *out, int len) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_player_pending) {
        strncpy(out, g_pending_url, len - 1);
        out[len - 1] = '\0';
        got = g_player_pending;
        g_player_pending = 0;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int httpd_take_stop_request(void) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_stop_pending) {
        g_stop_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int httpd_take_quit_request(void) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_quit_pending) {
        g_quit_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}
