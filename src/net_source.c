/* The download buffer behind every URL stream.
 *
 * One download thread per stream fetches the resource into a sliding window of
 * bytes (FA_NET_WINDOW, of which at most half is read ahead). The decoder reads
 * the window through ordinary fa_io_callbacks. A seek inside the window costs
 * nothing; outside it, the thread reconnects with a Range request at the new
 * position. A connection that drops is re-established for reconnect_ms: files
 * continue where they stopped, radio picks up the live stream.
 *
 * Icecast/Shoutcast metadata is cut out of the audio here and remembered with
 * the audio byte offset where it was sent; the stream turns that into the frame
 * where it is heard (fa_net_meta_update).
 *
 * Playlists: .pls and plain .m3u files are followed to their first entry; HLS
 * playlists hand the thread over to the HLS producer (net_hls.c), which writes
 * the audio of the segments into the same window. */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_NET_WINDOW (4 * 1024 * 1024)
#define FA_NET_PLAYLIST_MAX (2 * 1024 * 1024)
#define FA_NET_RESTART_GAP (256 * 1024)   /* forward seeks closer than this just wait for the data */
#define FA_NET_PLAYLIST_HOPS 4

static char* dup_str(const char* s)
{
    char* d;
    size_t n;
    if (!s) return NULL;
    n = strlen(s) + 1;
    d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void free_config(fa_net_config* c)
{
    free(c->url); free(c->user_agent); free(c->headers); free(c->proxy);
    memset(c, 0, sizeof(*c));
}

void fa_net_request(fa_net* n, fa_http_request* req, const char* url)
{
    memset(req, 0, sizeof(*req));
    req->url = url;
    req->range_start = -1;
    req->user_agent = n->cfg.user_agent;
    req->headers = n->cfg.headers;
    req->proxy = n->cfg.proxy;
    req->timeout_ms = n->cfg.timeout_ms;
    req->cancel = &n->cancel;
}

/* ---- the window ------------------------------------------------------------ */

/* Appends audio bytes, waiting while the reader is far enough behind.
 * Caller does not hold the lock. Returns 1 to stop the transfer. */
static int window_write(fa_net* n, const uint8_t* data, size_t size)
{
    fa_mutex_lock(&n->lock);
    while (size > 0) {
        int64_t piece, at, first;
        if (n->skip > 0) {                    /* the server ignored our Range: drop the head */
            int64_t k = n->skip < (int64_t)size ? n->skip : (int64_t)size;
            n->skip -= k;
            data += k;
            size -= (size_t)k;
            continue;
        }
        while (!n->quit && !n->restart && !n->hls_seek && n->win_end - n->read_pos >= n->ahead_limit)
            fa_cond_wait_ms(&n->cond, &n->lock, 100);
        if (n->quit || n->restart || n->hls_seek) { fa_mutex_unlock(&n->lock); return 1; }
        /* Never more than the window can take without overwriting unread bytes. */
        piece = n->cap - n->ahead_limit;
        if (piece > (int64_t)size) piece = (int64_t)size;
        if (piece > 65536) piece = 65536;
        if (n->win_end + piece - n->win_start > n->cap) n->win_start = n->win_end + piece - n->cap;
        at = n->win_end % n->cap;
        first = n->cap - at < piece ? n->cap - at : piece;
        memcpy(n->buf + at, data, (size_t)first);
        memcpy(n->buf, data + first, (size_t)(piece - first));
        n->win_end += piece;
        n->downloaded += piece;
        data += piece;
        size -= (size_t)piece;
        fa_cond_broadcast(&n->cond);
    }
    fa_mutex_unlock(&n->lock);
    return 0;
}

int fa_net_write(fa_net* n, const uint8_t* data, size_t size) { return window_write(n, data, size); }

int fa_net_wait(fa_net* n, uint32_t ms)
{
    int stop;
    fa_mutex_lock(&n->lock);
    if (!n->quit && !n->hls_seek) fa_cond_wait_ms(&n->cond, &n->lock, ms);
    stop = n->quit || n->hls_seek;
    fa_mutex_unlock(&n->lock);
    return stop;
}

/* ---- ICY metadata ---------------------------------------------------------- */

static int valid_utf8(const char* s)
{
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return 0;
        p++;
        while (k-- > 0) { if ((*p & 0xC0) != 0x80) return 0; p++; }
    }
    return 1;
}

/* Stations send UTF-8 or Latin-1; everything leaves FatzAudio as UTF-8. */
static char* to_utf8(const char* s, size_t len)
{
    char* out = (char*)malloc(len * 2 + 1);
    size_t i, o = 0;
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = 0;
    if (valid_utf8(out)) return out;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
    return out;
}

/* Value of key='...'; in an ICY metadata block. The value ends at "';" (titles
 * may contain apostrophes) or at the end of the block. */
static char* icy_value(const char* block, const char* key)
{
    const char* p = strstr(block, key);
    const char* end;
    size_t klen = strlen(key);
    if (!p || p[klen] != '=' || p[klen + 1] != '\'') return NULL;
    p += klen + 2;
    end = strstr(p, "';");
    if (!end) {
        end = p + strlen(p);
        while (end > p && (end[-1] == 0 || end[-1] == '\'' || end[-1] == ';')) end--;
    }
    return to_utf8(p, (size_t)(end - p));
}

static void meta_free_entry(fa_net_meta_entry* e)
{
    free(e->title);
    free(e->url);
    memset(e, 0, sizeof(*e));
}

/* Caller holds the lock. A block that repeats the current title is not a change. */
static void meta_add(fa_net* n, const char* block)
{
    char* title = icy_value(block, "StreamTitle");
    char* url = icy_value(block, "StreamUrl");
    fa_net_meta_entry* last = n->meta_count > 0 ? &n->meta[n->meta_count - 1] : NULL;
    if (!title && !url) return;
    if (last && strcmp(last->title ? last->title : "", title ? title : "") == 0 &&
        strcmp(last->url ? last->url : "", url ? url : "") == 0) {
        free(title); free(url);
        return;
    }
    if (n->meta_count == FA_NET_META_MAX) {
        /* Drop the oldest one; it is long heard (the buffer holds far less than 16 songs). */
        meta_free_entry(&n->meta[0]);
        memmove(&n->meta[0], &n->meta[1], sizeof(n->meta[0]) * (FA_NET_META_MAX - 1));
        memset(&n->meta[FA_NET_META_MAX - 1], 0, sizeof(n->meta[0]));
        n->meta_count--;
    }
    last = &n->meta[n->meta_count++];
    last->byte = n->win_end;
    last->frame = -1;
    last->seq = ++n->meta_seq;
    last->title = title;
    last->url = url;
}

/* Splits ICY metadata out of the body; the audio goes to the window. */
static int icy_feed(fa_net* n, const uint8_t* p, size_t size)
{
    while (size > 0) {
        if (n->icy_left > 0) {
            size_t k = size < n->icy_left ? size : n->icy_left;
            if (window_write(n, p, k)) return 1;
            n->icy_left -= (uint32_t)k;
            p += k;
            size -= k;
        } else if (n->meta_len < 0) {
            n->meta_len = *p * 16;
            n->meta_got = 0;
            p++;
            size--;
            if (n->meta_len == 0) { n->meta_len = -1; n->icy_left = n->metaint; }
        } else {
            size_t k = (size_t)(n->meta_len - n->meta_got) < size ? (size_t)(n->meta_len - n->meta_got) : size;
            memcpy(n->meta_buf + n->meta_got, p, k);
            n->meta_got += (int)k;
            p += k;
            size -= k;
            if (n->meta_got == n->meta_len) {
                n->meta_buf[n->meta_got] = 0;
                fa_mutex_lock(&n->lock);
                meta_add(n, n->meta_buf);
                fa_mutex_unlock(&n->lock);
                n->meta_len = -1;
                n->icy_left = n->metaint;
            }
        }
    }
    return 0;
}

/* ---- the download thread ----------------------------------------------------- */

typedef struct fetch_ctx {
    fa_net* n;
    int64_t pos;             /* where this connection should start */
    int collect;             /* collecting a playlist instead of audio */
    int got_data;
    int overflow;            /* the playlist was larger than FA_NET_PLAYLIST_MAX */
} fetch_ctx;

static int has_suffix_ci(const char* url, const char* suffix)
{
    size_t ul = strcspn(url, "?#"), sl = strlen(suffix);
    return ul >= sl && fa_strncasecmp_ascii(url + ul - sl, suffix, sl) == 0;
}

static int playlist_type(const char* ct)
{
    static const char* types[] = { "application/vnd.apple.mpegurl", "application/x-mpegurl", "audio/mpegurl",
                                   "audio/x-mpegurl", "audio/x-scpls", "audio/scpls", "application/pls+xml" };
    size_t i;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (fa_strncasecmp_ascii(ct, types[i], strlen(types[i])) == 0) return 1;
    return 0;
}

static int on_head(void* user, const fa_http_response* resp)
{
    fetch_ctx* c = (fetch_ctx*)user;
    fa_net* n = c->n;
    char v[256];
    int first;
    fa_mutex_lock(&n->lock);
    first = n->connections == 0;
    n->connections++;
    n->http_status = resp->status;
    free(n->headers);
    n->headers = dup_str(resp->headers);
    snprintf(n->final_url, sizeof(n->final_url), "%s", resp->final_url);
    if (resp->status >= 400) {
        fa_mutex_unlock(&n->lock);
        return 0;    /* fa_http_fetch reports the failure */
    }
    n->connected = 1;
    if (n->failed_since && !first) n->reconnects++;
    n->failed_since = 0;
    if (first) {
        n->content_type[0] = 0;
        if (fa_http_header(resp->headers, "Content-Type", v, sizeof(v))) snprintf(n->content_type, sizeof(n->content_type), "%.127s", v);
        n->icy = resp->status == 200 && (fa_http_header(resp->headers, "icy-metaint", v, sizeof(v)) ||
                                         fa_http_header(resp->headers, "icy-name", v, sizeof(v)) ||
                                         fa_http_header(resp->headers, "icy-br", v, sizeof(v)));
        if (fa_http_header(resp->headers, "icy-br", v, sizeof(v))) n->icy_br = (uint32_t)atoi(v);
        n->length = resp->status == 206 ? resp->total_length : resp->content_length;
        n->ranges = resp->status == 206 && resp->total_length >= 0 && resp->range_start == 0;
        if (!n->ranges && fa_http_header(resp->headers, "Accept-Ranges", v, sizeof(v)) && strstr(v, "bytes") && n->length >= 0)
            n->ranges = 1;
        n->live = n->icy || n->length < 0;
        if (n->icy) n->ranges = 0;
        if (playlist_type(n->content_type) || has_suffix_ci(resp->final_url, ".m3u8") ||
            has_suffix_ci(resp->final_url, ".m3u") || has_suffix_ci(resp->final_url, ".pls"))
            c->collect = 1;
    } else if (!n->live && c->pos > 0) {
        /* Resuming a file: the body must start at c->pos. */
        if (resp->status == 206 && resp->range_start <= c->pos) n->skip = c->pos - resp->range_start;
        else if (resp->status == 200) n->skip = c->pos;
        else { fa_mutex_unlock(&n->lock); return 1; }
    }
    n->metaint = 0;
    if (fa_http_header(resp->headers, "icy-metaint", v, sizeof(v)) && !(n->cfg.flags & FA_NET_NO_ICY)) {
        long mi = atol(v);
        if (mi > 0 && mi <= 1 << 20) n->metaint = (uint32_t)mi;
    }
    n->icy_left = n->metaint;
    n->meta_len = -1;
    if (first) n->head_ready = 1;
    fa_cond_broadcast(&n->cond);
    fa_mutex_unlock(&n->lock);
    return 0;
}

static int on_data(void* user, const uint8_t* data, size_t size)
{
    fetch_ctx* c = (fetch_ctx*)user;
    fa_net* n = c->n;
    if (!c->got_data && !c->collect && n->connections == 1 && n->win_end == 0) {
        /* Playlists served as text/plain or octet-stream. */
        if ((size >= 7 && memcmp(data, "#EXTM3U", 7) == 0) || (size >= 10 && fa_strncasecmp_ascii((const char*)data, "[playlist]", 10) == 0))
            c->collect = 1;
    }
    c->got_data = 1;
    if (c->collect) {
        char* p;
        if (n->playlist_len + size > FA_NET_PLAYLIST_MAX) { c->overflow = 1; return 1; }
        p = (char*)realloc(n->playlist, n->playlist_len + size + 1);
        if (!p) return 1;
        n->playlist = p;
        memcpy(p + n->playlist_len, data, size);
        n->playlist_len += size;
        p[n->playlist_len] = 0;
        return 0;
    }
    if (n->metaint) return icy_feed(n, data, size);
    return window_write(n, data, size);
}

/* First entry of a .pls ("File1=...") or plain .m3u playlist, resolved. */
static int playlist_first_entry(const char* text, const char* base, char* out, size_t cap)
{
    const char* line = text;
    int pls = fa_strncasecmp_ascii(text, "[playlist]", 10) == 0;
    while (*line) {
        size_t len = strcspn(line, "\r\n");
        char entry[FA_NET_URL_MAX];
        const char* v = NULL;
        size_t vlen = 0;
        if (pls) {
            if (len > 5 && fa_strncasecmp_ascii(line, "File", 4) == 0) {
                const char* eq = memchr(line, '=', len);
                if (eq) { v = eq + 1; vlen = len - (size_t)(eq + 1 - line); }
            }
        } else if (len > 0 && line[0] != '#') {
            v = line;
            vlen = len;
        }
        while (v && vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
        while (v && vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
        if (v && vlen > 0 && vlen < sizeof(entry)) {
            memcpy(entry, v, vlen);
            entry[vlen] = 0;
            if (fa_url_resolve(base, entry, out, cap) == FA_OK) return 1;
        }
        line += len;
        while (*line == '\r' || *line == '\n') line++;
    }
    return 0;
}

static int is_hls_text(const char* text)
{
    return strncmp(text, "#EXTM3U", 7) == 0 && (strstr(text, "#EXT-X-") || strstr(text, "#EXTINF"));
}

/* Waits `ms`, returning early on quit or restart. Caller holds the lock. */
static void backoff_locked(fa_net* n, uint32_t ms)
{
    int64_t until = fa_monotonic_ns() + (int64_t)ms * 1000000;
    while (!n->quit && !n->restart) {
        int64_t left = (until - fa_monotonic_ns()) / 1000000;
        if (left <= 0) break;
        fa_cond_wait_ms(&n->cond, &n->lock, (uint32_t)(left > 100 ? 100 : left));
    }
}

static void download_main(void* arg)
{
    fa_net* n = (fa_net*)arg;
    char* url = dup_str(n->cfg.url);
    int hops = 0;
    uint32_t backoff = 250;
    int64_t fail_start = 0;
    fa_mutex_lock(&n->lock);
    while (!n->quit && url) {
        fa_http_request req;
        fetch_ctx c;
        fa_result r;
        int64_t pos;
        if (n->finished && !n->restart) {   /* complete: idle until a seek needs more */
            fa_cond_wait_ms(&n->cond, &n->lock, 200);
            continue;
        }
        if (n->restart) {
            n->restart = 0;
            n->finished = 0;
            n->win_start = n->win_end = n->restart_pos;
            n->skip = 0;
            fail_start = 0;
            backoff = 250;
        }
        pos = n->win_end;
        memset(&c, 0, sizeof(c));
        c.n = n;
        c.pos = pos;
        fa_net_request(n, &req, url);
        req.icy = !(n->cfg.flags & FA_NET_NO_ICY);
        /* The first request asks for "bytes=0-": a 206 answer proves ranges work. */
        if (n->connections == 0) req.range_start = 0;
        else if (!n->live && pos > 0) req.range_start = pos;
        fa_store32(&n->cancel, 0);
        fa_mutex_unlock(&n->lock);
        r = fa_http_fetch(&req, on_head, on_data, &c);
        fa_mutex_lock(&n->lock);
        n->connected = 0;
        if (n->quit) break;
        if (n->restart) continue;               /* a seek: reconnect at once */

        if (c.collect && n->head_ready) {
            if (r != FA_OK || !n->playlist || c.overflow) { n->error = r != FA_OK ? r : FA_ERROR_UNSUPPORTED_FORMAT; break; }
            if (is_hls_text(n->playlist)) {
                char base[FA_NET_URL_MAX];
                n->hls = 1;
                snprintf(base, sizeof(base), "%s", n->final_url);
                fa_mutex_unlock(&n->lock);
                r = fa_hls_run(n, n->playlist, n->playlist_len, base);
                fa_mutex_lock(&n->lock);
                if (r != FA_OK && n->error == FA_OK) n->error = r;
                break;
            } else {
                char next[FA_NET_URL_MAX];
                if (++hops > FA_NET_PLAYLIST_HOPS || !playlist_first_entry(n->playlist, n->final_url, next, sizeof(next))) {
                    n->error = FA_ERROR_UNSUPPORTED_FORMAT;
                    break;
                }
                /* Follow the playlist: a fresh start on its first entry. */
                free(url);
                url = dup_str(next);
                free(n->playlist);
                n->playlist = NULL;
                n->playlist_len = 0;
                n->connections = 0;
                n->head_ready = 0;
                continue;
            }
        }

        if (r == FA_OK && !n->icy && (n->length < 0 || n->win_end + n->skip >= n->length)) {
            n->finished = 1;                    /* the whole file is here */
            fail_start = 0;
            backoff = 250;
            fa_cond_broadcast(&n->cond);
            continue;
        }
        /* The connection failed or dropped (radio servers never end cleanly). */
        if (r != FA_OK) n->error = r;
        else n->error = FA_ERROR_NETWORK;
        n->failed_since = 1;
        if (!n->head_ready || r == FA_ERROR_PROVIDER_MISSING || r == FA_ERROR_INVALID_ARG) break;  /* opening failed */
        if (fail_start == 0) fail_start = fa_monotonic_ns();
        if ((r == FA_ERROR_NOT_FOUND && !n->live) || (n->cfg.flags & FA_NET_NO_RECONNECT) ||
            fa_monotonic_ns() - fail_start > (int64_t)n->cfg.reconnect_ms * 1000000) {
            /* Give up: the stream plays what it has and ends. The thread stays, so a
             * seek back into the file can still try a new connection. */
            n->finished = 1;
            fa_cond_broadcast(&n->cond);
            continue;
        }
        backoff_locked(n, backoff);
        backoff = backoff * 2 > 4000 ? 4000 : backoff * 2;
    }
    n->finished = 1;
    n->thread_done = 1;
    fa_cond_broadcast(&n->cond);
    fa_mutex_unlock(&n->lock);
    free(url);
}

/* ---- opening and closing ------------------------------------------------------ */

fa_result fa_net_open(const fa_net_config* cfg, fa_net** out)
{
    fa_net* n;
    fa_result r = FA_OK;
    *out = NULL;
    n = (fa_net*)calloc(1, sizeof(*n));
    if (!n) { fa_net_config c = *cfg; free_config(&c); return FA_ERROR_OUT_OF_MEMORY; }
    n->cfg = *cfg;
    n->cap = FA_NET_WINDOW;
    n->ahead_limit = FA_NET_WINDOW / 2;
    n->length = -1;
    n->meta_len = -1;
    n->starving = 1;
    n->hls_duration = -1;
    n->buf = (uint8_t*)malloc((size_t)n->cap);
    if (!n->buf || fa_mutex_init(&n->lock) != 0) { free(n->buf); free_config(&n->cfg); free(n); return FA_ERROR_OUT_OF_MEMORY; }
    if (fa_cond_init(&n->cond) != 0) { fa_mutex_destroy(&n->lock); free(n->buf); free_config(&n->cfg); free(n); return FA_ERROR_OUT_OF_MEMORY; }
    if (fa_thread_create(&n->thread, download_main, n) != 0) { fa_net_close(n); return FA_ERROR_GENERIC; }
    n->thread_started = 1;

    /* Wait for the headers and enough bytes to detect the format. */
    fa_mutex_lock(&n->lock);
    {
        int64_t deadline = fa_monotonic_ns() + (int64_t)n->cfg.timeout_ms * 1000000 * 2;
        while (!n->finished && !(n->head_ready && n->win_end >= FA_PROBE_BYTES) && fa_monotonic_ns() < deadline)
            fa_cond_wait_ms(&n->cond, &n->lock, 50);
        if (n->win_end == 0) {
            r = n->error != FA_OK ? n->error : (n->finished ? FA_ERROR_UNSUPPORTED_FORMAT : FA_ERROR_NETWORK);
            if (n->finished && n->head_ready && n->http_status < 400 && n->error == FA_OK) r = FA_ERROR_UNSUPPORTED_FORMAT;
        }
    }
    fa_mutex_unlock(&n->lock);
    if (r != FA_OK) { fa_net_close(n); return r; }
    *out = n;
    return FA_OK;
}

void fa_net_interrupt(fa_net* n)
{
    if (!n) return;
    fa_mutex_lock(&n->lock);
    n->quit = 1;
    fa_store32(&n->cancel, 1);
    fa_cond_broadcast(&n->cond);
    fa_mutex_unlock(&n->lock);
    fa_http_abort(&n->cancel);
}

void fa_net_close(fa_net* n)
{
    int i;
    if (!n) return;
    if (n->thread_started) {
        fa_mutex_lock(&n->lock);
        n->quit = 1;
        fa_store32(&n->cancel, 1);
        fa_cond_broadcast(&n->cond);
        fa_mutex_unlock(&n->lock);
        fa_http_abort(&n->cancel);
        fa_thread_join(&n->thread);
    }
    for (i = 0; i < n->meta_count; i++) meta_free_entry(&n->meta[i]);
    fa_cond_destroy(&n->cond);
    fa_mutex_destroy(&n->lock);
    free(n->buf);
    free(n->headers);
    free(n->playlist);
    free_config(&n->cfg);
    free(n);
}

/* ---- reading (decoder side) ----------------------------------------------------- */

static int64_t FA_CALL io_read(void* user, void* buffer, size_t bytes)
{
    fa_net* n = (fa_net*)user;
    int64_t got = 0;
    fa_mutex_lock(&n->lock);
    for (;;) {
        if (n->read_pos >= n->win_start && n->read_pos < n->win_end && !n->restart) {
            int64_t avail = n->win_end - n->read_pos, at, first;
            got = avail < (int64_t)bytes ? avail : (int64_t)bytes;
            at = n->read_pos % n->cap;
            first = n->cap - at < got ? n->cap - at : got;
            memcpy(buffer, n->buf + at, (size_t)first);
            memcpy((uint8_t*)buffer + first, n->buf, (size_t)(got - first));
            n->read_pos += got;
            n->consumed += got;
            fa_cond_broadcast(&n->cond);
            break;
        }
        if (n->quit || (n->finished && !n->restart && n->read_pos >= n->win_start)) break;   /* the end */
        if (!n->restart && n->read_pos < n->win_start && !n->hls) {
            n->restart = 1;                       /* fell behind the window */
            n->restart_pos = n->read_pos;
            fa_store32(&n->cancel, 1);
            fa_cond_broadcast(&n->cond);
        }
        fa_cond_wait_ms(&n->cond, &n->lock, 100);
    }
    fa_mutex_unlock(&n->lock);
    return got;
}

static fa_result FA_CALL io_seek(void* user, int64_t offset)
{
    fa_net* n = (fa_net*)user;
    int abort = 0;
    if (offset < 0 || (n->length >= 0 && offset > n->length)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&n->lock);
    if (n->thread_done && (offset < n->win_start || offset > n->win_end)) {
        fa_mutex_unlock(&n->lock);
        return FA_ERROR_IO;                       /* nobody left to download it */
    }
    n->read_pos = offset;
    if (offset < n->win_start || offset > n->win_end + FA_NET_RESTART_GAP ||
        (offset > n->win_end && n->finished && (n->length < 0 || offset < n->length))) {
        n->restart = 1;
        n->restart_pos = offset;
        abort = 1;
        n->starving = 1;                      /* prebuffer again before playing on */
        fa_store32(&n->cancel, 1);
    }
    fa_cond_broadcast(&n->cond);
    fa_mutex_unlock(&n->lock);
    if (abort) fa_http_abort(&n->cancel);
    return FA_OK;
}

static int64_t FA_CALL io_length(void* user)
{
    fa_net* n = (fa_net*)user;
    return n->length;
}

void fa_net_io(fa_net* n, fa_io_callbacks* out)
{
    memset(out, 0, sizeof(*out));
    out->read = io_read;
    out->seek = (n->ranges && n->length >= 0 && !n->live && !n->hls) ? io_seek : NULL;
    out->length = n->hls ? NULL : io_length;
}

int fa_net_is_hls(const fa_net* n) { return n->hls; }

/* ---- decode gating, rate, status ------------------------------------------------- */

/* Bytes per second of the audio, 0 = unknown. Caller holds the lock. */
static double byte_rate_locked(fa_net* n)
{
    if (n->sample_rate && n->frames > (int64_t)n->sample_rate * 2 && n->consumed > 0)
        return (double)n->consumed / (double)n->frames * n->sample_rate;
    if (n->icy_br) return n->icy_br * 125.0;
    return 0.0;
}

int fa_net_can_decode(fa_net* n, int* out_starving)
{
    int ok;
    int64_t avail, gate;
    double rate;
    fa_mutex_lock(&n->lock);
    avail = (n->read_pos >= n->win_start && n->read_pos <= n->win_end) ? n->win_end - n->read_pos : 0;
    rate = byte_rate_locked(n);
    /* Enough for any decoder read of one chunk (a few compressed frames). */
    gate = rate > 0 ? (int64_t)(rate * 0.3) : 16384;
    if (gate < 6144) gate = 6144;
    if (gate > 32768) gate = 32768;
    if (n->restart || n->hls_seek) ok = 0;
    else if (n->finished) ok = 1;                  /* reads return what is left, then the end */
    else if (n->starving) {
        int64_t need = rate > 0 ? (int64_t)(rate * n->cfg.prebuffer_ms / 1000.0) : 65536;
        if (need < gate) need = gate;
        if (need > n->ahead_limit * 3 / 4) need = n->ahead_limit * 3 / 4;
        ok = avail >= need;
    } else {
        ok = avail >= gate;
    }
    n->starving = !ok && !n->finished;
    if (out_starving) *out_starving = n->starving;
    fa_mutex_unlock(&n->lock);
    return ok;
}

void fa_net_note_decoded(fa_net* n, int64_t frames)
{
    fa_mutex_lock(&n->lock);
    n->frames += frames;
    fa_mutex_unlock(&n->lock);
}

int fa_net_finished(fa_net* n)
{
    int f;
    fa_mutex_lock(&n->lock);
    f = n->finished;
    fa_mutex_unlock(&n->lock);
    return f;
}

double fa_net_bytes_per_frame(fa_net* n)
{
    double r, bpf = 0.0;
    fa_mutex_lock(&n->lock);
    r = byte_rate_locked(n);
    if (r > 0 && n->sample_rate) bpf = r / n->sample_rate;
    fa_mutex_unlock(&n->lock);
    return bpf;
}

void fa_net_get_status(fa_net* n, fa_net_status* out)
{
    fa_mutex_lock(&n->lock);
    out->connected = n->connected;
    out->http_status = n->http_status;
    out->is_live = n->live || (n->hls && n->hls_duration < 0);
    out->is_hls = n->hls;
    out->seekable = n->hls ? n->hls_duration >= 0 : (n->ranges && n->length >= 0 && !n->live);
    out->finished = n->finished;
    out->content_length = n->hls ? -1 : n->length;
    out->downloaded = n->downloaded;
    out->buffered_bytes = (n->read_pos >= n->win_start && n->read_pos <= n->win_end) ? n->win_end - n->read_pos : 0;
    out->reconnects = n->reconnects;
    out->last_error = n->error;
    snprintf(out->url, sizeof(out->url), "%s", n->final_url[0] ? n->final_url : n->cfg.url);
    fa_mutex_unlock(&n->lock);
}

fa_result fa_net_get_header(fa_net* n, const char* name, char* out, size_t cap)
{
    fa_result r;
    fa_mutex_lock(&n->lock);
    r = fa_http_header(n->headers, name, out, cap) ? FA_OK : FA_ERROR_NOT_FOUND;
    fa_mutex_unlock(&n->lock);
    return r;
}

fa_result fa_net_get_station(fa_net* n, fa_metadata* out)
{
    char v[512];
    fa_mutex_lock(&n->lock);
#define HDR(field, name) do { if (fa_http_header(n->headers, name, v, sizeof(v))) { char* u_ = to_utf8(v, strlen(v)); \
        if (u_) { snprintf(out->field, sizeof(out->field), "%s", u_); free(u_); } } } while (0)
    HDR(station, "icy-name");
    HDR(genre, "icy-genre");
    HDR(description, "icy-description");
    HDR(station_url, "icy-url");
#undef HDR
    snprintf(out->content_type, sizeof(out->content_type), "%s", n->content_type);
    out->bitrate_kbps = n->icy_br;
    fa_mutex_unlock(&n->lock);
    return FA_OK;
}

/* ---- metadata positions ------------------------------------------------------------ */

int64_t fa_net_meta_update(fa_net* n, int64_t frame_at_cursor, int64_t lag_bytes, int64_t heard, int64_t* out_seq)
{
    int i;
    int64_t next = INT64_MAX, cursor, seq = 0;
    double bpf;
    fa_mutex_lock(&n->lock);
    cursor = n->read_pos - lag_bytes;         /* the decoder has turned bytes up to here into frames */
    bpf = 0.0;
    {
        double r = byte_rate_locked(n);
        if (r > 0 && n->sample_rate) bpf = r / n->sample_rate;
    }
    for (i = 0; i < n->meta_count; i++) {
        fa_net_meta_entry* e = &n->meta[i];
        if (e->frame < 0) {
            if (e->byte <= cursor) {
                /* Already decoded: it started this many bytes before the cursor. */
                int64_t back = bpf > 0 ? (int64_t)((double)(cursor - e->byte) / bpf) : 0;
                e->frame = frame_at_cursor - back;
                if (e->frame < 0) e->frame = 0;
                /* The first title of a station is what is on now. */
                if (e->seq == 1 || (i > 0 && e->frame < n->meta[i - 1].frame)) e->frame = i > 0 ? n->meta[i - 1].frame : 0;
            } else {
                continue;                     /* not reached by the decoder yet */
            }
        }
        if (e->frame > heard && e->frame < next) { next = e->frame; seq = e->seq; }
    }
    fa_mutex_unlock(&n->lock);
    if (out_seq) *out_seq = seq;
    return next;
}

/* Caller holds the lock: the entry in effect at `heard`, or NULL. */
static fa_net_meta_entry* meta_at_locked(fa_net* n, int64_t heard)
{
    int i;
    fa_net_meta_entry* best = NULL;
    for (i = 0; i < n->meta_count; i++)
        if (n->meta[i].frame >= 0 && n->meta[i].frame <= heard) best = &n->meta[i];
    return best;
}

void fa_net_meta_at(fa_net* n, int64_t heard, fa_net_meta* out)
{
    fa_net_meta_entry* e;
    memset(out, 0, sizeof(*out));
    fa_mutex_lock(&n->lock);
    e = meta_at_locked(n, heard);
    if (e) {
        out->frame = e->frame;
        out->seq = e->seq;
        snprintf(out->title, sizeof(out->title), "%s", e->title ? e->title : "");
        snprintf(out->url, sizeof(out->url), "%s", e->url ? e->url : "");
    }
    fa_mutex_unlock(&n->lock);
}

int64_t fa_net_meta_seq_at(fa_net* n, int64_t heard)
{
    fa_net_meta_entry* e;
    int64_t seq;
    fa_mutex_lock(&n->lock);
    e = meta_at_locked(n, heard);
    seq = e ? e->seq : 0;
    fa_mutex_unlock(&n->lock);
    return seq;
}

void fa_net_meta_reset_frames(fa_net* n, int64_t frames)
{
    int i;
    fa_mutex_lock(&n->lock);
    for (i = 0; i < n->meta_count; i++)
        if (n->meta[i].frame >= 0) n->meta[i].frame = frames;   /* what was heard stays the current title */
    fa_mutex_unlock(&n->lock);
}

void fa_net_set_sample_rate(fa_net* n, uint32_t rate)
{
    fa_mutex_lock(&n->lock);
    n->sample_rate = rate;
    fa_mutex_unlock(&n->lock);
}

/* ---- small downloads (playlists, keys) ------------------------------------------- */

typedef struct small_ctx {
    char* data;
    size_t len;
    int status;
    char* final_url;
    size_t final_cap;
    int cut;                 /* stopped early: too large, or out of memory */
} small_ctx;

static int small_head(void* user, const fa_http_response* resp)
{
    small_ctx* c = (small_ctx*)user;
    c->status = resp->status;
    if (c->final_url) snprintf(c->final_url, c->final_cap, "%s", resp->final_url);
    return 0;
}

static int small_data(void* user, const uint8_t* data, size_t size)
{
    small_ctx* c = (small_ctx*)user;
    char* p;
    if (c->len + size > FA_NET_PLAYLIST_MAX) { c->cut = 1; return 1; }
    p = (char*)realloc(c->data, c->len + size + 1);
    if (!p) { c->cut = 1; return 1; }
    c->data = p;
    memcpy(p + c->len, data, size);
    c->len += size;
    p[c->len] = 0;
    return 0;
}

fa_result fa_net_get_small(fa_net* n, const char* url, int64_t offset, int64_t length, char** out, size_t* out_len,
                           char* final_url, size_t final_cap)
{
    fa_http_request req;
    small_ctx c;
    fa_result r;
    memset(&c, 0, sizeof(c));
    c.final_url = final_url;
    c.final_cap = final_cap;
    fa_net_request(n, &req, url);
    if (offset > 0) req.range_start = offset;
    r = fa_http_fetch(&req, small_head, small_data, &c);
    if (r == FA_OK && c.cut) r = FA_ERROR_UNSUPPORTED_FORMAT;   /* never hand on a truncated playlist or key */
    if (r == FA_OK && !c.data) {
        c.data = (char*)calloc(1, 1);
        if (!c.data) r = FA_ERROR_OUT_OF_MEMORY;
    }
    if (r == FA_OK && length >= 0 && (int64_t)c.len > length) { c.len = (size_t)length; c.data[c.len] = 0; }
    if (r != FA_OK) { free(c.data); c.data = NULL; c.len = 0; }
    *out = c.data;
    *out_len = c.len;
    return r;
}
