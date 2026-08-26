#include "native_http.h"
#include "urlopt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

#define NHTTP_CAP       (16 * 1024 * 1024)
#define NHTTP_NET_POOL  (64 * 1024)
#define NHTTP_SSL_POOL  (256 * 1024)
#define NHTTP_HTTP_POOL (512 * 1024)

typedef int32_t (*fn_ssl_init)(size_t);
typedef int32_t (*fn_http_init)(int32_t, int32_t, size_t);
typedef int32_t (*fn_create_template)(int32_t, const char *, int32_t, int32_t);
typedef int32_t (*fn_ssl_callback)(int32_t, void *, void *);
typedef int32_t (*fn_create_conn_url)(int32_t, const char *, int32_t);
typedef int32_t (*fn_create_req_url)(int32_t, int32_t, const char *, uint64_t);
typedef int32_t (*fn_add_header)(int32_t, const char *, const char *, int32_t);
typedef int32_t (*fn_set_timeout)(int32_t, uint32_t);
typedef int32_t (*fn_set_redirect)(int32_t, int32_t);
typedef int32_t (*fn_send)(int32_t, const void *, size_t);
typedef int32_t (*fn_get_status)(int32_t, int32_t *);
typedef int32_t (*fn_get_length)(int32_t, int32_t *, size_t *);
typedef int32_t (*fn_read)(int32_t, void *, uint32_t);
typedef int32_t (*fn_delete)(int32_t);
typedef int32_t (*fn_abort)(int32_t);

static fn_ssl_init       pSslInit;
static fn_http_init      pHttpInit;
static fn_create_template pCreateTemplate;
static fn_ssl_callback   pSetSslCallback;
static fn_create_conn_url pCreateConnection;
static fn_create_req_url pCreateRequest;
static fn_add_header     pAddHeader;
static fn_set_timeout    pSetConnectTimeout;
static fn_set_timeout    pSetResolveTimeout;
static fn_set_timeout    pSetSendTimeout;
static fn_set_timeout    pSetRecvTimeout;
static fn_set_redirect   pSetAutoRedirect;
static fn_send           pSendRequest;
static fn_get_status     pGetStatus;
static fn_get_length     pGetLength;
static fn_read           pReadData;
static fn_delete         pDeleteRequest;
static fn_delete         pDeleteConnection;
static fn_delete         pDeleteTemplate;
static fn_abort          pAbortRequest;

static int g_state; // 0 untried, 1 ready, -1 unavailable
static int g_netPool = -1, g_ssl = -1, g_http = -1;
static int g_template = -1, g_connection = -1;
static volatile int g_request = -1;
static char g_authority[320];
static char g_debug[192] = "native http idle";

static int accept_certificate(int ssl, unsigned verify_error,
                              void *const certs[], int cert_count, void *arg) {
    (void)ssl; (void)verify_error; (void)certs; (void)cert_count; (void)arg;
    return 1;
}

static int sym(int handle, const char *name, void **out) {
    *out = NULL;
    return sceKernelDlsym(handle, name, out) == 0 && *out ? 0 : -1;
}

static int find_loaded_module(const char *name) {
    OrbisKernelModule modules[256];
    size_t available = 0;
    if (sceKernelGetModuleList(modules, sizeof(modules), &available) < 0) return -1;
    if (available > sizeof(modules) / sizeof(modules[0]))
        available = sizeof(modules) / sizeof(modules[0]);
    for (size_t i = 0; i < available; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(modules[i], &info) == 0 && strstr(info.name, name))
            return (int)modules[i];
    }
    return -1;
}

static int load_api(void) {
    if (g_state) return g_state > 0 ? 0 : -1;
    // Retail apps cannot open these module files by path (ENOENT from the
    // sandbox). Ask Sysmodule to load them, then discover their process handles
    // for runtime symbol resolution. This is the same supported load route as
    // OpenOrbis's net_http sample without adding eager imports at boot.
    int ssl_load = (int32_t)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    int http_load = (int32_t)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    int hs = find_loaded_module("libSceSsl");
    int hh = find_loaded_module("libSceHttp");
    if (hs < 0 || hh < 0) {
        snprintf(g_debug, sizeof(g_debug),
                 "native sysmodule ssl=%#x/%#x http=%#x/%#x",
                 ssl_load, hs, http_load, hh);
        g_state = -1;
        return -1;
    }

#define LOAD(H, NAME, PTR) do { if (sym((H), (NAME), (void **)&(PTR)) != 0) goto missing; } while (0)
    LOAD(hs, "sceSslInit", pSslInit);
    LOAD(hh, "sceHttpInit", pHttpInit);
    LOAD(hh, "sceHttpCreateTemplate", pCreateTemplate);
    LOAD(hh, "sceHttpsSetSslCallback", pSetSslCallback);
    LOAD(hh, "sceHttpCreateConnectionWithURL", pCreateConnection);
    LOAD(hh, "sceHttpCreateRequestWithURL", pCreateRequest);
    LOAD(hh, "sceHttpAddRequestHeader", pAddHeader);
    LOAD(hh, "sceHttpSetConnectTimeOut", pSetConnectTimeout);
    LOAD(hh, "sceHttpSetResolveTimeOut", pSetResolveTimeout);
    LOAD(hh, "sceHttpSetSendTimeOut", pSetSendTimeout);
    LOAD(hh, "sceHttpSetRecvTimeOut", pSetRecvTimeout);
    LOAD(hh, "sceHttpSetAutoRedirect", pSetAutoRedirect);
    LOAD(hh, "sceHttpSendRequest", pSendRequest);
    LOAD(hh, "sceHttpGetStatusCode", pGetStatus);
    LOAD(hh, "sceHttpGetResponseContentLength", pGetLength);
    LOAD(hh, "sceHttpReadData", pReadData);
    LOAD(hh, "sceHttpDeleteRequest", pDeleteRequest);
    LOAD(hh, "sceHttpDeleteConnection", pDeleteConnection);
    LOAD(hh, "sceHttpDeleteTemplate", pDeleteTemplate);
    LOAD(hh, "sceHttpAbortRequest", pAbortRequest);
#undef LOAD

    g_netPool = sceNetPoolCreate("ps4cast_nhttp", NHTTP_NET_POOL, 0);
    if (g_netPool < 0) goto init_failed;
    g_ssl = pSslInit(NHTTP_SSL_POOL);
    if (g_ssl < 0) goto init_failed;
    g_http = pHttpInit(g_netPool, g_ssl, NHTTP_HTTP_POOL);
    if (g_http < 0) goto init_failed;
    g_template = pCreateTemplate(g_http, "PS4Cast/1.0", 2, 1);
    if (g_template < 0) goto init_failed;
    pSetSslCallback(g_template, (void *)accept_certificate, NULL);
    g_state = 1;
    snprintf(g_debug, sizeof(g_debug), "native http ready");
    return 0;

missing:
    snprintf(g_debug, sizeof(g_debug), "native http symbol missing");
    g_state = -1;
    return -1;
init_failed:
    snprintf(g_debug, sizeof(g_debug), "native init net=%#x ssl=%#x http=%#x tpl=%#x",
             g_netPool, g_ssl, g_http, g_template);
    g_state = -1;
    return -1;
}

static int authority_of(const char *url, char *out, int cap) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    const char *end = strchr(p + 3, '/');
    if (!end) end = url + strlen(url);
    int n = (int)(end - url);
    if (n <= 0 || n >= cap) return -1;
    memcpy(out, url, (size_t)n);
    out[n] = '\0';
    return 0;
}

static void reset_connection(void) {
    if (g_connection >= 0 && pDeleteConnection) pDeleteConnection(g_connection);
    g_connection = -1;
    g_authority[0] = '\0';
}

static void add_option_header(int req, const char *name) {
    const char *headers = urlopt_headers();
    size_t name_len = strlen(name);
    for (const char *p = headers; p && *p;) {
        const char *end = strstr(p, "\r\n");
        if (!end) break;
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && (size_t)(colon - p) == name_len && strncasecmp(p, name, name_len) == 0) {
            const char *v = colon + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            int n = (int)(end - v);
            if (n > 0 && n < 700) {
                char value[700];
                memcpy(value, v, (size_t)n); value[n] = '\0';
                pAddHeader(req, name, value, 1);
            }
            return;
        }
        p = end + 2;
    }
}

int native_http_fetch(const char *url, uint8_t **body, int *len,
                      int *status, uint64_t timeout_us) {
    if (!body || !len || !status || !url) return -1;
    *body = NULL; *len = 0; *status = 0;
    if (load_api() != 0) return -2;

    char authority[320];
    if (authority_of(url, authority, sizeof(authority)) != 0) return -3;
    if (g_connection < 0 || strcmp(authority, g_authority) != 0) {
        reset_connection();
        g_connection = pCreateConnection(g_template, url, 1);
        if (g_connection < 0) {
            snprintf(g_debug, sizeof(g_debug), "native conn rc=%#x", g_connection);
            return -4;
        }
        snprintf(g_authority, sizeof(g_authority), "%s", authority);
    }

    int req = pCreateRequest(g_connection, 0, url, 0);
    if (req < 0) {
        snprintf(g_debug, sizeof(g_debug), "native request rc=%#x", req);
        reset_connection();
        return -5;
    }
    g_request = req;

    uint32_t timeout = timeout_us > UINT32_MAX ? UINT32_MAX : (uint32_t)timeout_us;
    if (timeout < 1000) timeout = 1000;
    pSetConnectTimeout(req, timeout);
    pSetResolveTimeout(req, timeout);
    pSetSendTimeout(req, timeout);
    pSetRecvTimeout(req, timeout);
    pSetAutoRedirect(req, 1);
    pAddHeader(req, "Accept", "*/*", 1);
    add_option_header(req, "Referer");
    add_option_header(req, "Origin");
    add_option_header(req, "User-Agent");
    add_option_header(req, "Cookie");

    int rc = pSendRequest(req, NULL, 0);
    if (rc < 0 || pGetStatus(req, status) < 0) {
        snprintf(g_debug, sizeof(g_debug), "native send rc=%#x", rc);
        g_request = -1;
        pDeleteRequest(req);
        reset_connection();
        return -6;
    }

    int length_type = 0;
    size_t declared = 0;
    pGetLength(req, &length_type, &declared);
    if (declared > NHTTP_CAP) {
        snprintf(g_debug, sizeof(g_debug), "native too large status=%d bytes=%lu",
                 *status, (unsigned long)declared);
        g_request = -1;
        pDeleteRequest(req);
        return -7;
    }

    size_t cap = declared > 0 ? declared : 256 * 1024;
    if (cap < 4096) cap = 4096;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        g_request = -1;
        pDeleteRequest(req);
        return -8;
    }
    size_t used = 0;
    for (;;) {
        if (used == cap) {
            size_t next = cap < NHTTP_CAP / 2 ? cap * 2 : NHTTP_CAP;
            if (next <= cap) { rc = -9; break; }
            uint8_t *larger = realloc(buf, next);
            if (!larger) { rc = -8; break; }
            buf = larger; cap = next;
        }
        uint32_t want = (uint32_t)(cap - used);
        if (want > 64 * 1024) want = 64 * 1024;
        int got = pReadData(req, buf + used, want);
        if (got == 0) { rc = 0; break; }
        if (got < 0) { rc = got; break; }
        used += (size_t)got;
        if (used > NHTTP_CAP) { rc = -9; break; }
    }

    g_request = -1;
    pDeleteRequest(req);
    if (rc != 0 || used == 0) {
        snprintf(g_debug, sizeof(g_debug), "native read rc=%#x status=%d bytes=%lu",
                 rc, *status, (unsigned long)used);
        free(buf);
        reset_connection();
        return -10;
    }
    snprintf(g_debug, sizeof(g_debug), "native status=%d bytes=%lu", *status,
             (unsigned long)used);
    *body = buf;
    *len = (int)used;
    return 0;
}

void native_http_abort(void) {
    int req = g_request;
    if (req >= 0 && pAbortRequest) pAbortRequest(req);
    reset_connection();
}

const char *native_http_debug(void) { return g_debug; }
