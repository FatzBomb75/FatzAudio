/* What the operating systems' AAC decoders (codec_mf.c on Windows,
 * codec_at.c on macOS and iOS, codec_mediacodec.c on Android) need from the
 * stream, kept here in plain C so that every platform builds it and the tests
 * check it everywhere: the AudioSpecificConfig handed over (read by aac.c's
 * parser), the MPEG-4 ES descriptor AudioToolbox wants as its magic cookie, and
 * the library's channel order from Core Audio's channel labels. */
#include "fa_internal.h"
#include "demux.h"
#include <string.h>

/* Channels of an AAC channelConfiguration (0: a program config element says). */
uint32_t fa_aac_config_channels(uint32_t config)
{
    static const uint8_t ch[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 2, 3, 4, 7, 8, 24, 8, 12 };
    return config < 16 ? ch[config] : 0;
}

/* The AudioSpecificConfig the OS decoders get: the stream's own (MP4, Matroska,
 * CAF). Only configurations the built-in decoder (aac.c) does not take reach them,
 * so there always is one: ADTS frames never come here. An MP4 config that leaves
 * SBR implicit (SBR and PS are then only found in the audio) cannot say HE-AAC, so
 * `he_guess` asks for SBR to be signalled at low rates (and PS for mono), for a
 * decoder that does not find it by itself; an HE decoder plays plain LC too. */
size_t fa_aac_os_asc(const fa_codec_params* p, int he_guess, uint8_t* out, size_t cap)
{
    fa_aac_asc_info info;
    uint8_t asc[FA_AAC_ASC_MAX];
    size_t n;
    if (p->extradata_size == 0 || !p->extradata || p->extradata_size > cap ||
        !fa_aac_parse_asc((const uint8_t*)p->extradata, p->extradata_size, &info))
        return 0;
    /* LC at a low rate with SBR not said either way (implicit signalling): signalled
     * for a decoder whose output rate is fixed at open. */
    if (!(he_guess && info.object_type == 2 && info.sbr == -1 && info.rate <= 24000 &&
          (info.channel_config == 1 || info.channel_config == 2))) {
        memcpy(out, p->extradata, p->extradata_size);
        return p->extradata_size;
    }
    n = fa_aac_build_asc(asc, 2, info.rate, info.channel_config, info.rate * 2);
    if (info.channel_config == 1) asc[0] = (uint8_t)((asc[0] & 0x07) | (29 << 3));   /* object type 5 -> 29 (PS) */
    if (n == 0 || n > cap) return 0;
    memcpy(out, asc, n);
    return n;
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
