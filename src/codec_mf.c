/* Media Foundation's decoders as packet codecs (Windows only): one packet in
 * (ProcessInput), whatever PCM is ready out (ProcessOutput), as float.
 *  - Windows Media Audio: WMA 1, 2, 9 Professional and 9 Lossless go to the WMA
 *    decoder (CLSID_CWMADecMediaObject), WMA Voice to the WMA Speech decoder
 *    (CLSID_CWMSPDecMediaObject); one ASF media object per packet.
 *  - AAC (LC, HE-AAC, HE-AAC v2): the system's AAC decoder, found by MFTEnumEx,
 *    raw access units (payload type 0) with the AudioSpecificConfig after an
 *    HEAACWAVEINFO's fields as MF_MT_USER_DATA. ADTS frames lose their header
 *    first (codec_os.c). SBR doubles the rate: the output type says the rate.
 *  - ALAC (Windows 10 and later): the system's ALAC decoder, the 'alac' box
 *    holding the ALACSpecificConfig as MF_MT_USER_DATA.
 * Their output is in WAVE channel order, which is the library's.
 *
 * mfplat.dll is loaded at run time, never imported: Windows N and KN editions
 * without the Media Feature Pack, and Server Core, have no Media Foundation, and
 * fatzaudio.dll must still load there. When anything is missing (the DLL, the
 * decoder, the format), opening returns FA_ERROR_UNSUPPORTED_FORMAT and codec.c
 * tries the app's FFmpeg instead.
 *
 * COM: the decoder is created in the multithreaded apartment of the opening
 * thread (or in its single-threaded apartment if it already has one; both
 * decoders are free-threaded in-process objects, used directly from the decode
 * thread either way). */
#if defined(_WIN32)

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

/* The decoders' class IDs (wmcodecdsp.h), written out so no GUID library is needed. */
static const GUID fa_clsid_wma_dec = { 0x2eeb4adf, 0x4578, 0x4d10, { 0xbc, 0xa7, 0xbb, 0x95, 0x5f, 0x56, 0x32, 0x0a } };
static const GUID fa_clsid_wmsp_dec = { 0x874131cb, 0x4ecc, 0x443b, { 0x89, 0x48, 0x74, 0x6b, 0x89, 0x59, 0x5d, 0x20 } };
/* And what older SDKs lack (mfapi.h): the decoder category, the AAC and ALAC
 * subtypes (WAVE_FORMAT_MPEG_HEAAC, WAVE_FORMAT_ALAC) and the AAC attributes. */
static const GUID fa_mft_audio_decoder = { 0x9ea73fb4, 0xef7a, 0x4559, { 0x8d, 0x5d, 0x71, 0x9d, 0x8f, 0x04, 0x26, 0xc7 } };
static const GUID fa_mf_aac = { 0x00001610, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID fa_mf_alac = { 0x00006c61, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID fa_mt_aac_payload_type = { 0xbfbabe79, 0x7434, 0x4d1c, { 0x94, 0xf0, 0x72, 0xa3, 0xb9, 0xe1, 0x71, 0x88 } };
static const GUID fa_mt_aac_profile_level = { 0x7632f0e6, 0x9538, 0x4d61, { 0xac, 0xda, 0xea, 0x29, 0xc8, 0xc1, 0x44, 0x56 } };
static const GUID fa_mt_user_data = { 0xb6bc765f, 0x4c3b, 0x40a4, { 0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d } };

/* ---- mfplat.dll at run time -------------------------------------------------------- */

typedef HRESULT (WINAPI* pfn_startup)(ULONG, DWORD);
typedef HRESULT (WINAPI* pfn_shutdown)(void);
typedef HRESULT (WINAPI* pfn_create_media_type)(IMFMediaType**);
typedef HRESULT (WINAPI* pfn_init_from_wfx)(IMFMediaType*, const WAVEFORMATEX*, UINT32);
typedef HRESULT (WINAPI* pfn_create_sample)(IMFSample**);
typedef HRESULT (WINAPI* pfn_create_buffer)(DWORD, IMFMediaBuffer**);
typedef HRESULT (WINAPI* pfn_enum_ex)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*,
                                      IMFActivate***, UINT32*);

static struct {
    HMODULE dll;
    pfn_startup startup;
    pfn_shutdown shutdown;
    pfn_create_media_type create_media_type;
    pfn_init_from_wfx init_from_wfx;
    pfn_create_sample create_sample;
    pfn_create_buffer create_buffer;
    pfn_enum_ex enum_ex;        /* Windows 7 and later */
    int ok;
} mf;

static INIT_ONCE mf_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK mf_load(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    wchar_t path[MAX_PATH + 16];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    (void)once;
    (void)param;
    (void)ctx;
    /* Only the system's copy: never a mfplat.dll planted next to the app. */
    if (n == 0 || n >= MAX_PATH) return TRUE;
    memcpy(path + n, L"\\mfplat.dll", sizeof(L"\\mfplat.dll"));
    mf.dll = LoadLibraryW(path);
    if (!mf.dll) return TRUE;
    mf.startup = (pfn_startup)(void (*)(void))GetProcAddress(mf.dll, "MFStartup");
    mf.shutdown = (pfn_shutdown)(void (*)(void))GetProcAddress(mf.dll, "MFShutdown");
    mf.create_media_type = (pfn_create_media_type)(void (*)(void))GetProcAddress(mf.dll, "MFCreateMediaType");
    mf.init_from_wfx = (pfn_init_from_wfx)(void (*)(void))GetProcAddress(mf.dll, "MFInitMediaTypeFromWaveFormatEx");
    mf.create_sample = (pfn_create_sample)(void (*)(void))GetProcAddress(mf.dll, "MFCreateSample");
    mf.create_buffer = (pfn_create_buffer)(void (*)(void))GetProcAddress(mf.dll, "MFCreateMemoryBuffer");
    mf.enum_ex = (pfn_enum_ex)(void (*)(void))GetProcAddress(mf.dll, "MFTEnumEx");
    mf.ok = mf.startup && mf.shutdown && mf.create_media_type && mf.init_from_wfx && mf.create_sample && mf.create_buffer;
    return TRUE;   /* the DLL stays loaded for the life of the process */
}

/* ---- the codec ---------------------------------------------------------------------- */

typedef struct mf_state {
    IMFTransform* mft;
    IMFSample* out_sample;      /* ours, when the decoder does not provide samples */
    IMFMediaBuffer* out_buf;
    DWORD out_size;
    int provides;               /* the decoder allocates its output samples */
    int kind;                   /* FA_SAMPLE_* of the output */
    uint32_t bytes, rate, channels;
    int com;                    /* CoInitializeEx succeeded on com_thread */
    DWORD com_thread;
    int started;                /* MFStartup succeeded */
    int drained;
    int adts;                   /* AAC: ADTS headers come off the packets */
    LONGLONG time;              /* input sample times, 100 ns */
    int64_t frames_out;
} mf_state;

static void release_output_sample(mf_state* st)
{
    if (st->out_buf) IMFMediaBuffer_Release(st->out_buf);
    if (st->out_sample) IMFSample_Release(st->out_sample);
    st->out_buf = NULL;
    st->out_sample = NULL;
}

/* Picks the output type: float, else integer PCM. */
static HRESULT set_output_type(mf_state* st)
{
    IMFMediaType* best = NULL;
    IMFMediaType* pcm = NULL;
    MFT_OUTPUT_STREAM_INFO info;
    UINT32 bits = 0, ch = 0, rate = 0;
    HRESULT hr;
    DWORD i;
    for (i = 0; i < 64; i++) {
        IMFMediaType* t = NULL;
        GUID sub;
        hr = IMFTransform_GetOutputAvailableType(st->mft, 0, i, &t);
        if (FAILED(hr)) break;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub))) {
            if (!best && IsEqualGUID(&sub, &MFAudioFormat_Float)) { best = t; continue; }
            if (!pcm && IsEqualGUID(&sub, &MFAudioFormat_PCM)) { pcm = t; continue; }
        }
        IMFMediaType_Release(t);
    }
    if (!best) { best = pcm; pcm = NULL; }
    if (pcm) IMFMediaType_Release(pcm);
    if (!best) return E_FAIL;
    hr = IMFTransform_SetOutputType(st->mft, 0, best, 0);
    if (SUCCEEDED(hr)) {
        GUID sub;
        IMFMediaType_GetUINT32(best, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        IMFMediaType_GetUINT32(best, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        IMFMediaType_GetUINT32(best, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        if (FAILED(IMFMediaType_GetGUID(best, &MF_MT_SUBTYPE, &sub))) hr = E_FAIL;
        else if (IsEqualGUID(&sub, &MFAudioFormat_Float) && bits == 32) st->kind = FA_SAMPLE_F32;
        else if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 16) st->kind = FA_SAMPLE_S16;
        else if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 24) st->kind = FA_SAMPLE_S24;
        else if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 32) st->kind = FA_SAMPLE_S32;
        else hr = E_FAIL;
    }
    IMFMediaType_Release(best);
    if (FAILED(hr)) return hr;
    if (ch == 0 || ch > 32 || rate == 0) return E_FAIL;
    st->bytes = bits / 8;
    st->channels = ch;
    st->rate = rate;
    /* Our own output sample, big enough for what the decoder says it writes. */
    release_output_sample(st);
    memset(&info, 0, sizeof(info));
    hr = IMFTransform_GetOutputStreamInfo(st->mft, 0, &info);
    if (FAILED(hr)) return hr;
    st->provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    st->out_size = info.cbSize;
    if (st->out_size < st->rate / 4 * st->channels * st->bytes) st->out_size = st->rate / 4 * st->channels * st->bytes;
    if (!st->provides) {
        hr = mf.create_sample(&st->out_sample);
        if (SUCCEEDED(hr)) hr = mf.create_buffer(st->out_size, &st->out_buf);
        if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(st->out_sample, st->out_buf);
        if (FAILED(hr)) release_output_sample(st);
    }
    return hr;
}

/* Moves every block of PCM the decoder has ready into c->out. */
static fa_result pull(fa_codec* c, mf_state* st)
{
    int changes = 0, empty = 0;
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob;
        IMFMediaBuffer* buf = NULL;
        BYTE* data = NULL;
        DWORD status = 0, len = 0;
        fa_frames frames;
        HRESULT hr;
        memset(&ob, 0, sizeof(ob));
        if (!st->provides) {
            IMFMediaBuffer_SetCurrentLength(st->out_buf, 0);
            ob.pSample = st->out_sample;
        }
        hr = IMFTransform_ProcessOutput(st->mft, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return FA_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (st->provides && ob.pSample) IMFSample_Release(ob.pSample);
            if (++changes > 4 || FAILED(set_output_type(st))) return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
            /* Audio already out is in the old format: hand it out first. */
            if (c->out_frames > 0) return FA_OK;
            continue;
        }
        if (FAILED(hr) || !ob.pSample) {
            if (st->provides && ob.pSample) IMFSample_Release(ob.pSample);
            return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
        }
        hr = IMFSample_ConvertToContiguousBuffer(ob.pSample, &buf);
        if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buf, &data, NULL, &len);
        if (SUCCEEDED(hr)) {
            fa_frames have = (fa_frames)(len / (st->bytes * st->channels));
            frames = have;
            if (frames > 0 && c->out_frames > 0 && (c->channels != st->channels || c->rate != st->rate)) frames = 0;
            if (frames > 0 && fa_codec_reserve(c, frames, st->channels) == FA_OK) {
                const void* plane[1];
                plane[0] = data;
                c->channels = st->channels;
                c->rate = st->rate;
                fa_convert_to_f32(plane, st->kind, 0, 0, c->channels, frames, c->out + (size_t)c->out_frames * c->channels);
                c->out_frames += frames;
                st->frames_out += frames;
            }
            IMFMediaBuffer_Unlock(buf);
            empty = have > 0 ? 0 : empty + 1;
        }
        if (buf) IMFMediaBuffer_Release(buf);
        if (st->provides) IMFSample_Release(ob.pSample);
        if (FAILED(hr)) return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
        if (empty > 16) return FA_OK;            /* a decoder that keeps giving nothing */
    }
}

static fa_result mf_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    mf_state* st = (mf_state*)c->impl;
    IMFSample* sample = NULL;
    IMFMediaBuffer* buf = NULL;
    BYTE* p = NULL;
    HRESULT hr;
    c->out_frames = 0;
    if (!data && size == 0) {
        /* The end: the decoder gives back what it holds, once. */
        if (st->drained) return FA_OK;
        st->drained = 1;
        if (FAILED(IMFTransform_ProcessMessage(st->mft, MFT_MESSAGE_COMMAND_DRAIN, 0))) return FA_OK;
        return pull(c, st);
    }
    if (size == 0 || size > 0x7FFFFFF0 || st->drained) return FA_OK;
    /* Several raw data blocks in one ADTS frame: not one access unit, skipped. */
    if (st->adts && fa_adts_strip(&data, &size) < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    hr = mf.create_buffer((DWORD)size, &buf);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
    if (SUCCEEDED(hr)) {
        memcpy(p, data, size);
        IMFMediaBuffer_Unlock(buf);
        hr = IMFMediaBuffer_SetCurrentLength(buf, (DWORD)size);
    }
    if (SUCCEEDED(hr)) hr = mf.create_sample(&sample);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(sample, buf);
    if (SUCCEEDED(hr)) {
        IMFSample_SetSampleTime(sample, st->time);
        hr = IMFTransform_ProcessInput(st->mft, 0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            /* Output waiting first, then the packet again. */
            fa_result r = pull(c, st);
            if (r != FA_OK && r != FA_ERROR_UNSUPPORTED_FORMAT) hr = E_FAIL;
            else hr = IMFTransform_ProcessInput(st->mft, 0, sample, 0);
        }
    }
    if (sample) IMFSample_Release(sample);
    if (buf) IMFMediaBuffer_Release(buf);
    if (hr == E_OUTOFMEMORY) return FA_ERROR_OUT_OF_MEMORY;
    if (FAILED(hr)) return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;  /* corrupt: skipped */
    {
        fa_result r = pull(c, st);
        if (st->rate) st->time = (LONGLONG)(st->frames_out * 10000000 / st->rate);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
        return c->out_frames > 0 ? FA_OK : r;
    }
}

static void mf_flush(fa_codec* c)
{
    mf_state* st = (mf_state*)c->impl;
    IMFTransform_ProcessMessage(st->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(st->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    st->drained = 0;
}

static void mf_free(mf_state* st)
{
    if (!st) return;
    release_output_sample(st);
    if (st->mft) {
        IMFTransform_ProcessMessage(st->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(st->mft);
    }
    if (st->started) mf.shutdown();
    /* COM is left as it is on another thread than the one that started it. */
    if (st->com && st->com_thread == GetCurrentThreadId()) CoUninitialize();
    free(st);
}

static void mf_close(fa_codec* c)
{
    mf_free((mf_state*)c->impl);
    c->impl = NULL;
}

static const fa_codec_vtable mf_vtable = { "mf", mf_decode, mf_flush, mf_close };

/* A new state: COM on this thread, Media Foundation started. */
static mf_state* mf_new(void)
{
    mf_state* st = (mf_state*)calloc(1, sizeof(*st));
    HRESULT hr;
    if (!st) return NULL;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        st->com = 1;
        st->com_thread = GetCurrentThreadId();
    }
    if (SUCCEEDED(mf.startup(MF_VERSION, MFSTARTUP_LITE))) st->started = 1;
    return st;
}

/* Sets the input type, picks the output and starts streaming. */
static HRESULT mf_start(mf_state* st, IMFMediaType* in)
{
    HRESULT hr = IMFTransform_SetInputType(st->mft, 0, in, 0);
    if (SUCCEEDED(hr)) hr = set_output_type(st);
    if (SUCCEEDED(hr)) {
        IMFTransform_ProcessMessage(st->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        IMFTransform_ProcessMessage(st->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    return hr;
}

static fa_result mf_finish(fa_codec* c, mf_state* st, HRESULT hr)
{
    if (FAILED(hr)) {
        mf_free(st);
        return hr == E_OUTOFMEMORY ? FA_ERROR_OUT_OF_MEMORY : FA_ERROR_UNSUPPORTED_FORMAT;
    }
    c->impl = st;
    c->vt = &mf_vtable;
    c->trails = 1;
    c->rate = st->rate;
    c->channels = st->channels;
    return FA_OK;
}

/* Windows Media Audio: the decoder by class ID, the container's WAVEFORMATEX. */
static fa_result open_wma(fa_codec* c, const fa_codec_params* p, uint16_t tag, const GUID* clsid)
{
    mf_state* st;
    IMFMediaType* in = NULL;
    WAVEFORMATEX* wfx;
    size_t wfx_size;
    HRESULT hr;

    if (p->sample_rate == 0 || p->channels == 0 || p->channels > 32 || p->extradata_size > 4096 ||
        (p->extradata_size > 0 && !p->extradata))
        return FA_ERROR_UNSUPPORTED_FORMAT;
    st = mf_new();
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    hr = st->started ? S_OK : E_FAIL;
    if (SUCCEEDED(hr)) hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void**)&st->mft);

    /* The stream's WAVEFORMATEX as the container had it: the codec data follows it. */
    wfx_size = sizeof(WAVEFORMATEX) + p->extradata_size;
    wfx = (WAVEFORMATEX*)calloc(1, wfx_size);
    if (!wfx) { mf_free(st); return FA_ERROR_OUT_OF_MEMORY; }
    wfx->wFormatTag = tag;
    wfx->nChannels = (WORD)p->channels;
    wfx->nSamplesPerSec = p->sample_rate;
    wfx->nAvgBytesPerSec = p->bit_rate > 0 && p->bit_rate / 8 < 0xFFFFFFFF ? (DWORD)(p->bit_rate / 8) : 0;
    wfx->nBlockAlign = (WORD)(p->block_align < 0xFFFF ? p->block_align : 0);
    wfx->wBitsPerSample = (WORD)(p->bits_per_coded_sample ? p->bits_per_coded_sample : 16);
    wfx->cbSize = (WORD)p->extradata_size;
    if (p->extradata_size) memcpy(wfx + 1, p->extradata, p->extradata_size);

    if (SUCCEEDED(hr)) hr = mf.create_media_type(&in);
    if (SUCCEEDED(hr)) hr = mf.init_from_wfx(in, wfx, (UINT32)wfx_size);
    if (SUCCEEDED(hr)) hr = mf_start(st, in);
    if (in) IMFMediaType_Release(in);
    free(wfx);
    return mf_finish(c, st, hr);
}

/* One input type for the AAC or ALAC decoder. */
static HRESULT make_type(const GUID* subtype, uint32_t rate, uint32_t channels, uint32_t bits,
                         const uint8_t* user, size_t user_n, int aac, IMFMediaType** out)
{
    IMFMediaType* t = NULL;
    HRESULT hr = mf.create_media_type(&t);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
    if (SUCCEEDED(hr) && aac) hr = IMFMediaType_SetUINT32(t, &fa_mt_aac_payload_type, 0);   /* raw access units */
    if (SUCCEEDED(hr) && aac) hr = IMFMediaType_SetUINT32(t, &fa_mt_aac_profile_level, 0xFE);   /* not specified */
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetBlob(t, &fa_mt_user_data, user, (UINT32)user_n);
    if (FAILED(hr) && t) { IMFMediaType_Release(t); t = NULL; }
    *out = t;
    return hr;
}

/* The system's decoders for `subtype`, best first: the first that takes the stream. */
static fa_result open_enum(fa_codec* c, const GUID* subtype, const uint32_t* rates, const uint32_t* chans, int tries,
                           uint32_t bits, const uint8_t* const* user, const size_t* user_n, int users, int aac)
{
    MFT_REGISTER_TYPE_INFO want;
    IMFActivate** act = NULL;
    UINT32 n = 0, i;
    mf_state* st;
    HRESULT hr;
    int k, u, opened = 0;

    st = mf_new();
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    if (!st->started || !mf.enum_ex) { mf_free(st); return FA_ERROR_UNSUPPORTED_FORMAT; }
    want.guidMajorType = MFMediaType_Audio;
    want.guidSubtype = *subtype;
    hr = mf.enum_ex(fa_mft_audio_decoder, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &want, NULL, &act, &n);
    if (FAILED(hr)) n = 0;
    for (i = 0; i < n && !opened; i++) {
        if (FAILED(IMFActivate_ActivateObject(act[i], &IID_IMFTransform, (void**)&st->mft))) continue;
        /* The rate and channels the stream is declared with differ between
         * decoders for HE-AAC (output or core rate): each is tried. */
        for (u = 0; u < users && !opened; u++) {
            for (k = 0; k < tries && !opened; k++) {
                IMFMediaType* in = NULL;
                hr = make_type(subtype, rates[k], chans[k], bits, user[u], user_n[u], aac, &in);
                if (SUCCEEDED(hr)) hr = mf_start(st, in);
                if (in) IMFMediaType_Release(in);
                if (SUCCEEDED(hr)) opened = 1;
                else if (hr == E_OUTOFMEMORY) break;
            }
        }
        if (!opened) {
            release_output_sample(st);
            IMFTransform_Release(st->mft);
            st->mft = NULL;
            IMFActivate_ShutdownObject(act[i]);
        }
    }
    for (i = 0; i < n; i++) IMFActivate_Release(act[i]);
    CoTaskMemFree(act);
    if (!opened) {
        mf_free(st);
        return hr == E_OUTOFMEMORY ? FA_ERROR_OUT_OF_MEMORY : FA_ERROR_UNSUPPORTED_FORMAT;
    }
    st->adts = aac;
    return mf_finish(c, st, S_OK);
}

static fa_result open_aac(fa_codec* c, const fa_codec_params* p)
{
    /* HEAACWAVEINFO after its WAVEFORMATEX: payload type 0 (raw), profile and level
     * not specified (0xFE), struct type 0; then the AudioSpecificConfig. */
    uint8_t user[12 + 256];
    const uint8_t* users[1];
    size_t user_n[1], asc_n;
    uint32_t rates[3], chans[3];
    fa_aac_asc_info info;
    int tries = 0, k;

    asc_n = fa_aac_os_asc(p, 0, user + 12, sizeof(user) - 12);
    if (asc_n == 0 || !fa_aac_parse_asc(user + 12, asc_n, &info)) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* Main, LC, SSR and LTP cores, with or without SBR and PS: what the decoder
     * knows (xHE-AAC, ELD and the rest go to FFmpeg). */
    if (info.object_type < 1 || info.object_type > 4) return FA_ERROR_UNSUPPORTED_FORMAT;
    memset(user, 0, 12);
    user[2] = 0xFE;
    users[0] = user;
    user_n[0] = 12 + asc_n;
    {
        uint32_t ch = fa_aac_config_channels(info.channel_config);
        uint32_t cand_r[3], cand_c[3];
        if (ch == 0) ch = p->channels;
        if (ch == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        cand_r[0] = info.ext_rate ? info.ext_rate : info.rate; cand_c[0] = ch;
        cand_r[1] = info.rate; cand_c[1] = ch;
        cand_r[2] = p->sample_rate; cand_c[2] = p->channels;
        for (k = 0; k < 3; k++) {
            int dup = 0, j;
            if (cand_r[k] == 0 || cand_c[k] == 0 || cand_c[k] > 8) continue;
            for (j = 0; j < tries; j++) if (rates[j] == cand_r[k] && chans[j] == cand_c[k]) dup = 1;
            if (dup) continue;
            rates[tries] = cand_r[k];
            chans[tries] = cand_c[k];
            tries++;
        }
    }
    if (tries == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    return open_enum(c, &fa_mf_aac, rates, chans, tries, 16, users, user_n, 1, 1);
}

static fa_result open_alac(fa_codec* c, const fa_codec_params* p)
{
    fa_alac_info a;
    uint8_t atom[36];
    const uint8_t* users[2];
    size_t user_n[2];
    uint32_t rate, ch;
    if (!fa_alac_parse_config((const uint8_t*)p->extradata, p->extradata_size, &a)) return FA_ERROR_UNSUPPORTED_FORMAT;
    rate = a.rate;
    ch = a.channels;
    /* ALAC's own channel order (centre first) is not WAVE's, and what Media
     * Foundation gives is not documented: FFmpeg reorders. */
    if (ch > 2) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* The 'alac' box first (as Windows' own MP4 source passes it: the bare config
     * is taken at open but then decodes nothing), else the bare ALACSpecificConfig. */
    atom[0] = 0; atom[1] = 0; atom[2] = 0; atom[3] = 36;
    memcpy(atom + 4, "alac", 4);
    memset(atom + 8, 0, 4);
    memcpy(atom + 12, a.config, 24);
    users[0] = atom; user_n[0] = 36;
    users[1] = a.config; user_n[1] = 24;
    return open_enum(c, &fa_mf_alac, &rate, &ch, 1, a.bit_depth, users, user_n, 2, 0);
}

fa_result fa_mf_codec_open(fa_codec* c, const fa_codec_params* p)
{
    const GUID* clsid = &fa_clsid_wma_dec;
    uint16_t tag;
    int kind = 0;

    if (strcmp(p->codec, "wmav1") == 0) tag = 0x0160;
    else if (strcmp(p->codec, "wmav2") == 0) tag = 0x0161;
    else if (strcmp(p->codec, "wmapro") == 0) tag = 0x0162;
    else if (strcmp(p->codec, "wmalossless") == 0) tag = 0x0163;
    else if (strcmp(p->codec, "wmavoice") == 0) { tag = 0x000A; clsid = &fa_clsid_wmsp_dec; }
    else if (strcmp(p->codec, "aac") == 0) { tag = 0; kind = 1; }
    else if (strcmp(p->codec, "alac") == 0) { tag = 0; kind = 2; }
    else return FA_ERROR_UNSUPPORTED_FORMAT;
    if (p->extradata_size > 0 && !p->extradata) return FA_ERROR_UNSUPPORTED_FORMAT;
    InitOnceExecuteOnce(&mf_once, mf_load, NULL, NULL);
    if (!mf.ok) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (kind == 1) return open_aac(c, p);
    if (kind == 2) return open_alac(c, p);
    return open_wma(c, p, tag, clsid);
}

#endif /* _WIN32 */
