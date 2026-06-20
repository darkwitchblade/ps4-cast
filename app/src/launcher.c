#include "launcher.h"
#include "notify.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>   // syscall()

#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <orbis/_types/shell_ui.h>
#include <orbis/_types/sys_service.h>

extern int32_t sceSystemServiceLaunchWebBrowser(const char *url);
extern int32_t sceSystemServiceGetAppIdOfBigApp(void);
extern int32_t sceSystemServiceKillApp(uint32_t appid, int32_t opt, int32_t method, int32_t reason);

// FreeBSD/PS4 syscall numbers used by Itemzflow's launcher.
#define SYS_dynlib_load_prx  594
#define SYS_dynlib_dlsym     591

static char g_dbg[320] = "";
static char g_handoff_status[160] = "native idle";
const char *launch_debug(void) { return g_dbg; }
const char *handoff_status(void) { return g_handoff_status; }

typedef int (*pfn_lnc_init)(void);
typedef int32_t (*pfn_lnc_launch)(const char *title_id, const char *argv[], LncAppParam *param);

int launch_app(const char *title_id, const char *arg) {
    const char *PRX = "/system/common/lib/libSceLncUtil.sprx";

    int hw = (int)sceKernelLoadStartModule(PRX, 0, NULL, 0, NULL, NULL);
    int mid = -1;
    long hs = syscall(SYS_dynlib_load_prx, PRX, (long)0, (long)&mid, (long)0, (long)0, (long)0);
    int handle = (hw >= 0) ? hw : ((mid >= 0) ? mid : hw);

    int o = snprintf(g_dbg, sizeof(g_dbg),
                     "lnc load wrapper=0x%x sys=0x%lx mid=0x%x", (unsigned)hw, hs, (unsigned)mid);
    if (handle < 0) {
        snprintf(g_dbg + o, sizeof(g_dbg) - o, " | lnc prx load FAILED");
        return handle;
    }

    pfn_lnc_init pInit = NULL;
    pfn_lnc_launch pLaunch = NULL;
    sceKernelDlsym(handle, "sceLncUtilInitialize", (void **)&pInit);
    sceKernelDlsym(handle, "sceLncUtilLaunchApp", (void **)&pLaunch);
    if (!pInit) syscall(SYS_dynlib_dlsym, (long)handle, "sceLncUtilInitialize", (long)&pInit);
    if (!pLaunch) syscall(SYS_dynlib_dlsym, (long)handle, "sceLncUtilLaunchApp", (long)&pLaunch);
    o += snprintf(g_dbg + o, sizeof(g_dbg) - o, " | init=%p launch=%p", (void *)pInit, (void *)pLaunch);
    if (!pLaunch)
        return -2;

    int irc = pInit ? pInit() : 0;

    int32_t user_id = 0;
    sceUserServiceGetInitialUser(&user_id);
    LncAppParam param;
    memset(&param, 0, sizeof(param));
    param.size    = sizeof(param);
    param.user_id = (uint32_t)user_id;
    param.app_opt = 0;
    param.LaunchAppCheck_flag = LaunchApp_None;

    const char *argv[2] = { (arg && arg[0]) ? arg : NULL, NULL };
    int r = pLaunch(title_id, argv, &param);
    snprintf(g_dbg + o, sizeof(g_dbg) - o,
             " | initrc=%d launchapp(%s,%s)=0x%x", irc, title_id, (arg && arg[0]) ? "arg" : "noarg", (unsigned)r);
    return r;
}

int launch_web_browser(const char *url) {
    int r = sceSystemServiceLaunchWebBrowser(url);
    snprintf(g_dbg, sizeof(g_dbg), "sceSystemServiceLaunchWebBrowser(%s)=0x%x",
             (url && url[0]) ? "url" : "empty", (unsigned)r);
    return r;
}

int handoff_play_url(const char *url) {
    if (!url || !url[0]) {
        snprintf(g_handoff_status, sizeof(g_handoff_status), "native missing url");
        return -1;
    }
    snprintf(g_handoff_status, sizeof(g_handoff_status), "native handoff disabled");
    snprintf(g_dbg, sizeof(g_dbg), "browser handoff disabled after CE-36329-3");
    return -2;
}

int handoff_stop(void) {
    int appid = sceSystemServiceGetAppIdOfBigApp();
    if ((appid & ~0xFFFFFF) != 0x60000000) {
        snprintf(g_handoff_status, sizeof(g_handoff_status), "native no big app 0x%x", (unsigned)appid);
        return -1;
    }
    int r = sceSystemServiceKillApp((uint32_t)appid, -1, 0, 0);
    snprintf(g_handoff_status, sizeof(g_handoff_status), "native stop app=0x%x rc=0x%x", (unsigned)appid, (unsigned)r);
    return r;
}

typedef int (*pfn_init)(void);
typedef int (*pfn_launch)(const char *uri, OrbisShellUIUtilLaunchByUriParam *param);

int launch_by_uri(const char *uri) {
    const char *PRX = "/system/common/lib/libSceShellUIUtil.sprx";

    // Method A: libkernel wrapper.
    int hw = (int)sceKernelLoadStartModule(PRX, 0, NULL, 0, NULL, NULL);

    // Method B: raw syscall (Itemzflow path).
    int mid = -1;
    long hs = syscall(SYS_dynlib_load_prx, PRX, (long)0, (long)&mid, (long)0, (long)0, (long)0);

    int handle = (hw >= 0) ? hw : ((mid >= 0) ? mid : hw);

    int o = snprintf(g_dbg, sizeof(g_dbg),
                     "load wrapper=0x%x sys=0x%lx mid=0x%x", (unsigned)hw, hs, (unsigned)mid);

    if (handle < 0) {
        snprintf(g_dbg + o, sizeof(g_dbg) - o, " | prx load FAILED");
        return handle;
    }

    pfn_init   pInit   = NULL;
    pfn_launch pLaunch = NULL;
    sceKernelDlsym(handle, "sceShellUIUtilInitialize",  (void **)&pInit);
    sceKernelDlsym(handle, "sceShellUIUtilLaunchByUri", (void **)&pLaunch);
    if (!pInit || !pLaunch) {
        // fallback dlsym via syscall
        if (!pInit)   syscall(SYS_dynlib_dlsym, (long)handle, "sceShellUIUtilInitialize",  (long)&pInit);
        if (!pLaunch) syscall(SYS_dynlib_dlsym, (long)handle, "sceShellUIUtilLaunchByUri", (long)&pLaunch);
    }
    o += snprintf(g_dbg + o, sizeof(g_dbg) - o, " | init=%p launch=%p", (void *)pInit, (void *)pLaunch);
    if (!pInit || !pLaunch)
        return -2;

    int irc = pInit();

    int32_t user_id = 0;
    sceUserServiceGetInitialUser(&user_id);
    OrbisShellUIUtilLaunchByUriParam param;
    memset(&param, 0, sizeof(param));
    param.size   = sizeof(param);
    param.userId = (uint32_t)user_id;

    int r = pLaunch(uri, &param);
    snprintf(g_dbg + o, sizeof(g_dbg) - o, " | initrc=%d launchrc=0x%x", irc, (unsigned)r);
    return r;
}
