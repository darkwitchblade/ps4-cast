// uipreview.c — render the PS4 in-app screens (lobby + HUD) to PNG on a desktop
// so the console UI can be designed and reviewed without deploying. Compiles the
// real gfx primitives (GFX_HOST_PREVIEW) + qr, then mirrors the draw_* code that
// ships in main.c. Build: see tools/preview.sh.
#define GFX_HOST_PREVIEW
#include "../app/src/gfx.c"
#include "../app/src/qr.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT 8080

// ---- palette (kept identical to main.c) ----------------------------------
static const GfxColor BG_TOP = { 0x12, 0x18, 0x30 };
static const GfxColor BG_BOT = { 0x06, 0x09, 0x13 };
static const GfxColor SURF   = { 0x18, 0x20, 0x3a };
static const GfxColor SURF2  = { 0x22, 0x2c, 0x4e };
static const GfxColor HAIR   = { 0x8a, 0x99, 0xd8 };
static const GfxColor ACCENT = { 0x5b, 0x8c, 0xff };
static const GfxColor ACC_LT = { 0x9d, 0xb8, 0xff };
static const GfxColor LIVE   = { 0x2e, 0xe6, 0xa6 };
static const GfxColor DANGER = { 0xff, 0x5d, 0x7a };
static const GfxColor TXT    = { 0xf3, 0xf6, 0xff };
static const GfxColor MUT    = { 0x9a, 0xa4, 0xc8 };
static const GfxColor FAINT  = { 0x6b, 0x73, 0x98 };
static const GfxColor INK    = { 0x07, 0x0a, 0x14 };
static const GfxColor PAPER  = { 0xf4, 0xf7, 0xff };
static const GfxColor BTN_X  = { 0x86, 0xa9, 0xff };
static const GfxColor BTN_O  = { 0xff, 0x73, 0x88 };
static const GfxColor BTN_T  = { 0x44, 0xe0, 0xa6 };
static const GfxColor WARN   = { 0xff, 0xc4, 0x4a };
#define APP_VER "03.19"
typedef struct { int hw,hls,segDemux,w,h; long frames,drops; double bitrateMbps,aheadSec; int bufPct,lan; char codec[24]; } PlayerStats;
static void player_stats(PlayerStats *s){ memset(s,0,sizeof(*s)); s->hw=1; s->w=1920; s->h=1080; s->drops=2;
 s->bitrateMbps=6.4; s->aheadSec=4.1; s->bufPct=92; s->hls=1; s->segDemux=1; s->lan=0; snprintf(s->codec,sizeof(s->codec),"h264"); }

// ---- shared draw helpers (ported verbatim into main.c) -------------------
static void thick_line(Gfx *g, float x0, float y0, float x1, float y1, float th, GfxColor c) {
    float dx = x1 - x0, dy = y1 - y0, len = __builtin_sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * (th / 2), ny = dx / len * (th / 2);
    gfx_tri(g, (int)(x0 + nx), (int)(y0 + ny), (int)(x0 - nx), (int)(y0 - ny), (int)(x1 + nx), (int)(y1 + ny), c);
    gfx_tri(g, (int)(x0 - nx), (int)(y0 - ny), (int)(x1 + nx), (int)(y1 + ny), (int)(x1 - nx), (int)(y1 - ny), c);
}
static void gtext(Gfx *g, int x, int y, const char *s, int sc, GfxColor c, int tr) {
    if (sc >= 4) gfx_text_tr(g, x + 2, y + 2, s, sc, INK, tr);   // soft drop for headings
    gfx_text_tr(g, x, y, s, sc, c, tr);
}
static void ctext(Gfx *g, int cy, const char *s, int sc, GfxColor c, int tr) {
    int w = gfx_text_tr_w(s, sc, tr);
    gtext(g, (g->width - w) / 2, cy, s, sc, c, tr);
}
// soft shadow under a rounded panel for depth
static void panel(Gfx *g, int x, int y, int w, int h, int r, GfxColor c, int a) {
    gfx_round_a(g, x, y, w, h, r, c, a);
    gfx_rect_a(g, x + r, y, w - 2 * r, 1, HAIR, 36);   // top hairline highlight
}

// ---- icons ---------------------------------------------------------------
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
static void icon_stop(Gfx *g, int cx, int cy, int s, GfxColor c) {
    gfx_round(g, cx - s / 2, cy - s / 2, s, s, (int)(s * 0.24f), c);
}
// the brand cast glyph on its own rounded tile
static void icon_cast(Gfx *g, int cx, int cy, int box) {
    panel(g, cx - box / 2, cy - box / 2, box, box, box / 4, SURF2, 255);
    int s = (int)(box * 0.60f);
    int sw = s, sh = (int)(s * 0.64f);
    int sx = cx - sw / 2, sy = cy - (int)(s * 0.40f);
    int rr = (int)(s * 0.17f); int th = (int)(s * 0.11f); if (th < 2) th = 2;
    gfx_round(g, sx, sy, sw, sh, rr, ACC_LT);
    gfx_round(g, sx + th, sy + th, sw - 2 * th, sh - 2 * th, rr - 1, SURF2);  // punch -> outline
    int dx = sx, dy = sy + sh + (int)(s * 0.18f);
    gfx_circle(g, dx, dy, (int)(s * 0.08f) + 1, LIVE);
    gfx_arc(g, dx, dy, (int)(s * 0.24f), th, 1, ACCENT);
    gfx_arc(g, dx, dy, (int)(s * 0.42f), th, 1, ACCENT);
}
// PS4 controller button glyphs (filled, true colors)
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
    float ax = cx, ay = cy - r;            // apex
    float bx = cx - r * 0.92f, by = cy + r * 0.75f;
    float dx = cx + r * 0.92f, dy = cy + r * 0.75f;
    thick_line(g, ax, ay, bx, by, t, c);
    thick_line(g, bx, by, dx, dy, t, c);
    thick_line(g, dx, dy, ax, ay, t, c);
}

// ---- QR card -------------------------------------------------------------
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

// ---- legend chip ---------------------------------------------------------
// a control hint: button glyph + label, returns total width drawn
static int legend(Gfx *g, int x, int cy, int kind, const char *label) {
    int r = 13;
    int gx = x + r;
    if (kind == 0) btn_cross(g, gx, cy, r, BTN_X);
    else if (kind == 1) btn_circle(g, gx, cy, r, BTN_O);
    else if (kind == 2) btn_triangle(g, gx, cy, r, BTN_T);
    else { // dpad seek -> two small triangles
        gfx_tri(g, gx + 4, cy, gx + 12, cy - 7, gx + 12, cy + 7, MUT);
        gfx_tri(g, gx - 4, cy, gx - 12, cy - 7, gx - 12, cy + 7, MUT);
    }
    int tx = x + r * 2 + 12;
    gfx_text(g, tx, cy - 8, label, 2, MUT);
    return r * 2 + 12 + gfx_text_w(label, 2) + 40;
}

// ---- LOBBY ---------------------------------------------------------------
static void draw_lobby(Gfx *g, const char *ip, int net_ok, const char *status) {
    gfx_vgrad(g, 0, 0, g->width, g->height, BG_TOP, BG_BOT);
    gfx_rect_a(g, 0, 0, g->width, 360, ACCENT, 14);     // top accent veil
    int W = g->width;

    // brand row, centered
    int track = 3;
    const char *wm = "PS4 Cast";
    int ws = 6;
    int ww = gfx_text_tr_w(wm, ws, track);
    int box = 92;
    int group = box + 26 + ww;
    int gx = (W - group) / 2;
    int brandCy = 150;
    icon_cast(g, gx + box / 2, brandCy, box);
    gtext(g, gx + box + 26, brandCy - (ws * 8) / 2, wm, ws, TXT, track);
    ctext(g, 250, "WIRELESS CAST RECEIVER", 2, FAINT, 6);

    if (net_ok) {
        char url[80];
        snprintf(url, sizeof(url), "http://%s:%d", ip, PORT);
        draw_qr_card(g, url, W / 2, 320, 9);
        int qcard = (QR_SIZE + 6) * 9 + 60;
        int below = 320 + qcard + 40;
        ctext(g, below, "Scan with your phone to open the controls", 3, MUT, 1);

        // URL pill
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

    // status chip (centered)
    char st[160];
    snprintf(st, sizeof(st), "%s", status);
    int sw = gfx_text_w(st, 2) + 60;
    int sx = (W - sw) / 2, sy = 880;
    panel(g, sx, sy, sw, 44, 22, SURF, 220);
    gfx_circle(g, sx + 26, sy + 22, 5, net_ok ? LIVE : FAINT);
    gfx_text(g, sx + 42, sy + 14, st, 2, MUT);

    // control legend, centered row
    struct { int k; const char *l; } items[] = { {0,"Pause"},{1,"Stop"},{3,"Seek"},{2,"Exit"} };
    int total = 0, ws2[4];
    for (int i = 0; i < 4; i++) { ws2[i] = 13 * 2 + 12 + gfx_text_w(items[i].l, 2) + 40; total += ws2[i]; }
    int lx = (W - (total - 40)) / 2, ly = 980;
    for (int i = 0; i < 4; i++) lx += legend(g, lx, ly, items[i].k, items[i].l);
}

// ---- HUD -----------------------------------------------------------------
static void fmt_time(double sec, char *out, int cap) {
    if (sec < 0) sec = 0;
    int s = (int)(sec + 0.5), h = s / 3600, m = (s / 60) % 60; s %= 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, s);
    else snprintf(out, cap, "%d:%02d", m, s);
}
static void draw_hud(Gfx *g, const char *title, const char *status, double cur, double dur, int paused) {
    int W = g->width, H = 224, y = g->height - H - 36, x = 40, w = W - 80;
    panel(g, x, y, w, H, 24, INK, 200);

    int pad = 34;
    // now-playing badge
    int bx = x + pad, by = y + 26, bs = 56;
    panel(g, bx, by, bs, bs, 14, SURF2, 255);
    if (paused) icon_pause(g, bx + bs / 2, by + bs / 2, 22, ACC_LT);
    else icon_play(g, bx + bs / 2 + 2, by + bs / 2, 24, ACC_LT);

    gtext(g, bx + bs + 20, by + 2, title, 3, TXT, 0);
    char sub[200];
    snprintf(sub, sizeof(sub), "%s", paused ? "Paused" : status);
    gfx_text(g, bx + bs + 20, by + 32, sub, 2, MUT);

    // live/state pill at right
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
    if (fw > barH) {
        // accent fill (two-tone)
        gfx_round(g, barX, barY, fw, barH, barH / 2, ACCENT);
    }
    gfx_circle(g, barX + fw, barY + barH / 2, 11, TXT);
    gfx_circle(g, barX + fw, barY + barH / 2, 5, ACCENT);

    gfx_text(g, barX, barY + 24, curS, 2, MUT);
    gfx_text(g, barX + barW - gfx_text_w(durS, 2), barY + 24, durS, 2, MUT);

    // legend
    struct { int k; const char *l; } items[] = { {0,"Pause"},{1,"Stop"},{3,"Seek"},{2,"Exit"} };
    int lx = barX, ly = y + H - 30;
    for (int i = 0; i < 4; i++) lx += legend(g, lx, ly, items[i].k, items[i].l);
}

// ---- channel overlay (mirrors main.c draw_channel_overlay) ---------------
static const char *MOCKCH[] = {
 "BBC One HD","CNN International","Al Jazeera English","ESPN 1","Discovery Channel",
 "National Geographic","Cartoon Network","HBO Max 24/7","Sky Sports Main Event",
 "France 24 English","Bloomberg TV","MTV Live HD","Eurosport 1","Sintel Trailer"
};
static int MOCKN = 14, MOCKCUR = 3;
static int httpd_chan_count(void){ return MOCKN; }
static int httpd_chan_current(void){ return MOCKCUR; }
static int httpd_chan_get(int i, char *name, int nameCap, char *url, int urlCap){
    if(i<0||i>=MOCKN) return 0;
    if(name&&nameCap>0){ strncpy(name,MOCKCH[i],nameCap-1); name[nameCap-1]='\0'; }
    (void)url;(void)urlCap; return 1;
}
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
        if (idx == cur) {
            int dx = rx + rw - 40;
            gfx_circle(g, dx, rowY + rowH / 2, 6, seld ? INK : LIVE);
            gfx_text(g, dx + 14, rowY + rowH / 2 - 8, "LIVE", 1, seld ? INK : LIVE);
        }
    }
    gfx_text(g, x + 28, y + H - 34, "Up / Down  change channel      Cross  watch", 2, MUT);
}

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
    gfx_text(g, ix, y + ph - 30, "Touchpad  hide", 1, FAINT);
}

// ---- harness -------------------------------------------------------------
static void write_ppm(const char *path, Gfx *g) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", g->width, g->height);
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    for (int i = 0; i < g->width * g->height; i++) {
        uint32_t e = fb[i];
        unsigned char px[3] = { (e >> 16) & 0xff, (e >> 8) & 0xff, e & 0xff };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
int main(void) {
    Gfx g; memset(&g, 0, sizeof(g));
    g.width = 1920; g.height = 1080; g.depth = 4; g.activeIdx = 0;
    g.frameBuffers[0] = malloc((size_t)g.width * g.height * 4);

    draw_lobby(&g, "192.168.1.33", 1, "Ready  -  waiting for a cast");
    write_ppm("/tmp/ui_lobby.ppm", &g);

    // HUD over a faux video frame
    GfxColor v1 = { 0x20, 0x12, 0x32 }, v2 = { 0x08, 0x10, 0x28 };
    gfx_vgrad(&g, 0, 0, g.width, g.height, v1, v2);
    gfx_circle_a(&g, 1500, 360, 240, (GfxColor){0x40,0x30,0x70}, 120);
    draw_hud(&g, "sintel_trailer-720p.mp4", "Playing  -  hls direct  1280x720", 71, 215, 0);
    write_ppm("/tmp/ui_hud.ppm", &g);

    // connecting screen
    gfx_vgrad(&g, 0, 0, g.width, g.height, BG_TOP, BG_BOT);
    {
        int pw = 720, ph = 264, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
        panel(&g, px, py, pw, ph, 24, SURF, 235);
        icon_cast(&g, g.width / 2, py + 84, 96);
        ctext(&g, py + 156, "Connecting\xe2\x80\xa6", 4, TXT, 1);
        ctext(&g, py + 212, "opening source  -  hls fetch", 2, MUT, 0);
    }
    write_ppm("/tmp/ui_connect.ppm", &g);

    // buffering overlay on video
    gfx_vgrad(&g, 0, 0, g.width, g.height, v1, v2);
    {
        int pw = 560, ph = 188, px = (g.width - pw) / 2, py = (g.height - ph) / 2;
        panel(&g, px, py, pw, ph, 22, INK, 225);
        ctext(&g, py + 38, "Buffering  62%", 4, TXT, 1);
        int gx = px + 50, gy = py + 106, gw = pw - 100, gh = 10;
        gfx_round(&g, gx, gy, gw, gh, gh / 2, SURF2);
        gfx_round(&g, gx, gy, gw * 62 / 100, gh, gh / 2, ACCENT);
        ctext(&g, py + 140, "Circle  Stop      Left  seek back", 2, MUT, 0);
    }
    write_ppm("/tmp/ui_buffer.ppm", &g);

    // channel zapper overlay over playing video
    gfx_vgrad(&g, 0, 0, g.width, g.height, v1, v2);
    gfx_circle_a(&g, 1500, 360, 240, (GfxColor){0x40,0x30,0x70}, 120);
    draw_hud(&g, "Sky Sports Main Event", "Playing  -  hls live  1920x1080", 0, 0, 0);
    draw_channel_overlay(&g, 8);   // highlight a row away from the live one
    write_ppm("/tmp/ui_chan.ppm", &g);

    // stats overlay over playing video (HUD also up, to check they don't clash)
    gfx_vgrad(&g, 0, 0, g.width, g.height, v1, v2);
    gfx_circle_a(&g, 700, 360, 240, (GfxColor){0x40,0x30,0x70}, 120);
    draw_hud(&g, "Sky Sports Main Event", "Playing  -  hls live  1920x1080", 71, 215, 0);
    draw_stats_overlay(&g, 3.4, 50);
    write_ppm("/tmp/ui_stats.ppm", &g);
    return 0;
}
