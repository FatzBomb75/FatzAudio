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
 * never does, and its subcode stops being read. Subcode read before a seek is kept
 * until the reading is chosen, then written too. Once a reading is chosen by its
 * parity, each pack is checked: up to two wrong symbols are corrected (the P
 * parity's limit), a pack with more is dropped (a scratch would otherwise draw
 * garbage tiles), and both are counted for fa_cdg_info. A drive that returns no
 * parity gets every pack as read. */
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
#define PARITY_LOST 300                  /* this many bad packs in a row: the parity stopped
                                            coming (another read mode), not a scratch */
#define KEEP_MAX UNDECIDED_MAX           /* packs of earlier runs kept while undecided */

/* Readings: a byte layout times an interleave order. */
enum { LAYOUT_BYTES, LAYOUT_CHANNELS, LAYOUTS };
enum { ORDER_SORTED, ORDER_SWAP_DELAY, ORDER_DELAY_SWAP, ORDERS };
#define READINGS (LAYOUTS * ORDERS)
#define USUAL (LAYOUT_BYTES * ORDERS + ORDER_SWAP_DELAY)   /* raw from a drive */
enum { MODE_UNDECIDED = -1, MODE_OFF = -2 };

/* A contiguous run of sectors read. */
typedef struct cdg_run {
    int64_t lba, end;                    /* its sectors [lba, end) */
    /* Its symbols in each layout (only the chosen one once decided), from run pack
     * `raw_base`. */
    uint8_t* raw[LAYOUTS];
    size_t raw_base, raw_n, raw_cap;     /* in packs */
    size_t done;                         /* packs written (or judged, while undecided) */
} cdg_run;

/* What became of a pack the last time it was written (fate[]). */
enum { FATE_NONE, FATE_SOUND, FATE_CORRECTED, FATE_DROPPED };

struct fa_disc_cdg {
    struct fa_stream* stream;
    int64_t track_lba, track_sectors;
    int mode;                            /* a reading, or MODE_* */
    cdg_run run;                         /* the one being read; run.end -1 = none yet */
    cdg_run* kept;                       /* earlier runs, while undecided */
    size_t nkept, kept_packs;
    uint8_t* fate;                       /* FATE_* per track pack, once checked */
    uint64_t corrected, dropped;         /* packs whose fate is that */
    uint32_t seen[READINGS];             /* CD+G instructions found by each reading */
    uint32_t sound[READINGS];            /* ... of them with correct parity */
    uint32_t backward[READINGS];         /* ... that with the symbols weighed backward */
    size_t looked;                       /* packs judged while undecided */
    uint32_t token;                      /* the stream's CD+G generation of our packs */
    int checked;                         /* the chosen reading has parity: packs that fail
                                            it and cannot be corrected are dropped */
    int sym_backward;                    /* ... with the symbols in that order */
    uint32_t failing;                    /* packs dropped in a row */
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
static void decode_pack(const cdg_run* c, int v, size_t p, uint8_t out[PACK])
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

/* Syndrome j of a Reed-Solomon codeword over GF(64): sum of c[i] * a^(j * (n-1-i))
 * (Horner), 0 for every j < nsyn in a sound one. Discs may weigh the symbols in the
 * opposite order (`backward`): which one is found with the subcode layout. */
static uint8_t syndrome(const uint8_t* c, int n, int j, int backward)
{
    uint8_t root = (uint8_t)(1u << j), syn = 0;   /* a^j, a = x */
    int i;
    for (i = 0; i < n; i++) syn = (uint8_t)(gf_mul(syn, root) ^ c[backward ? n - 1 - i : i]);
    return syn;
}

/* Q parity (symbols 2-3 over the command and instruction) and P parity (symbols
 * 20-23 over the whole pack), with the symbols in order `backward`. */
static int parity_in(const uint8_t* p, int backward)
{
    int j;
    for (j = 0; j < 2; j++) if (syndrome(p, 4, j, backward)) return 0;
    for (j = 0; j < 4; j++) if (syndrome(p, PACK, j, backward)) return 0;
    return 1;
}

static uint8_t gf_inv(uint8_t a)
{
    uint8_t x;
    for (x = 1; x < 64; x++) if (gf_mul(a, x) == 1) return x;
    return 0;
}

/* Repairs a pack with up to two wrong symbols from its P parity (4 syndromes, a
 * distance of 5), one first; 1 if the pack then passes both parities. A symbol at
 * power e (weighed a^(j*e) in syndrome j) is index 23 - e, or e backward. Three or
 * more wrong symbols cannot be told from fewer in another codeword: such a pack is
 * dropped, unless it lies within two symbols of another codeword (about 1 in 15 of
 * them), which then passes as that. */
static int parity_correct(uint8_t* p, int backward)
{
    uint8_t S[4], loc[PACK];
    int errs, j, e;
    for (e = 0, loc[0] = 1; e + 1 < PACK; e++) loc[e + 1] = gf_mul(loc[e], 2);
    for (j = 0; j < 4; j++) S[j] = syndrome(p, PACK, j, backward);
    if (!(S[0] | S[1] | S[2] | S[3])) return 0;   /* P holds: only Q fails */
    for (errs = 1; errs <= 2; errs++) {
        int at[2], k;
        uint8_t val[2];
        if (errs == 1) {
            /* One error of value S0 at X: S[j] = S0 * X^j. */
            if (!S[0]) continue;
            for (e = 0; e < PACK; e++)
                if (gf_mul(S[0], loc[e]) == S[1]) break;
            if (e == PACK || gf_mul(S[1], loc[e]) != S[2] || gf_mul(S[2], loc[e]) != S[3]) continue;
            at[0] = e;
            val[0] = S[0];
        } else {
            /* Locator x^2 + s1 x + s2 with roots X1, X2 (Peterson):
             *   S2 + s1 S1 + s2 S0 = 0,  S3 + s1 S2 + s2 S1 = 0. */
            uint8_t det = (uint8_t)(gf_mul(S[1], S[1]) ^ gf_mul(S[0], S[2])), inv, s1, s2, d;
            int found = 0;
            if (!det) continue;
            inv = gf_inv(det);
            s1 = gf_mul((uint8_t)(gf_mul(S[1], S[2]) ^ gf_mul(S[0], S[3])), inv);
            s2 = gf_mul((uint8_t)(gf_mul(S[1], S[3]) ^ gf_mul(S[2], S[2])), inv);
            for (e = 0; e < PACK && found < 2; e++)
                if ((gf_mul(loc[e], loc[e]) ^ gf_mul(s1, loc[e]) ^ s2) == 0) at[found++] = e;
            if (found < 2) continue;   /* a root outside the pack: more errors */
            /* S0 = e1 + e2, S1 = e1 X1 + e2 X2. */
            d = gf_inv((uint8_t)(loc[at[0]] ^ loc[at[1]]));
            val[0] = gf_mul((uint8_t)(S[1] ^ gf_mul(S[0], loc[at[1]])), d);
            val[1] = (uint8_t)(S[0] ^ val[0]);
            if (!val[0] || !val[1]) continue;
        }
        for (k = 0; k < errs; k++) p[backward ? at[k] : PACK - 1 - at[k]] ^= val[k];
        if (parity_in(p, backward)) return 1;
        for (k = 0; k < errs; k++) p[backward ? at[k] : PACK - 1 - at[k]] ^= val[k];
    }
    return 0;
}

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
    c->run.end = -1;
    return c;
}

static void run_free(cdg_run* r, int keep)
{
    int l;
    for (l = 0; l < LAYOUTS; l++)
        if (l != keep) { free(r->raw[l]); r->raw[l] = NULL; }
}

static void kept_free(fa_disc_cdg* c)
{
    size_t i;
    for (i = 0; i < c->nkept; i++) run_free(&c->kept[i], -1);
    free(c->kept);
    c->kept = NULL;
    c->nkept = c->kept_packs = 0;
}

void fa_disc_cdg_free(fa_disc_cdg* c)
{
    if (!c) return;
    run_free(&c->run, -1);
    kept_free(c);
    free(c->fate);
    free(c);
}

int fa_disc_cdg_wanted(const fa_disc_cdg* c) { return c && c->mode != MODE_OFF; }

static void stop(fa_disc_cdg* c)
{
    c->mode = MODE_OFF;
    run_free(&c->run, -1);
    c->run.raw_n = c->run.raw_cap = 0;
    kept_free(c);
}

/* Writes packs [done, upto) of run `r` to the stream in the chosen reading. */
static void emit(fa_disc_cdg* c, cdg_run* r, size_t upto)
{
    uint8_t buf[64 * PACK];
    size_t first = (size_t)(r->lba - c->track_lba) * PACKS_PER_SECTOR;
    uint64_t corrected = c->corrected, dropped = c->dropped;
    while (r->done < upto) {
        size_t n = upto - r->done, i;
        if (n > 64) n = 64;
        for (i = 0; i < n; i++) {
            uint8_t* pk = buf + i * PACK;
            uint8_t* fate = c->fate ? &c->fate[first + r->done + i] : NULL;
            int now;
            decode_pack(r, c->mode, r->done + i, pk);
            /* A damaged pack would draw garbage: an all-zero pack does nothing. */
            if (!c->checked) continue;
            if (parity_in(pk, c->sym_backward)) now = FATE_SOUND;
            else if (parity_correct(pk, c->sym_backward)) now = FATE_CORRECTED;
            else if (++c->failing >= PARITY_LOST) { c->checked = 0; continue; }
            else { now = FATE_DROPPED; memset(pk, 0, PACK); }
            if (now != FATE_DROPPED) c->failing = 0;
            if (fate && *fate != now) {
                /* The latest read of each pack counts: a part read again replaces it. */
                if (*fate == FATE_CORRECTED) c->corrected--;
                if (*fate == FATE_DROPPED) c->dropped--;
                if (now == FATE_CORRECTED) c->corrected++;
                if (now == FATE_DROPPED) c->dropped++;
                *fate = (uint8_t)now;
            }
        }
        if (!fa_stream_cdg_write(c->stream, c->token, first + r->done, buf, n)) { stop(c); return; }
        r->done += n;
    }
    if (c->corrected != corrected || c->dropped != dropped)
        fa_stream_cdg_disc_counts(c->stream, c->token, c->corrected, c->dropped);
}

static int best_of(const uint32_t* score, int* out_second)
{
    int v, best = USUAL, second = -1;   /* ties go to the usual reading */
    for (v = 0; v < READINGS; v++) if (score[v] > score[best]) best = v;
    for (v = 0; v < READINGS; v++) if (v != best && (second < 0 || score[v] > score[second])) second = v;
    *out_second = second;
    return best;
}

/* Packs of run `r` whose whole look-ahead was read; at the track's end all are. */
static size_t run_final(const fa_disc_cdg* c, const cdg_run* r)
{
    size_t final = r->raw_base + r->raw_n;
    if (r->end < c->track_lba + c->track_sectors) final = final > LOOKAHEAD ? final - LOOKAHEAD : 0;
    return final;
}

/* Counts CD+G instructions per reading in run packs [done, upto) and decides when
 * one reading clearly finds them. */
static void judge(fa_disc_cdg* c, size_t upto)
{
    cdg_run* r = &c->run;
    uint8_t pk[PACK];
    int v, best, second, chosen = -1;
    for (; r->done < upto; r->done++, c->looked++)
        for (v = 0; v < READINGS; v++) {
            decode_pack(r, v, r->done, pk);
            if (is_cdg(pk)) {
                c->seen[v]++;
                if (parity_in(pk, 0)) c->sound[v]++;
                else if (parity_in(pk, 1)) c->sound[v]++, c->backward[v]++;
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
        size_t count = (size_t)c->track_sectors * PACKS_PER_SECTOR, i;
        if (fa_cdg_data_blank(count, &data) != FA_OK) { stop(c); return; }
        data->from_disc = 1;
        data->disc_parity = c->sound[chosen] > 0;   /* fa_cdg_info.disc_parity */
        c->checked = data->disc_parity;
        c->sym_backward = 2 * c->backward[chosen] > c->sound[chosen];
        /* Without memory for it the counts stay 0; the packs are checked all the same. */
        if (c->checked) c->fate = (uint8_t*)calloc(count, 1);
        if (!fa_stream_cdg_attach_if_none(c->stream, data, &c->token)) {
            stop(c);   /* out of memory, or the app gave the stream graphics of its own */
            return;
        }
        c->mode = chosen;
        /* What earlier runs read first (the run being read is newer), then this run. */
        for (i = 0; i < c->nkept && c->mode >= 0; i++) {
            cdg_run* k = &c->kept[i];
            k->done = k->raw_base;
            emit(c, k, run_final(c, k));
        }
        if (c->mode < 0) return;
        kept_free(c);
        run_free(r, chosen / ORDERS);
        r->done = r->raw_base;                   /* write what this run holds so far */
        emit(c, r, upto);
    } else if (c->looked >= UNDECIDED_MAX) {
        stop(c);
    }
}

/* A seek while undecided: the run read so far is kept for when a reading is chosen
 * (up to KEEP_MAX packs in all; past that it is read again when that part plays). */
static void keep_run(fa_disc_cdg* c)
{
    cdg_run* r = &c->run;
    if (r->raw_n > 0 && c->kept_packs + r->raw_n <= KEEP_MAX) {
        cdg_run* grown = (cdg_run*)realloc(c->kept, (c->nkept + 1) * sizeof(*grown));
        if (grown) {
            c->kept = grown;
            c->kept[c->nkept++] = *r;
            c->kept_packs += r->raw_n;
            memset(r->raw, 0, sizeof(r->raw));
            r->raw_cap = 0;
        }
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
    cdg_run* r;
    size_t add;
    int l;
    if (!c || c->mode == MODE_OFF) return;
    r = &c->run;
    track_end = c->track_lba + c->track_sectors;
    /* Only this track's sectors (offset reads reach into the neighbours). */
    if (lba < c->track_lba) { sub += (size_t)(c->track_lba - lba) * FA_CD_SUB_BYTES; lba = c->track_lba; }
    if (end > track_end) end = track_end;
    if (lba >= end) return;

    if (lba != r->end) {
        /* A seek. Once decided, the old run's last packs lack the look-ahead they need
         * and stay unwritten; they are read again when that part plays. */
        if (c->mode == MODE_UNDECIDED) keep_run(c);
        r->lba = lba;
        r->raw_base = r->raw_n = r->done = 0;
    }
    r->end = end;

    add = (size_t)(end - lba) * PACKS_PER_SECTOR;
    if (r->raw_n + add > r->raw_cap) {
        size_t cap = r->raw_cap ? r->raw_cap : 256;
        while (cap < r->raw_n + add) cap *= 2;
        for (l = 0; l < LAYOUTS; l++) {
            uint8_t* grown;
            if (c->mode >= 0 && l != c->mode / ORDERS) continue;
            grown = (uint8_t*)realloc(r->raw[l], cap * PACK);
            if (!grown) { stop(c); return; }
            r->raw[l] = grown;
        }
        r->raw_cap = cap;
    }
    for (k = 0; k < end - lba; k++) {
        size_t at = (r->raw_n + (size_t)k * PACKS_PER_SECTOR) * PACK;
        unpack_sector(sub + (size_t)k * FA_CD_SUB_BYTES, r->raw[LAYOUT_BYTES] ? r->raw[LAYOUT_BYTES] + at : NULL,
                      r->raw[LAYOUT_CHANNELS] ? r->raw[LAYOUT_CHANNELS] + at : NULL);
    }
    r->raw_n += add;

    if (c->mode == MODE_UNDECIDED) judge(c, run_final(c, r));
    else emit(c, r, run_final(c, r));

    /* Keep only what later packs still need (everything until a reading is chosen). */
    if (c->mode >= 0 && r->done > r->raw_base) {
        size_t drop = r->done - r->raw_base;
        uint8_t* raw = r->raw[c->mode / ORDERS];
        memmove(raw, raw + drop * PACK, (r->raw_n - drop) * PACK);
        r->raw_base += drop;
        r->raw_n -= drop;
    }
}
