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

/// <summary>Settings of an input opened as a stream (<see cref="AudioEngine.OpenCapture"/>).</summary>
public sealed class CaptureOptions
{
    /// <summary>The input (from <see cref="AudioEngine.InputDevices"/>); null = the system default input.</summary>
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
    int AttachedStreams);

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
    private readonly AudioCallback _fill;
    private readonly int _channels;

    public CallbackState(AudioCallback fill, int channels)
    {
        _fill = fill;
        _channels = channels;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static long Fill(void* buffer, long frames, void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((nint)user).Target is not CallbackState s) return AudioEngine.EndOfStream;
            int n = s._fill(new Span<float>(buffer, checked((int)(frames * s._channels))));
            return n < 0 ? AudioEngine.EndOfStream : Math.Min(n, frames);
        }
        catch
        {
            return AudioEngine.EndOfStream;   // never throw across the native boundary: the stream ends
        }
    }
}
