// ssdp.h - minimal UPnP/DLNA discovery responder.
#ifndef PS4CAST_SSDP_H
#define PS4CAST_SSDP_H

int ssdp_start(const char *ip, int http_port);
const char *ssdp_status(void);

#endif
