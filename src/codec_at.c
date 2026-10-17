/* AAC and ALAC through AudioToolbox (macOS and iOS): the system's decoders behind
 * an AudioConverter, one packet in, interleaved float out.
 *  - AAC: the AudioSpecificConfig goes in an MPEG-4 ES descriptor, the magic
 *    cookie Core Audio reads from an 'esds' box. kAudioFormatProperty_FormatList
 *    reads that cookie and names the richest format the system decodes it as
 *    (xHE-AAC, HE-AAC v2, HE-AAC or LC), with its rate after SBR. ADTS frames lose
 *    their header first; ADTS cannot say HE-AAC, so at 24 kHz and below SBR (and PS
 *    for mono) is signalled for it, as an HE decoder plays plain LC too.
 *  - ALAC: kAudioFormatAppleLossless with the 24-byte ALACSpecificConfig as cookie.
 * Core Audio's channel order is the stream's (AAC: centre first); the output
 * channel layout's labels map it to the library's (codec_os.c).
 * Nothing is linked beyond the AudioToolbox system framework. */
#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

/* The input callback's "no packet now" (any status that is not an error code the
 * converter uses itself). */
#define AT_NO_INPUT ((OSStatus)0x66614e49)   /* 'faNI' */

typedef struct at_state {
    AudioConverterRef conv;
    AudioStreamBasicDescription in, out;
    const uint8_t* pkt;                 /* the packet being fed, until taken */
    UInt32 pkt_size;
    int eof;                            /* the end: the converter drains */
    int drained;
    int adts;
    AudioStreamPacketDescription desc;
    uint32_t map[32];                   /* decoder channel for each library channel */
    int remap;
    float* buf;                         /* one call's output */
    UInt32 buf_frames;
} at_state;

static OSStatus feed(AudioConverterRef conv, UInt32* packets, AudioBufferList* data,
                     AudioStreamPacketDescription** desc, void* user)
{
    at_state* st = (at_state*)user;
    (void)conv;
    if (!st->pkt) {
        *packets = 0;
        data->mBuffers[0].mData = NULL;
        data->mBuffers[0].mDataByteSize = 0;
        /* At the end, no packets and no error: the converter gives what it holds. */
        return st->eof ? noErr : AT_NO_INPUT;
    }
    data->mNumberBuffers = 1;
    data->mBuffers[0].mData = (void*)st->pkt;
    data->mBuffers[0].mDataByteSize = st->pkt_size;
    data->mBuffers[0].mNumberChannels = st->in.mChannelsPerFrame;
    st->desc.mStartOffset = 0;
    st->desc.mVariableFramesInPacket = 0;
    st->desc.mDataByteSize = st->pkt_size;
    if (desc) *desc = &st->desc;
    *packets = 1;
    st->pkt = NULL;
    return noErr;
}

/* Everything the converter has for what it was given, into c->out. */
static fa_result pull(fa_codec* c, at_state* st)
{
    uint32_t ch = st->out.mChannelsPerFrame;
    int rounds = 0;
    for (;;) {
        AudioBufferList abl;
        UInt32 frames = st->buf_frames;
        OSStatus err;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = ch;
        abl.mBuffers[0].mDataByteSize = frames * ch * (UInt32)sizeof(float);
        abl.mBuffers[0].mData = st->buf;
        err = AudioConverterFillComplexBuffer(st->conv, feed, st, &frames, &abl, NULL);
        if (frames > 0) {
            float* dst;
            UInt32 f;
            uint32_t k;
            if (fa_codec_reserve(c, frames, ch) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
            dst = c->out + (size_t)c->out_frames * ch;
            if (st->remap) {
                for (f = 0; f < frames; f++)
                    for (k = 0; k < ch; k++) dst[(size_t)f * ch + k] = st->buf[(size_t)f * ch + st->map[k]];
            } else {
                memcpy(dst, st->buf, (size_t)frames * ch * sizeof(float));
            }
            c->out_frames += frames;
        }
        if (err == AT_NO_INPUT || frames == 0) return FA_OK;
        if (err != noErr) return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;   /* corrupt: skipped */
        if (++rounds > 64) return FA_OK;
    }
}

static fa_result at_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    at_state* st = (at_state*)c->impl;
    fa_result r;
    c->out_frames = 0;
    c->rate = (uint32_t)st->out.mSampleRate;
    c->channels = st->out.mChannelsPerFrame;
    if (!data && size == 0) {
        if (st->drained) return FA_OK;
        st->drained = 1;
        st->eof = 1;
        st->pkt = NULL;
        return pull(c, st);
    }
    if (size == 0 || size > 0x7FFFFFF0 || st->drained) return FA_OK;
    if (st->adts && fa_adts_strip(&data, &size) < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    st->pkt = data;
    st->pkt_size = (UInt32)size;
    r = pull(c, st);
    st->pkt = NULL;
    return r;
}

static void at_flush(fa_codec* c)
{
    at_state* st = (at_state*)c->impl;
    AudioConverterReset(st->conv);
    st->pkt = NULL;
    st->eof = 0;
    st->drained = 0;
}

static void at_free(at_state* st)
{
    if (!st) return;
    if (st->conv) AudioConverterDispose(st->conv);
    free(st->buf);
    free(st);
}

static void at_close(fa_codec* c)
{
    at_free((at_state*)c->impl);
    c->impl = NULL;
}

static const fa_codec_vtable at_vtable = { "at", at_decode, at_flush, at_close };

/* The format the system reads the cookie as, best first (kAudioFormatProperty_FormatList). */
static int format_from_cookie(UInt32 format_id, const void* cookie, UInt32 cookie_n, AudioStreamBasicDescription* out)
{
    AudioFormatInfo info;
    AudioFormatListItem items[16];
    UInt32 size = 0;
    memset(&info, 0, sizeof(info));
    info.mASBD.mFormatID = format_id;
    info.mMagicCookie = cookie;
    info.mMagicCookieSize = cookie_n;
    if (AudioFormatGetPropertyInfo(kAudioFormatProperty_FormatList, sizeof(info), &info, &size) != noErr ||
        size < sizeof(AudioFormatListItem))
        return 0;
    if (size > sizeof(items)) size = sizeof(items);
    if (AudioFormatGetProperty(kAudioFormatProperty_FormatList, sizeof(info), &info, &size, items) != noErr ||
        size < sizeof(AudioFormatListItem))
        return 0;
    *out = items[0].mASBD;
    return out->mSampleRate > 0 && out->mChannelsPerFrame > 0;
}

/* The decoder's channel labels, from its output layout. */
static void channel_map(at_state* st)
{
    AudioChannelLayout* layout = NULL;
    AudioChannelLayout* full = NULL;
    UInt32 size = 0, n = st->out.mChannelsPerFrame, i;
    Boolean writable = 0;
    uint32_t labels[32];
    st->remap = 0;
    if (n < 3 || n > 32) return;
    if (AudioConverterGetPropertyInfo(st->conv, kAudioConverterOutputChannelLayout, &size, &writable) != noErr ||
        size < sizeof(AudioChannelLayout) - sizeof(AudioChannelDescription))
        return;
    layout = (AudioChannelLayout*)calloc(1, size);
    if (!layout) return;
    if (AudioConverterGetProperty(st->conv, kAudioConverterOutputChannelLayout, &size, layout) != noErr) goto done;
    if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
        /* A layout tag or bitmap: Core Audio spells it out. */
        UInt32 fsize = 0;
        AudioFormatPropertyID prop = layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap
                                         ? kAudioFormatProperty_ChannelLayoutForBitmap
                                         : kAudioFormatProperty_ChannelLayoutForTag;
        const void* spec = prop == kAudioFormatProperty_ChannelLayoutForBitmap ? (const void*)&layout->mChannelBitmap
                                                                               : (const void*)&layout->mChannelLayoutTag;
        UInt32 spec_n = prop == kAudioFormatProperty_ChannelLayoutForBitmap ? (UInt32)sizeof(layout->mChannelBitmap)
                                                                           : (UInt32)sizeof(layout->mChannelLayoutTag);
        if (AudioFormatGetPropertyInfo(prop, spec_n, spec, &fsize) != noErr || fsize < sizeof(AudioChannelLayout)) goto done;
        full = (AudioChannelLayout*)calloc(1, fsize);
        if (!full || AudioFormatGetProperty(prop, spec_n, spec, &fsize, full) != noErr) goto done;
        free(layout);
        layout = full;
        full = NULL;
    }
    if (layout->mNumberChannelDescriptions != n) goto done;
    for (i = 0; i < n; i++) labels[i] = layout->mChannelDescriptions[i].mChannelLabel;
    st->remap = fa_os_channel_map(labels, n, st->map);
done:
    free(full);
    free(layout);
}

fa_result fa_at_codec_open(fa_codec* c, const fa_codec_params* p)
{
    uint8_t asc[256], cookie[300];
    const void* ck = NULL;
    UInt32 ck_n = 0, size;
    AudioStreamBasicDescription in;
    at_state* st;
    int aac = strcmp(p->codec, "aac") == 0;

    memset(&in, 0, sizeof(in));
    if (aac) {
        fa_aac_asc_info info;
        size_t n = fa_aac_os_asc(p, p->extradata_size == 0, asc, sizeof(asc));
        size_t m;
        if (n == 0 || !fa_aac_parse_asc(asc, n, &info)) return FA_ERROR_UNSUPPORTED_FORMAT;
        m = fa_aac_esds(asc, n, cookie, sizeof(cookie));
        if (m == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        ck = cookie;
        ck_n = (UInt32)m;
        if (!format_from_cookie(kAudioFormatMPEG4AAC, ck, ck_n, &in)) {
            /* No list: the format by object type, as the cookie says. */
            uint32_t ch = fa_aac_config_channels(info.channel_config);
            in.mFormatID = info.sbr == 1 ? (info.ps == 1 ? kAudioFormatMPEG4AAC_HE_V2 : kAudioFormatMPEG4AAC_HE)
                                         : kAudioFormatMPEG4AAC;
            in.mSampleRate = info.sbr == 1 && info.ext_rate ? info.ext_rate : info.rate;
            in.mChannelsPerFrame = info.ps == 1 ? 2 : ch ? ch : p->channels;
            in.mFramesPerPacket = info.sbr == 1 ? 2048 : 1024;
            if (info.object_type < 1 || info.object_type > 4) return FA_ERROR_UNSUPPORTED_FORMAT;
        }
    } else if (strcmp(p->codec, "alac") == 0) {
        fa_alac_info a;
        if (!fa_alac_parse_config((const uint8_t*)p->extradata, p->extradata_size, &a)) return FA_ERROR_UNSUPPORTED_FORMAT;
        ck = a.config;
        ck_n = 24;
        in.mFormatID = kAudioFormatAppleLossless;
        in.mSampleRate = a.rate;
        in.mChannelsPerFrame = a.channels;
        in.mFramesPerPacket = a.frame_length;
        in.mFormatFlags = a.bit_depth == 16 ? kAppleLosslessFormatFlag_16BitSourceData
                        : a.bit_depth == 20 ? kAppleLosslessFormatFlag_20BitSourceData
                        : a.bit_depth == 24 ? kAppleLosslessFormatFlag_24BitSourceData
                                            : kAppleLosslessFormatFlag_32BitSourceData;
    } else {
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (in.mChannelsPerFrame == 0 || in.mChannelsPerFrame > 32 || in.mSampleRate <= 0) return FA_ERROR_UNSUPPORTED_FORMAT;

    st = (at_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->in = in;
    st->adts = aac;
    memset(&st->out, 0, sizeof(st->out));
    st->out.mSampleRate = in.mSampleRate;
    st->out.mFormatID = kAudioFormatLinearPCM;
    st->out.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    st->out.mChannelsPerFrame = in.mChannelsPerFrame;
    st->out.mBitsPerChannel = 32;
    st->out.mFramesPerPacket = 1;
    st->out.mBytesPerFrame = 4 * in.mChannelsPerFrame;
    st->out.mBytesPerPacket = st->out.mBytesPerFrame;
    if (AudioConverterNew(&st->in, &st->out, &st->conv) != noErr) { st->conv = NULL; at_free(st); return FA_ERROR_UNSUPPORTED_FORMAT; }
    if (AudioConverterSetProperty(st->conv, kAudioConverterDecompressionMagicCookie, ck_n, ck) != noErr) {
        at_free(st);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    /* What it really gives (the rate after SBR, the channels after PS). */
    size = sizeof(st->out);
    if (AudioConverterGetProperty(st->conv, kAudioConverterCurrentOutputStreamDescription, &size, &st->out) != noErr ||
        st->out.mFormatID != kAudioFormatLinearPCM || st->out.mBitsPerChannel != 32 ||
        st->out.mChannelsPerFrame == 0 || st->out.mChannelsPerFrame > 32 || st->out.mSampleRate <= 0) {
        at_free(st);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    channel_map(st);
    st->buf_frames = in.mFramesPerPacket > 0 && in.mFramesPerPacket <= 65536 ? in.mFramesPerPacket * 2 : 8192;
    if (st->buf_frames < 4096) st->buf_frames = 4096;
    st->buf = (float*)malloc((size_t)st->buf_frames * st->out.mChannelsPerFrame * sizeof(float));
    if (!st->buf) { at_free(st); return FA_ERROR_OUT_OF_MEMORY; }
    c->impl = st;
    c->vt = &at_vtable;
    c->rate = (uint32_t)st->out.mSampleRate;
    c->channels = st->out.mChannelsPerFrame;
    return FA_OK;
}

#endif /* __APPLE__ */
