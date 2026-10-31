/* DLS (Downloadable Sounds, Level 1 and 2) banks, as Windows (gm.dls) and macOS
 * (gs_instruments.dls) ship them, turned into a SoundFont 2 image in memory for
 * the synthesizer. Written from the MMA DLS Level 1 and Level 2 specifications.
 *
 * A DLS bank is RIFF 'DLS ': instruments (LIST lins > LIST ins), each with
 * regions (LIST lrgn > LIST rgn/rgn2: key and velocity range, sample settings in
 * wsmp, the wave in wlnk, articulation in LIST lart/lar2), a pool table (ptbl)
 * of offsets into the wave pool (LIST wvpl > LIST wave: fmt, wsmp, data).
 *
 * The mapping: one SF2 preset per instrument (bank = CC0, drum kits on bank 128),
 * one SF2 instrument zone per region. Articulation connections without a
 * controller map onto the SF2 generator of the same meaning: envelope times are
 * timecents in both formats, sustain levels and gains are converted to SF2's
 * centibels, and the defaults of both formats (velocity to volume, mod wheel to
 * vibrato) already agree. Each wave becomes one block of 16-bit samples; a
 * region whose loop differs from another's gets a sample header of its own over
 * the same block. Connections with a MIDI controller, velocity, key or pressure
 * as source or control (velocity to attack time, a controller scaling the LFO, key to
 * filter cutoff) become SF2 instrument modulators. DLS 2 conditional chunks ('cdl ')
 * leave out the instruments, regions and articulations whose condition this synth
 * doesn't meet. Regions on different DLS 2 layers may overlap and all play, as do SF2
 * zones; the layer number itself only groups regions for editors. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DLS_MAX_INSTRUMENTS 4096
#define SF2_PAD 46                    /* zero samples after each sample block */
#define SF2_MAX_RECORDS 0xFFFF        /* SF2 indexes zones, generators and samples in 16 bits */
#define HDR_HASH_SIZE 131072u         /* power of two, over twice SF2_MAX_RECORDS */

/* ---- RIFF walking ------------------------------------------------------------ */

typedef struct chunk {
    char id[4];
    char type[4];               /* LIST type, "" for other chunks */
    const uint8_t* data;        /* body (a LIST's after its type) */
    uint32_t size;
} chunk;

/* The next chunk in [*p, end): 0 at the end or on a malformed header. */
static int next_chunk(const uint8_t** p, const uint8_t* end, chunk* c)
{
    uint32_t size;
    if (end - *p < 8) return 0;
    memcpy(c->id, *p, 4);
    size = fa_le32(*p + 4);
    if (size > (size_t)(end - *p - 8)) size = (uint32_t)(end - *p - 8);   /* truncated: what is there */
    c->data = *p + 8;
    c->size = size;
    memset(c->type, 0, 4);
    if ((memcmp(c->id, "LIST", 4) == 0 || memcmp(c->id, "RIFF", 4) == 0) && size >= 4) {
        memcpy(c->type, c->data, 4);
        c->data += 4;
        c->size -= 4;
    }
    *p += 8 + (size_t)size + (size & 1);
    if (*p > end) *p = end;
    return 1;
}

static int is_list(const chunk* c, const char* type) { return memcmp(c->id, "LIST", 4) == 0 && memcmp(c->type, type, 4) == 0; }
static int is_id(const chunk* c, const char* id) { return memcmp(c->id, id, 4) == 0; }

/* ---- DLS 2 conditional chunks ----------------------------------------------------- */

/* A 'cdl ' chunk is an expression over what the synthesizer supports (DLS 2 section
 * 1.14): when it is false, the list holding it (an instrument, a region, an articulation)
 * is not used. FatzAudio answers as a software DLS 2 synthesizer with no instruments of its
 * own: GM, GS and XG sets "in hardware" no (the bank's own instruments are wanted), DLS 1 and
 * DLS 2 yes, sample memory 1 GB, playback rate 44100 Hz, manufacturer and product 0. A query
 * it doesn't know is 0 (and "supported" 0). An expression it can't read counts as true, so a
 * malformed chunk loses nothing. */
enum {
    CDL_AND = 1, CDL_OR, CDL_XOR, CDL_ADD, CDL_SUBTRACT, CDL_MULTIPLY, CDL_DIVIDE, CDL_LOGICAL_AND, CDL_LOGICAL_OR,
    CDL_LT, CDL_LE, CDL_GT, CDL_GE, CDL_EQ, CDL_NOT, CDL_CONST, CDL_QUERY, CDL_QUERYSUPPORTED
};

typedef struct cdl_query {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
    uint32_t value;
} cdl_query;

static const cdl_query cdl_queries[] = {
    { 0x178f2f24, 0xc364, 0x11d1, { 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 }, 0 },          /* GM in hardware */
    { 0x178f2f25, 0xc364, 0x11d1, { 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 }, 0 },          /* GS in hardware */
    { 0x178f2f26, 0xc364, 0x11d1, { 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 }, 0 },          /* XG in hardware */
    { 0x178f2f27, 0xc364, 0x11d1, { 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 }, 1 },          /* supports DLS 1 */
    { 0xf14599e5, 0x4689, 0x11d2, { 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6 }, 1 },          /* supports DLS 2 */
    { 0x178f2f28, 0xc364, 0x11d1, { 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 }, 0x40000000 }, /* sample memory */
    { 0xb03e1181, 0x8095, 0x11d2, { 0xa1, 0xef, 0x00, 0x60, 0x08, 0x33, 0xdb, 0xd8 }, 0 },          /* manufacturer */
    { 0xb03e1182, 0x8095, 0x11d2, { 0xa1, 0xef, 0x00, 0x60, 0x08, 0x33, 0xdb, 0xd8 }, 0 },          /* product */
    { 0x2a91f713, 0xa4bf, 0x11d2, { 0xbb, 0xdf, 0x00, 0x60, 0x08, 0x33, 0xdb, 0xd8 }, 44100 },      /* playback rate */
};

/* The query of a GUID (16 bytes as in the file): its value, -1 if unknown. */
static int64_t cdl_query_value(const uint8_t* g)
{
    size_t i;
    for (i = 0; i < sizeof(cdl_queries) / sizeof(cdl_queries[0]); i++) {
        const cdl_query* q = &cdl_queries[i];
        if (fa_le32(g) == q->d1 && fa_le16(g + 4) == q->d2 && fa_le16(g + 6) == q->d3 && memcmp(g + 8, q->d4, 8) == 0)
            return q->value;
    }
    return -1;
}

static int cdl_true(const uint8_t* p, uint32_t n)
{
    uint32_t st[64];
    int sp = 0;
    const uint8_t* end = p + n;
    while (end - p >= 2) {
        unsigned op = fa_le16(p);
        uint32_t a, b, v = 0;
        p += 2;
        if (op == CDL_CONST || op == CDL_QUERY || op == CDL_QUERYSUPPORTED) {
            if (sp == 64) return 1;
            if (op == CDL_CONST) {
                if (end - p < 4) return 1;
                v = fa_le32(p);
                p += 4;
            } else {
                int64_t q;
                if (end - p < 16) return 1;
                q = cdl_query_value(p);
                p += 16;
                v = op == CDL_QUERYSUPPORTED ? q >= 0 : q >= 0 ? (uint32_t)q : 0;
            }
            st[sp++] = v;
            continue;
        }
        if (op == CDL_NOT) {
            if (sp < 1) return 1;
            st[sp - 1] = !st[sp - 1];
            continue;
        }
        if (op < CDL_AND || op > CDL_EQ || sp < 2) return 1;   /* not an operator we know */
        b = st[--sp];
        a = st[sp - 1];
        switch (op) {
        case CDL_AND: v = a & b; break;
        case CDL_OR: v = a | b; break;
        case CDL_XOR: v = a ^ b; break;
        case CDL_ADD: v = a + b; break;
        case CDL_SUBTRACT: v = a - b; break;
        case CDL_MULTIPLY: v = a * b; break;
        case CDL_DIVIDE: v = b ? a / b : 0; break;
        case CDL_LOGICAL_AND: v = a && b; break;
        case CDL_LOGICAL_OR: v = a || b; break;
        case CDL_LT: v = a < b; break;
        case CDL_LE: v = a <= b; break;
        case CDL_GT: v = a > b; break;
        case CDL_GE: v = a >= b; break;
        default: v = a == b; break;
        }
        st[sp - 1] = v;
    }
    return sp == 0 || st[sp - 1] != 0;
}

/* A list's chunks [p, p + n): 0 when a 'cdl ' among them is false. */
static int list_enabled(const uint8_t* p, uint32_t n)
{
    const uint8_t* end = p + n;
    chunk c;
    while (next_chunk(&p, end, &c))
        if (is_id(&c, "cdl ") && !cdl_true(c.data, c.size)) return 0;
    return 1;
}

/* ---- growable output ---------------------------------------------------------- */

typedef struct obuf {
    uint8_t* p;
    size_t n, cap;
    int oom;
} obuf;

static void ob_put(obuf* b, const void* d, size_t n)
{
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t* q;
        while (cap < b->n + n) cap *= 2;
        q = (uint8_t*)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
}
static void ob_u8(obuf* b, unsigned v) { uint8_t x = (uint8_t)v; ob_put(b, &x, 1); }
static void ob_u16(obuf* b, unsigned v) { ob_u8(b, v & 0xFF); ob_u8(b, (v >> 8) & 0xFF); }
static void ob_u32(obuf* b, uint32_t v) { ob_u16(b, v & 0xFFFF); ob_u16(b, v >> 16); }
static void ob_name(obuf* b, const char* s)
{
    char n[20];
    memset(n, 0, sizeof(n));
    if (s) { size_t k = strlen(s); memcpy(n, s, k > 19 ? 19 : k); }
    ob_put(b, n, 20);
}
static void ob_set32(obuf* b, size_t at, uint32_t v)
{
    if (b->oom) return;
    b->p[at] = (uint8_t)v; b->p[at + 1] = (uint8_t)(v >> 8); b->p[at + 2] = (uint8_t)(v >> 16); b->p[at + 3] = (uint8_t)(v >> 24);
}

/* ---- waves ---------------------------------------------------------------------- */

typedef struct dls_wsmp {
    int present;
    int unity;                  /* MIDI note the wave plays at its own pitch */
    int fine;                   /* cents */
    double gain_cb;             /* centibels, + = louder */
    int loop_type;              /* -1 none, 0 forward, 1 loop then play the release */
    uint32_t loop_start, loop_len;
} dls_wsmp;

typedef struct dls_wave {
    uint32_t offset;            /* from the start of the wave pool's data */
    const uint8_t* data;
    uint32_t frames;
    uint16_t channels, bits;
    uint32_t rate;
    dls_wsmp wsmp;
} dls_wave;

static void parse_wsmp(const uint8_t* p, uint32_t n, dls_wsmp* w)
{
    uint32_t cb, loops;
    memset(w, 0, sizeof(*w));
    w->loop_type = -1;
    if (n < 20) return;
    cb = fa_le32(p);
    if (cb < 20 || cb > n) cb = 20;
    w->present = 1;
    w->unity = fa_le16(p + 4) & 0x7F;
    w->fine = (int16_t)fa_le16(p + 6);
    w->gain_cb = (double)(int32_t)fa_le32(p + 8) / 65536.0;
    loops = fa_le32(p + 16);
    if (loops >= 1 && cb + 16 <= n) {
        const uint8_t* l = p + cb;
        uint32_t type = fa_le32(l + 4);
        w->loop_type = type == 1 ? 1 : 0;
        w->loop_start = fa_le32(l + 8);
        w->loop_len = fa_le32(l + 12);
    }
}

/* fmt + data of LIST wave: 0 if not PCM we can use. */
static int parse_wave(const uint8_t* p, uint32_t n, dls_wave* w)
{
    const uint8_t* end = p + n;
    chunk c;
    int have_fmt = 0;
    w->data = NULL;
    w->wsmp.present = 0;
    w->wsmp.loop_type = -1;
    while (next_chunk(&p, end, &c)) {
        if (is_id(&c, "fmt ") && c.size >= 16) {
            uint16_t tag = fa_le16(c.data);
            w->channels = fa_le16(c.data + 2);
            w->rate = fa_le32(c.data + 4);
            w->bits = fa_le16(c.data + 14);
            if (tag == 0xFFFE && c.size >= 26) tag = fa_le16(c.data + 24);   /* WAVE_FORMAT_EXTENSIBLE: the subformat */
            have_fmt = tag == 1;
        } else if (is_id(&c, "data")) {
            w->data = c.data;
            w->frames = c.size;
        } else if (is_id(&c, "wsmp")) {
            parse_wsmp(c.data, c.size, &w->wsmp);
        }
    }
    if (!have_fmt || !w->data || w->channels < 1 || w->channels > 8 || w->rate < 400 || w->rate > 384000 ||
        (w->bits != 8 && w->bits != 16 && w->bits != 24 && w->bits != 32))
        return 0;
    w->frames /= (uint32_t)w->channels * (w->bits / 8u);
    return w->frames > 0;
}

/* One sample point of channel `ch` (-1: the mix of all) as 16 bits. */
static int16_t wave_sample(const dls_wave* w, uint32_t frame, int ch)
{
    int c, from = ch < 0 ? 0 : ch, to = ch < 0 ? w->channels : ch + 1;
    int bytes = w->bits / 8;
    long sum = 0;
    for (c = from; c < to; c++) {
        const uint8_t* s = w->data + ((size_t)frame * w->channels + (size_t)c) * (size_t)bytes;
        long v;
        if (bytes == 1) v = ((long)s[0] - 128) * 256;
        else v = (int16_t)fa_le16(s + bytes - 2);       /* the top 16 bits */
        sum += v;
    }
    return (int16_t)(sum / (to - from));
}

/* ---- generators ----------------------------------------------------------------- */

enum {
    G_START_OFS = 0, G_MODLFO_PITCH = 5, G_VIBLFO_PITCH = 6, G_MODENV_PITCH = 7, G_FILTER_FC = 8, G_FILTER_Q = 9,
    G_MODLFO_FC = 10, G_MODENV_FC = 11, G_MODLFO_VOL = 13, G_PAN = 17, G_MODLFO_DELAY = 21, G_MODLFO_FREQ = 22,
    G_VIBLFO_DELAY = 23, G_VIBLFO_FREQ = 24, G_MODENV_DELAY = 25, G_MODENV_ATTACK = 26, G_MODENV_HOLD = 27,
    G_MODENV_DECAY = 28, G_MODENV_SUSTAIN = 29, G_MODENV_RELEASE = 30, G_KEY_MODENV_HOLD = 31, G_KEY_MODENV_DECAY = 32,
    G_VOLENV_DELAY = 33, G_VOLENV_ATTACK = 34, G_VOLENV_HOLD = 35, G_VOLENV_DECAY = 36, G_VOLENV_SUSTAIN = 37,
    G_VOLENV_RELEASE = 38, G_KEY_VOLENV_HOLD = 39, G_KEY_VOLENV_DECAY = 40, G_INSTRUMENT = 41, G_KEYRANGE = 43,
    G_CHORUS = 15, G_REVERB = 16, G_VELRANGE = 44, G_ATTENUATION = 48, G_FINETUNE = 52, G_SAMPLE = 53, G_SAMPLEMODES = 54, G_EXCLUSIVE = 57,
    G_ROOTKEY = 58, G_COUNT = 60
};

#define ZONE_MODS 16                  /* modulators a zone keeps (connections with a controller) */

/* An SF2 modulator record (SF2 2.04 section 7.4). */
typedef struct sf2_mod {
    uint16_t src, dst;
    int16_t amount;
    uint16_t amt_src, trans;
} sf2_mod;

typedef struct gens {
    uint8_t set[G_COUNT];
    double v[G_COUNT];
    sf2_mod mods[ZONE_MODS];
    uint32_t nmods;
    /* Key number to EG1 hold and decay, EG2 hold and decay (DLS scale), turned
     * into SF2 generators once the zone is complete (finish_zone). */
    uint8_t key_set[4];
    double key[4];
} gens;

static void gen_set(gens* g, int id, double v) { g->set[id] = 1; g->v[id] = v; }
static void gen_add(gens* g, int id, double v) { if (!g->set[id]) g->v[id] = 0.0; g->set[id] = 1; g->v[id] += v; }

/* A DLS time (timecents * 65536; 0x80000000 = no time at all). */
static double timecents(int32_t scale) { return scale == INT32_MIN ? -12000.0 : (double)scale / 65536.0; }

/* A sustain level in 0.1 % as SF2 attenuation in centibels. */
static double level_to_cb(double permille)
{
    if (permille >= 1000.0) return 0.0;
    if (permille <= 0.0) return 1440.0;
    return -200.0 * log10(permille / 1000.0);
}

/* Connection sources and destinations of DLS Level 1 and 2. */
enum {
    SRC_NONE = 0, SRC_LFO = 1, SRC_VELOCITY = 2, SRC_KEY = 3, SRC_EG1 = 4, SRC_EG2 = 5, SRC_PITCH_WHEEL = 6,
    SRC_POLY_PRESSURE = 7, SRC_CHANNEL_PRESSURE = 8, SRC_VIBRATO = 9, SRC_CC = 0x80, SRC_RPN0 = 0x100,
    DST_CHORUS = 0x80, DST_REVERB = 0x81,
    DST_GAIN = 1, DST_PITCH = 3, DST_PAN = 4, DST_LFO_FREQ = 0x104, DST_LFO_DELAY = 0x105, DST_VIB_FREQ = 0x114,
    DST_VIB_DELAY = 0x115, DST_EG1_ATTACK = 0x206, DST_EG1_DECAY = 0x207, DST_EG1_RELEASE = 0x209,
    DST_EG1_SUSTAIN = 0x20A, DST_EG1_DELAY = 0x20B, DST_EG1_HOLD = 0x20C, DST_EG2_ATTACK = 0x30A,
    DST_EG2_DECAY = 0x30B, DST_EG2_RELEASE = 0x30D, DST_EG2_SUSTAIN = 0x30E, DST_EG2_DELAY = 0x30F,
    DST_EG2_HOLD = 0x310, DST_FILTER_FC = 0x500, DST_FILTER_Q = 0x501
};

/* A DLS source (a MIDI controller or what the synth knows of the note) as an SF2 modulator
 * source operand, with the connection's curve, polarity and direction; 0 when SF2 has no
 * such source. DLS 2 packs those into the transform: source bits 10-15, control bits 4-9. */
static uint16_t mod_source(unsigned src, unsigned trans, int control, int art2)
{
    unsigned index, curve, bipolar, invert;
    if (src == SRC_VELOCITY) index = 2;
    else if (src == SRC_KEY) index = 3;
    else if (src == SRC_POLY_PRESSURE) index = 10;
    else if (src == SRC_CHANNEL_PRESSURE) index = 13;
    else if (src == SRC_PITCH_WHEEL) index = 14;
    else if (src >= SRC_CC && src < SRC_CC + 128) index = 0x80 | (src - SRC_CC);
    else return 0;
    if (art2) {
        unsigned bits = control ? (trans >> 4) & 0x3F : (trans >> 10) & 0x3F;
        curve = bits & 0xF;
        bipolar = (bits >> 4) & 1;
        invert = (bits >> 5) & 1;
        if (curve > 3) curve = 0;
    } else {
        curve = !control && trans == 1 ? 1 : 0;   /* DLS 1: concave, or nothing */
        bipolar = invert = 0;
    }
    if (src == SRC_PITCH_WHEEL) bipolar = 1;   /* the wheel is centred whatever the writer says */
    return (uint16_t)(curve << 10 | bipolar << 9 | invert << 8 | index);
}

/* The SF2 generator a DLS destination becomes, and the factor from DLS units to its; -1 none. */
static int mod_dest(unsigned dst, double* unit)
{
    *unit = 1.0;
    switch (dst) {
    case DST_GAIN: *unit = -1.0; return G_ATTENUATION;    /* + = louder in DLS, quieter in SF2 */
    case DST_PITCH: return G_FINETUNE;
    case DST_PAN: return G_PAN;
    case DST_FILTER_FC: return G_FILTER_FC;
    case DST_FILTER_Q: *unit = 10.0; return G_FILTER_Q;
    case DST_EG1_ATTACK: return G_VOLENV_ATTACK;
    case DST_EG1_DECAY: return G_VOLENV_DECAY;
    case DST_EG1_RELEASE: return G_VOLENV_RELEASE;
    case DST_EG1_DELAY: return G_VOLENV_DELAY;
    case DST_EG1_HOLD: return G_VOLENV_HOLD;
    case DST_EG2_ATTACK: return G_MODENV_ATTACK;
    case DST_EG2_DECAY: return G_MODENV_DECAY;
    case DST_EG2_RELEASE: return G_MODENV_RELEASE;
    case DST_EG2_DELAY: return G_MODENV_DELAY;
    case DST_EG2_HOLD: return G_MODENV_HOLD;
    case DST_LFO_FREQ: return G_MODLFO_FREQ;
    case DST_LFO_DELAY: return G_MODLFO_DELAY;
    case DST_VIB_FREQ: return G_VIBLFO_FREQ;
    case DST_VIB_DELAY: return G_VIBLFO_DELAY;
    case DST_CHORUS: return G_CHORUS;
    case DST_REVERB: return G_REVERB;
    default: return -1;
    }
}

/* Adds (or replaces: the same sources and destination) a modulator of the zone. */
static void add_mod(gens* g, uint16_t src, uint16_t dst, double amount, uint16_t amt_src)
{
    uint32_t i;
    long a = lround(amount);
    if (a < -32768) a = -32768;
    if (a > 32767) a = 32767;
    for (i = 0; i < g->nmods; i++)
        if (g->mods[i].src == src && g->mods[i].dst == dst && g->mods[i].amt_src == amt_src) break;
    if (i == g->nmods) {
        if (g->nmods == ZONE_MODS) return;
        g->nmods++;
    }
    g->mods[i].src = src;
    g->mods[i].dst = dst;
    g->mods[i].amount = (int16_t)a;
    g->mods[i].amt_src = amt_src;
    g->mods[i].trans = 0;
}

/* A connection with a MIDI controller in it (as its source or its control): an SF2
 * modulator. An LFO or envelope scaled by a controller becomes a modulator of the depth
 * generator (LFO x mod wheel -> pitch: the mod wheel to the vibrato depth, as SF2's own
 * default does). The connections SF2 synths apply anyway (pitch wheel x RPN 0 -> pitch, and
 * the mod wheel or channel pressure to 50 cents of vibrato) are left to the synth's
 * defaults, which follow the channel's modulation depth range. */
static void apply_controlled(gens* g, unsigned src, unsigned ctl, unsigned dst, unsigned trans, int32_t scale, int art2)
{
    double s = (double)scale / 65536.0, unit;
    int depth = -1, gen;
    uint16_t a, b;
    if (src == SRC_LFO || src == SRC_VIBRATO || src == SRC_EG2) {
        if (src == SRC_EG2) depth = dst == DST_PITCH ? G_MODENV_PITCH : dst == DST_FILTER_FC ? G_MODENV_FC : -1;
        else if (dst == DST_PITCH) depth = G_VIBLFO_PITCH;   /* the LFO the mod wheel drives */
        else if (dst == DST_GAIN) depth = G_MODLFO_VOL;
        else if (dst == DST_FILTER_FC) depth = G_MODLFO_FC;
        a = mod_source(ctl, trans, 1, art2);
        if (depth < 0 || !a) return;
        if (depth == G_VIBLFO_PITCH && (a == 0x0081 || a == 0x000D) && lround(s) == 50) return;   /* SF2's defaults */
        add_mod(g, a, (uint16_t)depth, s, 0);
        return;
    }
    if (src == SRC_PITCH_WHEEL && ctl == SRC_RPN0 && dst == DST_PITCH) return;   /* the synth's bend */
    gen = mod_dest(dst, &unit);
    if (gen < 0) return;
    if (src == SRC_NONE) { src = ctl; ctl = SRC_NONE; trans = (trans & ~0xFC00u) | ((trans >> 4 & 0x3F) << 10); }
    /* DLS's implied connections written out by some editors (CC7 and CC11 to gain, CC10 to
     * pan, CC91 and CC93 to the sends): the synth applies those itself, whatever curve bits
     * the file gives them, so a modulator would apply them twice. */
    if (ctl == SRC_NONE && ((gen == G_ATTENUATION && (src == SRC_CC + 7 || src == SRC_CC + 11)) || (gen == G_PAN && src == SRC_CC + 10) ||
                            (gen == G_REVERB && src == SRC_CC + 91) || (gen == G_CHORUS && src == SRC_CC + 93)))
        return;
    a = mod_source(src, trans, 0, art2);
    b = ctl == SRC_NONE ? 0 : mod_source(ctl, trans, 1, art2);
    if (!a || (ctl != SRC_NONE && !b)) return;
    add_mod(g, a, (uint16_t)gen, s * unit, b);
}

static void apply_connection(gens* g, unsigned src, unsigned ctl, unsigned dst, unsigned trans, int32_t scale, int art2)
{
    double s = (double)scale / 65536.0;
    if (ctl != SRC_NONE || (src >= SRC_PITCH_WHEEL && src != SRC_VIBRATO)) {
        apply_controlled(g, src, ctl, dst, trans, scale, art2);
        return;
    }
    switch (src) {
    case SRC_NONE:
        switch (dst) {
        case DST_EG1_ATTACK: gen_set(g, G_VOLENV_ATTACK, timecents(scale)); break;
        case DST_EG1_DECAY: gen_set(g, G_VOLENV_DECAY, timecents(scale)); break;
        case DST_EG1_RELEASE: gen_set(g, G_VOLENV_RELEASE, timecents(scale)); break;
        case DST_EG1_DELAY: gen_set(g, G_VOLENV_DELAY, timecents(scale)); break;
        case DST_EG1_HOLD: gen_set(g, G_VOLENV_HOLD, timecents(scale)); break;
        case DST_EG1_SUSTAIN: gen_set(g, G_VOLENV_SUSTAIN, level_to_cb(s)); break;
        case DST_EG2_ATTACK: gen_set(g, G_MODENV_ATTACK, timecents(scale)); break;
        case DST_EG2_DECAY: gen_set(g, G_MODENV_DECAY, timecents(scale)); break;
        case DST_EG2_RELEASE: gen_set(g, G_MODENV_RELEASE, timecents(scale)); break;
        case DST_EG2_DELAY: gen_set(g, G_MODENV_DELAY, timecents(scale)); break;
        case DST_EG2_HOLD: gen_set(g, G_MODENV_HOLD, timecents(scale)); break;
        case DST_EG2_SUSTAIN: gen_set(g, G_MODENV_SUSTAIN, 1000.0 - s); break;
        case DST_LFO_FREQ: gen_set(g, G_MODLFO_FREQ, s); break;
        case DST_LFO_DELAY: gen_set(g, G_MODLFO_DELAY, timecents(scale)); break;
        case DST_VIB_FREQ: gen_set(g, G_VIBLFO_FREQ, s); break;
        case DST_VIB_DELAY: gen_set(g, G_VIBLFO_DELAY, timecents(scale)); break;
        case DST_GAIN: gen_add(g, G_ATTENUATION, -s); break;
        case DST_PITCH: gen_add(g, G_FINETUNE, s); break;
        case DST_PAN: gen_set(g, G_PAN, s); break;
        case DST_FILTER_FC: gen_set(g, G_FILTER_FC, s); break;
        case DST_FILTER_Q: gen_set(g, G_FILTER_Q, s * 10.0); break;     /* dB to centibels */
        case DST_CHORUS: gen_set(g, G_CHORUS, s); break;                 /* 0.1 % in both */
        case DST_REVERB: gen_set(g, G_REVERB, s); break;
        default: break;
        }
        break;
    case SRC_LFO:
        if (dst == DST_PITCH) gen_set(g, G_MODLFO_PITCH, s);
        else if (dst == DST_GAIN) gen_set(g, G_MODLFO_VOL, s);   /* both: + = louder at the LFO's peak */
        else if (dst == DST_FILTER_FC) gen_set(g, G_MODLFO_FC, s);
        break;
    case SRC_VIBRATO:
        if (dst == DST_PITCH) gen_set(g, G_VIBLFO_PITCH, s);
        break;
    case SRC_EG2:
        if (dst == DST_PITCH) gen_set(g, G_MODENV_PITCH, s);
        else if (dst == DST_FILTER_FC) gen_set(g, G_MODENV_FC, s);
        break;
    case SRC_KEY: {
        int k = dst == DST_EG1_HOLD ? 0 : dst == DST_EG1_DECAY ? 1 : dst == DST_EG2_HOLD ? 2 : dst == DST_EG2_DECAY ? 3 : -1;
        if (k >= 0) { g->key_set[k] = 1; g->key[k] = s; }
        else if (dst != DST_PITCH) apply_controlled(g, src, ctl, dst, trans, scale, art2);   /* key to filter cutoff... */
        break;
    }
    case SRC_VELOCITY:
        /* Velocity to volume is the synth's own curve (as SF2's default); velocity to
         * attack time, filter cutoff and the rest are modulators. */
        if (dst != DST_GAIN) apply_controlled(g, src, ctl, dst, trans, scale, art2);
        break;
    default:
        break;
    }
}

/* What needs the whole zone: key tracking and the LFO defaults. */
static void finish_zone(gens* g)
{
    static const int key_gen[4] = { G_KEY_VOLENV_HOLD, G_KEY_VOLENV_DECAY, G_KEY_MODENV_HOLD, G_KEY_MODENV_DECAY };
    static const int base_gen[4] = { G_VOLENV_HOLD, G_VOLENV_DECAY, G_MODENV_HOLD, G_MODENV_DECAY };
    int k;
    /* DLS: time = base + scale * key / 128 (from key 0). SF2: time = base' +
     * keynumTo * (60 - key). So keynumTo = -scale / 128, base' = base + 60 * scale / 128. */
    for (k = 0; k < 4; k++) {
        if (!g->key_set[k]) continue;
        gen_set(g, key_gen[k], -g->key[k] / 128.0);
        gen_set(g, base_gen[k], (g->set[base_gen[k]] ? g->v[base_gen[k]] : -12000.0) + 60.0 * g->key[k] / 128.0);
    }
    /* DLS 1 has one LFO, which the default mod wheel vibrato also uses: SF2 drives
     * that from its vibrato LFO, so it takes the LFO's rate and delay unless a DLS 2
     * vibrato LFO sets its own. Unset rates are DLS's 5 Hz (SF2's default is 8.18 Hz). */
    if (!g->set[G_MODLFO_FREQ]) {
        int uses_lfo = g->set[G_MODLFO_PITCH] || g->set[G_MODLFO_VOL] || g->set[G_MODLFO_FC];
        uint32_t i;
        /* Also an LFO only a controller scales (a modulator of its depth). */
        for (i = 0; i < g->nmods && !uses_lfo; i++)
            uses_lfo = g->mods[i].dst == G_MODLFO_PITCH || g->mods[i].dst == G_MODLFO_VOL || g->mods[i].dst == G_MODLFO_FC;
        if (uses_lfo) gen_set(g, G_MODLFO_FREQ, -851.0);
    }
    if (!g->set[G_VIBLFO_FREQ]) gen_set(g, G_VIBLFO_FREQ, g->set[G_MODLFO_FREQ] ? g->v[G_MODLFO_FREQ] : -851.0);
    if (!g->set[G_VIBLFO_DELAY] && g->set[G_MODLFO_DELAY]) gen_set(g, G_VIBLFO_DELAY, g->v[G_MODLFO_DELAY]);
}

/* art1 / art2 connection blocks of a LIST lart or lar2. */
static void parse_articulation(const uint8_t* p, uint32_t n, gens* g)
{
    const uint8_t* end = p + n;
    chunk c;
    while (next_chunk(&p, end, &c)) {
        uint32_t cb, count, i;
        if (!is_id(&c, "art1") && !is_id(&c, "art2")) continue;
        if (c.size < 8) continue;
        cb = fa_le32(c.data);
        count = fa_le32(c.data + 4);
        if (cb < 8 || cb > c.size) continue;
        for (i = 0; i < count && cb + (size_t)(i + 1) * 12 <= c.size; i++) {
            const uint8_t* b = c.data + cb + (size_t)i * 12;
            apply_connection(g, fa_le16(b), fa_le16(b + 2), fa_le16(b + 4), fa_le16(b + 6), (int32_t)fa_le32(b + 8), is_id(&c, "art2"));
        }
    }
}

/* ---- the conversion --------------------------------------------------------------- */

typedef struct sample_hdr {
    uint32_t wave;
    int channel;                /* -1: the mix */
    int loop_type;
    uint32_t loop_start, loop_len;
    int unity;
} sample_hdr;

typedef struct block {          /* one wave (or channel of it) as 16-bit samples in smpl */
    uint32_t wave;
    int channel;
    uint32_t start;             /* in samples */
} block;

typedef struct zone {
    gens g;
    uint32_t sample;
} zone;

typedef struct preset {
    char name[20];
    uint16_t bank, program;
    uint32_t first_zone, zones;
} preset;

typedef struct conv {
    dls_wave* waves;
    uint32_t nwaves;
    uint32_t* cues;             /* pool table index -> wave */
    uint32_t ncues;
    sample_hdr* hdrs;
    uint32_t nhdrs, cap_hdrs;
    uint32_t* hdr_hash;         /* HDR_HASH_SIZE slots: header index + 1, 0 = free */
    zone* zones;
    uint32_t nzones, cap_zones;
    preset* presets;
    uint32_t npresets;
    int oom;
} conv;

static void* grow(void* p, uint32_t* cap, uint32_t need, size_t elem, int* oom)
{
    uint32_t c = *cap ? *cap : 64;
    void* q;
    if (need <= *cap) return p;
    while (c < need) c *= 2;
    q = realloc(p, (size_t)c * elem);
    if (!q) { *oom = 1; return p; }
    *cap = c;
    return q;
}

static uint32_t sample_for(conv* cv, uint32_t wave, int channel, const dls_wsmp* w)
{
    uint32_t slot;
    sample_hdr h;
    memset(&h, 0, sizeof(h));
    h.wave = wave;
    h.channel = channel;
    h.unity = w->present ? w->unity : 60;
    h.loop_type = w->loop_type;
    if (h.loop_type >= 0) {
        uint32_t frames = cv->waves[wave].frames;
        if (w->loop_start >= frames || w->loop_len < 2 || w->loop_len > frames - w->loop_start) h.loop_type = -1;
        else { h.loop_start = w->loop_start; h.loop_len = w->loop_len; }
    }
    /* An open-addressing table finds a header already made for the same wave,
     * channel, loop and root key. */
    slot = (h.wave * 2654435761u ^ (uint32_t)(h.channel + 1) * 40503u ^ h.loop_start * 2246822519u ^ h.loop_len * 3266489917u ^
            (uint32_t)(h.loop_type + 1) * 668265263u ^ (uint32_t)h.unity * 374761393u) & (HDR_HASH_SIZE - 1);
    for (;; slot = (slot + 1) & (HDR_HASH_SIZE - 1)) {
        const sample_hdr* o;
        if (!cv->hdr_hash[slot]) break;
        o = &cv->hdrs[cv->hdr_hash[slot] - 1];
        if (o->wave == h.wave && o->channel == h.channel && o->loop_type == h.loop_type && o->loop_start == h.loop_start &&
            o->loop_len == h.loop_len && o->unity == h.unity)
            return cv->hdr_hash[slot] - 1;
    }
    cv->hdrs = (sample_hdr*)grow(cv->hdrs, &cv->cap_hdrs, cv->nhdrs + 1, sizeof(*cv->hdrs), &cv->oom);
    if (cv->oom) return 0;
    cv->hdrs[cv->nhdrs] = h;
    cv->hdr_hash[slot] = cv->nhdrs + 1;
    return cv->nhdrs++;
}

static void parse_region(conv* cv, const uint8_t* p, uint32_t n, const gens* inst_art)
{
    const uint8_t* end = p + n;
    chunk c;
    int have_rgnh = 0, have_wlnk = 0, own_art = 0;
    unsigned klo = 0, khi = 127, vlo = 0, vhi = 127, group = 0;
    uint32_t cue = 0, channel_mask = 0;
    dls_wsmp wsmp;
    gens g = *inst_art;
    zone* z;
    const dls_wave* w;
    int channel = -1;
    wsmp.present = 0;
    if (!list_enabled(p, n)) return;   /* a DLS 2 condition this synth doesn't meet */
    while (next_chunk(&p, end, &c)) {
        if (is_id(&c, "rgnh") && c.size >= 12) {
            klo = fa_le16(c.data); khi = fa_le16(c.data + 2);
            vlo = fa_le16(c.data + 4); vhi = fa_le16(c.data + 6);
            group = fa_le16(c.data + 10);
            have_rgnh = 1;
        } else if (is_id(&c, "wsmp")) {
            parse_wsmp(c.data, c.size, &wsmp);
        } else if (is_id(&c, "wlnk") && c.size >= 12) {
            channel_mask = fa_le32(c.data + 4);
            cue = fa_le32(c.data + 8);
            have_wlnk = 1;
        } else if ((is_list(&c, "lart") || is_list(&c, "lar2")) && list_enabled(c.data, c.size)) {
            /* The region's own articulation replaces the instrument's. */
            if (!own_art) { memset(&g, 0, sizeof(g)); own_art = 1; }
            parse_articulation(c.data, c.size, &g);
        }
    }
    if (!have_rgnh || !have_wlnk || cue >= cv->ncues || cv->cues[cue] == UINT32_MAX) return;
    if (cv->nzones >= SF2_MAX_RECORDS || cv->nhdrs >= SF2_MAX_RECORDS) return;   /* past what SF2 can index */
    if (klo > 127 || vlo > 127 || klo > khi || vlo > vhi) return;
    if (khi > 127) khi = 127;
    if (vhi > 127) vhi = 127;
    /* Velocity ranges of 0..0 (DLS 1 writers that leave it unset) mean all of them. */
    if (vlo == 0 && vhi == 0) vhi = 127;
    w = &cv->waves[cv->cues[cue]];
    if (!wsmp.present) wsmp = w->wsmp;
    /* A stereo wave: the channel this region names (bit 0 left, bit 1 right), else the mix. */
    if (w->channels > 1 && channel_mask) {
        int b;
        for (b = 0; b < w->channels && b < 32; b++) if (channel_mask & (1u << b)) { channel = b; break; }
    }
    cv->zones = (zone*)grow(cv->zones, &cv->cap_zones, cv->nzones + 1, sizeof(*cv->zones), &cv->oom);
    if (cv->oom) return;
    z = &cv->zones[cv->nzones++];
    z->g = g;
    gen_set(&z->g, G_KEYRANGE, klo | (khi << 8));
    gen_set(&z->g, G_VELRANGE, vlo | (vhi << 8));
    if (wsmp.present) {
        gen_add(&z->g, G_ATTENUATION, -wsmp.gain_cb);
        gen_add(&z->g, G_FINETUNE, wsmp.fine);
        gen_set(&z->g, G_ROOTKEY, wsmp.unity);
    }
    if (group) gen_set(&z->g, G_EXCLUSIVE, group);
    z->sample = sample_for(cv, cv->cues[cue], channel, &wsmp);
    if (cv->oom) return;
    if (cv->hdrs[z->sample].loop_type >= 0) gen_set(&z->g, G_SAMPLEMODES, cv->hdrs[z->sample].loop_type == 1 ? 3 : 1);
    finish_zone(&z->g);
}

static void info_name(const uint8_t* p, uint32_t n, char* out, size_t cap)
{
    const uint8_t* end = p + n;
    chunk c;
    while (next_chunk(&p, end, &c)) {
        if (is_id(&c, "INAM")) {
            size_t k = 0;
            while (k < c.size && k + 1 < cap && c.data[k]) { out[k] = (char)c.data[k]; k++; }
            out[k] = '\0';
            return;
        }
    }
}

static void parse_instrument(conv* cv, const uint8_t* p, uint32_t n)
{
    const uint8_t* end = p + n;
    const uint8_t* lrgn = NULL;
    uint32_t lrgn_n = 0, bank = 0, program = 0, i;
    int have_insh = 0;
    gens art;
    chunk c;
    preset* pr;
    char name[64] = "";
    memset(&art, 0, sizeof(art));
    if (!list_enabled(p, n)) return;
    while (next_chunk(&p, end, &c)) {
        if (is_id(&c, "insh") && c.size >= 12) {
            bank = fa_le32(c.data + 4);
            program = fa_le32(c.data + 8);
            have_insh = 1;
        } else if (is_list(&c, "lrgn")) {
            lrgn = c.data;
            lrgn_n = c.size;
        } else if ((is_list(&c, "lart") || is_list(&c, "lar2")) && list_enabled(c.data, c.size)) {
            parse_articulation(c.data, c.size, &art);
        } else if (is_list(&c, "INFO")) {
            info_name(c.data, c.size, name, sizeof(name));
        }
    }
    if (!have_insh || !lrgn || cv->npresets >= DLS_MAX_INSTRUMENTS) return;
    {
        uint16_t b, prog = (uint16_t)(program & 0x7F);
        if (bank & 0x80000000u) b = 128;
        else {
            unsigned msb = (bank >> 8) & 0x7F, lsb = bank & 0x7F;
            b = (uint16_t)(msb ? msb : lsb);
        }
        /* A bank and program already taken: the first one stays. */
        for (i = 0; i < cv->npresets; i++)
            if (cv->presets[i].bank == b && cv->presets[i].program == prog) return;
        pr = &cv->presets[cv->npresets];
        memset(pr, 0, sizeof(*pr));
        pr->bank = b;
        pr->program = prog;
    }
    if (name[0]) memcpy(pr->name, name, strlen(name) < 19 ? strlen(name) : 19);
    else snprintf(pr->name, sizeof(pr->name), "Instrument %u", (unsigned)pr->program);
    pr->first_zone = cv->nzones;
    {
        const uint8_t* q = lrgn;
        const uint8_t* qe = lrgn + lrgn_n;
        while (next_chunk(&q, qe, &c) && !cv->oom)
            if (is_list(&c, "rgn ") || is_list(&c, "rgn2")) parse_region(cv, c.data, c.size, &art);
    }
    pr->zones = cv->nzones - pr->first_zone;
    if (pr->zones) cv->npresets++;
}

/* The generators of a zone in SF2 order (key range, velocity range first, the
 * sample last), leaving out values equal to SF2's defaults. */
static uint32_t write_gens(obuf* b, const gens* g, uint32_t sample)
{
    static const double defaults[G_COUNT] = {
        [G_FILTER_FC] = 13500, [G_MODLFO_DELAY] = -12000, [G_VIBLFO_DELAY] = -12000, [G_MODENV_DELAY] = -12000,
        [G_MODENV_ATTACK] = -12000, [G_MODENV_HOLD] = -12000, [G_MODENV_DECAY] = -12000, [G_MODENV_RELEASE] = -12000,
        [G_VOLENV_DELAY] = -12000, [G_VOLENV_ATTACK] = -12000, [G_VOLENV_HOLD] = -12000, [G_VOLENV_DECAY] = -12000,
        [G_VOLENV_RELEASE] = -12000, [G_ROOTKEY] = -1
    };
    uint32_t count = 0;
    int id;
    if (g->set[G_KEYRANGE]) { ob_u16(b, G_KEYRANGE); ob_u16(b, (unsigned)g->v[G_KEYRANGE]); count++; }
    if (g->set[G_VELRANGE]) { ob_u16(b, G_VELRANGE); ob_u16(b, (unsigned)g->v[G_VELRANGE]); count++; }
    for (id = 0; id < G_COUNT; id++) {
        long v;
        if (!g->set[id] || id == G_KEYRANGE || id == G_VELRANGE || id == G_SAMPLE || id == G_INSTRUMENT) continue;
        v = lround(g->v[id]);
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        if (id == G_ATTENUATION) {
            if (v < 0) v = 0;
            if (v > 1440) v = 1440;     /* 144 dB, the SF2 range */
        }
        if (v == lround(defaults[id])) continue;
        ob_u16(b, (unsigned)id);
        ob_u16(b, (unsigned)(uint16_t)(int16_t)v);
        count++;
    }
    ob_u16(b, G_SAMPLE);
    ob_u16(b, sample);
    return count + 1;
}

static void write_chunk_hdr(obuf* b, const char* id, uint32_t size) { ob_put(b, id, 4); ob_u32(b, size); }

static fa_result write_sf2(conv* cv, const char* bank_name, uint8_t** out, size_t* out_n)
{
    obuf o = { 0 }, smpl = { 0 }, phdr = { 0 }, pbag = { 0 }, pgen = { 0 }, inst = { 0 }, ibag = { 0 }, igen = { 0 }, shdr = { 0 };
    obuf imod = { 0 };
    block* blocks = NULL;
    uint32_t nblocks = 0, cap_blocks = 0, i, j, ngen = 0, nbag = 0, nmod = 0;
    size_t riff_at, pdta_at;
    fa_result r = FA_ERROR_OUT_OF_MEMORY;

    /* Sample data: each wave (or channel) once, then the headers over it. */
    for (i = 0; i < cv->nhdrs && !cv->oom; i++) {
        const sample_hdr* h = &cv->hdrs[i];
        const dls_wave* w = &cv->waves[h->wave];
        block* bl = NULL;
        char name[20];
        for (j = 0; j < nblocks; j++)
            if (blocks[j].wave == h->wave && blocks[j].channel == h->channel) { bl = &blocks[j]; break; }
        if (!bl) {
            uint32_t f;
            if ((smpl.n / 2) + (size_t)w->frames + SF2_PAD > 0x7FFFFFFFu) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
            blocks = (block*)grow(blocks, &cap_blocks, nblocks + 1, sizeof(*blocks), &cv->oom);
            if (cv->oom) break;
            bl = &blocks[nblocks++];
            bl->wave = h->wave;
            bl->channel = h->channel;
            bl->start = (uint32_t)(smpl.n / 2);
            for (f = 0; f < w->frames; f++) ob_u16(&smpl, (uint16_t)wave_sample(w, f, h->channel));
            ob_put(&smpl, NULL, SF2_PAD * 2);
        }
        snprintf(name, sizeof(name), "Wave %u", (unsigned)h->wave);
        ob_name(&shdr, name);
        ob_u32(&shdr, bl->start);
        ob_u32(&shdr, bl->start + w->frames);
        if (h->loop_type >= 0) {
            ob_u32(&shdr, bl->start + h->loop_start);
            ob_u32(&shdr, bl->start + h->loop_start + h->loop_len);
        } else {
            ob_u32(&shdr, bl->start);
            ob_u32(&shdr, bl->start + (w->frames > 1 ? w->frames - 1 : w->frames));
        }
        ob_u32(&shdr, w->rate);
        ob_u8(&shdr, (unsigned)h->unity);
        ob_u8(&shdr, 0);                 /* pitch correction: in the zone's fine tune */
        ob_u16(&shdr, 0);
        ob_u16(&shdr, 1);                /* mono */
    }
    ob_name(&shdr, "EOS");
    ob_put(&shdr, NULL, 26);

    /* One SF2 instrument (of the preset's zones) per preset. */
    for (i = 0; i < cv->npresets; i++) {
        const preset* p = &cv->presets[i];
        ob_name(&phdr, p->name);
        ob_u16(&phdr, p->program);
        ob_u16(&phdr, p->bank);
        ob_u16(&phdr, i);                /* one preset zone each */
        ob_u32(&phdr, 0); ob_u32(&phdr, 0); ob_u32(&phdr, 0);
        ob_u16(&pbag, i); ob_u16(&pbag, 0);
        ob_u16(&pgen, G_INSTRUMENT); ob_u16(&pgen, i);
        ob_name(&inst, p->name);
        if (nbag > 0xFFFF) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
        ob_u16(&inst, nbag);
        for (j = 0; j < p->zones; j++) {
            const zone* z = &cv->zones[p->first_zone + j];
            uint32_t k;
            if (ngen > 0xFFFF || nmod > 0xFFFF || z->sample > 0xFFFF) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
            ob_u16(&ibag, ngen); ob_u16(&ibag, nmod);
            ngen += write_gens(&igen, &z->g, z->sample);
            for (k = 0; k < z->g.nmods; k++) {
                const sf2_mod* x = &z->g.mods[k];
                ob_u16(&imod, x->src); ob_u16(&imod, x->dst); ob_u16(&imod, (unsigned)(uint16_t)x->amount);
                ob_u16(&imod, x->amt_src); ob_u16(&imod, x->trans);
            }
            nmod += z->g.nmods;
            nbag++;
        }
    }
    if (nbag > 0xFFFF || ngen > 0xFFFF || nmod > 0xFFFF || cv->npresets > 0xFFFF) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
    ob_name(&phdr, "EOP"); ob_u16(&phdr, 0); ob_u16(&phdr, 0); ob_u16(&phdr, cv->npresets); ob_u32(&phdr, 0); ob_u32(&phdr, 0); ob_u32(&phdr, 0);
    ob_u16(&pbag, cv->npresets); ob_u16(&pbag, 0);
    ob_u16(&pgen, 0); ob_u16(&pgen, 0);
    ob_name(&inst, "EOI"); ob_u16(&inst, nbag);
    ob_u16(&ibag, ngen); ob_u16(&ibag, nmod);
    ob_put(&imod, NULL, 10);             /* the terminal record */
    ob_u16(&igen, 0); ob_u16(&igen, 0);

    write_chunk_hdr(&o, "RIFF", 0);
    riff_at = 4;
    ob_put(&o, "sfbk", 4);
    {
        size_t nl = strlen(bank_name) + 1, info = 4 + 12 + 16 + 8 + nl + (nl & 1);
        write_chunk_hdr(&o, "LIST", (uint32_t)info);
        ob_put(&o, "INFO", 4);
        write_chunk_hdr(&o, "ifil", 4); ob_u16(&o, 2); ob_u16(&o, 1);
        write_chunk_hdr(&o, "isng", 8); ob_put(&o, "EMU8000\0", 8);
        write_chunk_hdr(&o, "INAM", (uint32_t)(nl + (nl & 1)));
        ob_put(&o, bank_name, nl);
        if (nl & 1) ob_u8(&o, 0);
    }
    write_chunk_hdr(&o, "LIST", (uint32_t)(4 + 8 + smpl.n));
    ob_put(&o, "sdta", 4);
    write_chunk_hdr(&o, "smpl", (uint32_t)smpl.n);
    ob_put(&o, smpl.p, smpl.n);
    pdta_at = o.n;
    write_chunk_hdr(&o, "LIST", 0);
    ob_put(&o, "pdta", 4);
    write_chunk_hdr(&o, "phdr", (uint32_t)phdr.n); ob_put(&o, phdr.p, phdr.n);
    write_chunk_hdr(&o, "pbag", (uint32_t)pbag.n); ob_put(&o, pbag.p, pbag.n);
    write_chunk_hdr(&o, "pmod", 10); ob_put(&o, NULL, 10);
    write_chunk_hdr(&o, "pgen", (uint32_t)pgen.n); ob_put(&o, pgen.p, pgen.n);
    write_chunk_hdr(&o, "inst", (uint32_t)inst.n); ob_put(&o, inst.p, inst.n);
    write_chunk_hdr(&o, "ibag", (uint32_t)ibag.n); ob_put(&o, ibag.p, ibag.n);
    write_chunk_hdr(&o, "imod", (uint32_t)imod.n); ob_put(&o, imod.p, imod.n);
    write_chunk_hdr(&o, "igen", (uint32_t)igen.n); ob_put(&o, igen.p, igen.n);
    write_chunk_hdr(&o, "shdr", (uint32_t)shdr.n); ob_put(&o, shdr.p, shdr.n);
    if (o.oom || smpl.oom || phdr.oom || pbag.oom || pgen.oom || inst.oom || ibag.oom || igen.oom || shdr.oom || imod.oom || cv->oom) goto done;
    ob_set32(&o, pdta_at + 4, (uint32_t)(o.n - pdta_at - 8));
    ob_set32(&o, riff_at, (uint32_t)(o.n - 8));
    *out = o.p;
    *out_n = o.n;
    o.p = NULL;
    r = FA_OK;
done:
    free(o.p); free(smpl.p); free(phdr.p); free(pbag.p); free(pgen.p); free(inst.p); free(ibag.p); free(igen.p); free(shdr.p);
    free(imod.p);
    free(blocks);
    return r;
}

int fa_dls_probe(const uint8_t* d, size_t n)
{
    return n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "DLS ", 4) == 0;
}

/* The wave at a pool offset, or UINT32_MAX. */
static uint32_t find_wave(const conv* cv, uint32_t offset)
{
    uint32_t lo = 0, hi = cv->nwaves;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cv->waves[mid].offset < offset) lo = mid + 1;
        else hi = mid;
    }
    if (lo < cv->nwaves && cv->waves[lo].offset == offset && cv->waves[lo].data) return lo;
    return UINT32_MAX;
}

fa_result fa_dls_to_sf2(const uint8_t* d, size_t n, uint8_t** out, size_t* out_n)
{
    const uint8_t *p, *end, *lins = NULL, *wvpl = NULL, *ptbl = NULL;
    uint32_t lins_n = 0, wvpl_n = 0, ptbl_n = 0, cues_at = 8, i;
    char name[256] = "";
    conv cv;
    chunk c;
    fa_result r;
    *out = NULL;
    *out_n = 0;
    if (!fa_dls_probe(d, n)) return FA_ERROR_UNSUPPORTED_FORMAT;
    memset(&cv, 0, sizeof(cv));
    end = d + n;
    {
        uint32_t riff = fa_le32(d + 4);
        if (riff >= 4 && riff - 4 < n - 12) end = d + 12 + (riff - 4);
    }
    p = d + 12;
    while (next_chunk(&p, end, &c)) {
        if (is_list(&c, "lins")) { lins = c.data; lins_n = c.size; }
        else if (is_list(&c, "wvpl")) { wvpl = c.data; wvpl_n = c.size; }
        else if (is_id(&c, "ptbl")) { ptbl = c.data; ptbl_n = c.size; }
        else if (is_list(&c, "INFO")) info_name(c.data, c.size, name, sizeof(name));
    }
    if (!lins || !wvpl) return FA_ERROR_UNSUPPORTED_FORMAT;

    /* The wave pool, with each wave's offset from the start of the pool's data. */
    {
        uint32_t cap = 0;
        const uint8_t* q = wvpl;
        const uint8_t* qe = wvpl + wvpl_n;
        for (;;) {
            const uint8_t* at = q;
            dls_wave w;
            if (!next_chunk(&q, qe, &c)) break;
            if (!is_list(&c, "wave")) continue;
            if (cv.nwaves >= SF2_MAX_RECORDS) break;
            memset(&w, 0, sizeof(w));
            w.offset = (uint32_t)(at - wvpl);
            if (!parse_wave(c.data, c.size, &w)) w.data = NULL;
            cv.waves = (dls_wave*)grow(cv.waves, &cap, cv.nwaves + 1, sizeof(*cv.waves), &cv.oom);
            if (cv.oom) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
            cv.waves[cv.nwaves++] = w;
        }
    }
    /* The pool table: cue -> wave. Writers differ on whether offsets count the
     * 'wvpl' type in front, so both are accepted. Without a table, cues are the
     * waves in order. */
    if (ptbl && ptbl_n >= 8) {
        uint32_t count = fa_le32(ptbl + 4);
        cues_at = fa_le32(ptbl);
        if (cues_at < 8 || cues_at > ptbl_n) cues_at = 8;
        if (count > (ptbl_n - cues_at) / 4) count = (ptbl_n - cues_at) / 4;
        if (count > SF2_MAX_RECORDS) count = SF2_MAX_RECORDS;
        cv.ncues = count;
    } else {
        cv.ncues = cv.nwaves;
    }
    cv.cues = (uint32_t*)malloc(((size_t)cv.ncues + 1) * sizeof(uint32_t));
    if (!cv.cues) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    /* The waves were read in pool order, so their offsets are ascending. */
    for (i = 0; i < cv.ncues; i++) {
        uint32_t want;
        cv.cues[i] = UINT32_MAX;
        if (!(ptbl && ptbl_n >= 8)) { if (cv.waves[i].data) cv.cues[i] = i; continue; }
        want = fa_le32(ptbl + cues_at + (size_t)i * 4);
        cv.cues[i] = find_wave(&cv, want);
        if (cv.cues[i] == UINT32_MAX && want >= 4) cv.cues[i] = find_wave(&cv, want - 4);
    }

    cv.presets = (preset*)malloc(DLS_MAX_INSTRUMENTS * sizeof(preset));
    cv.hdr_hash = (uint32_t*)calloc(HDR_HASH_SIZE, sizeof(uint32_t));
    if (!cv.presets || !cv.hdr_hash) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    p = lins;
    end = lins + lins_n;
    while (next_chunk(&p, end, &c) && !cv.oom)
        if (is_list(&c, "ins ")) parse_instrument(&cv, c.data, c.size);
    if (cv.oom) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    if (cv.npresets == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto done; }
    r = write_sf2(&cv, name[0] ? name : "DLS bank", out, out_n);
done:
    free(cv.waves);
    free(cv.cues);
    free(cv.hdrs);
    free(cv.zones);
    free(cv.presets);
    free(cv.hdr_hash);
    return r;
}
