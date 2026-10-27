/* Ogg (RFC 3533) with Opus (RFC 7845), Vorbis or FLAC audio: .ogg, .oga, .opus
 * files and Ogg web radio. Pages are read and checked here; the packets go to the
 * built-in codecs (codec_xiph.c, codec_flac.c) through the shared track decoder
 * (demux.c).
 *
 * Timeline: a page's granule position is the end of the last packet that
 * completes on it. Packet times follow from packet durations (Opus: the TOC;
 * Vorbis: half of each pair of neighbouring block sizes; FLAC: the frame header's
 * block size), forward from the
 * previous page's granule while reading on, backward from the page's own granule
 * after a jump. The first audible frame of each link is time 0 of that link, so
 * an Opus pre-skip lands before 0 (the codec drops it, like Matroska's
 * CodecDelay), and the last page's granule trims the end.
 *
 * Chained streams (one song after another, each with its own headers, as Ogg
 * radio sends them) play through: the codec is opened again at each new link,
 * and on a web stream the new song's comments become its title. A seekable
 * chained file is read once at open so the length and seeking cover every link.
 * Multiplexed files (audio next to video) play their audio: `audio_track` picks
 * among the audio streams that start the file. */
#include "demux.h"
#include "net.h"
#include <vorbis/codec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OGG_HEADER 27
#define OGG_MAX_BODY (255 * 255)
#define OGG_MAX_PACKET (16u << 20)   /* larger: corrupt or not audio */
#define OGG_RESYNC_MAX (1 << 20)     /* bytes searched for the next page */
#define OGG_MAX_LINKS 4096
#define OGG_MAX_KEEP 4               /* packets a seek carries from one page to the next */
#define OGG_MAX_QUEUE (255 + OGG_MAX_KEEP)   /* packets completed on one page, plus those carried */
#define OGG_NO_GRAN INT64_MIN

enum { OGG_NONE = 0, OGG_OPUS, OGG_VORBIS, OGG_FLAC };

/* Ogg FLAC's first packet: 0x7F "FLAC", version 1.x, header count, "fLaC", then
 * the STREAMINFO block (4-byte block header, 34 bytes). */
#define OGG_FLAC_HEAD 51
#define OGG_FLAC_SI 17

typedef struct oggs_page {
    int64_t pos;            /* file offset */
    uint32_t size;          /* header + body */
    uint8_t flags;          /* 1 continued, 2 first (BOS), 4 last (EOS) */
    int64_t granule;        /* -1 = no packet completes here */
    uint32_t serial, seq;
    uint32_t nseg;
    uint8_t lace[255];
    uint32_t body_len;
} oggs_page;

typedef struct oggs_link {
    int64_t start;          /* file offset of its first page */
    int64_t data;           /* first page after its headers */
    int64_t end;            /* offset after its last page, -1 = unknown */
    uint32_t serial;
    int kind;
    uint32_t rate, channels, preskip;
    int long_block;         /* Vorbis: the long block size */
    int64_t first_gran;     /* granule where its first packet starts; OGG_NO_GRAN = not seen yet */
    int64_t last_gran;      /* granule of its last page, OGG_NO_GRAN = unknown */
    int64_t base;           /* pts of its first audible frame */
    uint16_t vbs[64];       /* Vorbis: block size of an audio packet by the 6 bits after its
                               type bit (its mode), 0 = no such mode */
    uint8_t* extra;         /* the codec's extradata: OpusHead, or the 3 Vorbis headers laced */
    size_t extra_len;
    fa_codec_params params;
} oggs_link;

typedef struct oggs_pkt {
    size_t off, len;
    int64_t pts, dur, trim;
} oggs_pkt;

typedef struct ogg {
    fa_br br;
    fa_track track;
    oggs_link** links;       /* each one allocated alone: pointers to them stay valid */
    int nlinks, cap_links;
    uint32_t group[16];     /* serials of the first link's logical streams */
    int ngroup;
    int want;               /* the audio stream picked in every link (audio_track) */
    int cur;                /* link being read */
    int codec_link;         /* link the codec is set up for */
    int enumerated;         /* every link is known (a seekable file) */
    int scanning;           /* reading the whole file at open: nothing is handed out */
    double rate0;           /* pts units per second (the first link's rate) */
    uint8_t* body;          /* the last page's body, grown to the largest page */
    size_t body_cap;
    int oom;                /* growing it failed: every later call fails (sticky) */
    /* The packet being put together across pages. */
    uint8_t* pend;
    size_t pend_len, pend_cap;
    int pend_valid;
    uint32_t last_seq;
    int have_seq;
    /* Packets completed on the current page. */
    uint8_t* qdata;
    size_t qdata_len, qdata_cap;
    oggs_pkt* q;            /* grows up to OGG_MAX_QUEUE */
    int q_cap;
    int nq, iq;
    int q_new;              /* the first packet of the latest page (the ones before were kept) */
    int keep;               /* packets the next page keeps at the front of the queue */
    int64_t gran;           /* granule after the last packet of the current link, OGG_NO_GRAN = unknown */
    int prev_bs;            /* Vorbis: block size of the previous packet, 0 = first of the link, -1 = unknown */
    int64_t next_pts;       /* pts after the last packet put in the queue */
    const fa_codec_params* reopen;   /* goes with the next packet handed out */
    fa_net* net;            /* web stream: song titles of chained links go here */
    oggs_page held;         /* a page read one too far, handed out again next (the */
    int has_held;           /* source may not be able to go back to it) */
    int at_end;
    int skip_back;          /* after a seek: packets that end before skip_to are */
    int64_t skip_to;        /* dropped undecoded, but for the last skip_back of them; */
    int skipping;           /* off once the first packet to decode is reached */
} ogg;

/* ---- pages ------------------------------------------------------------------------------ */

/* CRC-32 (polynomial 0x04C11DB7, not reflected) four bytes at a time: crctab[k][i]
 * is byte i followed by k zero bytes. Shared by every stream, computed once. */
static uint32_t crctab[4][256];
static fa_atomic32 crc_state;     /* 0 not done, 1 in progress, 2 done */

static void crc_init(void)
{
    uint32_t i, j, r;
    if (fa_load32(&crc_state) == 2) return;
    if (!fa_cas32(&crc_state, 0, 1)) {
        while (fa_load32(&crc_state) != 2) fa_sleep_ms(1);
        return;
    }
    for (i = 0; i < 256; i++) {
        r = i << 24;
        for (j = 0; j < 8; j++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
        crctab[0][i] = r;
    }
    for (j = 1; j < 4; j++)
        for (i = 0; i < 256; i++) crctab[j][i] = (crctab[j - 1][i] << 8) ^ crctab[0][crctab[j - 1][i] >> 24];
    fa_store32(&crc_state, 2);
}

static uint32_t crc_update(uint32_t crc, const uint8_t* p, size_t n)
{
    for (; n >= 4; n -= 4, p += 4) {
        crc ^= ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        crc = crctab[3][crc >> 24] ^ crctab[2][(crc >> 16) & 0xFF] ^ crctab[1][(crc >> 8) & 0xFF] ^ crctab[0][crc & 0xFF];
    }
    while (n--) crc = (crc << 8) ^ crctab[0][((crc >> 24) ^ *p++) & 0xFF];
    return crc;
}

/* Reads the page at `pos`, whose first `have` header bytes are already in h, into
 * pg and o->body. 1 = a valid page, 0 = not one (the reader is left wherever
 * reading stopped), -1 = the data ends first, -2 = out of memory. */
static int page_rest(ogg* o, uint8_t* h, size_t have, int64_t pos, oggs_page* pg)
{
    uint32_t i, crc;
    size_t got = fa_br_read(&o->br, h + have, OGG_HEADER - have);
    if (got < OGG_HEADER - have) return -1;
    if (memcmp(h, "OggS", 4) != 0 || h[4] != 0) return 0;
    pg->nseg = h[26];
    if (fa_br_read(&o->br, h + OGG_HEADER, pg->nseg) < pg->nseg) return -1;
    pg->body_len = 0;
    for (i = 0; i < pg->nseg; i++) pg->body_len += h[OGG_HEADER + i];
    if (pg->body_len > o->body_cap || !o->body) {   /* never NULL, also for an empty page */
        size_t cap = (pg->body_len + 4096) & ~(size_t)4095;   /* whole 4 KB steps, not doubling; never 0 */
        if (cap > OGG_MAX_BODY) cap = OGG_MAX_BODY;
        free(o->body);                              /* nothing in it is needed any more */
        o->body = (uint8_t*)malloc(cap);
        o->body_cap = o->body ? cap : 0;
        if (!o->body) { o->oom = 1; return -2; }
    }
    if (fa_br_read(&o->br, o->body, pg->body_len) < pg->body_len) return -1;
    crc = fa_le32(h + 22);
    memset(h + 22, 0, 4);
    if (crc_update(crc_update(0, h, OGG_HEADER + pg->nseg), o->body, pg->body_len) != crc) return 0;
    pg->pos = pos;
    pg->size = OGG_HEADER + pg->nseg + pg->body_len;
    pg->flags = h[5];
    pg->granule = (int64_t)fa_le64(h + 6);
    pg->serial = fa_le32(h + 14);
    pg->seq = fa_le32(h + 18);
    memcpy(pg->lace, h + OGG_HEADER, pg->nseg);
    return 1;
}

/* The next valid page at or after the reader's position, searching at most
 * `limit` bytes. FA_ERROR_END_OF_STREAM when there is none, FA_ERROR_OUT_OF_MEMORY
 * when its body didn't fit and the buffer couldn't grow (o->oom: the scans that
 * stop on any error leave it to the entry points to report). */
static fa_result next_page(ogg* o, oggs_page* pg, int64_t limit)
{
    uint8_t h[OGG_HEADER + 255];
    int64_t start = fa_br_tell(&o->br), pos = start;
    int r;
    if (o->has_held) {
        *pg = o->held;           /* its body is still in o->body */
        o->has_held = 0;
        return FA_OK;
    }
    r = page_rest(o, h, 0, pos, pg);
    while (r == 0) {
        uint32_t win = 0;
        /* Not a page: on to the next "OggS", one byte after the false start where
         * the source can go back there, else from where reading stopped. */
        fa_br_seek(&o->br, pos + 1);
        for (;;) {
            fa_br* b = &o->br;
            uint8_t c;
            if (b->off < b->len && b->base + (int64_t)b->off - start <= limit) c = b->buf[b->off++];
            else if (fa_br_tell(b) - start > limit || fa_br_read(b, &c, 1) < 1) return FA_ERROR_END_OF_STREAM;
            win = (win << 8) | c;
            if (win == 0x4F676753u) break;     /* "OggS" */
        }
        pos = fa_br_tell(&o->br) - 4;
        memcpy(h, "OggS", 4);
        r = page_rest(o, h, 4, pos, pg);
    }
    return r > 0 ? FA_OK : r == -2 ? FA_ERROR_OUT_OF_MEMORY : FA_ERROR_END_OF_STREAM;
}

/* Moves the reader; a held page is forgotten. */
static fa_result seek_to(ogg* o, int64_t pos)
{
    o->has_held = 0;
    return fa_br_seek(&o->br, pos);
}

/* ---- packets ----------------------------------------------------------------------------- */

static int pend_add(ogg* o, const uint8_t* p, size_t n)
{
    if (o->pend_len + n > OGG_MAX_PACKET) return 0;
    if (o->pend_len + n > o->pend_cap) {
        size_t cap = o->pend_cap ? o->pend_cap : 8192;
        uint8_t* b;
        while (cap < o->pend_len + n) cap *= 2;
        b = (uint8_t*)realloc(o->pend, cap);
        if (!b) return 0;
        o->pend = b;
        o->pend_cap = cap;
    }
    memcpy(o->pend + o->pend_len, p, n);
    o->pend_len += n;
    return 1;
}

/* Splits the page's body into packets: completed ones go to the queue, an
 * unfinished last one waits in o->pend. */
static fa_result page_packets(ogg* o, const oggs_page* pg)
{
    size_t pos = 0;
    uint32_t i;
    {
        /* Room for the kept packets and one per segment, made before anything moves. */
        int need = (o->keep > 0 && o->keep <= o->nq ? o->keep : 0) + pg->nseg;
        if (need > OGG_MAX_QUEUE) need = OGG_MAX_QUEUE;
        if (need > o->q_cap) {
            int cap = o->q_cap ? o->q_cap : 32;
            oggs_pkt* b;
            while (cap < need) cap *= 2;
            if (cap > OGG_MAX_QUEUE) cap = OGG_MAX_QUEUE;
            b = (oggs_pkt*)realloc(o->q, sizeof(oggs_pkt) * (size_t)cap);
            if (!b) { o->oom = 1; return FA_ERROR_OUT_OF_MEMORY; }
            o->q = b;
            o->q_cap = cap;
        }
    }
    if (o->keep > 0 && o->keep <= o->nq) {
        /* Kept from the page before: moved to the front, data and all. */
        int i0 = o->nq - o->keep, j;
        size_t from = o->q[i0].off;
        memmove(o->qdata, o->qdata + from, o->qdata_len - from);
        memmove(o->q, o->q + i0, sizeof(oggs_pkt) * (size_t)o->keep);
        for (j = 0; j < o->keep; j++) o->q[j].off -= from;
        o->nq = o->keep;
        o->qdata_len -= from;
    } else {
        o->nq = 0;
        o->qdata_len = 0;
    }
    o->q_new = o->nq;
    o->iq = 0;
    o->keep = 0;
    if (o->have_seq && pg->seq != o->last_seq + 1) o->pend_valid = 0;    /* a page is missing */
    o->last_seq = pg->seq;
    o->have_seq = 1;
    if (!(pg->flags & 1) || !o->pend_valid) {
        /* Not continued from a packet we hold: a continuation is dropped. */
        int skip = (pg->flags & 1) != 0;
        o->pend_len = 0;
        o->pend_valid = 1;
        if (skip) {
            for (i = 0; i < pg->nseg; i++) {
                pos += pg->lace[i];
                if (pg->lace[i] < 255) { i++; break; }
            }
            if (i == pg->nseg && (pg->nseg == 0 || pg->lace[pg->nseg - 1] == 255)) {
                o->pend_valid = 0;               /* still inside the lost packet */
                return FA_OK;
            }
        } else {
            i = 0;
        }
    } else {
        i = 0;
    }
    for (; i < pg->nseg; i++) {
        if (!pend_add(o, o->body + pos, pg->lace[i])) {
            if (o->pend_len + pg->lace[i] > OGG_MAX_PACKET) { o->pend_len = 0; o->pend_valid = 0; return FA_OK; }
            return FA_ERROR_OUT_OF_MEMORY;
        }
        pos += pg->lace[i];
        if (pg->lace[i] < 255) {
            oggs_pkt* q;
            if (o->nq == OGG_MAX_QUEUE) return FA_ERROR_UNSUPPORTED_FORMAT;
            if (o->qdata_len + o->pend_len > o->qdata_cap) {
                /* Room for the rest of the page too: one realloc a page at most. */
                size_t cap = (o->qdata_len + o->pend_len + (pg->body_len - pos) + 4095) & ~(size_t)4095;
                uint8_t* b;
                b = (uint8_t*)realloc(o->qdata, cap);
                if (!b) return FA_ERROR_OUT_OF_MEMORY;
                o->qdata = b;
                o->qdata_cap = cap;
            }
            q = &o->q[o->nq++];
            memset(q, 0, sizeof(*q));
            q->off = o->qdata_len;
            q->len = o->pend_len;
            if (o->pend_len) memcpy(o->qdata + o->qdata_len, o->pend, o->pend_len);
            o->qdata_len += o->pend_len;
            o->pend_len = 0;
        }
    }
    return FA_OK;
}

/* Samples a packet adds to the timeline; -1 = not known (a Vorbis packet whose
 * neighbour before it was not seen). Moves the Vorbis block size state on. */
static int64_t packet_duration(ogg* o, oggs_link* l, const uint8_t* d, size_t n)
{
    if (l->kind == OGG_OPUS) {
        uint32_t rate = 0;
        int64_t s = fa_demux_packet_samples("opus", l->channels, 48000, d, n, &rate);
        return s < 0 ? 0 : s;       /* a corrupt packet decodes to nothing */
    } else if (l->kind == OGG_FLAC) {
        uint32_t rate = 0;
        int64_t s = fa_demux_packet_samples("flac", l->channels, l->rate, d, n, &rate);
        return s < 0 ? 0 : s;       /* metadata or a corrupt frame: no samples */
    } else {
        int64_t dur;
        int bs = n > 0 && !(d[0] & 1) ? l->vbs[(d[0] >> 1) & 63] : 0;
        if (bs <= 0) return 0;      /* not an audio packet: no samples */
        dur = o->prev_bs < 0 ? -1 : o->prev_bs == 0 ? 0 : (o->prev_bs + bs) / 4;
        o->prev_bs = (int)bs;
        return dur;
    }
}

/* Puts times on the packets of a page that belongs to link `l`. */
static void time_packets(ogg* o, oggs_link* l, const oggs_page* pg)
{
    int64_t g = pg->granule, sum = 0, at;
    double scale = o->rate0 / (double)l->rate;
    int i, known = 1;
    for (i = o->q_new; i < o->nq; i++) {
        oggs_pkt* q = &o->q[i];
        q->dur = packet_duration(o, l, o->qdata + q->off, q->len);
        if (q->dur < 0) known = 0;
        else sum += q->dur;
    }
    if (o->nq == o->q_new) return;
    /* A link's first page with more audio than its granule: the stream starts at
     * 0 and (on its last page) the end is trimmed. */
    if (o->gran == OGG_NO_GRAN && l->first_gran == OGG_NO_GRAN && known && g >= 0 && g < sum) o->gran = 0;
    if (o->gran != OGG_NO_GRAN && (g < 0 || (pg->flags & 4) || g <= o->gran + sum)) {
        /* Forward from where the last page ended; the last page's granule trims. */
        at = o->gran;
        for (i = o->q_new; i < o->nq; i++) {
            oggs_pkt* q = &o->q[i];
            int64_t d = q->dur < 0 ? 0 : q->dur;
            q->pts = at;
            if ((pg->flags & 4) && g >= 0 && at + d > g) q->trim = at + d - (g > at ? g : at);
            at += d;
        }
        o->gran = g >= 0 ? g : at;
    } else if (g >= 0) {
        /* Backward from this page's granule. */
        at = g;
        for (i = o->nq - 1; i >= o->q_new; i--) {
            oggs_pkt* q = &o->q[i];
            if (q->dur < 0) { q->pts = OGG_NO_GRAN; continue; }
            at -= q->dur;
            q->pts = at;
        }
        if (l->first_gran == OGG_NO_GRAN && known) l->first_gran = at;
        o->gran = g;
    } else {
        for (i = o->q_new; i < o->nq; i++) o->q[i].pts = OGG_NO_GRAN;
        return;
    }
    if (l->first_gran == OGG_NO_GRAN) l->first_gran = o->q[o->q_new].pts != OGG_NO_GRAN ? o->q[o->q_new].pts : g;
    for (i = o->q_new; i < o->nq; i++) {
        oggs_pkt* q = &o->q[i];
        if (q->pts == OGG_NO_GRAN) { q->pts = FA_DEMUX_NO_PTS; continue; }
        q->pts = l->base + (int64_t)llround((double)(q->pts - l->first_gran - (int64_t)l->preskip) * scale);
        q->trim = (int64_t)llround((double)q->trim * scale);
        if (q->dur >= 0) {
            q->dur = (int64_t)llround((double)q->dur * scale);
            o->next_pts = q->pts + q->dur - q->trim;
        }
    }
}

/* Parses the three Vorbis headers for the link's format and its mode table, then
 * lets libvorbis's copy go (the codec parses its own): only the block size of
 * each mode is kept. libvorbis reads a packet's mode from the bits right after
 * its type bit, at most 6 of them (64 modes), all in the first byte; asking it
 * about a one-byte packet for each of the 64 values gives the table. */
static fa_result vorbis_setup(oggs_link* l, uint8_t* const hdr[3], const size_t hlen[3])
{
    vorbis_info vi;
    vorbis_comment vc;
    fa_result r = FA_OK;
    int i;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    for (i = 0; i < 3 && r == FA_OK; i++) {
        ogg_packet op;
        memset(&op, 0, sizeof(op));
        op.packet = hdr[i];
        op.bytes = (long)hlen[i];
        op.b_o_s = i == 0;
        op.packetno = i;
        if (hlen[i] > 0x7FFFFFFF || vorbis_synthesis_headerin(&vi, &vc, &op) != 0) r = FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (r == FA_OK && (vi.rate <= 0 || vi.channels <= 0)) r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (r == FA_OK) {
        l->rate = (uint32_t)vi.rate;
        l->channels = (uint32_t)vi.channels;
        l->long_block = vorbis_info_blocksize(&vi, 1);
        for (i = 0; i < 64; i++) {
            unsigned char b = (unsigned char)(i << 1);
            ogg_packet op;
            long bs;
            memset(&op, 0, sizeof(op));
            op.packet = &b;
            op.bytes = 1;
            bs = vorbis_packet_blocksize(&vi, &op);
            l->vbs[i] = (uint16_t)(bs > 0 && bs <= 65535 ? bs : 0);
        }
    }
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return r;
}

/* ---- links -------------------------------------------------------------------------------- */

static void link_free(oggs_link* l)
{
    free(l->extra);
    memset(l, 0, sizeof(*l));
}

static void links_free(ogg* o, int from, int to)
{
    int i;
    for (i = from; i < to; i++) {
        link_free(o->links[i]);
        free(o->links[i]);
        o->links[i] = NULL;
    }
}

static int kind_of(const uint8_t* p, size_t n)
{
    if (n >= 19 && memcmp(p, "OpusHead", 8) == 0) return OGG_OPUS;
    if (n >= 30 && p[0] == 1 && memcmp(p + 1, "vorbis", 6) == 0) return OGG_VORBIS;
    if (n >= OGG_FLAC_HEAD && p[0] == 0x7F && memcmp(p + 1, "FLAC", 4) == 0 && p[5] == 1 &&
        memcmp(p + 9, "fLaC", 4) == 0 && (p[13] & 0x7F) == 0)
        return OGG_FLAC;
    return OGG_NONE;
}

/* "ARTIST - TITLE" from a comment packet (after its magic), for web radio.
 * Field names are case-insensitive. */
static void comment_title(const uint8_t* p, size_t n, char* out, size_t cap)
{
    char artist[256] = "", title[512] = "";
    size_t pos;
    uint32_t count, i;
    out[0] = '\0';
    if (n < 8) return;
    pos = 4 + (size_t)fa_le32(p);
    if (pos < 4 || pos + 4 > n) return;
    count = fa_le32(p + pos);
    pos += 4;
    for (i = 0; i < count && pos + 4 <= n; i++) {
        uint32_t len = fa_le32(p + pos);
        const char* f = (const char*)p + pos + 4;
        pos += 4;
        if (len > n - pos) return;
        if (len > 7 && !artist[0] && fa_strncasecmp_ascii(f, "ARTIST=", 7) == 0)
            snprintf(artist, sizeof(artist), "%.*s", (int)(len - 7), f + 7);
        else if (len > 6 && !title[0] && fa_strncasecmp_ascii(f, "TITLE=", 6) == 0)
            snprintf(title, sizeof(title), "%.*s", (int)(len - 6), f + 6);
        pos += len;
    }
    if (artist[0] && title[0]) snprintf(out, cap, "%s - %s", artist, title);
    else snprintf(out, cap, "%s", title[0] ? title : artist);
}

/* Reads a link's headers, starting at its BOS page group at the reader's
 * position: picks audio stream `want` (0-based) among the group's audio streams,
 * then collects its header packets. *out_audio = the group's audio stream count.
 * A later link with fewer audio streams plays its first one; the first link
 * returns FA_ERROR_NOT_FOUND then. */
static fa_result read_link(ogg* o, oggs_link* l, int want, int* out_audio, fa_tag_sink* tags)
{
    oggs_page pg;
    uint8_t* hdr[3] = { NULL, NULL, NULL };
    size_t hlen[3] = { 0, 0, 0 };
    int nh = 0, need = 0, audio = 0, picked = 0, i, flac_done = 0, flac_blocks = 0;
    char title[1024];
    fa_result r;
    memset(l, 0, sizeof(*l));
    l->first_gran = l->last_gran = OGG_NO_GRAN;
    l->end = -1;
    l->start = -1;
    title[0] = '\0';
    /* The BOS pages: one per logical stream, each with exactly one packet. */
    for (;;) {
        r = next_page(o, &pg, OGG_RESYNC_MAX);
        if (r == FA_ERROR_OUT_OF_MEMORY) goto done;
        if (r != FA_OK) break;
        if (!(pg.flags & 2)) {
            o->held = pg;
            o->has_held = 1;
            break;
        }
        if (l->start < 0) l->start = pg.pos;          /* the group's first page */
        if (o->nlinks == 1 && o->ngroup < 16) o->group[o->ngroup++] = pg.serial;
        if (pg.nseg == 0 || pg.lace[pg.nseg - 1] == 255) continue;
        if (kind_of(o->body, pg.body_len) != OGG_NONE) {
            if (audio++ == want || (picked == 0 && o->nlinks > 1 && audio == 1)) {
                free(hdr[0]);
                l->serial = pg.serial;
                l->kind = kind_of(o->body, pg.body_len);
                hdr[0] = (uint8_t*)malloc(pg.body_len);
                if (!hdr[0]) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
                memcpy(hdr[0], o->body, pg.body_len);
                hlen[0] = pg.body_len;
                nh = 1;
                picked = audio - 1 == want ? 2 : 1;   /* 1 = the fallback for now */
            }
        }
    }
    if (out_audio) *out_audio = audio;
    if (!picked) { r = audio > 0 ? FA_ERROR_NOT_FOUND : FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
    need = l->kind == OGG_OPUS ? 2 : 3;
    /* FLAC: metadata blocks, one per packet, up to the one marked last. */
    if (l->kind == OGG_FLAC) flac_done = (hdr[0][13] & 0x80) != 0;
    /* The other headers of our stream (other streams' pages are skipped). */
    o->pend_len = 0;
    o->pend_valid = 1;
    o->have_seq = 0;
    while (l->kind == OGG_FLAC ? !flac_done : nh < need) {
        r = next_page(o, &pg, OGG_RESYNC_MAX);
        if (r != FA_OK) { if (r != FA_ERROR_OUT_OF_MEMORY) r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
        if (pg.serial != l->serial) continue;
        r = page_packets(o, &pg);
        if (r != FA_OK) goto done;
        if (l->kind == OGG_FLAC) {
            for (i = 0; i < o->nq && !flac_done; i++) {
                const uint8_t* d = o->qdata + o->q[i].off;
                size_t len = o->q[i].len;
                if (len == 0 || d[0] == 0xFF) { flac_done = 1; i--; break; }   /* audio: no last mark */
                if (len < 4 || ++flac_blocks > 1024) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
                flac_done = (d[0] & 0x80) != 0;
                if ((d[0] & 0x7F) == 4 && !hdr[1]) {         /* VORBIS_COMMENT */
                    hdr[1] = (uint8_t*)malloc(len);
                    if (!hdr[1]) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
                    memcpy(hdr[1], d, len);
                    hlen[1] = len;
                }
            }
            if (flac_done && i < o->nq) {
                /* Audio on the page that ends the headers (against the mapping,
                 * but written): the page is read again as the first data page,
                 * where its metadata packets decode to nothing. */
                o->held = pg;
                o->has_held = 1;
                o->pend_len = 0;
                o->have_seq = 0;
                l->data = pg.pos;
            }
            continue;
        }
        for (i = 0; i < o->nq && nh < need; i++) {
            hdr[nh] = (uint8_t*)malloc(o->q[i].len ? o->q[i].len : 1);
            if (!hdr[nh]) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
            memcpy(hdr[nh], o->qdata + o->q[i].off, o->q[i].len);
            hlen[nh++] = o->q[i].len;
        }
    }
    o->nq = o->iq = 0;
    if (!o->has_held) l->data = fa_br_tell(&o->br);
    if (l->kind == OGG_OPUS) {
        const uint8_t* h = hdr[0];
        l->channels = h[9];
        l->preskip = (uint32_t)h[10] | ((uint32_t)h[11] << 8);
        l->rate = 48000;
        l->extra = hdr[0];
        l->extra_len = hlen[0];
        hdr[0] = NULL;
        if (hlen[1] >= 8 && memcmp(hdr[1], "OpusTags", 8) == 0) {
            comment_title(hdr[1] + 8, hlen[1] - 8, title, sizeof(title));
            if (tags) fa_tags_parse_vorbis_comments(hdr[1] + 8, hlen[1] - 8, tags);
        }
        l->params.codec = "opus";
    } else if (l->kind == OGG_FLAC) {
        const uint8_t* si = hdr[0] + OGG_FLAC_SI;
        fa_flac_streaminfo_format(si, &l->rate, &l->channels, &l->params.bits_per_coded_sample);
        if (l->rate == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
        l->extra = (uint8_t*)malloc(34);
        if (!l->extra) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
        memcpy(l->extra, si, 34);
        l->extra_len = 34;
        if (hlen[1] > 4) {
            comment_title(hdr[1] + 4, hlen[1] - 4, title, sizeof(title));
            if (tags) fa_tags_parse_vorbis_comments(hdr[1] + 4, hlen[1] - 4, tags);
        }
        l->params.codec = "flac";
    } else {
        size_t n = 1, k;
        uint8_t* e;
        r = vorbis_setup(l, hdr, hlen);
        if (r != FA_OK) goto done;
        if (hlen[1] > 7) {
            comment_title(hdr[1] + 7, hlen[1] - 7, title, sizeof(title));
            if (tags) fa_tags_parse_vorbis_comments(hdr[1] + 7, hlen[1] - 7, tags);
        }
        /* Xiph lacing: 2, the sizes of the first two, then all three. */
        for (i = 0; i < 2; i++) n += hlen[i] / 255 + 1;
        e = (uint8_t*)malloc(n + hlen[0] + hlen[1] + hlen[2]);
        if (!e) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
        k = 0;
        e[k++] = 2;
        for (i = 0; i < 2; i++) {
            size_t v = hlen[i];
            while (v >= 255) { e[k++] = 255; v -= 255; }
            e[k++] = (uint8_t)v;
        }
        for (i = 0; i < 3; i++) { memcpy(e + k, hdr[i], hlen[i]); k += hlen[i]; }
        l->extra = e;
        l->extra_len = k;
        l->params.codec = "vorbis";
    }
    l->params.struct_size = sizeof(l->params);
    l->params.sample_rate = l->rate;
    l->params.channels = l->channels;
    l->params.extradata = l->extra;
    l->params.extradata_size = l->extra_len;
    if (o->net && title[0] && !o->scanning) fa_net_meta_add_frame(o->net, o->next_pts > 0 ? o->next_pts : 0, title);
    r = FA_OK;
done:
    for (i = 0; i < 3; i++) free(hdr[i]);
    if (r != FA_OK) link_free(l);
    return r;
}

static fa_result add_link(ogg* o, oggs_link** out)
{
    if (o->nlinks == o->cap_links) {
        int cap = o->cap_links ? o->cap_links * 2 : 4;
        oggs_link** p;
        if (cap > OGG_MAX_LINKS) return FA_ERROR_UNSUPPORTED_FORMAT;
        p = (oggs_link**)realloc(o->links, sizeof(*p) * (size_t)cap);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        o->links = p;
        o->cap_links = cap;
    }
    *out = (oggs_link*)calloc(1, sizeof(oggs_link));
    if (!*out) return FA_ERROR_OUT_OF_MEMORY;
    o->links[o->nlinks++] = *out;
    return FA_OK;
}

/* Forgets a link read_link could not use (the last one added). */
static void drop_last_link(ogg* o)
{
    links_free(o, o->nlinks - 1, o->nlinks);
    o->nlinks--;
}

static int64_t link_length(const ogg* o, const oggs_link* l)
{
    if (l->first_gran == OGG_NO_GRAN || l->last_gran == OGG_NO_GRAN) return -1;
    {
        int64_t n = l->last_gran - l->first_gran - (int64_t)l->preskip;
        if (n < 0) n = 0;
        return (int64_t)llround((double)n * o->rate0 / (double)l->rate);
    }
}

/* ---- reading ----------------------------------------------------------------------------- */

/* Reads pages until the queue holds packets of the current link. */
static fa_result fill_queue(ogg* o)
{
    for (;;) {
        oggs_page pg;
        oggs_link* l = o->links[o->cur];
        fa_result r;
        if (o->iq < o->nq) return FA_OK;
        if (o->at_end) return FA_ERROR_END_OF_STREAM;
        r = next_page(o, &pg, OGG_RESYNC_MAX);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
        if (r != FA_OK) { o->at_end = 1; return FA_ERROR_END_OF_STREAM; }
        if (pg.flags & 2) {
            /* The next link of a chained stream. */
            int known = -1, i;
            o->keep = 0;            /* a new song: nothing of the last one is decoded */
            o->skipping = 0;
            for (i = o->cur + 1; i < o->nlinks; i++)
                if (o->links[i]->start == pg.pos) { known = i; break; }
            if (known >= 0) {
                o->cur = known;
                r = seek_to(o, o->links[known]->data);
                if (r != FA_OK) return r;
            } else {
                oggs_link* nl;
                o->held = pg;
                o->has_held = 1;
                r = add_link(o, &nl);
                if (r != FA_OK) return r;
                r = read_link(o, nl, o->want, NULL, NULL);
                if (r == FA_ERROR_OUT_OF_MEMORY) return r;
                if (r != FA_OK) {
                    /* Not audio we know: its pages are skipped. */
                    drop_last_link(o);
                    continue;
                }
                nl->base = o->next_pts;
                if (!o->enumerated && !o->scanning) {
                    /* A stream that cannot seek back keeps only the link it plays. */
                    links_free(o, 0, o->nlinks - 1);
                    o->links[0] = nl;
                    o->nlinks = 1;
                }
                o->cur = o->nlinks - 1;
            }
            o->gran = OGG_NO_GRAN;
            o->prev_bs = 0;
            o->pend_len = 0;
            o->pend_valid = 1;
            o->have_seq = 0;
            o->codec_link = o->cur;
            o->reopen = &o->links[o->cur]->params;
            continue;
        }
        if (pg.serial != l->serial) continue;
        r = page_packets(o, &pg);
        if (r != FA_OK) return r;
        time_packets(o, l, &pg);
        /* The last granule seen; a file's last page was read at open. */
        if (pg.granule >= 0 && (o->scanning || !o->enumerated)) l->last_gran = pg.granule;
    }
}

static fa_result ogg_next(void* ctx, fa_demux_packet* out)
{
    ogg* o = (ogg*)ctx;
    oggs_pkt* q;
    fa_result r = o->oom ? FA_ERROR_OUT_OF_MEMORY : fill_queue(o);
    if (r != FA_OK) return r;
    /* A seek lands on the page before the target, up to a second of audio; the
     * packets more than skip_back before the one holding it are never decoded.
     * Where a page ends before the target, its last skip_back packets are carried
     * to the next page, which tells whether they are needed. */
    while (o->skipping) {
        const oggs_pkt* k = o->iq + o->skip_back < o->nq ? &o->q[o->iq + o->skip_back] : NULL;
        if (k) {
            if (k->pts == FA_DEMUX_NO_PTS || k->dur < 0 || k->pts + k->dur > o->skip_to) o->skipping = 0;
            else o->iq++;
        } else {
            const oggs_pkt* e = &o->q[o->nq - 1];
            if (e->pts == FA_DEMUX_NO_PTS || e->dur < 0 || e->pts + e->dur > o->skip_to) {
                o->skipping = 0;
                break;
            }
            o->keep = o->nq - o->iq;
            o->iq = o->nq;
            r = fill_queue(o);
            if (r != FA_OK) return r;
        }
    }
    q = &o->q[o->iq++];
    out->data = o->qdata + q->off;
    out->size = q->len;
    out->pts = q->pts;
    out->duration = q->dur;
    out->trim_end = q->trim;
    out->reopen = o->reopen;
    o->reopen = NULL;
    return FA_OK;
}

/* The last page of `serial` that starts before `hi`, with a granule below `g`
 * (from a binary search over pages); -1 = none after `lo`. */
static int64_t find_page_before(ogg* o, uint32_t serial, int64_t lo, int64_t hi, int64_t g)
{
    int64_t best = -1, a = lo, b = hi;
    oggs_page pg;
    while (b - a > 65536) {
        int64_t mid = a + (b - a) / 2, got = -1;
        if (seek_to(o, mid) != FA_OK) return best;
        while (next_page(o, &pg, b - mid) == FA_OK && pg.pos < b) {
            if (pg.serial == serial && pg.granule >= 0) { got = pg.granule; break; }
        }
        if (got >= 0 && got < g) { best = pg.pos; a = pg.pos + pg.size; }
        else b = mid;
    }
    /* The rest page by page. */
    if (seek_to(o, a) != FA_OK) return best;
    while (next_page(o, &pg, OGG_RESYNC_MAX) == FA_OK && pg.pos < hi) {
        if (pg.serial != serial || pg.granule < 0) continue;
        if (pg.granule >= g) break;
        best = pg.pos;
    }
    return best;
}

static fa_result ogg_seek(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    ogg* o = (ogg*)ctx;
    oggs_link* l;
    int k = 0, i;
    int64_t g, at, end;
    fa_result r;
    (void)bytes_back;
    for (i = 1; i < o->nlinks; i++)
        if (o->links[i]->base <= t) k = i;
    l = o->links[k];
    g = (int64_t)llround((double)(t - l->base) * (double)l->rate / o->rate0) + l->first_gran + (int64_t)l->preskip;
    /* Vorbis needs the packet before the one holding the target: a long block back. */
    if (l->kind == OGG_VORBIS) g -= l->long_block > 0 ? l->long_block : 8192;
    end = l->end >= 0 ? l->end : o->br.src->length >= 0 ? o->br.src->length : INT64_MAX;
    at = g > l->first_gran ? find_page_before(o, l->serial, l->data, end, g) : -1;
    if (at < 0) at = l->data;
    o->cur = k;
    o->nq = o->iq = 0;
    o->pend_len = 0;
    o->have_seq = 0;
    o->at_end = 0;
    /* From the link's first audio page the times run forward from its start;
     * from anywhere else they come from the first granule, and a packet that
     * continues from the page before is dropped. */
    o->gran = at == l->data ? l->first_gran : OGG_NO_GRAN;
    o->prev_bs = at == l->data ? 0 : -1;
    o->pend_valid = at == l->data;
    o->keep = 0;
    o->skipping = 1;
    o->skip_to = t;
    o->skip_back = packets_back < OGG_MAX_KEEP ? packets_back : OGG_MAX_KEEP;
    if (o->codec_link != k) {
        o->codec_link = k;
        o->reopen = &l->params;
    }
    r = o->oom ? FA_ERROR_OUT_OF_MEMORY : seek_to(o, at);   /* the page search may have run out */
    if (r != FA_OK) o->skipping = 0;
    return r;
}

static const fa_demux_ops ogg_ops = { ogg_next, ogg_seek };

/* ---- open ------------------------------------------------------------------------------- */

static void ogg_free(ogg* o)
{
    links_free(o, 0, o->nlinks);
    free(o->links);
    free(o->pend);
    free(o->qdata);
    free(o->q);
    free(o->body);
    fa_br_free(&o->br);
    free(o);
}

static int in_group(const ogg* o, uint32_t serial)
{
    int i;
    for (i = 0; i < o->ngroup; i++)
        if (o->group[i] == serial) return 1;
    return 0;
}

/* The granule of the last page of `serial` in the file (read from its tail).
 * *out_chained = a stream that did not start the file comes after it. */
static int64_t last_granule(ogg* o, uint32_t serial, int* out_chained)
{
    int64_t len = o->br.src->length, from = len, gran = -1;
    oggs_page pg;
    *out_chained = 0;
    while (from > 0 && gran < 0) {
        int64_t back = len - from + 2 * (OGG_HEADER + 255 + OGG_MAX_BODY);
        int64_t at = len - back < 0 ? 0 : len - back;
        if (seek_to(o, at) != FA_OK) return -1;
        int ended = 0;
        while (next_page(o, &pg, from - at) == FA_OK && pg.pos < from) {
            /* Another stream, a stream starting, or pages after the end. */
            if (!in_group(o, pg.serial) || (pg.pos >= o->links[0]->data && (pg.flags & 2)) || (ended && pg.serial == serial))
                *out_chained = 1;
            if (pg.serial == serial && pg.granule >= 0) gran = pg.granule;
            if (pg.serial == serial && (pg.flags & 4)) ended = 1;
        }
        if (*out_chained || at == 0 || back > (16 << 20)) break;
        from = at;
    }
    return gran;
}

/* ---- finding the links of a chained file -------------------------------------------------- */

/* Pages of a link's audio stream seen while the links are searched for. They
 * must run in order (sequence numbers and granules growing with the position,
 * the end-of-stream mark only on the last), or the link hides another one with
 * the same serial number. Pages read one after another (a run) are checked as
 * they come, and the first and last of each run are kept with the single ones,
 * to be checked together at the end. */
#define OGG_SEEN 96

typedef struct seen_pg {
    int64_t pos, gran;
    uint32_t seq;
    uint8_t flags;
} seen_pg;

typedef struct link_seen {
    const uint32_t* s;      /* the link's streams */
    int ns;
    uint32_t serial;        /* its audio stream */
    int64_t lo;             /* where its audio starts */
    int n, bad, in_run;
    seen_pg run_first, run_last;
    seen_pg p[OGG_SEEN];    /* by position */
} link_seen;

static int has_serial(const uint32_t* s, int n, uint32_t serial)
{
    int i;
    for (i = 0; i < n; i++)
        if (s[i] == serial) return 1;
    return 0;
}

/* Two pages of the stream, a before b: in order. */
static int pg_order(const seen_pg* a, const seen_pg* b)
{
    return b->seq > a->seq && !(a->flags & 4) && (a->gran < 0 || b->gran < 0 || b->gran >= a->gran);
}

static void seen_keep(link_seen* c, const seen_pg* x)
{
    int i;
    for (i = 0; i < c->n; i++)
        if (c->p[i].pos == x->pos) return;
    if (c->n == OGG_SEEN) return;
    for (i = c->n++; i > 0 && c->p[i - 1].pos > x->pos; i--) c->p[i] = c->p[i - 1];
    c->p[i] = *x;
}

/* A page read on its own (run 0), or the next one of a run. */
static void seen_page(link_seen* c, const oggs_page* pg, int run)
{
    seen_pg x;
    if (pg->pos < c->lo) return;
    if ((pg->flags & 2) && has_serial(c->s, c->ns, pg->serial)) c->bad = 1;   /* one of its streams starts again */
    if (pg->serial != c->serial) return;
    x.pos = pg->pos;
    x.gran = pg->granule;
    x.seq = pg->seq;
    x.flags = pg->flags;
    if (!run) {
        seen_keep(c, &x);
    } else if (!c->in_run) {
        c->run_first = c->run_last = x;
        c->in_run = 1;
    } else {
        if (!pg_order(&c->run_last, &x)) c->bad = 1;
        c->run_last = x;
    }
}

static void seen_run_end(link_seen* c)
{
    if (!c->in_run) return;
    seen_keep(c, &c->run_first);
    seen_keep(c, &c->run_last);
    c->in_run = 0;
}

static int seen_in_order(const link_seen* c)
{
    int i;
    if (c->bad) return 0;
    for (i = 1; i < c->n; i++)
        if (!pg_order(&c->p[i - 1], &c->p[i])) return 0;
    return 1;
}

/* The page at or after `pos`, if one starts before `end`. */
static int page_from(ogg* o, int64_t pos, int64_t end, oggs_page* pg)
{
    return seek_to(o, pos) == FA_OK && next_page(o, pg, end - pos) == FA_OK && pg->pos < end;
}

/* The serials of the BOS pages at `pos` (a link's first group); 0 = no BOS page
 * there, -1 = more than `cap`. */
static int group_at(ogg* o, int64_t pos, uint32_t* s, int cap)
{
    oggs_page pg;
    int n = 0;
    if (seek_to(o, pos) != FA_OK) return 0;
    while (next_page(o, &pg, 0) == FA_OK && (pg.flags & 2)) {
        if (n == 0 && pg.pos != pos) return 0;
        if (n == cap) return -1;
        s[n++] = pg.serial;
    }
    return n;
}

/* Reads link k from where its audio starts, as reading the file through does,
 * until its first granule is known and its first 64 KB are past. *end = where
 * the link ends if that came first, else -1; *whole = the link was read to its
 * end (or the file's), *last = then its last granule; *after = where reading
 * stopped (a page start). */
static fa_result link_start(ogg* o, int k, link_seen* c, int64_t* end, int* whole, int64_t* last, int64_t* after)
{
    oggs_link* l = o->links[k];
    oggs_page pg;
    fa_result r;
    o->cur = k;
    o->nq = o->iq = 0;
    o->gran = OGG_NO_GRAN;
    o->prev_bs = 0;
    o->pend_len = 0;
    o->pend_valid = 1;
    o->have_seq = 0;
    o->keep = 0;
    o->skipping = 0;
    *end = -1;
    *whole = 0;
    *last = OGG_NO_GRAN;
    *after = l->data;
    r = seek_to(o, l->data);
    while (r == FA_OK && (l->first_gran == OGG_NO_GRAN || *after < l->data + 65536)) {
        r = next_page(o, &pg, OGG_RESYNC_MAX);
        if (r != FA_OK) break;
        if (!has_serial(c->s, c->ns, pg.serial)) {
            *end = pg.pos;
            *whole = 1;
            break;
        }
        seen_page(c, &pg, 1);
        if (pg.flags & 2) break;            /* a link inside the link: seen_page marked it */
        *after = pg.pos + pg.size;
        if (pg.serial != l->serial) continue;
        if (pg.granule >= 0) *last = pg.granule;
        if (l->first_gran != OGG_NO_GRAN) continue;
        r = page_packets(o, &pg);
        if (r == FA_OK) time_packets(o, l, &pg);
    }
    seen_run_end(c);
    o->nq = o->iq = 0;
    if (r == FA_ERROR_END_OF_STREAM) {
        *whole = 1;                         /* reading through stops here too */
        r = FA_OK;
    }
    return r;
}

/* Where the link ends: the first page at or after `a` (a page start) that is not
 * one of its streams, `best` if none starts before `b`. Found by bisection, as
 * vorbisfile does, since the pages of a link come before every page of the next.
 * *last = the last granule of the pages read one by one at the end, if any. */
static int64_t link_end(ogg* o, link_seen* c, int64_t a, int64_t b, int64_t best, int64_t* last)
{
    oggs_page pg;
    *last = OGG_NO_GRAN;
    while (b - a > 65536) {
        int64_t mid = a + (b - a) / 2;
        if (!page_from(o, mid, b, &pg)) {
            b = mid;                 /* no page starts in [mid, b) */
            continue;
        }
        if (has_serial(c->s, c->ns, pg.serial)) {
            seen_page(c, &pg, 0);
            a = pg.pos + pg.size;
        } else {
            best = pg.pos;
            b = mid;
        }
    }
    /* The rest page by page. */
    if (seek_to(o, a) == FA_OK) {
        while (next_page(o, &pg, OGG_RESYNC_MAX) == FA_OK && pg.pos < b) {
            if (!has_serial(c->s, c->ns, pg.serial)) {
                best = pg.pos;
                break;
            }
            seen_page(c, &pg, 1);
            if (pg.serial == c->serial && pg.granule >= 0) *last = pg.granule;
        }
    }
    seen_run_end(c);
    return best;
}

/* The granule of the link's last audio page before `end`, as reading the link
 * through leaves it; OGG_NO_GRAN if none. Read backwards from the end. */
static int64_t last_gran_before(ogg* o, link_seen* c, int64_t end)
{
    int64_t from = end, gran = OGG_NO_GRAN;
    oggs_page pg;
    while (from > c->lo && gran == OGG_NO_GRAN) {
        int64_t at = end - 2 * (end - from) - 16384;
        if (at < c->lo) at = c->lo;
        if (seek_to(o, at) != FA_OK) break;
        while (next_page(o, &pg, from - at) == FA_OK && pg.pos < from) {
            seen_page(c, &pg, 1);
            if (pg.serial == c->serial && pg.granule >= 0) gran = pg.granule;
        }
        seen_run_end(c);
        from = at;
    }
    return gran;
}

/* Every link of a local chained file without reading all of it: each link's
 * start is read, its end found by bisection (after doubling steps, which also
 * sample its pages) and its last page read, then the next link's headers. The
 * links come out as reading the file through finds them. *full_read = 1 when
 * the file needs that read after all (a link that isn't audio, serial numbers
 * used twice, pages that don't run in order); the links after the first are
 * dropped then. */
static fa_result find_links(ogg* o, int64_t len, int* full_read)
{
    uint32_t s[16], ns[16];
    int n = o->ngroup, nn, i, j, k;
    fa_result r;
    *full_read = 0;
    if (n < 1 || n > 15) goto full;          /* 16: the first group may have had more */
    memcpy(s, o->group, sizeof(uint32_t) * (size_t)n);
    for (k = 0;; k++) {
        oggs_link* l = o->links[k];
        link_seen c;
        oggs_page pg;
        int64_t end, last, a, b = len, step;
        int whole;
        c.s = s;
        c.ns = n;
        c.serial = l->serial;
        c.lo = l->data;
        c.n = c.bad = c.in_run = 0;
        r = o->oom ? FA_ERROR_OUT_OF_MEMORY : link_start(o, k, &c, &end, &whole, &last, &a);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
        if (r != FA_OK) goto full;          /* reading through meets it the same way */
        if (c.bad) goto full;
        if (!whole) {
            /* Steps doubling from there until one lands past the link, then bisection. */
            for (step = 65536; a + step < len; step *= 2) {
                if (!page_from(o, a + step, len, &pg)) { b = a + step; break; }
                if (!has_serial(s, n, pg.serial)) { end = pg.pos; b = a + step; break; }
                seen_page(&c, &pg, 0);
                a = pg.pos + pg.size;
            }
            end = link_end(o, &c, a, b, end, &last);
            if (last == OGG_NO_GRAN) last = last_gran_before(o, &c, end >= 0 ? end : len);
        }
        l->end = end >= 0 ? end : len;
        l->last_gran = last;
        if (o->oom) return FA_ERROR_OUT_OF_MEMORY;
        if (!seen_in_order(&c)) goto full;
        if (end < 0) break;
        /* The next link: a group of BOS pages, of serials not used before. */
        nn = group_at(o, end, ns, 15);
        if (nn < 1) goto full;
        for (i = 0; i < nn; i++) {
            if (has_serial(s, n, ns[i])) goto full;
            for (j = 0; j < k; j++)
                if (o->links[j]->serial == ns[i]) goto full;
        }
        {
            oggs_link* nl;
            r = add_link(o, &nl);
            if (r != FA_OK) return r;
            r = seek_to(o, end);
            if (r == FA_OK) r = read_link(o, nl, o->want, NULL, NULL);
            if (r == FA_ERROR_OUT_OF_MEMORY) return r;
            if (r != FA_OK || nl->start != end) goto full;
        }
        memcpy(s, ns, sizeof(uint32_t) * (size_t)nn);
        n = nn;
    }
    return o->oom ? FA_ERROR_OUT_OF_MEMORY : FA_OK;
full:
    links_free(o, 1, o->nlinks);
    o->nlinks = 1;
    o->links[0]->first_gran = o->links[0]->last_gran = OGG_NO_GRAN;
    o->links[0]->end = -1;
    *full_read = 1;
    return o->oom ? FA_ERROR_OUT_OF_MEMORY : FA_OK;
}

/* Back to the start of the first link's audio. */
static fa_result rewind_first(ogg* o)
{
    o->cur = 0;
    o->at_end = 0;
    o->nq = o->iq = 0;
    o->gran = OGG_NO_GRAN;
    o->prev_bs = 0;
    o->next_pts = 0;
    o->pend_len = 0;
    o->pend_valid = 1;
    o->have_seq = 0;
    o->codec_link = 0;
    o->reopen = NULL;
    o->keep = 0;
    o->skipping = 0;
    return seek_to(o, o->links[0]->data);
}

static fa_result ogg_open(fa_decoder* dec)
{
    ogg* o = (ogg*)calloc(1, sizeof(ogg));
    oggs_link* l;
    fa_track_params tp;
    int audio = 0, want = dec->audio_track < 0 ? 0 : dec->audio_track, chained = 0, i;
    fa_result r;
    if (!o) return FA_ERROR_OUT_OF_MEMORY;
    crc_init();
    r = fa_br_init(&o->br, dec->src);
    if (r != FA_OK) { free(o); return r; }
    /* A web stream's source reads from its fa_net (stream.c, net_hls.c). */
    if (dec->src->remote && dec->src->kind == FA_SOURCE_IO) o->net = (fa_net*)dec->src->io_user;
    r = add_link(o, &l);
    o->want = want;
    if (r == FA_OK) r = read_link(o, l, want, &audio, NULL);
    if (r != FA_OK) goto fail;
    dec->track_count = audio;
    o->rate0 = (double)l->rate;

    if (dec->src->seekable && dec->src->length > 0) {
        int64_t g = last_granule(o, l->serial, &chained);
        if (!chained) {
            o->enumerated = 1;
            l->end = dec->src->length;
            l->last_gran = g >= 0 ? g : OGG_NO_GRAN;
        } else if (!dec->src->remote) {
            /* Every link's start, headers and granules: found by bisection, or
             * else by reading the whole file once. */
            int full_read;
            o->scanning = 1;
            o->enumerated = 1;
            r = find_links(o, dec->src->length, &full_read);
            if (r != FA_OK) goto fail;
            if (full_read) {
                r = rewind_first(o);
                while (r == FA_OK && (r = fill_queue(o)) == FA_OK) o->iq = o->nq;
                if (r != FA_ERROR_END_OF_STREAM) goto fail;
            }
            o->scanning = 0;
            for (i = 0; i < o->nlinks; i++) {
                oggs_link* li = o->links[i];
                li->end = i + 1 < o->nlinks ? o->links[i + 1]->start : dec->src->length;
                if (link_length(o, li) < 0) o->enumerated = 0;
                if (i > 0) {
                    int64_t n = link_length(o, o->links[i - 1]);
                    li->base = o->links[i - 1]->base + (n > 0 ? n : 0);
                }
            }
        }
    }
    r = rewind_first(o);
    if (r != FA_OK) goto fail;
    /* The first link's start granule comes from its first audio page. */
    if (l->first_gran == OGG_NO_GRAN && dec->src->seekable) {
        r = fill_queue(o);
        if (r != FA_OK && r != FA_ERROR_END_OF_STREAM) goto fail;
        r = rewind_first(o);
        if (r != FA_OK) goto fail;
    }

    memset(&tp, 0, sizeof(tp));
    tp.codec = l->params;
    tp.time_base = 1.0 / o->rate0;
    tp.start = 0;
    tp.end = FA_DEMUX_NO_PTS;
    if (o->enumerated) {
        int64_t total = 0;
        for (i = 0; i < o->nlinks && total >= 0; i++) {
            int64_t n = link_length(o, o->links[i]);
            total = n < 0 ? -1 : total + n;
        }
        if (total >= 0) { tp.end = total; tp.clip_to_end = 1; }
    }
    tp.last_pts = FA_DEMUX_NO_PTS;
    tp.seek_preroll_s = 0.0;      /* Opus: the track's own (OPUS_PREROLL_S) */
    if (o->oom) { r = FA_ERROR_OUT_OF_MEMORY; goto fail; }   /* a scan above ran out */
    r = fa_track_open(&o->track, dec, &ogg_ops, o, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = o;
    /* A chained web file is played through; its links are not known up front. */
    dec->seekable = dec->src->seekable && o->enumerated;
    /* Everything after the first link's headers is audio (and the headers of later
     * links), unless other streams (video, Skeleton) are multiplexed in. */
    if (o->ngroup == 1 && dec->src->length > 0 && !dec->src->remote && o->links[0]->data > 0 &&
        o->links[0]->data < dec->src->length)
        dec->audio_bytes = dec->src->length - o->links[0]->data;
    snprintf(dec->container, sizeof(dec->container), "ogg");
    return FA_OK;
fail:
    ogg_free(o);
    return r;
}

static fa_result ogg_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((ogg*)dec->impl)->track, out, frames, got);
}

static fa_result ogg_dec_seek(fa_decoder* dec, fa_frames frame)
{
    return fa_track_seek(&((ogg*)dec->impl)->track, frame);
}

static void ogg_close(fa_decoder* dec)
{
    ogg* o = (ogg*)dec->impl;
    if (!o) return;
    fa_track_close(&o->track);
    ogg_free(o);
    dec->impl = NULL;
}

/* "OggS" at the start, with a BOS page. */
static int ogg_probe(const uint8_t* d, size_t n, size_t id3)
{
    (void)id3;
    if (n < OGG_HEADER || memcmp(d, "OggS", 4) != 0 || d[4] != 0 || !(d[5] & 2)) return 0;
    return 100;
}

const fa_decoder_vtable fa_decoder_ogg = {
    "ogg", "Ogg Opus, Ogg Vorbis and Ogg FLAC", "ogg;oga;opus;ogv;ogx",
    "audio/ogg;application/ogg;audio/opus;audio/vorbis;audio/x-vorbis+ogg;video/ogg",
    ogg_probe, ogg_open, ogg_read, ogg_dec_seek, ogg_close, 1, NULL, NULL
};

/* ---- loudness tags --------------------------------------------------------------------------- */

void fa_ogg_read_tags(fa_source* src, fa_tag_sink* out)
{
    ogg* o;
    oggs_link* l;
    const uint8_t* head;
    size_t n = fa_source_peek(src, &head);
    if (n < OGG_HEADER || memcmp(head, "OggS", 4) != 0) return;
    o = (ogg*)calloc(1, sizeof(ogg));
    if (!o) return;
    crc_init();
    o->scanning = 1;
    if (fa_source_seek(src, 0) == FA_OK && fa_br_init(&o->br, src) == FA_OK && add_link(o, &l) == FA_OK)
        read_link(o, l, 0, NULL, out);
    ogg_free(o);
}
