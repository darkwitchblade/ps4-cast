// PS4 Cast — homebrew media casting receiver.
//   * brings up the framebuffer + network
//   * shows a lobby screen with the console's cast URL
//   * runs an HTTP control server (phone web UI posts a video URL)
//   * plays the URL fullscreen via ffmpeg demux/audio plus hardware H.264 decode
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "aseg.h"
#include "qr.h"

#ifndef BOOT_MINIMAL
#include "netutil.h"
#include "httpd.h"
#include "player.h"
#include "httpsrc.h"
#include "ssdp.h"
#include "pad_diag.h"
#include "sys_diag.h"
#include "vdec_hw.h"
#include "notify.h"
#include "audio.h"
#endif

#ifndef BOOT_MINIMAL
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#endif

#include <signal.h>
#include <unistd.h>   // _exit
#ifndef SIGILL
#define SIGILL  4
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGFPE
#define SIGFPE  8
#endif
#ifndef SIGBUS
#define SIGBUS  10
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif

// A fatal fault on ANY thread (decode/present/audio/http) should fully CLOSE the
// app, not leave it half-alive and suspended (the rotating-circle hang). We catch
// the fatal signals process-wide and _exit immediately so the system reaps it
// cleanly. BUT _exit hides the fault from the klog/coredump — so first record the
// signal + fault address to /data (read back via GET /crashlog) so we can still
// diagnose WHAT crashed. Runs on an alt-stack so it works even on stack overflow.
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x0040
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x0001
#endif
extern int sigaltstack(const stack_t *, stack_t *);
extern void player_stage(const char **out);   // current player_play stage, for the hang log

// Persist a one-line crash/hang note to /data (read back via GET /crashlog).
// Uses raw syscalls so it's safe from a signal handler.
// APPEND crash/hang notes (bounded) instead of overwriting a single line. A
// single truncated file meant every new fault destroyed the evidence of the
// previous one, so intermittent crashes were impossible to correlate. Keeps the
// file under CRASHLOG_MAX by restarting it when it grows too large, so /data
// can't fill up. Uses raw syscalls: safe from a signal handler.
#define CRASHLOG_PATH "/data/ps4cast_crash.log"
#define CRASHLOG_MAX  4096
static void persist_crash(const char *buf, int n) {
    // O_APPEND=0x0008; drop back to truncate if the file has grown past the cap.
    int trunc = 0;
    int rd = sceKernelOpen(CRASHLOG_PATH, 0 /*O_RDONLY*/, 0);
    if (rd >= 0) {
        char probe[CRASHLOG_MAX + 1];
        int got = (int)sceKernelRead(rd, probe, sizeof(probe));
        sceKernelClose(rd);
        if (got >= CRASHLOG_MAX) trunc = 1;
    }
    int flags = 0x0201 /*WRONLY|CREAT*/ | (trunc ? 0x0400 /*TRUNC*/ : 0x0008 /*APPEND*/);
    int fd = sceKernelOpen(CRASHLOG_PATH, flags, 0666);
    if (fd >= 0) { sceKernelWrite(fd, buf, (size_t)n); sceKernelClose(fd); }
}

static void fatal_signal(int sig, struct __siginfo *info, void *uap) {
    (void)uap;
    char b[160];
    unsigned long a = info ? (unsigned long)info->si_addr : 0;
    int n = snprintf(b, sizeof(b), "CRASH v" APP_VER " sig=%d addr=0x%lx up=%llus\n", sig, a,
                     (unsigned long long)(sceKernelGetProcessTime() / 1000000ULL));
    persist_crash(b, n);
    gfx_emergency_release();   // release display/GPU so the exit is reclaimable, not unkillable
    _exit(0);
}
static void install_fatal_handlers(void) {
    static char altstk[64 * 1024];           // alt-stack so the handler runs on stack overflow
    stack_t ss; memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstk; ss.ss_size = sizeof(altstk); ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.__sa_handler.__sa_sigaction = fatal_signal;   // toolchain's sa_sigaction macro is broken
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int sigs[10] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP,
                     SIGHUP, SIGINT, SIGQUIT, SIGTERM };
    for (int i = 0; i < 10; i++) sigaction(sigs[i], &sa, NULL);
}

// ---- freeze watchdog ------------------------------------------------------
// fatal_signal handles CATCHABLE faults (a thread SIGSEGVs -> whole app exits ->
// reopen). But a DEADLOCK (a worker died holding a mutex the main loop then waits
// on) or a GPU HANG (gfx_present's flip-wait never returns) raises NO signal —
// the app just freezes and the only recovery is a hard console reboot. The main
// loop stamps a heartbeat each frame; this independent thread force-exits if the
// heartbeat goes stale, turning an indefinite freeze into an auto clean-close.
static volatile uint64_t g_heartbeat = 0;
// Set while a known-slow main-thread operation is running (player_play: tearing
// down the old pipeline + opening/probing the new stream — esp. switching off a
// 1080i software/deinterlace channel). The watchdog grants a longer grace then,
// so a slow-but-progressing channel switch isn't killed as a freeze (CE-34878).
static volatile int g_wdBusy = 0;
// Names the blocking operation currently in flight (e.g. "dns"), so a HANG line
// says WHERE it blocked instead of only which stage. Set it around unabortable
// syscalls; clear it after. Cheap: a pointer store to a static string literal.
static const char *volatile g_wdNote = "";
// The main (render-loop) thread. watchdog_kick() only counts from here: the
// heartbeat means "the main loop is alive", so letting the read-ahead / audio
// threads refresh it would keep a frozen main loop looking healthy forever.
static OrbisPthread g_mainTh = NULL;
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        sceKernelUsleep(2 * 1000 * 1000);                 // poll every 2s
        uint64_t hb = g_heartbeat;
        if (hb == 0) continue;                            // main loop not running yet
        uint64_t now = sceKernelGetProcessTime();
        // A blocked hardware decode does NOT stall the main loop (the heartbeat
        // keeps ticking), so the stale-heartbeat check below cannot see it. Check
        // it explicitly: sceVideodec2Decode is synchronous and unabortable, so the
        // only safe response to a GPU/driver hang is to fail-close the whole
        // process — never abandon the worker or tear the decoder down in place.
        uint64_t hwStuck = vdec_hw_inflight_us();
        if (hwStuck > 10ULL * 1000 * 1000) {
            gfx_emergency_release();
            char hb2[128];
            int hn = snprintf(hb2, sizeof(hb2),
                              "HWHANG v" APP_VER " sceVideodec2Decode blocked %llums\n",
                              (unsigned long long)(hwStuck / 1000));
            persist_crash(hb2, hn);
            _exit(0);
        }
        uint64_t lim = g_wdBusy ? 35ULL * 1000 * 1000     // mid channel-switch: generous
                                : 15ULL * 1000 * 1000;    // normal: ~15s with zero progress = frozen
        if (now > hb && (now - hb) > lim) {
            // Release the display/GPU FIRST so the exit is reclaimable (not
            // unkillable) — do this before the crash-log write, which could
            // itself stall on a frozen /data mount and gate the recovery.
            gfx_emergency_release();
            char b[128];
            const char *stg = "?"; player_stage(&stg);
            const char *nte = (const char *)g_wdNote;
            int n = snprintf(b, sizeof(b), "HANG v" APP_VER " stale=%llums up=%llus stage=%s at=%s\n",
                             (unsigned long long)((now - hb) / 1000),
                             (unsigned long long)(now / 1000000ULL), stg ? stg : "?",
                             (nte && *nte) ? nte : "-");
            persist_crash(b, n);                          // record the hang for /crashlog
            _exit(0);                                     // force full exit; user just reopens
        }
    }
    return NULL;
}

// Pet the freeze watchdog from a long, legitimately-progressing blocking call on
// the main thread (e.g. player_play's demux probe while switching channels),
// so a slow-but-alive stream switch isn't mistaken for a freeze and killed
// (that was the CE-34878 on channel switching). Only kicks once the loop is
// running; safe to call from anywhere.
// Only the MAIN thread's marker is meaningful: the watchdog fires on main-loop
// staleness, so at= must describe where the MAIN thread is blocked. This is a
// single global and aseg/hls/httpsrc all run on worker threads too -- a worker
// setting "dns"/"tls" clobbered main's marker, and its restore wrote back the
// WORKER's saved value. That made at= report a thread other than the stuck one.
// Same rule as watchdog_kick(): calls from workers are ignored.
const char *watchdog_note(const char *w) {
    if (g_mainTh && scePthreadSelf() != g_mainTh) return "";
    const char *p = (const char *)g_wdNote;
    g_wdNote = w ? w : "";
    return p;
}

void watchdog_kick(void) {
    if (!g_heartbeat) return;
    if (g_mainTh && scePthreadSelf() != g_mainTh) return;   // worker thread: not our liveness to vouch for
    g_heartbeat = sceKernelGetProcessTime();
}

// player_play() calls this(1) at entry; the main loop clears it (0) the moment it
// resumes, so the longer grace covers exactly the blocking switch and nothing more.
void watchdog_set_busy(int on) {
    g_wdBusy = on ? 1 : 0;
    if (on && g_heartbeat) g_heartbeat = sceKernelGetProcessTime();
}

#define FB_W 1920
#define FB_H 1080
#define PORT 8080

// Modern palette (shared with the web UI). Anti-aliased rounded panels, vector
// icons and gradients carry the polish; see tools/uipreview.c for a desktop
// renderer used to design these screens.
static const GfxColor BG_TOP = { 0x12, 0x18, 0x30 };
static const GfxColor BG_BOT = { 0x06, 0x09, 0x13 };
static const GfxColor BG     = { 0x06, 0x09, 0x13 };   // init clears
static const GfxColor SURF   = { 0x18, 0x20, 0x3a };
static const GfxColor SURF2  = { 0x22, 0x2c, 0x4e };
static const GfxColor HAIR   = { 0x8a, 0x99, 0xd8 };
static const GfxColor ACCENT = { 0x5b, 0x8c, 0xff };
static const GfxColor ACC_LT = { 0x9d, 0xb8, 0xff };
static const GfxColor LIVE   = { 0x2e, 0xe6, 0xa6 };
static const GfxColor WARN   = { 0xff, 0xc4, 0x4a };
static const GfxColor TXT    = { 0xf3, 0xf6, 0xff };
static const GfxColor MUT    = { 0x9a, 0xa4, 0xc8 };
static const GfxColor FAINT  = { 0x6b, 0x73, 0x98 };
static const GfxColor INK    = { 0x07, 0x0a, 0x14 };
static const GfxColor PAPER  = { 0xf4, 0xf7, 0xff };
// Kept for the (compiled-out) BOOT_MINIMAL diagnostic path.
static const GfxColor WHITE  = { 0xf3, 0xf6, 0xff };

typedef enum { HOME_CAST = 0, HOME_IPTV = 1 } HomeMode;
typedef enum { PLAYBACK_CAST = 0, PLAYBACK_IPTV = 1 } PlaybackOrigin;

#ifndef BOOT_MINIMAL
#define HOME_MODE_PATH "/data/ps4cast_home_mode"
static HomeMode home_mode_load(void) {
    char c = '0';
    int fd = sceKernelOpen(HOME_MODE_PATH, 0, 0);
    if (fd >= 0) { sceKernelRead(fd, &c, 1); sceKernelClose(fd); }
    return c == '1' ? HOME_IPTV : HOME_CAST;
}
static void home_mode_save(HomeMode mode) {
    char c = mode == HOME_IPTV ? '1' : '0';
    int fd = sceKernelOpen(HOME_MODE_PATH, 0x0201 | 0x0400 /*WRONLY|CREAT|TRUNC*/, 0666);
    if (fd >= 0) { sceKernelWrite(fd, &c, 1); sceKernelClose(fd); }
}
#endif

static void fmt_time(double sec, char *out, int cap) {
    if (sec < 0) sec = 0;
    int s = (int)(sec + 0.5);
    int h = s / 3600;
    int m = (s / 60) % 60;
    s %= 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, s);
    else snprintf(out, cap, "%d:%02d", m, s);
}

// last path segment of a URL/path (HUD title), without query/fragment
static void basename_of(const char *url, char *out, int cap) {
    const char *q = url; while (*q && *q != '?' && *q != '#') q++;
    const char *slash = q;
    while (slash > url && slash[-1] != '/') slash--;
    int n = (int)(q - slash);
    if (n <= 0 || n >= cap) { strncpy(out, "Now playing", cap - 1); out[cap - 1] = '\0'; return; }
    memcpy(out, slash, n); out[n] = '\0';
}

// ---- modern UI helpers (mirrored in tools/uipreview.c) -------------------
static void gtext(Gfx *g, int x, int y, const char *s, int sc, GfxColor c, int tr) {
    if (sc >= 4) gfx_text_tr(g, x + 2, y + 2, s, sc, INK, tr);
    gfx_text_tr(g, x, y, s, sc, c, tr);
}
// Always-shadowed text: legible directly over video with no panel behind it
// (a cheap drop shadow instead of a translucent backing blend).
static void stext(Gfx *g, int x, int y, const char *s, int sc, GfxColor c) {
    gfx_text(g, x + 2, y + 2, s, sc, INK);
    gfx_text(g, x, y, s, sc, c);
}
static void ctext(Gfx *g, int cy, const char *s, int sc, GfxColor c, int tr) {
    int w = gfx_text_tr_w(s, sc, tr);
    gtext(g, (g->width - w) / 2, cy, s, sc, c, tr);
}
static void panel(Gfx *g, int x, int y, int w, int h, int r, GfxColor c, int a) {
    gfx_round_a(g, x, y, w, h, r, c, a);
    gfx_rect_a(g, x + r, y, w - 2 * r, 1, HAIR, 36);   // top hairline highlight
}
static void icon_cast(Gfx *g, int cx, int cy, int box) {
    panel(g, cx - box / 2, cy - box / 2, box, box, box / 4, SURF2, 255);
    int s = (int)(box * 0.60f);
    int sw = s, sh = (int)(s * 0.64f);
    int sx = cx - sw / 2, sy = cy - (int)(s * 0.40f);
    int rr = (int)(s * 0.17f); int th = (int)(s * 0.11f); if (th < 2) th = 2;
    gfx_round(g, sx, sy, sw, sh, rr, ACC_LT);
    gfx_round(g, sx + th, sy + th, sw - 2 * th, sh - 2 * th, rr - 1, SURF2);
    int dx = sx, dy = sy + sh + (int)(s * 0.18f);
    gfx_circle(g, dx, dy, (int)(s * 0.08f) + 1, LIVE);
    gfx_arc(g, dx, dy, (int)(s * 0.24f), th, 1, ACCENT);
    gfx_arc(g, dx, dy, (int)(s * 0.42f), th, 1, ACCENT);
}

// The receiver and IPTV browser are presentation modes only. Networking stays
// live in both, so an incoming cast can always interrupt the home screen.
static void draw_home_tabs(Gfx *g, HomeMode mode, int channels) {
    int w = 610, h = 58, x = (g->width - w) / 2, y = 38, half = w / 2;
    panel(g, x, y, w, h, 8, SURF, 245);
    gfx_round(g, x + (mode == HOME_IPTV ? half : 0) + 4, y + 4,
              half - 8, h - 8, 6, ACCENT);
    const char *cast = "L1  Cast receiver";
    char tv[48]; snprintf(tv, sizeof(tv), "Live TV  %d  R1", channels);
    int c1 = x + (half - gfx_text_w(cast, 2)) / 2;
    int c2 = x + half + (half - gfx_text_w(tv, 2)) / 2;
    gfx_text(g, c1, y + 21, cast, 2, mode == HOME_CAST ? INK : MUT);
    gfx_text(g, c2, y + 21, tv, 2, mode == HOME_IPTV ? INK : MUT);
}
static void draw_qr_card(Gfx *g, const char *url, int cx, int top, int module) {
    int quiet = 3;
    int qpix = (QR_SIZE + quiet * 2) * module;
    int pad = 30;
    int card = qpix + pad * 2;
    int x0 = cx - card / 2;
    panel(g, x0, top, card, card, 26, PAPER, 255);
    QrCode qr;
    if (qr_make_url(url, &qr) != 0) return;
    int qx = x0 + pad + quiet * module, qy = top + pad + quiet * module;
    for (int yy = 0; yy < QR_SIZE; yy++)
        for (int xx = 0; xx < QR_SIZE; xx++)
            if (qr.m[yy][xx]) gfx_rect(g, qx + xx * module, qy + yy * module, module, module, INK);
}
static void draw_lobby(Gfx *g, const char *ip, int net_ok) {
    gfx_vgrad(g, 0, 0, g->width, g->height, BG_TOP, BG_BOT);
    int W = g->width;

#ifdef BOOT_MINIMAL
    draw_home_tabs(g, HOME_CAST, 0);
#else
    draw_home_tabs(g, HOME_CAST, httpd_chan_count());
#endif

    // Compact receiver identity; the QR and ready state remain the primary task.
    int track = 0, ws = 6;
    const char *wm = "PS4 Cast";
    int ww = gfx_text_tr_w(wm, ws, track);
    int box = 82, group = box + 24 + ww, gx = (W - group) / 2, brandCy = 172;
    icon_cast(g, gx + box / 2, brandCy, box);
    gtext(g, gx + box + 26, brandCy - (ws * 8) / 2, wm, ws, TXT, track);
    ctext(g, 242, "Ready to receive", 3, LIVE, 0);

    if (net_ok) {
        char url[80];
        if (httpd_pairing_required() && httpd_token()[0])
            snprintf(url, sizeof(url), "http://%s:%d/?t=%s", ip, PORT, httpd_token());
        else
            snprintf(url, sizeof(url), "http://%s:%d", ip, PORT);
        draw_qr_card(g, url, W / 2, 300, 9);
        int below = 300 + ((QR_SIZE + 6) * 9 + 60) + 34;
        ctext(g, below, "Scan to open phone controls", 3, MUT, 0);

        int uw = gfx_text_tr_w(url, 4, 0);
        int pw = uw + 64, ph = 58, px = (W - pw) / 2, py = below + 46;
        panel(g, px, py, pw, ph, 8, SURF2, 235);
        gfx_circle(g, px + 30, py + ph / 2, 6, LIVE);
        gtext(g, px + 52, py + (ph - 32) / 2, url, 4, TXT, 0);
        ctext(g, py + ph + 28, "Browser helper, direct link, or DLNA / UPnP", 2, FAINT, 0);
    } else {
        ctext(g, 470, "No network connection", 5, TXT, 0);
        ctext(g, 560, "Connect the PS4 to Wi-Fi or LAN, then relaunch.", 3, MUT, 0);
    }

    ctext(g, 1010, "L1 / R1  switch mode        Triangle  exit", 2, MUT, 0);
}

#ifndef BOOT_MINIMAL
typedef struct {
    int handles[8];
    int type[8];
    int index[8];
    int count;
    uint32_t prev[8];
    uint32_t down[8];
    int readRc[8];
    int connected[8];
    int infoRc[8];
    int connType[8];
    int deviceClass[8];
    uint8_t ext[8][16];
    uint8_t prevExt[8][16];
    int extChanged[8];
    int extRc[8];
    int ok;
} PadState;

static int pad_init(PadState *p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 8; i++) p->handles[i] = -1;

    scePadInit();

    int user = -1;
    OrbisUserServiceInitializeParams param;
    memset(&param, 0, sizeof(param));
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    sceUserServiceGetInitialUser(&user);
    if (user < 0) return -1;

    // Keep the render/control loop on standard pads only. Polling SPECIAL ports
    // and scePadReadStateExt here can block, leaving the HTTP server alive but
    // the main loop unable to consume casts.
    for (int type_i = 0; type_i < 1; type_i++) {
        int type = ORBIS_PAD_PORT_TYPE_STANDARD;
        for (int idx = 0; idx < 4 && p->count < 8; idx++) {
            int h = scePadOpen(user, type, idx, NULL);
            if (h >= 0) {
                int slot = p->count++;
                p->handles[slot] = h;
                p->type[slot] = type;
                p->index[slot] = idx;
            }
        }
    }
    p->ok = p->count > 0;
    char diag[240];
    snprintf(diag, sizeof(diag), "pad handles=%d", p->count);
    pad_diag_set(diag);
    return p->ok ? 0 : -2;
}

static uint32_t pad_poll(PadState *p) {
    if (!p->ok) return 0;
    uint32_t pressed = 0;
    for (int i = 0; i < p->count; i++) {
        OrbisPadData d;
        memset(&d, 0, sizeof(d));
        p->readRc[i] = scePadReadState(p->handles[i], &d);
        if (p->readRc[i] != 0)
            continue;
        p->connected[i] = d.connected;
        p->prev[i] = p->down[i];
        p->down[i] = d.buttons;
        OrbisPadInformation info;
        memset(&info, 0, sizeof(info));
        p->infoRc[i] = scePadGetControllerInformation(p->handles[i], &info);
        if (p->infoRc[i] == 0) {
            p->connType[i] = info.connectionType;
            p->deviceClass[i] = info.deviceClass;
        }
        p->extChanged[i] = memcmp(p->prevExt[i], d.ext, sizeof(d.ext)) != 0;
        memcpy(p->prevExt[i], d.ext, sizeof(d.ext));
        memcpy(p->ext[i], d.ext, sizeof(d.ext));
        p->extRc[i] = 0;
        uint32_t pbits = p->down[i] & ~p->prev[i];
        pressed |= pbits;
    }
    char diag[240];
    int n = snprintf(diag, sizeof(diag), "pad n=%d", p->count);
    for (int i = 0; i < p->count && n < (int)sizeof(diag) - 1; i++) {
        n += snprintf(diag + n, sizeof(diag) - n,
                      " [%d t=%d i=%d h=%d r=%d xr=%d c=%d b=%08x ct=%d dc=%d x=%02x%02x%02x%02x%s]",
                      i, p->type[i], p->index[i], p->handles[i], p->readRc[i],
                      p->extRc[i], p->connected[i], p->down[i], p->connType[i], p->deviceClass[i],
                      p->ext[i][0], p->ext[i][1], p->ext[i][2], p->ext[i][3],
                      p->extChanged[i] ? "*" : "");
    }
    pad_diag_set(diag);
    return pressed;
}

// Buttons currently HELD (pad_poll returns edges only). Used for continuous
// scrubbing: holding a seek button sweeps a preview position instead of firing
// one seek per press.
static uint32_t pad_held(PadState *p) {
    uint32_t m = 0;
    if (!p->ok) return 0;
    for (int i = 0; i < p->count; i++) if (p->readRc[i] == 0) m |= p->down[i];
    return m;
}

// Live scrub preview: while a seek button is held the HUD shows this target and
// NO seek is issued; the seek is committed once on release. Repeatedly seeking
// while held would flush + refetch the network buffer on every step.
static int    g_scrubActive = 0;
static double g_scrubTarget = 0;

static void draw_hud(Gfx *g, PlaybackOrigin origin) {
    double cur = 0, dur = 0;
    player_progress(&cur, &dur);
    if (g_scrubActive) cur = g_scrubTarget;   // preview the position being scrubbed to
    int paused = player_is_paused();

    // Netflix-style: no panel/scrim blend — just shadowed text and a thin
    // scrubber laid directly over the video, so it's nearly free to draw.
    int W = g->width;
    int barX = 80, barW = W - 160, barH = 6, barY = g->height - 78;

    // title + status (bottom-left, above the scrubber). Bigger + brighter than
    // before so the status/time read cleanly from the couch.
    char title[160];
    if (origin == PLAYBACK_IPTV && httpd_chan_current() >= 0)
        httpd_chan_get(httpd_chan_current(), title, sizeof(title), NULL, 0);
    else
        basename_of(httpd_last_push(), title, sizeof(title));
    stext(g, barX, barY - 92, title, 4, TXT);
    const char *state = paused ? "Paused" : (origin == PLAYBACK_IPTV ? "Live TV" : "Playing");
    gfx_circle(g, barX + 6, barY - 32, 6, paused ? WARN : LIVE);
    stext(g, barX + 24, barY - 44, state, 3, paused ? WARN : TXT);

    if (dur > 0) {
        // seekable VOD: scrubber + times
        gfx_round(g, barX, barY, barW, barH, barH / 2, SURF2);
        float p = (float)(cur / dur); if (p < 0) p = 0; if (p > 1) p = 1;
        int fw = (int)(barW * p);
        if (fw > barH) gfx_round(g, barX, barY, fw, barH, barH / 2, ACCENT);
        gfx_circle(g, barX + fw, barY + barH / 2, 9, TXT);
        gfx_circle(g, barX + fw, barY + barH / 2, 4, ACCENT);
        char curS[24], durS[24];
        fmt_time(cur, curS, sizeof(curS));
        fmt_time(dur, durS, sizeof(durS));
        stext(g, barX, barY + 18, curS, 3, TXT);
        stext(g, barX + barW - gfx_text_w(durS, 3), barY + 18, durS, 3, MUT);
    } else {
        // live stream: a thin static accent line + LIVE tag
        gfx_round(g, barX, barY, barW, barH, barH / 2, SURF2);
        gfx_round(g, barX, barY, barW, barH, barH / 2, ACCENT);
        stext(g, barX, barY + 18, "LIVE", 3, LIVE);
    }
}

static int chan_filtered_pos(int absolute) {
    int n = httpd_chan_filter_count();
    for (int i = 0; i < n; i++) if (httpd_chan_filter_abs(i) == absolute) return i;
    return -1;
}

static int chan_rail_for_abs(int absolute) {
    char grp[48]; httpd_chan_group(absolute, grp, sizeof(grp));
    if (!grp[0]) return 0;
    int n = httpd_chan_rail_count();
    for (int i = 2; i < n; i++) {
        char name[80]; httpd_chan_rail_name(i, name, sizeof(name));
        if (strcmp(name, grp) == 0) return i;
    }
    return 0;
}

// ---- Live TV home: one full-width bouquet/channel browser ----------------
// Cast reception remains active while this screen is open. Keeping the two
// products visually separate makes the current input semantics obvious.
static void draw_channel_home(Gfx *g, int sel, int railSel, int inChannels,
                              const char *ip, int net_ok) {
    gfx_vgrad(g, 0, 0, g->width, g->height, BG_TOP, BG_BOT);
    int W = g->width, H = g->height;
    draw_home_tabs(g, HOME_IPTV, httpd_chan_count());

    int lx = 150, lw = W - 300, ly = 200, rowH = 58;
    char rail[80]; httpd_chan_rail_name(railSel, rail, sizeof(rail));
    gfx_round(g, lx, 136, 6, 34, 3, ACCENT);
    gtext(g, lx + 20, 132, inChannels ? rail : "Bouquets", 5, TXT, 0);
    char sub[80];
    int n = inChannels ? httpd_chan_filter_count() : httpd_chan_rail_count();
    snprintf(sub, sizeof(sub), inChannels ? "%d channels" : "%d groups", n);
    gfx_text(g, lx + lw - gfx_text_w(sub, 2), 150, sub, 2, FAINT);
    gfx_rect_a(g, lx, 184, lw, 1, HAIR, 30);

    if (httpd_chan_count() <= 0) {
        icon_cast(g, W / 2, 390, 90);
        ctext(g, 474, "No IPTV playlist loaded", 5, TXT, 0);
        ctext(g, 544, "Open the web controls and add an M3U playlist", 3, MUT, 0);
        if (net_ok) {
            char url[80]; snprintf(url, sizeof(url), "http://%s:%d", ip, PORT);
            ctext(g, 602, url, 3, ACC_LT, 0);
        }
        ctext(g, H - 52, "L1 / R1  switch mode        Triangle  exit", 2, MUT, 0);
        return;
    }

    int rows = (H - ly - 112) / rowH;
    int start = sel - rows / 2;
    if (start > n - rows) start = n - rows;
    if (start < 0) start = 0;
    int cur = httpd_chan_current();
    for (int r = 0; r < rows && start + r < n; r++) {
        int i = start + r, y = ly + r * rowH, on = (i == sel);
        if (on) gfx_round_a(g, lx, y, lw, rowH - 6, 8, ACCENT, 235);
        if (!inChannels) {
            char nm[80]; httpd_chan_rail_name(i, nm, sizeof(nm));
            gfx_text(g, lx + 24, y + (rowH - 6) / 2 - 12, nm, 3, on ? INK : TXT);
            gfx_text(g, lx + lw - 40, y + (rowH - 6) / 2 - 8, ">", 2, on ? INK : FAINT);
        } else {
            int abs = httpd_chan_filter_abs(i);
            if (abs < 0) continue;
            char nm[96]; httpd_chan_get(abs, nm, sizeof(nm), NULL, 0);
            int maxch = (lw - 240) / 24; if (maxch < 6) maxch = 6;
            if ((int)strlen(nm) > maxch) nm[maxch] = 0;
            char num[8]; snprintf(num, sizeof(num), "%d", abs + 1);
            gfx_text(g, lx + 24, y + (rowH - 6) / 2 - 8, num, 2, on ? INK : FAINT);
            gfx_text(g, lx + 118, y + (rowH - 6) / 2 - 12, nm, 3, on ? INK : TXT);
            if (httpd_chan_is_fav(abs)) gfx_text(g, lx + lw - 142, y + 17, "FAV", 2, on ? INK : WARN);
            if (abs == cur) {
                gfx_circle(g, lx + lw - 50, y + (rowH - 6) / 2, 6, on ? INK : LIVE);
                gfx_text(g, lx + lw - 36, y + 18, "LIVE", 1, on ? INK : LIVE);
            }
        }
    }
    if (n == 0) ctext(g, ly + 70, "No channels in this bouquet", 3, MUT, 0);

    ctext(g, H - 56, inChannels
        ? "Up/Down channel   Cross watch   Square favourite   Circle bouquets   L2/R2 bouquet"
        : "Up/Down bouquet   Cross open   L1/R1 switch mode", 2, MUT, 0);
}

// The playback guide reuses the same filtered selection as the Live TV home.
// Browsing never tunes automatically; Cross is the single commit action.
static void draw_channel_guide(Gfx *g, int sel, int railSel) {
    int n = httpd_chan_filter_count();
    if (n <= 0) {
        int W = 800, H = 190, x = 56, y = (g->height - H) / 2;
        panel(g, x, y, W, H, 12, INK, 232);
        gtext(g, x + 34, y + 28, "Channel guide", 4, TXT, 0);
        gfx_text(g, x + 34, y + 92, "No channels in this bouquet", 3, MUT);
        gfx_text(g, x + 34, y + 146, "Left/Right bouquet   Circle close", 2, MUT);
        return;
    }
    int cur = httpd_chan_current();
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;

    int K = 9, rowH = 70, headH = 76, footH = 56;
    int shown = n < K ? n : K;
    int W = 800, H = headH + shown * rowH + footH;
    int x = 56, y = (g->height - H) / 2;
    panel(g, x, y, W, H, 12, INK, 232);

    gfx_round(g, x + 28, y + 26, 6, 28, 3, ACCENT);
    gtext(g, x + 46, y + 22, "Channel guide", 4, TXT, 0);
    char cnt[24]; snprintf(cnt, sizeof(cnt), "%d", n);
    char rail[80]; httpd_chan_rail_name(railSel, rail, sizeof(rail));
    gfx_text(g, x + W - 28 - gfx_text_w(cnt, 2), y + 30, cnt, 2, FAINT);
    gfx_text(g, x + W - 60 - gfx_text_w(cnt, 2) - gfx_text_w(rail, 2), y + 30, rail, 2, ACC_LT);
    gfx_rect_a(g, x + 24, y + headH - 12, W - 48, 1, HAIR, 30);

    int start = sel - K / 2;
    if (start > n - K) start = n - K;
    if (start < 0) start = 0;

    for (int r = 0; r < K && start + r < n; r++) {
        int pos = start + r, idx = httpd_chan_filter_abs(pos), rowY = y + headH + r * rowH;
        if (idx < 0) continue;
        int rx = x + 18, rw = W - 36;
        int seld = (pos == sel);
        gfx_round_a(g, rx, rowY + 6, rw, rowH - 12, 8, seld ? ACCENT : SURF, seld ? 240 : 130);

        char name[96];
        httpd_chan_get(idx, name, sizeof(name), NULL, 0);
        int maxch = (rw - 230) / 24; if (maxch < 4) maxch = 4;
        if ((int)strlen(name) > maxch) name[maxch] = '\0';

        char num[8]; snprintf(num, sizeof(num), "%d", idx + 1);
        GfxColor numc = seld ? INK : FAINT, nc = seld ? INK : TXT;
        gfx_text(g, rx + 26, rowY + rowH / 2 - 4, num, 2, numc);
        gfx_text(g, rx + 104, rowY + rowH / 2 - 12, name, 3, nc);
        if (idx == cur) {
            int dx = rx + rw - 72;
            gfx_circle(g, dx, rowY + rowH / 2, 6, seld ? INK : LIVE);
            gfx_text(g, dx + 14, rowY + rowH / 2 - 8, "LIVE", 1, seld ? INK : LIVE);
        }
    }
    gfx_text(g, x + 28, y + H - 36,
             "Up/Down select   Left/Right bouquet   Cross watch   Circle close", 2, MUT);
}

static void draw_channel_banner(Gfx *g) {
    int cur = httpd_chan_current();
    if (cur < 0) return;
    char name[96], grp[48];
    if (!httpd_chan_get(cur, name, sizeof(name), NULL, 0)) return;
    httpd_chan_group(cur, grp, sizeof(grp));
    int x = 64, y = 64, w = 680, h = 108;
    panel(g, x, y, w, h, 8, INK, 226);
    gfx_round(g, x + 24, y + 22, 6, h - 44, 3, ACCENT);
    gfx_text(g, x + 48, y + 22, name, 4, TXT);
    gfx_text(g, x + 48, y + 72, grp[0] ? grp : "Live TV", 2, MUT);
    gfx_circle(g, x + w - 70, y + h / 2, 7, LIVE);
    gfx_text(g, x + w - 52, y + h / 2 - 8, "LIVE", 1, LIVE);
}

// Top-right stream telemetry, toggled by the touchpad. Plain shadowed text with
// NO panel/blend behind it — the cheapest possible overlay, can't affect decode.
static void draw_stats_overlay(Gfx *g, double netBps, int fps) {
    PlayerStats s; player_stats(&s);
    // GoldHEN-style corner counter: white text, top-right, no panel — but with a
    // subtle drop-shadow (stext draws a dark copy underneath) so it stays readable
    // over bright/white scenes. Tight line spacing. Right-aligned to one margin.
    int rh = 22, ry = 28, rx = g->width - 34;
    char b[128];
    #define STAT(...) do { snprintf(b, sizeof(b), __VA_ARGS__); \
        stext(g, rx - gfx_text_w(b, 2), ry, b, 2, WHITE); ry += rh; } while (0)
    STAT("%s %s", s.hw ? "HW" : "SW", s.codec);
    STAT("%dx%d  %d fps", s.w, s.h, fps);
    if (s.aheadSec > 0.1) STAT("buffer %d%%  +%.1fs", s.bufPct, s.aheadSec);
    else                  STAT("buffer %d%%", s.bufPct);
    if (netBps >= 1e6)      STAT("%.1f MB/s", netBps / 1e6);
    else if (netBps >= 1e3) STAT("%.0f KB/s", netBps / 1e3);
    else                    STAT("%.0f B/s", netBps);
    STAT("%s%s", s.hls ? (s.segDemux ? "HLS seg" : "HLS") : "HTTP", s.lan ? " LAN" : "");
    STAT("drops %ld", s.drops);
    #undef STAT
}
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install_fatal_handlers();   // crash on any thread -> clean full close, not a suspended hang

    Gfx g;
    if (gfx_init(&g, FB_W, FB_H) != 0) {
        // Without a framebuffer we cannot show anything; bail.
        for (;;) {}
    }

    // Initialize every rotating scanout buffer before any partial overlay can
    // touch it. Leaving the third triple-buffer surface undefined can flash old
    // direct-memory contents during startup or the first playback transition.
    int bootFrameID = 0;
    for (int i = 0; i < GFX_BUFFER_COUNT; i++) {
        gfx_clear(&g, BG);
        gfx_present(&g, bootFrameID++);
    }

    // Start the freeze watchdog (auto-recovers a frozen app instead of a reboot).
    g_heartbeat = sceKernelGetProcessTime();
    OrbisPthread wd;
    g_mainTh = scePthreadSelf();
    aseg_init();      // before httpd/playback: the fetch lock must exist before any thread can race to create it
    scePthreadCreate(&wd, NULL, watchdog_main, NULL, "ps4cast_wd");

#ifdef BOOT_MINIMAL
    int frameID = bootFrameID;
    for (;;) {
        draw_lobby(&g, "0.0.0.0", 0);
        text_centered(&g, 735, "Minimal build: graphics only, no network, no player.", 3, MUTED);
        gfx_present(&g, frameID++);
    }
#else
    int net_ok = (net_init() == 0);
    char ip[32] = "0.0.0.0";
    if (net_ok && net_get_ip(ip, sizeof(ip)) != 0)
        net_ok = 0;

    if (net_ok) {
        httpd_start(PORT);
        // SSDP discovery responder. The earlier in-app SSDP builds (01.50/01.51)
        // destabilized boot because the socket never joined the multicast group
        // and the receive loop busy-spun on error. Both are fixed in ssdp.c, so
        // the PS4 can again advertise itself as a DLNA renderer to cast apps.
        ssdp_start(ip, PORT);
        // Explicit "ready" toast: tells the user (and the deploy script's /status
        // poll) the app is fully up and accepting casts — no more guessing the gap.
        notify("PS4 Cast " APP_VER " ready  -  http://%s:%d", ip, PORT);
    }

    char url[2048];
    int frameID = bootFrameID;
    int everDrew = 0;
    int running = 1;
    uint64_t hudUntil = 0;
    uint64_t channelBannerUntil = 0;  // compact feedback after trigger zapping
    int statsOn = 0;                  // touchpad-toggled stream stats overlay
    double netBps = 0;                // sampled download throughput (bytes/sec, smoothed)
    uint64_t rxT0 = 0, rxB0 = 0;      // throughput sampling anchor
    int fpsCount = 0, fpsVal = 0; uint64_t fpsT0 = 0;
    unsigned fpsSeenGen = player_present_generation();
    char lastUrl[1024] = "";          // resume: track the currently playing URL
    int resumePending = 0;            // resume: seek to saved position once dur known
    uint64_t resumeDeadline = 0, lastResumeSave = 0;
    int liveSource = 0;               // current source has no finite duration (live)
    int reconnecting = 0, reconnects = 0;   // auto-reconnect a dropped live stream
    uint64_t reconnectAt = 0, healthySince = 0;
    const int MAX_RECONNECT = 30;     // ~give up after this many attempts
    uint64_t noUserSince = 0;         // first time we saw NO valid signed-in user (debounce)
    uint64_t scrubStart = 0, scrubStep = 0;   // continuous-scrub timing
    int homeSel = 0, railSel = 0, inChannels = 0;  // home browser: bouquet level -> channel level
    int guideOpen = 0;
    HomeMode homeMode = home_mode_load();
    PlaybackOrigin playbackOrigin = PLAYBACK_CAST;
    PadState pad;
    pad_init(&pad);
    sys_diag_update();                // prime the user/system snapshot before the loop reads it

    while (running) {
        g_heartbeat = sceKernelGetProcessTime();   // pet the freeze watchdog each frame
        g_wdBusy = 0;                               // loop is alive again -> back to the strict 15s grace
        uint64_t now = sceKernelGetProcessTime();
        uint32_t pressed = pad_poll(&pad);
        uint32_t held = pad_held(&pad);

        // A valid PS4 user must be signed in. With an ANONYMOUS foreground user
        // (userId=0xffffffff -> sys_fg_user() <= 0) the system's VideoPlayingChecker
        // crashes SceShellUI (the compositor) -> CE-36329-3 -> our display is
        // orphaned while we keep running blind. So gate all video activity on it.
        // sys_fg_user() is refreshed ~1 Hz by sys_diag_update() below.
        int userOk = (sys_fg_user() > 0);
        if (userOk) noUserSince = 0;
        else if (noUserSince == 0) noUserSince = now;

        int nch = httpd_chan_count();
        // Touchpad toggles the lightweight stream-stats overlay.
        if (pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) statsOn = !statsOn;

        // Sample download throughput (~2.5 Hz) and presented-frame rate (1 Hz).
        // Short 400ms window so the on-screen speed updates in near-real-time;
        // EMA smooths the burstiness of HLS segment fetches without lagging a
        // whole second behind like the old 1s window did.
        {
            uint64_t rx = player_rx_total();
            if (rxT0 == 0) { rxT0 = now; rxB0 = rx; }
            else if (now - rxT0 >= 400000ULL) {               // 400ms window: live-feel updates
                double dt = (double)(now - rxT0) / 1e6;
                double db = (rx >= rxB0) ? (double)(rx - rxB0) : 0;   // 0 across a new stream
                double inst = db / dt;                               // bytes/sec this window
                netBps = netBps > 0 ? (netBps * 0.6 + inst * 0.4) : inst;   // responsive smoothing
                rxT0 = now; rxB0 = rx;
            }
            if (fpsT0 == 0) fpsT0 = now;
            else if (now - fpsT0 >= 1000000ULL) {
                fpsVal = fpsCount; fpsCount = 0; fpsT0 = now;
                sys_set_fps(fpsVal); // expose presented fps via /status (it2 measurement)
                sys_diag_update();   // sample system/user state ~1 Hz (SceShellUI-crash probe)
            }
        }

        // ---- resume: remember VOD position; seek back to it on replay --------
        if (!player_started()) {
            lastUrl[0] = '\0';
        } else {
            const char *lp = httpd_last_push();
            if (lp && lp[0] && strcmp(lp, lastUrl) != 0) {         // new content started
                strncpy(lastUrl, lp, sizeof(lastUrl) - 1); lastUrl[sizeof(lastUrl) - 1] = '\0';
                resumePending = 1; resumeDeadline = now + 10000000ULL;
            }
            if (resumePending) {
                double rc2 = 0, rd = 0; player_progress(&rc2, &rd);
                if (rd > 30) {                                    // seekable VOD with known length
                    int rp = httpd_resume_get(lastUrl);
                    if (rp > 5 && rp < (int)rd - 15) { player_seek((double)rp); notify("Resumed at %d:%02d", rp / 60, rp % 60); }
                    resumePending = 0;
                } else if (now > resumeDeadline) {
                    resumePending = 0;                            // live / no duration -> nothing to resume
                }
            } else if (now - lastResumeSave > 10000000ULL) {      // checkpoint every 10s
                double sc = 0, sd = 0; player_progress(&sc, &sd);
                if (sd > 0) httpd_resume_save(lastUrl, (int)sc, (int)sd);
                lastResumeSave = now;
            }
        }

        // ---- idle mode switch + Live TV home browser --------------------------
        if (!player_started() && pressed) {
            if ((pressed & ORBIS_PAD_BUTTON_L1) && homeMode != HOME_CAST) {
                homeMode = HOME_CAST; home_mode_save(homeMode);
            }
            if ((pressed & ORBIS_PAD_BUTTON_R1) && homeMode != HOME_IPTV) {
                homeMode = HOME_IPTV; home_mode_save(homeMode);
            }

            if (homeMode == HOME_IPTV && nch > 0) {
                if (!inChannels) {
                    int rn = httpd_chan_rail_count();
                    if (pressed & ORBIS_PAD_BUTTON_UP)   railSel = (railSel - 1 + rn) % rn;
                    if (pressed & ORBIS_PAD_BUTTON_DOWN) railSel = (railSel + 1) % rn;
                    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                        httpd_chan_rail_select(railSel);
                        inChannels = 1; homeSel = 0;
                    }
                } else {
                    int shown = httpd_chan_filter_count();
                    if (shown > 0) {
                        if (pressed & ORBIS_PAD_BUTTON_UP)   homeSel = (homeSel - 1 + shown) % shown;
                        if (pressed & ORBIS_PAD_BUTTON_DOWN) homeSel = (homeSel + 1) % shown;
                    }
                    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) inChannels = 0;
                    if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                        int abs = httpd_chan_filter_abs(homeSel);
                        if (abs >= 0) {
                            httpd_chan_toggle_fav(abs);
                            notify(httpd_chan_is_fav(abs) ? "Added to favourites" : "Removed from favourites");
                        }
                    }
                    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                        int abs = httpd_chan_filter_abs(homeSel);
                        char curl[1024];
                        if (abs >= 0 && httpd_chan_get(abs, NULL, 0, curl, sizeof(curl))) {
                            httpd_chan_set_current(abs);
                            playbackOrigin = PLAYBACK_IPTV;
                            guideOpen = 0;
                            player_play(curl);
                            everDrew = 0; reconnecting = 0; reconnects = 0;
                            hudUntil = now + 5000000ULL;
                        }
                    }
                }
                // Triggers step bouquets while browsing, without changing modes.
                if (pressed & (ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2)) {
                    int rn = httpd_chan_rail_count();
                    railSel = (railSel + ((pressed & ORBIS_PAD_BUTTON_R2) ? 1 : rn - 1)) % rn;
                    httpd_chan_rail_select(railSel);
                    homeSel = 0;
                }
            }
        }

        // ---- IPTV playback: one guide state + distinct quick-zap controls ------
        int tuneAbs = -1;
        if (player_started() && playbackOrigin == PLAYBACK_IPTV && nch > 0) {
            if (!guideOpen && (pressed & ORBIS_PAD_BUTTON_DOWN)) {
                guideOpen = 1;
                {
                    int p = chan_filtered_pos(httpd_chan_current());
                    if (p < 0) {
                        railSel = 0; httpd_chan_rail_select(railSel);
                        p = chan_filtered_pos(httpd_chan_current());
                    }
                    homeSel = p >= 0 ? p : 0;
                }
                pressed &= ~ORBIS_PAD_BUTTON_DOWN;
                hudUntil = 0;
            }

            if (guideOpen) {
                int shown = httpd_chan_filter_count();
                if (shown > 0) {
                    if (pressed & ORBIS_PAD_BUTTON_UP) homeSel = (homeSel - 1 + shown) % shown;
                    if (pressed & ORBIS_PAD_BUTTON_DOWN) homeSel = (homeSel + 1) % shown;
                    if (pressed & ORBIS_PAD_BUTTON_L1) homeSel = (homeSel - 8 + shown * 8) % shown;
                    if (pressed & ORBIS_PAD_BUTTON_R1) homeSel = (homeSel + 8) % shown;
                }
                if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                    int rn = httpd_chan_rail_count();
                    railSel = (railSel + ((pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : rn - 1)) % rn;
                    httpd_chan_rail_select(railSel);
                    homeSel = 0;
                }
                if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                    int abs = httpd_chan_filter_abs(homeSel);
                    if (abs >= 0) {
                        httpd_chan_toggle_fav(abs);
                        notify(httpd_chan_is_fav(abs) ? "Added to favourites" : "Removed from favourites");
                    }
                }
                if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                    tuneAbs = httpd_chan_filter_abs(homeSel);
                    guideOpen = 0;
                }
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) guideOpen = 0;
                pressed &= ~(ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN |
                             ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT |
                             ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 |
                             ORBIS_PAD_BUTTON_SQUARE | ORBIS_PAD_BUTTON_CROSS |
                             ORBIS_PAD_BUTTON_CIRCLE);
            } else {
                // Up is a non-destructive information peek. Down opened the guide.
                if (pressed & ORBIS_PAD_BUTTON_UP) {
                    channelBannerUntil = now + 3500000ULL;
                    pressed &= ~ORBIS_PAD_BUTTON_UP;
                }
                // L1/R1: previous/next channel within the current bouquet.
                if (pressed & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1)) {
                    int shown = httpd_chan_filter_count();
                    int pos = chan_filtered_pos(httpd_chan_current());
                    if (shown <= 0 || pos < 0) {
                        railSel = 0; httpd_chan_rail_select(railSel);
                        shown = httpd_chan_filter_count(); pos = chan_filtered_pos(httpd_chan_current());
                    }
                    if (shown > 0) {
                        if (pos < 0) pos = 0;
                        homeSel = (pos + ((pressed & ORBIS_PAD_BUTTON_R1) ? 1 : shown - 1)) % shown;
                        tuneAbs = httpd_chan_filter_abs(homeSel);
                    }
                    pressed &= ~(ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1);
                }
                // L2/R2: previous/next non-empty bouquet. It tunes one channel and
                // shows only the compact banner, never the full guide.
                if (pressed & (ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2)) {
                    int rn = httpd_chan_rail_count();
                    int dir = (pressed & ORBIS_PAD_BUTTON_R2) ? 1 : -1;
                    for (int tries = 0; tries < rn; tries++) {
                        railSel = (railSel + (dir > 0 ? 1 : rn - 1)) % rn;
                        httpd_chan_rail_select(railSel);
                        if (httpd_chan_filter_count() > 0) {
                            homeSel = 0; tuneAbs = httpd_chan_filter_abs(0); break;
                        }
                    }
                    pressed &= ~(ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2);
                }
            }

            if (tuneAbs >= 0) {
                if (tuneAbs != httpd_chan_current()) {
                    char curl[1024];
                    if (httpd_chan_get(tuneAbs, NULL, 0, curl, sizeof(curl))) {
                        int wasPlaying = player_started() && everDrew;
                        httpd_chan_set_current(tuneAbs);
                        int rc = player_play(curl);
                        if (rc != 0 || !wasPlaying) everDrew = 0;
                        reconnecting = 0; reconnects = 0;
                    }
                }
                channelBannerUntil = now + 3500000ULL;
                hudUntil = 0;
            }
        }

        // In casting mode Down only reveals/hides the lightweight HUD. A loaded
        // IPTV playlist can never steal the cast transport controls.
        if (player_started() && playbackOrigin == PLAYBACK_CAST &&
            (pressed & ORBIS_PAD_BUTTON_DOWN)) {
            hudUntil = now < hudUntil ? 0 : now + 5000000ULL;
            pressed &= ~ORBIS_PAD_BUTTON_DOWN;
        }

        // ---- casting-mode continuous scrub -----------------------------------
        // Holding a seek button sweeps a preview target (accelerating for L1/R1,
        // fine for Left/Right) and commits ONE seek on release.
        if (player_started() && playbackOrigin == PLAYBACK_CAST) {
            double sc = 0, sd = 0; player_progress(&sc, &sd);
            const uint32_t SEEKB = ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 |
                                   ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT;
            uint32_t hs = held & SEEKB;
            if (hs && sd > 0) {
                if (!g_scrubActive) { g_scrubActive = 1; g_scrubTarget = sc; scrubStart = now; scrubStep = 0; }
                if (now - scrubStep >= 90000ULL) {          // ~11 steps/sec
                    double heldSec = (double)(now - scrubStart) / 1e6;
                    double step;
                    if (hs & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1))
                        step = 15.0 + heldSec * 60.0;       // coarse, accelerates the longer you hold
                    else
                        step = 1.0;                          // fine (Left/Right)
                    if (hs & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_LEFT)) step = -step;
                    g_scrubTarget += step;
                    if (g_scrubTarget < 0) g_scrubTarget = 0;
                    if (g_scrubTarget > sd) g_scrubTarget = sd;
                    scrubStep = now;
                }
                hudUntil = now + 2000000ULL;                 // keep the scrubber on screen
            } else if (g_scrubActive) {
                g_scrubActive = 0;
                player_seek(g_scrubTarget);                  // commit exactly one seek
                hudUntil = now + 3000000ULL;
            }
        } else if (g_scrubActive) {
            g_scrubActive = 0;
        }

        if (player_started() && pressed) {
            hudUntil = sceKernelGetProcessTime() + 5000000ULL;
            if (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_OPTIONS)) {
                player_pause(!player_is_paused());
                hudUntil = sceKernelGetProcessTime() + 9000000ULL;
            }
            if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                if (playbackOrigin == PLAYBACK_IPTV && httpd_chan_current() >= 0) {
                    int c = httpd_chan_current(); httpd_chan_toggle_fav(c);
                    notify(httpd_chan_is_fav(c) ? "Added to favourites" : "Removed from favourites");
                } else {
                    player_seek(0); notify("Restarted from the beginning");
                }
            }
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                player_stop();
                guideOpen = 0;
                everDrew = 0; reconnecting = 0; reconnects = 0;
            }
        }

        // In-app EXIT: TRIANGLE closes PS4 Cast back to the home menu via the
        // clean LoadExec("exit") teardown below — works whether idle or playing,
        // and never raises the system crash dialog. (Deliberate face button so it
        // isn't hit by accident while seeking/pausing.)
        if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
            player_stop();
            running = 0;
            break;
        }

        if (httpd_take_quit_request()) {
            player_stop();
            running = 0;
            break;
        }
        if (httpd_take_stop_request()) {
            player_stop();
            guideOpen = 0;
            everDrew = 0; reconnecting = 0; reconnects = 0;
        }
        if (httpd_take_play_request(url, sizeof(url))) {
            playbackOrigin = PLAYBACK_CAST;
            homeMode = HOME_CAST;
            guideOpen = 0;
            httpd_chan_set_current(-1);
            if (!userOk) {
                notify("Sign in a PS4 user to cast");   // playing under ANONYMOUS crashes SceShellUI
            } else {
                int wasPlaying = player_started() && everDrew;
                player_set_startup_headstart(1);
                int rc = player_play(url);
                if (rc != 0 || !wasPlaying) everDrew = 0; // hold old frame across the switch
                reconnecting = 0; reconnects = 0;
                hudUntil = sceKernelGetProcessTime() + 6000000ULL;
            }
        }
        int playerRequestKind = httpd_take_player_request(url, sizeof(url));
        if (playerRequestKind) {
            playbackOrigin = playerRequestKind == 2 ? PLAYBACK_IPTV : PLAYBACK_CAST;
            homeMode = playbackOrigin == PLAYBACK_IPTV ? HOME_IPTV : HOME_CAST;
            guideOpen = 0;
            if (playbackOrigin == PLAYBACK_IPTV) {
                railSel = chan_rail_for_abs(httpd_chan_current());
                httpd_chan_rail_select(railSel);
                homeSel = chan_filtered_pos(httpd_chan_current());
                if (homeSel < 0) homeSel = 0;
                inChannels = 1;
            } else {
                httpd_chan_set_current(-1);
            }
            if (!userOk) {
                notify("Sign in a PS4 user to cast");
            } else {
                int wasPlaying = player_started() && everDrew;
                player_set_startup_headstart(1);
                int rc = player_play(url);
                if (rc != 0 || !wasPlaying) everDrew = 0; // hold old frame across the switch
                reconnecting = 0; reconnects = 0;
                hudUntil = sceKernelGetProcessTime() + 6000000ULL;
            }
        }

        // Fail-closed: if we're trying to show video with NO valid signed-in user
        // for a sustained window, the compositor (SceShellUI) has almost certainly
        // crashed on our ANONYMOUS user and our display is orphaned — we'd be
        // "playing" blind (the false positive). Close cleanly to the home menu via
        // the LoadExec teardown below instead of zombie-ing in the background. The
        // 8s debounce clears the brief ANONYMOUS window during the launch user-switch.
        if (player_started() && !userOk && noUserSince && (now - noUserSince > 8000000ULL)) {
            player_stop();
            running = 0;
            break;
        }

        // Autoplay / EOF cleanup: once the decoder reports inactive, either
        // advance to the queued item or fully tear down the finished playback.
        // Playback went inactive (EOF / dropped stream). Advance the queue, or —
        // for a LIVE source that dropped — begin auto-reconnect instead of dying.
        if (!reconnecting && player_started() && !player_is_active()) {
            healthySince = 0;
            if (httpd_take_next(url, sizeof(url))) {
                player_play(url);
                everDrew = 0; reconnecting = 0; reconnects = 0;
                hudUntil = sceKernelGetProcessTime() + 6000000ULL;
            } else if (liveSource && lastUrl[0]) {
                reconnecting = 1; reconnects = 0; reconnectAt = now + 800000ULL;
            } else {
                player_stop();
                everDrew = 0;
            }
        }
        // Reconnect driver: retry the same live URL with capped exponential backoff
        // until frames resume (clears `reconnecting`) or we exhaust the budget.
        if (reconnecting) {
            if (reconnects >= MAX_RECONNECT) {
                reconnecting = 0; player_stop(); everDrew = 0;
                notify("Stream lost - tap a channel to retry");
            } else if (now >= reconnectAt) {
                reconnects++;
                player_play(lastUrl);
                uint64_t s = reconnects < 4 ? (uint64_t)reconnects : 4;   // 1,2,3,4,4.. *2s
                reconnectAt = now + (s * 2000000ULL);
            }
        }

        // Keep the system's inactivity timer at bay EVERY frame, even when idle.
        // Previously PowerTick ran only while playing, so an idle app stopped
        // telling the system it was alive -> after the no-input timeout the
        // system screensavered/suspended it and the homebrew died (the recurring
        // "idle-death"). Ticking unconditionally keeps the lobby alive too.
        sceSystemServicePowerTick();

        if (player_started()) {
            // Only poke the system "video playing" notifier with a VALID user —
            // ticking it under an ANONYMOUS user is what drives SceShellUI's
            // VideoPlayingChecker into its Invalid-User-Id crash.
            if (userOk) sceSystemServiceTickVideoPlayback();
            int drew = player_render(&g);   // always pump frames while started
            if (drew) {
                everDrew = 1;
                unsigned shown = player_present_generation();
                if (shown != fpsSeenGen) { fpsCount++; fpsSeenGen = shown; }
                if (reconnecting) reconnecting = 0;          // recovered
                if (healthySince == 0) healthySince = now;
                else if (now - healthySince > 8000000ULL) reconnects = 0;  // stable -> reset budget
                liveSource = player_is_live();   // only true live streams auto-reconnect
            } else if (!everDrew) {
                gfx_vgrad(&g, 0, 0, g.width, g.height, BG_TOP, BG_BOT);
                int pw = 720, ph = 264, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
                panel(&g, px, py, pw, ph, 24, SURF, 235);
                icon_cast(&g, g.width / 2, py + 84, 96);
                ctext(&g, py + 156, "Connecting...", 4, TXT, 0);
                char st[200];
                snprintf(st, sizeof(st), "%s", player_status());
                ctext(&g, py + 212, st, 2, MUT, 0);
            }
            // if !drew && everDrew: keep the previous frame (no black flicker)

            // Mid-playback stall: overlay a buffering panel on the held frame so
            // it's clear what's happening, with a live buffer gauge + controls.
            if (everDrew && player_buffering()) {
                int pw = 560, ph = 188, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
                panel(&g, px, py, pw, ph, 22, INK, 225);
                char b[80];
                snprintf(b, sizeof(b), "Buffering  %d%%", player_buffer_pct());
                ctext(&g, py + 38, b, 4, TXT, 0);
                int gx = px + 50, gy = py + 106, gw = pw - 100, gh = 10;
                gfx_round(&g, gx, gy, gw, gh, gh / 2, SURF2);
                int fillw = gw * player_buffer_pct() / 100; if (fillw < 0) fillw = 0; if (fillw > gw) fillw = gw;
                if (fillw > gh) gfx_round(&g, gx, gy, fillw, gh, gh / 2, ACCENT);
                ctext(&g, py + 140,
                      playbackOrigin == PLAYBACK_IPTV ? "Circle  stop      Down  channel guide"
                                                      : "Circle  stop      Left  seek back",
                      2, MUT, 0);
                hudUntil = sceKernelGetProcessTime() + 2000000ULL;  // keep HUD visible too
            }

            // The guide replaces the HUD while browsing, so only one control
            // surface is ever visible over playback.
            if (!guideOpen && (!everDrew || player_is_paused() || sceKernelGetProcessTime() < hudUntil)) {
                draw_hud(&g, playbackOrigin);
                player_request_bar_clear();   // HUD scrubber/times sit on the bottom bar
            }
        } else if (!reconnecting) {
            if (homeMode == HOME_IPTV)
                draw_channel_home(&g, inChannels ? homeSel : railSel, railSel, inChannels, ip, net_ok);
            else
                draw_lobby(&g, ip, net_ok);
            everDrew = 0;
        }
        // (while reconnecting with no live player, the last frame is kept on screen)

        if (guideOpen && player_started() && playbackOrigin == PLAYBACK_IPTV) {
            draw_channel_guide(&g, homeSel, railSel);
            player_request_bar_clear();
        } else if (player_started() && playbackOrigin == PLAYBACK_IPTV &&
                   sceKernelGetProcessTime() < channelBannerUntil) {
            draw_channel_banner(&g);
            player_request_bar_clear();
        }

        // Auto-reconnect banner for a dropped live stream.
        if (reconnecting) {
            int pw = 580, ph = 160, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
            panel(&g, px, py, pw, ph, 22, INK, 225);
            char b[80]; snprintf(b, sizeof(b), "Reconnecting...  (%d)", reconnects);
            ctext(&g, py + 44, b, 4, TXT, 0);
            ctext(&g, py + 100, "live stream dropped - retrying", 2, MUT, 0);
            hudUntil = now + 1500000ULL;
        }

        // No PS4 user signed in: casting is disabled (it would crash SceShellUI),
        // so make the reason explicit on the lobby. Debounced past the brief
        // launch user-switch; only while idle (a playing session that loses its
        // user is handled by the fail-closed exit above).
        if (!userOk && noUserSince && (now - noUserSince > 1500000ULL) && !player_started()) {
            int pw = 780, ph = 200, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
            panel(&g, px, py, pw, ph, 22, INK, 235);
            ctext(&g, py + 58, "Sign in a PS4 user", 4, TXT, 0);
            ctext(&g, py + 120, "Casting stays off until a user is signed in", 2, MUT, 0);
        }

        // Lightweight stream stats (touchpad), top-right, only while playing.
        if (statsOn && player_started()) {
            draw_stats_overlay(&g, netBps, fpsVal);
            player_request_bar_clear();   // stats top rows sit on the top bar
        }

        gfx_present(&g, frameID++);
    }
    player_stop();
    audio_shutdown();
    // Clean close: returning from main / _exit can be read by the system as an
    // abnormal termination and pop the "application closed" crash dialog. LoadExec
    // ("exit") is the recognized normal app-exit path → returns to the home menu
    // with no dialog. Fall back to _exit only if it somehow returns.
    sceSystemServiceLoadExec("exit", NULL);
    _exit(0);
#endif
    return 0;
}
