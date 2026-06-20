#include "netutil.h"

#include <string.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>

int net_init(void) {
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0)
        return -1;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL) < 0)
        return -2;

    if (sceNetInit() < 0)
        return -3;
    if (sceNetCtlInit() < 0)
        return -4;
    return 0;
}

int net_get_ip(char *out, int outlen) {
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    int rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc < 0)
        return rc;
    strncpy(out, info.ip_address, outlen - 1);
    out[outlen - 1] = '\0';
    return 0;
}
