#include "escalate.h"
#include "notify.h"

#include <stddef.h>
#include <orbis/libkernel.h>

#define JB_PRX_PATH "/app0/assets/jb.prx"

static int g_jb = 0x7fffffff;  // not run

typedef int (*pfn_jb)(void);

int jailbreak(void) {
    int handle = (int)sceKernelLoadStartModule(JB_PRX_PATH, 0, NULL, 0, NULL, NULL);
    if (handle < 0) {
        g_jb = handle;
        notify("Escalate: jb.prx load FAILED 0x%x", (unsigned)handle);
        return handle;
    }
    pfn_jb jb = NULL;
    sceKernelDlsym(handle, "jailbreak_me", (void **)&jb);
    if (!jb) {
        g_jb = -2;
        notify("Escalate: jailbreak_me symbol not found");
        return -2;
    }
    g_jb = jb();
    notify("Escalate: jailbreak_me() = %d", g_jb);
    return g_jb;
}

int jb_result(void) { return g_jb; }
