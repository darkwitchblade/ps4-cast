// PS4 Cast — homebrew media casting receiver.
//   * brings up the framebuffer + network
//   * shows a lobby screen with the console's cast URL
//   * runs an HTTP control server (phone web UI posts a video URL)
//   * plays the URL fullscreen via libSceAvPlayer (hardware decode)
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "qr.h"

#ifndef BOOT_MINIMAL
#include "netutil.h"
#include "httpd.h"
#include "player.h"
#include "httpsrc.h"
#include "escalate.h"
#include "launcher.h"
#include "ssdp.h"
#include "pad_diag.h"
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

// Persist a one-line crash/hang note to /data (read back via GET /crashlog).
// Uses raw syscalls so it's safe from a signal handler.
static void persist_crash(const char *buf, int n) {
    int fd = sceKernelOpen("/data/ps4cast_crash.log", 0x0201 /*WRONLY|CREAT*/ | 0x0400 /*TRUNC*/, 0666);
    if (fd >= 0) { sceKernelWrite(fd, buf, (size_t)n); sceKernelClose(fd); }
}

static void fatal_signal(int sig, struct __siginfo *info, void *uap) {
    (void)uap;
    char b[160];
    unsigned long a = info ? (unsigned long)info->si_addr : 0;
    int n = snprintf(b, sizeof(b), "CRASH v" APP_VER " sig=%d addr=0x%lx\n", sig, a);
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
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        sceKernelUsleep(2 * 1000 * 1000);                 // poll every 2s
        uint64_t hb = g_heartbeat;
        if (hb == 0) continue;                            // main loop not running yet
        uint64_t now = sceKernelGetProcessTime();
        if (now > hb && (now - hb) > 15ULL * 1000 * 1000) { // ~15s with zero progress = frozen
            // Release the display/GPU FIRST so the exit is reclaimable (not
            // unkillable) — do this before the crash-log write, which could
            // itself stall on a frozen /data mount and gate the recovery.
            gfx_emergency_release();
            char b[96];
            int n = snprintf(b, sizeof(b), "HANG v" APP_VER " watchdog stale=%llums\n",
                             (unsigned long long)((now - hb) / 1000));
            persist_crash(b, n);                          // record the hang for /crashlog
            _exit(0);                                     // force full exit; user just reopens
        }
    }
    return NULL;
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
static const GfxColor DANGER = { 0xff, 0x5d, 0x7a };
static const GfxColor WARN   = { 0xff, 0xc4, 0x4a };
static const GfxColor TXT    = { 0xf3, 0xf6, 0xff };
static const GfxColor MUT    = { 0x9a, 0xa4, 0xc8 };
static const GfxColor FAINT  = { 0x6b, 0x73, 0x98 };
static const GfxColor INK    = { 0x07, 0x0a, 0x14 };
static const GfxColor PAPER  = { 0xf4, 0xf7, 0xff };
static const GfxColor BLACK  = { 0x00, 0x00, 0x00 };
static const GfxColor BTN_X  = { 0x86, 0xa9, 0xff };
static const GfxColor BTN_O  = { 0xff, 0x73, 0x88 };
static const GfxColor BTN_T  = { 0x44, 0xe0, 0xa6 };
// Kept for the (compiled-out) BOOT_MINIMAL diagnostic path.
static const GfxColor WHITE  = { 0xf3, 0xf6, 0xff };
static const GfxColor MUTED  = { 0x9a, 0xa4, 0xc8 };

static void text_centered(Gfx *g, int cy, const char *s, int scale, GfxColor c) {
    int w = gfx_text_w(s, scale);
    gfx_text(g, (g->width - w) / 2, cy, s, scale, c);
}

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
static void thick_line(Gfx *g, float x0, float y0, float x1, float y1, float th, GfxColor c) {
    float dx = x1 - x0, dy = y1 - y0, len = __builtin_sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * (th / 2), ny = dx / len * (th / 2);
    gfx_tri(g, (int)(x0 + nx), (int)(y0 + ny), (int)(x0 - nx), (int)(y0 - ny), (int)(x1 + nx), (int)(y1 + ny), c);
    gfx_tri(g, (int)(x0 - nx), (int)(y0 - ny), (int)(x1 + nx), (int)(y1 + ny), (int)(x1 - nx), (int)(y1 - ny), c);
}
static void gtext(Gfx *g, int x, int y, const char *s, int sc, GfxColor c, int tr) {
    if (sc >= 4) gfx_text_tr(g, x + 2, y + 2, s, sc, INK, tr);
    gfx_text_tr(g, x, y, s, sc, c, tr);
}
static void ctext(Gfx *g, int cy, const char *s, int sc, GfxColor c, int tr) {
    int w = gfx_text_tr_w(s, sc, tr);
    gtext(g, (g->width - w) / 2, cy, s, sc, c, tr);
}
static void panel(Gfx *g, int x, int y, int w, int h, int r, GfxColor c, int a) {
    gfx_round_a(g, x, y, w, h, r, c, a);
    gfx_rect_a(g, x + r, y, w - 2 * r, 1, HAIR, 36);   // top hairline highlight
}
static void icon_play(Gfx *g, int cx, int cy, int s, GfxColor c) {
    gfx_tri(g, cx - (int)(s * 0.28f), cy - (int)(s * 0.5f),
               cx - (int)(s * 0.28f), cy + (int)(s * 0.5f),
               cx + (int)(s * 0.50f), cy, c);
}
static void icon_pause(Gfx *g, int cx, int cy, int s, GfxColor c) {
    int bw = (int)(s * 0.28f), bh = s, gap = (int)(s * 0.26f), r = bw / 2;
    gfx_round(g, cx - gap / 2 - bw, cy - bh / 2, bw, bh, r, c);
    gfx_round(g, cx + gap / 2, cy - bh / 2, bw, bh, r, c);
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
static void btn_cross(Gfx *g, int cx, int cy, int r, GfxColor c) {
    float t = r * 0.42f;
    thick_line(g, cx - r * 0.7f, cy - r * 0.7f, cx + r * 0.7f, cy + r * 0.7f, t, c);
    thick_line(g, cx - r * 0.7f, cy + r * 0.7f, cx + r * 0.7f, cy - r * 0.7f, t, c);
}
static void btn_circle(Gfx *g, int cx, int cy, int r, GfxColor c) {
    int th = (int)(r * 0.42f); if (th < 2) th = 2;
    for (int q = 0; q < 4; q++) gfx_arc(g, cx, cy, r, th, q, c);
}
static void btn_triangle(Gfx *g, int cx, int cy, int r, GfxColor c) {
    float t = r * 0.40f;
    float ax = cx, ay = cy - r;
    float bx = cx - r * 0.92f, by = cy + r * 0.75f;
    float dx = cx + r * 0.92f, dy = cy + r * 0.75f;
    thick_line(g, ax, ay, bx, by, t, c);
    thick_line(g, bx, by, dx, dy, t, c);
    thick_line(g, dx, dy, ax, ay, t, c);
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
// control hint: PS4 button glyph + label. kinds: 0 cross,1 circle,2 triangle,3 seek
static int legend(Gfx *g, int x, int cy, int kind, const char *label) {
    int r = 13, gx = x + r;
    if (kind == 0) btn_cross(g, gx, cy, r, BTN_X);
    else if (kind == 1) btn_circle(g, gx, cy, r, BTN_O);
    else if (kind == 2) btn_triangle(g, gx, cy, r, BTN_T);
    else {
        gfx_tri(g, gx + 4, cy, gx + 12, cy - 7, gx + 12, cy + 7, MUT);
        gfx_tri(g, gx - 4, cy, gx - 12, cy - 7, gx - 12, cy + 7, MUT);
    }
    gfx_text(g, x + r * 2 + 12, cy - 8, label, 2, MUT);
    return r * 2 + 12 + gfx_text_w(label, 2) + 40;
}
static void draw_legend_row(Gfx *g, int cx, int ly) {
    struct { int k; const char *l; } items[] = { {0,"Pause"},{1,"Stop"},{3,"Seek"},{2,"Exit"} };
    int total = 0;
    for (int i = 0; i < 4; i++) total += 13 * 2 + 12 + gfx_text_w(items[i].l, 2) + 40;
    int lx = cx - (total - 40) / 2;
    for (int i = 0; i < 4; i++) lx += legend(g, lx, ly, items[i].k, items[i].l);
}

static void draw_lobby(Gfx *g, const char *ip, int net_ok) {
    gfx_vgrad(g, 0, 0, g->width, g->height, BG_TOP, BG_BOT);
    gfx_rect_a(g, 0, 0, g->width, 360, ACCENT, 14);
    int W = g->width;

    // brand row, centered: logo tile + wordmark
    int track = 3, ws = 6;
    const char *wm = "PS4 Cast";
    int ww = gfx_text_tr_w(wm, ws, track);
    int box = 92, group = box + 26 + ww, gx = (W - group) / 2, brandCy = 150;
    icon_cast(g, gx + box / 2, brandCy, box);
    gtext(g, gx + box + 26, brandCy - (ws * 8) / 2, wm, ws, TXT, track);
    ctext(g, 250, "WIRELESS CAST RECEIVER", 2, FAINT, 6);

    if (net_ok) {
        char url[80];
        snprintf(url, sizeof(url), "http://%s:%d", ip, PORT);
        draw_qr_card(g, url, W / 2, 320, 9);
        int below = 320 + ((QR_SIZE + 6) * 9 + 60) + 40;
        ctext(g, below, "Scan with your phone to open the controls", 3, MUT, 1);

        int uw = gfx_text_tr_w(url, 4, 1);
        int pw = uw + 64, ph = 64, px = (W - pw) / 2, py = below + 52;
        panel(g, px, py, pw, ph, ph / 2, SURF2, 235);
        gfx_circle(g, px + 30, py + ph / 2, 6, LIVE);
        gtext(g, px + 52, py + (ph - 32) / 2, url, 4, TXT, 1);
        ctext(g, py + ph + 34, "or cast from any DLNA / UPnP app on your network", 2, FAINT, 1);
    } else {
        ctext(g, 470, "No network connection", 5, TXT, 1);
        ctext(g, 560, "Connect the PS4 to Wi-Fi or LAN, then relaunch.", 3, MUT, 1);
    }

    // status chip
    char st[160];
#ifdef BOOT_MINIMAL
    snprintf(st, sizeof(st), "minimal boot diagnostic");
#else
    snprintf(st, sizeof(st), "%s", player_status());
#endif
    int sw = gfx_text_w(st, 2) + 60, sx = (W - sw) / 2, sy = 884;
    panel(g, sx, sy, sw, 44, 22, SURF, 220);
    gfx_circle(g, sx + 26, sy + 22, 5, net_ok ? LIVE : FAINT);
    gfx_text(g, sx + 42, sy + 14, st, 2, MUT);

    draw_legend_row(g, W / 2, 984);
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

static void draw_hud(Gfx *g) {
    double cur = 0, dur = 0;
    player_progress(&cur, &dur);
    int paused = player_is_paused();

    int W = g->width, H = 224, y = g->height - H - 36, x = 40, w = W - 80;
    panel(g, x, y, w, H, 24, INK, 200);
    int pad = 34;

    // now-playing badge + title
    int bx = x + pad, by = y + 26, bs = 56;
    panel(g, bx, by, bs, bs, 14, SURF2, 255);
    if (paused) icon_pause(g, bx + bs / 2, by + bs / 2, 22, ACC_LT);
    else icon_play(g, bx + bs / 2 + 2, by + bs / 2, 24, ACC_LT);

    char title[160];
    basename_of(httpd_last_push(), title, sizeof(title));
    gtext(g, bx + bs + 20, by + 2, title, 3, TXT, 0);
    gfx_text(g, bx + bs + 20, by + 32, paused ? "Paused" : player_status(), 2, MUT);

    // state pill
    const char *badge = paused ? "PAUSED" : "PLAYING";
    int pw = gfx_text_w(badge, 2) + 44, px = x + w - pad - pw, py = by + 8;
    panel(g, px, py, pw, 34, 17, SURF2, 255);
    gfx_circle(g, px + 20, py + 17, 5, paused ? FAINT : LIVE);
    gfx_text(g, px + 34, py + 9, badge, 2, paused ? MUT : LIVE);

    // progress bar with knob
    char curS[24], durS[24];
    fmt_time(cur, curS, sizeof(curS));
    fmt_time(dur, durS, sizeof(durS));
    int barX = x + pad, barY = y + 118, barW = w - pad * 2, barH = 8;
    gfx_round(g, barX, barY, barW, barH, barH / 2, SURF2);
    float p = dur > 0 ? (float)(cur / dur) : 0; if (p < 0) p = 0; if (p > 1) p = 1;
    int fw = (int)(barW * p);
    if (fw > barH) gfx_round(g, barX, barY, fw, barH, barH / 2, ACCENT);
    gfx_circle(g, barX + fw, barY + barH / 2, 11, TXT);
    gfx_circle(g, barX + fw, barY + barH / 2, 5, ACCENT);
    gfx_text(g, barX, barY + 24, curS, 2, MUT);
    gfx_text(g, barX + barW - gfx_text_w(durS, 2), barY + 24, durS, 2, MUT);

    draw_legend_row(g, W / 2, y + H - 30);
}

// TV-box-style channel list overlay: a fast, scrollable list of the loaded
// playlist with the highlighted selection and a live marker on the tuned one.
static void draw_channel_overlay(Gfx *g, int sel) {
    int n = httpd_chan_count();
    if (n <= 0) return;
    int cur = httpd_chan_current();
    if (sel < 0) sel = cur < 0 ? 0 : cur;
    if (sel >= n) sel = n - 1;

    int K = 9, rowH = 76, headH = 72, footH = 50;
    int shown = n < K ? n : K;
    int W = 660, H = headH + shown * rowH + footH;
    int x = 56, y = (g->height - H) / 2;
    panel(g, x, y, W, H, 24, INK, 226);

    // header: accent tick + title + count
    gfx_round(g, x + 28, y + 26, 6, 28, 3, ACCENT);
    gtext(g, x + 46, y + 24, "CHANNELS", 3, TXT, 1);
    char cnt[24]; snprintf(cnt, sizeof(cnt), "%d", n);
    gfx_text(g, x + W - 28 - gfx_text_w(cnt, 2), y + 30, cnt, 2, FAINT);
    gfx_rect_a(g, x + 24, y + headH - 12, W - 48, 1, HAIR, 30);

    int start = sel - K / 2;
    if (start > n - K) start = n - K;
    if (start < 0) start = 0;

    for (int r = 0; r < K && start + r < n; r++) {
        int idx = start + r, rowY = y + headH + r * rowH;
        int rx = x + 18, rw = W - 36;
        int seld = (idx == sel);
        gfx_round_a(g, rx, rowY + 6, rw, rowH - 12, 14, seld ? ACCENT : SURF, seld ? 240 : 130);

        char name[96];
        httpd_chan_get(idx, name, sizeof(name), NULL, 0);
        int maxch = (rw - 170) / 16; if (maxch < 4) maxch = 4;
        if ((int)strlen(name) > maxch) name[maxch] = '\0';

        char num[8]; snprintf(num, sizeof(num), "%d", idx + 1);
        GfxColor numc = seld ? INK : FAINT, nc = seld ? INK : TXT;
        gfx_text(g, rx + 26, rowY + rowH / 2 - 8, num, 2, numc);
        gfx_text(g, rx + 104, rowY + rowH / 2 - 8, name, 2, nc);
        if (idx == cur) {   // live marker on the tuned channel
            int dx = rx + rw - 40;
            gfx_circle(g, dx, rowY + rowH / 2, 6, seld ? INK : LIVE);
            gfx_text(g, dx + 14, rowY + rowH / 2 - 8, "LIVE", 1, seld ? INK : LIVE);
        }
    }
    gfx_text(g, x + 28, y + H - 34, "Up / Down  change channel      Cross  watch", 2, MUT);
}

// Lightweight top-right stream telemetry, toggled by the touchpad button. Kept
// small + text-only so it never competes with software decode (unlike the HUD).
static void draw_stats_overlay(Gfx *g, double netMBs, int fps) {
    PlayerStats s; player_stats(&s);
    int pw = 446, ph = 250, x = g->width - pw - 40, y = 40;
    panel(g, x, y, pw, ph, 18, INK, 205);

    int ix = x + 26, iy = y + 24;
    gfx_circle(g, ix + 4, iy + 7, 5, LIVE);
    gtext(g, ix + 18, iy, "STREAM", 2, TXT, 1);
    char ver[16]; snprintf(ver, sizeof(ver), "v%s", APP_VER);
    gfx_text(g, x + pw - 26 - gfx_text_w(ver, 2), iy, ver, 2, FAINT);
    gfx_rect_a(g, ix, iy + 26, pw - 52, 1, HAIR, 40);

    int lx = ix, vx = ix + 150, ry = iy + 42, rh = 30;
    char b[80];
    snprintf(b, sizeof(b), "%s  %s", s.hw ? "HW" : "SW", s.codec);
    gfx_text(g, lx, ry, "Decode", 2, FAINT);  gfx_text(g, vx, ry, b, 2, s.hw ? LIVE : ACC_LT);  ry += rh;
    snprintf(b, sizeof(b), "%dx%d   %d fps", s.w, s.h, fps);
    gfx_text(g, lx, ry, "Video", 2, FAINT);   gfx_text(g, vx, ry, b, 2, (fps >= 24 || fps == 0) ? TXT : WARN);  ry += rh;
    snprintf(b, sizeof(b), "%d%%   +%.1fs", s.bufPct, s.aheadSec);
    GfxColor bc = s.bufPct >= 40 ? LIVE : (s.bufPct >= 15 ? WARN : DANGER);
    gfx_text(g, lx, ry, "Buffer", 2, FAINT);  gfx_text(g, vx, ry, b, 2, bc);  ry += rh;
    if (netMBs >= 0.05) snprintf(b, sizeof(b), "%.1f MB/s", netMBs);
    else snprintf(b, sizeof(b), "%.1f Mbps", s.bitrateMbps);
    gfx_text(g, lx, ry, "Network", 2, FAINT); gfx_text(g, vx, ry, b, 2, TXT);  ry += rh;
    snprintf(b, sizeof(b), "%s%s", s.hls ? (s.segDemux ? "HLS seg-demux" : "HLS") : "HTTP", s.lan ? "   LAN" : "");
    gfx_text(g, lx, ry, "Source", 2, FAINT);  gfx_text(g, vx, ry, b, 2, MUT);  ry += rh;
    snprintf(b, sizeof(b), "%ld", s.drops);
    gfx_text(g, lx, ry, "Dropped", 2, FAINT); gfx_text(g, vx, ry, b, 2, s.drops > 0 ? WARN : MUT);  ry += rh;
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

    // Clear both buffers up front so nothing shows garbage.
    gfx_clear(&g, BG); gfx_present(&g, 0);
    gfx_clear(&g, BG); gfx_present(&g, 1);

    // Start the freeze watchdog (auto-recovers a frozen app instead of a reboot).
    g_heartbeat = sceKernelGetProcessTime();
    OrbisPthread wd;
    scePthreadCreate(&wd, NULL, watchdog_main, NULL, "ps4cast_wd");

#ifdef BOOT_MINIMAL
    int frameID = 2;
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

    char url[1024];
    int frameID = 2;
    int everDrew = 0;
    int running = 1;
    uint64_t hudUntil = 0;
    int navSel = -1;                  // highlighted channel in the zapper overlay
    uint64_t chanUntil = 0;           // overlay visible until this time
    uint64_t chanTuneAt = 0;          // pending tune time (settle-to-tune)
    int statsOn = 0;                  // touchpad-toggled stream stats overlay
    double netMBs = 0;                // sampled download throughput
    uint64_t rxT0 = 0, rxB0 = 0;      // throughput sampling anchor
    int fpsCount = 0, fpsVal = 0; uint64_t fpsT0 = 0;
    PadState pad;
    pad_init(&pad);

    while (running) {
        g_heartbeat = sceKernelGetProcessTime();   // pet the freeze watchdog each frame
        uint64_t now = sceKernelGetProcessTime();
        uint32_t pressed = pad_poll(&pad);

        // ---- TV-box channel zapper: D-pad Up/Down browse the loaded playlist ----
        int nch = httpd_chan_count();
        if (nch > 0 && (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN))) {
            if (navSel < 0 || now > chanUntil) {       // (re)open at the tuned channel
                navSel = httpd_chan_current(); if (navSel < 0) navSel = 0;
            } else if (pressed & ORBIS_PAD_BUTTON_UP) {
                navSel = (navSel - 1 + nch) % nch;
            } else {
                navSel = (navSel + 1) % nch;
            }
            chanUntil = now + 6000000ULL;
            chanTuneAt = now + 850000ULL;              // tune once you settle
            hudUntil = now + 5000000ULL;
            pressed &= ~(ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN);  // don't also seek
        }
        // Cross while the overlay is up = watch the highlighted channel now.
        if (nch > 0 && now < chanUntil && (pressed & ORBIS_PAD_BUTTON_CROSS)) {
            chanTuneAt = now;
            pressed &= ~(ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_OPTIONS);
        }
        // Touchpad toggles the lightweight stream-stats overlay.
        if (pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) statsOn = !statsOn;

        // Sample download throughput (~2 Hz) and presented-frame rate (1 Hz).
        {
            uint64_t rx = httpsrc_rx_total();
            if (rxT0 == 0) { rxT0 = now; rxB0 = rx; }
            else if (now - rxT0 >= 500000ULL) {
                double dt = (double)(now - rxT0) / 1e6;
                double db = (rx >= rxB0) ? (double)(rx - rxB0) : 0;   // 0 across a new stream
                netMBs = (db / dt) / 1e6;
                rxT0 = now; rxB0 = rx;
            }
            if (fpsT0 == 0) fpsT0 = now;
            else if (now - fpsT0 >= 1000000ULL) { fpsVal = fpsCount; fpsCount = 0; fpsT0 = now; }
        }

        // Settle-to-tune: switch to the highlighted channel.
        if (chanTuneAt && now >= chanTuneAt) {
            chanTuneAt = 0;
            if (navSel >= 0 && navSel != httpd_chan_current()) {
                char curl[1024];
                if (httpd_chan_get(navSel, NULL, 0, curl, sizeof(curl))) {
                    httpd_chan_set_current(navSel);
                    player_play(curl);
                    everDrew = 0;
                    hudUntil = now + 6000000ULL;
                    chanUntil = now + 3500000ULL;
                }
            }
        }

        if (player_started() && pressed) {
            double cur = 0, dur = 0;
            player_progress(&cur, &dur);
            hudUntil = sceKernelGetProcessTime() + 5000000ULL;
            if (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_OPTIONS)) {
                player_pause(!player_is_paused());
                hudUntil = sceKernelGetProcessTime() + 9000000ULL;
            }
            if (pressed & ORBIS_PAD_BUTTON_LEFT)  player_seek(cur - 10.0);
            if (pressed & ORBIS_PAD_BUTTON_RIGHT) player_seek(cur + 10.0);
            if (pressed & ORBIS_PAD_BUTTON_DOWN)  player_seek(cur - 30.0);
            if (pressed & ORBIS_PAD_BUTTON_UP)    player_seek(cur + 30.0);
            if (pressed & ORBIS_PAD_BUTTON_L1)    player_seek(cur - 60.0);
            if (pressed & ORBIS_PAD_BUTTON_R1)    player_seek(cur + 60.0);
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                player_stop();
                handoff_stop();
                everDrew = 0;
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
            handoff_stop();
            everDrew = 0;
        }
        if (httpd_take_play_request(url, sizeof(url))) {
            // Native app/browser handoff is permanently disabled (CE-36329-3),
            // so /play now drives the in-app AvPlayer just like /avplay.
            player_play(url);
            everDrew = 0;
            hudUntil = sceKernelGetProcessTime() + 6000000ULL;
        }
        if (httpd_take_player_request(url, sizeof(url))) {
            player_play(url);
            everDrew = 0;
            hudUntil = sceKernelGetProcessTime() + 6000000ULL;
        }

        // Autoplay / EOF cleanup: once the decoder reports inactive, either
        // advance to the queued item or fully tear down the finished playback.
        if (player_started() && !player_is_active()) {
            if (httpd_take_next(url, sizeof(url))) {
                player_play(url);
                everDrew = 0;
                hudUntil = sceKernelGetProcessTime() + 6000000ULL;
            } else {
                player_stop();
                everDrew = 0;
            }
        }

        if (player_started()) {
            sceSystemServiceTickVideoPlayback();
            sceSystemServicePowerTick();
            int drew = player_render(&g);   // always pump frames while started
            if (drew) {
                everDrew = 1;
                fpsCount++;                 // count real presented video frames
            } else if (!everDrew) {
                gfx_vgrad(&g, 0, 0, g.width, g.height, BG_TOP, BG_BOT);
                int pw = 720, ph = 264, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
                panel(&g, px, py, pw, ph, 24, SURF, 235);
                icon_cast(&g, g.width / 2, py + 84, 96);
                ctext(&g, py + 156, "Connecting...", 4, TXT, 1);
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
                ctext(&g, py + 38, b, 4, TXT, 1);
                int gx = px + 50, gy = py + 106, gw = pw - 100, gh = 10;
                gfx_round(&g, gx, gy, gw, gh, gh / 2, SURF2);
                int fillw = gw * player_buffer_pct() / 100; if (fillw < 0) fillw = 0; if (fillw > gw) fillw = gw;
                if (fillw > gh) gfx_round(&g, gx, gy, fillw, gh, gh / 2, ACCENT);
                ctext(&g, py + 140, "Circle  Stop      Left  seek back", 2, MUT, 0);
                hudUntil = sceKernelGetProcessTime() + 2000000ULL;  // keep HUD visible too
            }

            // The channel overlay replaces the HUD while browsing (no clutter).
            int overlay = (sceKernelGetProcessTime() < chanUntil && httpd_chan_count() > 0);
            if (!overlay && (!everDrew || player_is_paused() || sceKernelGetProcessTime() < hudUntil))
                draw_hud(&g);
        } else {
            draw_lobby(&g, ip, net_ok);
            everDrew = 0;
        }

        // Channel zapper overlay sits on top of whatever is showing.
        if (sceKernelGetProcessTime() < chanUntil && httpd_chan_count() > 0)
            draw_channel_overlay(&g, navSel);

        // Lightweight stream stats (touchpad), top-right, only while playing.
        if (statsOn && player_started())
            draw_stats_overlay(&g, netMBs, fpsVal);

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
