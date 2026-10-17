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

/* The app's extra headers plus ours, one CRLF-terminated line each. */
static wchar_t* build_headers(const fa_http_request* req)
{
    size_t cap = 1024 + (req->headers ? strlen(req->headers) * 2 : 0);
    char* h = (char*)malloc(cap);
    size_t n = 0;
    wchar_t* w;
    if (!h) return NULL;
    h[0] = 0;
    if (req->icy) n += (size_t)snprintf(h + n, cap - n, "Icy-MetaData: 1\r\n");
    if (req->range_start >= 0) n += (size_t)snprintf(h + n, cap - n, "Range: bytes=%lld-\r\n", (long long)req->range_start);
    {
        /* WinHTTP ignores credentials in the URL: send them as the socket client does. */
        fa_url u;
        if (fa_url_parse(req->url, &u) == FA_OK && u.userinfo[0]) {
            char auth[512];
            fa_http_basic_auth(u.userinfo, auth, sizeof(auth));
            n += (size_t)snprintf(h + n, cap - n, "Authorization: %s\r\n", auth);
        }
    }
    if (req->headers) {
        const char* p = req->headers;
        while (*p) {
            size_t len = strcspn(p, "\r\n");
            if (len > 0 && n + len + 3 < cap) { memcpy(h + n, p, len); n += len; h[n++] = '\r'; h[n++] = '\n'; h[n] = 0; }
            p += len;
            while (*p == '\r' || *p == '\n') p++;
        }
    }
    w = widen(h);
    free(h);
    return w;
}

fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user)
{
    HINTERNET session = NULL, connect = NULL, request = NULL;
    wchar_t *wurl = NULL, *wua = NULL, *whost = NULL, *wpath = NULL, *wheaders = NULL, *wproxy = NULL;
    URL_COMPONENTS uc;
    fa_result r = FA_ERROR_NETWORK;
    DWORD status = 0, size;
    int slot = -1;
    uint8_t* buf = NULL;
    char* headers = NULL;
    char* final_url = NULL;

    wurl = widen(req->url);
    wua = widen(req->user_agent ? req->user_agent : "FatzAudio");
    if (!wurl || !wua) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
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

    if (req->proxy && !req->proxy[0]) {
        session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    } else if (req->proxy) {
        /* "http://user:pass@host:port" -> "host:port" (credentials are not passed on). */
        const char* p = strstr(req->proxy, "://");
        const char* at;
        p = p ? p + 3 : req->proxy;
        at = strchr(p, '@');
        if (at) p = at + 1;
        wproxy = widen(p);
        if (wproxy) { size_t l = wcslen(wproxy); if (l > 0 && wproxy[l - 1] == L'/') wproxy[l - 1] = 0; }
        session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, wproxy, WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)   /* before Windows 8.1 */
            session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) goto done;
    WinHttpSetTimeouts(session, (int)req->timeout_ms, (int)req->timeout_ms, (int)req->timeout_ms, (int)req->timeout_ms);
    connect = WinHttpConnect(session, whost, uc.nPort, 0);
    if (!connect) goto done;
    request = WinHttpOpenRequest(connect, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto done;
    slot = slot_add(req->cancel, request);
    if (req->cancel && fa_load32(req->cancel)) { r = FA_ERROR_BUSY; goto done; }
    wheaders = build_headers(req);
    if (!WinHttpSendRequest(request, wheaders && wheaders[0] ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                            wheaders && wheaders[0] ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL))
        goto done;

    size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    if (size > 0) {
        wchar_t* wh = (wchar_t*)malloc(size + sizeof(wchar_t));
        if (wh && WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, wh, &size,
                                      WINHTTP_NO_HEADER_INDEX)) {
            wh[size / sizeof(wchar_t)] = 0;
            headers = narrow(wh);
        }
        free(wh);
    }
    size = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, NULL, &size);
    if (size > 0) {
        wchar_t* wu = (wchar_t*)malloc(size + sizeof(wchar_t));
        if (wu && WinHttpQueryOption(request, WINHTTP_OPTION_URL, wu, &size)) {
            wu[size / sizeof(wchar_t)] = 0;
            final_url = narrow(wu);
        }
        free(wu);
    }
    {
        fa_http_response resp;
        char* lines = headers ? headers + strcspn(headers, "\n") : NULL;   /* skip the status line */
        if (lines && *lines) lines++;
        memset(&resp, 0, sizeof(resp));
        resp.status = (int)status;
        resp.headers = lines ? lines : (char*)"";
        resp.final_url = final_url ? final_url : req->url;
        fa_http_fill_lengths(&resp);
        if (on_head && on_head(user, &resp)) { r = FA_OK; goto done; }
        if (status >= 400) { r = FA_ERROR_NOT_FOUND; goto done; }
    }
    buf = (uint8_t*)malloc(65536);
    if (!buf) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    for (;;) {
        DWORD got = 0;
        if (req->cancel && fa_load32(req->cancel)) { r = FA_ERROR_BUSY; goto done; }
        if (!WinHttpReadData(request, buf, 65536, &got)) { r = FA_ERROR_NETWORK; break; }
        if (got == 0) { r = FA_OK; break; }    /* WinHTTP checks Content-Length: a short body is an error above */
        if (on_data(user, buf, got)) { r = FA_OK; break; }
    }
done:
    if (req->cancel && fa_load32(req->cancel)) r = FA_ERROR_BUSY;
    if (request && !slot_remove(slot)) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    free(buf); free(headers); free(final_url);
    free(wurl); free(wua); free(whost); free(wpath); free(wheaders); free(wproxy);
    return r;
}

#endif
