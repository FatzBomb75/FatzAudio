/* URL parsing and resolution (RFC 3986), header lookup and base64. */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int fa_strncasecmp_ascii(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = lower(a[i]), y = lower(b[i]);
        if (x != y) return (unsigned char)x - (unsigned char)y;
        if (!x) return 0;
    }
    return 0;
}

static int copy_n(char* out, size_t cap, const char* s, size_t n)
{
    if (n >= cap) return -1;
    memcpy(out, s, n);
    out[n] = 0;
    return 0;
}

fa_result fa_url_parse(const char* text, fa_url* out)
{
    const char *p, *auth, *auth_end, *at, *host, *host_end, *colon = NULL, *path, *frag;
    size_t slen;
    memset(out, 0, sizeof(*out));
    if (!text) return FA_ERROR_INVALID_ARG;
    while (*text == ' ' || *text == '\t') text++;
    p = strstr(text, "://");
    if (!p) return FA_ERROR_INVALID_ARG;
    slen = (size_t)(p - text);
    if (slen == 4 && fa_strncasecmp_ascii(text, "http", 4) == 0) strcpy(out->scheme, "http");
    else if (slen == 5 && fa_strncasecmp_ascii(text, "https", 5) == 0) strcpy(out->scheme, "https");
    else return FA_ERROR_INVALID_ARG;

    auth = p + 3;
    auth_end = auth + strcspn(auth, "/?#");
    at = NULL;
    for (host = auth; host < auth_end; host++) if (*host == '@') at = host;
    host = auth;
    if (at) {
        if (copy_n(out->userinfo, sizeof(out->userinfo), auth, (size_t)(at - auth)) != 0) return FA_ERROR_INVALID_ARG;
        host = at + 1;
    }
    if (host < auth_end && *host == '[') {         /* IPv6 literal */
        const char* close = memchr(host, ']', (size_t)(auth_end - host));
        if (!close) return FA_ERROR_INVALID_ARG;
        if (copy_n(out->host, sizeof(out->host), host + 1, (size_t)(close - host - 1)) != 0) return FA_ERROR_INVALID_ARG;
        host_end = close + 1;
        if (host_end < auth_end && *host_end == ':') colon = host_end;
        else if (host_end != auth_end) return FA_ERROR_INVALID_ARG;
    } else {
        const char* c;
        for (c = host; c < auth_end; c++) if (*c == ':') colon = c;
        host_end = colon ? colon : auth_end;
        if (copy_n(out->host, sizeof(out->host), host, (size_t)(host_end - host)) != 0) return FA_ERROR_INVALID_ARG;
    }
    if (!out->host[0]) return FA_ERROR_INVALID_ARG;
    out->default_port = 1;
    out->port = strcmp(out->scheme, "https") == 0 ? 443 : 80;
    if (colon && colon + 1 < auth_end) {
        long port = 0;
        const char* c;
        for (c = colon + 1; c < auth_end; c++) {
            if (*c < '0' || *c > '9') return FA_ERROR_INVALID_ARG;
            port = port * 10 + (*c - '0');
            if (port > 65535) return FA_ERROR_INVALID_ARG;
        }
        if (port == 0) return FA_ERROR_INVALID_ARG;
        out->port = (uint16_t)port;
        out->default_port = 0;
    }
    path = auth_end;
    frag = path + strcspn(path, "#");
    if (*path != '/') {
        out->path[0] = '/';
        if (copy_n(out->path + 1, sizeof(out->path) - 1, path, (size_t)(frag - path)) != 0) return FA_ERROR_INVALID_ARG;
    } else if (copy_n(out->path, sizeof(out->path), path, (size_t)(frag - path)) != 0) {
        return FA_ERROR_INVALID_ARG;
    }
    /* Spaces are common in hand-written playlist URLs; no request line may contain them. */
    if (strpbrk(out->path, " \t\r\n") || strpbrk(out->host, " \t\r\n/")) return FA_ERROR_INVALID_ARG;
    return FA_OK;
}

/* RFC 3986 section 5.2.4: removes "." and ".." segments from a path, in place. */
static void remove_dot_segments(char* path)
{
    char* in = path;    /* the input buffer is the rest of `path`; output grows behind it */
    char* w = path;
    while (*in) {
        if (strncmp(in, "../", 3) == 0) in += 3;
        else if (strncmp(in, "./", 2) == 0) in += 2;
        else if (strncmp(in, "/./", 3) == 0) in += 2;
        else if (strcmp(in, "/.") == 0) { in += 1; in[0] = '/'; }   /* "/." becomes "/" */
        else if (strncmp(in, "/../", 4) == 0) {
            in += 3;
            while (w > path && *(w - 1) != '/') w--;
            if (w > path) w--;
        } else if (strcmp(in, "/..") == 0) {
            in += 2;
            in[0] = '/';
            while (w > path && *(w - 1) != '/') w--;
            if (w > path) w--;
        } else if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0) {
            break;
        } else {
            do { *w++ = *in++; } while (*in && *in != '/');
        }
    }
    *w = 0;
}

fa_result fa_url_resolve(const char* base, const char* ref, char* out, size_t cap)
{
    fa_url b;
    char* tmp;
    char* query;
    char port[8] = "";
    size_t need;
    int n, v6;
    if (!base || !ref || !out || cap == 0) return FA_ERROR_INVALID_ARG;
    while (*ref == ' ' || *ref == '\t') ref++;
    if (strstr(ref, "://") && strcspn(ref, ":") < strcspn(ref, "/?#")) {   /* absolute */
        if (strlen(ref) >= cap) return FA_ERROR_INVALID_ARG;
        strcpy(out, ref);
        return FA_OK;
    }
    if (fa_url_parse(base, &b) != FA_OK) return FA_ERROR_INVALID_ARG;
    if (ref[0] == '/' && ref[1] == '/') {          /* network-path reference */
        n = snprintf(out, cap, "%s:%s", b.scheme, ref);
        return (n < 0 || (size_t)n >= cap) ? FA_ERROR_INVALID_ARG : FA_OK;
    }
    need = strlen(b.path) + strlen(ref) + 2;
    tmp = (char*)malloc(need);
    if (!tmp) return FA_ERROR_OUT_OF_MEMORY;
    if (ref[0] == '/') {
        strcpy(tmp, ref);
    } else if (ref[0] == '?') {
        size_t plen = strcspn(b.path, "?");
        memcpy(tmp, b.path, plen);
        strcpy(tmp + plen, ref);
    } else if (ref[0] == 0 || ref[0] == '#') {
        strcpy(tmp, b.path);
    } else {
        /* Merge: the base path up to and including its last '/', then the reference. */
        size_t plen = strcspn(b.path, "?"), i, keep = 1;
        for (i = 0; i < plen; i++) if (b.path[i] == '/') keep = i + 1;
        memcpy(tmp, b.path, keep);
        strcpy(tmp + keep, ref);
    }
    tmp[strcspn(tmp, "#")] = 0;
    query = strchr(tmp, '?');
    if (query) *query++ = 0;
    remove_dot_segments(tmp);
    if (!b.default_port) snprintf(port, sizeof(port), ":%u", (unsigned)b.port);
    v6 = strchr(b.host, ':') != NULL;
    n = snprintf(out, cap, "%s://%s%s%s%s%s%s%s%s%s", b.scheme, b.userinfo, b.userinfo[0] ? "@" : "",
                 v6 ? "[" : "", b.host, v6 ? "]" : "", port, tmp[0] ? tmp : "/", query ? "?" : "", query ? query : "");
    free(tmp);
    return (n < 0 || (size_t)n >= cap) ? FA_ERROR_INVALID_ARG : FA_OK;
}

void fa_base64(const uint8_t* in, size_t n, char* out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    char* o = out;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        *o++ = tbl[(v >> 18) & 63]; *o++ = tbl[(v >> 12) & 63]; *o++ = tbl[(v >> 6) & 63]; *o++ = tbl[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        *o++ = tbl[(v >> 18) & 63];
        *o++ = tbl[(v >> 12) & 63];
        *o++ = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
}

const char* fa_http_header(const char* headers, const char* name, char* out, size_t cap)
{
    size_t nlen = strlen(name);
    const char* line = headers;
    if (!headers || cap == 0) return NULL;
    while (*line) {
        const char* end = line + strcspn(line, "\r\n");
        if ((size_t)(end - line) > nlen && line[nlen] == ':' && fa_strncasecmp_ascii(line, name, nlen) == 0) {
            const char* v = line + nlen + 1;
            size_t len;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
            len = (size_t)(end - v);
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return out;
        }
        line = end;
        while (*line == '\r' || *line == '\n') line++;
    }
    return NULL;
}

void fa_http_fill_lengths(fa_http_response* r)
{
    char v[128];
    r->content_length = -1;
    r->range_start = 0;
    r->total_length = -1;
    if (fa_http_header(r->headers, "Content-Length", v, sizeof(v))) {
        char* end;
        long long x = strtoll(v, &end, 10);
        if (end != v && x >= 0) r->content_length = (int64_t)x;
    }
    if (r->status == 206 && fa_http_header(r->headers, "Content-Range", v, sizeof(v))) {
        /* "bytes first-last/total" (total may be "*") */
        long long first = -1, last = -1, total = -1;
        const char* p = v;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (sscanf(p, "%lld-%lld/%lld", &first, &last, &total) >= 2 && first >= 0) {
            r->range_start = (int64_t)first;
            if (total > 0) r->total_length = (int64_t)total;
            if (r->content_length < 0 && last >= first) r->content_length = (int64_t)(last - first + 1);
        }
    } else if (r->status == 200) {
        r->total_length = r->content_length;
    }
}
