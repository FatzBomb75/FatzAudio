/* The time stretcher and pitch shifter: a thin C wrapper around Signalsmith Stretch
 * (third_party/signalsmith, MIT), a phase-vocoder with phase locking and transient
 * handling. This is the library's only C++ file; nothing C++ crosses this API.
 *
 * Buffers are interleaved float frames. Output frames line up with the input: after
 * fa_stretch_start, the output of every fa_stretch_process call is the audio of the
 * input it was given, at the time ratio n_in / n_out.
 *
 * Only its time stretching is used, with the frequencies left where they are: its
 * own spectral transposition lands a few cents off (up to ~6.5 cents at -12
 * semitones on a test tone), so pitch is changed by stretching and then resampling
 * (resample.c), which is exact. */
#include "tempo.h"

#include <new>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "signalsmith-stretch/signalsmith-stretch.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

typedef signalsmith::stretch::SignalsmithStretch<float, std::mt19937> Stretcher;

/* Channel c of an interleaved buffer, as Signalsmith's buffer[c][i] wants it. */
struct Interleaved {
    float* data;
    int channels;
    struct Channel {
        float* data;
        int channels;
        float& operator[](int i) const { return data[(size_t)i * (size_t)channels]; }
    };
    Channel operator[](int c) const { return Channel{ data + c, channels }; }
};

struct ConstInterleaved {
    const float* data;
    int channels;
    struct Channel {
        const float* data;
        int channels;
        float operator[](int i) const { return data[(size_t)i * (size_t)channels]; }
    };
    Channel operator[](int c) const { return Channel{ data + c, channels }; }
};

} // namespace

struct fa_stretch {
    Stretcher st;
    int channels;
    fa_stretch() : st(0x46415a5aL) {} /* fixed seed: the same input always renders the same */
};

extern "C" fa_stretch* fa_stretch_create(uint32_t channels, uint32_t rate, int fast)
{
    fa_stretch* s = nullptr;
    if (channels == 0 || rate == 0) return nullptr;
    try {
        s = new fa_stretch();
        s->channels = (int)channels;
        if (fast) s->st.presetCheaper((int)channels, (float)rate, false);
        else s->st.presetDefault((int)channels, (float)rate, false);
    } catch (...) {
        delete s;
        return nullptr;
    }
    return s;
}

extern "C" void fa_stretch_destroy(fa_stretch* s)
{
    delete s;
}

extern "C" void fa_stretch_set_formants(fa_stretch* s, double factor)
{
    s->st.setTransposeFactor(1.0f);
    s->st.setFormantFactor((float)factor, false);
}

extern "C" int fa_stretch_start_length(const fa_stretch* s, double ratio)
{
    return s->st.outputSeekLength((float)ratio);
}

extern "C" int fa_stretch_input_latency(const fa_stretch* s)
{
    return s->st.inputLatency();
}

extern "C" int fa_stretch_output_latency(const fa_stretch* s)
{
    return s->st.outputLatency();
}

extern "C" int fa_stretch_start(fa_stretch* s, const float* in, int frames)
{
    try {
        s->st.outputSeek(ConstInterleaved{ in, s->channels }, frames);
    } catch (...) {
        return 0;
    }
    return 1;
}

extern "C" int fa_stretch_process(fa_stretch* s, const float* in, int in_frames, float* out, int out_frames)
{
    try {
        s->st.process(ConstInterleaved{ in, s->channels }, in_frames, Interleaved{ out, s->channels }, out_frames);
    } catch (...) {
        return 0;
    }
    return 1;
}

extern "C" int fa_stretch_flush(fa_stretch* s, float* out, int frames, double ratio)
{
    try {
        s->st.flush(Interleaved{ out, s->channels }, frames, (float)ratio);
    } catch (...) {
        return 0;
    }
    return 1;
}
