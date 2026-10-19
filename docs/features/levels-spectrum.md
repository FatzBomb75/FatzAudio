# Levels and spectrum

FatzAudio measures what is being heard, for VU meters and visualizers: peak and RMS levels, and a magnitude spectrum (FFT). Both work on one stream or on a whole output device. They replace `BASS_ChannelGetLevel`/`GetLevelEx` and `BASS_ChannelGetData` with the `BASS_DATA_FFT...` flags.

| Call | Measures |
|---|---|
| `fa_stream_get_levels`, `fa_stream_get_spectrum` | The stream's own audio, after its equalizer and DSP but before its volume, pan, fades and normalization (as BASS measures a channel). A stream turned down still shows its full level, and a meter doesn't drop during a fade-out. |
| `fa_device_get_levels`, `fa_device_get_spectrum` | The device's whole output after the limiter: every stream, bus and volume, as it reaches the speakers. |

Both read the audio being heard now: the window ends at the device's latency behind the mix, so meters line up with the sound. They give zeros while the stream is not playing (stopped, paused, buffering, detached) or the device is stopped.

Nothing is recorded until the first call: once something asks, a stream or device keeps its last second plus the device's latency, rounded up (512 KB at 44.1 and 48 kHz on most devices, twice that with large callbacks or Bluetooth latency), so streams you never measure cost nothing. The first call therefore reads silence. Ask once when the meter is created, or just let the first frame of the meter show zero. Moments the stream did not play (an underrun, a pause) read as silence.

## Levels

`fa_stream_get_levels(stream, window, peak, rms, channels)`:

- `window` is how much audio is measured, in nanoseconds. Use 20–50 ms for a VU meter that moves like a needle, and 100–300 ms for a calmer one. Windows over about 1 s are shortened.
- `channels` is 2 for left and right, or 1 for one value covering both (the louder peak, the RMS of both sides together).
- Values are linear, 1.0 = full scale. Convert to dB with `20 * log10(value)`.
- Either output may be NULL.

## Spectrum

`fa_stream_get_spectrum(stream, size, flags, bins, &bin_hz)`:

- `size` is the transform size, a power of two from 64 to 16384. `bins` gets `size / 2` magnitudes, DC first. Bin `k` is centred on `k * bin_hz` Hz, and `bin_hz` is the device rate divided by `size` (0 while nothing plays).
- Bigger sizes give finer frequencies and react more slowly. At 48 kHz, 1024 is 46.9 Hz per bin over 21 ms, and 2048 is 23.4 Hz over 43 ms (the usual choice for a bar visualizer). 4096 is 11.7 Hz over 85 ms.
- Magnitudes are linear: a full-scale sine reads 1.0 in its bin, a little less when its frequency falls between two bins.
- A Hann window is applied unless `FA_SPECTRUM_NO_WINDOW` is set. `FA_SPECTRUM_LEFT` or `FA_SPECTRUM_RIGHT` analyse one channel; the default is both mixed.
- The library gives the raw bins only. Grouping them into bars, a log frequency scale, dB, smoothing and peak-hold are the app's choice and take a few lines (below).

## C

```c
#include <fatzaudio/fatzaudio.h>
#include <math.h>

/* Called from a 30-60 fps UI timer. */
void draw_meters(fa_stream* song, fa_device* output)
{
    float peak[2], rms[2];
    fa_stream_get_levels(song, 50000000 /* 50 ms */, peak, rms, 2);
    draw_vu(20 * log10f(peak[0] + 1e-9f), 20 * log10f(peak[1] + 1e-9f));

    /* 32 bars on a log scale from the master output. */
    float bins[1024], hz;
    fa_device_get_spectrum(output, 2048, 0, bins, &hz);
    for (int bar = 0; bar < 32 && hz > 0; bar++) {
        double f0 = 40.0 * pow(16000.0 / 40.0, bar / 32.0);
        double f1 = 40.0 * pow(16000.0 / 40.0, (bar + 1) / 32.0);
        float v = 0;
        for (int k = (int)(f0 / hz); k <= (int)(f1 / hz) && k < 1024; k++) if (bins[k] > v) v = bins[k];
        draw_bar(bar, 20 * log10f(v + 1e-9f));
    }
}
```

## C#

```csharp
using FatzAudio;

// Once, when the window opens (the first call starts recording):
song.GetLevels();
output.GetLevels();

// In a UI timer:
var levels = song.GetLevels(TimeSpan.FromMilliseconds(50));
leftMeter.Value = levels.Peak[0];
rightMeter.Value = levels.Peak.Length > 1 ? levels.Peak[1] : levels.Peak[0];   // mono files give one value

var spectrum = output.GetSpectrum(2048);           // allocates a new array
visualizer.Draw(spectrum.Magnitudes, spectrum.BinHz);

// 60 fps without garbage: reuse your own buffers.
float[] bins = new float[1024];
float binHz = song.GetSpectrum(bins, SpectrumOptions.Left);
float[] peak = new float[2], rms = new float[2];
int channels = song.GetLevels(peak, rms);          // 1 for a mono file, else 2
output.GetLevels(peak, rms);                       // always left and right
```

`OutputDevice.GetLevels` always gives left and right. `AudioStream.GetLevels` gives one value for a mono file and two otherwise. The overloads that take a `Span<float>` (levels and spectrum) write into your buffers and allocate nothing; the others allocate a small result per call, which is fine for an occasional read. `AudioStream.Info` is also allocation-free after the first read while the stream is unchanged.

## Threads

Any thread may call these at any rate. They copy from a buffer the audio thread fills and never wait for it, so a UI timer can't cause dropouts. A spectrum of 2048 takes well under a millisecond. Each call allocates a scratch buffer, so call them from the UI or a timer, not from the audio thread.

## Pitfalls

- The first call reads silence (see above).
- Stream levels ignore the stream's volume by design. Use the device's levels for "how loud is it really", or multiply by the volume yourself.
- Streams that play through the app's FFmpeg packets or a push stream are measured like any other stream. Decode-only streams play nothing, so they return `FA_ERROR_INVALID_ARG`: measure the frames `fa_stream_read` gives you.
- A stream's levels are of its stereo mix as played: a 5.1 file gives the left and right of its downmix, not six channels.
- BASS's `BASS_DATA_FFT_INDIVIDUAL` (one spectrum per channel in one call) is two calls here, with `FA_SPECTRUM_LEFT` and `FA_SPECTRUM_RIGHT`. `BASS_DATA_FFT_COMPLEX` (real and imaginary parts) and `BASS_DATA_FFT_REMOVEDC` are not provided; ignore bin 0 for the DC part.
