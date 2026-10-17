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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    (void)req;
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

static void handle_request(sock_t s);

/* One connection: one request, then the server closes its side (the socket itself
 * is closed by server_stop, so the descriptor is never reused while listed). */
static void handle(void* arg)
{
    sock_t s = *(sock_t*)arg;
    free(arg);
    handle_request(s);
    shutdown(s, SHUT_BOTH);
}

static void handle_request(sock_t s)
{
    char req[4096], path[512];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        int k = (int)recv(s, req + got, (int)(sizeof(req) - 1 - got), 0);
        if (k <= 0) return;
        got += (size_t)k;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (sscanf(req, "GET %511s", path) != 1) return;
    fa_fetch_add32(&g_requests, 1);
    {
        const char* ua = strstr(req, "\r\nUser-Agent: ");
        if (ua) { ua += 14; snprintf(g_last_agent, sizeof(g_last_agent), "%.*s", (int)strcspn(ua, "\r\n"), ua); }
    }
    if (strcmp(path, "/tone.wav") == 0) serve_bytes(s, req, g_wav, g_wav_n, "audio/wav", 1, 0);
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
}

static void accept_main(void* arg)
{
    (void)arg;
    while (!fa_load32(&g_stop)) {
        sock_t c = accept(g_listen, NULL, NULL);
        sock_t* p;
        if (c == BAD_SOCK) continue;
        if (fa_load32(&g_stop)) { close_sock(c); break; }
        p = (sock_t*)malloc(sizeof(*p));
        *p = c;
        fa_mutex_lock(&g_conn_lock);
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

static void url_of(char* out, size_t cap, const char* path) { snprintf(out, cap, "http://127.0.0.1:%d%s", g_port, path); }

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
    CHECK(p.audio_group_uri != NULL);
    CHECK_STR(p.audio_group_uri, "http://h.com/m/audio/en.m3u8");
    CHECK_EQ(fa_hls_pick_variant(&p, 0), 0);
    CHECK_EQ(fa_hls_pick_variant(&p, 1000000), 1);
    fa_hls_playlist_free(&p);

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
    CHECK_EQ(fa_stream_get_net_status(s, &st), FA_OK);
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

#define AAC_FRAMES (131 * 1024)

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
    if (r == FA_ERROR_PROVIDER_MISSING) { printf("  (skipped: FFmpeg not found)\n"); return; }
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
    for (i = 0; i < AAC_FRAMES * 2 && i < n * 2; i++) err += fabs(got[i] - ref[i]);
    CHECK(err < 1e-3);
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
    if (r == FA_ERROR_PROVIDER_MISSING) { printf("  %s (skipped: FFmpeg not found)\n", path); return; }
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
    if (r == FA_ERROR_PROVIDER_MISSING) { printf("  (skipped: FFmpeg not found)\n"); return; }
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
    if (r == FA_ERROR_PROVIDER_MISSING) { printf("  (skipped: FFmpeg not found)\n"); return; }
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
    for (i = 0; i < 44100 * 2 && i < n * 2; i++) err += fabs(got[i] - ref[i]);
    CHECK(err < 1e-3);
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

    RUN(test_url_parse);
    RUN(test_url_resolve);
    RUN(test_http_header_lookup);
    RUN(test_hls_parse);
    RUN(test_aes_vectors);
    RUN(test_ts_demux);
    RUN(test_http_fetch);
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
    RUN(test_hls_vod);
    RUN(test_hls_ac3);
    RUN(test_hls_retry_and_byteranges);
    RUN(test_aac_over_http);
    RUN(test_aac_radio);
    RUN(test_asf_over_http);
    RUN(test_hls_aac);
    RUN(test_seek_after_giving_up);
    RUN(test_long_user_agent);
    RUN(test_close_while_streaming);
    RUN(test_https_optional);

    fa_engine_destroy(g_engine);
    server_stop();
    for (i = 0; i < HLS_SEGS; i++) { free(g_seg[i].p); free(g_aseg[i].p); }
    free(g_ac3_seg[0].p);
    free(g_ac3_seg[1].p);
    free(g_ac3);
    free(g_oggradio);
    free(g_mp3);
    free(g_mp3x);
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
