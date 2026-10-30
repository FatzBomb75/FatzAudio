/* HTTP: the request dispatcher (redirects across http and https, proxies) and
 * FatzAudio's own HTTP/1.1 client on plain sockets for http:// URLs. The own
 * client also understands Shoutcast's "ICY 200 OK" responses, which the system
 * HTTP stacks reject. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _DEFAULT_SOURCE
#  if defined(__APPLE__)
#    define _DARWIN_C_SOURCE
#  endif
#endif
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET fa_socket;
#  define FA_BAD_SOCKET INVALID_SOCKET
#  define fa_closesocket closesocket
static int sock_would_block(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static int sock_set_nonblocking(fa_socket s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on); }
static fa_atomic32 g_wsa_state;
static int wsa_start(void* ctx)
{
    WSADATA wsa;
    (void)ctx;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;   /* stays started for the life of the process */
}
static int net_startup(void) { return fa_once(&g_wsa_state, wsa_start, NULL) ? 0 : -1; }
#  define FA_SEND_FLAGS 0
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
typedef int fa_socket;
#  define FA_BAD_SOCKET (-1)
#  define fa_closesocket close
static int sock_would_block(void) { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR; }
static int sock_set_nonblocking(fa_socket s) { int fl = fcntl(s, F_GETFL, 0); return fcntl(s, F_SETFL, fl | O_NONBLOCK); }
static int net_startup(void) { return 0; }
#  if defined(MSG_NOSIGNAL)
#    define FA_SEND_FLAGS MSG_NOSIGNAL
#  else
#    define FA_SEND_FLAGS 0
#  endif
#endif

#define FA_HTTP_SLICE_MS 100

static int cancelled(const fa_http_request* req) { return req->cancel && fa_load32(req->cancel); }

/* ---- proxies ---------------------------------------------------------------- */

static int is_loopback(const char* host)
{
    return fa_strncasecmp_ascii(host, "localhost", 10) == 0 || strncmp(host, "127.", 4) == 0 || strcmp(host, "::1") == 0;
}

/* no_proxy: comma-separated host names, ".domain" / "*.domain" suffixes, or "*". */
static int no_proxy_matches(const char* list, const char* host)
{
    size_t hl = strlen(host);
    while (list && *list) {
        size_t len = strcspn(list, ", ");
        const char* item = list;
        if (len > 0) {
            if (len == 1 && item[0] == '*') return 1;
            if (item[0] == '*') { item++; len--; }
            if (item[0] == '.') { item++; len--; }
            if (len > 0 && len <= hl && fa_strncasecmp_ascii(host + hl - len, item, len) == 0 &&
                (len == hl || host[hl - len - 1] == '.'))
                return 1;
        }
        list += strcspn(list, ", ");
        while (*list == ',' || *list == ' ') list++;
    }
    return 0;
}

#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
int fa_net_system_proxy(const char* scheme, const char* host, char* out, size_t cap);  /* net_https_*.c */
#endif

/* The proxy for an http:// request as "host:port" plus credentials, or 0 = direct. */
static int find_proxy(const fa_http_request* req, const fa_url* target, fa_url* proxy)
{
    char buf[1024];
    const char* p = req->proxy;
    if (is_loopback(target->host)) return 0;
    if (!p) {
        const char* np = getenv("no_proxy");
        if (!np) np = getenv("NO_PROXY");
        if (np && no_proxy_matches(np, target->host)) return 0;
        p = getenv("http_proxy");
        if (!p || !*p) p = getenv("HTTP_PROXY");
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
        if ((!p || !*p) && fa_net_system_proxy("http", target->host, buf, sizeof(buf))) p = buf;
#endif
        if (!p || !*p) return 0;
    }
    if (!*p) return 0;
    if (!strstr(p, "://")) {
        /* p may be buf itself (the system proxy): move it, never snprintf over it. */
        size_t len = strlen(p);
        if (len + 8 > sizeof(buf)) return 0;
        memmove(buf + 7, p, len + 1);
        memcpy(buf, "http://", 7);
        p = buf;
    }
    return fa_url_parse(p, proxy) == FA_OK;
}

/* Percent-decodes userinfo for Basic credentials. */
static void percent_decode(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    while (*in && o + 1 < cap) {
        if (in[0] == '%' && in[1] && in[2]) {
            char hex[3] = { in[1], in[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            in += 3;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = 0;
}

/* "Basic <base64>" for a URL's percent-encoded "user:password". */
static void basic_auth(const char* userinfo, char* out, size_t cap)
{
    char plain[256], enc[400];
    percent_decode(userinfo, plain, sizeof(plain));
    fa_base64((const uint8_t*)plain, strlen(plain), enc);
    snprintf(out, cap, "Basic %s", enc);
}

/* ---- request headers ---------------------------------------------------------- */

/* A header name is an RFC 9110 token. */
static int valid_name(const char* s)
{
    if (!*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("!#$%&'*+-.^_`|~", c)))
            return 0;
    }
    return 1;
}

/* A value is printable ASCII and tabs. Other bytes (RFC 9110's obs-text) are
 * refused: WinHTTP takes headers as UTF-16 and HttpURLConnection throws on them,
 * so they could not be sent the same way everywhere. */
static int valid_value(const char* s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c >= 0x7F) return 0;
    }
    return 1;
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

static int same_name(const char* a, const char* b) { return fa_strncasecmp_ascii(a, b, strlen(a) + 1) == 0; }

/* The app's header text as "name\0value\0" pairs in `out` (room for strlen(text) + 2),
 * the valid lines only. Returns the number of pairs. */
static size_t parse_app_headers(const char* text, char* out, int other_origin)
{
    static const char* const owned[] = { "Host", "Connection", "Content-Length", "Transfer-Encoding",
                                         "Authorization", "Cookie" };   /* the last two: for the first origin only */
    size_t count = 0, i, nowned = sizeof(owned) / sizeof(owned[0]) - (other_origin ? 0 : 2);
    char* o = out;
    while (*text) {
        size_t len = strcspn(text, "\r\n");
        const char* colon = (const char*)memchr(text, ':', len);
        if (colon && len <= FA_HTTP_LINE_MAX) {
            const char* name_end = colon;
            const char* v = colon + 1;
            const char* v_end = text + len;
            char *name = o, *value;
            int keep;
            while (name_end > text && is_space(name_end[-1])) name_end--;
            while (v < v_end && is_space(*v)) v++;
            while (v_end > v && is_space(v_end[-1])) v_end--;
            memcpy(o, text, (size_t)(name_end - text));
            o += name_end - text;
            *o++ = 0;
            value = o;
            memcpy(o, v, (size_t)(v_end - v));
            o += v_end - v;
            *o++ = 0;
            keep = valid_name(name) && valid_value(value);
            for (i = 0; keep && i < nowned; i++)
                if (same_name(name, owned[i])) keep = 0;
            if (keep) count++;
            else o = name;
        }
        text += len;
        while (*text == '\r' || *text == '\n') text++;
    }
    return count;
}

fa_result fa_http_each_header(const fa_http_request* req, const fa_url* url, fa_http_header_proc add, void* user)
{
    const char* own[6][2];
    char range[48], auth[512];
    char* app = NULL;
    char* jar = NULL;
    size_t napp = 0, nown = 0, i, j;
    fa_result r = FA_OK;
    if (req->headers && req->headers[0]) {
        app = (char*)malloc(strlen(req->headers) + 2);
        if (!app) return FA_ERROR_OUT_OF_MEMORY;
        napp = parse_app_headers(req->headers, app, req->other_origin);
    }
    if (req->cookies) jar = fa_http_cookies_for(req->cookies, req->url);   /* NULL also when out of memory: sent without */
    if (req->user_agent) { own[nown][0] = "User-Agent"; own[nown++][1] = req->user_agent; }
    own[nown][0] = "Accept"; own[nown++][1] = "*/*";
    /* The bytes as the server stores them: byte ranges and offsets must match. */
    own[nown][0] = "Accept-Encoding"; own[nown++][1] = "identity";
    if (req->icy) { own[nown][0] = "Icy-MetaData"; own[nown++][1] = "1"; }
    if (req->range_start >= 0) {
        if (req->range_length > 0)
            snprintf(range, sizeof(range), "bytes=%lld-%lld", (long long)req->range_start,
                     (long long)(req->range_start + req->range_length - 1));
        else
            snprintf(range, sizeof(range), "bytes=%lld-", (long long)req->range_start);
        own[nown][0] = "Range"; own[nown++][1] = range;
    }
    if (url && url->userinfo[0]) {
        /* Sent up front by every transport: WinHTTP, URLSession and HttpURLConnection
         * ignore credentials in the URL or answer challenges only through callbacks. */
        basic_auth(url->userinfo, auth, sizeof(auth));
        own[nown][0] = "Authorization"; own[nown++][1] = auth;
    }
    for (i = 0; i < nown && r == FA_OK; i++) {
        const char* p = app;
        int replaced = 0;
        for (j = 0; j < napp && !replaced; j++) {
            replaced = same_name(p, own[i][0]);
            p += strlen(p) + 1;
            p += strlen(p) + 1;
        }
        if (!replaced && valid_value(own[i][1]) && add(user, own[i][0], own[i][1])) r = FA_ERROR_OUT_OF_MEMORY;
    }
    {
        const char* p = app;
        for (j = 0; j < napp && r == FA_OK; j++) {
            const char* name = p;
            const char* value = p + strlen(p) + 1;
            p = value + strlen(value) + 1;
            if (jar && same_name(name, "Cookie")) {
                /* One Cookie line: the app's cookies, then the servers'. */
                size_t lv = strlen(value), lj = strlen(jar);
                char* both = (char*)malloc(lv + lj + 3);
                if (!both) { r = FA_ERROR_OUT_OF_MEMORY; break; }
                memcpy(both, value, lv);
                memcpy(both + lv, "; ", 2);
                memcpy(both + lv + 2, jar, lj + 1);
                if (lv + lj + 2 <= FA_HTTP_LINE_MAX && add(user, name, both)) r = FA_ERROR_OUT_OF_MEMORY;
                else if (lv + lj + 2 > FA_HTTP_LINE_MAX && add(user, name, value)) r = FA_ERROR_OUT_OF_MEMORY;
                free(both);
                free(jar);
                jar = NULL;
                continue;
            }
            if (add(user, name, value)) r = FA_ERROR_OUT_OF_MEMORY;
        }
    }
    if (r == FA_OK && jar && add(user, "Cookie", jar)) r = FA_ERROR_OUT_OF_MEMORY;
    free(jar);
    free(app);
    return r;
}

/* ---- redirects ------------------------------------------------------------------- */

int fa_http_redirect_target(const fa_http_request* req, int status, const char* headers, char** out)
{
    char loc[FA_NET_URL_MAX];
    *out = NULL;
    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) return 0;
    if (!fa_http_header(headers, "Location", loc, sizeof(loc))) return 0;   /* delivered as it is */
    /* A login redirect often sets the session cookie the next hop wants. */
    if (req->cookies) fa_http_cookies_take(req->cookies, req->url, headers);
    *out = (char*)malloc(FA_NET_URL_MAX);
    if (!*out) return -1;
    if (fa_url_resolve(req->url, loc, *out, FA_NET_URL_MAX) != FA_OK) { free(*out); *out = NULL; return -1; }
    return 1;
}

int fa_http_unframed(const char* headers)
{
    char v[64];
    return !fa_http_header(headers, "Content-Length", v, sizeof(v)) && !fa_http_header(headers, "Transfer-Encoding", v, sizeof(v));
}

static int same_origin(const fa_url* a, const fa_url* b)
{
    return strcmp(a->scheme, b->scheme) == 0 && a->port == b->port && fa_strncasecmp_ascii(a->host, b->host, sizeof(a->host)) == 0;
}

/* The next hop of a redirect from the request for `first`: `target` (malloc'd,
 * absolute) with the user info of `first` when it goes back to that origin without
 * credentials of its own (an absolute Location on the same server). *out_other: the
 * hop is not on `app`'s origin (where the app's Authorization and Cookie may go). Returns the URL to fetch (target, or a new string
 * replacing it), NULL when out of memory. */
static char* next_hop(const fa_url* first, const fa_url* app, char* target, int* out_other)
{
    fa_url t;
    char* with;
    const char* rest;
    size_t ln, lr;
    if (fa_url_parse(target, &t) != FA_OK) { *out_other = 1; return target; }   /* the transport rejects it */
    *out_other = !same_origin(app, &t);
    if (!same_origin(first, &t) || t.userinfo[0] || !first->userinfo[0]) return target;
    rest = strstr(target, "://") + 3;
    ln = strlen(first->userinfo);
    lr = strlen(rest);
    if ((size_t)(rest - target) + ln + 1 + lr >= FA_NET_URL_MAX) return target;   /* too long: without them */
    with = (char*)malloc(FA_NET_URL_MAX);
    if (!with) { free(target); return NULL; }
    memcpy(with, target, (size_t)(rest - target));
    memcpy(with + (rest - target), first->userinfo, ln);
    with[(rest - target) + ln] = '@';
    memcpy(with + (rest - target) + ln + 1, rest, lr + 1);
    free(target);
    return with;
}

/* ---- sockets ------------------------------------------------------------------ */

/* Waits up to FA_HTTP_SLICE_MS for the socket to become readable (write = 0) or
 * writable (write = 1). 1 ready, 0 timeout, -1 error. */
static int wait_socket(fa_socket s, int write)
{
    fd_set set, err;
    struct timeval tv;
    int r;
    FD_ZERO(&set);
    FD_ZERO(&err);
    FD_SET(s, &set);
    FD_SET(s, &err);
    tv.tv_sec = 0;
    tv.tv_usec = FA_HTTP_SLICE_MS * 1000;
    r = select((int)s + 1, write ? NULL : &set, write ? &set : NULL, &err, &tv);
    if (r < 0) {
#if !defined(_WIN32)
        if (errno == EINTR) return 0;
#endif
        return -1;
    }
    if (r == 0) return 0;
    return FD_ISSET(s, &set) ? 1 : -1;
}

/* ---- resolving ------------------------------------------------------------------ */

void (*fa_net_test_resolve_hook)(const char* host);

/* getaddrinfo can't be cancelled, so it runs on a detached helper thread: a close
 * (or a timeout) leaves it behind instead of waiting for it. The caller and the
 * helper each hold a reference; the last one to let go frees the lookup. */
typedef struct resolve_job {
    fa_atomic32 refs;
    fa_mutex lock;
    fa_cond cond;
    int done;
    int status;               /* getaddrinfo's */
    struct addrinfo* res;
    char service[8];
    char host[sizeof(((fa_url*)0)->host)];
} resolve_job;

static void resolve_release(resolve_job* j)
{
    if (fa_fetch_add32(&j->refs, -1) != 1) return;
    if (j->res) freeaddrinfo(j->res);
    fa_cond_destroy(&j->cond);
    fa_mutex_destroy(&j->lock);
    free(j);
}

static void resolve_main(void* arg)
{
    resolve_job* j = (resolve_job*)arg;
    struct addrinfo hints, *res = NULL;
    int status;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (fa_net_test_resolve_hook) fa_net_test_resolve_hook(j->host);
    status = getaddrinfo(j->host, j->service, &hints, &res);
    fa_mutex_lock(&j->lock);
    j->status = status;
    j->res = status == 0 ? res : NULL;
    j->done = 1;
    fa_cond_broadcast(&j->cond);
    fa_mutex_unlock(&j->lock);
    resolve_release(j);
}

/* The addresses of host:port, or FA_ERROR_BUSY when `cancel` was raised and
 * FA_ERROR_NETWORK when the lookup failed or took longer than the timeout. On FA_OK
 * the caller frees *out_job (resolve_release) when done with *out_res. */
static fa_result resolve(const fa_http_request* req, const char* host, uint16_t port, resolve_job** out_job,
                         struct addrinfo** out_res)
{
    resolve_job* j = (resolve_job*)calloc(1, sizeof(*j));
    fa_thread t;
    int64_t deadline = fa_monotonic_ns() + (int64_t)req->timeout_ms * 1000000;
    fa_result r;
    *out_job = NULL;
    *out_res = NULL;
    if (!j) return FA_ERROR_OUT_OF_MEMORY;
    if (strlen(host) >= sizeof(j->host)) { free(j); return FA_ERROR_NETWORK; }
    if (fa_mutex_init(&j->lock) != 0) { free(j); return FA_ERROR_OUT_OF_MEMORY; }
    if (fa_cond_init(&j->cond) != 0) { fa_mutex_destroy(&j->lock); free(j); return FA_ERROR_OUT_OF_MEMORY; }
    strcpy(j->host, host);
    snprintf(j->service, sizeof(j->service), "%u", (unsigned)port);
    fa_store32(&j->refs, 2);
    if (fa_thread_create(&t, resolve_main, j) != 0) {
        fa_store32(&j->refs, 1);
        resolve_release(j);
        return FA_ERROR_GENERIC;
    }
    fa_thread_detach(&t);
    fa_mutex_lock(&j->lock);
    /* Short slices: nothing wakes this wait on a close but the flag. */
    while (!j->done && !cancelled(req) && fa_monotonic_ns() < deadline) fa_cond_wait_ms(&j->cond, &j->lock, 10);
    r = j->done ? (j->status == 0 && j->res ? FA_OK : FA_ERROR_NETWORK) : cancelled(req) ? FA_ERROR_BUSY : FA_ERROR_NETWORK;
    fa_mutex_unlock(&j->lock);
    if (r != FA_OK) { resolve_release(j); return r; }
    *out_job = j;
    *out_res = j->res;
    return FA_OK;
}

static fa_result connect_to(const fa_http_request* req, const char* host, uint16_t port, fa_socket* out)
{
    struct addrinfo *res = NULL, *ai;
    resolve_job* job;
    fa_result result;
    *out = FA_BAD_SOCKET;
    if (net_startup() != 0) return FA_ERROR_NETWORK;
    result = resolve(req, host, port, &job, &res);
    if (result != FA_OK) return result;
    result = FA_ERROR_NETWORK;
    for (ai = res; ai; ai = ai->ai_next) {
        fa_socket s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        uint32_t waited = 0;
        int ok = 0;
        if (s == FA_BAD_SOCKET) continue;
#if defined(SO_NOSIGPIPE)
        { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
        { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)); }
        sock_set_nonblocking(s);
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) ok = 1;
        else if (sock_would_block()) {
            while (waited < req->timeout_ms && !cancelled(req)) {
                int w = wait_socket(s, 1);
                if (w < 0) break;
                if (w > 0) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                    ok = err == 0;
                    break;
                }
                waited += FA_HTTP_SLICE_MS;
            }
        }
        if (ok) { *out = s; result = FA_OK; break; }
        fa_closesocket(s);
        if (cancelled(req)) { result = FA_ERROR_BUSY; break; }
    }
    resolve_release(job);
    return result;
}

static fa_result send_all(const fa_http_request* req, fa_socket s, const char* data, size_t n)
{
    uint32_t idle = 0;
    while (n > 0) {
        int w;
        if (cancelled(req)) return FA_ERROR_BUSY;
        w = (int)send(s, data, (int)n, FA_SEND_FLAGS);
        if (w > 0) { data += w; n -= (size_t)w; idle = 0; continue; }
        if (w < 0 && !sock_would_block()) return FA_ERROR_NETWORK;
        if (wait_socket(s, 1) < 0) return FA_ERROR_NETWORK;
        idle += FA_HTTP_SLICE_MS;
        if (idle >= req->timeout_ms) return FA_ERROR_NETWORK;
    }
    return FA_OK;
}

/* Reads what is there (up to cap): >0 bytes, 0 = closed by the server, <0 = fa_result. */
static int64_t recv_some(const fa_http_request* req, fa_socket s, uint8_t* buf, size_t cap)
{
    uint32_t idle = 0;
    for (;;) {
        int r;
        if (cancelled(req)) return FA_ERROR_BUSY;
        r = (int)recv(s, (char*)buf, (int)cap, 0);
        if (r > 0) return r;
        if (r == 0) return 0;
        if (!sock_would_block()) return FA_ERROR_NETWORK;
        r = wait_socket(s, 0);
        if (r < 0) return FA_ERROR_NETWORK;
        if (r == 0) {
            idle += FA_HTTP_SLICE_MS;
            if (idle >= req->timeout_ms) return FA_ERROR_NETWORK;   /* the server went quiet */
        }
    }
}

/* ---- the client --------------------------------------------------------------- */

typedef struct body_state {
    int chunked;
    int chunk_state;          /* 0 size line, 1 data, 2 CRLF after data, 3 trailers, 4 done */
    int64_t chunk_left;
    char line[64];
    size_t line_len;
    int64_t left;             /* plain body: bytes still expected, -1 = until closed */
} body_state;

/* Feeds raw body bytes through the chunked decoder. Returns 1 when the callback
 * stopped, 2 when the body is complete, 0 to read more, -1 on a malformed body. */
static int body_feed(body_state* b, const uint8_t* p, size_t n, fa_http_data_proc on_data, void* user)
{
    if (!b->chunked) {
        if (b->left >= 0 && (int64_t)n > b->left) n = (size_t)b->left;
        if (n > 0 && on_data(user, p, n)) return 1;
        if (b->left >= 0) {
            b->left -= (int64_t)n;
            if (b->left == 0) return 2;
        }
        return 0;
    }
    while (n > 0) {
        if (b->chunk_state == 1) {
            size_t k = (int64_t)n < b->chunk_left ? n : (size_t)b->chunk_left;
            if (on_data(user, p, k)) return 1;
            p += k; n -= k; b->chunk_left -= (int64_t)k;
            if (b->chunk_left == 0) { b->chunk_state = 2; b->line_len = 0; }
            continue;
        }
        if (b->chunk_state == 4) return 2;
        /* Line-based states: size line, the CRLF after data, trailer lines. */
        {
            char c = (char)*p++;
            n--;
            if (c != '\n') {
                if (c != '\r' && b->line_len + 1 < sizeof(b->line)) b->line[b->line_len++] = c;
                continue;
            }
            b->line[b->line_len] = 0;
            if (b->chunk_state == 0) {
                char* end;
                long long size = strtoll(b->line, &end, 16);
                if (end == b->line || size < 0) return -1;
                b->chunk_left = (int64_t)size;
                b->chunk_state = size == 0 ? 3 : 1;
            } else if (b->chunk_state == 2) {
                b->chunk_state = 0;
            } else if (b->chunk_state == 3 && b->line_len == 0) {
                b->chunk_state = 4;
                return 2;
            }
            b->line_len = 0;
        }
    }
    return b->chunk_state == 4 ? 2 : 0;
}

/* The socket client's sink for fa_http_each_header: "Name: value\r\n" lines. */
typedef struct text_sink {
    char* buf;
    size_t n, cap;
} text_sink;

static int add_text_line(void* user, const char* name, const char* value)
{
    text_sink* t = (text_sink*)user;
    size_t ln = strlen(name), lv = strlen(value);
    if (t->n + ln + lv + 7 > t->cap) return 1;   /* + ": ", CRLF, and the blank line's CRLF and NUL */
    memcpy(t->buf + t->n, name, ln);
    memcpy(t->buf + t->n + ln, ": ", 2);
    memcpy(t->buf + t->n + ln + 2, value, lv);
    memcpy(t->buf + t->n + ln + 2 + lv, "\r\n", 2);
    t->n += ln + lv + 4;
    return 0;
}

fa_result fa_http_fetch_socket(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data,
                               void* user, char** out_redirect)
{
    fa_url url, proxy;
    int use_proxy;
    fa_socket s = FA_BAD_SOCKET;
    char* request = NULL;
    uint8_t* head = NULL;
    size_t head_len = 0, req_cap;
    fa_result r;
    char auth[512];
    int n;
    *out_redirect = NULL;
    if (fa_url_parse(req->url, &url) != FA_OK || strcmp(url.scheme, "http") != 0) return FA_ERROR_INVALID_ARG;
    use_proxy = find_proxy(req, &url, &proxy);

    r = use_proxy ? connect_to(req, proxy.host, proxy.port, &s) : connect_to(req, url.host, url.port, &s);
    if (r != FA_OK) return r;

    /* Room for every line below: URL (path) twice through a proxy, the app's
     * headers (each line can grow by its ": " and CRLF), the user agent, two auth
     * lines (512 each) and the fixed text. */
    req_cap = 2 * strlen(req->url) + (req->headers ? 2 * strlen(req->headers) : 0) +
              (req->user_agent ? strlen(req->user_agent) : 0) + 2048;
    request = (char*)malloc(req_cap);
    head = (uint8_t*)malloc(FA_HTTP_HEAD_MAX + 1);
    if (!request || !head) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    {
        char hostport[300];
        int v6 = strchr(url.host, ':') != NULL;
        if (url.default_port) snprintf(hostport, sizeof(hostport), "%s%s%s", v6 ? "[" : "", url.host, v6 ? "]" : "");
        else snprintf(hostport, sizeof(hostport), "%s%s%s:%u", v6 ? "[" : "", url.host, v6 ? "]" : "", (unsigned)url.port);
        /* Through a proxy the request line carries the whole URL (without credentials). */
        n = snprintf(request, req_cap, "GET %s%s%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                     use_proxy ? "http://" : "", use_proxy ? hostport : "", url.path, hostport);
    }
    if (use_proxy && proxy.userinfo[0]) {
        basic_auth(proxy.userinfo, auth, sizeof(auth));
        n += snprintf(request + n, req_cap - (size_t)n, "Proxy-Authorization: %s\r\n", auth);
    }
    {
        text_sink sink;
        sink.buf = request;
        sink.n = (size_t)n;
        sink.cap = req_cap;
        r = fa_http_each_header(req, &url, add_text_line, &sink);
        if (r != FA_OK) goto done;
        n = (int)sink.n;
    }
    n += snprintf(request + n, req_cap - (size_t)n, "\r\n");
    r = send_all(req, s, request, (size_t)n);
    if (r != FA_OK) goto done;

    /* Response head: until the blank line. */
    for (;;) {
        int64_t got;
        uint8_t* end;
        if (head_len >= FA_HTTP_HEAD_MAX) { r = FA_ERROR_NETWORK; goto done; }
        got = recv_some(req, s, head + head_len, FA_HTTP_HEAD_MAX - head_len);
        if (got < 0) { r = (fa_result)got; goto done; }
        if (got == 0) { r = FA_ERROR_NETWORK; goto done; }
        head_len += (size_t)got;
        head[head_len] = 0;
        end = (uint8_t*)strstr((char*)head, "\r\n\r\n");
        if (end) { end += 4; } else if ((end = (uint8_t*)strstr((char*)head, "\n\n")) != NULL) { end += 2; }
        if (end) {
            size_t hlen = (size_t)(end - head);
            fa_http_response resp;
            body_state body;
            char* lines;
            char v[FA_NET_URL_MAX];
            int status = 0;
            char* sp;
            /* "HTTP/1.1 200 OK" or Shoutcast's "ICY 200 OK". */
            if (strncmp((char*)head, "HTTP/", 5) != 0 && strncmp((char*)head, "ICY ", 4) != 0) { r = FA_ERROR_NETWORK; goto done; }
            sp = strchr((char*)head, ' ');
            if (sp) status = atoi(sp + 1);
            if (status < 100 || status > 599) { r = FA_ERROR_NETWORK; goto done; }
            lines = (char*)head + strcspn((char*)head, "\n");
            if (*lines) lines++;
            head[hlen - 1] = 0;  /* cut before the body; keeps the last header's line end */
            memset(&resp, 0, sizeof(resp));
            resp.status = status;
            resp.headers = lines;
            resp.final_url = req->url;
            fa_http_fill_lengths(&resp);

            switch (fa_http_redirect_target(req, status, lines, out_redirect)) {
            case 1: r = FA_OK; goto done;          /* the connection closes: its body doesn't matter */
            case -1: r = FA_ERROR_NETWORK; goto done;
            default: break;
            }
            if (on_head && on_head(user, &resp)) { r = FA_OK; goto done; }
            if (status >= 400) { r = FA_ERROR_NOT_FOUND; goto done; }

            memset(&body, 0, sizeof(body));
            body.left = -1;
            if (fa_http_header(lines, "Transfer-Encoding", v, sizeof(v)) && strstr(v, "chunked")) body.chunked = 1;
            else if (resp.content_length >= 0) body.left = resp.content_length;
            if (status == 204 || status == 304) body.left = 0;
            {
                int fr = 0;
                if (!body.chunked && body.left == 0) { r = FA_OK; goto done; }
                if (head_len > hlen) fr = body_feed(&body, head + hlen, head_len - hlen, on_data, user);
                while (fr == 0) {
                    int64_t got2 = recv_some(req, s, head, FA_HTTP_HEAD_MAX);
                    if (got2 < 0) { r = (fa_result)got2; goto done; }
                    if (got2 == 0) {
                        /* Closed: complete only if no length was promised. */
                        r = (body.chunked || body.left > 0) ? FA_ERROR_NETWORK : FA_OK;
                        if (r == FA_OK && req->closed_end) *req->closed_end = 1;
                        goto done;
                    }
                    fr = body_feed(&body, head, (size_t)got2, on_data, user);
                }
                r = fr < 0 ? FA_ERROR_NETWORK : FA_OK;
            }
            goto done;
        }
    }
done:
    if (s != FA_BAD_SOCKET) fa_closesocket(s);
    free(request);
    free(head);
    return r;
}

/* ---- dispatcher ------------------------------------------------------------------ */

int64_t fa_http_range_skip(const fa_http_response* resp, int64_t offset, int any_status)
{
    if (resp->status == 206 && resp->range_start <= offset) return offset - resp->range_start;
    if (resp->status == 200 || (resp->status != 206 && any_status)) return offset;
    return -1;
}

/* fa_http_fetch's callbacks when the request has a cookie jar: the response's
 * cookies are kept, then the caller's callbacks run. */
typedef struct cookie_wrap {
    const fa_http_request* req;   /* the hop being fetched */
    fa_http_head_proc on_head;
    fa_http_data_proc on_data;
    void* user;
} cookie_wrap;

static int cookie_head_proc(void* user, const fa_http_response* resp)
{
    cookie_wrap* w = (cookie_wrap*)user;
    fa_http_cookies_take(w->req->cookies, w->req->url, resp->headers);
    return w->on_head ? w->on_head(w->user, resp) : 0;
}

static int cookie_data_proc(void* user, const uint8_t* data, size_t n)
{
    cookie_wrap* w = (cookie_wrap*)user;
    return w->on_data(w->user, data, n);
}

fa_result fa_http_fetch(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user)
{
    fa_http_request cur = *req;
    char* owned = NULL;
    fa_url first, app;
    cookie_wrap cw;
    int hops;
    fa_result r = FA_ERROR_NETWORK;
    if (fa_url_parse(req->url, &first) != FA_OK) return FA_ERROR_INVALID_ARG;
    if (!req->app_origin) app = first;
    else if (fa_url_parse(req->app_origin, &app) != FA_OK) memset(&app, 0, sizeof(app));   /* matches nothing */
    cur.other_origin = !same_origin(&app, &first);
    if (req->closed_end) *req->closed_end = 0;
    if (req->cookies) {
        cw.req = &cur;
        cw.on_head = on_head;
        cw.on_data = on_data;
        cw.user = user;
        on_head = cookie_head_proc;
        on_data = cookie_data_proc;
        user = &cw;
    }
    for (hops = 0; hops <= FA_NET_MAX_REDIRECTS; hops++) {
        fa_url u;
        char* next = NULL;
        if (fa_url_parse(cur.url, &u) != FA_OK) { r = FA_ERROR_INVALID_ARG; break; }
        if (strcmp(u.scheme, "https") == 0) r = fa_http_fetch_tls(&cur, on_head, on_data, user, &next);
        else r = fa_http_fetch_socket(&cur, on_head, on_data, user, &next);
        if (r != FA_OK || !next) { free(next); break; }
        if (cur.cancel && fa_load32(cur.cancel)) { free(next); r = FA_ERROR_BUSY; break; }
        next = next_hop(&first, &app, next, &cur.other_origin);
        if (!next) { r = FA_ERROR_OUT_OF_MEMORY; break; }
        free(owned);
        owned = next;
        cur.url = owned;
        r = FA_ERROR_NETWORK;    /* too many redirects, unless a later hop answers */
    }
    free(owned);
    return r;
}
