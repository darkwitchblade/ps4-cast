#include "httpd.h"
#include "web_ui.h"
#include "player.h"
#include "escalate.h"
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

static OrbisNetId        g_listen = -1;
static OrbisPthread      g_thread;
static OrbisPthreadMutex g_mtx;
static int               g_started = 0;

static char g_pending_url[1024];
static int  g_player_pending = 0;
static int  g_play_pending = 0;
static int  g_stop_pending = 0;
static int  g_quit_pending = 0;

// ---- recents / play-next queue / favorites --------------------------------
#define URL_MAX    1024
#define MAX_RECENT 12
#define MAX_QUEUE  16
#define MAX_FAV    32
#define FAV_PATH   "/data/ps4cast_favs.txt"
static char g_recent[MAX_RECENT][URL_MAX]; static int g_recentN = 0;
static char g_queue[MAX_QUEUE][URL_MAX];   static int g_queueHead = 0, g_queueN = 0;
static char g_fav[MAX_FAV][URL_MAX];       static int g_favN = 0;

// Loaded M3U/IPTV channel list, shared between the web UI and the on-screen
// (D-pad) channel zapper. g_chanCur is the channel currently tuned, -1 if none.
#define CHAN_NAME_MAX 96
#define CHAN_GRP_MAX  48
// Real IPTV playlists routinely carry thousands of channels; 256 silently
// truncated them (the parse loop just stopped), losing most of the list.
#define MAX_CHAN      2000
static char g_chanName[MAX_CHAN][CHAN_NAME_MAX];
static char g_chanGroup[MAX_CHAN][CHAN_GRP_MAX];
static char g_chanUrl[MAX_CHAN][URL_MAX];
static unsigned char g_chanFav[MAX_CHAN];
static char g_filtLetter = 0;      // 0 = no letter filter
static int  g_filtFav = 0;         // 1 = favourites only
static int  g_filt[MAX_CHAN];
static int  g_filtN = 0;
static int  g_railRow = 0;      // selected bouquet row (0=All,1=Favourites,2+=groups)
// Favourites + an on-screen filter. With thousands of channels the flat zapper
// list is unusable, so the overlay can narrow to a starting letter (A-Z, '#' for
// non-alphabetic) and/or favourites only. The filter maps filtered positions ->
// absolute channel indices so navigation stays simple in main.c.

static int  g_chanN = 0;
static int  g_chanCur = -1;

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
// ---- persisted settings (debug toasts on/off) ----------------------------
#define CFG_PATH "/data/ps4cast_cfg.txt"
static void cfg_save(void) {
    int fd = sceKernelOpen(CFG_PATH, 0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd < 0) return;
    char line[64];
    // avsync is user-tuned for their TV/soundbar; losing it on every relaunch
    // (while channels/recent/resume persisted) was an inconsistency.
    int n = snprintf(line, sizeof(line), "debug=%d\navsync=%d\n",
                     notify_get_debug(), player_get_avsync());
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
static void chan_save_file(void) {
    int fd = sceKernelOpen(CHAN_PATH, 0x0201 | 0x0400, 0666);
    if (fd < 0) return;
    char line[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8];
    for (int i = 0; i < g_chanN; i++) {
        int n = snprintf(line, sizeof(line), "%s\t%s\t%s\t%d\n", g_chanName[i], g_chanGroup[i], g_chanUrl[i], g_chanFav[i] ? 1 : 0);
        sceKernelWrite(fd, line, n);
    }
    sceKernelClose(fd);
}
static void chan_load_file(void) {
    int fd = sceKernelOpen(CHAN_PATH, 0, 0);
    if (fd < 0) return;
    static char buf[MAX_CHAN * (URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8)];
    int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    g_chanN = 0; g_chanCur = -1;
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln && g_chanN < MAX_CHAN; ln = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(ln, '\t'); if (!t1) continue; *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = '\0';
        const char *grp = t1 + 1, *url = t2 + 1; if (!url[0]) continue;
        int fav = 0;
        char *t3 = strchr(t2 + 1, '\t');           // optional 4th column: favourite
        if (t3) { *t3 = '\0'; fav = atoi(t3 + 1) ? 1 : 0; }
        strncpy(g_chanName[g_chanN], ln, CHAN_NAME_MAX - 1);   g_chanName[g_chanN][CHAN_NAME_MAX - 1] = '\0';
        strncpy(g_chanGroup[g_chanN], grp, CHAN_GRP_MAX - 1);  g_chanGroup[g_chanN][CHAN_GRP_MAX - 1] = '\0';
        strncpy(g_chanUrl[g_chanN], url, URL_MAX - 1);         g_chanUrl[g_chanN][URL_MAX - 1] = '\0';
        g_chanFav[g_chanN] = (unsigned char)fav;
        g_chanN++;
    }
}

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

static void chan_add(const char *name, const char *group, const char *url) {
    if (g_chanN >= MAX_CHAN) return;
    strncpy(g_chanName[g_chanN], name, CHAN_NAME_MAX - 1);  g_chanName[g_chanN][CHAN_NAME_MAX - 1] = '\0';
    strncpy(g_chanGroup[g_chanN], group ? group : "", CHAN_GRP_MAX - 1); g_chanGroup[g_chanN][CHAN_GRP_MAX - 1] = '\0';
    strncpy(g_chanUrl[g_chanN], url, URL_MAX - 1);          g_chanUrl[g_chanN][URL_MAX - 1] = '\0';
    g_chanN++;
}

// Pull a quoted #EXTINF attribute value, e.g. key = "group-title=\"".
static void extinf_attr(const char *line, const char *key, char *out, int cap) {
    out[0] = '\0';
    const char *k = strstr(line, key);
    if (!k) return;
    k += strlen(key);
    int i = 0;
    while (k[i] && k[i] != '"' && i < cap - 1) { out[i] = k[i]; i++; }
    out[i] = '\0';
}

// Parse a fetched M3U/IPTV playlist into the shared channel store (caller holds
// g_mtx). A genuine HLS stream (#EXT-X- tags) is one castable entry, not a list.
static void playlist_store(const char *text, const char *srcUrl) {
    g_chanN = 0; g_chanCur = -1;
    if (strstr(text, "#EXT-X-STREAM-INF") || strstr(text, "#EXT-X-TARGETDURATION") ||
        strstr(text, "#EXT-X-MEDIA-SEQUENCE") || strstr(text, "#EXT-X-PLAYLIST-TYPE")) {
        char nm[CHAN_NAME_MAX]; name_from_url(srcUrl, nm, sizeof(nm));
        chan_add(nm, "", srcUrl);
        return;
    }
    char pend[256]; pend[0] = '\0';
    char pendGrp[CHAN_GRP_MAX]; pendGrp[0] = '\0';
    char sticky[CHAN_GRP_MAX]; sticky[0] = '\0';   // #EXTGRP applies until changed
    for (const char *p = text; *p && g_chanN < MAX_CHAN; ) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char line[1100];
        int ll = len < (int)sizeof(line) - 1 ? len : (int)sizeof(line) - 1;
        memcpy(line, p, ll); line[ll] = '\0';
        for (int i = (int)strlen(line) - 1; i >= 0 && (line[i]=='\r'||line[i]==' '||line[i]=='\t'); i--) line[i] = '\0';
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s) {
            if (strncmp(s, "#EXTINF:", 8) == 0) {
                // Channel name = text after the first comma outside quotes
                // (attributes like group-title="A,B" may contain commas).
                const char *cur = s + 8; int inq = 0; const char *name = NULL;
                for (; *cur; cur++) {
                    if (*cur == '"') inq = !inq;
                    else if (*cur == ',' && !inq) { name = cur + 1; break; }
                }
                if (name) {
                    while (*name == ' ' || *name == '\t') name++;
                    strncpy(pend, name, sizeof(pend) - 1); pend[sizeof(pend) - 1] = '\0';
                }
                extinf_attr(s, "group-title=\"", pendGrp, sizeof(pendGrp));
            } else if (strncmp(s, "#EXTGRP:", 8) == 0) {
                const char *g = s + 8; while (*g == ' ' || *g == '\t') g++;
                strncpy(sticky, g, sizeof(sticky) - 1); sticky[sizeof(sticky) - 1] = '\0';
            } else if (s[0] != '#') {
                char nm[CHAN_NAME_MAX];
                if (pend[0]) { strncpy(nm, pend, sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0'; }
                else name_from_url(s, nm, sizeof(nm));
                chan_add(nm, pendGrp[0] ? pendGrp : sticky, s);
                pend[0] = '\0'; pendGrp[0] = '\0';
            }
            // other #directives (#EXTM3U, #EXTVLCOPT, ...) are ignored
        }
        if (!nl) break;
        p = nl + 1;
    }
}

// Serialize the channel store to JSON [{"n":..,"u":..},..] (caller holds g_mtx).
static int chans_to_json(char *out, int cap) {
    int o = 0;
    out[o++] = '[';
    for (int i = 0; i < g_chanN && o < cap - 2400; i++) {
        if (i) out[o++] = ',';
        out[o++] = '{';
        o += snprintf(out + o, cap - o, "\"n\":"); json_str(out, cap, &o, g_chanName[i], 90);
        o += snprintf(out + o, cap - o, ",\"g\":"); json_str(out, cap, &o, g_chanGroup[i], 44);
        o += snprintf(out + o, cap - o, ",\"u\":"); json_str(out, cap, &o, g_chanUrl[i], 1000);
        out[o++] = '}';
    }
    out[o++] = ']';
    return o;
}

// ---- channel store accessors (for the on-screen D-pad zapper, main.c) -----

static void filter_rebuild(void) {
    g_filtN = 0;
    for (int i = 0; i < g_chanN; i++) {
        if (g_filtFav && !g_chanFav[i]) continue;
        if (g_filtLetter) {
            char c = g_chanName[i][0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (g_filtLetter == '#') { if (c >= 'A' && c <= 'Z') continue; }
            else if (c != g_filtLetter) continue;
        }
        g_filt[g_filtN++] = i;
    }
}

void httpd_chan_filter(char letter, int favOnly) {
    g_filtLetter = letter; g_filtFav = favOnly ? 1 : 0; g_railRow = favOnly ? 1 : 0;
    scePthreadMutexLock(&g_mtx); filter_rebuild(); scePthreadMutexUnlock(&g_mtx);
}
char httpd_chan_filter_letter(void) { return g_filtLetter; }
int  httpd_chan_filter_fav(void)    { return g_filtFav; }
int  httpd_chan_filter_count(void)  { return (g_filtLetter || g_filtFav || g_railRow >= 2) ? g_filtN : g_chanN; }
int  httpd_chan_filter_abs(int n) {
    if (!(g_filtLetter || g_filtFav || g_railRow >= 2)) return (n >= 0 && n < g_chanN) ? n : -1;
    return (n >= 0 && n < g_filtN) ? g_filt[n] : -1;
}
int  httpd_chan_is_fav(int i) { return (i >= 0 && i < g_chanN) ? g_chanFav[i] : 0; }
void httpd_chan_toggle_fav(int i) {
    if (i < 0 || i >= g_chanN) return;
    scePthreadMutexLock(&g_mtx);
    g_chanFav[i] = g_chanFav[i] ? 0 : 1;
    filter_rebuild();
    scePthreadMutexUnlock(&g_mtx);
    chan_save_file();
}
// True if any channel starts with `letter` ('#' = non-alphabetic), so the A-Z
// strip can grey out letters that would show an empty list.
int httpd_chan_letter_has(char letter) {
    for (int i = 0; i < g_chanN; i++) {
        char c = g_chanName[i][0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (letter == '#') { if (!(c >= 'A' && c <= 'Z')) return 1; }
        else if (c == letter) return 1;
    }
    return 0;
}

// ---- bouquet rail ---------------------------------------------------------
// Row 0 = All, row 1 = Favourites, then each distinct group ("bouquet") in
// playlist order. Selecting a row just drives httpd_chan_filter().
int httpd_chan_rail_count(void) {
    int n = 2;
    for (int i = 0; i < g_chanN; i++) {
        if (!g_chanGroup[i][0]) continue;
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (g_chanGroup[j][0] && strcmp(g_chanGroup[j], g_chanGroup[i]) == 0) { seen = 1; break; }
        if (!seen) n++;
    }
    return n;
}

void httpd_chan_rail_name(int row, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (row == 0) { snprintf(out, cap, "All"); return; }
    if (row == 1) { snprintf(out, cap, "Favourites"); return; }
    int n = 2;
    for (int i = 0; i < g_chanN; i++) {
        if (!g_chanGroup[i][0]) continue;
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (g_chanGroup[j][0] && strcmp(g_chanGroup[j], g_chanGroup[i]) == 0) { seen = 1; break; }
        if (seen) continue;
        if (n == row) { snprintf(out, cap, "%s", g_chanGroup[i]); return; }
        n++;
    }
    snprintf(out, cap, "Group %d", row - 1);
}

// Apply a rail row as the active filter (keeps any A-Z letter narrowing).
void httpd_chan_rail_select(int row) {
    char letter = httpd_chan_filter_letter();
    if (row == 1) { httpd_chan_filter(letter, 1); return; }
    if (row <= 0) { httpd_chan_filter(letter, 0); return; }
    char grp[CHAN_GRP_MAX]; httpd_chan_rail_name(row, grp, sizeof(grp));
    scePthreadMutexLock(&g_mtx);
    g_filtFav = 0; g_filtLetter = letter;
    g_filtN = 0;
    for (int i = 0; i < g_chanN; i++) {
        if (strcmp(g_chanGroup[i], grp) != 0) continue;
        if (g_filtLetter) {
            char c = g_chanName[i][0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (g_filtLetter == '#') { if (c >= 'A' && c <= 'Z') continue; }
            else if (c != g_filtLetter) continue;
        }
        g_filt[g_filtN++] = i;
    }
    g_railRow = row;
    scePthreadMutexUnlock(&g_mtx);
}

int httpd_chan_count(void) { return g_chanN; }
int httpd_chan_current(void) { return g_chanCur; }
// Copy channel i's name/url into caller buffers under lock (safe vs. reloads).
int httpd_chan_get(int i, char *name, int nameCap, char *url, int urlCap) {
    int ok = 0;
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) {
        if (name && nameCap > 0) { strncpy(name, g_chanName[i], nameCap - 1); name[nameCap - 1] = '\0'; }
        if (url && urlCap > 0)   { strncpy(url, g_chanUrl[i], urlCap - 1);   url[urlCap - 1] = '\0'; }
        ok = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return ok;
}
// Copy channel i's group label (empty string if none).
void httpd_chan_group(int i, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) { strncpy(out, g_chanGroup[i], cap - 1); out[cap - 1] = '\0'; }
    scePthreadMutexUnlock(&g_mtx);
}
// Mark channel i as the one now tuned (also updates the HUD title source).
void httpd_chan_set_current(int i) {
    scePthreadMutexLock(&g_mtx);
    if (i >= -1 && i < g_chanN) {
        g_chanCur = i;
        if (i >= 0) { strncpy(g_last_push, g_chanUrl[i], sizeof(g_last_push) - 1); g_last_push[sizeof(g_last_push) - 1] = '\0'; }
    }
    scePthreadMutexUnlock(&g_mtx);
}

// Pop the next queued URL (main loop calls this on playback finish for autoplay).
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
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType><allowedValueList><allowedValue>ABS_TIME</allowedValue><allowedValue>REL_TIME</allowedValue><allowedValue>TRACK_NR</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>TransportState</name><dataType>string</dataType></stateVariable>"
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

static void send_response(OrbisNetId c, const char *status, const char *ctype,
                          const char *body, int bodylen);

static void xml_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
        else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
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

static void set_pending_play(const char *url) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_play_pending = 1;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(url);
    player_interrupt();   // unblock a stuck read so the new cast is processed
}

static void set_pending_player(const char *url) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_player_pending = 1;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(url);
    player_interrupt();
}

static void send_soap_ok(OrbisNetId c, const char *action, const char *inner) {
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">%s</u:%sResponse></s:Body>"
        "</s:Envelope>", action, inner ? inner : "", action);
    send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body, n);
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

static void handle_client(OrbisNetId c) {
    char req[8192];
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
    if (hdrend) {
        int clen = 0;
        const char *cl = ci_strstr(req, "Content-Length:");
        if (cl) clen = atoi(cl + (int)strlen("Content-Length:"));
        int have = n - (int)((hdrend + 4) - req);           // body bytes already read
        while (clen > have && n < (int)sizeof(req) - 1) {    // wait for the rest of the body
            int r = sceNetRecv(c, req + n, sizeof(req) - 1 - n, 0);
            if (r <= 0) break;
            n += r; req[n] = '\0';
            have = n - (int)((hdrend + 4) - req);
        }
    }

    // Method + path
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/") == 0 || strncmp(path, "/index", 6) == 0)) {
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

    if (strcmp(path, "/status") == 0) {
        char dbg[512];
        player_debug(dbg, sizeof(dbg));
        char json[1800];
        int active = player_is_active();
        double cur = 0, dur = 0;
        player_progress(&cur, &dur);
        int j = snprintf(json, sizeof(json),
                         "{\"ver\":\"%s\",\"jb\":%d,\"goldhen\":\"%s\",\"status\":\"%s\",\"ssdp\":\"%s\",\"active\":%d,\"paused\":%d,\"cur\":%d,\"dur\":%d,\"last_push\":\"%s\",\"diag\":\"%s\",\"pad\":\"%s\",\"hw_enabled\":%d,\"debug\":%d,\"chan_n\":%d,\"chan_cur\":%d,\"buf\":%d,\"rx\":%llu,\"sys\":\"%s\",\"fps\":%d,\"avsync\":%d}",
                         APP_VER, jb_result(), goldhen_status(), player_status(), ssdp_status(),
                         active, player_is_paused(), (int)(cur + 0.5), (int)(dur + 0.5), g_last_push, dbg, pad_diag_get(),
                         player_hw_enabled(), notify_get_debug(), g_chanN, g_chanCur,
                         player_buffer_pct(), (unsigned long long)player_rx_total(), sys_diag_get(), sys_get_fps(), player_get_avsync());
        send_response(c, "200 OK", "application/json", json, j);
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

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/AVTransport") == 0) {
        if (strstr(req, "SetAVTransportURI")) {
            char uri[1024];
            if (extract_tag(body, "CurrentURI", uri, sizeof(uri))) {
                strncpy(g_dlna_uri, uri, sizeof(g_dlna_uri) - 1);
                g_dlna_uri[sizeof(g_dlna_uri) - 1] = '\0';
                strncpy(g_last_push, uri, sizeof(g_last_push) - 1);
                g_last_push[sizeof(g_last_push) - 1] = '\0';
                set_pending_player(g_dlna_uri);
            }
            send_soap_ok(c, "SetAVTransportURI", "");
            return;
        }
        if (strstr(req, "Play")) {
            if (g_dlna_uri[0])
                set_pending_player(g_dlna_uri);
            send_soap_ok(c, "Play", "");
            return;
        }
        if (strstr(req, "Stop") || strstr(req, "Pause")) {
            if (strstr(req, "Pause")) {
                player_pause(1);
                send_soap_ok(c, "Pause", "");
            } else {
                scePthreadMutexLock(&g_mtx);
                g_stop_pending = 1;
                scePthreadMutexUnlock(&g_mtx);
                player_interrupt();
                send_soap_ok(c, "Stop", "");
            }
            return;
        }
        if (strstr(req, "Seek")) {
            send_soap_ok(c, "Seek", "");
            return;
        }
        if (strstr(req, "GetTransportInfo")) {
            const char *state = player_started() ? (player_is_paused() ? "PAUSED_PLAYBACK" : "PLAYING") : "STOPPED";
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
            send_soap_ok(c, "GetPositionInfo",
                         "<Track>0</Track><TrackDuration>00:00:00</TrackDuration>"
                         "<TrackMetaData></TrackMetaData><TrackURI></TrackURI>"
                         "<RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime>"
                         "<RelCount>0</RelCount><AbsCount>0</AbsCount>");
            return;
        }
        if (strstr(req, "GetMediaInfo")) {
            send_soap_ok(c, "GetMediaInfo",
                         "<NrTracks>0</NrTracks><MediaDuration>00:00:00</MediaDuration>"
                         "<CurrentURI></CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
                         "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                         "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                         "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
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
    // ---- channel list management (web UI) ---------------------------------
    // GET /channels -> [{i,n,g,u,f},...] so the phone/browser can manage the
    // list, which is far easier than editing it with a gamepad.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/channels") == 0) {
        static char j[96 * 1024];
        scePthreadMutexLock(&g_mtx);
        int o = 0; j[o++] = '[';
        for (int i = 0; i < g_chanN && o < (int)sizeof(j) - 1600; i++) {
            if (i) j[o++] = ',';
            o += snprintf(j + o, sizeof(j) - o, "{\"i\":%d,\"n\":", i);
            json_str(j, sizeof(j), &o, g_chanName[i], CHAN_NAME_MAX);
            o += snprintf(j + o, sizeof(j) - o, ",\"g\":");
            json_str(j, sizeof(j), &o, g_chanGroup[i], CHAN_GRP_MAX);
            o += snprintf(j + o, sizeof(j) - o, ",\"u\":");
            json_str(j, sizeof(j), &o, g_chanUrl[i], 1000);
            o += snprintf(j + o, sizeof(j) - o, ",\"f\":%d}", g_chanFav[i] ? 1 : 0);
        }
        j[o++] = ']';
        scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, o); return;
    }
    // POST /channel/add   body: name\tgroup\turl
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/add") == 0) {
        char b[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8];
        strncpy(b, body, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        for (int i = (int)strlen(b) - 1; i >= 0 && (b[i]=='\r'||b[i]=='\n'); i--) b[i] = 0;
        char *t1 = strchr(b, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2) { send_response(c, "400 Bad Request", "text/plain", "need name\tgroup\turl", 20); return; }
        *t1 = 0; *t2 = 0;
        scePthreadMutexLock(&g_mtx);
        chan_add(b, t1 + 1, t2 + 1);
        scePthreadMutexUnlock(&g_mtx);
        chan_save_file();
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
    }
    // POST /channel/edit  body: index\tname\tgroup\turl
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/edit") == 0) {
        char b[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 16];
        strncpy(b, body, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        for (int i = (int)strlen(b) - 1; i >= 0 && (b[i]=='\r'||b[i]=='\n'); i--) b[i] = 0;
        char *t1 = strchr(b, '\t'); if (!t1) goto edit_bad; *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) goto edit_bad; *t2 = 0;
        char *t3 = strchr(t2 + 1, '\t'); if (!t3) goto edit_bad; *t3 = 0;
        {
            int idx = atoi(b);
            scePthreadMutexLock(&g_mtx);
            if (idx >= 0 && idx < g_chanN) {
                strncpy(g_chanName[idx], t1 + 1, CHAN_NAME_MAX - 1); g_chanName[idx][CHAN_NAME_MAX-1] = 0;
                strncpy(g_chanGroup[idx], t2 + 1, CHAN_GRP_MAX - 1); g_chanGroup[idx][CHAN_GRP_MAX-1] = 0;
                strncpy(g_chanUrl[idx], t3 + 1, URL_MAX - 1);        g_chanUrl[idx][URL_MAX-1] = 0;
            }
            filter_rebuild();
            scePthreadMutexUnlock(&g_mtx);
            chan_save_file();
            send_response(c, "200 OK", "text/plain", "ok", 2); return;
        }
    edit_bad:
        send_response(c, "400 Bad Request", "text/plain", "need i\tname\tgroup\turl", 23); return;
    }
    // POST /channel/del   body: index   (empty body = clear the whole list)
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/del") == 0) {
        scePthreadMutexLock(&g_mtx);
        if (!body[0] || body[0] == '\n') { g_chanN = 0; g_chanCur = -1; }
        else {
            int idx = atoi(body);
            if (idx >= 0 && idx < g_chanN) {
                for (int i = idx; i < g_chanN - 1; i++) {
                    memcpy(g_chanName[i], g_chanName[i+1], CHAN_NAME_MAX);
                    memcpy(g_chanGroup[i], g_chanGroup[i+1], CHAN_GRP_MAX);
                    memcpy(g_chanUrl[i], g_chanUrl[i+1], URL_MAX);
                    g_chanFav[i] = g_chanFav[i+1];
                }
                g_chanN--;
                if (g_chanCur == idx) g_chanCur = -1;
                else if (g_chanCur > idx) g_chanCur--;
            }
        }
        filter_rebuild();
        scePthreadMutexUnlock(&g_mtx);
        chan_save_file();
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
    }
    // POST /channel/fav   body: index   (toggles)
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/fav") == 0) {
        httpd_chan_toggle_fav(atoi(body));
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
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
                    scePthreadMutexLock(&g_mtx);
                    playlist_store((const char *)buf, url);   // populate shared channel store
                    chan_save_file();                         // persist so it survives a relaunch
                    n = chans_to_json(out, CAP);
                    scePthreadMutexUnlock(&g_mtx);
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
        int i = atoi(body);
        char curl[URL_MAX]; curl[0] = '\0';
        scePthreadMutexLock(&g_mtx);
        if (i >= 0 && i < g_chanN) {
            g_chanCur = i;
            strncpy(curl, g_chanUrl[i], sizeof(curl) - 1); curl[sizeof(curl) - 1] = '\0';
            strncpy(g_last_push, curl, sizeof(g_last_push) - 1); g_last_push[sizeof(g_last_push) - 1] = '\0';
        }
        scePthreadMutexUnlock(&g_mtx);
        if (curl[0]) { set_pending_player(curl); send_response(c, "200 OK", "text/plain", "ok", 2); }
        else send_response(c, "400 Bad Request", "text/plain", "bad channel", 11);
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
        handle_client(c);
        sceNetSocketClose(c);
    }
    return NULL;
}

int httpd_start(int port) {
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_mtx");
    favs_load();    // restore saved favorites from /data
    cfg_load();     // restore persisted settings (debug toasts)
    resume_load();  // restore saved per-URL resume positions
    chan_load_file(); // restore the last-loaded channel list

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

    if (scePthreadCreate(&g_thread, NULL, server_main, NULL, "ps4cast_httpd") != 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -4;
    }
    g_started = 1;
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
        g_player_pending = 0;
        got = 1;
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
