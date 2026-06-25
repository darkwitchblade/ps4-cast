#include "sys_diag.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Declared here (not via orbis/SystemService.h) because the toolchain header
// prototypes sceSystemServiceGetStatus() with NO arguments; redeclaring it with
// the real signature in a TU that includes that header would conflict. Keeping
// these externs local to this file sidesteps that — the linker resolves the same
// system symbols either way.
extern int32_t sceUserServiceGetForegroundUser(int32_t *userId);
extern int32_t sceUserServiceGetInitialUser(int32_t *userId);

// Real SceSystemServiceStatus layout (eventNum + a handful of bool flags). The
// generous reserved tail guards against the kernel writing a larger struct than
// the documented head — better an oversized stack buffer than a smashed frame.
typedef struct {
    int32_t eventNum;
    uint8_t isSystemUiOverlaid;
    uint8_t isInBackgroundExecution;
    uint8_t isCpuMode7CpuNormal;
    uint8_t isGameLiveStreamingOnAir;
    uint8_t isOutOfVrPlayArea;
    uint8_t reserved[216];
} SysSvcStatus;
extern int32_t sceSystemServiceGetStatus(SysSvcStatus *status);

static char g_sys_diag[256] = "sys init";
static int  g_fg_user = -999;
static int  g_ui_overlaid = 0;
static int  g_in_background = 0;

void sys_diag_update(void) {
    int32_t fg = -999, iu = -999;
    int rfg = (int)sceUserServiceGetForegroundUser(&fg);
    int riu = (int)sceUserServiceGetInitialUser(&iu);

    SysSvcStatus st;
    memset(&st, 0, sizeof(st));
    int rs = (int)sceSystemServiceGetStatus(&st);

    g_fg_user = (rfg == 0) ? (int)fg : -999;
    g_ui_overlaid  = (rs == 0) ? (st.isSystemUiOverlaid ? 1 : 0) : -1;
    g_in_background = (rs == 0) ? (st.isInBackgroundExecution ? 1 : 0) : -1;

    snprintf(g_sys_diag, sizeof(g_sys_diag),
             "fg=0x%x(r%d) iu=0x%x(r%d) st(r%d) ev=%d ui=%d bg=%d live=%d",
             (unsigned)fg, rfg, (unsigned)iu, riu, rs,
             st.eventNum, st.isSystemUiOverlaid, st.isInBackgroundExecution,
             st.isGameLiveStreamingOnAir);
}

const char *sys_diag_get(void) { return g_sys_diag; }
int sys_fg_user(void)     { return g_fg_user; }
int sys_ui_overlaid(void) { return g_ui_overlaid; }
int sys_in_background(void){ return g_in_background; }

static volatile int g_fps = 0;
void sys_set_fps(int fps) { g_fps = fps; }
int  sys_get_fps(void)    { return g_fps; }
