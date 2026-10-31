/* A web stream's cookie jar (RFC 6265, the parts a media player needs): cookies a
 * server sets with Set-Cookie on a playlist, a redirect or a segment go back to it
 * on the stream's later requests, as a browser would send them. The jar lives as
 * long as the stream and is never written anywhere. The stream's download thread
 * uses it, and a demuxer's own range requests (fa_net_read_range), under its lock.
 *
 * Kept: Domain (the host and its subdomains; without it, the host only), Path,
 * Secure (https only), Max-Age and Expires (an expired cookie deletes the one it
 * names). Left out: SameSite and HttpOnly (no pages or scripts here), and the
 * public suffix list (a Domain must hold a dot, so no cookie covers a whole
 * top-level domain; one stream's servers are its only audience). */
#include "net.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FA_COOKIE_MAX 64            /* cookies per stream; the oldest goes first */
#define FA_COOKIE_BYTES 4096        /* name and value of one cookie */

typedef struct fa_cookie {
    char* name;
    char* value;
    char* domain;            /* lower case, no leading dot */
    char* path;
    int host_only, secure;
    int64_t expires;         /* Unix seconds, -1 = until the stream closes */
    uint64_t created;        /* order of creation, for the Cookie header and eviction */
} fa_cookie;

struct fa_http_cookies {
    fa_mutex lock;
    fa_cookie c[FA_COOKIE_MAX];
    int count;
    uint64_t next;
};

fa_http_cookies* fa_http_cookies_create(void)
{
    fa_http_cookies* j = (fa_http_cookies*)calloc(1, sizeof(fa_http_cookies));
    if (j && fa_mutex_init(&j->lock) != 0) { free(j); j = NULL; }
    return j;
}

static void cookie_free(fa_cookie* k)
{
    free(k->name); free(k->value); free(k->domain); free(k->path);
    memset(k, 0, sizeof(*k));
}

void fa_http_cookies_destroy(fa_http_cookies* j)
{
    int i;
    if (!j) return;
    for (i = 0; i < j->count; i++) cookie_free(&j->c[i]);
    fa_mutex_destroy(&j->lock);
    free(j);
}

static void cookie_remove(fa_http_cookies* j, int i)
{
    cookie_free(&j->c[i]);
    memmove(&j->c[i], &j->c[i + 1], sizeof(j->c[0]) * (size_t)(j->count - i - 1));
    memset(&j->c[j->count - 1], 0, sizeof(j->c[0]));
    j->count--;
}

static int is_ws(char c) { return c == ' ' || c == '\t'; }

static char* dup_range(const char* a, const char* b)
{
    while (a < b && is_ws(*a)) a++;
    while (b > a && is_ws(b[-1])) b--;
    return fa_strndup(a, (size_t)(b - a));
}

static void lower(char* s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a'); }

/* Bytes a Cookie header can carry as they are (fa_http_each_header's rule). */
static int printable(const char* s)
{
    for (; *s; s++) if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7F) return 0;
    return 1;
}

static int is_ip_literal(const char* host)
{
    if (strchr(host, ':')) return 1;
    for (; *host; host++) if (!((*host >= '0' && *host <= '9') || *host == '.')) return 0;
    return 1;
}

/* RFC 6265 5.1.3: host is domain, or a subdomain of it (not for IP addresses). */
static int domain_match(const char* host, const char* domain)
{
    size_t lh = strlen(host), ld = strlen(domain);
    if (lh == ld) return fa_strncasecmp_ascii(host, domain, lh) == 0;
    return lh > ld && host[lh - ld - 1] == '.' && fa_strncasecmp_ascii(host + lh - ld, domain, ld) == 0 && !is_ip_literal(host);
}

/* RFC 6265 5.1.4. `path` is a request path (query cut off). */
static int path_match(const char* path, const char* cpath)
{
    size_t lc = strlen(cpath);
    if (strncmp(path, cpath, lc) != 0) return 0;
    return path[lc] == 0 || cpath[lc - 1] == '/' || path[lc] == '/';
}

/* The default path: the request path up to (not including) its last '/', "/" when that is the first. */
static char* default_path(const char* path)
{
    size_t n = strcspn(path, "?#");
    const char* last = NULL;
    size_t i;
    for (i = 0; i < n; i++) if (path[i] == '/') last = path + i;
    if (!last || last == path) return fa_strdup("/");
    return fa_strndup(path, (size_t)(last - path));
}

/* RFC 6265 5.1.1, leniently: the time, day, month and year tokens in any order.
 * -1 when it can't be read (the attribute is then ignored). */
static int64_t parse_date(const char* s)
{
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int have_time = 0, have_day = 0, have_month = 0, have_year = 0;
    int hh = 0, mm = 0, ss = 0, day = 0, mon = 0, year = 0;
    while (*s) {
        char tok[32];
        size_t n = 0;
        while (*s && !((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == ':')) s++;
        while (*s && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == ':')) {
            if (n + 1 < sizeof(tok)) tok[n++] = *s;
            s++;
        }
        tok[n] = 0;
        if (n == 0) break;
        if (!have_time && strchr(tok, ':')) {
            int a, b, c;
            if (sscanf(tok, "%2d:%2d:%2d", &a, &b, &c) == 3) { hh = a; mm = b; ss = c; have_time = 1; continue; }
        }
        if (tok[0] >= '0' && tok[0] <= '9') {
            size_t d = strspn(tok, "0123456789");
            int v = atoi(tok);
            if (!have_day && d <= 2) { day = v; have_day = 1; continue; }
            if (!have_year && d >= 2 && d <= 4) { year = v; have_year = 1; continue; }
            continue;
        }
        if (!have_month && n >= 3) {
            int i;
            for (i = 0; i < 12; i++)
                if (fa_strncasecmp_ascii(tok, months + i * 3, 3) == 0) { mon = i + 1; have_month = 1; break; }
        }
    }
    if (!have_time || !have_day || !have_month || !have_year) return -1;
    if (year >= 70 && year <= 99) year += 1900;
    else if (year < 70) year += 2000;
    if (day < 1 || day > 31 || year < 1601 || hh > 23 || mm > 59 || ss > 59) return -1;
    {
        /* Days from 1970-01-01 (H. Hinnant's days_from_civil). */
        int y = mon <= 2 ? year - 1 : year;
        int era = (y >= 0 ? y : y - 399) / 400;
        int yoe = y - era * 400;
        int doy = (153 * (mon > 2 ? mon - 3 : mon + 9) + 2) / 5 + day - 1;
        int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        int64_t days = (int64_t)era * 146097 + doe - 719468;
        return days * 86400 + hh * 3600 + mm * 60 + ss;
    }
}

/* One Set-Cookie value from a response for `u`. */
static void take_one(fa_http_cookies* j, const fa_url* u, const char* line, int64_t now)
{
    const char* end = line + strlen(line);
    const char* semi = strchr(line, ';');
    const char* pair_end = semi ? semi : end;
    const char* eq = memchr(line, '=', (size_t)(pair_end - line));
    fa_cookie k;
    int64_t max_age = INT64_MIN, expires = -1;
    int i;
    if (!eq) return;   /* "a" without a value: ignored, as browsers do for a missing name */
    memset(&k, 0, sizeof(k));
    k.name = dup_range(line, eq);
    k.value = dup_range(eq + 1, pair_end);
    k.expires = -1;
    k.host_only = 1;
    if (!k.name || !k.value || !k.name[0] || strlen(k.name) + strlen(k.value) > FA_COOKIE_BYTES ||
        !printable(k.name) || !printable(k.value) || strchr(k.name, ' '))
        goto drop;
    while (semi) {
        const char* a = semi + 1;
        const char* a_end;
        const char* a_eq;
        char* av;
        semi = strchr(a, ';');
        a_end = semi ? semi : end;
        a_eq = memchr(a, '=', (size_t)(a_end - a));
        while (a < a_end && is_ws(*a)) a++;
        av = a_eq ? dup_range(a_eq + 1, a_end) : NULL;
#define ATTR(nm) ((size_t)((a_eq ? a_eq : a_end) - a) >= sizeof(nm) - 1 && \
                  fa_strncasecmp_ascii(a, nm, sizeof(nm) - 1) == 0 && \
                  ((a_eq ? a_eq : a_end) - a == (ptrdiff_t)sizeof(nm) - 1 || is_ws(a[sizeof(nm) - 1])))
        if (ATTR("domain") && av && av[0]) {
            char* d = av[0] == '.' ? av + 1 : av;
            lower(d);
            free(k.domain);
            k.domain = fa_strdup(d);
            k.host_only = 0;
        } else if (ATTR("path") && av && av[0] == '/') {
            free(k.path);
            k.path = av;
            av = NULL;
        } else if (ATTR("secure")) {
            k.secure = 1;
        } else if (ATTR("max-age") && av && ((av[0] >= '0' && av[0] <= '9') || av[0] == '-')) {
            max_age = strtoll(av, NULL, 10);
        } else if (ATTR("expires") && av) {
            int64_t t = parse_date(av);
            if (t >= 0) expires = t;
        }
#undef ATTR
        free(av);
    }
    if (max_age != INT64_MIN) k.expires = max_age <= 0 ? 0 : now + (max_age > INT64_MAX / 2 ? INT64_MAX / 2 : max_age);
    else if (expires >= 0) k.expires = expires;
    if (k.host_only) {
        k.domain = fa_strdup(u->host);
        if (!k.domain) goto drop;
        lower(k.domain);
    } else if (!k.domain || !strchr(k.domain, '.') || !domain_match(u->host, k.domain)) {
        goto drop;   /* another server's, or a whole top-level domain */
    }
    if (!k.path) k.path = default_path(u->path);
    if (!k.path) goto drop;
    /* The same cookie (name, domain, path) is replaced, or deleted by an expired one. */
    for (i = 0; i < j->count; i++) {
        fa_cookie* o = &j->c[i];
        if (strcmp(o->name, k.name) == 0 && strcmp(o->domain, k.domain) == 0 && strcmp(o->path, k.path) == 0) {
            k.created = o->created;
            cookie_remove(j, i);
            break;
        }
    }
    if (k.expires >= 0 && k.expires <= now) goto drop;
    if (j->count == FA_COOKIE_MAX) cookie_remove(j, 0);   /* the oldest */
    if (!k.created) k.created = ++j->next;
    j->c[j->count++] = k;
    return;
drop:
    cookie_free(&k);
}

void fa_http_cookies_take(fa_http_cookies* j, const char* url, const char* headers)
{
    fa_url u;
    int64_t now = (int64_t)time(NULL);
    const char* line = headers;
    if (!j || !headers || fa_url_parse(url, &u) != FA_OK) return;
    fa_mutex_lock(&j->lock);
    while (*line) {
        size_t len = strcspn(line, "\r\n");
        if (len > 11 && fa_strncasecmp_ascii(line, "Set-Cookie:", 11) == 0) {
            char* v = fa_strndup(line + 11, len - 11);
            if (v) {
                take_one(j, &u, v, now);
                free(v);
            }
        }
        line += len;
        while (*line == '\r' || *line == '\n') line++;
    }
    fa_mutex_unlock(&j->lock);
}

/* Caller holds the jar's lock. */
static char* cookies_for_locked(fa_http_cookies* j, const char* url)
{
    fa_url u;
    int64_t now = (int64_t)time(NULL);
    int pick[FA_COOKIE_MAX];
    int n = 0, i, a;
    size_t len = 0;
    char* out;
    char path[FA_NET_URL_MAX];
    if (!j || j->count == 0 || fa_url_parse(url, &u) != FA_OK) return NULL;
    snprintf(path, sizeof(path), "%.*s", (int)strcspn(u.path, "?#"), u.path);
    for (i = 0; i < j->count; i++) {
        fa_cookie* k = &j->c[i];
        if (k->expires >= 0 && k->expires <= now) { cookie_remove(j, i--); continue; }
        if (k->host_only ? fa_strncasecmp_ascii(u.host, k->domain, sizeof(u.host)) != 0 : !domain_match(u.host, k->domain)) continue;
        if (!path_match(path, k->path)) continue;
        if (k->secure && strcmp(u.scheme, "https") != 0) continue;
        pick[n++] = i;
    }
    if (n == 0) return NULL;
    /* Longer paths first, then the older cookie (RFC 6265 5.4). */
    for (a = 1; a < n; a++) {
        int v = pick[a], b = a;
        size_t lv = strlen(j->c[v].path);
        while (b > 0) {
            fa_cookie* p = &j->c[pick[b - 1]];
            size_t lp = strlen(p->path);
            if (lp > lv || (lp == lv && p->created < j->c[v].created)) break;
            pick[b] = pick[b - 1];
            b--;
        }
        pick[b] = v;
    }
    for (i = 0; i < n; i++) len += strlen(j->c[pick[i]].name) + strlen(j->c[pick[i]].value) + 3;
    out = (char*)malloc(len + 1);
    if (!out) return NULL;
    len = 0;
    for (i = 0; i < n; i++) {
        const fa_cookie* k = &j->c[pick[i]];
        size_t ln = strlen(k->name), lv = strlen(k->value);
        if (i) { memcpy(out + len, "; ", 2); len += 2; }
        memcpy(out + len, k->name, ln);
        out[len + ln] = '=';
        memcpy(out + len + ln + 1, k->value, lv);
        len += ln + 1 + lv;
    }
    out[len] = 0;
    return out;
}

char* fa_http_cookies_for(fa_http_cookies* j, const char* url)
{
    char* out;
    if (!j) return NULL;
    fa_mutex_lock(&j->lock);
    out = cookies_for_locked(j, url);
    fa_mutex_unlock(&j->lock);
    return out;
}
