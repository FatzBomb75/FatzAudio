/* Web streams: URLs, HTTP transports, the download buffer behind URL streams,
 * ICY metadata and HLS. Everything that touches the network runs on a stream's
 * own download thread; decode workers only read bytes that are already here. */
#ifndef FA_NET_H
#define FA_NET_H

#include "fa_internal.h"

#define FA_NET_DEFAULT_TIMEOUT_MS 10000
#define FA_NET_DEFAULT_PREBUFFER_MS 1000
#define FA_NET_DEFAULT_RECONNECT_MS 30000
#define FA_NET_MAX_REDIRECTS 8
#define FA_NET_URL_MAX 2048

/* ---- URLs (net_url.c) ------------------------------------------------------ */

typedef struct fa_url {
    char scheme[8];          /* "http" or "https", lower case */
    char host[256];          /* without brackets for IPv6 literals */
    uint16_t port;           /* explicit or the scheme's default */
    int default_port;        /* 1 = the port was not in the URL */
    char userinfo[256];      /* "user:password" (still percent-encoded), "" = none */
    char path[FA_NET_URL_MAX]; /* path and query, always starts with '/'; no fragment */
} fa_url;

/* FA_OK, or FA_ERROR_INVALID_ARG for anything that is not an absolute http(s) URL. */
fa_result fa_url_parse(const char* text, fa_url* out);
/* Resolves `ref` (absolute, or relative as in an HLS playlist) against the absolute
 * URL `base` (RFC 3986 section 5). FA_ERROR_INVALID_ARG if it does not fit. */
fa_result fa_url_resolve(const char* base, const char* ref, char* out, size_t cap);
/* Base64 of `n` bytes into `out` (cap >= 4 * ((n + 2) / 3) + 1). */
void fa_base64(const uint8_t* in, size_t n, char* out);
/* Case-insensitive ASCII compare of at most n characters. */
int fa_strncasecmp_ascii(const char* a, const char* b, size_t n);

/* Value of header `name` in `headers` ("Name: value\r\n" lines), trimmed, into `out`.
 * Returns out, or NULL if absent. */
const char* fa_http_header(const char* headers, const char* name, char* out, size_t cap);

/* ---- HTTP transports (net_http.c, net_https_*.c) ---------------------------- */

typedef struct fa_http_request {
    const char* url;
    int64_t range_start;     /* -1 = no Range header */
    int icy;                 /* ask for inline ICY metadata (Icy-MetaData: 1) */
    const char* user_agent;
    const char* headers;     /* extra request header lines, NULL = none */
    const char* proxy;       /* NULL = system / environment, "" = direct */
    uint32_t timeout_ms;     /* connect, and the longest wait for data */
    fa_atomic32* cancel;     /* nonzero = stop as soon as possible */
} fa_http_request;

typedef struct fa_http_response {
    int status;              /* 200, 206, ... (also for "ICY 200 OK") */
    char* headers;           /* the response header lines, NUL-terminated; owned by the transport */
    int64_t content_length;  /* body bytes, -1 = unknown */
    int64_t range_start;     /* 206: first byte of the body in the resource, else 0 */
    int64_t total_length;    /* size of the whole resource, -1 = unknown */
    const char* final_url;   /* after redirects */
} fa_http_response;

/* Return 0 to go on, nonzero to stop the transfer (fa_http_fetch then returns FA_OK). */
typedef int (*fa_http_head_proc)(void* user, const fa_http_response* resp);
typedef int (*fa_http_data_proc)(void* user, const uint8_t* data, size_t n);

/* Blocking GET that streams the body to `on_data` on the calling thread, following
 * redirects. FA_OK when the body ended cleanly or a callback stopped it;
 * FA_ERROR_NETWORK for connection failures, timeouts and bodies cut short;
 * FA_ERROR_NOT_FOUND for HTTP 4xx/5xx (after on_head saw the status);
 * FA_ERROR_PROVIDER_MISSING when https has no transport on this system;
 * FA_ERROR_BUSY when `cancel` was raised. */
fa_result fa_http_fetch(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user);

/* The built-in HTTP/1.1 client on plain sockets, for http:// URLs. Sets
 * *out_redirect (malloc'd absolute URL) instead of calling on_head for a redirect. */
fa_result fa_http_fetch_socket(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data,
                               void* user, char** out_redirect);
/* The platform's HTTPS stack (WinHTTP, URLSession, libcurl loaded at runtime). It
 * follows redirects itself. */
fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user);

/* Called after raising `cancel`: interrupts a TLS transfer blocked in the system
 * stack (WinHTTP), so the download thread notices at once. */
void fa_http_abort(fa_atomic32* cancel);

/* "Basic <base64>" for a URL's percent-encoded "user:password". */
void fa_http_basic_auth(const char* userinfo, char* out, size_t cap);

/* Shared by the transports: parses a header block (status line excluded) into resp. */
void fa_http_fill_lengths(fa_http_response* resp);

/* ---- The download buffer behind a URL stream (net_source.c) ------------------ */

typedef struct fa_net fa_net;

#define FA_NET_META_MAX 16      /* ICY titles kept (heard and still buffered) */

typedef struct fa_net_meta_entry {
    int64_t byte;            /* audio byte offset where the title starts */
    int64_t frame;           /* source frame, -1 until the decoder got there */
    int64_t seq;
    char* title;
    char* url;
} fa_net_meta_entry;

typedef struct fa_net_config {
    char* url;
    char* user_agent;
    char* headers;
    char* proxy;             /* NULL = system */
    uint32_t timeout_ms, prebuffer_ms, reconnect_ms;
    uint32_t flags;          /* fa_net_flags */
    uint32_t hls_max_bandwidth;
} fa_net_config;

struct fa_net {
    fa_net_config cfg;
    fa_mutex lock;
    fa_cond cond;            /* data, space, or a state change */
    fa_thread thread;
    int thread_started;
    int thread_done;         /* the download thread has finished for good */
    fa_atomic32 cancel;      /* stops the transfer in progress (close, or a restart) */
    int quit;

    /* The window: absolute byte x of the resource is at buf[x % cap] while
     * win_start <= x < win_end. The reader is at read_pos. */
    uint8_t* buf;
    int64_t cap, ahead_limit;
    int64_t win_start, win_end, read_pos;
    int64_t consumed;        /* bytes handed to the decoder (seeks excluded) */
    int64_t frames;          /* frames the decoder made of them */

    /* The resource */
    int64_t length;          /* -1 = unknown */
    int ranges;              /* the server honours Range */
    int live;                /* no end: radio, live HLS */
    int icy;                 /* an Icecast/Shoutcast stream */
    int hls;                 /* an HLS playlist; hls_* below */
    int finished;            /* the download is over: complete, or given up */
    fa_result error;         /* why the last connection failed */
    int head_ready;          /* the first response is in */
    int restart;             /* the reader moved outside the window */
    int64_t restart_pos;
    int64_t skip;            /* bytes of the current response to drop (Range ignored) */
    int connections;         /* responses received */
    int failed_since;        /* a connection failed since the last good one */
    int starving;

    /* ICY: inline metadata every `metaint` audio bytes. */
    uint32_t metaint, icy_left;
    int meta_len;            /* -1 = expecting the length byte */
    int meta_got;
    char meta_buf[16 * 255 + 1];
    fa_net_meta_entry meta[FA_NET_META_MAX];
    int meta_count;
    int64_t meta_seq;

    /* Status */
    int connected, http_status;
    int64_t downloaded;
    uint32_t reconnects;
    char* headers;           /* of the last response */
    char final_url[FA_NET_URL_MAX];
    char content_type[128];
    uint32_t icy_br;
    uint32_t sample_rate;    /* of the decoded audio, set once the decoder is open */

    /* HLS (net_hls.c) */
    char* playlist;          /* the first response, collected while hls is set */
    size_t playlist_len;
    int hls_seek;            /* a seek to hls_seek_to seconds is pending */
    double hls_seek_to;
    double hls_seek_done;    /* where the restarted data begins, seconds */
    double hls_duration;     /* total seconds of a VOD playlist, -1 = live */
};

/* For the HLS producer: appends audio bytes to the window, waiting for room.
 * Returns 1 when the producer should stop (close, or a seek). */
int fa_net_write(fa_net* n, const uint8_t* data, size_t size);
/* Waits up to ms, returns 1 when the producer should stop. */
int fa_net_wait(fa_net* n, uint32_t ms);
/* Fills a request with the stream's settings. */
void fa_net_request(fa_net* n, fa_http_request* req, const char* url);
/* Downloads a small resource (a playlist, a key) into memory (NUL-terminated). */
fa_result fa_net_get_small(fa_net* n, const char* url, int64_t offset, int64_t length, char** out, size_t* out_len,
                           char* final_url, size_t final_cap);
/* HLS seek: restarts the producer at the segment holding `seconds`; waits until it
 * did and returns where the new data starts (seconds). Generation-relative reads. */
fa_result fa_net_hls_seek(fa_net* n, double seconds, double* out_start);

/* Opens `url` and waits until the response headers and the first bytes are in.
 * Takes ownership of the config's strings, also on failure. */
fa_result fa_net_open(const fa_net_config* cfg, fa_net** out);
/* Ends the download for good and wakes a reader waiting for bytes (its read
 * returns the end). For closing a stream while a worker may be reading. NULL ok. */
void fa_net_interrupt(fa_net* n);
void fa_net_close(fa_net* n);    /* stops the download thread, frees everything */
/* I/O callbacks over the downloaded bytes, for fa_source_open_io. seek is NULL
 * for live streams. Reads wait for the network; decode workers only call them
 * once fa_net_can_decode said enough is here. */
void fa_net_io(fa_net* n, fa_io_callbacks* out);
int fa_net_is_hls(const fa_net* n);

/* Decode workers: 1 when a decoder read will not wait for the network (enough is
 * buffered, or the download is over). While the stream is starving it waits for
 * the prebuffer amount first. Sets *out_starving to 1 while not enough is here. */
int fa_net_can_decode(fa_net* n, int* out_starving);
/* The decoder produced `frames` more source frames: refines the byte rate. */
void fa_net_note_decoded(fa_net* n, int64_t frames);

/* ICY metadata: a title change heard at source frame `frame`. */
typedef struct fa_net_meta {
    int64_t frame;           /* source frame where it starts */
    int64_t seq;             /* 1, 2, ... */
    char title[1024];        /* StreamTitle */
    char url[512];           /* StreamUrl */
} fa_net_meta;

/* Worker, after a decoder read: the decoder has turned the bytes up to `lag_bytes`
 * before the read position into source frames up to `frame_at_cursor`. Places
 * newly reached ICY blocks on the frame timeline. Returns the frame of the
 * earliest title that starts after `heard` (its seq in *out_seq), or INT64_MAX. */
int64_t fa_net_meta_update(fa_net* n, int64_t frame_at_cursor, int64_t lag_bytes, int64_t heard, int64_t* out_seq);
/* A title change at source frame `frame` that the audio itself announces (the
 * comments of a chained Ogg stream's next song). */
void fa_net_meta_add_frame(fa_net* n, int64_t frame, const char* title);
/* The decoder is open: its sample rate turns bytes per frame into a byte rate. */
void fa_net_set_sample_rate(fa_net* n, uint32_t rate);
/* The metadata in effect at source frame `heard` (seq 0 = none yet). */
void fa_net_meta_at(fa_net* n, int64_t heard, fa_net_meta* out);
/* Seq of the metadata in effect at `heard`. */
int64_t fa_net_meta_seq_at(fa_net* n, int64_t heard);
/* Forgets frame positions (after a seek: only live streams carry ICY data). */
void fa_net_meta_reset_frames(fa_net* n, int64_t frames);

/* Status for fa_stream_get_net_status (fills everything but buffered time). */
void fa_net_get_status(fa_net* n, fa_net_status* out);
fa_result fa_net_get_header(fa_net* n, const char* name, char* out, size_t cap);
fa_result fa_net_get_station(fa_net* n, fa_metadata* out);  /* icy-name etc.; title fields left alone */
/* Estimated bytes per source frame, 0 if not known yet. */
double fa_net_bytes_per_frame(fa_net* n);

/* ---- HLS (net_hls.c) -------------------------------------------------------- */

typedef struct fa_hls fa_hls;
/* Parses playlists: helpers exposed for tests. */
typedef struct fa_hls_segment {
    char* uri;               /* absolute */
    double duration;         /* seconds */
    double start;            /* seconds from the start of the playlist */
    int64_t seq;             /* media sequence number */
    int discontinuity;       /* EXT-X-DISCONTINUITY before it */
    int key_method;          /* 0 none, 1 AES-128 */
    char* key_uri;
    uint8_t iv[16];
    int has_iv;
    char* map_uri;           /* EXT-X-MAP init section, NULL = none */
    int64_t byte_offset, byte_length;   /* EXT-X-BYTERANGE, length -1 = whole */
} fa_hls_segment;

typedef struct fa_hls_playlist {
    int is_master;
    /* master */
    int variant_count;
    char** variant_uri;
    uint32_t* variant_bandwidth;
    char** variant_codecs;
    char* audio_group_uri;   /* first EXT-X-MEDIA TYPE=AUDIO with a URI (DEFAULT=YES preferred) */
    /* media */
    int segment_count;
    fa_hls_segment* segments;
    double target_duration;
    int64_t media_sequence;
    int endlist;             /* VOD, or a live event that has ended */
    double total_duration;
} fa_hls_playlist;

fa_result fa_hls_parse(const char* text, size_t n, const char* base_url, fa_hls_playlist* out);
void fa_hls_playlist_free(fa_hls_playlist* p);
/* Index of the variant to play: the highest bandwidth not above max_bandwidth
 * (0 = no limit), audio-only variants preferred when they exist. */
int fa_hls_pick_variant(const fa_hls_playlist* p, uint32_t max_bandwidth);

/* MPEG-TS to audio elementary stream (first audio PID of the first program). */
typedef struct fa_ts_demux {
    uint8_t carry[188];
    size_t carry_len;
    int pmt_pid, audio_pid;
    int stream_type;         /* of audio_pid: 0x03/0x04 MPEG audio, 0x0F ADTS AAC, 0x81 AC-3, 0x87 E-AC-3 */
    int pes_header_left;     /* bytes of the current PES header still to skip */
    uint8_t pes_head[32];    /* the start of a PES header split over packets */
    int pes_head_len;
    int in_pes;
} fa_ts_demux;

void fa_ts_init(fa_ts_demux* d);
/* Feeds TS bytes; audio payload goes to `out`. Returns 0, or nonzero when `out` asked to stop. */
int fa_ts_feed(fa_ts_demux* d, const uint8_t* data, size_t n, int (*out)(void* user, const uint8_t* p, size_t n), void* user);

/* AES-128 CBC decryption in place (n multiple of 16), for EXT-X-KEY METHOD=AES-128. */
void fa_aes128_cbc_decrypt(const uint8_t key[16], uint8_t iv[16], uint8_t* data, size_t n);

/* The decoder of HLS streams (wraps the decoder of the segments' format). */
extern const fa_decoder_vtable fa_decoder_hls;

/* The HLS producer: runs on the net's download thread instead of a single GET. */
fa_result fa_hls_run(fa_net* n, const char* playlist_text, size_t playlist_len, const char* playlist_url);

#endif
