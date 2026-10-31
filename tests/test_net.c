/* Web streams: URL handling, HLS parsing, the TS demuxer, AES, and URL streams
 * end to end against a small HTTP server running inside the test on 127.0.0.1.
 * The server plays every part a real one would: byte ranges, chunked bodies,
 * redirects, errors, a connection that drops, an Icecast radio station with song
 * titles, and an HLS playlist with MPEG-TS segments. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE   /* INADDR_LOOPBACK and friends, hidden by _POSIX_C_SOURCE */
#endif
#include "net.h"
#if defined(__ANDROID__)
#  include "net_tls_android.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define close_sock closesocket
#  define SHUT_BOTH SD_BOTH
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define close_sock close
#  define SHUT_BOTH SHUT_RDWR
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_STR(a, b) do { const char* va_ = (a); const char* vb_ = (b); if (strcmp(va_, vb_) != 0) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { const char* only_ = getenv("FA_TEST"); int before_ = g_failures; \
    if (only_ && strcmp(only_, #t) != 0) break; \
    printf("%s\n", #t); fflush(stdout); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

/* ---- test media ------------------------------------------------------------- */

static uint8_t* g_mp3;          /* tests/data/net/tone_cbr.mp3 */
static size_t g_mp3_n;
static uint8_t* g_mp3x;         /* tests/data/net/tone_xing.mp3 (Xing header, ID3 tag) */
static size_t g_mp3x_n;
static uint8_t* g_oggradio;     /* tests/data/ogg/radio.opus: two chained 1 s songs with titles */
static size_t g_oggradio_n;
static uint8_t* g_aac;          /* tests/data/net/tone.aac (ADTS, 131 frames at 44.1 kHz) */
static size_t g_aac_n;
static uint8_t* g_aac_tagged;   /* g_aac after a 30 KB ID3v2 tag (cover art) */
static size_t g_aac_tagged_n;
static uint8_t* g_wma;          /* tests/data/asf/tone.wma (WMA 2, 2 s at 44.1 kHz) */
static size_t g_wma_n;
static uint8_t* g_asf_pcm;      /* tests/data/asf/pcm.asf (PCM in ASF, 10000 frames at 8 kHz) */
static size_t g_asf_pcm_n;
static uint8_t* g_wav;          /* 2 s of 16-bit stereo at 44100 Hz, generated */
static size_t g_wav_n;
#define WAV_FRAMES 88200

static uint8_t* read_file(const char* path, size_t* out_n)
{
    FILE* f = fopen(path, "rb");
    uint8_t* b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t*)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *out_n = (size_t)n;
    return b;
}

static void make_wav(void)
{
    size_t data = (size_t)WAV_FRAMES * 4;
    uint32_t i, v, rate = 44100;
    uint8_t* b;
    g_wav_n = 44 + data;
    b = g_wav = (uint8_t*)malloc(g_wav_n);
    memcpy(b, "RIFF", 4); v = (uint32_t)(g_wav_n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[21] = 0; b[22] = 2; b[23] = 0;
    memcpy(b + 24, &rate, 4); v = rate * 4; memcpy(b + 28, &v, 4);
    b[32] = 4; b[33] = 0; b[34] = 16; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < WAV_FRAMES; i++) {
        /* The left channel encodes the frame index, so any misplaced byte shows. */
        int16_t l = (int16_t)((int)(i % 8192) * 4 - 16384), r = (int16_t)(i / 8192 * 100);
        memcpy(b + 44 + (size_t)i * 4, &l, 2);
        memcpy(b + 46 + (size_t)i * 4, &r, 2);
    }
}

/* MP3 frame boundaries of g_mp3 (MPEG-1 layer III), for cutting HLS segments. */
static size_t mp3_frame_size(const uint8_t* h)
{
    static const int kbps[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int rates[4] = { 44100, 48000, 32000, 0 };
    int br, sr;
    if (h[0] != 0xFF || (h[1] & 0xFE) != 0xFA) return 0;
    br = kbps[h[2] >> 4];
    sr = rates[(h[2] >> 2) & 3];
    if (!br || !sr) return 0;
    return (size_t)(144 * br * 1000 / sr + ((h[2] >> 1) & 1));
}

/* ---- MPEG-TS muxing (for the HLS segments) ----------------------------------- */

static uint32_t crc32_mpeg(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 24;
        for (b = 0; b < 8; b++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

typedef struct tsbuf { uint8_t* p; size_t n, cap; int cc[0x2000]; } tsbuf;

static void ts_packet(tsbuf* t, int pid, int start, const uint8_t* payload, size_t len)
{
    uint8_t pk[188];
    size_t head = 4;
    if (t->n + 188 > t->cap) { t->cap = t->cap * 2 + 188 * 64; t->p = (uint8_t*)realloc(t->p, t->cap); }
    pk[0] = 0x47;
    pk[1] = (uint8_t)((start ? 0x40 : 0) | (pid >> 8));
    pk[2] = (uint8_t)(pid & 0xFF);
    if (len < 184) {
        size_t af = 183 - len;           /* adaptation field length */
        pk[3] = (uint8_t)(0x30 | (t->cc[pid] & 15));
        pk[4] = (uint8_t)af;
        if (af > 0) { pk[5] = 0; memset(pk + 6, 0xFF, af - 1); }
        head = 5 + af;
    } else {
        pk[3] = (uint8_t)(0x10 | (t->cc[pid] & 15));
    }
    t->cc[pid]++;
    memcpy(pk + head, payload, len);
    memcpy(t->p + t->n, pk, 188);
    t->n += 188;
}

static void ts_section(tsbuf* t, int pid, const uint8_t* sec, size_t n)
{
    uint8_t buf[184];
    uint32_t crc = crc32_mpeg(sec, n);
    buf[0] = 0;                          /* pointer field */
    memcpy(buf + 1, sec, n);
    buf[1 + n] = (uint8_t)(crc >> 24); buf[2 + n] = (uint8_t)(crc >> 16);
    buf[3 + n] = (uint8_t)(crc >> 8); buf[4 + n] = (uint8_t)crc;
    memset(buf + 5 + n, 0xFF, 184 - 5 - n);
    ts_packet(t, pid, 1, buf, 184);
}

/* A TS file with PAT, PMT (one audio stream of the given type on PID 0x101) and
 * the audio in PES packets of up to 2000 bytes with the given stream id. */
static void ts_mux_as(tsbuf* t, const uint8_t* es, size_t n, uint8_t stream_type, uint8_t pes_id)
{
    static const uint8_t pat[] = { 0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xF0, 0x00 };
    uint8_t pmt[] = { 0x02, 0xB0, 18, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00,
                      0x03, 0xE1, 0x01, 0xF0, 0x00 };
    pmt[12] = stream_type;
    ts_section(t, 0, pat, sizeof(pat));
    ts_section(t, 0x1000, pmt, sizeof(pmt));
    while (n > 0) {
        uint8_t pes[2009];
        size_t chunk = n < 2000 ? n : 2000, len = 9 + chunk, off = 0;
        pes[0] = 0; pes[1] = 0; pes[2] = 1; pes[3] = pes_id;
        pes[4] = (uint8_t)((chunk + 3) >> 8); pes[5] = (uint8_t)(chunk + 3);
        pes[6] = 0x80; pes[7] = 0; pes[8] = 0;
        memcpy(pes + 9, es, chunk);
        while (off < len) {
            size_t k = len - off < 184 ? len - off : 184;
            ts_packet(t, 0x101, off == 0, pes + off, k);
            off += k;
        }
        es += chunk;
        n -= chunk;
    }
}

static void ts_mux(tsbuf* t, const uint8_t* es, size_t n) { ts_mux_as(t, es, n, 0x03, 0xC0); }

/* HLS: g_mp3 cut at frame boundaries into HLS_SEGS segments. */
#define HLS_SEGS 3
static tsbuf g_seg[HLS_SEGS];
static double g_seg_dur[HLS_SEGS];

static void make_hls(void)
{
    size_t pos = 0, cut[HLS_SEGS + 1];
    int frames = 0, total = 0, s;
    size_t p = 0;
    while (p + 4 <= g_mp3_n) { size_t fs = mp3_frame_size(g_mp3 + p); if (!fs) break; p += fs; total++; }
    cut[0] = 0;
    s = 1;
    p = 0;
    while (p + 4 <= g_mp3_n && s < HLS_SEGS) {
        size_t fs = mp3_frame_size(g_mp3 + p);
        if (!fs) break;
        p += fs;
        frames++;
        if (frames >= total * s / HLS_SEGS) { cut[s++] = p; }
    }
    cut[HLS_SEGS] = g_mp3_n;
    for (s = 0; s < HLS_SEGS; s++) {
        size_t f = 0, q = cut[s];
        while (q + 4 <= cut[s + 1]) { size_t fs = mp3_frame_size(g_mp3 + q); if (!fs) break; q += fs; f++; }
        g_seg_dur[s] = (double)f * 1152.0 / 44100.0;
        memset(&g_seg[s], 0, sizeof(g_seg[s]));
        ts_mux(&g_seg[s], g_mp3 + cut[s], cut[s + 1] - cut[s]);
    }
    (void)pos;
}

static size_t adts_frame_size(const uint8_t* h)
{
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) return 0;
    return ((size_t)(h[3] & 3) << 11) | ((size_t)h[4] << 3) | (h[5] >> 5);
}

/* HLS with AAC: g_aac cut at frame boundaries into HLS_SEGS TS segments. */
static tsbuf g_aseg[HLS_SEGS];
static double g_aseg_dur[HLS_SEGS];

static void make_hls_aac(void)
{
    size_t p = 0, start = 0;
    int f = 0, s = 0, per = 131 / HLS_SEGS + 1;
    while (s < HLS_SEGS) {
        size_t fs = p + 7 <= g_aac_n ? adts_frame_size(g_aac + p) : 0;
        if (fs) { p += fs; f++; }
        if (!fs || f == per * (s + 1)) {
            g_aseg_dur[s] = (double)(f - per * s) * 1024.0 / 44100.0;
            memset(&g_aseg[s], 0, sizeof(g_aseg[s]));
            ts_mux_as(&g_aseg[s], g_aac + start, p - start, 0x0F, 0xC0);
            start = p;
            s++;
            if (!fs) break;
        }
    }
}

/* HLS with AC-3 (stream type 0x81, private stream 1): tone.ac3 in two segments. */
static uint8_t* g_ac3;
static size_t g_ac3_n;
static tsbuf g_ac3_seg[2];
static size_t g_ac3_cut;   /* bytes in the first segment, a whole number of 768-byte frames */

static void make_hls_ac3(void)
{
    g_ac3_cut = g_ac3_n / 768 / 2 * 768;
    memset(g_ac3_seg, 0, sizeof(g_ac3_seg));
    ts_mux_as(&g_ac3_seg[0], g_ac3, g_ac3_cut, 0x81, 0xBD);
    ts_mux_as(&g_ac3_seg[1], g_ac3 + g_ac3_cut, g_ac3_n - g_ac3_cut, 0x81, 0xBD);
}

/* ---- the HTTP server ---------------------------------------------------------- */

/* Single-file HLS with an init section (EXT-X-MAP BYTERANGE): tone_cbr.mp3 behind
 * MAP_PAD bytes of zeros. The map is its first frames, the segments the rest, all
 * byte ranges of the one file. */
#define MAP_PAD 4096
#define MAP_SEGS 3
static uint8_t* g_pad;
static size_t g_pad_n;
static size_t g_map_cut[MAP_SEGS + 2];   /* frame boundaries in g_mp3: map, segments, end */

static void make_hls_map(void)
{
    size_t at = 0, frames = 0, total = 0, i, k = 1;
    while (at + 4 <= g_mp3_n && mp3_frame_size(g_mp3 + at)) { at += mp3_frame_size(g_mp3 + at); total++; }
    g_map_cut[0] = 0;
    at = 0;
    while (k <= MAP_SEGS && at + 4 <= g_mp3_n && mp3_frame_size(g_mp3 + at)) {
        at += mp3_frame_size(g_mp3 + at);
        frames++;
        if (frames == (k == 1 ? 8 : 8 + (total - 8) * (k - 1) / MAP_SEGS)) g_map_cut[k++] = at;
    }
    for (i = k; i <= MAP_SEGS + 1; i++) g_map_cut[i] = g_mp3_n;
    g_pad_n = MAP_PAD + g_mp3_n;
    g_pad = (uint8_t*)calloc(1, g_pad_n);
    memcpy(g_pad + MAP_PAD, g_mp3, g_mp3_n);
}

static char g_cred_playlist[2048];   /* /hls/cred.m3u8: segments on another origin (set by the test) */
static fa_atomic32 g_cred_home;      /* requests to 127.0.0.1 with a Cookie or Authorization */
static fa_atomic32 g_cred_foreign;   /* ... and to "localhost" */

static sock_t g_listen = BAD_SOCK;
static int g_port;
static fa_thread g_accept_thread;
static fa_atomic32 g_stop;
static fa_mutex g_conn_lock;
#define MAX_CONNS 256
static sock_t g_conns[MAX_CONNS];
static fa_thread g_conn_threads[MAX_CONNS];
static int g_conn_count;
static fa_atomic32 g_requests;     /* requests served */
static fa_atomic32 g_drop_served;  /* /drop.wav requests */
static fa_atomic32 g_stall_served; /* /stall.wav requests */
static fa_atomic32 g_seg_drop;     /* /hls/flaky*.ts: the next request of segment 1 breaks off */
static fa_atomic32 g_live_served;  /* /live-close requests */
static fa_atomic32 g_cookie_ok;    /* /hls/cseg*.ts requests with the expected cookies */
static fa_atomic32 g_cookie_bad;   /* ... and without them (refused) */
static char g_cookie_line[512];    /* the Cookie line of the last /hls/cseg*.ts request */
static char g_last_agent[4096];    /* User-Agent of the last request */

static int send_all(sock_t s, const void* p, size_t n)
{
    const char* c = (const char*)p;
    while (n > 0) {
        int k = (int)send(s, c, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0) return -1;
        c += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_str(sock_t s, const char* text) { return send_all(s, text, strlen(text)); }

/* Serves `body` honouring "Range: bytes=N-" when `ranges`; `cut` > 0 closes the
 * connection after that many body bytes (a dropped connection). */
static void serve_bytes(sock_t s, const char* req, const uint8_t* body, size_t n, const char* type, int ranges, size_t cut)
{
    char head[512];
    const char* r = strstr(req, "\r\nRange: bytes=");
    size_t from = 0, len;
    if (ranges && r) from = (size_t)strtoull(r + 15, NULL, 10);
    if (from > n) {
        snprintf(head, sizeof(head), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        send_str(s, head);
        return;
    }
    len = n - from;
    if (ranges && r)
        snprintf(head, sizeof(head), "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                 "Content-Length: %zu\r\nContent-Range: bytes %zu-%zu/%zu\r\nConnection: close\r\n\r\n",
                 type, len, from, n - 1, n);
    else
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 type, ranges ? "Accept-Ranges: bytes\r\n" : "", len);
    if (send_str(s, head)) return;
    if (cut && len > cut) len = cut;
    send_all(s, body + from, len);
}

static void serve_chunked(sock_t s, const uint8_t* body, size_t n)
{
    size_t off = 0;
    send_str(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
    while (off < n) {
        char line[32];
        size_t k = n - off < 3000 ? n - off : 3000;
        snprintf(line, sizeof(line), "%zx\r\n", k);
        if (send_str(s, line) || send_all(s, body + off, k) || send_str(s, "\r\n")) return;
        off += k;
    }
    send_str(s, "0\r\n\r\n");
}

/* An Icecast station: the MP3 tone looped forever, a title block every
 * RADIO_METAINT audio bytes, and the second song from audio byte RADIO_SONG2. */
#define RADIO_METAINT 8192
#define RADIO_SONG2 32768
static void serve_radio(sock_t s, const char* req, const uint8_t* body, size_t n, const char* type, int kbps)
{
    int icy = strstr(req, "\r\nIcy-MetaData: 1") != NULL;
    size_t audio = 0, at = 0;
    char head[512];
    snprintf(head, sizeof(head), "ICY 200 OK\r\nContent-Type: %s\r\nicy-name: Test FM\r\nicy-genre: Testing\r\n"
             "icy-br: %d\r\n%s\r\n", type, kbps, icy ? "icy-metaint: 8192\r\n" : "");
    if (send_str(s, head)) return;
    while (!fa_load32(&g_stop)) {
        size_t k = RADIO_METAINT - audio % RADIO_METAINT;
        if (k > n - at) k = n - at;
        if (send_all(s, body + at, k)) return;
        at = (at + k) % n;
        audio += k;
        if (icy && audio % RADIO_METAINT == 0) {
            uint8_t block[1 + 16 * 16];
            const char* title = audio <= RADIO_SONG2 - RADIO_METAINT ? "Artist A - Song 1" : "Artist B - Song 2";
            int len;
            memset(block, 0, sizeof(block));
            len = snprintf((char*)block + 1, sizeof(block) - 1, "StreamTitle='%s';StreamUrl='http://example.com/%c';",
                           title, title[7]);
            block[0] = (uint8_t)((len + 15) / 16);
            if (send_all(s, block, 1 + (size_t)block[0] * 16)) return;
        }
    }
}

/* An Ogg Opus station: the two chained songs of radio.opus over and over, each
 * song's title in its own comment header (no ICY metadata). */
static void serve_ogg_radio(sock_t s)
{
    if (send_str(s, "ICY 200 OK\r\nContent-Type: audio/ogg\r\nicy-name: Ogg FM\r\n\r\n")) return;
    while (!fa_load32(&g_stop))
        if (send_all(s, g_oggradio, g_oggradio_n)) return;
}

static void serve_hls(sock_t s, const char* req, const char* path)
{
    char text[2048], head[256];
    int i;
    if (strcmp(path, "/hls/flaky.m3u8") == 0 || strcmp(path, "/hls/ranged.m3u8") == 0) {
        /* flaky: segment 1 breaks off halfway the first time. ranged: all segments are
         * byte ranges of one file, on a server that ignores Range. */
        int ranged = path[5] == 'r';
        int n = snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-TARGETDURATION:2\n");
        size_t off = 0;
        for (i = 0; i < HLS_SEGS; i++) {
            if (ranged) n += snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-BYTERANGE:%zu@%zu\n", g_seg[i].n, off);
            n += snprintf(text + n, sizeof(text) - (size_t)n, "#EXTINF:%.5f,\n%s%d.ts\n", g_seg_dur[i], ranged ? "all" : "flaky", ranged ? 0 : i);
            off += g_seg[i].n;
        }
        snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-ENDLIST\n");
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(text));
        if (!send_str(s, head)) send_str(s, text);
        return;
    }
    if (strcmp(path, "/hls/map.m3u8") == 0 || strcmp(path, "/hls/map-nr.m3u8") == 0) {
        /* nr: on a server that ignores Range. */
        const char* file = path[9] == '-' ? "padded-nr.mp3" : "padded.mp3";
        int n = snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n"
                         "#EXT-X-MAP:URI=\"%s\",BYTERANGE=\"%zu@%d\"\n", file, g_map_cut[1], MAP_PAD);
        for (i = 1; i <= MAP_SEGS; i++)
            n += snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-BYTERANGE:%zu@%zu\n#EXTINF:1.0,\n%s\n",
                          g_map_cut[i + 1] - g_map_cut[i], MAP_PAD + g_map_cut[i], file);
        snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-ENDLIST\n");
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(text));
        if (!send_str(s, head)) send_str(s, text);
        return;
    }
    if (strcmp(path, "/hls/padded.mp3") == 0 || strcmp(path, "/hls/padded-nr.mp3") == 0) {
        serve_bytes(s, req, g_pad, g_pad_n, "audio/mpeg", path[11] != '-', 0);
        return;
    }
    if (strcmp(path, "/hls/cred.m3u8") == 0) {
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(g_cred_playlist));
        if (!send_str(s, head)) send_str(s, g_cred_playlist);
        return;
    }
    if (strcmp(path, "/hls/all0.ts") == 0) {
        size_t total = 0;
        for (i = 0; i < HLS_SEGS; i++) total += g_seg[i].n;
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", total);
        if (send_str(s, head)) return;
        for (i = 0; i < HLS_SEGS; i++) if (send_all(s, g_seg[i].p, g_seg[i].n)) return;
        return;
    }
    if (strncmp(path, "/hls/flaky", 10) == 0) {
        i = atoi(path + 10);
        if (i >= 0 && i < HLS_SEGS) {
            size_t len = g_seg[i].n;
            snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len);
            if (i == 1 && fa_exchange32(&g_seg_drop, 0)) len /= 2;
            if (!send_str(s, head)) send_all(s, g_seg[i].p, len);
            return;
        }
    }
    if (strcmp(path, "/hls/ac3.m3u8") == 0) {
        double d0 = (double)(g_ac3_cut / 768) * 1536.0 / 48000.0, d1 = (double)((g_ac3_n - g_ac3_cut) / 768) * 1536.0 / 48000.0;
        snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:%.5f,\nac3_0.ts\n#EXTINF:%.5f,\nac3_1.ts\n#EXT-X-ENDLIST\n", d0, d1);
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(text));
        if (!send_str(s, head)) send_str(s, text);
        return;
    }
    if (strncmp(path, "/hls/ac3_", 9) == 0) {
        i = atoi(path + 9);
        if (i == 0 || i == 1) {
            snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_ac3_seg[i].n);
            if (!send_str(s, head)) send_all(s, g_ac3_seg[i].p, g_ac3_seg[i].n);
            return;
        }
    }
    if (strcmp(path, "/hls/cookie-login") == 0) {
        /* A login: a session cookie for /hls, and one for another path, then the playlist. */
        send_str(s, "HTTP/1.1 302 Found\r\nLocation: /hls/cookie.m3u8\r\nSet-Cookie: sid=abc123; Path=/hls; HttpOnly\r\n"
                    "Set-Cookie: other=x; Path=/elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    if (strcmp(path, "/hls/cookie.m3u8") == 0) {
        /* The playlist adds a cookie of its own (default path /hls). */
        int n = snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n");
        for (i = 0; i < HLS_SEGS; i++) n += snprintf(text + n, sizeof(text) - (size_t)n, "#EXTINF:%.5f,\ncseg%d.ts\n", g_seg_dur[i], i);
        snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-ENDLIST\n");
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nSet-Cookie: seen=1\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(text));
        if (!send_str(s, head)) send_str(s, text);
        return;
    }
    if (strncmp(path, "/hls/cseg", 9) == 0) {
        /* Only with both cookies, and never the one for another path. */
        const char* ck = strstr(req, "\r\nCookie: ");
        i = atoi(path + 9);
        if (ck) snprintf(g_cookie_line, sizeof(g_cookie_line), "%.*s", (int)strcspn(ck + 10, "\r\n"), ck + 10);
        if (i < 0 || i >= HLS_SEGS || !ck || !strstr(g_cookie_line, "sid=abc123") || !strstr(g_cookie_line, "seen=1") ||
            strstr(g_cookie_line, "other=")) {
            fa_fetch_add32(&g_cookie_bad, 1);
            send_str(s, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        fa_fetch_add32(&g_cookie_ok, 1);
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_seg[i].n);
        if (!send_str(s, head)) send_all(s, g_seg[i].p, g_seg[i].n);
        return;
    }
    if (strcmp(path, "/hls/tiers.m3u8") == 0) {
        /* Video variants with tiered audio groups: MP3 segments in the top tier's group,
         * AAC in the lower one's. The video playlists don't exist. */
        snprintf(text, sizeof(text), "#EXTM3U\n"
                 "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lo\",NAME=\"Main\",DEFAULT=YES,URI=\"aac.m3u8\"\n"
                 "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hi\",NAME=\"Main\",DEFAULT=YES,URI=\"audio.m3u8\"\n"
                 "#EXT-X-STREAM-INF:BANDWIDTH=2500000,CODECS=\"avc1.64001f,mp4a.40.2\",AUDIO=\"hi\"\nnone-hi.m3u8\n"
                 "#EXT-X-STREAM-INF:BANDWIDTH=500000,CODECS=\"avc1.64001f,mp4a.40.5\",AUDIO=\"lo\"\nnone-lo.m3u8\n");
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(text));
        if (!send_str(s, head)) send_str(s, text);
        return;
    }
    if (strcmp(path, "/hls/master.m3u8") == 0) {
        snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                 "video.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=130000,CODECS=\"mp4a.40.34\"\naudio.m3u8\n");
    } else if (strcmp(path, "/hls/aac.m3u8") == 0) {
        int n = snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n");
        for (i = 0; i < HLS_SEGS; i++) n += snprintf(text + n, sizeof(text) - (size_t)n, "#EXTINF:%.5f,\naac%d.ts\n", g_aseg_dur[i], i);
        snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-ENDLIST\n");
    } else if (strncmp(path, "/hls/aac", 8) == 0 && strstr(path, ".ts")) {
        i = atoi(path + 8);
        if (i >= 0 && i < HLS_SEGS) {
            snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_aseg[i].n);
            if (!send_str(s, head)) send_all(s, g_aseg[i].p, g_aseg[i].n);
            return;
        }
        send_str(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    } else if (strcmp(path, "/hls/audio.m3u8") == 0) {
        int n = snprintf(text, sizeof(text), "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n");
        for (i = 0; i < HLS_SEGS; i++) n += snprintf(text + n, sizeof(text) - (size_t)n, "#EXTINF:%.5f,\nseg%d.ts\n", g_seg_dur[i], i);
        snprintf(text + n, sizeof(text) - (size_t)n, "#EXT-X-ENDLIST\n");
    } else if (strncmp(path, "/hls/seg", 8) == 0) {
        i = atoi(path + 8);
        if (i >= 0 && i < HLS_SEGS) {
            snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_seg[i].n);
            if (!send_str(s, head)) send_all(s, g_seg[i].p, g_seg[i].n);
            return;
        }
        send_str(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    } else {
        send_str(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
             strlen(text));
    if (!send_str(s, head)) send_str(s, text);
}

typedef struct conn_arg { sock_t s; int id; } conn_arg;
static int handle_request(sock_t s, int id);

/* One connection: one request, then the server closes its side (the socket itself
 * is closed by server_stop, so the descriptor is never reused while listed). Only
 * /keep-echo keeps the connection for another request. */
static void handle(void* arg)
{
    conn_arg c = *(conn_arg*)arg;
    free(arg);
    while (handle_request(c.s, c.id)) {}
    shutdown(c.s, SHUT_BOTH);
}

/* Serves one request; returns 1 to wait for another on the same connection. */
static int handle_request(sock_t s, int id)
{
    char req[16384], path[512];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        int k = (int)recv(s, req + got, (int)(sizeof(req) - 1 - got), 0);
        if (k <= 0) return 0;
        got += (size_t)k;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (sscanf(req, "GET %511s", path) != 1) return 0;
    fa_fetch_add32(&g_requests, 1);
    {
        const char* ua = strstr(req, "\r\nUser-Agent: ");
        if (ua) { ua += 14; snprintf(g_last_agent, sizeof(g_last_agent), "%.*s", (int)strcspn(ua, "\r\n"), ua); }
        if (strstr(req, "\r\nCookie:") || strstr(req, "\r\nAuthorization:"))
            fa_fetch_add32(strstr(req, "\r\nHost: localhost") ? &g_cred_foreign : &g_cred_home, 1);
    }
    if (strcmp(path, "/echo-headers") == 0 || strcmp(path, "/keep-echo") == 0) {
        /* The request's header lines as the body (as tools/test-https/serve.py does).
         * /keep-echo keeps the connection open unless the client closes it, and
         * says which connection it is (X-Connection). */
        char head[256];
        const char* lines = strstr(req, "\r\n") + 2;
        size_t n = (size_t)(strstr(req, "\r\n\r\n") + 2 - lines);
        int keep = path[1] == 'k' && !strstr(req, "\r\nConnection: close\r\n");
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nX-Connection: %d\r\n%s\r\n",
                 n, id, keep ? "" : "Connection: close\r\n");
        if (send_str(s, head) || send_all(s, lines, n)) return 0;
        return keep;
    }
    else if (strncmp(path, "/redirect?to=", 13) == 0) {
        char head[700];
        snprintf(head, sizeof(head), "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", path + 13);
        send_str(s, head);
    }
    else if (strcmp(path, "/tone.wav") == 0) serve_bytes(s, req, g_wav, g_wav_n, "audio/wav", 1, 0);
    else if (strcmp(path, "/tone.mp3") == 0) serve_bytes(s, req, g_mp3, g_mp3_n, "audio/mpeg", 1, 0);
    else if (strcmp(path, "/xing.mp3") == 0) serve_bytes(s, req, g_mp3x, g_mp3x_n, "audio/mpeg", 1, 0);
    else if (strcmp(path, "/noranges.wav") == 0) serve_bytes(s, req, g_wav, g_wav_n, "audio/wav", 0, 0);
    else if (strcmp(path, "/chunked.wav") == 0) serve_chunked(s, g_wav, g_wav_n);
    else if (strcmp(path, "/drop.wav") == 0) {
        /* The first connection breaks off after 100000 bytes. */
        int first = fa_fetch_add32(&g_drop_served, 1) == 0;
        serve_bytes(s, req, g_wav, g_wav_n, "audio/wav", 1, first ? 100000 : 0);
    }
    else if (strcmp(path, "/stall.wav") == 0) {
        /* 1 s of audio, then the server goes quiet for 1.5 s, then the rest. The pause
         * is timed by the clock (sleeps overshoot a lot on busy CI machines) and only
         * the first connection pauses, so a reconnect can't stall again. */
        char head[256];
        size_t first = 44 + 44100 * 4;
        int pause = fa_fetch_add32(&g_stall_served, 1) == 0;
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_wav_n);
        if (!send_str(s, head) && !send_all(s, g_wav, first)) {
            int64_t until = fa_monotonic_ns() + 1500000000LL;
            while (pause && fa_monotonic_ns() < until && !fa_load32(&g_stop)) fa_sleep_ms(10);
            send_all(s, g_wav + first, g_wav_n - first);
        }
    }
    else if (strcmp(path, "/hang.wav") == 0) {
        /* 1 s of audio, then nothing for 5 s (every connection), then the rest. */
        char head[256];
        size_t first = 44 + 44100 * 4;
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_wav_n);
        if (!send_str(s, head) && !send_all(s, g_wav, first)) {
            int64_t until = fa_monotonic_ns() + 5000000000LL;
            while (fa_monotonic_ns() < until && !fa_load32(&g_stop)) fa_sleep_ms(10);
            send_all(s, g_wav + first, g_wav_n - first);
        }
    }
    else if (strcmp(path, "/live-close") == 0) {
        /* A live stream with no length and no chunked framing: the first connection
         * sends half the MP3 and closes cleanly, the second all of it, then the
         * stream is gone (404). */
        int k = fa_fetch_add32(&g_live_served, 1);
        if (k >= 2) send_str(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        else if (!send_str(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: close\r\n\r\n"))
            send_all(s, g_mp3, k == 0 ? g_mp3_n / 2 : g_mp3_n);
    }
    else if (strcmp(path, "/redirect") == 0)
        send_str(s, "HTTP/1.1 302 Found\r\nLocation: /tone.wav\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    else if (strcmp(path, "/station.pls") == 0) {
        char body[256], head[256];
        snprintf(body, sizeof(body), "[playlist]\nNumberOfEntries=1\nFile1=http://127.0.0.1:%d/radio\nTitle1=Test FM\n", g_port);
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: audio/x-scpls\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
        if (!send_str(s, head)) send_str(s, body);
    }
    else if (strcmp(path, "/radio") == 0) serve_radio(s, req, g_mp3, g_mp3_n, "audio/mpeg", 128);
    else if (strcmp(path, "/oggradio") == 0) serve_ogg_radio(s);
    else if (strcmp(path, "/radio.aac") == 0) serve_radio(s, req, g_aac, g_aac_n, "audio/aacp", 64);
    else if (strcmp(path, "/tone.aac") == 0) serve_bytes(s, req, g_aac, g_aac_n, "audio/aac", 1, 0);
    else if (strcmp(path, "/tagged.aac") == 0) serve_bytes(s, req, g_aac_tagged, g_aac_tagged_n, "audio/aac", 1, 0);
    else if (strcmp(path, "/tone.wma") == 0) serve_bytes(s, req, g_wma, g_wma_n, "audio/x-ms-wma", 1, 0);
    else if (strcmp(path, "/noranges.wma") == 0) serve_bytes(s, req, g_wma, g_wma_n, "audio/x-ms-wma", 0, 0);
    else if (strcmp(path, "/pcm.asf") == 0) serve_bytes(s, req, g_asf_pcm, g_asf_pcm_n, "video/x-ms-asf", 1, 0);
    else if (strcmp(path, "/chunked.asf") == 0) serve_chunked(s, g_asf_pcm, g_asf_pcm_n);
    else if (strncmp(path, "/hls/", 5) == 0) serve_hls(s, req, path);
    else send_str(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found");
    return 0;
}

static void accept_main(void* arg)
{
    (void)arg;
    while (!fa_load32(&g_stop)) {
        sock_t c = accept(g_listen, NULL, NULL);
        conn_arg* p;
        if (c == BAD_SOCK) continue;
        if (fa_load32(&g_stop)) { close_sock(c); break; }
        p = (conn_arg*)malloc(sizeof(*p));
        p->s = c;
        fa_mutex_lock(&g_conn_lock);
        p->id = g_conn_count + 1;
        if (g_conn_count < MAX_CONNS && fa_thread_create(&g_conn_threads[g_conn_count], handle, p) == 0)
            g_conns[g_conn_count++] = c;
        else { close_sock(c); free(p); }
        fa_mutex_unlock(&g_conn_lock);
    }
}

static int server_start(void)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int one = 1;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == BAD_SOCK) return -1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(g_listen, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(g_listen, 64) != 0) return -1;
    if (getsockname(g_listen, (struct sockaddr*)&a, &len) != 0) return -1;
    g_port = ntohs(a.sin_port);
    fa_mutex_init(&g_conn_lock);
    return fa_thread_create(&g_accept_thread, accept_main, NULL);
}

static void server_stop(void)
{
    int i;
    sock_t wake;
    struct sockaddr_in a;
    fa_store32(&g_stop, 1);
    /* Wake accept() with one last connection. */
    wake = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)g_port);
    connect(wake, (struct sockaddr*)&a, sizeof(a));
    fa_thread_join(&g_accept_thread);
    close_sock(wake);
    for (i = 0; i < g_conn_count; i++) shutdown(g_conns[i], SHUT_BOTH);
    for (i = 0; i < g_conn_count; i++) { fa_thread_join(&g_conn_threads[i]); close_sock(g_conns[i]); }
    close_sock(g_listen);
    fa_mutex_destroy(&g_conn_lock);
}

/* Android runs the URL tests a second time over HTTPS (net_tls_android.c). */
static const char* g_scheme = "http";
static int g_url_port;

static void url_of(char* out, size_t cap, const char* path) { snprintf(out, cap, "%s://127.0.0.1:%d%s", g_scheme, g_url_port, path); }

/* ---- helpers ------------------------------------------------------------------- */

static fa_stream* open_url(const char* path, uint32_t flags, fa_result* out_r)
{
    char url[256];
    fa_open_options o;
    fa_net_options net;
    fa_stream* s = NULL;
    fa_result r;
    url_of(url, sizeof(url), path);
    fa_open_options_init(&o);
    fa_net_options_init(&net);
    net.timeout_ms = 3000;
    net.proxy = "";                    /* never through a proxy of the machine running the test */
    o.flags = flags;
    o.net = &net;
    r = fa_stream_open_url(g_engine, url, &o, &s);
    if (out_r) *out_r = r;
    return r == FA_OK ? s : NULL;
}

/* Decodes all of `s` (decode-only) into a malloc'd buffer. */
static float* read_all(fa_stream* s, fa_frames* out_frames)
{
    fa_stream_info info;
    size_t cap = 1 << 16, n = 0;
    float* buf;
    fa_stream_get_info(s, &info);
    buf = (float*)malloc(cap * info.format.channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        if (n + 4096 > cap) { cap *= 2; buf = (float*)realloc(buf, cap * info.format.channels * sizeof(float)); }
        fa_stream_read(s, buf + n * info.format.channels, 4096, &got);
        if (got == 0) break;
        n += (size_t)got;
    }
    *out_frames = (fa_frames)n;
    return buf;
}

static float* read_all_memory(const uint8_t* data, size_t size, fa_frames* out_frames)
{
    fa_stream* s = NULL;
    fa_open_options o;
    float* buf;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    if (fa_stream_open_memory(g_engine, data, size, &o, &s) != FA_OK) return NULL;
    buf = read_all(s, out_frames);
    fa_stream_close(s);
    return buf;
}

static float wav_left(fa_frames i) { return (float)((int)(i % 8192) * 4 - 16384) / 32768.0f; }

/* ---- unit tests ------------------------------------------------------------------ */

static void test_url_parse(void)
{
    fa_url u;
    CHECK_EQ(fa_url_parse("http://example.com", &u), FA_OK);
    CHECK_STR(u.host, "example.com");
    CHECK_EQ(u.port, 80);
    CHECK_STR(u.path, "/");
    CHECK_EQ(fa_url_parse("HTTPS://user:pw@Example.com:8443/a/b.mp3?x=1#frag", &u), FA_OK);
    CHECK_STR(u.scheme, "https");
    CHECK_STR(u.userinfo, "user:pw");
    CHECK_EQ(u.port, 8443);
    CHECK_STR(u.path, "/a/b.mp3?x=1");
    CHECK_EQ(fa_url_parse("http://[::1]:8000/live", &u), FA_OK);
    CHECK_STR(u.host, "::1");
    CHECK_EQ(u.port, 8000);
    CHECK_EQ(fa_url_parse("ftp://example.com/x", &u), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_url_parse("/relative", &u), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_url_parse("http://", &u), FA_ERROR_INVALID_ARG);
    /* Proxy settings: the same host and port for every form. */
    CHECK_EQ(fa_proxy_parse("proxy.example:3128", &u), FA_OK);
    CHECK_STR(u.host, "proxy.example");
    CHECK_EQ(u.port, 3128);
    CHECK_EQ(fa_proxy_parse("http://user:pw@proxy.example/", &u), FA_OK);
    CHECK_STR(u.host, "proxy.example");
    CHECK_EQ(u.port, 80);
    CHECK_EQ(fa_proxy_parse("http://[::1]:8080/x", &u), FA_OK);
    CHECK_STR(u.host, "::1");
    CHECK_EQ(u.port, 8080);
    CHECK_EQ(fa_proxy_parse("socks5://proxy.example:1080", &u), FA_ERROR_INVALID_ARG);
}

static void test_url_resolve(void)
{
    char out[FA_NET_URL_MAX];
    const char* base = "http://a.com/b/c/d.m3u8?q=1";
    CHECK_EQ(fa_url_resolve(base, "seg1.ts", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "http://a.com/b/c/seg1.ts");
    CHECK_EQ(fa_url_resolve(base, "../x/seg.ts", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "http://a.com/b/x/seg.ts");
    CHECK_EQ(fa_url_resolve(base, "/root.ts", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "http://a.com/root.ts");
    CHECK_EQ(fa_url_resolve(base, "//cdn.com/k.key", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "http://cdn.com/k.key");
    CHECK_EQ(fa_url_resolve(base, "https://o.com/p", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "https://o.com/p");
    CHECK_EQ(fa_url_resolve(base, "?r=2", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "http://a.com/b/c/d.m3u8?r=2");
    CHECK_EQ(fa_url_resolve(base, "./a/./b/../c.ts", out, sizeof(out)), FA_OK);
    CHECK_STR(out, "http://a.com/b/c/a/c.ts");
}

static void test_http_header_lookup(void)
{
    char v[64];
    const char* h = "Content-Type: audio/mpeg\r\nicy-name:  My Station \r\nX-Empty:\r\n";
    CHECK(fa_http_header(h, "content-type", v, sizeof(v)) != NULL);
    CHECK_STR(v, "audio/mpeg");
    CHECK(fa_http_header(h, "ICY-NAME", v, sizeof(v)) != NULL);
    CHECK_STR(v, "My Station");
    CHECK(fa_http_header(h, "X-Empty", v, sizeof(v)) != NULL);
    CHECK_STR(v, "");
    CHECK(fa_http_header(h, "icy-br", v, sizeof(v)) == NULL);
}

static void test_hls_parse(void)
{
    const char* master =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English\",DEFAULT=YES,URI=\"audio/en.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"avc1.64001f,mp4a.40.2\",AUDIO=\"aud\"\n"
        "hi/video.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.64001f,mp4a.40.2\",AUDIO=\"aud\"\n"
        "lo/video.m3u8\n";
    const char* media =
        "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV=0x000102030405060708090a0b0c0d0e0f\n"
        "#EXTINF:6.0,\nseg100.ts\n"
        "#EXT-X-BYTERANGE:1000@500\n#EXTINF:5.5,title\nall.ts\n"
        "#EXT-X-KEY:METHOD=NONE\n#EXTINF:4.0,\nseg102.ts\n#EXT-X-ENDLIST\n";
    fa_hls_playlist p;
    CHECK_EQ(fa_hls_parse(master, strlen(master), "http://h.com/m/master.m3u8", &p), FA_OK);
    CHECK(p.is_master);
    CHECK_EQ(p.variant_count, 2);
    CHECK_EQ(p.rendition_count, 1);
    CHECK_STR(fa_hls_audio_uri(&p, 0), "http://h.com/m/audio/en.m3u8");
    CHECK_EQ(fa_hls_pick_variant(&p, 0), 0);
    CHECK_EQ(fa_hls_pick_variant(&p, 1000000), 1);
    fa_hls_playlist_free(&p);
    /* Tiered audio groups: the rendition comes from the chosen variant's group (its
     * DEFAULT, else AUTOSELECT, else the first); a rendition without a URI, or a
     * variant without a group, plays the variant's own playlist. */
    {
        const char* tiered =
            "#EXTM3U\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hi\",NAME=\"Stereo\",AUTOSELECT=YES,URI=\"hi/stereo.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hi\",NAME=\"Main\",DEFAULT=YES,URI=\"hi/main.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lo\",NAME=\"Talk\",URI=\"lo/talk.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lo\",NAME=\"Main\",AUTOSELECT=YES,URI=\"lo/main.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"mux\",NAME=\"Main\",DEFAULT=YES\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS=\"avc1.64001f,mp4a.40.2\",AUDIO=\"hi\"\n"
            "v/hi.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=600000,CODECS=\"avc1.64001f,mp4a.40.5\",AUDIO=\"lo\"\n"
            "v/lo.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=300000,CODECS=\"avc1.64001f,mp4a.40.5\",AUDIO=\"mux\"\n"
            "v/mux.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=200000,CODECS=\"avc1.64001f,mp4a.40.5\"\n"
            "v/plain.m3u8\n";
        const char* only_media = "#EXTM3U\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"One\",URI=\"one.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"b\",NAME=\"Two\",DEFAULT=YES,URI=\"two.m3u8\"\n";
        int v;
        CHECK_EQ(fa_hls_parse(tiered, strlen(tiered), "http://h.com/m/master.m3u8", &p), FA_OK);
        CHECK_EQ(p.rendition_count, 5);
        v = fa_hls_pick_variant(&p, 0);
        CHECK_EQ(v, 0);
        CHECK_STR(fa_hls_audio_uri(&p, v), "http://h.com/m/hi/main.m3u8");
        v = fa_hls_pick_variant(&p, 1000000);
        CHECK_EQ(v, 1);
        CHECK_STR(fa_hls_audio_uri(&p, v), "http://h.com/m/lo/main.m3u8");
        v = fa_hls_pick_variant(&p, 400000);
        CHECK_EQ(v, 2);
        CHECK(fa_hls_audio_uri(&p, v) == NULL);
        CHECK(fa_hls_audio_uri(&p, 3) == NULL);
        fa_hls_playlist_free(&p);
        CHECK_EQ(fa_hls_parse(only_media, strlen(only_media), "http://h.com/m/master.m3u8", &p), FA_OK);
        CHECK_EQ(p.variant_count, 0);
        CHECK_STR(fa_hls_audio_uri(&p, fa_hls_pick_variant(&p, 0)), "http://h.com/m/two.m3u8");
        fa_hls_playlist_free(&p);
    }

    CHECK_EQ(fa_hls_parse(media, strlen(media), "http://h.com/m/a.m3u8", &p), FA_OK);
    CHECK(!p.is_master);
    CHECK(p.endlist);
    CHECK_EQ(p.media_sequence, 100);
    CHECK_EQ(p.segment_count, 3);
    CHECK_STR(p.segments[0].uri, "http://h.com/m/seg100.ts");
    CHECK_EQ(p.segments[0].key_method, 1);
    CHECK_STR(p.segments[0].key_uri, "http://h.com/m/key.bin");
    CHECK(p.segments[0].has_iv);
    CHECK_EQ(p.segments[0].iv[15], 0x0f);
    CHECK_EQ(p.segments[1].byte_offset, 500);
    CHECK_EQ(p.segments[1].byte_length, 1000);
    CHECK_EQ(p.segments[1].key_method, 1);
    CHECK_EQ(p.segments[2].key_method, 0);
    CHECK(fabs(p.segments[1].duration - 5.5) < 1e-9);
    fa_hls_playlist_free(&p);
    /* EXT-X-MAP with a BYTERANGE (offset 0 when left out); a malformed one is refused. */
    {
        const char* fmp4 = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MAP:URI=\"main.mp4\",BYTERANGE=\"720@16\"\n"
                           "#EXT-X-BYTERANGE:5000@736\n#EXTINF:6.0,\nmain.mp4\n"
                           "#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"64\"\n#EXTINF:6.0,\nb.m4s\n"
                           "#EXT-X-MAP:URI=\"whole.mp4\"\n#EXTINF:6.0,\nc.m4s\n#EXT-X-ENDLIST\n";
        const char* bad = "#EXTM3U\n#EXT-X-MAP:URI=\"main.mp4\",BYTERANGE=\"720@x\"\n#EXTINF:6.0,\nmain.mp4\n#EXT-X-ENDLIST\n";
        CHECK_EQ(fa_hls_parse(fmp4, strlen(fmp4), "http://h.com/m/a.m3u8", &p), FA_OK);
        CHECK_EQ(p.segment_count, 3);
        CHECK_STR(p.segments[0].map_uri, "http://h.com/m/main.mp4");
        CHECK_EQ(p.segments[0].map_offset, 16);
        CHECK_EQ(p.segments[0].map_length, 720);
        CHECK_EQ(p.segments[0].byte_offset, 736);
        CHECK_EQ(p.segments[1].map_offset, 0);
        CHECK_EQ(p.segments[1].map_length, 64);
        CHECK_EQ(p.segments[2].map_length, -1);
        fa_hls_playlist_free(&p);
        CHECK_EQ(fa_hls_parse(bad, strlen(bad), "http://h.com/m/a.m3u8", &p), FA_ERROR_UNSUPPORTED_FORMAT);
    }
    /* SAMPLE-AES and DRM key formats are protected content. */
    {
        const char* drm = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
                          "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://key\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
                          "#EXTINF:6.0,\nseg.ts\n#EXT-X-ENDLIST\n";
        CHECK_EQ(fa_hls_parse(drm, strlen(drm), "http://h.com/m/a.m3u8", &p), FA_ERROR_PROTECTED_CONTENT);
    }
}

static void test_aes_vectors(void)
{
    /* FIPS-197 appendix C.1 (one block, so CBC with a zero IV is plain AES). */
    uint8_t key[16], iv[16], data[64];
    static const uint8_t ct1[16] = { 0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    /* NIST SP 800-38A F.2.2, CBC-AES128 decryption. */
    static const uint8_t key2[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
    static const uint8_t ct2[64] = {
        0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,
        0x50,0x86,0xcb,0x9b,0x50,0x72,0x19,0xee,0x95,0xdb,0x11,0x3a,0x91,0x76,0x78,0xb2,
        0x73,0xbe,0xd6,0xb8,0xe3,0xc1,0x74,0x3b,0x71,0x16,0xe6,0x9e,0x22,0x22,0x95,0x16,
        0x3f,0xf1,0xca,0xa1,0x68,0x1f,0xac,0x09,0x12,0x0e,0xca,0x30,0x75,0x86,0xe1,0xa7 };
    static const uint8_t pt2[64] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10 };
    int i;
    for (i = 0; i < 16; i++) { key[i] = (uint8_t)i; iv[i] = 0; }
    memcpy(data, ct1, 16);
    fa_aes128_cbc_decrypt(key, iv, data, 16);
    for (i = 0; i < 16; i++) CHECK_EQ(data[i], (uint8_t)(i * 0x11));
    for (i = 0; i < 16; i++) iv[i] = (uint8_t)i;
    memcpy(data, ct2, 64);
    fa_aes128_cbc_decrypt(key2, iv, data, 64);
    CHECK(memcmp(data, pt2, 64) == 0);
}

/* The byte-wise AES that the table version replaced (FIPS-197 5.3 as written),
 * kept as a reference: random keys, IVs and lengths must decrypt the same. */
static uint8_t ref_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t ref_gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = ref_xtime(a); b >>= 1; }
    return r;
}

static uint8_t ref_sbox_at(uint8_t x)
{
    /* The S-box from its definition: the inverse in GF(2^8), then the affine map. */
    uint8_t inv = 0, r;
    int i;
    if (x) for (i = 1; i < 256; i++) if (ref_gmul(x, (uint8_t)i) == 1) { inv = (uint8_t)i; break; }
    r = inv;
    for (i = 1; i < 5; i++) r ^= (uint8_t)((inv << i) | (inv >> (8 - i)));
    return (uint8_t)(r ^ 0x63);
}

static void ref_aes_cbc(const uint8_t key[16], uint8_t iv[16], uint8_t* data, size_t n)
{
    uint8_t sbox[256], inv[256], rk[176], rcon = 1;
    size_t b;
    int i;
    for (i = 0; i < 256; i++) { sbox[i] = ref_sbox_at((uint8_t)i); inv[sbox[i]] = (uint8_t)i; }
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t x = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
            rcon = ref_xtime(rcon);
        }
        rk[i] = rk[i - 16] ^ t[0]; rk[i + 1] = rk[i - 15] ^ t[1]; rk[i + 2] = rk[i - 14] ^ t[2]; rk[i + 3] = rk[i - 13] ^ t[3];
    }
    for (b = 0; b + 16 <= n; b += 16) {
        uint8_t* s = data + b;
        uint8_t prev[16];
        int round;
        memcpy(prev, s, 16);
        for (i = 0; i < 16; i++) s[i] ^= rk[160 + i];
        for (round = 9; round >= 0; round--) {
            uint8_t t[16];
            for (i = 0; i < 16; i++) t[(((i / 4) + (i % 4)) % 4) * 4 + i % 4] = inv[s[i]];
            for (i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
            if (round == 0) break;
            for (i = 0; i < 4; i++) {
                uint8_t a = s[i * 4], c1 = s[i * 4 + 1], c2 = s[i * 4 + 2], d = s[i * 4 + 3];
                s[i * 4]     = ref_gmul(a, 14) ^ ref_gmul(c1, 11) ^ ref_gmul(c2, 13) ^ ref_gmul(d, 9);
                s[i * 4 + 1] = ref_gmul(a, 9) ^ ref_gmul(c1, 14) ^ ref_gmul(c2, 11) ^ ref_gmul(d, 13);
                s[i * 4 + 2] = ref_gmul(a, 13) ^ ref_gmul(c1, 9) ^ ref_gmul(c2, 14) ^ ref_gmul(d, 11);
                s[i * 4 + 3] = ref_gmul(a, 11) ^ ref_gmul(c1, 13) ^ ref_gmul(c2, 9) ^ ref_gmul(d, 14);
            }
        }
        for (i = 0; i < 16; i++) s[i] ^= iv[i];
        memcpy(iv, prev, 16);
    }
}

static void test_aes_reference(void)
{
    uint32_t rng = 0x2545F491u;
    int run;
    for (run = 0; run < 300; run++) {
        uint8_t key[16], iv_a[16], iv_b[16], a[200], b[200];
        size_t n, i;
        for (i = 0; i < 16; i++) {
            rng = rng * 1664525u + 1013904223u; key[i] = (uint8_t)(rng >> 24);
            rng = rng * 1664525u + 1013904223u; iv_a[i] = iv_b[i] = (uint8_t)(rng >> 24);
        }
        rng = rng * 1664525u + 1013904223u;
        n = (rng >> 16) % sizeof(a);   /* a partial last block stays as it is */
        for (i = 0; i < n; i++) { rng = rng * 1664525u + 1013904223u; a[i] = b[i] = (uint8_t)(rng >> 24); }
        fa_aes128_cbc_decrypt(key, iv_a, a, n);
        ref_aes_cbc(key, iv_b, b, n);
        CHECK(memcmp(a, b, n) == 0);
        CHECK(memcmp(iv_a, iv_b, 16) == 0);
    }
}

typedef struct collect { uint8_t* p; size_t n, cap; } collect;
static int collect_out(void* user, const uint8_t* p, size_t n)
{
    collect* c = (collect*)user;
    if (c->n + n > c->cap) { c->cap = (c->n + n) * 2; c->p = (uint8_t*)realloc(c->p, c->cap); }
    memcpy(c->p + c->n, p, n);
    c->n += n;
    return 0;
}

static void test_ts_demux(void)
{
    fa_ts_demux d;
    collect c = { NULL, 0, 0 };
    size_t off = 0;
    CHECK(g_seg[0].n > 0);
    fa_ts_init(&d);
    /* Odd-sized pieces, so packets are split across calls. */
    while (off < g_seg[0].n) {
        size_t k = g_seg[0].n - off < 1000 ? g_seg[0].n - off : 1000;
        fa_ts_feed(&d, g_seg[0].p + off, k, collect_out, &c);
        off += k;
    }
    CHECK_EQ(d.stream_type, 0x03);
    CHECK(c.n > 0);
    CHECK(memcmp(c.p, g_mp3, c.n) == 0);
    free(c.p);
}

/* ---- HTTP transport ---------------------------------------------------------- */

typedef struct fetch_result { int status; char type[64]; size_t bytes; } fetch_result;
static int fr_head(void* user, const fa_http_response* r)
{
    fetch_result* f = (fetch_result*)user;
    f->status = r->status;
    fa_http_header(r->headers, "Content-Type", f->type, sizeof(f->type));
    return 0;
}
static int fr_data(void* user, const uint8_t* p, size_t n) { (void)p; ((fetch_result*)user)->bytes += n; return 0; }

static fa_result fetch(const char* path, fetch_result* f)
{
    char url[256];
    fa_http_request req;
    memset(&req, 0, sizeof(req));
    memset(f, 0, sizeof(*f));
    url_of(url, sizeof(url), path);
    req.url = url;
    req.range_start = -1;
    req.user_agent = "test";
    req.proxy = "";
    req.timeout_ms = 3000;
    return fa_http_fetch(&req, fr_head, fr_data, f);
}

/* ---- request headers ------------------------------------------------------------ */

/* Collects fa_http_each_header's output as "name=value|". */
typedef struct walk_text { char text[1024]; int fail_at, calls; } walk_text;
static int walk_add(void* user, const char* name, const char* value)
{
    walk_text* w = (walk_text*)user;
    size_t n = strlen(w->text);
    if (++w->calls == w->fail_at) return 1;
    snprintf(w->text + n, sizeof(w->text) - n, "%s=%s|", name, value);
    return 0;
}

/* The header walk on odd app input: bare LF, blank lines, a colon in the value,
 * lines without a name or colon, control characters, a line over the limit, and
 * app headers that replace ours or that belong to the transport. */
static void test_request_header_walk(void)
{
    static char headers[FA_HTTP_LINE_MAX + 512];
    fa_http_request req;
    fa_url u;
    walk_text w;
    size_t n;
    memset(&req, 0, sizeof(req));
    req.url = "https://user:p%20w@example.com/a";
    req.range_start = -1;
    req.user_agent = "fa-test/1";
    CHECK_EQ(fa_url_parse(req.url, &u), FA_OK);

    /* Ours only. */
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
    CHECK_STR(w.text, "User-Agent=fa-test/1|Accept=*/*|Accept-Encoding=identity|");
    req.icy = 1;
    req.range_start = 1234567890123LL;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, &u, walk_add, &w), FA_OK);
    CHECK_STR(w.text, "User-Agent=fa-test/1|Accept=*/*|Accept-Encoding=identity|Icy-MetaData=1|"
                      "Range=bytes=1234567890123-|Authorization=Basic dXNlcjpwIHc=|");

    /* The app's lines. */
    n = (size_t)snprintf(headers, sizeof(headers),
                         "X-One: a\n\nX-Two: \t b c \t\r\n\r\n no colon\n: no name\nBad Name: x\nX-Colon:a:b\n"
                         "accept: audio/*\nHost: evil\nconnection: keep-alive\nContent-Length: 5\nTransfer-Encoding: chunked\n"
                         "X-Ctl: a\x01" "b\nX-Del: a\x7f\nX-Empty:\nX-Latin: caf\xe9\nX-Utf8: caf\xc3\xa9\nX-Name : v\n"
                         "X-Long: ");
    memset(headers + n, 'x', FA_HTTP_LINE_MAX - 7);   /* "X-Long: " + this = one byte over */
    snprintf(headers + n + FA_HTTP_LINE_MAX - 7, sizeof(headers) - n - FA_HTTP_LINE_MAX + 7, "\r\nX-Last: z");
    req.headers = headers;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, &u, walk_add, &w), FA_OK);
    CHECK_STR(w.text, "User-Agent=fa-test/1|Accept-Encoding=identity|Icy-MetaData=1|Range=bytes=1234567890123-|"
                      "Authorization=Basic dXNlcjpwIHc=|X-One=a|X-Two=b c|X-Colon=a:b|accept=audio/*|X-Empty=|"
                      "X-Name=v|X-Last=z|");
    /* A line exactly at the limit is kept. */
    headers[n + FA_HTTP_LINE_MAX - 8] = '\r';
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
    CHECK(strstr(w.text, "X-Long=xxx") != NULL);

    /* After a redirect to another origin: no Authorization or Cookie of the app. A
     * Range with a length. */
    req.headers = "Authorization: Bearer t\nCookie: a=b\nX-Keep: 1";
    req.range_start = 100;
    req.range_length = 50;
    req.icy = 0;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
    CHECK_STR(w.text, "User-Agent=fa-test/1|Accept=*/*|Accept-Encoding=identity|Range=bytes=100-149|"
                      "Authorization=Bearer t|Cookie=a=b|X-Keep=1|");
    req.other_origin = 1;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
    CHECK_STR(w.text, "User-Agent=fa-test/1|Accept=*/*|Accept-Encoding=identity|Range=bytes=100-149|X-Keep=1|");
    req.other_origin = 0;

    /* A user agent with a line break is not sent; the sink failing stops the walk. */
    req.headers = NULL;
    req.user_agent = "evil\r\nX-Injected: 1";
    memset(&w, 0, sizeof(w));
    CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
    CHECK(strstr(w.text, "evil") == NULL);
    memset(&w, 0, sizeof(w));
    w.fail_at = 2;
    CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_ERROR_OUT_OF_MEMORY);
    CHECK_EQ(w.calls, 2);
}

/* The body of a GET, NUL-terminated (malloc'd). */
typedef struct body_text { char* p; size_t n; } body_text;
static int body_add(void* user, const uint8_t* p, size_t n)
{
    body_text* b = (body_text*)user;
    char* grown = (char*)realloc(b->p, b->n + n + 1);
    if (!grown) return 1;
    memcpy(grown + b->n, p, n);
    b->p = grown;
    b->n += n;
    b->p[b->n] = 0;
    return 0;
}

/* How often header `name` is in `lines` ("Name: value\r\n"); its last value into out. */
static int count_header(const char* lines, const char* name, char* out, size_t cap)
{
    size_t ln = strlen(name);
    int count = 0;
    if (out && cap) out[0] = 0;
    while (*lines) {
        size_t len = strcspn(lines, "\r\n");
        if (len > ln && lines[ln] == ':' && fa_strncasecmp_ascii(lines, name, ln) == 0) {
            const char* v = lines + ln + 1;
            while (*v == ' ') v++;
            count++;
            if (out) snprintf(out, cap, "%.*s", (int)(lines + len - v), v);
        }
        lines += len;
        while (*lines == '\r' || *lines == '\n') lines++;
    }
    return count;
}

/* Fetches `url` (a server answering with the request's header lines) with every
 * kind of header, and checks what the server got: each header once, the app's
 * lines trimmed, the invalid ones dropped. The same on every transport. */
static void check_request_headers(const char* url)
{
    static char headers[6000];
    fa_http_request req;
    body_text body = { NULL, 0 };
    char v[4100];
    size_t n;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.range_start = 5;
    req.icy = 1;
    req.user_agent = "fa-test/1";
    req.timeout_ms = 5000;
    req.proxy = "";
    n = (size_t)snprintf(headers, sizeof(headers), "X-One: a\nX-Two: \t b c \t\r\n\r\nno colon\nBad Name: x\nX-Colon: a:b\n"
                                                   "Accept: audio/*\nHost: evil\nX-Ctl: a\x01" "b\nX-Latin: caf\xe9\nX-Empty:\nX-Big: ");
    memset(headers + n, 'y', 4000);
    snprintf(headers + n + 4000, sizeof(headers) - n - 4000, "\r\n");
    req.headers = headers;
    CHECK_EQ(fa_http_fetch(&req, NULL, body_add, &body), FA_OK);
    CHECK(body.p != NULL);
    if (!body.p) return;
    printf("  %zu bytes of request headers\n", body.n);
#define EXPECT_ONE(name, value) do { CHECK_EQ(count_header(body.p, name, v, sizeof(v)), 1); CHECK_STR(v, value); } while (0)
    EXPECT_ONE("User-Agent", "fa-test/1");
    EXPECT_ONE("Accept", "audio/*");
    EXPECT_ONE("Accept-Encoding", "identity");
    EXPECT_ONE("Icy-MetaData", "1");
    EXPECT_ONE("Range", "bytes=5-");
    EXPECT_ONE("Authorization", "Basic dXNlcjpwIHc=");
    EXPECT_ONE("X-One", "a");
    EXPECT_ONE("X-Two", "b c");
    EXPECT_ONE("X-Colon", "a:b");
#undef EXPECT_ONE
    CHECK_EQ(count_header(body.p, "Host", v, sizeof(v)), 1);
    CHECK(strcmp(v, "evil") != 0);
    CHECK_EQ(count_header(body.p, "X-Big", v, sizeof(v)), 1);
    CHECK_EQ(strlen(v), 4000);
    CHECK(count_header(body.p, "X-Empty", v, sizeof(v)) <= 1);   /* some stacks drop an empty header */
    CHECK_STR(v, "");
    CHECK_EQ(count_header(body.p, "X-Ctl", NULL, 0), 0);
    CHECK_EQ(count_header(body.p, "X-Latin", NULL, 0), 0);
    CHECK_EQ(count_header(body.p, "Bad Name", NULL, 0), 0);
    CHECK(strstr(body.p, "no colon") == NULL);
    free(body.p);
}

static void test_request_headers_sent(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s://user:p%%20w@127.0.0.1:%d/echo-headers", g_scheme, g_url_port);
    check_request_headers(url);
}

/* A GET through `session`: the status, the body, and which connection of the test
 * server carried it (X-Connection). With `cancel_on_data`, the first body bytes
 * raise the cancel flag instead. */
typedef struct conn_fetch {
    int status, conn;
    body_text body;
    int cancel_on_data;
    fa_atomic32 cancel;
} conn_fetch;

static int cf_head(void* user, const fa_http_response* r)
{
    conn_fetch* c = (conn_fetch*)user;
    char v[32];
    c->status = r->status;
    c->conn = fa_http_header(r->headers, "X-Connection", v, sizeof(v)) ? atoi(v) : 0;
    return 0;
}

static int cf_data(void* user, const uint8_t* p, size_t n)
{
    conn_fetch* c = (conn_fetch*)user;
    if (c->cancel_on_data) {
        fa_store32(&c->cancel, 1);
        fa_http_abort(&c->cancel);   /* as a seek or close on another thread would */
        return 0;
    }
    return body_add(&c->body, p, n);
}

static fa_result fetch_with(fa_http_session* session, const char* url, const char* headers, conn_fetch* c)
{
    fa_http_request req;
    int cancel_on_data = c->cancel_on_data;
    free(c->body.p);
    memset(c, 0, sizeof(*c));
    c->cancel_on_data = cancel_on_data;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.range_start = -1;
    req.user_agent = "fa-test/1";
    req.headers = headers;
    req.proxy = "";
    req.timeout_ms = 5000;
    req.cancel = &c->cancel;
    req.session = session;
    return fa_http_fetch(&req, cf_head, cf_data, c);
}

/* Redirects take the URL's credentials only back to the URL's own origin, and the
 * app's Authorization and Cookie lines only to it too. "localhost" is another
 * origin than "127.0.0.1" (the same server). */
static void test_redirect_credentials(void)
{
    char url[512];
    conn_fetch c;
    const char* sc = g_scheme;
    int port = g_url_port;
    memset(&c, 0, sizeof(c));
    /* The same origin, by an absolute Location without credentials: they stay. */
    snprintf(url, sizeof(url), "%s://user:p%%20w@127.0.0.1:%d/redirect?to=%s://127.0.0.1:%d/echo-headers", sc, port, sc, port);
    CHECK_EQ(fetch_with(NULL, url, "Cookie: a=b", &c), FA_OK);
    CHECK(c.body.p != NULL);
    CHECK_EQ(count_header(c.body.p, "Authorization", NULL, 0), 1);
    CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 1);
    /* Another origin: neither the URL's nor the app's. */
    snprintf(url, sizeof(url), "%s://user:p%%20w@127.0.0.1:%d/redirect?to=%s://localhost:%d/echo-headers", sc, port, sc, port);
    CHECK_EQ(fetch_with(NULL, url, "Cookie: a=b\r\nX-Keep: 1", &c), FA_OK);
    CHECK(c.body.p != NULL);
    CHECK_EQ(count_header(c.body.p, "Authorization", NULL, 0), 0);
    CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 0);
    CHECK_EQ(count_header(c.body.p, "X-Keep", NULL, 0), 1);
    snprintf(url, sizeof(url), "%s://127.0.0.1:%d/redirect?to=%s://localhost:%d/echo-headers", sc, port, sc, port);
    CHECK_EQ(fetch_with(NULL, url, "Authorization: Bearer t", &c), FA_OK);
    CHECK_EQ(count_header(c.body.p, "Authorization", NULL, 0), 0);
    /* Away and back again: the first origin gets them again. */
    snprintf(url, sizeof(url), "%s://user:p%%20w@127.0.0.1:%d/redirect?to=%s://localhost:%d/redirect?to=%s://127.0.0.1:%d/echo-headers",
             sc, port, sc, port, sc, port);
    CHECK_EQ(fetch_with(NULL, url, "Cookie: a=b", &c), FA_OK);
    CHECK_EQ(count_header(c.body.p, "Authorization", NULL, 0), 1);
    CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 1);
    free(c.body.p);
}

/* Requests through one session: over https they share a connection, plain http
 * opens one per request. A cancelled transfer leaves the session usable. */
static void test_session_reuse(void)
{
    char url[256];
    conn_fetch c;
    fa_http_session* session = fa_http_session_create();
    int first, i;
    CHECK(session != NULL);
    memset(&c, 0, sizeof(c));
    url_of(url, sizeof(url), "/keep-echo");
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
    first = c.conn;
    CHECK(first > 0);
    for (i = 0; i < 2; i++) {
        CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
        CHECK_EQ(c.status, 200);
        if (strcmp(g_scheme, "https") == 0) CHECK_EQ(c.conn, first);
        else CHECK(c.conn != first);
    }
    url_of(url, sizeof(url), "/tone.wav");
    c.cancel_on_data = 1;
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_ERROR_BUSY);
    c.cancel_on_data = 0;
    url_of(url, sizeof(url), "/keep-echo");
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
    CHECK_EQ(c.status, 200);
    free(c.body.p);
    fa_http_session_destroy(session);
}

static void test_http_fetch(void)
{
    fetch_result f;
    CHECK_EQ(fetch("/tone.wav", &f), FA_OK);
    CHECK_EQ(f.status, 200);
    CHECK_STR(f.type, "audio/wav");
    CHECK_EQ(f.bytes, g_wav_n);
    CHECK_EQ(fetch("/chunked.wav", &f), FA_OK);
    CHECK_EQ(f.bytes, g_wav_n);
    CHECK_EQ(fetch("/redirect", &f), FA_OK);
    CHECK_EQ(f.status, 200);
    CHECK_EQ(f.bytes, g_wav_n);
    CHECK_EQ(fetch("/nothing-here", &f), FA_ERROR_NOT_FOUND);
    CHECK_EQ(f.status, 404);
}

/* ---- URL streams ------------------------------------------------------------- */

static void test_wav_over_http(void)
{
    fa_result r;
    fa_stream* s = open_url("/tone.wav", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_net_status st;
    fa_frames n = 0, i;
    float* got;
    char v[64];
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, WAV_FRAMES);
    CHECK(info.seekable);
    CHECK_EQ(fa_stream_get_http_header(s, "content-type", v, sizeof(v)), FA_OK);
    CHECK_STR(v, "audio/wav");
    CHECK_EQ(fa_stream_get_http_header(s, "icy-name", v, sizeof(v)), FA_ERROR_NOT_FOUND);
    got = read_all(s, &n);
    CHECK_EQ(n, WAV_FRAMES);
    for (i = 0; i < n; i++) if (got[i * 2] != wav_left(i)) { CHECK_EQ(i, -1); }
    free(got);
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    CHECK_EQ(st.http_status, 206);
    CHECK(!st.is_live);
    CHECK(st.seekable);
    CHECK_EQ(st.content_length, (int64_t)g_wav_n);

    /* Seek back and forward; the data must match the frame index exactly. */
    {
        static const fa_frames targets[] = { 70000, 1000, 44100, 0 };
        size_t t;
        float buf[512 * 2];
        for (t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
            fa_frames rd = 0;
            CHECK_EQ(fa_stream_seek(s, targets[t]), FA_OK);
            CHECK_EQ(fa_stream_read(s, buf, 512, &rd), FA_OK);
            CHECK_EQ(rd, 512);
            for (i = 0; i < rd; i++) if (buf[i * 2] != wav_left(targets[t] + i)) { CHECK_EQ(targets[t] + i, -1); }
        }
    }
    fa_stream_close(s);
}

static void test_mp3_over_http(void)
{
    fa_result r;
    fa_stream* s = open_url("/tone.mp3", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_frames n = 0, ref_n = 0, i;
    float *got, *ref;
    double err = 0;
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "mp3");
    CHECK(info.seekable);
    /* No Xing header: the length is estimated from the size and bitrate. */
    CHECK(llabs((long long)info.length_frames - 132300) < 4410);
    got = read_all(s, &n);
    ref = read_all_memory(g_mp3, g_mp3_n, &ref_n);
    CHECK(ref != NULL);
    CHECK_EQ(n, ref_n);
    for (i = 0; i < n * 2; i++) err += fabs(got[i] - ref[i]);
    CHECK(err / (double)(n * 2) < 1e-6);
    free(got);
    free(ref);
    /* A seek lands near the target (MP3 over the network seeks by estimated byte). */
    {
        fa_frames pos = 0, rd = 0;
        float buf[1024 * 2];
        CHECK_EQ(fa_stream_seek(s, 88200), FA_OK);
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        CHECK_EQ(pos, 88200);
        CHECK_EQ(fa_stream_read(s, buf, 1024, &rd), FA_OK);
        CHECK_EQ(rd, 1024);
    }
    fa_stream_close(s);

    /* With a Xing header the length is exact, as for the local file. */
    s = open_url("/xing.mp3", FA_OPEN_DECODE_ONLY, &r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    ref = read_all_memory(g_mp3x, g_mp3x_n, &ref_n);
    free(ref);
    CHECK_EQ(info.length_frames, ref_n);
    got = read_all(s, &n);
    free(got);
    CHECK_EQ(n, ref_n);
    fa_stream_close(s);
}

static void test_no_ranges_and_chunked(void)
{
    fa_result r;
    fa_stream_info info;
    fa_frames n = 0, i;
    float* got;
    fa_stream* s = open_url("/noranges.wav", FA_OPEN_DECODE_ONLY, &r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    CHECK(!info.seekable);
    CHECK_EQ(fa_stream_seek(s, 1000), FA_ERROR_NOT_SEEKABLE);
    got = read_all(s, &n);
    CHECK_EQ(n, WAV_FRAMES);
    for (i = 0; i < n; i++) if (got[i * 2] != wav_left(i)) { CHECK_EQ(i, -1); }
    free(got);
    fa_stream_close(s);

    s = open_url("/chunked.wav", FA_OPEN_DECODE_ONLY, &r);
    CHECK_EQ(r, FA_OK);
    got = read_all(s, &n);
    CHECK_EQ(n, WAV_FRAMES);
    for (i = 0; i < n; i++) if (got[i * 2] != wav_left(i)) { CHECK_EQ(i, -1); }
    free(got);
    fa_stream_close(s);
}

static void test_redirect_and_errors(void)
{
    fa_result r;
    fa_net_status st;
    char url[256];
    fa_stream* s = open_url("/redirect", FA_OPEN_DECODE_ONLY, &r);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    url_of(url, sizeof(url), "/tone.wav");
    CHECK_STR(st.url, url);
    fa_stream_close(s);

    CHECK(open_url("/missing.mp3", 0, &r) == NULL);
    CHECK_EQ(r, FA_ERROR_NOT_FOUND);
    {
        fa_stream* t = NULL;
        CHECK_EQ(fa_stream_open_url(g_engine, "ftp://example.com/a.mp3", NULL, &t), FA_ERROR_INVALID_ARG);
        CHECK_EQ(fa_stream_open_url(g_engine, "not a url", NULL, &t), FA_ERROR_INVALID_ARG);
        CHECK(t == NULL);
    }
    /* Nobody listening on port 1: refused, quickly. */
    {
        fa_stream* t = NULL;
        fa_open_options o;
        fa_net_options net;
        fa_open_options_init(&o);
        fa_net_options_init(&net);
        net.proxy = "";
        net.timeout_ms = 2000;
        net.flags = FA_NET_NO_RECONNECT;
        o.net = &net;
        r = fa_stream_open_url(g_engine, "http://127.0.0.1:1/x.mp3", &o, &t);
        CHECK_EQ(r, FA_ERROR_NETWORK);
    }
    /* Not a web stream. */
    {
        fa_stream* m = NULL;
        fa_metadata md;
        CHECK_EQ(fa_stream_open_memory(g_engine, g_wav, g_wav_n, NULL, &m), FA_OK);
        CHECK_EQ(fa_stream_get_net_status(m, &st), FA_ERROR_INVALID_ARG);
        CHECK_EQ(fa_stream_get_metadata(m, &md), FA_OK);
        CHECK_EQ(md.seq, 0);
        fa_stream_close(m);
    }
}

static void test_reconnect_resumes(void)
{
    fa_result r;
    fa_net_status st;
    fa_frames n = 0, i;
    float* got;
    fa_stream* s;
    fa_store32(&g_drop_served, 0);
    s = open_url("/drop.wav", FA_OPEN_DECODE_ONLY, &r);
    CHECK_EQ(r, FA_OK);
    got = read_all(s, &n);
    CHECK_EQ(n, WAV_FRAMES);
    for (i = 0; i < n; i++) if (got[i * 2] != wav_left(i)) { CHECK_EQ(i, -1); }
    free(got);
    /* The decoder stops at the end of the WAV data, which can be before the download
     * thread sees the connection end and marks the download finished. */
    {
        int64_t until = fa_monotonic_ns() + 5000000000LL;
        do {
            CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
            if (st.finished) break;
            fa_sleep_ms(5);
        } while (fa_monotonic_ns() < until);
    }
    CHECK(st.reconnects >= 1);
    CHECK(st.finished);
    fa_stream_close(s);
}

/* ---- radio --------------------------------------------------------------------- */

typedef struct meta_log {
    fa_mutex lock;
    int by_type[32];
    int order[64];           /* event types in arrival order */
    int order_count;
    int count;
    int64_t seq[16];
    fa_frames frame[16];
} meta_log;

static void FA_CALL on_event(const fa_event* ev, void* user)
{
    meta_log* m = (meta_log*)user;
    fa_mutex_lock(&m->lock);
    if ((int)ev->type >= 0 && (int)ev->type < 32) m->by_type[ev->type]++;
    if (m->order_count < 64) m->order[m->order_count++] = (int)ev->type;
    if (ev->type != FA_EVENT_METADATA) { fa_mutex_unlock(&m->lock); return; }
    if (m->count < 16) { m->seq[m->count] = ev->value; m->frame[m->count] = ev->frame; m->count++; }
    fa_mutex_unlock(&m->lock);
}

static int wait_events(meta_log* m, int n)
{
    int tries, c = 0;
    for (tries = 0; tries < 300; tries++) {
        fa_mutex_lock(&m->lock);
        c = m->count;
        fa_mutex_unlock(&m->lock);
        if (c >= n) break;
        fa_sleep_ms(10);
    }
    return c;
}

static void test_icy_radio_decode_only(void)
{
    fa_result r;
    fa_stream* s = open_url("/radio", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_metadata md;
    fa_net_status st;
    meta_log log;
    float buf[4096 * 2];
    fa_frames total = 0;
    double song2_at = (double)RADIO_SONG2 / (128000.0 / 8.0) * 44100.0;
    CHECK_EQ(r, FA_OK);
    memset(&log, 0, sizeof(log));
    fa_mutex_init(&log.lock);
    fa_stream_set_event_callback(s, on_event, &log);
    fa_stream_get_info(s, &info);
    CHECK(!info.seekable);
    CHECK_EQ(info.length_frames, -1);
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    CHECK(st.is_live);

    /* 1 s in: the first song. */
    while (total < 44100) { fa_frames got = 0; fa_stream_read(s, buf, 4096, &got); CHECK(got > 0); total += got; }
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.station, "Test FM");
    CHECK_STR(md.genre, "Testing");
    CHECK_EQ(md.bitrate_kbps, 128);
    /* 128 kbps with the default 1 s prebuffer: the smallest read-ahead, not 2 MB. */
    fa_mutex_lock(&s->net->lock);
    CHECK_EQ(s->net->ahead_limit, 256 * 1024);
    CHECK(s->net->cap < 512 * 1024);
    fa_mutex_unlock(&s->net->lock);
    CHECK_STR(md.content_type, "audio/mpeg");
    CHECK_STR(md.stream_title, "Artist A - Song 1");
    CHECK_STR(md.artist, "Artist A");
    CHECK_STR(md.title, "Song 1");
    CHECK_STR(md.url, "http://example.com/A");
    CHECK_EQ(md.seq, 1);
    CHECK(wait_events(&log, 1) >= 1);
    CHECK_EQ(log.seq[0], 1);

    /* 3 s in: the second song, announced where it starts. */
    while (total < 3 * 44100) { fa_frames got = 0; fa_stream_read(s, buf, 4096, &got); CHECK(got > 0); total += got; }
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.stream_title, "Artist B - Song 2");
    CHECK_EQ(md.seq, 2);
    CHECK(wait_events(&log, 2) >= 2);
    CHECK_EQ(log.seq[1], 2);
    printf("  song 2 announced at frame %lld (starts near %.0f)\n", (long long)log.frame[1], song2_at);
    /* Decode-only streams announce a title when it is read, within one read. */
    CHECK(fabs((double)log.frame[1] - song2_at) < 0.25 * 44100);
    fa_stream_close(s);
    fa_mutex_destroy(&log.lock);
}

/* ---- AAC (ADTS) ------------------------------------------------------------------- */

#define AAC_FRAMES (130 * 1024)   /* 131 frames less the priming */

/* Two decodes of the same AAC: the same audio, as a signal to noise ratio in dB
 * over the first `n` samples (any decoder: the system's on Windows, macOS, iOS and
 * Android, FFmpeg elsewhere). Where FFmpeg decodes, they are the same to the bit. */
static double snr_db(const float* got, const float* ref, size_t n)
{
    double sig = 0, err = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        sig += (double)ref[i] * ref[i];
        err += ((double)got[i] - ref[i]) * ((double)got[i] - ref[i]);
    }
    if (err == 0) return 200.0;
    return 10.0 * log10(sig / err);
}
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
#  define AAC_BIT_EXACT 0
#else
#  define AAC_BIT_EXACT 1
#endif

/* An .aac file on a web server: length and seeks estimated from the frame sizes,
 * every frame decoded. */
static void test_aac_over_http(void)
{
    fa_result r;
    fa_stream* s = open_url("/tone.aac", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_frames n = 0, ref_n = 0, pos = 0, rd = 0;
    float *got, *ref, buf[2048 * 2];
    double err = 0;
    int i;
    CHECK_EQ(r, FA_OK);
    if (!s) return;
    fa_stream_get_info(s, &info);
    CHECK_STR(info.container, "adts");
    CHECK_STR(info.codec, "aac");
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK(info.seekable);
    printf("  estimated length %lld frames (exact %d)\n", (long long)info.length_frames, AAC_FRAMES);
    CHECK(llabs((long long)info.length_frames - AAC_FRAMES) < AAC_FRAMES / 10);
    got = read_all(s, &n);
    ref = read_all_memory(g_aac, g_aac_n, &ref_n);
    CHECK_EQ(n, AAC_FRAMES);
    CHECK_EQ(ref_n, AAC_FRAMES);
    CHECK(snr_db(got, ref, AAC_FRAMES * 2) > 60.0);
    for (i = 0; i < AAC_FRAMES * 2 && i < n * 2; i++) err += fabs(got[i] - ref[i]);
    if (AAC_BIT_EXACT) CHECK(err < 1e-3);
    /* A seek by estimated byte position: lands on a frame boundary near the target. */
    CHECK_EQ(fa_stream_seek(s, 88200), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 88200);
    CHECK_EQ(fa_stream_read(s, buf, 2048, &rd), FA_OK);
    CHECK_EQ(rd, 2048);
    {
        /* The audio is the tone, from somewhere within a few frames of the target. */
        double best = 1e9;
        fa_frames at;
        for (at = 88200 - 8192; at <= 88200 + 8192; at++) {
            double e = 0;
            for (i = 0; i < 1024 * 2 && e < best; i++) e += fabs(buf[2048 + i] - ref[(size_t)(at + 1024) * 2 + i]);
            if (e < best) best = e;
        }
        printf("  after seek: summed difference %g\n", best);
        CHECK(best < 0.05);
    }
    /* Into the last frames: the last one has no header after it. */
    CHECK_EQ(fa_stream_seek(s, AAC_FRAMES - 1500), FA_OK);
    rd = 0;
    CHECK_EQ(fa_stream_read(s, buf, 2048, &rd), FA_OK);
    CHECK(rd > 0);
    free(got);
    free(ref);
    fa_stream_close(s);

    /* Cover art in front: the frames after it still give the length and seeks. */
    s = open_url("/tagged.aac", FA_OPEN_DECODE_ONLY, &r);
    CHECK_EQ(r, FA_OK);
    if (!s) return;
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "aac");
    CHECK(info.seekable);
    CHECK(llabs((long long)info.length_frames - AAC_FRAMES) < AAC_FRAMES / 10);
    got = read_all(s, &n);
    CHECK_EQ(n, AAC_FRAMES);
    CHECK_EQ(fa_stream_seek(s, 44100), FA_OK);
    rd = 0;
    CHECK_EQ(fa_stream_read(s, buf, 2048, &rd), FA_OK);
    CHECK_EQ(rd, 2048);
    free(got);
    fa_stream_close(s);
}

/* ASF over HTTP: with byte ranges seekable (the packets are searched by send time
 * over range requests), without them or chunked read straight through. */
static void check_asf_url(const char* path, const uint8_t* file, size_t file_n, int seekable, fa_frames seek_to)
{
    fa_result r;
    fa_stream* s = open_url(path, FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_frames n = 0, ref_n = 0, i, rd = 0;
    float *got, *ref, buf[1024 * 2];
    uint32_t ch;
#ifndef _WIN32
    if (strstr(path, ".wma")) {                     /* WMA decodes on Windows only */
        CHECK_EQ(r, FA_ERROR_UNSUPPORTED_FORMAT);
        if (s) fa_stream_close(s);
        return;
    }
#endif
    CHECK_EQ(r, FA_OK);
    if (!s) return;
    fa_stream_get_info(s, &info);
    ch = info.format.channels;
    CHECK_STR(info.container, "asf");
    CHECK_EQ(info.seekable, seekable);
    got = read_all(s, &n);
    ref = read_all_memory(file, file_n, &ref_n);
    CHECK(ref != NULL);
    CHECK_EQ(n, ref_n);
    for (i = 0; i < n * (fa_frames)ch; i++) if (got[i] != ref[i]) { CHECK_EQ(i, -1); }
    if (seekable) {
        CHECK_EQ(fa_stream_seek(s, seek_to), FA_OK);
        CHECK_EQ(fa_stream_read(s, buf, 1024, &rd), FA_OK);
        CHECK_EQ(rd, 1024);
        for (i = 0; i < 1024 * (fa_frames)ch; i++) if (buf[i] != ref[(size_t)seek_to * ch + (size_t)i]) { CHECK_EQ(i, -1); }
    } else {
        CHECK_EQ(fa_stream_seek(s, seek_to), FA_ERROR_NOT_SEEKABLE);
    }
    free(got);
    free(ref);
    fa_stream_close(s);
}

static void test_asf_over_http(void)
{
    check_asf_url("/pcm.asf", g_asf_pcm, g_asf_pcm_n, 1, 6543);
    check_asf_url("/chunked.asf", g_asf_pcm, g_asf_pcm_n, 0, 6543);
    check_asf_url("/tone.wma", g_wma, g_wma_n, 1, 44100);
    check_asf_url("/noranges.wma", g_wma, g_wma_n, 0, 44100);
}

/* AAC internet radio (audio/aacp): song titles where the songs are heard. */
static void test_aac_radio(void)
{
    fa_result r;
    fa_stream* s = open_url("/radio.aac", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_metadata md;
    meta_log log;
    float buf[4096 * 2];
    fa_frames total = 0;
    double song2_at = (double)RADIO_SONG2 / (double)g_aac_n * AAC_FRAMES;
    CHECK_EQ(r, FA_OK);
    if (!s) return;
    memset(&log, 0, sizeof(log));
    fa_mutex_init(&log.lock);
    fa_stream_set_event_callback(s, on_event, &log);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "aac");
    CHECK(!info.seekable);
    CHECK_EQ(info.length_frames, -1);
    while (total < song2_at + 44100) { fa_frames got = 0; fa_stream_read(s, buf, 4096, &got); CHECK(got > 0); if (!got) break; total += got; }
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.content_type, "audio/aacp");
    CHECK_EQ(md.bitrate_kbps, 64);
    CHECK_STR(md.stream_title, "Artist B - Song 2");
    CHECK(wait_events(&log, 2) >= 2);
    CHECK_EQ(log.seq[1], 2);
    printf("  song 2 announced at frame %lld (starts near %.0f)\n", (long long)log.frame[1], song2_at);
    CHECK(fabs((double)log.frame[1] - song2_at) < 0.25 * 44100);
    fa_stream_close(s);
    fa_mutex_destroy(&log.lock);
}

/* HLS with AAC in MPEG-TS segments. */
static void test_hls_aac(void)
{
    fa_result r;
    fa_stream* s = open_url("/hls/aac.m3u8", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_frames n = 0, ref_n = 0, pos = 0, rd = 0;
    float *got, *ref, buf[4096 * 2];
    double err = 0;
    int i;
    CHECK_EQ(r, FA_OK);
    if (!s) return;
    fa_stream_get_info(s, &info);
    CHECK_STR(info.container, "hls");
    CHECK_STR(info.codec, "aac");
    CHECK(info.seekable);
    CHECK_EQ(info.length_frames, AAC_FRAMES);
    got = read_all(s, &n);
    ref = read_all_memory(g_aac, g_aac_n, &ref_n);
    printf("  hls %lld frames, file %lld\n", (long long)n, (long long)ref_n);
    CHECK_EQ(n, ref_n);
    CHECK(n >= 44100 && snr_db(got, ref, 44100 * 2) > 60.0);
    for (i = 0; i < 44100 * 2 && i < n * 2; i++) err += fabs(got[i] - ref[i]);
    if (AAC_BIT_EXACT) CHECK(err < 1e-3);
    /* Into the last segment: the position is exact, the audio settles within a frame. */
    {
        fa_frames target = (fa_frames)((g_aseg_dur[0] + g_aseg_dur[1]) * 44100) + 3000;
        double e2 = 0;
        CHECK_EQ(fa_stream_seek(s, target), FA_OK);
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        CHECK_EQ(pos, target);
        CHECK_EQ(fa_stream_read(s, buf, 4096, &rd), FA_OK);
        CHECK_EQ(rd, 4096);
        for (i = 2048 * 2; i < 4096 * 2; i++) e2 += fabs(buf[i] - ref[(size_t)target * 2 + i]);
        printf("  after seek: mean error %f\n", e2 / (2048 * 2));
        CHECK(e2 / (2048 * 2) < 0.01);
    }
    free(got);
    free(ref);
    fa_stream_close(s);
}

/* Ogg radio: every song is a new link with its own headers; its comments become
 * the title, announced where the song starts. */
static void test_ogg_radio_titles(void)
{
    fa_result r;
    fa_stream* s = open_url("/oggradio", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_metadata md;
    meta_log log;
    float buf[4096 * 2];
    fa_frames total = 0;
    CHECK_EQ(r, FA_OK);
    memset(&log, 0, sizeof(log));
    fa_mutex_init(&log.lock);
    fa_stream_set_event_callback(s, on_event, &log);
    fa_stream_get_info(s, &info);
    CHECK(strcmp(info.codec, "opus") == 0);
    CHECK(strcmp(info.container, "ogg") == 0);
    CHECK_EQ(info.format.sample_rate, 48000);
    CHECK(!info.seekable);
    while (total < 24000) { fa_frames got = 0; fa_stream_read(s, buf, 4096, &got); CHECK(got > 0); total += got; }
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.station, "Ogg FM");
    CHECK_STR(md.stream_title, "Artist A - Song 1");
    CHECK_STR(md.title, "Song 1");
    CHECK_EQ(md.seq, 1);
    /* Into the second song, then around to the first again. */
    while (total < 72000) { fa_frames got = 0; fa_stream_read(s, buf, 4096, &got); CHECK(got > 0); total += got; }
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.stream_title, "Artist B - Song 2");
    CHECK_EQ(md.seq, 2);
    while (total < 120000) { fa_frames got = 0; fa_stream_read(s, buf, 4096, &got); CHECK(got > 0); total += got; }
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.stream_title, "Artist A - Song 1");
    CHECK_EQ(md.seq, 3);
    CHECK(wait_events(&log, 3) >= 3);
    printf("  songs announced at frames %lld and %lld (start at 48000 and 96000)\n",
           (long long)log.frame[1], (long long)log.frame[2]);
    CHECK(llabs((long long)log.frame[1] - 48000) < 4800);
    CHECK(llabs((long long)log.frame[2] - 96000) < 4800);
    fa_stream_close(s);
    fa_mutex_destroy(&log.lock);
}

static void test_playlist_file_is_followed(void)
{
    fa_result r;
    fa_stream* s = open_url("/station.pls", FA_OPEN_DECODE_ONLY, &r);
    fa_metadata md;
    float buf[4096 * 2];
    fa_frames got = 0;
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_read(s, buf, 4096, &got), FA_OK);
    CHECK(got > 0);
    CHECK_EQ(fa_stream_get_metadata(s, &md), FA_OK);
    CHECK_STR(md.station, "Test FM");
    fa_stream_close(s);
}

/* Attached to a (never started) null device: the test is the audio thread, so the
 * event can be checked against exactly what was played. */
static void test_icy_radio_attached(void)
{
    fa_result r;
    fa_device* dev = NULL;
    fa_stream* s;
    meta_log log;
    float out[512 * 2];
    int64_t played = 0;
    double song2_at;
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    s = open_url("/radio", 0, &r);
    CHECK_EQ(r, FA_OK);
    memset(&log, 0, sizeof(log));
    fa_mutex_init(&log.lock);
    fa_stream_set_event_callback(s, on_event, &log);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    song2_at = (double)RADIO_SONG2 / (128000.0 / 8.0) * dev->rate;
    while (played < (int64_t)dev->rate * 3) {
        int tries;
        for (tries = 0; tries < 2000 && fa_ring_readable(&s->ring) < 512; tries++) fa_sleep_ms(1);
        fa_fetch_add64(&dev->epoch, 1);
        fa_device_render(dev, out, 512);
        fa_fetch_add64(&dev->epoch, 1);
        played += 512;
        if (played < song2_at - dev->rate * 0.3) {
            fa_mutex_lock(&log.lock);
            if (log.count > 1) { fa_mutex_unlock(&log.lock); CHECK(!"song 2 announced before it was heard"); }
            fa_mutex_unlock(&log.lock);
        }
    }
    CHECK(wait_events(&log, 2) >= 2);
    CHECK_EQ(log.seq[0], 1);
    CHECK_EQ(log.seq[1], 2);
    printf("  song 2 heard at frame %lld (device frames ~%.0f)\n", (long long)log.frame[1], song2_at);
    CHECK(fabs((double)log.frame[1] * dev->rate / 44100.0 - song2_at) < 0.3 * dev->rate);
    CHECK(fa_ring_readable(&s->ring) >= 0);
    fa_stream_close(s);
    fa_device_close(dev);
    fa_mutex_destroy(&log.lock);
}

/* A server that goes quiet: the stream buffers (STALLED), then plays on (RESUMED)
 * and reaches the end with every frame played. */
static void test_stall_and_resume(void)
{
    fa_result r;
    fa_device* dev = NULL;
    fa_stream* s;
    meta_log log;
    float out[512 * 2];
    int64_t t0;
    int saw_buffering = 0;
    fa_stream_state st = FA_STATE_STOPPED;
    fa_store32(&g_stall_served, 0);
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    s = open_url("/stall.wav", 0, &r);
    CHECK_EQ(r, FA_OK);
    memset(&log, 0, sizeof(log));
    fa_mutex_init(&log.lock);
    fa_stream_set_event_callback(s, on_event, &log);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t0 = fa_monotonic_ns();
    /* Render in real time (the device is ours to drive) until the stream ends. */
    while (fa_monotonic_ns() - t0 < 8000000000LL) {
        int64_t due = (fa_monotonic_ns() - t0) * dev->rate / 1000000000LL;
        while (fa_load64(&dev->clock) < due) {
            fa_fetch_add64(&dev->epoch, 1);
            fa_device_render(dev, out, 512);
            fa_fetch_add64(&dev->epoch, 1);
        }
        fa_stream_get_state(s, &st);
        if (st == FA_STATE_BUFFERING) saw_buffering = 1;
        if (st == FA_STATE_ENDED) break;
        fa_sleep_ms(5);
    }
    CHECK(saw_buffering);
    CHECK_EQ(st, FA_STATE_ENDED);
    fa_sleep_ms(50);
    fa_mutex_lock(&log.lock);
    {
        int stalled = log.by_type[FA_EVENT_STALLED], resumed = log.by_type[FA_EVENT_RESUMED], ended = log.by_type[FA_EVENT_ENDED];
        int i, first_stall = -1, first_resume = -1;
        for (i = 0; i < log.order_count; i++) {
            if (log.order[i] == FA_EVENT_STALLED && first_stall < 0) first_stall = i;
            if (log.order[i] == FA_EVENT_RESUMED && first_resume < 0) first_resume = i;
        }
        fa_mutex_unlock(&log.lock);
        printf("  stalled %d, resumed %d, ended %d\n", stalled, resumed, ended);
        CHECK_EQ(stalled, 1);
        CHECK_EQ(resumed, 1);
        CHECK_EQ(ended, 1);
        CHECK(first_stall < first_resume);
    }
    {
        fa_frames pos = 0;
        fa_stream_get_position(s, &pos);
        CHECK_EQ(pos, WAV_FRAMES);
    }
    fa_stream_close(s);
    fa_device_close(dev);
    fa_mutex_destroy(&log.lock);
}

static fa_result FA_CALL discard_write(void* user, const void* data, size_t size)
{
    (void)user; (void)data; (void)size;
    return FA_OK;
}

/* An encoder converting a web stream whose download stalls closes at once: it
 * doesn't wait in a network read for the data (or the timeout). */
static void test_encoder_close_during_stall(void)
{
    fa_result r;
    fa_stream* s = open_url("/hang.wav", FA_OPEN_DECODE_ONLY, &r);
    fa_encoder_output out;
    fa_encoder* e = NULL;
    fa_encoder_status st;
    int64_t t0;
    CHECK_EQ(r, FA_OK);
    if (!s) return;
    memset(&out, 0, sizeof(out));
    memset(&st, 0, sizeof(st));
    out.write = discard_write;
    CHECK_EQ(fa_encoder_create_output(&out, NULL, NULL, &e), FA_OK);
    CHECK_EQ(fa_encoder_encode_stream(e, s), FA_OK);
    /* The first second is encoded, then it waits. Timed by the clock, not by
     * sleeps: a slow emulator takes longer than 400 short ones. */
    t0 = fa_monotonic_ns() + 20000000000LL;
    for (;;) {
        CHECK_EQ(fa_encoder_get_status(e, &st), FA_OK);
        if (st.frames >= 30000 || fa_monotonic_ns() > t0) break;
        fa_sleep_ms(5);
    }
    CHECK(st.frames >= 30000);
    fa_sleep_ms(300);
    t0 = fa_monotonic_ns();
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    t0 = fa_monotonic_ns() - t0;
    printf("  close took %.0f ms\n", (double)t0 / 1e6);
    CHECK(t0 < 1000000000LL);
    fa_stream_close(s);
}

/* ---- HLS ------------------------------------------------------------------------- */

static void test_hls_vod(void)
{
    fa_result r;
    fa_stream* s = open_url("/hls/master.m3u8", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_net_status st;
    fa_frames n = 0, ref_n = 0;
    float *got, *ref;
    char url[256];
    double total = 0, err = 0;
    int i;
    CHECK_EQ(r, FA_OK);
    for (i = 0; i < HLS_SEGS; i++) total += g_seg_dur[i];
    fa_stream_get_info(s, &info);
    CHECK_STR(info.container, "hls");
    CHECK_STR(info.codec, "mp3");
    CHECK(info.seekable);
    CHECK(llabs((long long)info.length_frames - (long long)(total * 44100 + 0.5)) <= 1);
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    CHECK(st.is_hls);
    CHECK(!st.is_live);
    url_of(url, sizeof(url), "/hls/audio.m3u8");   /* the audio-only variant was picked */
    CHECK_STR(st.url, url);

    got = read_all(s, &n);
    ref = read_all_memory(g_mp3, g_mp3_n, &ref_n);
    CHECK(ref != NULL);
    printf("  hls %lld frames, file %lld\n", (long long)n, (long long)ref_n);
    CHECK(llabs((long long)n - (long long)ref_n) <= 1152);
    /* Within the first segment the audio is bit-identical. */
    for (i = 0; i < 44100 * 2; i++) err += fabs(got[i] - ref[i]);
    CHECK(err < 1e-3);

    /* Seek into the second segment: exact frame position, audio continues. */
    {
        fa_frames pos = 0, rd = 0, target = (fa_frames)(g_seg_dur[0] * 44100) + 5000;
        float buf[2048 * 2];
        double e2 = 0;
        CHECK_EQ(fa_stream_seek(s, target), FA_OK);
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        CHECK_EQ(pos, target);
        CHECK_EQ(fa_stream_read(s, buf, 2048, &rd), FA_OK);
        CHECK_EQ(rd, 2048);
        /* A decoder starting at a segment boundary needs a frame or two to settle
         * (the bit reservoir); compare after that. */
        for (i = 1152 * 2; i < 2048 * 2; i++) e2 += fabs(buf[i] - ref[(size_t)target * 2 + i]);
        printf("  after seek: mean error %.6f\n", e2 / (2048 * 2 - 1152 * 2));
        CHECK(e2 / (2048 * 2 - 1152 * 2) < 0.02);
    }
    free(got);
    free(ref);
    fa_stream_close(s);
}

/* AC-3 in HLS (as broadcasters send it) decodes natively and matches the file. */
static void test_hls_ac3(void)
{
    fa_result r;
    fa_stream* s = open_url("/hls/ac3.m3u8", FA_OPEN_DECODE_ONLY, &r);
    fa_stream_info info;
    fa_frames n = 0, ref_n = 0;
    float *got, *ref;
    double err = 0;
    int i;
    CHECK_EQ(r, FA_OK);
    if (r != FA_OK) return;
    fa_stream_get_info(s, &info);
    CHECK_STR(info.container, "hls");
    CHECK_STR(info.codec, "ac3");
    CHECK_EQ(info.format.sample_rate, 48000);
    CHECK_EQ(info.format.channels, 2);
    got = read_all(s, &n);
    ref = read_all_memory(g_ac3, g_ac3_n, &ref_n);
    CHECK(ref != NULL);
    printf("  hls %lld frames, file %lld\n", (long long)n, (long long)ref_n);
    CHECK_EQ(n, ref_n);
    for (i = 0; i < (int)(n < ref_n ? n : ref_n) * 2; i++) err += fabs(got[i] - ref[i]);
    CHECK(err < 1e-3);
    free(got);
    free(ref);
    fa_stream_close(s);
}

/* FA_TEST_REAL_SYSTEM=1: the system's services are there, so a test that needs them
 * fails instead of skipping (the same test as test_aac.c and test_device.c). */
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
static int real_system(void)
{
    const char* v = getenv("FA_TEST_REAL_SYSTEM");
    return v != NULL && strcmp(v, "1") == 0;
}
#endif

/* HTTPS through the system's TLS stack, when FA_NET_HTTPS_URL names an audio file
 * (CI sets none by default: the test must not depend on the internet). */
static void test_https_optional(void)
{
    const char* url = getenv("FA_NET_HTTPS_URL");
    fa_stream* s = NULL;
    fa_open_options o;
    fa_stream_info info;
    float buf[4096 * 2];
    fa_frames got = 0;
    fa_result r;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* The iOS simulator CI job serves one over a trusted test certificate. */
    CHECK(url != NULL || !real_system());
#endif
    if (!url) { printf("  skipped (set FA_NET_HTTPS_URL)\n"); return; }
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_url(g_engine, url, &o, &s);
    printf("  open: %d\n", (int)r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    printf("  %s, %u Hz, %u channels\n", info.codec, info.format.sample_rate, info.format.channels);
    CHECK_EQ(fa_stream_read(s, buf, 4096, &got), FA_OK);
    CHECK(got > 0);
    fa_stream_close(s);
}

/* The request headers through the system's TLS stack, when FA_NET_HTTPS_ECHO_URL
 * names tools/test-https/serve.py's /echo-headers (with credentials in the URL:
 * https://user:p%20w@127.0.0.1:8443/echo-headers). */
static void test_https_headers_optional(void)
{
    const char* url = getenv("FA_NET_HTTPS_ECHO_URL");
#if defined(__APPLE__) && TARGET_OS_IPHONE
    CHECK(url != NULL || !real_system());
#endif
    if (!url) { printf("  skipped (set FA_NET_HTTPS_ECHO_URL)\n"); return; }
    check_request_headers(url);
}

/* The origin of FA_NET_HTTPS_ECHO_URL (tools/test-https/serve.py) as
 * "https://127.0.0.1:8443", and its port. 0 when it is not set. */
static int https_origin(char* out, size_t cap, int* out_port)
{
    const char* url = getenv("FA_NET_HTTPS_ECHO_URL");
    fa_url u;
    if (!url || fa_url_parse(url, &u) != FA_OK) return 0;
    snprintf(out, cap, "https://%s:%u", u.host, (unsigned)u.port);
    *out_port = u.port;
    return 1;
}

/* How many connections serve.py has opened (this request's included). */
static int https_connections(fa_http_session* session, const char* origin)
{
    char url[300];
    conn_fetch c;
    int n;
    memset(&c, 0, sizeof(c));
    snprintf(url, sizeof(url), "%s/connections", origin);
    n = fetch_with(session, url, NULL, &c) == FA_OK && c.body.p ? atoi(c.body.p) : -1;
    free(c.body.p);
    return n;
}

/* Connection reuse through the system's TLS stack, against serve.py
 * (FA_NET_HTTPS_ECHO_URL): requests of one session share a connection, one the
 * server closed without notice is replaced, cookies are not kept, a cancelled
 * transfer leaves the session usable, and redirects to another origin (serve.py's second port) drop the
 * credentials. */
static void test_https_session_optional(void)
{
    char origin[300], url[700];
    conn_fetch c;
    fa_http_session* session;
    int port, first, i;
    if (!https_origin(origin, sizeof(origin), &port)) { printf("  skipped (set FA_NET_HTTPS_ECHO_URL)\n"); return; }
    session = fa_http_session_create();
    CHECK(session != NULL);
    memset(&c, 0, sizeof(c));
    snprintf(url, sizeof(url), "%s/echo-headers", origin);
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
    first = c.conn;
    CHECK(first > 0);
    for (i = 0; i < 2; i++) {
        CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
        CHECK_EQ(c.conn, first);
    }
    /* The server closes the kept connection: the next request gets a new one, once,
     * right away and after the close has arrived. */
    for (i = 0; i < 2; i++) {
        snprintf(url, sizeof(url), "%s/echo-headers?close", origin);
        CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
        first = c.conn;
        if (i == 1) fa_sleep_ms(200);
        snprintf(url, sizeof(url), "%s/echo-headers", origin);
        CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
        CHECK_EQ(c.status, 200);
        CHECK(c.conn != first);
        first = c.conn;
        CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
        CHECK_EQ(c.conn, first);
    }
    /* A server's cookie is not sent back: no transport keeps a cookie store. */
    snprintf(url, sizeof(url), "%s/echo-headers?cookie", origin);
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
    snprintf(url, sizeof(url), "%s/echo-headers", origin);
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
    CHECK(c.body.p != NULL);
    CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 0);
    /* Cancelled in the middle of a body, then the next request. */
    snprintf(url, sizeof(url), "%s/net/tone_cbr.mp3", origin);
    c.cancel_on_data = 1;
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_ERROR_BUSY);
    c.cancel_on_data = 0;
    snprintf(url, sizeof(url), "%s/echo-headers", origin);
    CHECK_EQ(fetch_with(session, url, NULL, &c), FA_OK);
    CHECK_EQ(c.status, 200);
    /* Redirects: another origin gets no credentials, the same one does. */
    snprintf(url, sizeof(url), "https://user:p%%20w@127.0.0.1:%d/redirect?to=https://127.0.0.1:%d/echo-headers", port, port + 1);
    CHECK_EQ(fetch_with(session, url, "Cookie: a=b\r\nX-Keep: 1", &c), FA_OK);
    CHECK(c.body.p != NULL);
    CHECK_EQ(count_header(c.body.p, "Authorization", NULL, 0), 0);
    CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 0);
    CHECK_EQ(count_header(c.body.p, "X-Keep", NULL, 0), 1);
    snprintf(url, sizeof(url), "https://user:p%%20w@127.0.0.1:%d/redirect?to=https://127.0.0.1:%d/echo-headers", port, port);
    CHECK_EQ(fetch_with(session, url, "Cookie: a=b", &c), FA_OK);
    CHECK_EQ(count_header(c.body.p, "Authorization", NULL, 0), 1);
    CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 1);
    free(c.body.p);
    fa_http_session_destroy(session);
}

/* An HLS stream from serve.py (a master playlist, a media playlist, three
 * byte-range segments of one file) plays over a single connection: the stream
 * opens at most one (none when the system pools connections across sessions, as
 * WinHTTP does). The two counts go over a session of their own, so they add none.
 * Prints the time to the first audio. */
static void test_https_hls_optional(void)
{
    char origin[300], url[400];
    fa_open_options o;
    fa_net_options net;
    fa_stream* s = NULL;
    float buf[1152 * 2];
    fa_frames got = 0, total = 0;
    int port, before, after;
    int64_t t0, t_first = 0;
    fa_http_session* counts;
    if (!https_origin(origin, sizeof(origin), &port)) { printf("  skipped (set FA_NET_HTTPS_ECHO_URL)\n"); return; }
    counts = fa_http_session_create();
    before = https_connections(counts, origin);
    CHECK(before > 0);
    snprintf(url, sizeof(url), "%s/hls/master.m3u8", origin);
    fa_open_options_init(&o);
    fa_net_options_init(&net);
    net.proxy = "";
    net.timeout_ms = 5000;
    o.flags = FA_OPEN_DECODE_ONLY;
    o.net = &net;
    t0 = fa_monotonic_ns();
    CHECK_EQ(fa_stream_open_url(g_engine, url, &o, &s), FA_OK);
    while (fa_stream_read(s, buf, 1152, &got) == FA_OK && got > 0) {
        if (!t_first) t_first = fa_monotonic_ns();
        total += got;
    }
    fa_stream_close(s);
    after = https_connections(counts, origin);
    fa_http_session_destroy(counts);
    printf("  %lld frames, first audio after %.0f ms, %d new connection(s)\n", (long long)total,
           (double)(t_first - t0) / 1e6, after - before);
    CHECK(total > 44100 * 2);
    CHECK(after - before <= 1);
}

/* Through the system's TLS stack, against serve.py (FA_NET_HTTPS_ECHO_URL): how a
 * body ended. One with a length or chunked framing is complete (never "closed"); one
 * without either that the server closes is reported as such (or as a network error:
 * a reconnect either way); a chunked one cut before its last chunk is an error. And
 * an HLS stream behind cookies: the master playlist's two cookies (URLSession joins
 * them into one header) come back on the media playlist and the segments. */
static void test_https_framing_optional(void)
{
    static const char* const framing[] = { "length", "chunked", "close", "cut" };
    char origin[300], url[400];
    int port, i;
    if (!https_origin(origin, sizeof(origin), &port)) { printf("  skipped (set FA_NET_HTTPS_ECHO_URL)\n"); return; }
    for (i = 0; i < 4; i++) {
        fa_http_request req;
        conn_fetch c;
        int closed = -1;
        fa_result r;
        snprintf(url, sizeof(url), "%s/body?framing=%s", origin, framing[i]);
        memset(&c, 0, sizeof(c));
        memset(&req, 0, sizeof(req));
        req.url = url;
        req.range_start = -1;
        req.proxy = "";
        req.timeout_ms = 5000;
        req.closed_end = &closed;
        r = fa_http_fetch(&req, cf_head, cf_data, &c);
        printf("  %s: result %d, %zu bytes, closed_end %d\n", framing[i], (int)r, c.body.n, closed);
        if (i < 2) {
            CHECK_EQ(r, FA_OK);
            CHECK_EQ(c.body.n, 20000);
            CHECK_EQ(closed, 0);
        } else if (i == 2) {
            CHECK((r == FA_OK && closed == 1 && c.body.n == 20000) || r == FA_ERROR_NETWORK);
        } else {
            CHECK_EQ(r, FA_ERROR_NETWORK);
        }
        free(c.body.p);
    }
    {
        fa_open_options o;
        fa_net_options net;
        fa_stream* s = NULL;
        float buf[1152 * 2];
        fa_frames got = 0, total = 0;
        snprintf(url, sizeof(url), "%s/hls-cookie/master.m3u8", origin);
        fa_open_options_init(&o);
        fa_net_options_init(&net);
        net.proxy = "";
        net.timeout_ms = 5000;
        o.flags = FA_OPEN_DECODE_ONLY;
        o.net = &net;
        CHECK_EQ(fa_stream_open_url(g_engine, url, &o, &s), FA_OK);
        while (fa_stream_read(s, buf, 1152, &got) == FA_OK && got > 0) total += got;
        fa_stream_close(s);
        printf("  behind cookies: %lld frames\n", (long long)total);
        CHECK(total > 44100 * 2);
    }
}

/* A segment whose download breaks off is fetched again, and the audio already
 * delivered is not repeated; byte-range segments work on a server ignoring Range. */
static void test_hls_retry_and_byteranges(void)
{
    static const char* const paths[] = { "/hls/flaky.m3u8", "/hls/ranged.m3u8" };
    fa_frames ref_n = 0;
    float* ref = read_all_memory(g_mp3, g_mp3_n, &ref_n);
    size_t p;
    CHECK(ref != NULL);
    for (p = 0; p < 2; p++) {
        fa_result r;
        fa_stream* s;
        fa_frames n = 0;
        float* got;
        double err = 0;
        fa_frames i;
        fa_store32(&g_seg_drop, 1);
        s = open_url(paths[p], FA_OPEN_DECODE_ONLY, &r);
        CHECK_EQ(r, FA_OK);
        got = read_all(s, &n);
        printf("  %s: %lld frames (file %lld)\n", paths[p], (long long)n, (long long)ref_n);
        CHECK(llabs((long long)n - (long long)ref_n) <= 1152);
        for (i = 0; i < 44100 * 2 && i < n * 2; i++) err += fabs(got[i] - ref[i]);
        CHECK(err < 1e-3);
        if (p == 0) CHECK_EQ(fa_load32(&g_seg_drop), 0);   /* the drop did happen */
        free(got);
        fa_stream_close(s);
    }
    free(ref);
}

/* Single-file fMP4-style HLS: the EXT-X-MAP init section is a byte range of the
 * file the segments are ranges of. Only that range goes in front of the first
 * segment (it used to be the whole file), also from a server that ignores Range. */
static void test_hls_map_byterange(void)
{
    static const char* const paths[] = { "/hls/map.m3u8", "/hls/map-nr.m3u8" };
    fa_frames ref_n = 0;
    float* ref = read_all_memory(g_mp3, g_mp3_n, &ref_n);
    size_t p;
    CHECK(ref != NULL);
    for (p = 0; p < 2; p++) {
        fa_result r;
        fa_stream* s = open_url(paths[p], FA_OPEN_DECODE_ONLY, &r);
        fa_frames n = 0, i;
        float* got;
        double err = 0;
        CHECK_EQ(r, FA_OK);
        if (!s) continue;
        got = read_all(s, &n);
        printf("  %s: %lld frames (file %lld)\n", paths[p], (long long)n, (long long)ref_n);
        CHECK(llabs((long long)n - (long long)ref_n) <= 1152);
        for (i = 0; i < n * 2 && i < ref_n * 2; i++) err += fabs(got[i] - ref[i]);
        CHECK(err / (double)(n * 2 + 1) < 1e-6);
        free(got);
        fa_stream_close(s);
    }
    free(ref);
}

/* The parts of the download buffer a playlist and an init section use: a byte
 * range from either kind of server, and a window no bigger than a small file. */
static void test_net_small_range_and_window(void)
{
    char url[256];
    fa_net_config cfg;
    fa_net* n = NULL;
    char* data = NULL;
    size_t len = 0;
    memset(&cfg, 0, sizeof(cfg));
    url_of(url, sizeof(url), "/tone.wav");
    cfg.url = fa_strdup(url);
    cfg.proxy = fa_strdup("");
    cfg.timeout_ms = 3000;
    cfg.prebuffer_ms = 100;
    cfg.reconnect_ms = 1000;
    CHECK_EQ(fa_net_open(&cfg, &n), FA_OK);
    if (!n) return;
    fa_mutex_lock(&n->lock);
    CHECK_EQ(n->cap, (int64_t)g_wav_n + 65536);   /* not 4 MiB for a 345 KB file */
    fa_mutex_unlock(&n->lock);
    {
        fa_io_callbacks io;
        uint8_t* all = (uint8_t*)malloc(g_wav_n + 16);
        size_t got = 0;
        int64_t k;
        fa_net_io(n, &io);
        while ((k = io.read(n, all + got, g_wav_n + 16 - got)) > 0) got += (size_t)k;
        CHECK_EQ(got, g_wav_n);
        CHECK(memcmp(all, g_wav, g_wav_n) == 0);
        free(all);
    }
    url_of(url, sizeof(url), "/hls/padded.mp3");
    CHECK_EQ(fa_net_get_small(n, url, MAP_PAD + 100, 720, &data, &len, NULL, 0), FA_OK);
    CHECK_EQ(len, 720);
    CHECK(data && memcmp(data, g_mp3 + 100, 720) == 0);
    free(data);
    url_of(url, sizeof(url), "/hls/padded-nr.mp3");
    CHECK_EQ(fa_net_get_small(n, url, MAP_PAD + 100, 720, &data, &len, NULL, 0), FA_OK);
    CHECK_EQ(len, 720);
    CHECK(data && memcmp(data, g_mp3 + 100, 720) == 0);
    free(data);
    /* A range past the end is never handed on short. */
    CHECK_EQ(fa_net_get_small(n, url, (int64_t)g_pad_n - 10, 720, &data, &len, NULL, 0), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(data == NULL);
    fa_net_close(n);
}

/* The app's Cookie and Authorization go to the stream's own server only: segments
 * a playlist names on another origin don't get them (as after a redirect there). */
static void test_playlist_credentials(void)
{
    char url[256];
    fa_open_options o;
    fa_net_options net;
    fa_stream* s = NULL;
    fa_result r;
    int i, n;
    n = snprintf(g_cred_playlist, sizeof(g_cred_playlist), "#EXTM3U\n#EXT-X-TARGETDURATION:2\n");
    for (i = 0; i < HLS_SEGS; i++)
        n += snprintf(g_cred_playlist + n, sizeof(g_cred_playlist) - (size_t)n, "#EXTINF:%.5f,\n%s://localhost:%d/hls/seg%d.ts\n",
                      g_seg_dur[i], g_scheme, g_url_port, i);
    snprintf(g_cred_playlist + n, sizeof(g_cred_playlist) - (size_t)n, "#EXT-X-ENDLIST\n");
    fa_store32(&g_cred_home, 0);
    fa_store32(&g_cred_foreign, 0);
    url_of(url, sizeof(url), "/hls/cred.m3u8");
    fa_open_options_init(&o);
    fa_net_options_init(&net);
    net.timeout_ms = 3000;
    net.proxy = "";
    net.headers = "Cookie: a=b\r\nAuthorization: Bearer t";
    o.flags = FA_OPEN_DECODE_ONLY;
    o.net = &net;
    r = fa_stream_open_url(g_engine, url, &o, &s);
    CHECK_EQ(r, FA_OK);
    if (s) {
        fa_frames got = 0;
        float* buf = read_all(s, &got);
        CHECK(got > 44100);
        free(buf);
        fa_stream_close(s);
    }
    CHECK(fa_load32(&g_cred_home) >= 1);   /* the playlist itself */
    CHECK_EQ(fa_load32(&g_cred_foreign), 0);

    /* The same rule one level down: app_origin names the origin they may go to. */
    {
        fa_http_request req;
        conn_fetch c;
        memset(&c, 0, sizeof(c));
        memset(&req, 0, sizeof(req));
        snprintf(url, sizeof(url), "%s://localhost:%d/echo-headers", g_scheme, g_url_port);
        req.url = url;
        req.range_start = -1;
        req.proxy = "";
        req.timeout_ms = 3000;
        req.headers = "Cookie: a=b\r\nX-Keep: 1";
        req.cancel = &c.cancel;
        req.app_origin = "http://127.0.0.1:1/stream.m3u8";
        CHECK_EQ(fa_http_fetch(&req, cf_head, cf_data, &c), FA_OK);
        CHECK(c.body.p != NULL);
        CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 0);
        CHECK_EQ(count_header(c.body.p, "X-Keep", NULL, 0), 1);
        free(c.body.p);
        memset(&c, 0, sizeof(c));
        req.app_origin = url;
        CHECK_EQ(fa_http_fetch(&req, cf_head, cf_data, &c), FA_OK);
        CHECK(c.body.p != NULL);
        CHECK_EQ(count_header(c.body.p, "Cookie", NULL, 0), 1);
        free(c.body.p);
    }
}

/* A file whose connection dropped with reconnecting off: what arrived plays and
 * the stream ends; a later seek past it connects again. */
static void test_seek_after_giving_up(void)
{
    char url[256];
    fa_open_options o;
    fa_net_options net;
    fa_stream* s = NULL;
    fa_frames n = 0, rd = 0, i;
    float* got;
    float buf[512 * 2];
    url_of(url, sizeof(url), "/drop.wav");
    fa_store32(&g_drop_served, 0);
    fa_open_options_init(&o);
    fa_net_options_init(&net);
    net.proxy = "";
    net.timeout_ms = 3000;
    net.flags = FA_NET_NO_RECONNECT;
    o.flags = FA_OPEN_DECODE_ONLY;
    o.net = &net;
    CHECK_EQ(fa_stream_open_url(g_engine, url, &o, &s), FA_OK);
    got = read_all(s, &n);
    free(got);
    CHECK_EQ(n, (100000 - 44) / 4);
    CHECK_EQ(fa_stream_seek(s, 60000), FA_OK);
    CHECK_EQ(fa_stream_read(s, buf, 512, &rd), FA_OK);
    CHECK_EQ(rd, 512);
    for (i = 0; i < rd; i++) if (buf[i * 2] != wav_left(60000 + i)) { CHECK_EQ(60000 + i, -1); }
    fa_stream_close(s);
}

/* A long user agent (the request is sized to fit everything it carries). */
static void test_long_user_agent(void)
{
    char url[256], agent[3001];
    fa_open_options o;
    fa_net_options net;
    fa_stream* s = NULL;
    memset(agent, 'a', 3000);
    agent[3000] = 0;
    url_of(url, sizeof(url), "/tone.wav");
    fa_open_options_init(&o);
    fa_net_options_init(&net);
    net.proxy = "";
    net.user_agent = agent;
    o.flags = FA_OPEN_DECODE_ONLY;
    o.net = &net;
    CHECK_EQ(fa_stream_open_url(g_engine, url, &o, &s), FA_OK);
    CHECK_EQ(strlen(g_last_agent), 3000);
    fa_stream_close(s);
}

static void test_close_while_streaming(void)
{
    /* Closing a radio stream mid-download returns promptly. */
    fa_result r;
    int64_t t0;
    fa_stream* s = open_url("/radio", 0, &r);
    fa_device* dev = NULL;
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    fa_sleep_ms(100);
    t0 = fa_monotonic_ns();
    fa_stream_close(s);
    CHECK((fa_monotonic_ns() - t0) < 2000000000LL);
    fa_device_close(dev);
}

/* ---- step 11: audio groups, resolver, clean closes, the tap, cookies ------------ */

/* open_url with the caller's network settings (proxy and timeout as open_url's
 * unless set). */
static fa_stream* open_url_net(const char* path, uint32_t flags, fa_net_options* net, fa_result* out_r)
{
    char url[256];
    fa_open_options o;
    fa_stream* s = NULL;
    url_of(url, sizeof(url), path);
    fa_open_options_init(&o);
    if (!net->proxy) net->proxy = "";
    if (!net->timeout_ms) net->timeout_ms = 3000;
    o.flags = flags;
    o.net = net;
    *out_r = fa_stream_open_url(g_engine, url, &o, &s);
    return *out_r == FA_OK ? s : NULL;
}

/* Tiered audio groups: the rendition of the chosen variant's group plays, and
 * hls_max_bandwidth picks the variant. */
static void test_hls_audio_groups(void)
{
    fa_net_options net;
    fa_stream* s;
    fa_stream_info info;
    fa_net_status st;
    fa_result r;
    fa_frames n = 0;
    float* got;
    fa_net_options_init(&net);
    s = open_url_net("/hls/tiers.m3u8", FA_OPEN_DECODE_ONLY, &net, &r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "mp3");
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    CHECK(strstr(st.url, "/hls/audio.m3u8") != NULL);
    fa_stream_close(s);
    fa_net_options_init(&net);
    net.hls_max_bandwidth = 1000000;
    s = open_url_net("/hls/tiers.m3u8", FA_OPEN_DECODE_ONLY, &net, &r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "aac");
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    CHECK(strstr(st.url, "/hls/aac.m3u8") != NULL);
    got = read_all(s, &n);
    free(got);
    CHECK(n > 44100);
    fa_stream_close(s);
}

/* A resolver that never answers: the hook holds every lookup after the first. */
static fa_atomic32 g_resolve_hold, g_resolve_calls, g_resolve_in, g_resolve_out;

static void hold_resolve(const char* host)
{
    (void)host;
    if (fa_fetch_add32(&g_resolve_calls, 1) == 0) return;
    fa_fetch_add32(&g_resolve_in, 1);
    while (fa_load32(&g_resolve_hold)) fa_sleep_ms(2);
    fa_fetch_add32(&g_resolve_out, 1);
}

/* A reconnect that hangs in the plain-socket client's name lookup doesn't hold up
 * the close: it returns in well under 50 ms and leaves the lookup behind. */
static void test_close_during_resolve(void)
{
    fa_net_options net;
    fa_stream* s;
    fa_result r;
    int64_t t0, took;
    if (strcmp(g_scheme, "http") != 0) return;
    fa_store32(&g_drop_served, 0);
    fa_store32(&g_resolve_calls, 0);
    fa_store32(&g_resolve_in, 0);
    fa_store32(&g_resolve_out, 0);
    fa_store32(&g_resolve_hold, 1);
    fa_net_test_resolve_hook = hold_resolve;
    fa_net_options_init(&net);
    s = open_url_net("/drop.wav", FA_OPEN_DECODE_ONLY, &net, &r);   /* the first connection breaks off */
    CHECK_EQ(r, FA_OK);
    t0 = fa_monotonic_ns();
    while (!fa_load32(&g_resolve_in) && fa_monotonic_ns() - t0 < 5000000000LL) fa_sleep_ms(2);
    CHECK_EQ(fa_load32(&g_resolve_in), 1);
    t0 = fa_monotonic_ns();
    fa_stream_close(s);
    took = fa_monotonic_ns() - t0;
    printf("  close took %.1f ms\n", (double)took / 1e6);
    CHECK(took < 50000000LL);
    /* Let the abandoned lookup finish (it frees itself). */
    fa_store32(&g_resolve_hold, 0);
    t0 = fa_monotonic_ns();
    while (!fa_load32(&g_resolve_out) && fa_monotonic_ns() - t0 < 5000000000LL) fa_sleep_ms(2);
    fa_sleep_ms(50);
    fa_net_test_resolve_hook = NULL;
}

/* A live stream without a length or chunked framing that the server closes cleanly
 * is reconnected like a dropped one, not taken as the end of a file. */
static void test_clean_close_reconnects(void)
{
    fa_net_options net;
    fa_stream* s;
    fa_result r;
    fa_net_status st;
    fa_frames n = 0, ref_n = 0;
    float* got;
    float* ref = read_all_memory(g_mp3, g_mp3_n, &ref_n);
    free(ref);
    fa_store32(&g_live_served, 0);
    fa_net_options_init(&net);
    net.reconnect_ms = 1500;
    s = open_url_net("/live-close", FA_OPEN_DECODE_ONLY, &net, &r);
    CHECK_EQ(r, FA_OK);
    got = read_all(s, &n);
    free(got);
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
    printf("  %lld frames (one body: %lld), %u reconnect(s)\n", (long long)n, (long long)ref_n, st.reconnects);
    CHECK(st.reconnects >= 1);
    CHECK(n > ref_n + ref_n / 4);    /* half the first time, all of it the second */
    fa_stream_close(s);
}

typedef struct tap_buf {
    fa_mutex lock;
    uint8_t* p;
    size_t n, cap;
    int calls;
    fa_atomic_ptr stream;     /* set once open returned */
    size_t stop_after;        /* > 0: the callback turns itself off past this many bytes */
    int off;                  /* it did */
} tap_buf;

static void FA_CALL tap_proc(void* user, const void* data, size_t size)
{
    tap_buf* t = (tap_buf*)user;
    fa_stream* s;
    fa_mutex_lock(&t->lock);
    if (t->n + size > t->cap) {
        size_t cap = (t->n + size) * 2;
        uint8_t* p = (uint8_t*)realloc(t->p, cap);
        if (p) { t->p = p; t->cap = cap; }
    }
    if (t->n + size <= t->cap) { memcpy(t->p + t->n, data, size); t->n += size; }
    t->calls++;
    fa_mutex_unlock(&t->lock);
    s = (fa_stream*)fa_load_ptr(&t->stream);
    if (t->stop_after && s && t->n > t->stop_after) {
        fa_stream_set_download_callback(s, NULL, NULL);   /* from inside */
        fa_mutex_lock(&t->lock);
        t->off = 1;
        fa_mutex_unlock(&t->lock);
    }
}

/* The tap hands on the downloaded bytes: a file exactly as served, radio without
 * its ICY title blocks. It can be turned off, also from inside the callback, and
 * isn't called again after. */
static void test_download_tap(void)
{
    fa_net_options net;
    fa_stream* s;
    fa_result r;
    fa_net_status st;
    tap_buf t;
    fa_frames n = 0;
    float* got;
    size_t before;
    memset(&t, 0, sizeof(t));
    fa_mutex_init(&t.lock);
    fa_net_options_init(&net);
    net.download_proc = tap_proc;
    net.download_user = &t;
    s = open_url_net("/tone.mp3", FA_OPEN_DECODE_ONLY, &net, &r);
    CHECK_EQ(r, FA_OK);
    got = read_all(s, &n);
    free(got);
    {
        int64_t until = fa_monotonic_ns() + 5000000000LL;
        do { fa_stream_get_net_status(s, &st); if (st.finished) break; fa_sleep_ms(5); } while (fa_monotonic_ns() < until);
    }
    CHECK(st.finished);
    fa_mutex_lock(&t.lock);
    CHECK_EQ(t.n, g_mp3_n);
    CHECK(t.n == g_mp3_n && memcmp(t.p, g_mp3, g_mp3_n) == 0);
    fa_mutex_unlock(&t.lock);
    CHECK_EQ(fa_stream_set_download_callback(s, NULL, NULL), FA_OK);
    fa_stream_close(s);

    /* Radio: the audio only; turned off from inside the callback past 64 KB. */
    t.n = 0;
    t.calls = 0;
    t.stop_after = 65536;
    s = open_url_net("/radio", FA_OPEN_DECODE_ONLY, &net, &r);
    CHECK_EQ(r, FA_OK);
    fa_exchange_ptr(&t.stream, s);
    {
        float buf[1152 * 2];
        fa_frames rd = 0;
        int64_t until = fa_monotonic_ns() + 5000000000LL;
        for (;;) {
            int off;
            fa_mutex_lock(&t.lock);
            before = t.n;
            off = t.off;
            fa_mutex_unlock(&t.lock);
            if (off || fa_monotonic_ns() > until) break;
            fa_stream_read(s, buf, 1152, &rd);   /* the window has room once the decoder reads */
        }
        CHECK(before > t.stop_after);
        for (rd = 0; rd < 44100 * 3; ) { fa_frames k = 0; fa_stream_read(s, buf, 1152, &k); if (!k) break; rd += k; }
    }
    fa_mutex_lock(&t.lock);
    CHECK_EQ(t.n, before);                 /* not called again */
    CHECK(t.n >= RADIO_METAINT * 2 && memcmp(t.p, g_mp3, RADIO_METAINT) == 0);
    CHECK(memcmp(t.p + RADIO_METAINT, g_mp3 + RADIO_METAINT, RADIO_METAINT) == 0);   /* the title block is cut out */
    fa_mutex_unlock(&t.lock);
    fa_stream_close(s);
    {
        fa_stream* m = NULL;
        CHECK_EQ(fa_stream_open_memory(g_engine, g_wav, g_wav_n, NULL, &m), FA_OK);
        CHECK_EQ(fa_stream_set_download_callback(m, tap_proc, &t), FA_ERROR_INVALID_ARG);
        fa_stream_close(m);
    }
    free(t.p);
    fa_mutex_destroy(&t.lock);
}

/* The cookie jar's rules: domain and host-only cookies, paths, Secure, expiry,
 * replacement and deletion, the order of the Cookie header. */
static void test_cookie_jar(void)
{
    fa_http_cookies* j = fa_http_cookies_create();
    char* c;
    CHECK(j != NULL);
    fa_http_cookies_take(j, "https://a.example.com/radio/live.m3u8",
                         "Content-Type: x\r\nSet-Cookie: host=1\r\nSet-Cookie: dom=2; Domain=.example.com; Path=/\r\n"
                         "set-cookie: deep=3; Path=/radio/hi\r\nSet-Cookie: sec=4; Secure; Path=/\r\n"
                         "Set-Cookie: gone=5; Max-Age=0\r\nSet-Cookie: old=6; Expires=Thu, 01 Jan 1998 00:00:00 GMT\r\n"
                         "Set-Cookie: later=7; Path=/; expires=Wed, 21-Oct-2099 07:28:00 GMT\r\n"
                         "Set-Cookie: tld=8; Domain=com\r\nSet-Cookie: other=9; Domain=other.org\r\n"
                         "Set-Cookie: novalue\r\nSet-Cookie: =x\r\n");
    c = fa_http_cookies_for(j, "https://a.example.com/radio/hi/seg1.ts?x=1");
    CHECK(c != NULL);
    CHECK_STR(c, "deep=3; host=1; dom=2; sec=4; later=7");   /* longest path first, then oldest */
    free(c);
    c = fa_http_cookies_for(j, "http://a.example.com/radio/seg.ts");   /* no Secure over http, not deep */
    CHECK_STR(c, "host=1; dom=2; later=7");
    free(c);
    c = fa_http_cookies_for(j, "http://cdn.example.com/x");   /* the domain cookie only */
    CHECK_STR(c, "dom=2");
    free(c);
    c = fa_http_cookies_for(j, "http://a.example.com/radiox");   /* "/radio" doesn't cover "/radiox" */
    CHECK_STR(c, "dom=2; later=7");
    free(c);
    CHECK(fa_http_cookies_for(j, "http://example.org/") == NULL);
    /* Replaced (same name, domain and path), then deleted by an expired one. */
    fa_http_cookies_take(j, "https://a.example.com/radio/x", "Set-Cookie: host=new\r\n");
    c = fa_http_cookies_for(j, "http://a.example.com/radio/x");
    CHECK_STR(c, "host=new; dom=2; later=7");
    free(c);
    fa_http_cookies_take(j, "https://a.example.com/radio/x", "Set-Cookie: host=; Expires=Sun, 06 Nov 1994 08:49:37 GMT\r\n"
                                                             "Set-Cookie: dom=; Domain=example.com; Path=/; Max-Age=-1\r\n");
    c = fa_http_cookies_for(j, "http://a.example.com/radio/x");
    CHECK_STR(c, "later=7");
    free(c);
    fa_http_cookies_destroy(j);
    /* The jar's line joins the app's Cookie line on its origin, and goes alone elsewhere. */
    {
        fa_http_request req;
        walk_text w;
        j = fa_http_cookies_create();
        fa_http_cookies_take(j, "http://h.com/a", "Set-Cookie: s=1; Domain=h.com\r\n");
        memset(&req, 0, sizeof(req));
        req.url = "http://h.com/a/b";
        req.range_start = -1;
        req.headers = "Cookie: app=1\r\nX-A: 1";
        req.cookies = j;
        memset(&w, 0, sizeof(w));
        CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
        CHECK_STR(w.text, "Accept=*/*|Accept-Encoding=identity|Cookie=app=1; s=1|X-A=1|");
        req.url = "http://cdn.h.com/seg.ts";
        req.other_origin = 1;
        memset(&w, 0, sizeof(w));
        CHECK_EQ(fa_http_each_header(&req, NULL, walk_add, &w), FA_OK);
        CHECK_STR(w.text, "Accept=*/*|Accept-Encoding=identity|X-A=1|Cookie=s=1|");
        fa_http_cookies_destroy(j);
    }
    /* A long jar line still fits the plain-socket client's request (6 KB of cookies). */
    {
        char big[3100], line[3200], url[256];
        fa_http_request req;
        conn_fetch c;
        memset(big, 'v', 3000);
        big[3000] = 0;
        j = fa_http_cookies_create();
        snprintf(line, sizeof(line), "Set-Cookie: a=%s\r\n", big);
        fa_http_cookies_take(j, "http://127.0.0.1/", line);
        snprintf(line, sizeof(line), "Set-Cookie: b=%s\r\n", big);
        fa_http_cookies_take(j, "http://127.0.0.1/", line);
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/echo-headers", g_port);
        memset(&c, 0, sizeof(c));
        memset(&req, 0, sizeof(req));
        req.url = url;
        req.range_start = -1;
        req.proxy = "";
        req.timeout_ms = 3000;
        req.cookies = j;
        CHECK_EQ(fa_http_fetch(&req, cf_head, cf_data, &c), FA_OK);
        CHECK(c.body.p != NULL && strstr(c.body.p, "Cookie: a=vvv") != NULL && strstr(c.body.p, "; b=vvv") != NULL);
        free(c.body.p);
        fa_http_cookies_destroy(j);
    }
}

/* An HLS stream behind a login redirect: the cookies the redirect and the playlist
 * set go back on every segment, joined with the app's own Cookie line. */
static void test_hls_cookies(void)
{
    fa_net_options net;
    fa_stream* s;
    fa_result r;
    fa_frames n = 0;
    float* got;
    fa_store32(&g_cookie_ok, 0);
    fa_store32(&g_cookie_bad, 0);
    fa_net_options_init(&net);
    net.headers = "Cookie: app=1";
    s = open_url_net("/hls/cookie-login", FA_OPEN_DECODE_ONLY, &net, &r);
    CHECK_EQ(r, FA_OK);
    got = read_all(s, &n);
    free(got);
    fa_stream_close(s);
    printf("  %d segment(s) let in, %d refused; last Cookie: %s\n", (int)fa_load32(&g_cookie_ok), (int)fa_load32(&g_cookie_bad), g_cookie_line);
    CHECK_EQ(fa_load32(&g_cookie_ok), HLS_SEGS);
    CHECK_EQ(fa_load32(&g_cookie_bad), 0);
    CHECK_STR(g_cookie_line, "app=1; sid=abc123; seen=1");
    CHECK(n > 44100);
}

#if defined(__ANDROID__)
/* HTTPS through HttpURLConnection: the transport, then the URL stream tests again
 * over HTTPS. Needs a Java VM, which the test starts itself; skipped where none
 * can run. */
static void test_android_https(void)
{
    int port = 0, before;
    void* vm = NULL;
    fetch_result f;
    CHECK_EQ(fa_android_set_java_vm(NULL), FA_ERROR_INVALID_ARG);
    /* No Java VM in the process yet: https is missing, http still works. */
    g_scheme = "https";
    CHECK_EQ(fetch("/tone.wav", &f), FA_ERROR_PROVIDER_MISSING);
    g_scheme = "http";
    if (tls_front_start(FA_TEST_DATA_DIR "/net/tls", g_port, &port, &vm) != 0) {
        CHECK(!real_system());   /* a device or emulator always has one */
        printf("  skipped (no Java VM here)\n");
        return;
    }
    printf("  https front on 127.0.0.1:%d\n", port);
    g_scheme = "https";
    g_url_port = port;
    /* Found by itself (JNI_GetCreatedJavaVMs) ... */
    CHECK_EQ(fetch("/tone.wav", &f), FA_OK);
    CHECK_EQ(f.bytes, g_wav_n);
    /* ... or set by the app. */
    CHECK_EQ(fa_android_set_java_vm(vm), FA_OK);
    before = g_failures;
    test_http_fetch();
    test_request_headers_sent();
    test_redirect_credentials();
    test_session_reuse();
    test_wav_over_http();
    test_mp3_over_http();
    test_no_ranges_and_chunked();
    test_redirect_and_errors();
    test_reconnect_resumes();
    test_icy_radio_decode_only();
    test_playlist_file_is_followed();   /* the playlist names an http:// station */
    test_stall_and_resume();
    test_hls_vod();
    test_hls_retry_and_byteranges();
    test_seek_after_giving_up();
    test_long_user_agent();
    test_close_while_streaming();
    test_hls_audio_groups();
    test_clean_close_reconnects();
    test_download_tap();
    test_hls_cookies();
    g_scheme = "http";
    g_url_port = g_port;
    CHECK_EQ(g_failures, before);
}
#endif

int main(void)
{
    fa_engine_config cfg;
    int i;
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    g_mp3 = read_file(FA_TEST_DATA_DIR "/net/tone_cbr.mp3", &g_mp3_n);
    g_mp3x = read_file(FA_TEST_DATA_DIR "/net/tone_xing.mp3", &g_mp3x_n);
    g_aac = read_file(FA_TEST_DATA_DIR "/net/tone.aac", &g_aac_n);
    g_ac3 = read_file(FA_TEST_DATA_DIR "/tone.ac3", &g_ac3_n);
    g_oggradio = read_file(FA_TEST_DATA_DIR "/ogg/radio.opus", &g_oggradio_n);
    g_wma = read_file(FA_TEST_DATA_DIR "/asf/tone.wma", &g_wma_n);
    g_asf_pcm = read_file(FA_TEST_DATA_DIR "/asf/pcm.asf", &g_asf_pcm_n);
    if (!g_mp3 || !g_mp3x || !g_aac || !g_ac3 || !g_oggradio || !g_wma || !g_asf_pcm) { fprintf(stderr, "missing test data in %s\n", FA_TEST_DATA_DIR "/net"); return 1; }
    make_wav();
    make_hls();
    make_hls_aac();
    make_hls_ac3();
    make_hls_map();
    g_aac_tagged_n = 30000 + g_aac_n;
    g_aac_tagged = (uint8_t*)calloc(1, g_aac_tagged_n);
    memcpy(g_aac_tagged, "ID3\x04\x00\x00", 6);   /* size 29990 (syncsafe), all padding */
    g_aac_tagged[7] = (uint8_t)(29990 >> 14); g_aac_tagged[8] = (uint8_t)((29990 >> 7) & 127); g_aac_tagged[9] = (uint8_t)(29990 & 127);
    memcpy(g_aac_tagged + 30000, g_aac, g_aac_n);

    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (server_start() != 0) { fprintf(stderr, "server start failed\n"); return 1; }
    printf("test server on 127.0.0.1:%d\n", g_port);
    g_url_port = g_port;

    RUN(test_url_parse);
    RUN(test_url_resolve);
    RUN(test_http_header_lookup);
    RUN(test_request_header_walk);
    RUN(test_hls_parse);
    RUN(test_aes_vectors);
    RUN(test_aes_reference);
    RUN(test_ts_demux);
    RUN(test_http_fetch);
    RUN(test_request_headers_sent);
    RUN(test_redirect_credentials);
    RUN(test_session_reuse);
    RUN(test_wav_over_http);
    RUN(test_mp3_over_http);
    RUN(test_no_ranges_and_chunked);
    RUN(test_redirect_and_errors);
    RUN(test_reconnect_resumes);
    RUN(test_icy_radio_decode_only);
    RUN(test_ogg_radio_titles);
    RUN(test_playlist_file_is_followed);
    RUN(test_icy_radio_attached);
    RUN(test_stall_and_resume);
    RUN(test_encoder_close_during_stall);
    RUN(test_hls_vod);
    RUN(test_hls_ac3);
    RUN(test_hls_retry_and_byteranges);
    RUN(test_hls_map_byterange);
    RUN(test_net_small_range_and_window);
    RUN(test_playlist_credentials);
    RUN(test_aac_over_http);
    RUN(test_aac_radio);
    RUN(test_asf_over_http);
    RUN(test_hls_aac);
    RUN(test_seek_after_giving_up);
    RUN(test_long_user_agent);
    RUN(test_close_while_streaming);
    RUN(test_hls_audio_groups);
    RUN(test_close_during_resolve);
    RUN(test_clean_close_reconnects);
    RUN(test_download_tap);
    RUN(test_cookie_jar);
    RUN(test_hls_cookies);
    RUN(test_https_optional);
    RUN(test_https_headers_optional);
    RUN(test_https_session_optional);
    RUN(test_https_hls_optional);
    RUN(test_https_framing_optional);
#if defined(__ANDROID__)
    RUN(test_android_https);
#endif

    fa_engine_destroy(g_engine);
    server_stop();
#if defined(__ANDROID__)
    tls_front_stop();
#endif
    for (i = 0; i < HLS_SEGS; i++) { free(g_seg[i].p); free(g_aseg[i].p); }
    free(g_ac3_seg[0].p);
    free(g_ac3_seg[1].p);
    free(g_ac3);
    free(g_oggradio);
    free(g_mp3);
    free(g_mp3x);
    free(g_pad);
    free(g_aac);
    free(g_aac_tagged);
    free(g_wma);
    free(g_asf_pcm);
    free(g_wav);
    printf("%d request(s) served\n", fa_load32(&g_requests));
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
