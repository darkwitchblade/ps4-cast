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

static const GfxColor BG     = { 0x0b, 0x10, 0x20 };
static const GfxColor PANEL  = { 0x15, 0x1c, 0x33 };
static const GfxColor ACCENT = { 0x3b, 0x82, 0xf6 };
static const GfxColor WHITE  = { 0xf0, 0xf4, 0xff };
static const GfxColor MUTED  = { 0x8b, 0x97, 0xc4 };
static const GfxColor BLACK  = { 0x00, 0x00, 0x00 };
static const GfxColor HUD_BG = { 0x06, 0x08, 0x0f };
static const GfxColor BAR_BG = { 0x24, 0x2b, 0x3d };

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

static void draw_qr(Gfx *g, const char *url, int cx, int y, int module) {
    QrCode qr;
    int quiet = 4;
    int total = (QR_SIZE + quiet * 2) * module;
    int x0 = cx - total / 2;
    GfxColor paper = { 0xf8, 0xfa, 0xff };
    GfxColor ink = { 0x05, 0x09, 0x15 };
    GfxColor edge = { 0x2a, 0x35, 0x55 };

    gfx_rect(g, x0 - 8, y - 8, total + 16, total + 16, edge);
    gfx_rect(g, x0, y, total, total, paper);
    if (qr_make_url(url, &qr) != 0)
        return;

    for (int yy = 0; yy < QR_SIZE; yy++) {
        for (int xx = 0; xx < QR_SIZE; xx++) {
            if (qr.m[yy][xx])
                gfx_rect(g, x0 + (quiet + xx) * module, y + (quiet + yy) * module, module, module, ink);
        }
    }
}

static void draw_lobby(Gfx *g, const char *ip, int net_ok) {
    gfx_clear(g, BG);

    // Header band
    gfx_rect(g, 0, 0, g->width, 200, PANEL);
    gfx_rect(g, 0, 200, g->width, 4, ACCENT);
    text_centered(g, 78, "PS4 CAST", 8, WHITE);

    if (net_ok) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s:%d", ip, PORT);

        text_centered(g, 270, "Scan to open the PS4 Cast controls", 3, MUTED);
        draw_qr(g, url, g->width / 2, 330, 10);

        // URL plate
        int uw = gfx_text_w(url, 5);
        int px = (g->width - uw) / 2 - 40;
        gfx_rect(g, px, 700, uw + 80, 88, PANEL);
        gfx_rect(g, px, 700, uw + 80, 4, ACCENT);
        text_centered(g, 724, url, 5, WHITE);

        text_centered(g, 825, "Paste a direct video link, or cast to PS4 Cast from a DLNA app.", 3, MUTED);
    } else {
        text_centered(g, 430, "No network connection.", 5, WHITE);
        text_centered(g, 520, "Connect the PS4 to Wi-Fi or LAN, then relaunch.", 3, MUTED);
    }

    // Status line
    char st[200];
#ifdef BOOT_MINIMAL
    snprintf(st, sizeof(st), "Status: minimal boot diagnostic");
#else
    snprintf(st, sizeof(st), "Status: %s", player_status());
#endif
    text_centered(g, 900, st, 3, MUTED);
    text_centered(g, 1020, "ps4-cast  -  boot diagnostic", 2, MUTED);
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
    // and scePadReadStateExt here can block on this console/TV remote path,
    // leaving the HTTP server alive but the main loop unable to consume casts.
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
        pressed |= p->down[i] & ~p->prev[i];
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

// ---- HDMI-CEC / TV remote ------------------------------------------------
// The TV remote (and other SPECIAL-port devices) sit on
// ORBIS_PAD_PORT_TYPE_SPECIAL and are read via scePadReadStateExt, which can
// BLOCK — so they must NOT be polled in the render loop (that would stall casts,
// which is exactly why the original code only polled STANDARD pads). This
// dedicated thread blocks freely and publishes edge-detected presses into
// g_remotePressed; the main loop ORs that into its normal pad handling, so every
// existing control (pause / seek / stop) works from the remote with no new
// mapping. Remote keys arrive in OrbisPadData.buttons using the same bit set.
static volatile uint32_t g_remotePressed = 0;
// Default OFF: the SPECIAL-port reader is unproven and the audit flagged it as a
// freeze risk (scePadReadStateExt can block holding HID; opening the port may
// disturb the controller bus — see the usb_hid/jackDisconnected klog). Keep it
// dormant (never touches the SPECIAL port) until the freeze-hardening is verified
// on-device, then enable it deliberately for a careful test. Flip to 1 (or add a
// runtime toggle) once proven.
static volatile int      g_remoteEnabled = 0;
static volatile int      g_remoteStop = 0;
static volatile int      g_remoteHandle = -1;   // exposed so a stop path can close it

static void *remote_thread_main(void *arg) {
    (void)arg;
    int user = -1;
    int h = -1;
    uint32_t prev = 0;
    while (!g_remoteStop) {
        if (!g_remoteEnabled) {                  // dormant: release the port, idle, touch nothing
            if (h >= 0) { scePadClose(h); h = -1; g_remoteHandle = -1; }
            sceKernelUsleep(1000 * 1000);
            continue;
        }
        if (h < 0) {
            if (user < 0) sceUserServiceGetInitialUser(&user);
            h = scePadOpen(user, ORBIS_PAD_PORT_TYPE_SPECIAL, 0, NULL);  // remote is index 0
            if (h < 0) { sceKernelUsleep(3000 * 1000); continue; }       // slow retry; low bus churn
            g_remoteHandle = h;
            prev = 0;
            pad_diag_set("remote: SPECIAL port open");
        }
        OrbisPadData d;
        memset(&d, 0, sizeof(d));
        int rc = scePadReadStateExt(h, &d);
        if (g_remoteStop) break;
        if (rc != 0) { scePadClose(h); h = -1; g_remoteHandle = -1; sceKernelUsleep(500 * 1000); continue; }
        if (d.connected) {
            uint32_t pressed = d.buttons & ~prev;   // rising edges only
            prev = d.buttons;
            if (pressed) g_remotePressed |= pressed;
        } else {
            prev = 0;
        }
        sceKernelUsleep(16 * 1000);                  // ~60Hz; blocking-safe on this thread
    }
    if (h >= 0) { scePadClose(h); g_remoteHandle = -1; }
    return NULL;
}

static void draw_hud(Gfx *g) {
    double cur = 0, dur = 0;
    player_progress(&cur, &dur);

    int y = g->height - 206;
    gfx_rect(g, 0, y, g->width, 206, HUD_BG);
    gfx_rect(g, 0, y, g->width, 4, ACCENT);

    char curS[24], durS[24], leftS[24], line[220];
    fmt_time(cur, curS, sizeof(curS));
    fmt_time(dur, durS, sizeof(durS));
    fmt_time((dur > cur) ? (dur - cur) : 0, leftS, sizeof(leftS));

    snprintf(line, sizeof(line), "%s   %s / %s   -%s",
             player_is_paused() ? "Paused" : player_status(), curS, durS, leftS);
    gfx_text(g, 56, y + 28, line, 3, WHITE);

    char dbg[220];
    player_debug(dbg, sizeof(dbg));
    gfx_text(g, 56, y + 62, dbg, 2, MUTED);

    int bx = 56, by = y + 126, bw = g->width - 112, bh = 18;
    gfx_rect(g, bx, by, bw, bh, BAR_BG);
    if (dur > 0) {
        int fill = (int)((cur / dur) * bw);
        if (fill < 0) fill = 0;
        if (fill > bw) fill = bw;
        gfx_rect(g, bx, by, fill, bh, ACCENT);
    }
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
    PadState pad;
    pad_init(&pad);
    // TV remote (HDMI-CEC) reader on its own thread — see remote_thread_main.
    OrbisPthread rt;
    scePthreadCreate(&rt, NULL, remote_thread_main, NULL, "ps4cast_remote");

    while (running) {
        g_heartbeat = sceKernelGetProcessTime();   // pet the freeze watchdog each frame
        uint32_t pressed = pad_poll(&pad);
        uint32_t rp = g_remotePressed; g_remotePressed = 0; pressed |= rp;   // fold in TV-remote presses
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
            } else if (!everDrew) {
                gfx_clear(&g, BLACK);
                text_centered(&g, 460, "Buffering...", 4, WHITE);
                char st[200];
                snprintf(st, sizeof(st), "%s", player_status());
                text_centered(&g, 540, st, 3, MUTED);
                char dbg[200];
                player_debug(dbg, sizeof(dbg));
                text_centered(&g, 620, dbg, 3, ACCENT);
            }
            // if !drew && everDrew: keep the previous frame (no black flicker)

            // Mid-playback stall: overlay a buffering panel on the held frame so
            // it's clear what's happening, with a live buffer gauge + controls.
            if (everDrew && player_buffering()) {
                int pw = 620, ph = 170, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
                gfx_rect(&g, px, py, pw, ph, HUD_BG);
                gfx_rect(&g, px, py, pw, 4, ACCENT);
                char b[80];
                snprintf(b, sizeof(b), "Buffering   %d%%", player_buffer_pct());
                text_centered(&g, py + 40, b, 5, WHITE);
                // gauge bar
                int gx = px + 60, gy = py + 96, gw = pw - 120, gh = 16;
                gfx_rect(&g, gx, gy, gw, gh, BAR_BG);
                int fillw = gw * player_buffer_pct() / 100; if (fillw < 0) fillw = 0; if (fillw > gw) fillw = gw;
                gfx_rect(&g, gx, gy, fillw, gh, ACCENT);
                text_centered(&g, py + 132, "(O) Stop    (left) seek back", 3, MUTED);
                hudUntil = sceKernelGetProcessTime() + 2000000ULL;  // keep HUD visible too
            }

            if (!everDrew || player_is_paused() || sceKernelGetProcessTime() < hudUntil)
                draw_hud(&g);
        } else {
            draw_lobby(&g, ip, net_ok);
            everDrew = 0;
        }

        gfx_present(&g, frameID++);
    }
    // Stop the remote reader and unblock any in-flight scePadReadStateExt by
    // closing its handle, so a blocked HID syscall can't pin the process at exit.
    g_remoteStop = 1;
    if (g_remoteHandle >= 0) scePadClose(g_remoteHandle);
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
