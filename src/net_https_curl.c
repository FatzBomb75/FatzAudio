/* HTTPS on Linux: libcurl, loaded at runtime (never linked, never
 * bundled). Desktop Linux distributions ship it; when it is missing, https URLs
 * return FA_ERROR_PROVIDER_MISSING and http:// keeps working. */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The few libcurl entry points and option numbers used here. The numbers are part
 * of libcurl's stable ABI (curl.h: CURLOPTTYPE_* base + index). */
typedef void CURL;
struct curl_slist;
typedef int (*curl_global_init_fn)(long flags);
typedef CURL* (*curl_easy_init_fn)(void);
typedef int (*curl_easy_setopt_fn)(CURL* curl, int option, ...);
typedef int (*curl_easy_perform_fn)(CURL* curl);
typedef int (*curl_easy_getinfo_fn)(CURL* curl, int info, ...);
typedef void (*curl_easy_cleanup_fn)(CURL* curl);
typedef void (*curl_easy_reset_fn)(CURL* curl);
typedef struct curl_slist* (*curl_slist_append_fn)(struct curl_slist* list, const char* s);
typedef void (*curl_slist_free_all_fn)(struct curl_slist* list);

enum {
    C_WRITEDATA = 10001, C_URL = 10002, C_PROXY = 10004, C_WRITEFUNCTION = 20011,
    C_USERAGENT = 10018, C_LOW_SPEED_LIMIT = 19, C_LOW_SPEED_TIME = 20, C_HTTPHEADER = 10023,
    C_HEADERDATA = 10029, C_NOPROGRESS = 43, C_XFERINFODATA = 10057,
    C_HEADERFUNCTION = 20079, C_NOSIGNAL = 99, C_CONNECTTIMEOUT_MS = 156,
    C_XFERINFOFUNCTION = 20219
};
enum { CI_RESPONSE_CODE = 0x200002 };
enum { CE_OK = 0, CE_WRITE_ERROR = 23, CE_ABORTED_BY_CALLBACK = 42 };
#define C_GLOBAL_DEFAULT 3L   /* CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32 */

static struct {
    fa_atomic32 state;        /* 0 not tried, 1 loading, 2 ready, 3 missing */
    curl_easy_init_fn easy_init;
    curl_easy_setopt_fn setopt;
    curl_easy_perform_fn perform;
    curl_easy_getinfo_fn getinfo;
    curl_easy_cleanup_fn cleanup;
    curl_easy_reset_fn reset;
    curl_slist_append_fn slist_append;
    curl_slist_free_all_fn slist_free;
} g_curl;

static int curl_load(void)
{
    static const char* names[] = { "libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl-nss.so.4" };
    for (;;) {
        int32_t st = fa_load32(&g_curl.state);
        if (st == 2) return 1;
        if (st == 3) return 0;
        if (st == 0 && fa_cas32(&g_curl.state, 0, 1)) {
            void* lib = NULL;
            size_t i;
            curl_global_init_fn global_init = NULL;
            for (i = 0; i < sizeof(names) / sizeof(names[0]) && !lib; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            if (lib) {
                /* Function pointers through a union: ISO C has no object/function pointer cast. */
#define LOAD(dst, name) do { union { void* p; __typeof__(dst) f; } u_; u_.p = dlsym(lib, name); dst = u_.f; } while (0)
                LOAD(global_init, "curl_global_init");
                LOAD(g_curl.easy_init, "curl_easy_init");
                LOAD(g_curl.setopt, "curl_easy_setopt");
                LOAD(g_curl.perform, "curl_easy_perform");
                LOAD(g_curl.getinfo, "curl_easy_getinfo");
                LOAD(g_curl.cleanup, "curl_easy_cleanup");
                LOAD(g_curl.reset, "curl_easy_reset");
                LOAD(g_curl.slist_append, "curl_slist_append");
                LOAD(g_curl.slist_free, "curl_slist_free_all");
#undef LOAD
            }
            if (lib && global_init && g_curl.easy_init && g_curl.setopt && g_curl.perform && g_curl.getinfo &&
                g_curl.cleanup && g_curl.reset && g_curl.slist_append && g_curl.slist_free && global_init(C_GLOBAL_DEFAULT) == CE_OK) {
                fa_store32(&g_curl.state, 2);    /* stays loaded for the life of the process */
                return 1;
            }
            if (lib) dlclose(lib);
            fa_store32(&g_curl.state, 3);
            return 0;
        }
        fa_sleep_ms(1);
    }
}

/* One easy handle per stream: its connection cache keeps connections (and TLS
 * sessions) between requests. curl_easy_reset clears the options, not the cache. */
struct fa_http_session {
    CURL* curl;               /* NULL until the first https request */
};

fa_http_session* fa_http_session_create(void) { return (fa_http_session*)calloc(1, sizeof(fa_http_session)); }

void fa_http_session_destroy(fa_http_session* s)
{
    if (!s) return;
    if (s->curl) g_curl.cleanup(s->curl);   /* set only once libcurl is loaded */
    free(s);
}

typedef struct curl_ctx {
    const fa_http_request* req;
    fa_http_head_proc on_head;
    fa_http_data_proc on_data;
    void* user;
    CURL* curl;
    char* headers;            /* the current header block */
    size_t hlen, hcap;
    int status;
    int head_done;            /* on_head was called */
    int stopped;              /* a callback asked to stop */
    int failed_status;        /* 4xx/5xx */
    char* redirect;           /* the target of a redirect response */
    int bad_redirect;         /* a redirect with a Location that can't be followed */
    size_t drained;           /* body bytes of a redirect read and dropped */
} curl_ctx;

static size_t on_header(char* data, size_t size, size_t nitems, void* user)
{
    curl_ctx* c = (curl_ctx*)user;
    size_t n = size * nitems;
    if (n >= 5 && (strncmp(data, "HTTP/", 5) == 0 || strncmp(data, "ICY ", 4) == 0)) {
        /* A new response (the first, or after a redirect): start over. */
        const char* sp = memchr(data, ' ', n);
        c->hlen = 0;
        c->status = sp ? atoi(sp + 1) : 0;
        return n;
    }
    if (!c->head_done && c->hlen + n > FA_HTTP_HEAD_MAX) return 0;   /* curl stops: a network error (trailers after the head don't count) */
    if (c->hlen + n + 1 > c->hcap) {
        size_t cap = (c->hlen + n + 1) * 2;
        char* p = (char*)realloc(c->headers, cap);
        if (!p) return 0;
        c->headers = p;
        c->hcap = cap;
    }
    memcpy(c->headers + c->hlen, data, n);
    c->hlen += n;
    c->headers[c->hlen] = 0;
    return n;
}

/* Calls on_head once, with the response (a redirect goes to c->redirect instead).
 * Returns nonzero to stop delivering the body. */
static int deliver_head(curl_ctx* c)
{
    fa_http_response resp;
    long code = 0;
    if (c->head_done) return c->stopped || c->failed_status || c->redirect || c->bad_redirect;
    c->head_done = 1;
    memset(&resp, 0, sizeof(resp));
    if (g_curl.getinfo(c->curl, CI_RESPONSE_CODE, &code) == CE_OK && code > 0) c->status = (int)code;
    resp.status = c->status;
    resp.headers = c->headers ? c->headers : (char*)"";
    resp.final_url = c->req->url;
    switch (fa_http_redirect_target(c->req->url, c->status, resp.headers, &c->redirect)) {
    case 1: return 1;
    case -1: c->bad_redirect = 1; return 1;
    default: break;
    }
    fa_http_fill_lengths(&resp);
    if (c->on_head && c->on_head(c->user, &resp)) { c->stopped = 1; return 1; }
    if (c->status >= 400) { c->failed_status = 1; return 1; }
    return 0;
}

static size_t on_write(char* data, size_t size, size_t nmemb, void* user)
{
    curl_ctx* c = (curl_ctx*)user;
    size_t n = size * nmemb;
    if (c->req->cancel && fa_load32(c->req->cancel)) return 0;
    if (deliver_head(c)) {
        /* A redirect's body is read to its end, so the connection can be kept. */
        if (!c->redirect || c->drained + n > FA_HTTP_DRAIN_MAX) return 0;
        c->drained += n;
        return n;
    }
    if (n > 0 && c->on_data(c->user, (const uint8_t*)data, n)) { c->stopped = 1; return 0; }
    return n;
}

static int on_progress(void* user, long long dltotal, long long dlnow, long long ultotal, long long ulnow)
{
    curl_ctx* c = (curl_ctx*)user;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return c->req->cancel && fa_load32(c->req->cancel) ? 1 : 0;
}

/* fa_http_each_header's sink: one "Name: value" list entry ("Name;" sends an empty
 * value: "Name:" would remove the header). The User-Agent also goes to
 * CURLOPT_USERAGENT, for a proxy's CONNECT, which does not get the list. */
typedef struct curl_headers {
    CURL* curl;
    struct curl_slist* list;
} curl_headers;

static int add_list_line(void* user, const char* name, const char* value)
{
    curl_headers* h = (curl_headers*)user;
    struct curl_slist* grown;
    size_t ln = strlen(name), lv = strlen(value);
    char* line = (char*)malloc(ln + lv + 3);
    if (!line) return 1;
    if (lv) snprintf(line, ln + lv + 3, "%s: %s", name, value);
    else snprintf(line, ln + 2, "%s;", name);
    grown = g_curl.slist_append(h->list, line);
    free(line);
    if (!grown) return 1;
    h->list = grown;
    if (fa_strncasecmp_ascii(name, "User-Agent", 11) == 0) g_curl.setopt(h->curl, C_USERAGENT, value);   /* curl copies it */
    return 0;
}

fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user,
                            char** out_redirect)
{
    curl_ctx c;
    curl_headers headers;
    fa_url u;
    int code;
    fa_result r;
    long secs;
    fa_http_session* s = req->session;
    *out_redirect = NULL;
    if (!curl_load()) return FA_ERROR_PROVIDER_MISSING;
    memset(&c, 0, sizeof(c));
    c.req = req;
    c.on_head = on_head;
    c.on_data = on_data;
    c.user = user;
    if (s && s->curl) {
        c.curl = s->curl;
        g_curl.reset(c.curl);
    } else {
        c.curl = g_curl.easy_init();
        if (!c.curl) return FA_ERROR_OUT_OF_MEMORY;
        if (s) s->curl = c.curl;
    }

    /* Redirects come back to fa_http_fetch, which follows them for every transport. */
    g_curl.setopt(c.curl, C_URL, req->url);
    g_curl.setopt(c.curl, C_NOSIGNAL, 1L);
    g_curl.setopt(c.curl, C_CONNECTTIMEOUT_MS, (long)req->timeout_ms);
    secs = (long)((req->timeout_ms + 999) / 1000);
    g_curl.setopt(c.curl, C_LOW_SPEED_LIMIT, 1L);           /* the server went quiet */
    g_curl.setopt(c.curl, C_LOW_SPEED_TIME, secs > 0 ? secs : 1L);
    g_curl.setopt(c.curl, C_HEADERFUNCTION, on_header);
    g_curl.setopt(c.curl, C_HEADERDATA, &c);
    g_curl.setopt(c.curl, C_WRITEFUNCTION, on_write);
    g_curl.setopt(c.curl, C_WRITEDATA, &c);
    g_curl.setopt(c.curl, C_NOPROGRESS, 0L);
    g_curl.setopt(c.curl, C_XFERINFOFUNCTION, on_progress);
    g_curl.setopt(c.curl, C_XFERINFODATA, &c);
    if (req->proxy) g_curl.setopt(c.curl, C_PROXY, req->proxy);    /* "" = no proxy, even from the environment */
    /* Every header goes in the list (User-Agent and Range too), so curl's own give way. */
    headers.curl = c.curl;
    headers.list = NULL;
    r = fa_http_each_header(req, fa_url_parse(req->url, &u) == FA_OK ? &u : NULL, add_list_line, &headers);
    if (headers.list) g_curl.setopt(c.curl, C_HTTPHEADER, headers.list);

    if (r == FA_OK) {
        code = g_curl.perform(c.curl);
        if (code == CE_OK && !c.head_done) deliver_head(&c);   /* an empty body */
        if (req->cancel && fa_load32(req->cancel)) r = FA_ERROR_BUSY;
        else if (c.bad_redirect) r = FA_ERROR_NETWORK;
        else if (c.redirect) { r = FA_OK; *out_redirect = c.redirect; c.redirect = NULL; }
        else if (c.failed_status) r = FA_ERROR_NOT_FOUND;
        else if (c.stopped) r = FA_OK;
        else if (code == CE_OK) r = FA_OK;
        else r = FA_ERROR_NETWORK;
    }
    (void)CE_WRITE_ERROR; (void)CE_ABORTED_BY_CALLBACK;
    if (!s) g_curl.cleanup(c.curl);
    if (headers.list) g_curl.slist_free(headers.list);   /* after perform: curl reads the list until then */
    free(c.headers);
    free(c.redirect);
    return r;
}

void fa_http_abort(fa_atomic32* cancel) { (void)cancel; /* the progress callback polls the flag */ }

#endif
