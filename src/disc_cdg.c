/* CD+G from audio CDs: the R-W subcode read with each sector's audio becomes the
 * stream's CD+G packs, which cdg.c decodes and draws like a .cdg file.
 *
 * A sector carries 96 subcode bytes; their R-W channels are 4 packs of 24 six-bit
 * symbols, 300 packs per second, exactly the packs a .cdg file holds. On the disc
 * the symbols of a pack are interleaved: three pairs are swapped (1/18, 2/5, 3/23)
 * and symbol n is delayed by n % 8 packs. Drives return this raw form (one byte per
 * symbol, P and Q in the top bits) or already sorted packs; .sub files beside disc
 * images may also hold the channels one after another (12 bytes each). Which layout
 * it is, and the order of the two interleave steps, is found by trying each reading
 * on the packs as they arrive and keeping the one that turns up CD+G instructions
 * whose parity checks out (the P parity covers all 24 symbols, the swapped ones too,
 * so only the right reading passes where packs differ). A track with no graphics
 * never does, and its subcode stops being read. */
#include "disc.h"
#include "cdg.h"
#include <stdlib.h>
#include <string.h>

#define PACK 24
#define PACKS_PER_SECTOR 4
#define LOOKAHEAD 7                      /* the longest delay, in packs */
#define DECIDE_MIN 12                    /* CD+G instructions needed to decide */
#define UNDECIDED_MAX (75 * 90 * PACKS_PER_SECTOR)   /* give up after 90 s without any */
#define NO_PARITY_AFTER (75 * 4 * PACKS_PER_SECTOR)  /* decide without parity after 4 s */

/* Readings: a byte layout times an interleave order. */
enum { LAYOUT_BYTES, LAYOUT_CHANNELS, LAYOUTS };
enum { ORDER_SORTED, ORDER_SWAP_DELAY, ORDER_DELAY_SWAP, ORDERS };
#define READINGS (LAYOUTS * ORDERS)
#define USUAL (LAYOUT_BYTES * ORDERS + ORDER_SWAP_DELAY)   /* raw from a drive */
enum { MODE_UNDECIDED = -1, MODE_OFF = -2 };

struct fa_disc_cdg {
    struct fa_stream* stream;
    int64_t track_lba, track_sectors;
    int mode;                            /* a reading, or MODE_* */
    int64_t run_lba;                     /* the contiguous run of sectors being read */
    int64_t run_end;                     /* -1 = no run */
    /* Symbols of the run in each layout (only the chosen one once decided), from run
     * pack `raw_base`. */
    uint8_t* raw[LAYOUTS];
    size_t raw_base, raw_n, raw_cap;     /* in packs */
    size_t done;                         /* run packs written (or judged, while undecided) */
    uint32_t seen[READINGS];             /* CD+G instructions found by each reading */
    uint32_t sound[READINGS];            /* ... of them with correct parity */
    size_t looked;                       /* packs judged while undecided */
    uint32_t token;                      /* the stream's CD+G generation of our packs */
};

static int swap_pos(int n)
{
    switch (n) {
    case 1: return 18; case 18: return 1;
    case 2: return 5;  case 5: return 2;
    case 3: return 23; case 23: return 3;
    default: return n;
    }
}

/* Pack `p` of the run in reading `v`. Symbols past what was read count as 0.
 *   swapped, then delayed on the disc:  pack[n] = raw[p + swap(n) % 8][swap(n)]
 *   delayed, then swapped on the disc:  pack[n] = raw[p + n % 8][swap(n)] */
static void decode_pack(const fa_disc_cdg* c, int v, size_t p, uint8_t out[PACK])
{
    const uint8_t* raw = c->raw[v / ORDERS];
    int order = v % ORDERS, n;
    for (n = 0; n < PACK; n++) {
        int pos = order == ORDER_SORTED ? n : swap_pos(n);
        size_t from = p;
        if (order == ORDER_SWAP_DELAY) from += (size_t)(pos % 8);
        else if (order == ORDER_DELAY_SWAP) from += (size_t)(n % 8);
        out[n] = from - c->raw_base < c->raw_n ? raw[(from - c->raw_base) * PACK + pos] : 0;
    }
}

/* GF(64) with x^6 + x + 1, the field of the subcode's Reed-Solomon parity. */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        b >>= 1;
        a = (uint8_t)(a << 1);
        if (a & 0x40) a ^= 0x43;
    }
    return r;
}

/* Syndromes of a Reed-Solomon codeword over GF(64): sum of c[i] * a^(j * (n-1-i))
 * for j < nsyn (Horner), all zero for a sound one. The opposite symbol order is
 * accepted as well. */
static int rs_ok(const uint8_t* c, int n, int nsyn)
{
    int order, j, i;
    for (order = 0; order < 2; order++) {
        int ok = 1;
        for (j = 0; j < nsyn && ok; j++) {
            uint8_t root = (uint8_t)(1u << j), syn = 0;   /* a^j, a = x */
            for (i = 0; i < n; i++) syn = (uint8_t)(gf_mul(syn, root) ^ c[order ? n - 1 - i : i]);
            ok = syn == 0;
        }
        if (ok) return 1;
    }
    return 0;
}

/* Q parity (symbols 2-3 over the command and instruction) and P parity (symbols
 * 20-23 over the whole pack). */
static int parity_ok(const uint8_t* p) { return rs_ok(p, 4, 2) && rs_ok(p, PACK, 4); }

/* A CD+G graphics instruction (what a reading must produce to be the right one). */
static int is_cdg(const uint8_t* p)
{
    if ((p[0] & 0x3F) != 0x09) return 0;
    switch (p[1] & 0x3F) {
    case 1: case 2: case 6: case 20: case 24: case 28: case 30: case 31: case 38: return 1;
    default: return 0;
    }
}

fa_disc_cdg* fa_disc_cdg_create(struct fa_stream* stream, int64_t track_lba, int64_t track_sectors)
{
    fa_disc_cdg* c;
    if (!stream || track_sectors <= 0) return NULL;
    c = (fa_disc_cdg*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->stream = stream;
    c->track_lba = track_lba;
    c->track_sectors = track_sectors;
    c->mode = MODE_UNDECIDED;
    c->run_end = -1;
    return c;
}

static void free_raw(fa_disc_cdg* c, int keep)
{
    int l;
    for (l = 0; l < LAYOUTS; l++)
        if (l != keep) { free(c->raw[l]); c->raw[l] = NULL; }
}

void fa_disc_cdg_free(fa_disc_cdg* c)
{
    if (!c) return;
    free_raw(c, -1);
    free(c);
}

int fa_disc_cdg_wanted(const fa_disc_cdg* c) { return c && c->mode != MODE_OFF; }

static void stop(fa_disc_cdg* c)
{
    c->mode = MODE_OFF;
    free_raw(c, -1);
    c->raw_n = c->raw_cap = 0;
}

/* Writes run packs [done, upto) to the stream in the chosen reading. */
static void emit(fa_disc_cdg* c, size_t upto)
{
    uint8_t buf[64 * PACK];
    size_t first = (size_t)(c->run_lba - c->track_lba) * PACKS_PER_SECTOR;
    while (c->done < upto) {
        size_t n = upto - c->done, i;
        if (n > 64) n = 64;
        for (i = 0; i < n; i++) decode_pack(c, c->mode, c->done + i, buf + i * PACK);
        if (!fa_stream_cdg_write(c->stream, c->token, first + c->done, buf, n)) { stop(c); return; }
        c->done += n;
    }
}

static int best_of(const uint32_t* score, int* out_second)
{
    int v, best = USUAL, second = -1;   /* ties go to the usual reading */
    for (v = 0; v < READINGS; v++) if (score[v] > score[best]) best = v;
    for (v = 0; v < READINGS; v++) if (v != best && (second < 0 || score[v] > score[second])) second = v;
    *out_second = second;
    return best;
}

/* Counts CD+G instructions per reading in run packs [done, upto) and decides when
 * one reading clearly finds them. */
static void judge(fa_disc_cdg* c, size_t upto)
{
    uint8_t pk[PACK];
    int v, best, second, chosen = -1;
    for (; c->done < upto; c->done++, c->looked++)
        for (v = 0; v < READINGS; v++) {
            decode_pack(c, v, c->done, pk);
            if (is_cdg(pk)) {
                c->seen[v]++;
                if (parity_ok(pk)) c->sound[v]++;
            }
        }
    best = best_of(c->sound, &second);
    if (c->sound[best] >= DECIDE_MIN && c->sound[best] >= 4 * c->sound[second] + 1) chosen = best;
    else if (c->sound[best] == 0 && c->looked >= NO_PARITY_AFTER) {
        /* Instructions but never a matching parity (a drive that zeroes it): go by
         * the instructions. The two interleave orders then differ only where
         * neighbouring packs differ, so a near tie between them means the usual one. */
        best = best_of(c->seen, &second);
        if (c->seen[best] >= DECIDE_MIN && c->seen[best] >= 4 * c->seen[second] + 1) chosen = best;
        else if (c->seen[best] >= DECIDE_MIN && best / ORDERS == second / ORDERS &&
                 best % ORDERS != ORDER_SORTED && second % ORDERS != ORDER_SORTED)
            chosen = (best / ORDERS) * ORDERS + ORDER_SWAP_DELAY;
    }
    if (chosen >= 0) {
        fa_cdg_data* data;
        if (fa_cdg_data_blank((size_t)c->track_sectors * PACKS_PER_SECTOR, &data) != FA_OK ||
            !fa_stream_cdg_attach_if_none(c->stream, data, &c->token)) {
            stop(c);   /* out of memory, or the app gave the stream graphics of its own */
            return;
        }
        c->mode = chosen;
        free_raw(c, chosen / ORDERS);
        c->done = c->raw_base;                   /* write what this run holds so far */
        emit(c, upto);
    } else if (c->looked >= UNDECIDED_MAX) {
        stop(c);
    }
}

/* One sector's subcode (96 bytes) as the symbols of its 4 packs, in each layout. */
static void unpack_sector(const uint8_t* in, uint8_t* bytes, uint8_t* channels)
{
    int k, ch;
    if (bytes) for (k = 0; k < 96; k++) bytes[k] = in[k] & 0x3F;
    if (channels)
        for (k = 0; k < 96; k++) {
            uint8_t v = 0;
            for (ch = 2; ch < 8; ch++)   /* R..W: 12 bytes each, after P and Q */
                v = (uint8_t)((v << 1) | ((in[ch * 12 + k / 8] >> (7 - k % 8)) & 1));
            channels[k] = v;
        }
}

void fa_disc_cdg_feed(fa_disc_cdg* c, int64_t lba, uint32_t count, const uint8_t* sub)
{
    int64_t end = lba + count, track_end, k;
    size_t add, final;
    int l;
    if (!c || c->mode == MODE_OFF) return;
    track_end = c->track_lba + c->track_sectors;
    /* Only this track's sectors (offset reads reach into the neighbours). */
    if (lba < c->track_lba) { sub += (size_t)(c->track_lba - lba) * FA_CD_SUB_BYTES; lba = c->track_lba; }
    if (end > track_end) end = track_end;
    if (lba >= end) return;

    if (lba != c->run_end) {
        /* A seek. The old run's last packs lack the look-ahead they need and stay
         * unwritten; they are read again when that part plays. */
        c->run_lba = lba;
        c->raw_base = c->raw_n = c->done = 0;
    }
    c->run_end = end;

    add = (size_t)(end - lba) * PACKS_PER_SECTOR;
    if (c->raw_n + add > c->raw_cap) {
        size_t cap = c->raw_cap ? c->raw_cap : 256;
        while (cap < c->raw_n + add) cap *= 2;
        for (l = 0; l < LAYOUTS; l++) {
            uint8_t* grown;
            if (c->mode >= 0 && l != c->mode / ORDERS) continue;
            grown = (uint8_t*)realloc(c->raw[l], cap * PACK);
            if (!grown) { stop(c); return; }
            c->raw[l] = grown;
        }
        c->raw_cap = cap;
    }
    for (k = 0; k < end - lba; k++) {
        size_t at = (c->raw_n + (size_t)k * PACKS_PER_SECTOR) * PACK;
        unpack_sector(sub + (size_t)k * FA_CD_SUB_BYTES, c->raw[LAYOUT_BYTES] ? c->raw[LAYOUT_BYTES] + at : NULL,
                      c->raw[LAYOUT_CHANNELS] ? c->raw[LAYOUT_CHANNELS] + at : NULL);
    }
    c->raw_n += add;

    /* Packs with their whole look-ahead read are final; at the track's end all are. */
    final = c->raw_base + c->raw_n;
    if (end < track_end) final = final > LOOKAHEAD ? final - LOOKAHEAD : 0;
    if (c->mode == MODE_UNDECIDED) judge(c, final);
    else emit(c, final);

    /* Keep only what later packs still need (everything until a reading is chosen). */
    if (c->mode >= 0 && c->done > c->raw_base) {
        size_t drop = c->done - c->raw_base;
        uint8_t* raw = c->raw[c->mode / ORDERS];
        memmove(raw, raw + drop * PACK, (c->raw_n - drop) * PACK);
        c->raw_base += drop;
        c->raw_n -= drop;
    }
}
