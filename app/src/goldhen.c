#include "goldhen.h"
#include "notify.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

#define GOLDHEN_SDK_CMD_VERSION     0
#define GOLDHEN_SDK_CMD_JAILBREAK   2
#define GOLDHEN_SDK_CMD_UNJAILBREAK 3

struct jailbreak_backup {
    uint32_t cr_uid;
    uint32_t cr_ruid;
    uint32_t cr_rgid;
    uint32_t cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void *cr_prison;
    void *fd_cdir;
    void *fd_jdir;
    void *fd_rdir;
};

static char g_status[320] = "not run";
static struct jailbreak_backup g_backup;
static int g_have_backup = 0;

static long gh_syscall(int num, uint64_t cmd, void *data)
{
    long ret;
    register long rax __asm__("rax") = num;
    register long rdi __asm__("rdi") = (long)cmd;
    register long rsi __asm__("rsi") = (long)data;
    register long r10 __asm__("r10");
    (void)r10;
    __asm__ volatile (
        "syscall"
        : "+a"(rax)
        : "D"(rdi), "S"(rsi)
        : "rcx", "r11", "memory"
    );
    ret = rax;
    return ret;
}

static long goldhen_cmd(uint64_t cmd, void *data)
{
    return gh_syscall(500, cmd, data);
}

int goldhen_probe(char *out, int len)
{
    struct jailbreak_backup backup;
    memset(&backup, 0, sizeof(backup));

    long ver = goldhen_cmd(GOLDHEN_SDK_CMD_VERSION, NULL);
    long jb = goldhen_cmd(GOLDHEN_SDK_CMD_JAILBREAK, &backup);
    long ujb = -1;

    if (jb == 0) {
        memcpy(&g_backup, &backup, sizeof(g_backup));
        g_have_backup = 1;
        ujb = goldhen_cmd(GOLDHEN_SDK_CMD_UNJAILBREAK, &g_backup);
        if (ujb == 0)
            g_have_backup = 0;
    }

    snprintf(g_status, sizeof(g_status),
             "gh_ver=0x%lx jb=0x%lx ujb=0x%lx paid=0x%llx cap0=0x%llx cap1=0x%llx",
             ver,
             jb,
             ujb,
             (unsigned long long)backup.cr_paid,
             (unsigned long long)backup.cr_caps[0],
             (unsigned long long)backup.cr_caps[1]);

    notify("GoldHEN probe: %s", g_status);
    if (out && len > 0) {
        snprintf(out, len, "%s", g_status);
    }
    return (int)jb;
}

int goldhen_enter(char *out, int len)
{
    struct jailbreak_backup backup;
    memset(&backup, 0, sizeof(backup));

    long ver = goldhen_cmd(GOLDHEN_SDK_CMD_VERSION, NULL);
    long jb = goldhen_cmd(GOLDHEN_SDK_CMD_JAILBREAK, &backup);
    if (jb == 0) {
        memcpy(&g_backup, &backup, sizeof(g_backup));
        g_have_backup = 1;
    }

    snprintf(g_status, sizeof(g_status),
             "goldhen_enter gh_ver=0x%lx jb=0x%lx paid=0x%llx cap0=0x%llx cap1=0x%llx",
             ver,
             jb,
             (unsigned long long)backup.cr_paid,
             (unsigned long long)backup.cr_caps[0],
             (unsigned long long)backup.cr_caps[1]);
    notify("GoldHEN enter: %s", g_status);
    if (out && len > 0) {
        snprintf(out, len, "%s", g_status);
    }
    return (int)jb;
}

int goldhen_restore(char *out, int len)
{
    long ujb = 0;
    if (g_have_backup) {
        ujb = goldhen_cmd(GOLDHEN_SDK_CMD_UNJAILBREAK, &g_backup);
        if (ujb == 0)
            g_have_backup = 0;
    }

    snprintf(g_status, sizeof(g_status), "goldhen_restore ujb=0x%lx pending=%d", ujb, g_have_backup);
    notify("GoldHEN restore: %s", g_status);
    if (out && len > 0) {
        snprintf(out, len, "%s", g_status);
    }
    return (int)ujb;
}

const char *goldhen_status(void)
{
    return g_status;
}
