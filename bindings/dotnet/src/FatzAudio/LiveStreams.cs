using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace FatzAudio;

/// <summary>
/// Fills a callback stream (<see cref="AudioEngine.OpenCallback"/>) with interleaved float samples.
/// </summary>
/// <param name="buffer">Room for some frames: <c>buffer.Length / channels</c> of them. Write from the start.</param>
/// <returns>
/// The frames written (fewer than asked is fine); 0 when there is nothing right now (asked again about 5 ms later);
/// <see cref="AudioEngine.EndOfStream"/> (or any negative number) when there is no more: the stream plays what it
/// has, then ends. An exception ends the stream too.
/// </returns>
public delegate int AudioCallback(Span<float> buffer);

/// <summary>
/// Fills a callback stream made for any sample format (<see cref="AudioEngine.OpenCallback(AudioFormat, RawAudioCallback, StreamOptions?)"/>).
/// </summary>
/// <param name="buffer">Room for some frames, in the stream's format (<c>buffer.Length / frame size</c> of them). Write from the start.</param>
/// <returns>The frames written; the same rules as <see cref="AudioCallback"/>.</returns>
public delegate int RawAudioCallback(Span<byte> buffer);

/// <summary>Settings of an input opened as a stream (<see cref="AudioEngine.OpenCapture"/>).</summary>
public sealed class CaptureOptions
{
    /// <summary>
    /// The input (from <see cref="AudioEngine.InputDevices"/>); null = the system default input. With
    /// <see cref="Loopback"/>: an output (from <see cref="AudioEngine.OutputDevices"/>); null = the default output.
    /// </summary>
    public AudioDeviceInfo? Device { get; init; }

    /// <summary>The sample rate to record at; 0 = the input's own (the system converts otherwise).</summary>
    public int SampleRate { get; init; }

    /// <summary>The channels to record; 0 = the input's own (the system converts otherwise).</summary>
    public int Channels { get; init; }

    /// <summary>
    /// Keep the system's voice processing (echo cancelling, noise suppression, automatic gain): for calls. Off by
    /// default: it spoils music.
    /// </summary>
    public bool KeepOsProcessing { get; init; }

    /// <summary>
    /// Record what an output plays (every app's sound, the system's mix before its master volume) instead of an input
    /// (BASS_DEVICE_LOOPBACK). Windows only: elsewhere the open throws with <see cref="ErrorCode.NotImplemented"/>.
    /// Gaps where nothing plays are recorded as silence. <see cref="KeepOsProcessing"/> has no effect with it.
    /// </summary>
    public bool Loopback { get; init; }
}

/// <summary>How busy the engine is (<see cref="AudioEngine.GetStats"/>).</summary>
/// <param name="AudioLoad">
/// Percent: the time the output callbacks take to mix, of the time of the audio they make, averaged over about 1 s and
/// added up over the open outputs. Near 100 the mix cannot keep up and the sound breaks up. Mic inputs are not counted.
/// </param>
/// <param name="AudioLoadPeak">Percent: the slowest single callback of any output in the last 1 to 2 s.</param>
/// <param name="WorkerLoad">
/// Percent of one CPU core: the time the decode workers spent decoding ahead (and the tempo, pitch and effects that run
/// with it) since the previous call, or since the engine was created; up to 100 x <paramref name="Workers"/>. Calls
/// less than 100 ms apart repeat the previous value.
/// </param>
/// <param name="Workers">Decode worker threads.</param>
/// <param name="Devices">Open outputs.</param>
/// <param name="AttachedStreams">Streams attached to an output.</param>
public sealed record EngineStats(double AudioLoad, double AudioLoadPeak, double WorkerLoad, int Workers, int Devices,
    int AttachedStreams)
{
    /// <summary>
    /// Dropouts of all streams since the engine was created (every stream's <see cref="StreamStats.Underruns"/>
    /// added up, closed streams too).
    /// </summary>
    public long Underruns { get; init; }
}

/// <summary>How well the decode workers keep up with a stream (<see cref="AudioStream.GetStats"/>).</summary>
/// <param name="Underruns">
/// Output callbacks that found too little decoded audio for the stream while it played (a dropout), since it was
/// opened. Its end, a pause and a stop are not counted; a push, callback or web stream that runs out of data is.
/// </param>
/// <param name="Buffered">
/// Decoded audio ready ahead of the mix, as playing time (tempo and rate applied); zero when not on a device.
/// </param>
public sealed record StreamStats(long Underruns, TimeSpan Buffered);

/// <summary>
/// Loudness meter readings (<see cref="AudioStream.GetLoudnessMeter"/>, <see cref="OutputDevice.GetLoudnessMeter"/>),
/// in LUFS (EBU R128 / ITU-R BS.1770). <see cref="double.NegativeInfinity"/> for silence.
/// </summary>
/// <param name="Momentary">The loudness of the last 400 ms.</param>
/// <param name="ShortTerm">The loudness of the last 3 s.</param>
public readonly record struct LoudnessMeter(double Momentary, double ShortTerm);

/// <summary>Bridges a managed <see cref="AudioCallback"/> to the native fa_stream_proc.</summary>
internal sealed unsafe class CallbackState
{
    private readonly AudioCallback? _fill;
    private readonly RawAudioCallback? _raw;
    private readonly int _unit;   // floats per frame (channels), or bytes per frame for a raw callback

    public CallbackState(AudioCallback fill, int channels)
    {
        _fill = fill;
        _unit = channels;
    }

    public CallbackState(RawAudioCallback raw, int frameBytes)
    {
        _raw = raw;
        _unit = frameBytes;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static long Fill(void* buffer, long frames, void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((nint)user).Target is not CallbackState s) return AudioEngine.EndOfStream;
            int n = s._fill is { } f
                ? f(new Span<float>(buffer, checked((int)(frames * s._unit))))
                : s._raw!(new Span<byte>(buffer, checked((int)(frames * s._unit))));
            return n < 0 ? AudioEngine.EndOfStream : Math.Min(n, frames);
        }
        catch
        {
            return AudioEngine.EndOfStream;   // never throw across the native boundary: the stream ends
        }
    }
}
