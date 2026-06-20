// netutil.h — bring up networking and read the console's LAN IP.
#ifndef PS4CAST_NETUTIL_H
#define PS4CAST_NETUTIL_H

int  net_init(void);                         // 0 on success
int  net_get_ip(char *out, int outlen);      // fills "192.168.x.y"; 0 on success

#endif
