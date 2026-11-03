/* Fuzz target: the web client, against a small server on 127.0.0.1 inside the process, and the text parsers behind it.
 * The first input byte picks the mode (mode = byte & 3); the rest is the payload.
 *   0  text: URL, NUL, response headers, NUL, URL. Set-Cookie lines and dates (net_cookie.c) into a jar and the Cookie
 *      header back for both URLs, URL parse / resolve / proxy parse, header lookup and the first entry of a .pls / .m3u.
 *   1  the payload IS the server's reply to every request, verbatim (status line, headers, chunked or unframed body,
 *      redirects, ICY, Set-Cookie, ranges): net_http.c, the redirect and cookie rules, ICY metadata, then the audio.
 *   2  the payload is a file the server serves with byte ranges (Range, 206, 416): the MP4 sidx / tfra / mfra and the
 *      Matroska Cues and last-block reads over their own range requests (fa_net_read_range), indexed seeks.
 *   3  HLS: the payload up to its first NUL is the playlist (any path ending in .m3u8), after it the bytes of every
 *      segment, served with ranges.
 * The client opens http://127.0.0.1:<port>/x, reads some blocks, seeks twice and closes. Modes 1 to 3 need sockets, so
 * they do nothing on Windows (the regression run there covers mode 0). */
#include "fa_internal.h"
#include "net.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static fa_engine* g_engine;
static int g_listen = -1, g_port;
static fa_thread g_thread, g_dog;
static fa_mutex g_lock;
static int g_mode;
static const uint8_t* g_body;     /* mode 1: the reply; 2: the file; 3: the segment bytes */
static size_t g_body_n;
static const uint8_t* g_list;     /* mode 3: the playlist */
static size_t g_list_n;

static void send_all(int s, const void* p, size_t n)
{
    const char* c = (const char*)p;
    while (n > 0) {
        ssize_t k = send(s, c, n > 65536 ? 65536 : n, MSG_NOSIGNAL);
        if (k <= 0) return;
        c += k;
        n -= (size_t)k;
    }
}

/* body[0, n) with the request's Range honoured, as a server with Accept-Ranges does. */
static void serve_ranged(int s, const char* req, const uint8_t* body, size_t n, const char* type)
{
    char head[512];
    const char* r = strstr(req, "\r\nRange: bytes=");
    long long a = 0, b = (long long)n - 1;
    int partial = 0;
    if (r) {
        char* end;
        if (r[15] == '-') {   /* a suffix range: the last N bytes */
            a = (long long)n - strtoll(r + 16, NULL, 10);
            if (a < 0) a = 0;
            partial = 1;
            end = NULL;
        } else {
            a = strtoll(r + 15, &end, 10);
        }
        if (end && *end == '-') {
            if (end[1] >= '0' && end[1] <= '9') b = strtoll(end + 1, NULL, 10);
            partial = 1;
        }
    }
    if (partial && (a < 0 || a >= (long long)n || b < a)) {
        snprintf(head, sizeof(head), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%zu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", n);
        send_all(s, head, strlen(head));
        return;
    }
    if (b >= (long long)n) b = (long long)n - 1;
    if (partial)
        snprintf(head, sizeof(head), "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%zu\r\n"
                 "Content-Length: %lld\r\nConnection: close\r\n\r\n", type, a, b, n, b - a + 1);
    else
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", type, n);
    send_all(s, head, strlen(head));
    if (n) send_all(s, body + a, (size_t)(b - a + 1));
}

static void serve_one(int c)
{
    char req[8192], path[1024] = "";
    size_t got = 0;
    int mode;
    uint8_t *body = NULL, *list = NULL;
    size_t body_n, list_n;
    while (got < sizeof(req) - 1) {
        ssize_t k = recv(c, req + got, sizeof(req) - 1 - got, 0);
        if (k <= 0) return;
        got += (size_t)k;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    sscanf(req, "GET %1023s", path);
    /* Copies: the input's bytes are freed once its open ends, which may be while a
     * late request from it is still being answered here. */
    fa_mutex_lock(&g_lock);
    mode = g_mode; body_n = g_body_n; list_n = g_list_n;
    body = (uint8_t*)malloc(body_n ? body_n : 1);
    list = (uint8_t*)malloc(list_n ? list_n : 1);
    if (body && body_n) memcpy(body, g_body, body_n);
    if (list && list_n) memcpy(list, g_list, list_n);
    fa_mutex_unlock(&g_lock);
    if (!body || !list) mode = 0;
    if (mode == 1) {
        send_all(c, body, body_n);
    } else if (mode == 2) {
        serve_ranged(c, req, body, body_n, "application/octet-stream");
    } else if (mode == 3) {
        size_t pl = strlen(path);
        if (pl >= 5 && strcmp(path + pl - 5, ".m3u8") == 0) serve_ranged(c, req, list, list_n, "application/vnd.apple.mpegurl");
        else serve_ranged(c, req, body, body_n, "video/mp2t");
    }
    free(body);
    free(list);
}

/* One connection at a time: every reply ends with the connection closed. (A stream's side request for an index waits for
 * the connection of its download; both are short, and a stalled one ends at the 2 s socket timeouts.) */
static void server_main(void* arg)
{
    (void)arg;
    for (;;) {
        struct timeval tv = { 2, 0 };
        int c = accept(g_listen, NULL, NULL);
        if (c < 0) continue;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serve_one(c);
        shutdown(c, SHUT_RDWR);
        close(c);
    }
}

static void watchdog_main(void* arg);

static int server_start(void)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int one = 1;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) return -1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(g_listen, 64) != 0) return -1;
    if (getsockname(g_listen, (struct sockaddr*)&a, &len) != 0) return -1;
    g_port = ntohs(a.sin_port);
    fa_mutex_init(&g_lock);
    return fa_thread_create(&g_thread, server_main, NULL) || fa_thread_create(&g_dog, watchdog_main, NULL);
}

/* A server that answers but never gives the stream any audio (a live playlist whose segments are empty) keeps an open
 * waiting by design: the app's cancel flag ends it. A watchdog raises that flag after 3 s. */
static volatile int32_t g_cancel;
static fa_atomic64 g_deadline_ns;   /* 0 = no open in progress */

static void watchdog_main(void* arg)
{
    (void)arg;
    for (;;) {
        int64_t d = fa_load64(&g_deadline_ns);
        fa_mutex_lock(&g_lock);   /* with play_url's reset: a stale deadline never cancels the next open */
        if (d && fa_monotonic_ns() > d && fa_load64(&g_deadline_ns) == d) g_cancel = 1;
        fa_mutex_unlock(&g_lock);
        fa_sleep_ms(50);
    }
}

static void play_url_on(const uint8_t* data, size_t size, int mode, const uint8_t* list, size_t list_n)
{
    char url[64];
    fa_open_options o;
    fa_net_options no;
    fa_stream* s = NULL;
    fa_stream_info info = { .struct_size = sizeof(info) };
    static float buf[16384];
    int k;
    fa_mutex_lock(&g_lock);
    g_mode = mode; g_body = data; g_body_n = size; g_list = list; g_list_n = list_n;
    fa_mutex_unlock(&g_lock);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/%s", g_port, mode == 3 ? "p.m3u8" : "x");
    fa_net_options_init(&no);
    no.struct_size = sizeof(no);
    no.timeout_ms = 1500;
    no.prebuffer_ms = 20;
    no.reconnect_ms = 1;
    no.proxy = "";
    no.flags = FA_NET_NO_RECONNECT;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.net = &no;
    o.cancel = &g_cancel;
    fa_mutex_lock(&g_lock);
    g_cancel = 0;
    fa_store64(&g_deadline_ns, fa_monotonic_ns() + 3000000000LL);
    fa_mutex_unlock(&g_lock);
    k = fa_stream_open_url(g_engine, url, &o, &s) != FA_OK;
    fa_mutex_lock(&g_lock);
    fa_store64(&g_deadline_ns, 0);
    fa_mutex_unlock(&g_lock);
    if (k) return;
    fa_stream_get_info(s, &info);
    for (k = 0; k < 8; k++) {
        fa_frames got = 0, i;
        uint32_t ch = info.format.channels ? info.format.channels : 1;
        if (fa_stream_read(s, buf, ch < 4096 ? 4096 / ch : 1, &got) != FA_OK || got == 0) break;
        for (i = 0; i < got * ch; i++)
            if (!isfinite(buf[i])) { fprintf(stderr, "non-finite sample %zu of block %d\n", (size_t)i, k); abort(); }
    }
    if (info.seekable && info.length_frames > 0) {
        fa_frames got, n = 16384 / (info.format.channels ? info.format.channels : 1);
        if (n > 1000) n = 1000;
        fa_stream_seek(s, info.length_frames / 2);
        fa_stream_read(s, buf, n, &got);
        fa_stream_seek(s, info.length_frames / 7);
        fa_stream_read(s, buf, n, &got);
    }
    fa_stream_close(s);
}

/* The server serves the input only while its open runs: after that the bytes are freed. */
static void play_url(const uint8_t* data, size_t size, int mode, const uint8_t* list, size_t list_n)
{
    play_url_on(data, size, mode, list, list_n);
    fa_mutex_lock(&g_lock);
    g_mode = 0; g_body = NULL; g_body_n = 0; g_list = NULL; g_list_n = 0;
    fa_mutex_unlock(&g_lock);
}
#endif /* !_WIN32 */

static void text_mode(const uint8_t* data, size_t size)
{
    /* Three NUL-separated texts: URL, headers, URL. */
    char* t = (char*)malloc(size + 4);
    const char *url1, *hdr, *url2;
    char out[FA_NET_URL_MAX], val[256];
    fa_url u;
    fa_http_cookies* jar;
    char* c;
    int i;
    if (!t) return;
    memcpy(t, data, size);
    memset(t + size, 0, 4);
    url1 = t;
    hdr = url1 + strlen(url1) + 1;
    if (hdr > t + size) hdr = t + size;
    url2 = hdr + strlen(hdr) + 1;
    if (url2 > t + size) url2 = t + size;
    (void)fa_url_parse(url1, &u);
    (void)fa_proxy_parse(url1, &u);
    (void)fa_url_resolve(url1, url2, out, sizeof(out));
    (void)fa_url_resolve(url1, hdr, out, sizeof(out));
    (void)fa_http_header(hdr, "Content-Length", val, sizeof(val));
    (void)fa_http_header(hdr, "Set-Cookie", val, sizeof(val));
    (void)fa_http_header(hdr, "Location", val, 8);
    (void)fa_http_unframed(hdr);
    (void)fa_playlist_first_entry(hdr, url1, out, sizeof(out));
    (void)fa_playlist_first_entry(hdr, "http://h.example/a/b.pls", out, 16);
    jar = fa_http_cookies_create();
    if (jar) {
        for (i = 0; i < 3; i++) {
            fa_http_cookies_take(jar, url1, hdr);
            fa_http_cookies_take(jar, url2, hdr);
            c = fa_http_cookies_for(jar, url1);
            free(c);
            c = fa_http_cookies_for(jar, url2);
            free(c);
            c = fa_http_cookies_for(jar, "https://sub.example.com/a/b/c?x=1");
            free(c);
        }
        fa_http_cookies_destroy(jar);
    }
    free(t);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1) return 0;
    if ((data[0] & 3) == 0) {
        text_mode(data + 1, size - 1);
        return 0;
    }
#ifndef _WIN32
    if (!g_engine) {
        fa_engine_config cfg;
        fa_engine_config_init(&cfg);
        if (fa_engine_create(&cfg, &g_engine) != FA_OK) abort();
        if (server_start() != 0) g_port = 0;   /* no loopback here (a sandbox): modes 1 to 3 do nothing */
    }
    if (!g_port) return 0;
    if ((data[0] & 3) == 3) {
        const uint8_t* nul = (const uint8_t*)memchr(data + 1, 0, size - 1);
        size_t pl = nul ? (size_t)(nul - (data + 1)) : size - 1;
        /* A live playlist (no ENDLIST) is polled for ever by design, so the target ends it. */
        char* text = (char*)malloc(pl + 32);
        if (text) {
            memcpy(text, data + 1, pl);
            memcpy(text + pl, "\n#EXT-X-ENDLIST\n", 17);
            play_url(nul ? nul + 1 : data + size, nul ? size - 1 - pl - 1 : 0, 3, (const uint8_t*)text, pl + 17);
            free(text);
        }
    } else {
        play_url(data + 1, size - 1, data[0] & 3, NULL, 0);
    }
#endif
    return 0;
}
