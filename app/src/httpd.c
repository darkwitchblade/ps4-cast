#include "httpd.h"
#include "web_ui.h"
#include "player.h"
#include "launcher.h"
#include "escalate.h"
#include "goldhen.h"
#include "ssdp.h"
#include "pad_diag.h"
#include "vdec_probe.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

// IPv4 sockaddr (16 bytes) — OpenOrbis has no OrbisNetSockaddrIn, so we lay it
// out by hand (matches FreeBSD / OrbisNetSockaddr size).
typedef struct {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;       // network byte order
    uint32_t addr;       // network byte order
    uint8_t  zero[8];
} ps4_sockaddr_in;

#define SOL_SOCKET_PS4   0xffff
#define SO_REUSEADDR_PS4 0x0004

static OrbisNetId        g_listen = -1;
static OrbisPthread      g_thread;
static OrbisPthreadMutex g_mtx;
static int               g_started = 0;

static char g_pending_url[1024];
static int  g_player_pending = 0;
static int  g_play_pending = 0;
static int  g_stop_pending = 0;
static int  g_quit_pending = 0;

// ---- recents / play-next queue / favorites --------------------------------
#define URL_MAX    1024
#define MAX_RECENT 12
#define MAX_QUEUE  16
#define MAX_FAV    32
#define FAV_PATH   "/data/ps4cast_favs.txt"
static char g_recent[MAX_RECENT][URL_MAX]; static int g_recentN = 0;
static char g_queue[MAX_QUEUE][URL_MAX];   static int g_queueHead = 0, g_queueN = 0;
static char g_fav[MAX_FAV][URL_MAX];       static int g_favN = 0;

static void recent_add(const char *url) {     // most-recent-first, deduped
    scePthreadMutexLock(&g_mtx);
    if (g_recentN == 0 || strcmp(g_recent[0], url) != 0) {
        int existing = -1;
        for (int i = 0; i < g_recentN; i++) if (strcmp(g_recent[i], url) == 0) { existing = i; break; }
        int top = existing >= 0 ? existing : (g_recentN < MAX_RECENT ? g_recentN++ : MAX_RECENT - 1);
        for (int i = top; i > 0; i--) strncpy(g_recent[i], g_recent[i-1], URL_MAX - 1);
        strncpy(g_recent[0], url, URL_MAX - 1); g_recent[0][URL_MAX-1] = '\0';
    }
    scePthreadMutexUnlock(&g_mtx);
}
static void queue_push(const char *url) {
    scePthreadMutexLock(&g_mtx);
    if (g_queueN < MAX_QUEUE) {
        strncpy(g_queue[(g_queueHead + g_queueN) % MAX_QUEUE], url, URL_MAX - 1);
        g_queue[(g_queueHead + g_queueN) % MAX_QUEUE][URL_MAX-1] = '\0';
        g_queueN++;
    }
    scePthreadMutexUnlock(&g_mtx);
}

static void favs_save(void) {
    int fd = sceKernelOpen(FAV_PATH, 0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd < 0) return;
    for (int i = 0; i < g_favN; i++) { sceKernelWrite(fd, g_fav[i], strlen(g_fav[i])); sceKernelWrite(fd, "\n", 1); }
    sceKernelClose(fd);
}
static void favs_load(void) {
    int fd = sceKernelOpen(FAV_PATH, 0 /*O_RDONLY*/, 0);
    if (fd < 0) return;
    static char buf[MAX_FAV * URL_MAX];
    int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    g_favN = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_favN < MAX_FAV; line = strtok_r(NULL, "\n", &save)) {
        for (int i = (int)strlen(line) - 1; i >= 0 && (line[i]=='\r'||line[i]==' '); i--) line[i] = '\0';
        if (line[0]) { strncpy(g_fav[g_favN], line, URL_MAX - 1); g_fav[g_favN][URL_MAX-1]='\0'; g_favN++; }
    }
}
static void fav_toggle(const char *url) {
    scePthreadMutexLock(&g_mtx);
    int idx = -1;
    for (int i = 0; i < g_favN; i++) if (strcmp(g_fav[i], url) == 0) { idx = i; break; }
    if (idx >= 0) { for (int i = idx; i < g_favN - 1; i++) strncpy(g_fav[i], g_fav[i+1], URL_MAX - 1); g_favN--; }
    else if (g_favN < MAX_FAV) { strncpy(g_fav[g_favN], url, URL_MAX - 1); g_fav[g_favN][URL_MAX-1]='\0'; g_favN++; }
    favs_save();
    scePthreadMutexUnlock(&g_mtx);
}

// Build a JSON array of strings into out. Minimal escaping of " and \.
static int json_list(char *out, int cap, char arr[][URL_MAX], int n) {
    int o = 0; o += snprintf(out + o, cap - o, "[");
    for (int i = 0; i < n && o < cap - 4; i++) {
        if (i) o += snprintf(out + o, cap - o, ",");
        o += snprintf(out + o, cap - o, "\"");
        for (const char *p = arr[i]; *p && o < cap - 8; p++) {
            if (*p == '"' || *p == '\\') out[o++] = '\\';
            out[o++] = *p;
        }
        o += snprintf(out + o, cap - o, "\"");
    }
    o += snprintf(out + o, cap - o, "]");
    return o;
}

// Pop the next queued URL (main loop calls this on playback finish for autoplay).
int httpd_take_next(char *out, int len) {
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_queueN > 0) {
        strncpy(out, g_queue[g_queueHead], len - 1); out[len-1] = '\0';
        g_queueHead = (g_queueHead + 1) % MAX_QUEUE; g_queueN--;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

static const char DEVICE_XML[] =
"<?xml version=\"1.0\"?>"
"<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<device>"
"<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
"<friendlyName>PS4 Cast</friendlyName>"
"<manufacturer>Sony Interactive Entertainment</manufacturer>"
"<manufacturerURL>https://github.com/</manufacturerURL>"
"<modelName>PS4 Cast Receiver</modelName>"
"<modelDescription>PS4 Cast DLNA Renderer</modelDescription>"
"<modelNumber>1</modelNumber>"
"<serialNumber>PCST00001</serialNumber>"
"<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
"<UDN>uuid:7b2f63a8-2530-4e47-9f3a-0000000c5701</UDN>"
"<presentationURL>/</presentationURL>"
"<serviceList>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
"<SCPDURL>/AVTransport.xml</SCPDURL>"
"<controlURL>/upnp/control/AVTransport</controlURL>"
"<eventSubURL>/upnp/event/AVTransport</eventSubURL>"
"</service>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
"<SCPDURL>/RenderingControl.xml</SCPDURL>"
"<controlURL>/upnp/control/RenderingControl</controlURL>"
"<eventSubURL>/upnp/event/RenderingControl</eventSubURL>"
"</service>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
"<SCPDURL>/ConnectionManager.xml</SCPDURL>"
"<controlURL>/upnp/control/ConnectionManager</controlURL>"
"<eventSubURL>/upnp/event/ConnectionManager</eventSubURL>"
"</service>"
"</serviceList>"
"</device>"
"</root>";

static const char AVTRANSPORT_XML[] =
"<?xml version=\"1.0\"?>"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>SetAVTransportURI</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
"<argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Play</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Stop</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Pause</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>Seek</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>"
"<argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetTransportInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
"<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
"<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetPositionInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>"
"<argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>"
"<argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>"
"<argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>"
"<argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>"
"<argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>"
"<argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>"
"<argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetMediaInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
"<argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
"<argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
"<argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
"<argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
"<argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
"<argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument>"
"<argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument>"
"<argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType><allowedValueList><allowedValue>ABS_TIME</allowedValue><allowedValue>REL_TIME</allowedValue><allowedValue>TRACK_NR</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>TransportState</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>TransportStatus</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>PlaybackStorageMedium</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RecordStorageMedium</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>RecordMediumWriteStatus</name><dataType>string</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static const char RENDERING_XML[] =
"<?xml version=\"1.0\"?>"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>GetVolume</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>SetVolume</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetMute</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>SetMute</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType><allowedValueList><allowedValue>Master</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>Volume</name><dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>Mute</name><dataType>boolean</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static const char CONNECTION_XML[] =
"<?xml version=\"1.0\"?>"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>GetProtocolInfo</name><argumentList>"
"<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
"<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentConnectionIDs</name><argumentList>"
"<argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentConnectionInfo</name><argumentList>"
"<argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>"
"<argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>"
"<argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>"
"<argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>"
"<argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>"
"<argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType><allowedValueList><allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue><allowedValue>InsufficientBandwidth</allowedValue><allowedValue>UnreliableChannel</allowedValue><allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType><allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static char g_dlna_uri[1024];
static char g_last_push[1024];

static void send_response(OrbisNetId c, const char *status, const char *ctype,
                          const char *body, int bodylen);

static void xml_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
        else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
        else { *w++ = *r++; }
    }
    *w = '\0';
}

static int extract_tag(const char *body, const char *tag, char *out, int outlen) {
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(body, open);
    if (!s) return 0;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return 0;
    int n = (int)(e - s);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    xml_unescape(out);
    return 1;
}

static void set_pending_play(const char *url) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_play_pending = 1;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(url);
    player_interrupt();   // unblock a stuck read so the new cast is processed
}

static void set_pending_player(const char *url) {
    scePthreadMutexLock(&g_mtx);
    strncpy(g_pending_url, url, sizeof(g_pending_url) - 1);
    g_pending_url[sizeof(g_pending_url) - 1] = '\0';
    g_player_pending = 1;
    scePthreadMutexUnlock(&g_mtx);
    recent_add(url);
    player_interrupt();
}

static void send_soap_ok(OrbisNetId c, const char *action, const char *inner) {
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">%s</u:%sResponse></s:Body>"
        "</s:Envelope>", action, inner ? inner : "", action);
    send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body, n);
}

static void send_all(OrbisNetId c, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = sceNetSend(c, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

static void send_response(OrbisNetId c, const char *status, const char *ctype,
                          const char *body, int bodylen) {
    char hdr[256];
    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, bodylen);
    send_all(c, hdr, h);
    if (bodylen > 0)
        send_all(c, body, bodylen);
}

static void handle_client(OrbisNetId c) {
    char req[8192];
    int n = sceNetRecv(c, req, sizeof(req) - 1, 0);
    if (n <= 0)
        return;
    req[n] = '\0';

    // Method + path
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/") == 0 || strncmp(path, "/index", 6) == 0)) {
        send_response(c, "200 OK", "text/html; charset=utf-8",
                      WEB_UI_HTML, (int)sizeof(WEB_UI_HTML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/description.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", DEVICE_XML, (int)sizeof(DEVICE_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/AVTransport.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", AVTRANSPORT_XML, (int)sizeof(AVTRANSPORT_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/RenderingControl.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", RENDERING_XML, (int)sizeof(RENDERING_XML) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ConnectionManager.xml") == 0) {
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", CONNECTION_XML, (int)sizeof(CONNECTION_XML) - 1);
        return;
    }

    if (strcmp(path, "/status") == 0) {
        char dbg[512];
        player_debug(dbg, sizeof(dbg));
        char json[1400];
        int active = player_is_active();
        double cur = 0, dur = 0;
        player_progress(&cur, &dur);
        int j = snprintf(json, sizeof(json),
                         "{\"ver\":\"%s\",\"jb\":%d,\"goldhen\":\"%s\",\"status\":\"%s\",\"native\":\"%s\",\"ssdp\":\"%s\",\"active\":%d,\"paused\":%d,\"cur\":%d,\"dur\":%d,\"last_push\":\"%s\",\"debug\":\"%s\",\"pad\":\"%s\"}",
                         APP_VER, jb_result(), goldhen_status(), player_status(), handoff_status(), ssdp_status(),
                         active, player_is_paused(), (int)(cur + 0.5), (int)(dur + 0.5), g_last_push, dbg, pad_diag_get());
        send_response(c, "200 OK", "application/json", json, j);
        return;
    }

    // Hardware-decode research probe (isolated from the player). Trigger with
    // e.g. GET /vdecprobe?sweep=1  or  GET /vdecprobe?codec=1&profile=0 .
    if (strcmp(method, "GET") == 0 && strncmp(path, "/vdecprobe", 10) == 0) {
        const char *q = strchr(path, '?');
        char resp[8192];
        int rn = vdec_probe_run(q ? q + 1 : "", resp, sizeof(resp));
        send_response(c, "200 OK", "text/plain", resp, rn);
        return;
    }

    // Toggle the hardware-decode fast path at runtime (A/B testing vs software).
    // POST /hwdecode body "0"/"1"; takes effect on the next cast.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/hwdecode") == 0) {
        int on = (body[0] != '0');
        player_set_hw(on);
        send_response(c, "200 OK", "text/plain", on ? "hw on" : "hw off", on ? 5 : 6);
        return;
    }

    // Read back the last probe's pre-decode log persisted on /data (survives an
    // uncatchable hard crash, so we can see how far it got after a relaunch).
    if (strcmp(method, "GET") == 0 && strcmp(path, "/vdeclog") == 0) {
        int fd = sceKernelOpen("/data/ps4cast_vdec.log", 0 /*O_RDONLY*/, 0);
        if (fd < 0) { send_response(c, "200 OK", "text/plain", "(no vdec log yet)", 17); return; }
        static char lb[4096];
        int ln = (int)sceKernelRead(fd, lb, sizeof(lb) - 1);
        sceKernelClose(fd);
        if (ln < 0) ln = 0;
        lb[ln] = '\0';
        send_response(c, "200 OK", "text/plain", lb, ln);
        return;
    }

    // Transport controls. /pause body: "1"/"0"/empty(toggle). /seek body: seconds.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/pause") == 0) {
        int want;
        if (body[0] == '1')      want = 1;
        else if (body[0] == '0') want = 0;
        else                     want = !player_is_paused();   // toggle
        player_pause(want);
        send_response(c, "200 OK", "text/plain", want ? "paused" : "playing", want ? 6 : 7);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/seek") == 0) {
        // Body is an absolute position in seconds (may be fractional).
        double sec = 0; int whole = 0, ok = 0;
        for (const char *p = body; *p >= '0' && *p <= '9'; p++) { whole = whole * 10 + (*p - '0'); ok = 1; }
        sec = whole;
        if (ok) {
            player_seek(sec);
            send_response(c, "200 OK", "text/plain", "ok", 2);
        } else {
            send_response(c, "400 Bad Request", "text/plain", "bad seconds", 11);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/goldhen") == 0) {
        char resp[420];
        goldhen_probe(resp, sizeof(resp));
        send_response(c, "200 OK", "text/plain", resp, (int)strlen(resp));
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/goldhen/restore") == 0) {
        char resp[420];
        goldhen_restore(resp, sizeof(resp));
        send_response(c, "200 OK", "text/plain", resp, (int)strlen(resp));
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/play") == 0) {
        char trimmed[1024];
        strncpy(trimmed, body, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        // trim trailing whitespace/newlines
        for (int i = (int)strlen(trimmed) - 1; i >= 0; i--) {
            char ch = trimmed[i];
            if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') trimmed[i] = '\0';
            else break;
        }
        set_pending_play(trimmed);
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/avplay") == 0) {
        char trimmed[1024];
        strncpy(trimmed, body, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        for (int i = (int)strlen(trimmed) - 1; i >= 0; i--) {
            char ch = trimmed[i];
            if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') trimmed[i] = '\0';
            else break;
        }
        strncpy(g_last_push, trimmed, sizeof(g_last_push) - 1);
        g_last_push[sizeof(g_last_push) - 1] = '\0';
        set_pending_player(trimmed);
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/AVTransport") == 0) {
        if (strstr(req, "SetAVTransportURI")) {
            char uri[1024];
            if (extract_tag(body, "CurrentURI", uri, sizeof(uri))) {
                strncpy(g_dlna_uri, uri, sizeof(g_dlna_uri) - 1);
                g_dlna_uri[sizeof(g_dlna_uri) - 1] = '\0';
                strncpy(g_last_push, uri, sizeof(g_last_push) - 1);
                g_last_push[sizeof(g_last_push) - 1] = '\0';
                set_pending_player(g_dlna_uri);
            }
            send_soap_ok(c, "SetAVTransportURI", "");
            return;
        }
        if (strstr(req, "Play")) {
            if (g_dlna_uri[0])
                set_pending_player(g_dlna_uri);
            send_soap_ok(c, "Play", "");
            return;
        }
        if (strstr(req, "Stop") || strstr(req, "Pause")) {
            if (strstr(req, "Pause")) {
                player_pause(1);
                send_soap_ok(c, "Pause", "");
            } else {
                scePthreadMutexLock(&g_mtx);
                g_stop_pending = 1;
                scePthreadMutexUnlock(&g_mtx);
                player_interrupt();
                send_soap_ok(c, "Stop", "");
            }
            return;
        }
        if (strstr(req, "Seek")) {
            send_soap_ok(c, "Seek", "");
            return;
        }
        if (strstr(req, "GetTransportInfo")) {
            const char *state = player_started() ? (player_is_paused() ? "PAUSED_PLAYBACK" : "PLAYING") : "STOPPED";
            char inner[220];
            snprintf(inner, sizeof(inner),
                         "<CurrentTransportState>%s</CurrentTransportState>"
                         "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                         "<CurrentSpeed>1</CurrentSpeed>", state);
            send_soap_ok(c, "GetTransportInfo",
                         inner);
            return;
        }
        if (strstr(req, "GetPositionInfo")) {
            send_soap_ok(c, "GetPositionInfo",
                         "<Track>0</Track><TrackDuration>00:00:00</TrackDuration>"
                         "<TrackMetaData></TrackMetaData><TrackURI></TrackURI>"
                         "<RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime>"
                         "<RelCount>0</RelCount><AbsCount>0</AbsCount>");
            return;
        }
        if (strstr(req, "GetMediaInfo")) {
            send_soap_ok(c, "GetMediaInfo",
                         "<NrTracks>0</NrTracks><MediaDuration>00:00:00</MediaDuration>"
                         "<CurrentURI></CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
                         "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                         "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                         "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
            return;
        }
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/RenderingControl") == 0) {
        if (strstr(req, "GetVolume")) {
            const char *body_ok =
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:GetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                "<CurrentVolume>50</CurrentVolume></u:GetVolumeResponse></s:Body></s:Envelope>";
            send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body_ok, (int)strlen(body_ok));
        } else if (strstr(req, "GetMute")) {
            const char *body_ok =
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:GetMuteResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                "<CurrentMute>0</CurrentMute></u:GetMuteResponse></s:Body></s:Envelope>";
            send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", body_ok, (int)strlen(body_ok));
        } else {
            const char *rc_ok = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body/></s:Envelope>";
            send_response(c, "200 OK", "text/xml; charset=\"utf-8\"",
                          rc_ok, (int)strlen(rc_ok));
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/upnp/control/ConnectionManager") == 0) {
        const char *resp;
        if (strstr(req, "GetProtocolInfo")) {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                   "<s:Body><u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                   "<Source></Source><Sink>http-get:*:video/mp4:*,http-get:*:video/x-matroska:*,http-get:*:video/*:*</Sink>"
                   "</u:GetProtocolInfoResponse></s:Body></s:Envelope>";
        } else if (strstr(req, "GetCurrentConnectionIDs")) {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                   "<s:Body><u:GetCurrentConnectionIDsResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                   "<ConnectionIDs>0</ConnectionIDs></u:GetCurrentConnectionIDsResponse></s:Body></s:Envelope>";
        } else if (strstr(req, "GetCurrentConnectionInfo")) {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                   "<s:Body><u:GetCurrentConnectionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                   "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
                   "<ProtocolInfo>http-get:*:video/mp4:*</ProtocolInfo>"
                   "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                   "<Direction>Input</Direction><Status>OK</Status>"
                   "</u:GetCurrentConnectionInfoResponse></s:Body></s:Envelope>";
        } else {
            resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body/></s:Envelope>";
        }
        send_response(c, "200 OK", "text/xml; charset=\"utf-8\"", resp, (int)strlen(resp));
        return;
    }

    // Privileged launch by URI (ShellUI). Body = the URI to try. Lets us probe
    // many URI formats over curl without rebuilding.
    if (strcmp(method, "POST") == 0 && strcmp(path, "/launch") == 0) {
        char uri[512];
        strncpy(uri, body, sizeof(uri) - 1);
        uri[sizeof(uri) - 1] = '\0';
        for (int i = (int)strlen(uri) - 1; i >= 0 && (uri[i]=='\r'||uri[i]=='\n'||uri[i]==' '); i--) uri[i]='\0';
        int r = launch_by_uri(uri);
        char resp[400];
        int rn = snprintf(resp, sizeof(resp), "rc=0x%x | %s", (unsigned)r, launch_debug());
        send_response(c, "200 OK", "text/plain", resp, rn);
        return;
    }

    // Native app handoff. Body:
    //   TITLE_ID\noptional argument/URL
    if (strcmp(method, "POST") == 0 && strcmp(path, "/launchapp") == 0) {
        char title[32];
        char arg[768];
        memset(title, 0, sizeof(title));
        memset(arg, 0, sizeof(arg));

        const char *nl = strchr(body, '\n');
        int title_len = nl ? (int)(nl - body) : (int)strlen(body);
        if (title_len >= (int)sizeof(title)) title_len = (int)sizeof(title) - 1;
        memcpy(title, body, title_len);
        title[title_len] = '\0';
        for (int i = (int)strlen(title) - 1; i >= 0 && (title[i]=='\r'||title[i]=='\n'||title[i]==' '||title[i]=='\t'); i--) title[i]='\0';

        if (nl) {
            strncpy(arg, nl + 1, sizeof(arg) - 1);
            arg[sizeof(arg) - 1] = '\0';
            for (int i = (int)strlen(arg) - 1; i >= 0 && (arg[i]=='\r'||arg[i]=='\n'||arg[i]==' '||arg[i]=='\t'); i--) arg[i]='\0';
        }

        if (!title[0]) {
            send_response(c, "400 Bad Request", "text/plain", "missing title id", 16);
            return;
        }

        int r = launch_app(title, arg);
        char resp[420];
        int rn = snprintf(resp, sizeof(resp), "rc=0x%x | %s", (unsigned)r, launch_debug());
        send_response(c, "200 OK", "text/plain", resp, rn);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/browser") == 0) {
        const char *resp = "browser handoff disabled after CE-36329-3";
        send_response(c, "409 Conflict", "text/plain", resp, (int)strlen(resp));
        return;
    }

    if (strcmp(path, "/stop") == 0) {
        scePthreadMutexLock(&g_mtx);
        g_stop_pending = 1;
        scePthreadMutexUnlock(&g_mtx);
        player_interrupt();   // break a stuck read so Stop takes effect now
        send_response(c, "200 OK", "text/plain", "ok", 2);
        return;
    }

    // ---- recents / queue / favorites -------------------------------------
    if (strcmp(method, "GET") == 0 && strcmp(path, "/recent") == 0) {
        static char j[MAX_RECENT * URL_MAX + 64];
        scePthreadMutexLock(&g_mtx); int n = json_list(j, sizeof(j), g_recent, g_recentN); scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, n); return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/favs") == 0) {
        static char j[MAX_FAV * URL_MAX + 64];
        scePthreadMutexLock(&g_mtx); int n = json_list(j, sizeof(j), g_fav, g_favN); scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, n); return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/queue") == 0) {
        static char tmp[MAX_QUEUE][URL_MAX]; static char j[MAX_QUEUE * URL_MAX + 64]; int n;
        scePthreadMutexLock(&g_mtx);
        n = g_queueN;
        for (int i = 0; i < n; i++) { strncpy(tmp[i], g_queue[(g_queueHead + i) % MAX_QUEUE], URL_MAX - 1); tmp[i][URL_MAX-1] = '\0'; }
        scePthreadMutexUnlock(&g_mtx);
        int jn = json_list(j, sizeof(j), tmp, n);
        send_response(c, "200 OK", "application/json", j, jn); return;
    }
    if (strcmp(method, "POST") == 0 && (strcmp(path, "/queue") == 0 || strcmp(path, "/fav") == 0)) {
        char u[URL_MAX]; strncpy(u, body, sizeof(u) - 1); u[sizeof(u)-1] = '\0';
        for (int i = (int)strlen(u) - 1; i >= 0 && (u[i]=='\r'||u[i]=='\n'||u[i]==' '||u[i]=='\t'); i--) u[i] = '\0';
        if (u[0]) { if (path[1] == 'q') queue_push(u); else fav_toggle(u); }
        send_response(c, "200 OK", "text/plain", "ok", 2); return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/quit") == 0) {
        scePthreadMutexLock(&g_mtx);
        g_quit_pending = 1;
        scePthreadMutexUnlock(&g_mtx);
        player_interrupt();
        send_response(c, "200 OK", "text/plain", "closing", 7);
        return;
    }

    send_response(c, "404 Not Found", "text/plain", "not found", 9);
}

static void *server_main(void *arg) {
    (void)arg;
    for (;;) {
        OrbisNetId c = sceNetAccept(g_listen, NULL, NULL);
        if (c < 0)
            continue;
        handle_client(c);
        sceNetSocketClose(c);
    }
    return NULL;
}

int httpd_start(int port) {
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_mtx");
    favs_load();   // restore saved favorites from /data

    g_listen = sceNetSocket("ps4cast", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (g_listen < 0)
        return -1;

    int on = 1;
    sceNetSetsockopt(g_listen, SOL_SOCKET_PS4, SO_REUSEADDR_PS4, &on, sizeof(on));

    ps4_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.len    = sizeof(addr);
    addr.family = ORBIS_NET_AF_INET;
    addr.port   = sceNetHtons((uint16_t)port);
    addr.addr   = 0;  // INADDR_ANY

    if (sceNetBind(g_listen, (const OrbisNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -2;
    }
    if (sceNetListen(g_listen, 8) < 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -3;
    }

    if (scePthreadCreate(&g_thread, NULL, server_main, NULL, "ps4cast_httpd") != 0) {
        sceNetSocketClose(g_listen);
        g_listen = -1;
        return -4;
    }
    g_started = 1;
    return 0;
}

void httpd_poll(void) {
    // Nothing to do here; the accept loop runs on its own thread. Kept so the
    // main loop can call it if we later move to a single-threaded model.
}

int httpd_take_play_request(char *out, int len) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_play_pending) {
        strncpy(out, g_pending_url, len - 1);
        out[len - 1] = '\0';
        g_play_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int httpd_take_player_request(char *out, int len) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_player_pending) {
        strncpy(out, g_pending_url, len - 1);
        out[len - 1] = '\0';
        g_player_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int httpd_take_stop_request(void) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_stop_pending) {
        g_stop_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int httpd_take_quit_request(void) {
    if (!g_started) return 0;
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    if (g_quit_pending) {
        g_quit_pending = 0;
        got = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}
