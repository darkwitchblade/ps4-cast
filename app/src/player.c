#include "player.h"
#include "avplayer_abi.h"
#include "escalate.h"
#include "goldhen.h"
#include "notify.h"
#include "httpsrc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#define SYS_dynlib_load_prx 594

// Internal sysmodule ids (from _types/sysmodule.h) — AvPlayer's network
// streaming pulls in HTTP/SSL, so make sure they're resident.
#ifndef ORBIS_SYSMODULE_INTERNAL_HTTP
#define ORBIS_SYSMODULE_INTERNAL_HTTP 0x8000000A
#endif
#ifndef ORBIS_SYSMODULE_INTERNAL_SSL
#define ORBIS_SYSMODULE_INTERNAL_SSL  0x8000000B
#endif
#ifndef ORBIS_KERNEL_WB_ONION
#define ORBIS_KERNEL_WB_ONION 0x0
#endif

#ifndef USE_STATIC_AVPLAYER
typedef SceAvPlayerHandle (*pfnAvInit)(SceAvPlayerInitData *data);
typedef int32_t (*pfnAvPostInit)(SceAvPlayerHandle handle, void *postInitData);
typedef int32_t (*pfnAvAddSource)(SceAvPlayerHandle handle, const char *filename);
typedef int32_t (*pfnAvStart)(SceAvPlayerHandle handle);
typedef int32_t (*pfnAvStop)(SceAvPlayerHandle handle);
typedef int32_t (*pfnAvClose)(SceAvPlayerHandle handle);
typedef bool (*pfnAvIsActive)(SceAvPlayerHandle handle);
typedef bool (*pfnAvGetVideoData)(SceAvPlayerHandle handle, SceAvPlayerFrameInfo *frameInfo);
typedef bool (*pfnAvGetAudioData)(SceAvPlayerHandle handle, SceAvPlayerFrameInfo *frameInfo);

static pfnAvInit         pAvInit = NULL;
static pfnAvPostInit     pAvPostInit = NULL;
static pfnAvAddSource    pAvAddSource = NULL;
static pfnAvStart        pAvStart = NULL;
static pfnAvStop         pAvStop = NULL;
static pfnAvClose        pAvClose = NULL;
static pfnAvIsActive     pAvIsActive = NULL;
static pfnAvGetVideoData pAvGetVideoData = NULL;
static pfnAvGetAudioData pAvGetAudioData = NULL;
static int               g_avHandle = -1;

#define sceAvPlayerInit         pAvInit
#define sceAvPlayerPostInit     pAvPostInit
#define sceAvPlayerAddSource    pAvAddSource
#define sceAvPlayerStart        pAvStart
#define sceAvPlayerStop         pAvStop
#define sceAvPlayerClose        pAvClose
#define sceAvPlayerIsActive     pAvIsActive
#define sceAvPlayerGetVideoData pAvGetVideoData
#define sceAvPlayerGetAudioData pAvGetAudioData

static int bind_avplayer(void) {
    if (pAvInit && pAvAddSource && pAvStart)
        return 0;

    const char *path = "/system/common/lib/libSceAvPlayer.sprx";
    int handle = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (handle < 0) {
        int mid = -1;
        long sr = syscall(SYS_dynlib_load_prx, path, (long)0, (long)&mid, (long)0, (long)0, (long)0);
        handle = (mid >= 0) ? mid : handle;
        if (handle < 0) {
            notify("AvPlayer dynload failed w=0x%x s=0x%lx", (unsigned)handle, sr);
            return -1;
        }
    }
    g_avHandle = handle;

    #define BIND_AV(ptr, sym) do { \
        if (sceKernelDlsym(g_avHandle, sym, (void **)&ptr) != 0 || !ptr) { \
            notify("AvPlayer dlsym failed: " sym); \
            return -2; \
        } \
    } while (0)

    BIND_AV(pAvInit, "sceAvPlayerInit");
    BIND_AV(pAvPostInit, "sceAvPlayerPostInit");
    BIND_AV(pAvAddSource, "sceAvPlayerAddSource");
    BIND_AV(pAvStart, "sceAvPlayerStart");
    BIND_AV(pAvStop, "sceAvPlayerStop");
    BIND_AV(pAvClose, "sceAvPlayerClose");
    BIND_AV(pAvIsActive, "sceAvPlayerIsActive");
    BIND_AV(pAvGetVideoData, "sceAvPlayerGetVideoData");
    BIND_AV(pAvGetAudioData, "sceAvPlayerGetAudioData");

    #undef BIND_AV
    return 0;
}
#else
static int bind_avplayer(void) { return 0; }
#endif

// Live debug state (drawn on the buffering screen).
static int  g_dbgActive = -1;   // sceAvPlayerIsActive()
static int  g_dbgVget   = -1;   // last GetVideoData() return
static int  g_dbgPdata  = -1;   // last frame had non-null pData
static int  g_dbgW = 0, g_dbgH = 0;
static long g_dbgAudio  = 0;    // total audio packets drained
static long g_dbgVcalls = 0;    // total GetVideoData calls
static int  g_dbgOpens  = 0;    // file open callbacks
static long g_dbgReads  = 0;    // readOffset callbacks
static long g_dbgBytes  = 0;    // total bytes read through our callback
static int  g_dbgEvent  = -1;   // last AvPlayer event id
static char g_dbgMode[32] = "std";
static long long g_dbgFileSize = 0;
static unsigned long long g_dbgLastPos = 0;
static unsigned int g_dbgLastLen = 0;
static int g_dbgLastRead = 0;

// ---------------------------------------------------------------------------
// Unified direct-memory arena for ALL AvPlayer allocations. The PS4 hardware
// decoder needs its work/output buffers in physical (direct) memory — malloc'd
// memory is not usable by the decoder, which makes it fail right after probing
// and STOP. So we serve both the general allocator AND the texture allocator
// from one WB_ONION (CPU-cached, decoder-coherent) arena. Bump-allocated and
// reset per play; deallocate is a no-op (fine for a single bounded playback).
// ---------------------------------------------------------------------------
#define ARENA_SIZE   (256 * 1024 * 1024)
#define ARENA_ALIGN  (2 * 1024 * 1024)

static void     *g_arenaBase = NULL;
static uintptr_t  g_arenaSP   = 0;
static off_t      g_arenaOff  = 0;
static int        g_oomNotified = 0;

static int tex_pool_init(void) {
    if (g_arenaBase) return 0;
    int types[2] = { ORBIS_KERNEL_WB_ONION, 3 /*WC_GARLIC*/ };
    for (int t = 0; t < 2; t++) {
        off_t off = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                          ARENA_SIZE, ARENA_ALIGN, types[t], &off) < 0)
            continue;
        void *base = NULL;
        if (sceKernelMapDirectMemory(&base, ARENA_SIZE, 0x33, 0, off, ARENA_ALIGN) < 0) {
            sceKernelReleaseDirectMemory(off, ARENA_SIZE);
            continue;
        }
        g_arenaOff = off; g_arenaBase = base; g_arenaSP = (uintptr_t)base;
        return 0;
    }
    return -1;
}
static void tex_pool_reset(void) { g_arenaSP = (uintptr_t)g_arenaBase; g_oomNotified = 0; }

static void *arena_bump(uint32_t align, uint32_t size) {
    if (align < 256) align = 256;
    uintptr_t aligned = (g_arenaSP + align - 1) & ~((uintptr_t)align - 1);
    uintptr_t end = aligned + size;
    if (g_arenaBase && end <= (uintptr_t)g_arenaBase + ARENA_SIZE) {
        g_arenaSP = end;
        return (void *)aligned;
    }
    if (!g_oomNotified) { g_oomNotified = 1; notify("Cast: arena OUT OF MEMORY"); }
    return NULL;
}

// ---- AvPlayer allocator callbacks (all physical) --------------------------
static void *cb_allocate(void *p, uint32_t align, uint32_t size)        { (void)p; return arena_bump(align, size); }
static void  cb_deallocate(void *p, void *mem)                          { (void)p; (void)mem; }
static void *cb_allocate_tex(void *p, uint32_t align, uint32_t size)    { (void)p; return arena_bump(align, size); }
static void  cb_deallocate_tex(void *p, void *mem)                      { (void)p; (void)mem; }

// ---- file replacement: the app does the I/O ------------------------------
// In homebrew, AvPlayer's internal file reader doesn't service /app0 paths, so
// local bundled clips need app-managed reads. Remote http(s) URLs should use
// AvPlayer's native network path so libSceHttp/libSceSsl can handle TLS.
static int     g_fileFd   = -1;
static int64_t g_fileSize = 0;

static int cb_file_open(void *p, const char *filename) {
    (void)p;
    g_dbgOpens++;
    g_fileFd = sceKernelOpen(filename, 0 /*O_RDONLY*/, 0);
    if (g_fileFd < 0) {
        notify("Cast: file open FAILED (%.70s)", filename);
        return -1;
    }
    g_fileSize = (int64_t)sceKernelLseek(g_fileFd, 0, 2 /*SEEK_END*/);
    sceKernelLseek(g_fileFd, 0, 0 /*SEEK_SET*/);
    g_dbgFileSize = (long long)g_fileSize;
    notify_dbg("Cast: file opened, %lld bytes", (long long)g_fileSize);
    return 0;
}
static int cb_file_close(void *p) {
    (void)p;
    if (g_fileFd >= 0) { sceKernelClose(g_fileFd); g_fileFd = -1; }
    return 0;
}
static int cb_file_readoffset(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
    (void)p;
    if (g_fileFd < 0) return -1;
    int n = (int)sceKernelPread(g_fileFd, buf, len, (off_t)pos);  // positional = thread-safe
    g_dbgReads++;
    g_dbgLastPos = (unsigned long long)pos;
    g_dbgLastLen = len;
    g_dbgLastRead = n;
    if (n > 0) g_dbgBytes += n;
    return n;
}
static uint64_t cb_file_size(void *p) {
    (void)p;
    return (uint64_t)g_fileSize;
}

// ---- remote http:// reader: app does the network I/O ----------------------
// For http URLs AvPlayer's native reader can't fetch in homebrew (AddSource
// returns "bad source"), so we drive the same fileReplacement interface with an
// app-managed ranged-HTTP reader (httpsrc). AvPlayer hands us the URL as the
// "filename"; we fetch the bytes ourselves.
static int cb_http_open(void *p, const char *filename) {
    (void)p;
    g_dbgOpens++;
    int rc = httpsrc_open(filename);
    if (rc != 0) {
        notify("Cast: http open FAILED rc=%d", rc);
        return -1;
    }
    g_fileSize = (int64_t)httpsrc_size();
    g_dbgFileSize = (long long)g_fileSize;
    notify_dbg("Cast: http opened, %lld bytes", (long long)g_fileSize);
    return 0;
}
static int cb_http_close(void *p) {
    (void)p;
    httpsrc_close();
    return 0;
}
static int cb_http_readoffset(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
    (void)p;
    int n = httpsrc_read(buf, pos, len);
    g_dbgReads++;
    g_dbgLastPos = (unsigned long long)pos;
    g_dbgLastLen = len;
    g_dbgLastRead = n;
    if (n > 0) g_dbgBytes += n;
    return n;
}
static uint64_t cb_http_size(void *p) {
    (void)p;
    return (uint64_t)g_fileSize;
}

// ---- state ----------------------------------------------------------------
static SceAvPlayerHandle g_player = -1;
static int  g_playerInitDone = 0;
static int  g_active = 0;
static int  g_gotFrame = 0;
static int  g_waitFrames = 0;
static char g_status[160] = "idle";


// Event history + the detail carried by warning/error events.
static int  g_evSeq[16];
static int  g_evN = 0;
static int  g_evSrc = 0;     // sourceId of last event
static long g_evData = 0;    // data pointer value of last event
static int  g_evDataVal = 0; // *(int*)data of last event (guarded)
static int  g_prc = 99, g_arc = 99, g_strc = 99; // postinit/addsource/start rc

static void on_event(void *p, int32_t eventId, int32_t srcId, void *data) {
    (void)p;
    if (g_evN < 16) g_evSeq[g_evN++] = eventId;
    g_evSrc = srcId;
    g_evData = (long)(intptr_t)data;
    if (data) g_evDataVal = *(volatile int *)data;   // warnings often point at a code
    g_dbgEvent = eventId;
    switch (eventId) {
        case SCE_AVPLAYER_STATE_READY:     snprintf(g_status, sizeof(g_status), "ready"); break;
        case SCE_AVPLAYER_STATE_PLAY:      snprintf(g_status, sizeof(g_status), "playing"); break;
        case SCE_AVPLAYER_STATE_PAUSE:     snprintf(g_status, sizeof(g_status), "paused"); break;
        case SCE_AVPLAYER_STATE_BUFFERING: snprintf(g_status, sizeof(g_status), "buffering"); break;
        case SCE_AVPLAYER_STATE_STOP:      snprintf(g_status, sizeof(g_status), "stopped"); g_active = 0; break;
        case SCE_AVPLAYER_WARNING_ID:      notify_dbg("AvPlayer warning"); break;
        default: break;
    }
}

int player_init(void) {
    if (g_playerInitDone)
        return 0;

    sceUserServiceInitialize(NULL);

    // Use GoldHEN's supported SDK syscall only while loading restricted media
    // modules, then restore before actual playback. The old jb.prx path fails
    // ENOEXEC on this console and persistent elevation has caused CE-36329-3.
    char gh[256];
    int gh_rc = goldhen_enter(gh, sizeof(gh));
    if (gh_rc != 0)
        jailbreak();

    // AvPlayer needs the actual audio/video decoder modules resident — loading
    // AV_PLAYER alone parses the container but can't init a decoder, so it reads
    // the header then STOPs. Load the decoders explicitly.
    int v2  = sceSysmoduleLoadModule((enum OrbisSysModule)0x00CF); // VIDEODEC2
    int vw  = sceSysmoduleLoadModule((enum OrbisSysModule)0x00D0); // VDECWRAP
    int v1  = sceSysmoduleLoadModule((enum OrbisSysModule)0x008E); // VIDEODEC
    int ad  = sceSysmoduleLoadModule((enum OrbisSysModule)0x0088); // AUDIODEC
    notify_dbg("Cast modules: vdec2=%d vdecw=%d vdec=%d adec=%d", v2, vw, v1, ad);

    int avmod = sceSysmoduleLoadModule(ORBIS_SYSMODULE_AV_PLAYER);
    if (avmod < 0) {
        snprintf(g_status, sizeof(g_status), "avplayer module load failed 0x%x", (unsigned)avmod);
        notify("PS4 Cast: AvPlayer module FAILED 0x%x", (unsigned)avmod);
        return -1;
    }
    int bindrc = bind_avplayer();
    if (bindrc != 0) {
        goldhen_restore(NULL, 0);
        snprintf(g_status, sizeof(g_status), "avplayer bind failed %d", bindrc);
        return -1;
    }
    // Best-effort: AvPlayer URL streaming needs these resident.
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);

    if (tex_pool_init() != 0) {
        goldhen_restore(NULL, 0);
        snprintf(g_status, sizeof(g_status), "texture pool alloc failed");
        notify("PS4 Cast: texture pool FAILED");
        return -2;
    }
    goldhen_restore(NULL, 0);
    g_playerInitDone = 1;
    snprintf(g_status, sizeof(g_status), "ready to cast");
    return 0;
}

int player_play(const char *url) {
    player_stop();
    g_gotFrame = 0;
    g_waitFrames = 0;
    g_dbgReads = 0; g_dbgBytes = 0; g_dbgOpens = 0; g_dbgEvent = -1;
    g_dbgFileSize = 0; g_dbgLastPos = 0; g_dbgLastLen = 0; g_dbgLastRead = 0;
    g_dbgAudio = 0; g_dbgVcalls = 0;
    g_evN = 0; g_evSrc = 0; g_evData = 0; g_evDataVal = 0;
    g_prc = 99; g_arc = 99; g_strc = 99;

    // Bisection helpers: bundled clips with different H.264 profiles to find what
    // the PS4 decoder accepts. (Original test.mp4 is High@L5.0 -> rejected.)
    const char *src = url;
    int is_local = 0;
    int mode_no_post = strstr(url, "_NP") != NULL;
    int mode_start   = strstr(url, "_S")  != NULL;
    int mode_buf5    = strstr(url, "_5")  != NULL;
    int mode_no_lang = strstr(url, "_NL") != NULL;
    snprintf(g_dbgMode, sizeof(g_dbgMode), "%s%s%s%s",
             mode_no_post ? "np" : "post",
             mode_start ? "+start" : "+auto",
             mode_buf5 ? "+5" : "+2",
             mode_no_lang ? "+nl" : "+en");

    if      (strncmp(url, "TESTA", 5) == 0) { src = "/app0/assets/avtest.mp4"; is_local = 1; }
    else if (strncmp(url, "TESTB", 5) == 0) { src = "/app0/assets/base.mp4";  is_local = 1; }
    else if (strncmp(url, "TESTM", 5) == 0) { src = "/app0/assets/main.mp4";  is_local = 1; }
    else if (strncmp(url, "TESTH", 5) == 0) { src = "/app0/assets/high.mp4";  is_local = 1; }
    else if (strncmp(url, "TEST", 4) == 0) { src = "/app0/assets/test.mp4"; is_local = 1; }

    int is_http  = (strncmp(src, "http://", 7) == 0);
    int is_https = (strncmp(src, "https://", 8) == 0);
    int is_remote = is_http || is_https;

    // No TLS reader in homebrew — tell the user instead of failing as "bad
    // source", which is misleading. Plain http (incl. local media servers) works.
    if (is_https) {
        snprintf(g_status, sizeof(g_status), "https not supported - use an http:// link");
        notify("Cast: https rejected (no TLS reader)");
        return -3;
    }

    notify_dbg("Cast: 1/4 init (%s)", is_local ? "local file" : (is_remote ? "remote url" : "source"));

    int initRc = player_init();
    if (initRc != 0) {
        if (strncmp(g_status, "ready", 5) == 0 || strcmp(g_status, "stopped") == 0 || strcmp(g_status, "idle") == 0)
            snprintf(g_status, sizeof(g_status), "player init failed %d", initRc);
        return -1;
    }
    tex_pool_reset();

    // Over-sized, zeroed init block so any ABI fields we don't model read as 0
    // and the library can't scribble past our storage.
    unsigned char initBuf[512];
    memset(initBuf, 0, sizeof(initBuf));
    SceAvPlayerInitData *init = (SceAvPlayerInitData *)initBuf;
    init->memoryReplacement.allocate          = cb_allocate;
    init->memoryReplacement.deallocate        = cb_deallocate;
    init->memoryReplacement.allocateTexture   = cb_allocate_tex;
    init->memoryReplacement.deallocateTexture = cb_deallocate_tex;
    if (is_http) {
        // Remote http: app-managed ranged-HTTP reader.
        init->fileReplacement.open       = cb_http_open;
        init->fileReplacement.close      = cb_http_close;
        init->fileReplacement.readOffset = cb_http_readoffset;
        init->fileReplacement.size       = cb_http_size;
    } else if (!is_remote) {
        // Local /app0 bundled clip: app-managed file reader.
        init->fileReplacement.open       = cb_file_open;
        init->fileReplacement.close      = cb_file_close;
        init->fileReplacement.readOffset = cb_file_readoffset;
        init->fileReplacement.size       = cb_file_size;
    }
    init->eventReplacement.eventCallback      = on_event;
    init->debugLevel = 0;
    init->basePriority = 700;
    init->numOutputVideoFrameBuffers = mode_buf5 ? 5 : 2;
    init->autoStart = mode_start ? 0 : 1;
    init->defaultLanguage = mode_no_lang ? NULL : "en";

    g_player = sceAvPlayerInit(init);
    if (g_player <= 0) {   // valid handle is a non-null pointer
        snprintf(g_status, sizeof(g_status), "init failed");
        notify("Cast: init FAILED (0x%llx)", (unsigned long long)g_player);
        g_player = -1;
        return -1;
    }

    if (mode_no_post) {
        g_prc = 0x7ffffffe;
    } else {
        unsigned char postBuf[256];
        memset(postBuf, 0, sizeof(postBuf));
        g_prc = sceAvPlayerPostInit(g_player, postBuf);
    }

    g_arc = sceAvPlayerAddSource(g_player, src);
    if (g_arc < 0) {
        // Surface the real rc (and http reader state) instead of a bare "bad
        // source" so on-device runs tell us why AddSource rejected the source.
        snprintf(g_status, sizeof(g_status), "addsource failed 0x%x [%s]",
                 (unsigned)g_arc, is_http ? httpsrc_debug() : "local");
        sceAvPlayerClose(g_player);
        g_player = -1;
        return -2;
    }

    g_strc = mode_start ? sceAvPlayerStart(g_player) : 0;
    if (g_strc < 0)
        notify("Cast: Start rc=0x%x", (unsigned)g_strc);
    g_active = 1;
    snprintf(g_status, sizeof(g_status), "buffering...");
    return 0;
}

void player_stop(void) {
    if (g_player > 0) {
        sceAvPlayerStop(g_player);
        sceAvPlayerClose(g_player);
        g_player = -1;
    }
    if (g_fileFd >= 0) { sceKernelClose(g_fileFd); g_fileFd = -1; }
    g_active = 0;
    g_gotFrame = 0;
    snprintf(g_status, sizeof(g_status), "stopped");
}

int player_is_active(void) {
    if (g_player <= 0) return 0;
    return sceAvPlayerIsActive(g_player) ? 1 : 0;
}

// Our own intent flag: true from Start until Stop/STOP-event. We drive the frame
// pump off this (not sceAvPlayerIsActive, which is false while buffering).
int player_started(void) { return g_active; }

const char *player_status(void) { return g_status; }

// Transport controls are implemented in the ffmpeg backend (player_ff.c). The
// legacy AvPlayer path only provides stubs so the app links with USE_FFMPEG=0.
void player_pause(int paused) { (void)paused; }
int  player_is_paused(void) { return 0; }
void player_seek(double seconds) { (void)seconds; }
void player_progress(double *cur, double *dur) { if (cur) *cur = 0; if (dur) *dur = 0; }
void player_interrupt(void) { }
int  player_buffering(void) { return 0; }
int  player_buffer_pct(void) { return 0; }

void player_debug(char *out, int len) {
    char seq[80]; int o = 0;
    for (int i = 0; i < g_evN && o < (int)sizeof(seq) - 6; i++)
        o += snprintf(seq + o, sizeof(seq) - o, "%s%d", i ? "," : "", g_evSeq[i]);
    seq[o] = '\0';
    snprintf(out, len,
             "mode=%s | act=%d vget=%d aud=%ld vc=%ld | open=%d sz=%lld rd=%ld by=%ldK last=%llu/%u=%d | seq=[%s] | prc=%d arc=%d strc=%d val=0x%x",
             g_dbgMode,
             g_dbgActive, g_dbgVget, g_dbgAudio, g_dbgVcalls,
             g_dbgOpens, g_dbgFileSize, g_dbgReads, g_dbgBytes / 1024,
             g_dbgLastPos, g_dbgLastLen, g_dbgLastRead,
             seq, g_prc, g_arc, g_strc, (unsigned)g_evDataVal);
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

int player_render(Gfx *g) {
    if (g_player <= 0) return 0;

    // Drain audio every pump. AvPlayer's demuxer stalls (and video never
    // advances) if the decoded audio queue is never consumed. We discard it for
    // now — real audio output is a follow-up.
    {
        unsigned char audioBuf[256];
        for (int i = 0; i < 8; i++) {
            memset(audioBuf, 0, sizeof(audioBuf));
            if (!sceAvPlayerGetAudioData(g_player, (SceAvPlayerFrameInfo *)audioBuf))
                break;
            g_dbgAudio++;
        }
    }

    g_dbgActive = sceAvPlayerIsActive(g_player) ? 1 : 0;

    // Over-sized frame-info storage to absorb ABI size drift.
    unsigned char frameBuf[256];
    memset(frameBuf, 0, sizeof(frameBuf));
    SceAvPlayerFrameInfo *frame = (SceAvPlayerFrameInfo *)frameBuf;

    g_dbgVcalls++;
    int got = sceAvPlayerGetVideoData(g_player, frame) ? 1 : 0;
    g_dbgVget = got;
    if (!got) {
        return 0;
    }
    g_dbgPdata = frame->pData ? 1 : 0;
    g_dbgW = (int)frame->details.video.width;
    g_dbgH = (int)frame->details.video.height;
    if (!frame->pData) return 0;

    int sw = g_dbgW;
    int sh = g_dbgH;
    if (sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096) return 0;   // sanity guard

    if (!g_gotFrame) {
        g_gotFrame = 1;
        notify_dbg("Cast: first frame %dx%d", sw, sh);
        snprintf(g_status, sizeof(g_status), "playing %dx%d", sw, sh);
    }

    // We have a real frame: clear the whole buffer (clean letterbox bars) then blit.
    GfxColor black = { 0, 0, 0 };
    gfx_clear(g, black);

    const uint8_t *Y  = frame->pData;
    const uint8_t *UV = frame->pData + (size_t)sw * sh;

    int dw = g->width, dh = g->height;
    int scaledW = dw, scaledH = (int)((int64_t)dw * sh / sw);
    if (scaledH > dh) { scaledH = dh; scaledW = (int)((int64_t)dh * sw / sh); }
    int ox = (dw - scaledW) / 2;
    int oy = (dh - scaledH) / 2;

    for (int dy = 0; dy < scaledH; dy++) {
        int sy = (int)((int64_t)dy * sh / scaledH);
        const uint8_t *yrow  = Y + (size_t)sy * sw;
        const uint8_t *uvrow = UV + (size_t)(sy >> 1) * sw;
        for (int dx = 0; dx < scaledW; dx++) {
            int sx = (int)((int64_t)dx * sw / scaledW);
            int yv = yrow[sx];
            int uvx = (sx >> 1) << 1;
            int u = uvrow[uvx] - 128;
            int v = uvrow[uvx + 1] - 128;
            int c = yv - 16;
            GfxColor col;
            col.r = clamp8((298 * c + 409 * v + 128) >> 8);
            col.g = clamp8((298 * c - 100 * u - 208 * v + 128) >> 8);
            col.b = clamp8((298 * c + 516 * u + 128) >> 8);
            gfx_pixel(g, ox + dx, oy + dy, col);
        }
    }
    return 1;
}
