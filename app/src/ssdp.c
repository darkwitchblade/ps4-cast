#include "ssdp.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

typedef struct {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;
    uint32_t addr;
    uint8_t  zero[8];
} ps4_sockaddr_in;

#define SSDP_PORT 1900
#define SOL_SOCKET_PS4   0xffff
#define SO_REUSEADDR_PS4 0x0004

// OrbisNet (SceNet) IP-level socket option numbers. These are Sony's own
// values and differ from BSD's <netinet/in.h>; the OpenOrbis headers don't
// export them, so define the ones we need. The multicast group JOIN is the
// piece the earlier in-app SSDP builds were missing: without it the socket is
// bound to :1900 but never actually receives the M-SEARCH multicast, so the
// PS4 stayed invisible to cast apps.
#define ORBIS_NET_IPPROTO_IP        0
#define ORBIS_NET_IP_MULTICAST_IF   9
#define ORBIS_NET_IP_MULTICAST_TTL  10
#define ORBIS_NET_IP_ADD_MEMBERSHIP 12

#define PS4CAST_UUID "uuid:7b2f63a8-2530-4e47-9f3a-0000000c5701"

// SceNet ip_mreq: two in_addr (network byte order), interface 0 = INADDR_ANY.
typedef struct {
    uint32_t imr_multiaddr;
    uint32_t imr_interface;
} ps4_ip_mreq;

// 239.255.255.250 in network byte order, built byte-wise so it is correct
// regardless of host endianness.
static uint32_t mcast_group_addr(void) {
    const uint8_t b[4] = { 239, 255, 255, 250 };
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}

static OrbisNetId g_sock = -1;
static OrbisPthread g_thread;
static char g_ip[32] = "0.0.0.0";
static int g_http_port = 8080;
static char g_diag[160] = "ssdp not started";
static unsigned g_seen = 0;
static char g_last_st[96] = "";
static char g_last_from[32] = "";

const char *ssdp_status(void) { return g_diag; }

static void addr_to_str(uint32_t addr, char *out, int cap) {
    const uint8_t *b = (const uint8_t *)&addr;
    snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void record_search(const ps4_sockaddr_in *from, const char *req) {
    g_seen++;
    addr_to_str(from->addr, g_last_from, sizeof(g_last_from));
    const char *st = NULL;
    for (const char *p = req; *p; p++) {
        int line_start = (p == req) || (p[-1] == '\n');
        if (line_start && (p[0] == 'S' || p[0] == 's') &&
            (p[1] == 'T' || p[1] == 't') && p[2] == ':') {
            st = p + 3;
            break;
        }
    }
    if (st) {
        while (*st == ' ' || *st == '\t') st++;
        const char *e = strstr(st, "\r\n");
        int n = e ? (int)(e - st) : (int)strlen(st);
        if (n >= (int)sizeof(g_last_st)) n = (int)sizeof(g_last_st) - 1;
        memcpy(g_last_st, st, n);
        g_last_st[n] = '\0';
    }
    snprintf(g_diag, sizeof(g_diag), "ssdp up ip=%s seen=%u from=%s st=%s",
             g_ip, g_seen, g_last_from, g_last_st);
}

static int contains_ci(const char *hay, const char *needle) {
    int nl = (int)strlen(needle);
    if (nl <= 0) return 1;
    for (int i = 0; hay[i]; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (!a) return 0;
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void send_ssdp_response(const ps4_sockaddr_in *to, const char *st) {
    char msg[1024];
    char usn[160];
    if (strcmp(st, PS4CAST_UUID) == 0)
        snprintf(usn, sizeof(usn), "%s", PS4CAST_UUID);
    else
        snprintf(usn, sizeof(usn), "%s::%s", PS4CAST_UUID, st);
    int n = snprintf(msg, sizeof(msg),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: FreeBSD/9.0 UPnP/1.0 PS4-Cast/1.0\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n",
        g_ip, g_http_port, st, usn);
    sceNetSendto(g_sock, msg, n, 0, (const OrbisNetSockaddr *)to, sizeof(*to));
}

// Proactively announce ourselves to the multicast group. Some control points
// (including several phone cast apps) populate their device list from these
// ssdp:alive advertisements rather than issuing their own M-SEARCH, so sending
// a burst at startup makes the PS4 appear without the user re-scanning.
static void send_ssdp_alive(const char *st) {
    char msg[1024];
    char usn[160];
    if (strcmp(st, PS4CAST_UUID) == 0)
        snprintf(usn, sizeof(usn), "%s", PS4CAST_UUID);
    else
        snprintf(usn, sizeof(usn), "%s::%s", PS4CAST_UUID, st);
    int n = snprintf(msg, sizeof(msg),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: FreeBSD/9.0 UPnP/1.0 PS4-Cast/1.0\r\n"
        "NT: %s\r\n"
        "NTS: ssdp:alive\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n",
        g_ip, g_http_port, st, usn);

    ps4_sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.len    = sizeof(to);
    to.family = ORBIS_NET_AF_INET;
    to.port   = sceNetHtons(SSDP_PORT);
    to.addr   = mcast_group_addr();
    sceNetSendto(g_sock, msg, n, 0, (const OrbisNetSockaddr *)&to, sizeof(to));
}

static void announce_all(void) {
    send_ssdp_alive(PS4CAST_UUID);
    send_ssdp_alive("upnp:rootdevice");
    send_ssdp_alive("urn:schemas-upnp-org:device:MediaRenderer:1");
    send_ssdp_alive("urn:schemas-upnp-org:service:ConnectionManager:1");
    send_ssdp_alive("urn:schemas-upnp-org:service:AVTransport:1");
    send_ssdp_alive("urn:schemas-upnp-org:service:RenderingControl:1");
}

static void *ssdp_main(void *arg) {
    (void)arg;
    char buf[2048];
    // Initial advertisement burst (UPnP recommends repeating the alive set).
    for (int i = 0; i < 3; i++) announce_all();
    for (;;) {
        ps4_sockaddr_in from;
        OrbisNetSocklen_t fromlen = sizeof(from);
        int n = sceNetRecvfrom(g_sock, buf, sizeof(buf) - 1, 0,
                               (OrbisNetSockaddr *)&from, &fromlen);
        if (n <= 0) {
            // A healthy socket blocks here; on a transient error recvfrom can
            // return immediately, so back off to avoid pegging a core (the
            // busy-spin that destabilized the earlier in-app SSDP builds).
            sceKernelUsleep(200 * 1000);
            continue;
        }
        buf[n] = '\0';
        if (!contains_ci(buf, "M-SEARCH") || !contains_ci(buf, "SSDP:DISCOVER"))
            continue;
        record_search(&from, buf);

        if (contains_ci(buf, PS4CAST_UUID)) {
            send_ssdp_response(&from, PS4CAST_UUID);
        } else if (contains_ci(buf, "MediaRenderer")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:device:MediaRenderer:1");
        } else if (contains_ci(buf, "ConnectionManager")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:ConnectionManager:1");
        } else if (contains_ci(buf, "AVTransport")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:AVTransport:1");
        } else if (contains_ci(buf, "RenderingControl")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:RenderingControl:1");
        } else if (contains_ci(buf, "ssdp:all") || contains_ci(buf, "upnp:rootdevice")) {
            send_ssdp_response(&from, PS4CAST_UUID);
            send_ssdp_response(&from, "upnp:rootdevice");
            send_ssdp_response(&from, "urn:schemas-upnp-org:device:MediaRenderer:1");
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:ConnectionManager:1");
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:AVTransport:1");
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:RenderingControl:1");
        }
    }
    return NULL;
}

int ssdp_start(const char *ip, int http_port) {
    if (g_sock >= 0) return 0;
    strncpy(g_ip, ip ? ip : "0.0.0.0", sizeof(g_ip) - 1);
    g_ip[sizeof(g_ip) - 1] = '\0';
    g_http_port = http_port;

    g_sock = sceNetSocket("ps4cast_ssdp", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, 0);
    if (g_sock < 0) { snprintf(g_diag, sizeof(g_diag), "ssdp socket failed %d", g_sock); return -1; }

    int on = 1;
    sceNetSetsockopt(g_sock, SOL_SOCKET_PS4, SO_REUSEADDR_PS4, &on, sizeof(on));

    ps4_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.len = sizeof(addr);
    addr.family = ORBIS_NET_AF_INET;
    addr.port = sceNetHtons(SSDP_PORT);
    addr.addr = 0;

    if (sceNetBind(g_sock, (const OrbisNetSockaddr *)&addr, sizeof(addr)) < 0) {
        snprintf(g_diag, sizeof(g_diag), "ssdp bind :1900 failed");
        sceNetSocketClose(g_sock);
        g_sock = -1;
        return -2;
    }

    // Resolve our own LAN IP so the multicast join + send egress the active
    // interface explicitly. INADDR_ANY for the membership interface does not
    // reliably bind to wlan0 on this stack, which is likely why discovery still
    // failed even with the join added.
    uint32_t ifaddr = 0; // INADDR_ANY fallback
    sceNetInetPton(ORBIS_NET_AF_INET, g_ip, &ifaddr);

    // Join the SSDP multicast group so we actually receive M-SEARCH probes.
    ps4_ip_mreq mreq;
    mreq.imr_multiaddr = mcast_group_addr();
    mreq.imr_interface = ifaddr;
    int jrc = sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_ADD_MEMBERSHIP,
                               &mreq, sizeof(mreq));

    // Send alive/response advertisements out of the same interface, bounded TTL.
    int mif = sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_MULTICAST_IF,
                               &ifaddr, sizeof(ifaddr));
    int ttl = 2;
    sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_MULTICAST_TTL,
                     &ttl, sizeof(ttl));

    snprintf(g_diag, sizeof(g_diag), "ssdp up ip=%s join=%d mif=%d", g_ip, jrc, mif);

    if (scePthreadCreate(&g_thread, NULL, ssdp_main, NULL, "ps4cast_ssdp") != 0) {
        sceNetSocketClose(g_sock);
        g_sock = -1;
        return -3;
    }
    return 0;
}
