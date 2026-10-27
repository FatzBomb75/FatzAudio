/* HTTPS on Windows: WinHTTP (part of every Windows since XP), so TLS uses the
 * system's SChannel, certificate store and proxy settings. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include "net.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#  define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4   /* Windows 8.1 and later */
#endif

static wchar_t* widen(const char* s)
{
    wchar_t* w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char* narrow(const wchar_t* w)
{
    char* s;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char*)malloc((size_t)n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* Requests in flight, so fa_http_abort can close the one a cancel flag belongs to:
 * closing a WinHTTP handle makes a blocked synchronous call return at once. */
#define FA_WINHTTP_SLOTS 64
static SRWLOCK g_slots_lock = SRWLOCK_INIT;
static struct { fa_atomic32* cancel; HINTERNET request; int closed; } g_slots[FA_WINHTTP_SLOTS];

static int slot_add(fa_atomic32* cancel, HINTERNET request)
{
    int i, found = -1;
    if (!cancel) return -1;
    AcquireSRWLockExclusive(&g_slots_lock);
    for (i = 0; i < FA_WINHTTP_SLOTS; i++)
        if (!g_slots[i].request) { g_slots[i].cancel = cancel; g_slots[i].request = request; g_slots[i].closed = 0; found = i; break; }
    ReleaseSRWLockExclusive(&g_slots_lock);
    return found;
}

/* Removes the slot; returns 1 if fa_http_abort already closed the handle. */
static int slot_remove(int i)
{
    int closed;
    if (i < 0) return 0;
    AcquireSRWLockExclusive(&g_slots_lock);
    closed = g_slots[i].closed;
    g_slots[i].request = NULL;
    g_slots[i].cancel = NULL;
    ReleaseSRWLockExclusive(&g_slots_lock);
    return closed;
}

void fa_http_abort(fa_atomic32* cancel)
{
    int i;
    AcquireSRWLockExclusive(&g_slots_lock);
    /* The download thread clears the flag before it opens its next request: a
     * cleared flag means the transfer this call was meant for is already over,
     * and the open one is new. */
    if (cancel && fa_load32(cancel))
        for (i = 0; i < FA_WINHTTP_SLOTS; i++)
            if (g_slots[i].request && g_slots[i].cancel == cancel && !g_slots[i].closed) {
                g_slots[i].closed = 1;
                WinHttpCloseHandle(g_slots[i].request);
            }
    ReleaseSRWLockExclusive(&g_slots_lock);
}

/* The system proxy for plain http:// (used by the socket client): the static
 * proxy of the Internet settings, if one is set. */
int fa_net_system_proxy(const char* scheme, const char* host, char* out, size_t cap)
{
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg;
    int found = 0;
    (void)scheme;
    memset(&cfg, 0, sizeof(cfg));
    if (!WinHttpGetIEProxyConfigForCurrentUser(&cfg)) return 0;
    if (cfg.lpszProxy) {
        char* all = narrow(cfg.lpszProxy);
        char* bypass = cfg.lpszProxyBypass ? narrow(cfg.lpszProxyBypass) : NULL;
        int bypassed = 0;
        if (bypass && host) {
            /* "<local>" and ';'-separated host patterns with a leading '*'. */
            char* tok = bypass;
            while (tok && *tok && !bypassed) {
                char* next = strchr(tok, ';');
                size_t len, hl = strlen(host);
                if (next) *next++ = 0;
                while (*tok == ' ') tok++;
                len = strlen(tok);
                if (strcmp(tok, "<local>") == 0) bypassed = strchr(host, '.') == NULL;
                else if (tok[0] == '*' && len - 1 <= hl && fa_strncasecmp_ascii(host + hl - (len - 1), tok + 1, len - 1) == 0) bypassed = 1;
                else if (fa_strncasecmp_ascii(host, tok, len + 1) == 0) bypassed = 1;
                tok = next;
            }
        }
        if (all && !bypassed) {
            /* Either "host:port" for everything, or "http=host:port;https=host:port". */
            const char* p = strstr(all, "http=");
            const char* v = p ? p + 5 : (strchr(all, '=') ? NULL : all);
            if (v) {
                size_t len = strcspn(v, "; ");
                if (len > 0 && len < cap) { memcpy(out, v, len); out[len] = 0; found = 1; }
            }
        }
        free(all);
        free(bypass);
    }
    if (cfg.lpszAutoConfigUrl) GlobalFree(cfg.lpszAutoConfigUrl);
    if (cfg.lpszProxy) GlobalFree(cfg.lpszProxy);
    if (cfg.lpszProxyBypass) GlobalFree(cfg.lpszProxyBypass);
    return found;
}

/* fa_http_each_header's sink: CRLF-terminated lines for WinHttpSendRequest, except
 * the User-Agent, which WinHTTP takes from the session (one given in the block
 * would be sent twice). */
typedef struct header_block {
    char* text;
    size_t n, cap;
    char* agent;              /* the User-Agent value, NULL = none */
} header_block;

static int add_block_line(void* user, const char* name, const char* value)
{
    header_block* b = (header_block*)user;
    size_t ln = strlen(name), lv = strlen(value);
    if (_stricmp(name, "User-Agent") == 0) {
        free(b->agent);
        b->agent = _strdup(value);
        return b->agent == NULL;
    }
    if (b->n + ln + lv + 5 > b->cap) {
        size_t cap = (b->n + ln + lv + 5) * 2;
        char* p = (char*)realloc(b->text, cap);
        if (!p) return 1;
        b->text = p;
        b->cap = cap;
    }
    b->n += (size_t)snprintf(b->text + b->n, b->cap - b->n, "%s: %s\r\n", name, value);
    return 0;
}

/* One session per stream (per user agent and proxy setting: WinHTTP takes both at
 * WinHttpOpen), and a connection handle for the last server. WinHTTP pools the
 * session's connections, so later requests skip the TCP and TLS handshakes. */
struct fa_http_session {
    HINTERNET session, connect;
    wchar_t* agent;
    char* proxy;              /* the request's proxy setting (a copy), NULL = the system's */
    wchar_t* host;            /* connect's server */
    INTERNET_PORT port;
};

fa_http_session* fa_http_session_create(void) { return (fa_http_session*)calloc(1, sizeof(fa_http_session)); }

static void session_close(fa_http_session* s)
{
    if (s->connect) WinHttpCloseHandle(s->connect);
    if (s->session) WinHttpCloseHandle(s->session);
    free(s->agent); free(s->proxy); free(s->host);
    memset(s, 0, sizeof(*s));
}

void fa_http_session_destroy(fa_http_session* s)
{
    if (!s) return;
    session_close(s);
    free(s);
}

static HINTERNET open_session(const wchar_t* wua, const char* proxy)
{
    HINTERNET session;
    if (proxy && !proxy[0]) {
        session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    } else if (proxy) {
        /* "http://user:pass@host:port" -> "host:port" (credentials are not passed on). */
        const char* p = strstr(proxy, "://");
        const char* at;
        wchar_t* wproxy;
        p = p ? p + 3 : proxy;
        at = strchr(p, '@');
        if (at) p = at + 1;
        wproxy = widen(p);
        if (!wproxy) return NULL;
        { size_t l = wcslen(wproxy); if (l > 0 && wproxy[l - 1] == L'/') wproxy[l - 1] = 0; }
        session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, wproxy, WINHTTP_NO_PROXY_BYPASS, 0);
        free(wproxy);
    } else {
        session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)   /* before Windows 8.1 */
            session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    return session;
}

fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user,
                            char** out_redirect)
{
    fa_http_session one = { 0 };
    fa_http_session* s = req->session ? req->session : &one;
    HINTERNET request = NULL;
    wchar_t *wurl = NULL, *wua = NULL, *whost = NULL, *wpath = NULL, *wheaders = NULL;
    URL_COMPONENTS uc;
    fa_result r = FA_ERROR_NETWORK;
    DWORD status = 0, size, features = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    int slot = -1;
    uint8_t* buf = NULL;
    char* headers = NULL;
    header_block block;
    fa_url u;

    *out_redirect = NULL;
    memset(&block, 0, sizeof(block));
    /* WinHTTP ignores credentials in the URL: the walk sends them. */
    if (fa_http_each_header(req, fa_url_parse(req->url, &u) == FA_OK ? &u : NULL, add_block_line, &block) != FA_OK) {
        r = FA_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    wurl = widen(req->url);
    wua = widen(block.agent ? block.agent : "FatzAudio");
    wheaders = block.text ? widen(block.text) : NULL;
    if (!wurl || !wua || (block.text && !wheaders)) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { r = FA_ERROR_INVALID_ARG; goto done; }
    whost = (wchar_t*)calloc(uc.dwHostNameLength + 1, sizeof(wchar_t));
    wpath = (wchar_t*)calloc(uc.dwUrlPathLength + uc.dwExtraInfoLength + 2, sizeof(wchar_t));
    if (!whost || !wpath) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    memcpy(whost, uc.lpszHostName, uc.dwHostNameLength * sizeof(wchar_t));
    memcpy(wpath, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(wchar_t));
    memcpy(wpath + uc.dwUrlPathLength, uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(wchar_t));
    if (!wpath[0]) wpath[0] = L'/';

    if (!s->session || wcscmp(s->agent, wua) != 0 ||
        !(req->proxy ? s->proxy && strcmp(s->proxy, req->proxy) == 0 : !s->proxy)) {
        session_close(s);
        if (req->proxy && !(s->proxy = _strdup(req->proxy))) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
        s->session = open_session(wua, req->proxy);
        if (!s->session) goto done;
        s->agent = wua;
        wua = NULL;
    }
    if (!s->connect || _wcsicmp(s->host, whost) != 0 || s->port != uc.nPort) {
        if (s->connect) WinHttpCloseHandle(s->connect);
        free(s->host);
        s->host = NULL;
        s->connect = WinHttpConnect(s->session, whost, uc.nPort, 0);
        if (!s->connect) goto done;
        s->host = whost;
        whost = NULL;
        s->port = uc.nPort;
    }
    request = WinHttpOpenRequest(s->connect, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto done;
    WinHttpSetTimeouts(request, (int)req->timeout_ms, (int)req->timeout_ms, (int)req->timeout_ms, (int)req->timeout_ms);
    /* Redirects come back to fa_http_fetch, which follows them for every transport
     * (WinHTTP's own would keep the Authorization header on another server). No
     * cookie store either, as on the other transports: a kept session would replay
     * a server's cookies on the stream's later requests. */
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
    slot = slot_add(req->cancel, request);
    if (req->cancel && fa_load32(req->cancel)) { r = FA_ERROR_BUSY; goto done; }
    if (!WinHttpSendRequest(request, wheaders && wheaders[0] ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                            wheaders && wheaders[0] ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL))
        goto done;

    size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    if (size / sizeof(wchar_t) > FA_HTTP_HEAD_MAX) goto done;   /* a network error, as the other backends */
    if (size > 0) {
        wchar_t* wh = (wchar_t*)malloc(size + sizeof(wchar_t));
        if (wh && WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, wh, &size,
                                      WINHTTP_NO_HEADER_INDEX)) {
            wh[size / sizeof(wchar_t)] = 0;
            headers = narrow(wh);
        }
        free(wh);
    }
    buf = (uint8_t*)malloc(65536);
    if (!buf) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    {
        fa_http_response resp;
        char* lines = headers ? headers + strcspn(headers, "\n") : NULL;   /* skip the status line */
        if (lines && *lines) lines++;
        memset(&resp, 0, sizeof(resp));
        resp.status = (int)status;
        resp.headers = lines ? lines : (char*)"";
        resp.final_url = req->url;
        switch (fa_http_redirect_target(req->url, (int)status, resp.headers, out_redirect)) {
        case 1: {
            /* Its body is read to the end, so the connection goes back to the pool. */
            DWORD got = 0, total = 0;
            while (total <= FA_HTTP_DRAIN_MAX && WinHttpReadData(request, buf, 65536, &got) && got > 0) total += got;
            r = FA_OK;
            goto done;
        }
        case -1: r = FA_ERROR_NETWORK; goto done;
        default: break;
        }
        fa_http_fill_lengths(&resp);
        if (on_head && on_head(user, &resp)) { r = FA_OK; goto done; }
        if (status >= 400) { r = FA_ERROR_NOT_FOUND; goto done; }
    }
    for (;;) {
        DWORD got = 0;
        if (req->cancel && fa_load32(req->cancel)) { r = FA_ERROR_BUSY; goto done; }
        if (!WinHttpReadData(request, buf, 65536, &got)) { r = FA_ERROR_NETWORK; break; }
        if (got == 0) { r = FA_OK; break; }    /* WinHTTP checks Content-Length: a short body is an error above */
        if (on_data(user, buf, got)) { r = FA_OK; break; }
    }
done:
    if (req->cancel && fa_load32(req->cancel)) { r = FA_ERROR_BUSY; free(*out_redirect); *out_redirect = NULL; }
    if (request && !slot_remove(slot)) WinHttpCloseHandle(request);
    if (s == &one) session_close(&one);
    free(buf); free(headers); free(block.text); free(block.agent);
    free(wurl); free(wua); free(whost); free(wpath); free(wheaders);
    return r;
}

#endif
