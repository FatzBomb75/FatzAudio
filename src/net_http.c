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
static fa_atomic32 g_wsa_state;   /* 0 = not started, 1 = starting, 2 = ready */
static int net_startup(void)
{
    for (;;) {
        int32_t st = fa_load32(&g_wsa_state);
        if (st == 2) return 0;
        if (st == 0 && fa_cas32(&g_wsa_state, 0, 1)) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fa_store32(&g_wsa_state, 0); return -1; }
            fa_store32(&g_wsa_state, 2);   /* stays started for the life of the process */
            return 0;
        }
        Sleep(1);
    }
}
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

#define FA_HTTP_HEAD_MAX 65536
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

#if defined(_WIN32) || defined(__APPLE__)
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
#if defined(_WIN32) || defined(__APPLE__)
        if ((!p || !*p) && fa_net_system_proxy("http", target->host, buf, sizeof(buf))) p = buf;
#endif
        if (!p || !*p) return 0;
    }
    if (!*p) return 0;
    if (!strstr(p, "://")) {
        snprintf(buf, sizeof(buf), "http://%s", p);
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

void fa_http_basic_auth(const char* userinfo, char* out, size_t cap)
{
    char plain[256], enc[400];
    percent_decode(userinfo, plain, sizeof(plain));
    fa_base64((const uint8_t*)plain, strlen(plain), enc);
    snprintf(out, cap, "Basic %s", enc);
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

static fa_result connect_to(const fa_http_request* req, const char* host, uint16_t port, fa_socket* out)
{
    struct addrinfo hints, *res = NULL, *ai;
    char service[8];
    fa_result result = FA_ERROR_NETWORK;
    *out = FA_BAD_SOCKET;
    if (net_startup() != 0) return FA_ERROR_NETWORK;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return FA_ERROR_NETWORK;
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
    freeaddrinfo(res);
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
     * headers, the user agent, two auth lines (512 each) and the fixed text. */
    req_cap = 2 * strlen(req->url) + (req->headers ? strlen(req->headers) : 0) +
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
        n = snprintf(request, req_cap, "GET %s%s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n",
                     use_proxy ? "http://" : "", use_proxy ? hostport : "", url.path, hostport, req->user_agent);
    }
    if (req->icy) n += snprintf(request + n, req_cap - (size_t)n, "Icy-MetaData: 1\r\n");
    if (req->range_start >= 0) n += snprintf(request + n, req_cap - (size_t)n, "Range: bytes=%lld-\r\n", (long long)req->range_start);
    if (url.userinfo[0]) {
        fa_http_basic_auth(url.userinfo, auth, sizeof(auth));
        n += snprintf(request + n, req_cap - (size_t)n, "Authorization: %s\r\n", auth);
    }
    if (use_proxy && proxy.userinfo[0]) {
        fa_http_basic_auth(proxy.userinfo, auth, sizeof(auth));
        n += snprintf(request + n, req_cap - (size_t)n, "Proxy-Authorization: %s\r\n", auth);
    }
    if (req->headers) {
        /* The app's lines, normalised to CRLF; empty lines dropped. */
        const char* h = req->headers;
        while (*h) {
            size_t len = strcspn(h, "\r\n");
            if (len > 0 && (size_t)n + len + 3 < req_cap) {
                memcpy(request + n, h, len);
                n += (int)len;
                request[n++] = '\r';
                request[n++] = '\n';
            }
            h += len;
            while (*h == '\r' || *h == '\n') h++;
        }
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

            if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
                fa_http_header(lines, "Location", v, sizeof(v))) {
                char* target = (char*)malloc(FA_NET_URL_MAX);
                if (!target) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
                if (fa_url_resolve(req->url, v, target, FA_NET_URL_MAX) != FA_OK) { free(target); r = FA_ERROR_NETWORK; goto done; }
                *out_redirect = target;
                r = FA_OK;
                goto done;
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

fa_result fa_http_fetch(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user)
{
    fa_http_request cur = *req;
    char* owned = NULL;
    int hops;
    fa_result r = FA_ERROR_NETWORK;
    for (hops = 0; hops <= FA_NET_MAX_REDIRECTS; hops++) {
        fa_url u;
        char* next = NULL;
        if (fa_url_parse(cur.url, &u) != FA_OK) { r = FA_ERROR_INVALID_ARG; break; }
        if (strcmp(u.scheme, "https") == 0) {
            r = fa_http_fetch_tls(&cur, on_head, on_data, user);   /* follows its own redirects */
            break;
        }
        r = fa_http_fetch_socket(&cur, on_head, on_data, user, &next);
        if (r != FA_OK || !next) break;
        free(owned);
        owned = next;
        cur.url = owned;
        r = FA_ERROR_NETWORK;    /* too many redirects, unless a later hop answers */
    }
    free(owned);
    return r;
}
