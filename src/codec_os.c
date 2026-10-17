/* What the operating systems' AAC and ALAC decoders (codec_mf.c on Windows,
 * codec_at.c on macOS and iOS, codec_mediacodec.c on Android) need from the
 * stream, kept here in plain C so that every platform builds it and the tests
 * check it everywhere: the AudioSpecificConfig read, ADTS headers taken off
 * packets, the MPEG-4 ES descriptor AudioToolbox wants as its magic cookie, the
 * ALAC configuration out of the containers' 'alac' box, and the library's
 * channel order from Core Audio's channel labels. */
#include "fa_internal.h"
#include "demux.h"
#include <string.h>

static const uint32_t asc_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000, 7350 };

typedef struct bitr {
    const uint8_t* p;
    size_t n, bit;          /* n in bits */
    int bad;                /* read past the end */
} bitr;

static uint32_t get_bits(bitr* r, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        if (r->bit >= r->n) { r->bad = 1; return 0; }
        v = (v << 1) | ((r->p[r->bit >> 3] >> (7 - (r->bit & 7))) & 1u);
        r->bit++;
    }
    return v;
}

static size_t bits_left(const bitr* r)
{
    return r->bit < r->n ? r->n - r->bit : 0;
}

static int get_aot(bitr* r)
{
    int aot = (int)get_bits(r, 5);
    if (aot == 31) aot = 32 + (int)get_bits(r, 6);
    return aot;
}

static uint32_t get_rate(bitr* r)
{
    uint32_t i = get_bits(r, 4);
    if (i == 15) return get_bits(r, 24);
    return i < 13 ? asc_rates[i] : 0;
}

/* ISO/IEC 14496-3 1.6.2.1 AudioSpecificConfig, as far as the decoders' setup needs:
 * the object type, the rates, the channel configuration and whether SBR and PS are
 * signalled (explicitly, or by the backward compatible sync extension). */
int fa_aac_parse_asc(const uint8_t* p, size_t n, fa_aac_asc_info* out)
{
    bitr r;
    int aot;
    memset(out, 0, sizeof(*out));
    out->sbr = -1;
    out->ps = -1;
    if (!p || n < 2 || n > 4096) return 0;
    r.p = p;
    r.n = n * 8;
    r.bit = 0;
    r.bad = 0;
    aot = get_aot(&r);
    out->rate = get_rate(&r);
    out->channel_config = get_bits(&r, 4);
    if (aot == 5 || aot == 29) {
        out->sbr = 1;
        out->ps = aot == 29;
        out->ext_rate = get_rate(&r);
        aot = get_aot(&r);
        if (aot == 22) get_bits(&r, 4);   /* extensionChannelConfiguration */
    }
    out->object_type = aot;
    if (r.bad || out->rate == 0 || aot == 0) return 0;
    switch (aot) {
    case 1: case 2: case 3: case 4: case 6: case 7: case 17: case 19: case 20: case 21: case 22: case 23:
        /* GASpecificConfig */
        get_bits(&r, 1);                         /* frameLengthFlag */
        if (get_bits(&r, 1)) get_bits(&r, 14);   /* dependsOnCoreCoder: coreCoderDelay */
        {
            uint32_t ext = get_bits(&r, 1);
            /* A program config element: its size takes parsing it; what follows
             * (the sync extension) stays unknown. */
            if (out->channel_config == 0) return !r.bad;
            if (aot == 6 || aot == 20) get_bits(&r, 3);
            if (ext) {
                if (aot == 22) get_bits(&r, 16);
                if (aot == 17 || aot == 19 || aot == 20 || aot == 23) get_bits(&r, 3);
                get_bits(&r, 1);
            }
        }
        if (aot >= 17 && aot <= 27) get_bits(&r, 2);   /* epConfig */
        break;
    default:
        return !r.bad;   /* USAC, ELD and the rest: their own configs follow */
    }
    if (r.bad) return 0;
    if (out->sbr != 1 && bits_left(&r) >= 16 && get_bits(&r, 11) == 0x2b7) {
        int ext_aot = get_aot(&r);
        if (ext_aot == 5) {
            out->sbr = (int)get_bits(&r, 1);
            if (out->sbr) {
                out->ext_rate = get_rate(&r);
                if (bits_left(&r) >= 12 && get_bits(&r, 11) == 0x548) out->ps = (int)get_bits(&r, 1);
                else out->ps = 0;
            } else {
                out->ps = 0;
            }
        }
        if (r.bad) { out->sbr = -1; out->ps = -1; out->ext_rate = 0; }
    }
    return 1;
}

/* Channels of an AAC channelConfiguration (0: a program config element says). */
uint32_t fa_aac_config_channels(uint32_t config)
{
    static const uint8_t ch[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 2, 3, 4, 7, 8, 24, 8, 12 };
    return config < 16 ? ch[config] : 0;
}

/* The AudioSpecificConfig the OS decoders get: the stream's own (MP4, Matroska),
 * or, for ADTS frames (no extradata), LC from the stream's rate and channels. ADTS
 * cannot say HE-AAC (SBR and PS are only found in the audio), so `he_guess` asks
 * for SBR to be signalled at low rates (and PS for mono), for a decoder that does
 * not find it by itself; an HE decoder plays plain LC too. */
size_t fa_aac_os_asc(const fa_codec_params* p, int he_guess, uint8_t* out, size_t cap)
{
    fa_aac_asc_info info;
    uint8_t asc[FA_AAC_ASC_MAX];
    size_t n;
    if (p->extradata_size > 0) {
        if (!p->extradata || p->extradata_size > cap || !fa_aac_parse_asc((const uint8_t*)p->extradata, p->extradata_size, &info))
            return 0;
        memcpy(out, p->extradata, p->extradata_size);
        return p->extradata_size;
    }
    if (p->sample_rate == 0 || p->channels == 0) return 0;
    if (he_guess && p->sample_rate <= 24000 && p->channels <= 2) {
        n = fa_aac_build_asc(asc, 2, p->sample_rate, p->channels, p->sample_rate * 2);
        if (p->channels == 1) asc[0] = (uint8_t)((asc[0] & 0x07) | (29 << 3));   /* object type 5 -> 29 (PS) */
    } else {
        n = fa_aac_build_asc(asc, 2, p->sample_rate, p->channels, 0);
    }
    if (n == 0 || n > cap) return 0;
    memcpy(out, asc, n);
    return n;
}

/* An ADTS frame (ISO/IEC 13818-7 6.2) of exactly `*size` bytes loses its header:
 * 1. A packet that is not one is left as it is: 0. A frame of several raw data
 * blocks cannot be handed over as one access unit: -1. */
int fa_adts_strip(const uint8_t** data, size_t* size)
{
    const uint8_t* d = *data;
    size_t n = *size, len, hdr;
    if (n < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return 0;
    len = ((size_t)(d[3] & 3) << 11) | ((size_t)d[4] << 3) | (d[5] >> 5);
    hdr = (d[1] & 1) ? 7 : 9;
    if (len != n || len <= hdr || ((d[2] >> 2) & 15) >= 13) return 0;
    if ((d[6] & 3) != 0) return -1;
    *data = d + hdr;
    *size = n - hdr;
    return 1;
}

static size_t put_desc(uint8_t* out, size_t at, uint8_t tag, size_t len)
{
    out[at++] = tag;
    out[at++] = (uint8_t)(0x80 | ((len >> 21) & 0x7F));
    out[at++] = (uint8_t)(0x80 | ((len >> 14) & 0x7F));
    out[at++] = (uint8_t)(0x80 | ((len >> 7) & 0x7F));
    out[at++] = (uint8_t)(len & 0x7F);
    return at;
}

/* ISO/IEC 14496-1 ES_Descriptor around an AudioSpecificConfig: the contents of an
 * MP4 'esds' box after its version and flags, which is AudioToolbox's AAC cookie. */
size_t fa_aac_esds(const uint8_t* asc, size_t n, uint8_t* out, size_t cap)
{
    size_t dsi = 5 + n, dcd = 5 + 13 + dsi, sl = 5 + 1, es = 5 + 3 + dcd + sl, at = 0;
    if (!asc || n == 0 || n > 0xFFFF || es > cap) return 0;
    at = put_desc(out, at, 0x03, es - 5);
    out[at++] = 0; out[at++] = 0;   /* ES_ID */
    out[at++] = 0;                  /* no dependence, URL or OCR stream; priority 0 */
    at = put_desc(out, at, 0x04, dcd - 5);
    out[at++] = 0x40;               /* objectTypeIndication: MPEG-4 audio */
    out[at++] = 0x15;               /* streamType 5 (audio) << 2 | upStream 0 | reserved 1 */
    memset(out + at, 0, 11);        /* bufferSizeDB, maxBitrate, avgBitrate: unknown */
    at += 11;
    at = put_desc(out, at, 0x05, n);
    memcpy(out + at, asc, n);
    at += n;
    at = put_desc(out, at, 0x06, 1);
    out[at++] = 0x02;               /* SLConfigDescriptor: predefined for MP4 */
    return at;
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The 24-byte ALACSpecificConfig in the containers' extradata: the bare config,
 * the 'alac' box (MP4, and Matroska's A_ALAC as this library passes it), or a
 * Core Audio cookie with a 'frma' box in front. */
int fa_alac_parse_config(const uint8_t* p, size_t n, fa_alac_info* out)
{
    const uint8_t* c = NULL;
    memset(out, 0, sizeof(*out));
    if (!p) return 0;
    if (n == 24) c = p;
    else if (n >= 36 && memcmp(p + 4, "alac", 4) == 0) c = p + 12;
    else if (n >= 48 && memcmp(p + 4, "frma", 4) == 0 && memcmp(p + 16, "alac", 4) == 0) c = p + 24;
    if (!c) return 0;
    out->config = c;
    out->frame_length = be32(c);
    out->bit_depth = c[5];
    out->channels = c[9];
    out->rate = be32(c + 20);
    if (c[4] != 0 || out->frame_length == 0 || out->frame_length > 65536 || out->channels == 0 || out->channels > 8 ||
        out->rate == 0 || out->rate > 768000 ||
        (out->bit_depth != 16 && out->bit_depth != 20 && out->bit_depth != 24 && out->bit_depth != 32))
        return 0;
    return 1;
}

/* Core Audio channel labels (CoreAudioTypes.h, kAudioChannelLabel_*). */
enum {
    LBL_L = 1, LBL_R = 2, LBL_C = 3, LBL_LFE = 4, LBL_LS = 5, LBL_RS = 6, LBL_LC = 7, LBL_RC = 8,
    LBL_CS = 9, LBL_LSD = 10, LBL_RSD = 11, LBL_RLS = 33, LBL_RRS = 34, LBL_LW = 35, LBL_RW = 36,
    LBL_LFE2 = 37, LBL_MONO = 42
};

/* The library's order is WAVE's (FL FR FC LFE BL BR FLC FRC BC SL SR): rank of each
 * label in it. Core Audio's Ls/Rs are the surrounds: the back pair of 5.1 (where
 * WAVE and FFmpeg put AAC's surround channels) unless the layout has rear surrounds
 * or a back centre too, which makes them the side pair. */
static int label_rank(uint32_t label, int ls_is_side)
{
    switch (label) {
    case LBL_L: return 0;
    case LBL_R: return 1;
    case LBL_C: case LBL_MONO: return 2;
    case LBL_LFE: return 3;
    case LBL_RLS: return 4;
    case LBL_RRS: return 5;
    case LBL_LS: return ls_is_side ? 9 : 4;
    case LBL_RS: return ls_is_side ? 10 : 5;
    case LBL_LC: case LBL_LW: return 6;
    case LBL_RC: case LBL_RW: return 7;
    case LBL_CS: return 8;
    case LBL_LSD: return 9;
    case LBL_RSD: return 10;
    case LBL_LFE2: return 11;
    default: return 12;
    }
}

/* src_of_dst[i]: the decoder's channel that becomes the library's channel i.
 * Returns 1 if that is not the identity (only for 3 to 32 channels whose labels
 * are all distinct). */
int fa_os_channel_map(const uint32_t* labels, uint32_t n, uint32_t* src_of_dst)
{
    int rank[32], ls_is_side = 0, moved = 0;
    uint32_t i, j, k = 0;
    for (i = 0; i < n && i < 32; i++) src_of_dst[i] = i;
    if (n < 3 || n > 32) return 0;
    for (i = 0; i < n; i++) {
        for (j = 0; j < i; j++) if (labels[i] == labels[j]) return 0;
        if (labels[i] == LBL_RLS || labels[i] == LBL_RRS || labels[i] == LBL_CS) ls_is_side = 1;
    }
    for (i = 0; i < n; i++) rank[i] = label_rank(labels[i], ls_is_side);
    /* A stable pick by rank: unknown labels keep their order, last. */
    for (k = 0; k < n; k++) {
        int best = -1;
        for (i = 0; i < n; i++) {
            if (rank[i] < 0) continue;
            if (best < 0 || rank[i] < rank[best]) best = (int)i;
        }
        src_of_dst[k] = (uint32_t)best;
        rank[best] = -1;
        if ((uint32_t)best != k) moved = 1;
    }
    return moved;
}
