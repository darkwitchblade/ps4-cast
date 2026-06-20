#include <stddef.h>
#include <stdint.h>
#include "/private/tmp/dpi-src/DirectPackageInstaller-DN6/Payload/ps4-libjbc/jailbreak.h"
#include "/private/tmp/dpi-src/DirectPackageInstaller-DN6/Payload/ps4-libjbc/kernelrw.h"

#if defined(CONTROL_LAUNCH_MEDIA)
#define TARGET_TITLE_ID "CUSA02012"
#elif defined(CONTROL_LAUNCH_LAPY20002)
#define TARGET_TITLE_ID "LAPY20002"
#elif defined(CONTROL_LAUNCH_NPXX51363)
#define TARGET_TITLE_ID "NPXX51363"
#elif !defined(TARGET_TITLE_ID)
#define TARGET_TITLE_ID "PCST00001"
#endif
#define TITLE_ID TARGET_TITLE_ID
#define CONTENT_ID "IV0000-PCST00001_00-PS4CAST000000001"
#define ORBIS_KERNEL_PRIO_FIFO_NORMAL 0x2BC
#define CALLBACK_IP 0x8B01A8C0U
#define CALLBACK_PORT 0xAB26U

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int socket(int, int, int);
int connect(int, const void *, unsigned int);
long write(int, const void *, unsigned long);
int close(int);

void *memset(void *dst, int value, size_t len)
{
    unsigned char *p = (unsigned char *)dst;
    while (len--) *p++ = (unsigned char)value;
    return dst;
}

asm("clear_stack:\nmov $0x800,%ecx\nxor %rax, %rax\n.L1:\npush %rax\nloop .L1\nadd $0x4000,%rsp\nret");
void clear_stack(void);

typedef struct OrbisUserServiceInitializeParams {
    uint32_t priority;
} OrbisUserServiceInitializeParams;

typedef struct LncAppParam {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t LaunchAppCheck_flag;
} LncAppParam;

typedef struct SockaddrIn {
    uint8_t len;
    uint8_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
} SockaddrIn;

static void int32_to_hex(int32_t value, char *hex)
{
    hex[0] = '0';
    hex[1] = 'x';
    for (int i = 7, x = 2; i >= 0; i--, x++) {
        int part = (value >> (4 * i)) & 0x0F;
        hex[x] = (part < 10) ? ('0' + part) : ('A' + (part - 10));
    }
    hex[10] = 0;
}

static void u32_to_hex(uint32_t value, char *hex)
{
    int32_to_hex((int32_t)value, hex);
}

static void append(const char *a, const char *b, char *out)
{
    int off = 0;
    for (int i = 0; a[i]; i++) out[off++] = a[i];
    for (int i = 0; b[i]; i++) out[off++] = b[i];
    out[off] = 0;
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void append_inplace(char *out, const char *s)
{
    int off = slen(out);
    for (int i = 0; s[i]; i++) out[off++] = s[i];
    out[off] = 0;
}

static void append_hex64(char *out, uint64_t value)
{
    char hex[19] = {0};
    hex[0] = '0';
    hex[1] = 'x';
    for (int i = 15, x = 2; i >= 0; i--, x++) {
        int part = (value >> (4 * i)) & 0x0F;
        hex[x] = (part < 10) ? ('0' + part) : ('A' + (part - 10));
    }
    append_inplace(out, hex);
}

static void append_dec(char *out, int value)
{
    char tmp[16];
    int n = 0;
    unsigned int v;
    if (value < 0) {
        append_inplace(out, "-");
        v = (unsigned int)(-value);
    } else {
        v = (unsigned int)value;
    }
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < (int)sizeof(tmp));
    while (n--) {
        char c[2] = { tmp[n], 0 };
        append_inplace(out, c);
    }
}

static int contains_bytes(const unsigned char *buf, int len, const char *needle)
{
    int nl = slen(needle);
    if (nl <= 0 || nl > len) return 0;
    for (int i = 0; i <= len - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            if (buf[i + j] != (unsigned char)needle[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static int contains_u32(const unsigned char *buf, int len, uint32_t value)
{
    for (int i = 0; i <= len - 4; i++) {
        uint32_t v = (uint32_t)buf[i] |
                     ((uint32_t)buf[i + 1] << 8) |
                     ((uint32_t)buf[i + 2] << 16) |
                     ((uint32_t)buf[i + 3] << 24);
        if (v == value) return 1;
    }
    return 0;
}

static void callback(const char *msg)
{
    int fd = socket(2, 1, 0);
    if (fd < 0) return;
    SockaddrIn addr = {
        .len = sizeof(SockaddrIn),
        .family = 2,
        .port = CALLBACK_PORT,
        .addr = CALLBACK_IP,
        .zero = {0}
    };
    if (connect(fd, &addr, sizeof(addr)) == 0) {
        write(fd, msg, (unsigned long)slen(msg));
    }
    close(fd);
}

static void notify(int (*send)(int, const char *), const char *prefix, int32_t rv)
{
    char hex[0x20] = {0};
    char msg[0x200] = {0};
    int32_to_hex(rv, hex);
    append(prefix, hex, msg);
    send(222, msg);
}

static int elevate(void)
{
    struct jbc_cred cred;
    int rv = jbc_get_cred(&cred);
    if (rv) return rv;
    rv = jbc_jailbreak_cred(&cred);
    if (rv) return rv;

    cred.jdir = 0;
    cred.sceProcType = 0x3800000000000010;
    cred.sonyCred = 0x40001c0000000000;
    cred.sceProcCap = 0x900000000000ff00;
    return jbc_set_cred(&cred);
}

static uintptr_t current_proc(void)
{
    uintptr_t td = jbc_krw_get_td();
    return (uintptr_t)jbc_krw_read64(td + 8, KERNEL_HEAP);
}

static uintptr_t find_pid1_proc(void)
{
    uintptr_t proc = current_proc();
    for (int i = 0; i < 512 && proc; i++) {
        int pid = 0;
        if (jbc_krw_memcpy((uintptr_t)&pid, proc + 0xb0, sizeof(pid), KERNEL_HEAP))
            return 0;
        if (pid == 1)
            return proc;
        uintptr_t next = (uintptr_t)jbc_krw_read64(proc, KERNEL_HEAP);
        uintptr_t back = next ? (uintptr_t)jbc_krw_read64(next + 8, KERNEL_HEAP) : 0;
        if (!next || back != proc)
            return 0;
        proc = next;
    }
    return 0;
}

static int proc_pid(uintptr_t proc)
{
    int pid = -1;
    if (proc)
        jbc_krw_memcpy((uintptr_t)&pid, proc + 0xb0, sizeof(pid), KERNEL_HEAP);
    return pid;
}

static uint32_t get_lnc_appid(char *report)
{
    uint32_t appid = 0;
    void *lnc = dlopen("/system/common/lib/libSceLncUtil.sprx", 0);
    int (*lnc_init)(void) = dlsym(lnc, "sceLncUtilInitialize");
    int (*lnc_get_app_id)(const char *) = dlsym(lnc, "sceLncUtilGetAppId");
    if (lnc_init) lnc_init();
    if (lnc_get_app_id) appid = (uint32_t)lnc_get_app_id(TITLE_ID);
    void *system = dlopen("/system/common/lib/libSceSystemService.sprx", 0);
    int (*get_big_app)(void) = dlsym(system, "sceSystemServiceGetAppIdOfBigApp");
    int (*get_mini_app)(void) = dlsym(system, "sceSystemServiceGetAppIdOfMiniApp");
    append_inplace(report, "lnc_appid=");
    append_hex64(report, appid);
    if (get_big_app) {
        append_inplace(report, " big=");
        append_hex64(report, (uint32_t)get_big_app());
    }
    if (get_mini_app) {
        append_inplace(report, " mini=");
        append_hex64(report, (uint32_t)get_mini_app());
    }
    append_inplace(report, "\n");
    return appid;
}

static uintptr_t find_cast_proc(char *report)
{
    uint32_t appid = get_lnc_appid(report);
    uintptr_t pid1 = find_pid1_proc();
    append_inplace(report, "pid1=");
    append_hex64(report, pid1);
    append_inplace(report, "\n");
    if (!pid1) return 0;

    uintptr_t best = 0;
    uintptr_t proc = pid1;
    for (int i = 0; i < 384 && proc; i++) {
        int pid = proc_pid(proc);
        unsigned char page[0x1000];
        int have = jbc_krw_memcpy((uintptr_t)page, proc, sizeof(page), KERNEL_HEAP) == 0;
        int hit_title = have && contains_bytes(page, sizeof(page), TITLE_ID);
        int hit_content = have && contains_bytes(page, sizeof(page), CONTENT_ID);
        int hit_eboot = have && contains_bytes(page, sizeof(page), "eboot.bin");
        int hit_appid = appid && have && contains_u32(page, sizeof(page), appid);
        uintptr_t ucred = (uintptr_t)jbc_krw_read64(proc + 0x40, KERNEL_HEAP);
        uint64_t ptype = ucred ? jbc_krw_read64(ucred + 88, KERNEL_HEAP) : 0;
        uint64_t sony = ucred ? jbc_krw_read64(ucred + 96, KERNEL_HEAP) : 0;
        uint64_t cap = ucred ? jbc_krw_read64(ucred + 104, KERNEL_HEAP) : 0;

        if (hit_title || hit_content || hit_eboot || hit_appid || (ptype == 0x3800000000000011ULL)) {
            append_inplace(report, "proc[");
            append_dec(report, i);
            append_inplace(report, "] pid=");
            append_dec(report, pid);
            append_inplace(report, " proc=");
            append_hex64(report, proc);
            append_inplace(report, " ucred=");
            append_hex64(report, ucred);
            append_inplace(report, " ptype=");
            append_hex64(report, ptype);
            append_inplace(report, " sony=");
            append_hex64(report, sony);
            append_inplace(report, " cap=");
            append_hex64(report, cap);
            append_inplace(report, " hits=");
            if (hit_title) append_inplace(report, "title,");
            if (hit_content) append_inplace(report, "content,");
            if (hit_eboot) append_inplace(report, "eboot,");
            if (hit_appid) append_inplace(report, "appid,");
            append_inplace(report, "\n");
            if (!best && (hit_title || hit_content || hit_appid || ptype == 0x3800000000000011ULL))
                best = proc;
        }

        uintptr_t next = (uintptr_t)jbc_krw_read64(proc, KERNEL_HEAP);
        if (!next || next == pid1)
            break;
        proc = next;
    }
    append_inplace(report, "best=");
    append_hex64(report, best);
    append_inplace(report, " pid=");
    append_dec(report, proc_pid(best));
    append_inplace(report, "\n");
    return best;
}

static int patch_cast_proc(char *report)
{
    uintptr_t proc = find_cast_proc(report);
    if (!proc) return -10;
    uintptr_t ucred = (uintptr_t)jbc_krw_read64(proc + 0x40, KERNEL_HEAP);
    uintptr_t fd = (uintptr_t)jbc_krw_read64(proc + 0x48, KERNEL_HEAP);
    uintptr_t prison0 = jbc_get_prison0();
    uintptr_t rootvnode = jbc_get_rootvnode();
    if (!ucred || !fd || !prison0 || !rootvnode) return -11;

    uint32_t zero = 0;
    uint64_t ptype = 0x3801000000000013ULL;
    uint64_t sony = 0xffffffffffffffffULL;
    uint64_t cap = 0xffffffffffffffffULL;
    int rv = 0;
    rv |= jbc_krw_memcpy(ucred + 4, (uintptr_t)&zero, sizeof(zero), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 8, (uintptr_t)&zero, sizeof(zero), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 12, (uintptr_t)&zero, sizeof(zero), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 20, (uintptr_t)&zero, sizeof(zero), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 24, (uintptr_t)&zero, sizeof(zero), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 0x30, (uintptr_t)&prison0, sizeof(prison0), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(fd + 0x10, (uintptr_t)&rootvnode, sizeof(rootvnode), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(fd + 0x18, (uintptr_t)&rootvnode, sizeof(rootvnode), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(fd + 0x20, (uintptr_t)&rootvnode, sizeof(rootvnode), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 88, (uintptr_t)&ptype, sizeof(ptype), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 96, (uintptr_t)&sony, sizeof(sony), KERNEL_HEAP);
    rv |= jbc_krw_memcpy(ucred + 104, (uintptr_t)&cap, sizeof(cap), KERNEL_HEAP);

    append_inplace(report, "patch rv=");
    append_dec(report, rv);
    append_inplace(report, " proc=");
    append_hex64(report, proc);
    append_inplace(report, " ucred=");
    append_hex64(report, ucred);
    append_inplace(report, "\n");
    return rv;
}

int main(void)
{
    int rv = elevate();
    clear_stack();

    void *sysutil = dlopen("/system/common/lib/libSceSysUtil.sprx", 0);
    int (*send)(int, const char *) = dlsym(sysutil, "sceSysUtilSendSystemNotificationWithText");
    if (!send) return -1;
    if (rv) {
        notify(send, "PS4 Cast ctrl elevate failed ", rv);
        callback("elevate failed\n");
        return rv;
    }

    void *usersrv = dlopen("/system/common/lib/libSceUserService.sprx", 0);
    int (*user_init)(OrbisUserServiceInitializeParams *) = dlsym(usersrv, "sceUserServiceInitialize");
    int (*foreground_user)(int *) = dlsym(usersrv, "sceUserServiceGetForegroundUser");
    int (*user_term)(void) = dlsym(usersrv, "sceUserServiceTerminate");
    int uid = 0;
    if (user_init && foreground_user && user_term) {
        OrbisUserServiceInitializeParams params = { .priority = ORBIS_KERNEL_PRIO_FIFO_NORMAL };
        user_init(&params);
        foreground_user(&uid);
        user_term();
    }

#if defined(CONTROL_LAUNCH)
    void *lnc = dlopen("/system/common/lib/libSceLncUtil.sprx", 0);
    int (*lnc_init)(void) = dlsym(lnc, "sceLncUtilInitialize");
    int (*launch)(const char *, const char **, LncAppParam *) = dlsym(lnc, "sceLncUtilLaunchApp");
    if (!launch) launch = dlsym(lnc, "sceLncUtilStartLaunchAppByTitleId");
    if (lnc_init) lnc_init();
    if (!launch) {
        void *system = dlopen("/system/common/lib/libSceSystemService.sprx", 0);
        launch = dlsym(system, "sceSystemServiceLaunchApp");
        if (!launch) launch = dlsym(system, "sceLncUtilStartLaunchAppByTitleId");
        if (!launch) launch = dlsym(system, "sceLncUtilLaunchApp");
    }
    if (!launch) {
        send(222, "PS4 Cast ctrl launch symbol missing");
        callback("launch symbol missing\n");
        return -2;
    }
    LncAppParam param = {
        .size = sizeof(LncAppParam),
        .user_id = (uint32_t)uid,
        .app_opt = 0,
        .crash_report = 0,
        .LaunchAppCheck_flag = 2
    };
    rv = launch(TITLE_ID, NULL, &param);
    char cb[0x200] = {0};
    char hx[0x20] = {0};
    void *system2 = dlopen("/system/common/lib/libSceSystemService.sprx", 0);
    int (*get_big_app2)(void) = dlsym(system2, "sceSystemServiceGetAppIdOfBigApp");
    int (*get_mini_app2)(void) = dlsym(system2, "sceSystemServiceGetAppIdOfMiniApp");
    int32_to_hex(rv, hx);
    append_inplace(cb, "launch rv=");
    append_inplace(cb, hx);
    append_inplace(cb, " title=");
    append_inplace(cb, TITLE_ID);
    if (get_big_app2) {
        append_inplace(cb, " big=");
        append_hex64(cb, (uint32_t)get_big_app2());
    }
    if (get_mini_app2) {
        append_inplace(cb, " mini=");
        append_hex64(cb, (uint32_t)get_mini_app2());
    }
    append_inplace(cb, "\n");
    callback(cb);
    notify(send, "PS4 Cast launch rv ", rv);
    return rv;
#elif defined(CONTROL_KILL)
    void *lnc = dlopen("/system/common/lib/libSceLncUtil.sprx", 0);
    int (*lnc_init)(void) = dlsym(lnc, "sceLncUtilInitialize");
    int (*lnc_get_app_id)(const char *) = dlsym(lnc, "sceLncUtilGetAppId");
    int (*lnc_kill_app)(uint32_t, int32_t, int32_t, int32_t) = dlsym(lnc, "sceLncUtilKillApp");
    int (*lnc_force_kill_app)(uint32_t, int32_t, int32_t, int32_t) = dlsym(lnc, "sceLncUtilForceKillApp");
    if (lnc_init) lnc_init();
    void *system = dlopen("/system/common/lib/libSceSystemService.sprx", 0);
    int (*get_big_app)(void) = dlsym(system, "sceSystemServiceGetAppIdOfBigApp");
    int (*kill_app)(uint32_t, int32_t, int32_t, int32_t) = dlsym(system, "sceSystemServiceKillApp");
    if ((!lnc_get_app_id && !get_big_app) || (!lnc_kill_app && !lnc_force_kill_app && !kill_app)) {
        send(222, "PS4 Cast ctrl kill symbol missing");
        callback("kill symbol missing\n");
        return -2;
    }
    int appid = lnc_get_app_id ? lnc_get_app_id(TITLE_ID) : get_big_app();
    const char *method = "system";
    if (lnc_force_kill_app) {
        method = "lnc_force";
        rv = lnc_force_kill_app((uint32_t)appid, -1, 0, 0);
    } else if (lnc_kill_app) {
        method = "lnc_kill";
        rv = lnc_kill_app((uint32_t)appid, -1, 0, 0);
    } else {
        rv = kill_app((uint32_t)appid, -1, 0, 0);
    }
    char apphex[0x20] = {0};
    char msg[0x200] = {0};
    char rvhex[0x20] = {0};
    u32_to_hex((uint32_t)appid, apphex);
    int32_to_hex(rv, rvhex);
    append_inplace(msg, "kill method=");
    append_inplace(msg, method);
    append_inplace(msg, " appid=");
    append_inplace(msg, apphex);
    append_inplace(msg, " rv=");
    append_inplace(msg, rvhex);
    append_inplace(msg, "\n");
    callback(msg);
    msg[0] = 0;
    append("PS4 Cast kill appid ", apphex, msg);
    send(222, msg);
    notify(send, "PS4 Cast kill rv ", rv);
    return rv;
#elif defined(CONTROL_UNINSTALL)
    void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx", 0);
    int (*appinst_init)(void) = dlsym(appinst, "sceAppInstUtilInitialize");
    int (*uninstall_user)(const char *, int32_t) = dlsym(appinst, "sceAppInstUtilAppUnInstallByUser");
    int (*uninstall)(const char *) = dlsym(appinst, "sceAppInstUtilAppUnInstall");
    if (appinst_init) {
        rv = appinst_init();
        if (rv) {
            notify(send, "PS4 Cast uninstall init rv ", rv);
            return rv;
        }
    }
    if (uninstall_user) rv = uninstall_user(TITLE_ID, uid);
    else if (uninstall) rv = uninstall(TITLE_ID);
    else {
        send(222, "PS4 Cast ctrl uninstall symbol missing");
        callback("uninstall symbol missing\n");
        return -2;
    }
    char cb[0x200] = {0};
    char hx[0x20] = {0};
    int32_to_hex(rv, hx);
    append_inplace(cb, "uninstall rv=");
    append_inplace(cb, hx);
    append_inplace(cb, "\n");
    callback(cb);
    notify(send, "PS4 Cast uninstall rv ", rv);
    return rv;
#elif defined(CONTROL_INSTALL_MEDIA_PLAYER)
    void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx", 0);
    int (*appinst_init)(void) = dlsym(appinst, "sceAppInstUtilInitialize");
    int (*install_media_player)(void) = dlsym(appinst, "sceAppInstUtilAppInstallMediaPlayer");
    if (appinst_init) {
        rv = appinst_init();
        if (rv) {
            notify(send, "Media Player install init rv ", rv);
            return rv;
        }
    }
    if (!install_media_player) {
        send(222, "Media Player install symbol missing");
        callback("install media symbol missing\n");
        return -2;
    }
    rv = install_media_player();
    char cb[0x200] = {0};
    char hx[0x20] = {0};
    int32_to_hex(rv, hx);
    append_inplace(cb, "install media rv=");
    append_inplace(cb, hx);
    append_inplace(cb, "\n");
    callback(cb);
    notify(send, "Media Player install rv ", rv);
    return rv;
#elif defined(CONTROL_INSTALL_PKG)
    void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx", 0);
    int (*appinst_init)(void) = dlsym(appinst, "sceAppInstUtilInitialize");
    int (*install_pkg)(const char *, void *) = dlsym(appinst, "sceAppInstUtilAppInstallPkg");
    if (appinst_init) {
        rv = appinst_init();
        if (rv) {
            notify(send, "PS4 Cast pkg install init rv ", rv);
            return rv;
        }
    }
    if (!install_pkg) {
        send(222, "PS4 Cast pkg install symbol missing");
        callback("install pkg symbol missing\n");
        return -2;
    }
    rv = install_pkg("/data/PS4-Cast.pkg", NULL);
    char cb[0x200] = {0};
    char hx[0x20] = {0};
    int32_to_hex(rv, hx);
    append_inplace(cb, "install pkg rv=");
    append_inplace(cb, hx);
    append_inplace(cb, "\n");
    callback(cb);
    notify(send, "PS4 Cast pkg install rv ", rv);
    return rv;
#elif defined(CONTROL_APP_STATUS)
    char report[0x400] = {0};
    get_lnc_appid(report);
    callback(report);
    send(222, "PS4 Cast app status sent");
    return 0;
#elif defined(CONTROL_PROBE)
    char report[0x1000] = {0};
    append_inplace(report, "probe\n");
    find_cast_proc(report);
    callback(report);
    send(222, "PS4 Cast probe sent");
    return 0;
#elif defined(CONTROL_PATCH)
    char report[0x1000] = {0};
    append_inplace(report, "patch\n");
    rv = patch_cast_proc(report);
    callback(report);
    notify(send, "PS4 Cast patch rv ", rv);
    return rv;
#else
    send(222, "PS4 Cast ctrl no command");
    return -3;
#endif
}
