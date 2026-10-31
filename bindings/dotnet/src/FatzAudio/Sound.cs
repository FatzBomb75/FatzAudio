using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Which voice a <see cref="Sound"/> takes over when all of them are busy.</summary>
public enum SoundSteal
{
    /// <summary>The voice that started longest ago restarts as the new copy (the default; BASS_SAMPLE_OVER_POS).</summary>
    Oldest = 0,
    /// <summary>The voice with the lowest volume right now; the older one on a tie (BASS_SAMPLE_OVER_VOL).</summary>
    Quietest = 1,
    /// <summary>None: <see cref="Sound.Play(OutputDevice, float, float)"/> throws <see cref="FatzAudioException"/>
    /// with <see cref="ErrorCode.Busy"/>.</summary>
    None = 2,
}

/// <summary>Options for <see cref="AudioEngine.LoadSound(string, SoundOptions?)"/> and the other sound loaders.</summary>
public sealed class SoundOptions
{
    /// <summary>Copies that can play at once, 1 to 1024. Default 8.</summary>
    public int MaxVoices { get; init; } = 8;

    /// <summary>What a play does when every voice is busy. Default <see cref="SoundSteal.Oldest"/>.</summary>
    public SoundSteal Steal { get; init; } = SoundSteal.Oldest;

    /// <summary>How the file is opened (audio track, soundfont, MIME hint); null = defaults. Flags such as
    /// <see cref="StreamOptions.Loop"/> are ignored.</summary>
    public StreamOptions? Open { get; init; }
}

/// <summary>Information about a <see cref="Sound"/>.</summary>
/// <param name="SampleRate">The sound's own rate (each voice converts to its device).</param>
/// <param name="Channels">Its channel count.</param>
/// <param name="Frames">Its length in frames.</param>
/// <param name="MemoryBytes">Bytes of PCM (32-bit float) held in memory, shared by all voices.</param>
public sealed record SoundInfo(int SampleRate, int Channels, long Frames, long MemoryBytes)
{
    /// <summary>The length as time.</summary>
    public TimeSpan Duration => SampleRate > 0 ? TimeSpan.FromSeconds((double)Frames / SampleRate) : TimeSpan.Zero;
}

/// <summary>
/// A short sound decoded once into memory and played as many overlapping copies (voices) as you like, each with
/// its own volume and pan: jingles, applause, air horns, sound-effect pads (BASS_Sample*). All voices read the one
/// copy of the audio.
/// </summary>
/// <remarks>
/// Each voice is an ordinary <see cref="AudioStream"/>, owned by the sound: stop it, change its volume, pan or
/// rate, ramp it, put effects on it or listen for <see cref="AudioStream.Ended"/>. A voice that ended or was
/// stopped is reused by a later <see cref="Play(OutputDevice, float, float)"/>, so the same object may by then be
/// playing a newer copy; a reused voice starts at its play's volume and pan with looping off. Disposing a voice is
/// allowed (the sound makes a new one when it needs it). At most <see cref="MaxVoices"/> copies play at once; a
/// play beyond that takes a voice over by <see cref="Steal"/> (it restarts at once, without a fade).
/// Disposing the sound stops and disposes its voices. All members are thread-safe.
/// </remarks>
/// <example>
/// <code>
/// using var horn = engine.LoadSound("horn.wav");
/// horn.Play(output);                       // fire and forget
/// horn.Play(output, volume: 0.5f, pan: -0.8f);   // a second copy over it, to the left
/// </code>
/// </example>
public sealed unsafe class Sound : IDisposable
{
    private readonly SoundHandle _handle;
    private readonly Dictionary<nint, AudioStream> _voices = new();

    private Sound(AudioEngine engine, SoundHandle handle)
    {
        Engine = engine;
        _handle = handle;
        var n = ReadInfo();
        Info = new SoundInfo((int)n.SampleRate, (int)n.Channels, n.Frames, (long)n.MemoryBytes);
    }

    internal static Sound Wrap(AudioEngine engine, int result, nint native)
    {
        FatzAudioException.ThrowIfFailed(result);
        var h = new SoundHandle();
        h.Initialize(native, engine.Handle);
        return new Sound(engine, h);
    }

    /// <summary>The native <c>fa_sound_options</c> for a call (Open is filled in by the caller).</summary>
    internal static FaSoundOptions ToNative(SoundOptions? options)
    {
        FaSoundOptions o;
        NativeMethods.fa_sound_options_init(&o);
        if (options is not null)
        {
            if (options.MaxVoices is < 1 or > 1024)
                throw new ArgumentOutOfRangeException(nameof(options), "MaxVoices must be 1 to 1024.");
            o.MaxVoices = (uint)options.MaxVoices;
            o.Steal = (int)options.Steal;
        }
        return o;
    }

    private SoundHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>The engine the sound belongs to.</summary>
    public AudioEngine Engine { get; }

    /// <summary>The sound's format, length and memory use.</summary>
    public SoundInfo Info { get; }

    private FaSoundInfo ReadInfo()
    {
        FaSoundInfo n;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_sound_get_info(Handle, &n));
        return n;
    }

    /// <summary>Copies that can play at once, 1 to 1024. Lowering it retires the voices over the limit (free ones
    /// first, then the oldest playing): they stop and the sound no longer uses them.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is outside 1 to 1024.</exception>
    public int MaxVoices
    {
        get => (int)ReadInfo().MaxVoices;
        set
        {
            if (value is < 1 or > 1024) throw new ArgumentOutOfRangeException(nameof(value), "MaxVoices must be 1 to 1024.");
            SetOptions((uint)value, null);
        }
    }

    /// <summary>What a play does when every voice is busy.</summary>
    public SoundSteal Steal
    {
        get => (SoundSteal)ReadInfo().Steal;
        set => SetOptions(null, value);
    }

    private void SetOptions(uint? maxVoices, SoundSteal? steal)
    {
        lock (_voices)
        {
            var n = ReadInfo();
            FaSoundOptions o;
            NativeMethods.fa_sound_options_init(&o);
            o.MaxVoices = maxVoices ?? n.MaxVoices;
            o.Steal = steal is { } s ? (int)s : n.Steal;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_sound_set_options(Handle, &o));
        }
    }

    /// <summary>Voices made so far, playing or free.</summary>
    public int VoiceCount => (int)ReadInfo().Voices;

    /// <summary>Voices busy now: playing, paused or scheduled.</summary>
    public int PlayingCount => (int)ReadInfo().Playing;

    /// <summary>Plays a copy now on <paramref name="output"/>'s master mix (starting the device if needed). The
    /// first audio is buffered before the call returns.</summary>
    /// <param name="output">The device to play on.</param>
    /// <param name="volume">Linear gain, 0 or more (1 = as recorded).</param>
    /// <param name="pan">-1 (left) to 1 (right).</param>
    /// <returns>The voice playing the copy. The sound owns it: there is no need to dispose it.</returns>
    /// <exception cref="FatzAudioException">Every voice is busy and <see cref="Steal"/> is <see cref="SoundSteal.None"/>
    /// (<see cref="ErrorCode.Busy"/>), or the device belongs to another engine (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public AudioStream Play(OutputDevice output, float volume = 1.0f, float pan = 0.0f)
    {
        ArgumentNullException.ThrowIfNull(output);
        return PlayOn(output, null, volume, pan);
    }

    /// <summary>Plays a copy now through <paramref name="bus"/> (starting its device if needed).</summary>
    /// <param name="bus">The bus to play through.</param>
    /// <param name="volume">Linear gain, 0 or more.</param>
    /// <param name="pan">-1 (left) to 1 (right).</param>
    /// <returns>The voice playing the copy (owned by the sound).</returns>
    /// <exception cref="FatzAudioException">See <see cref="Play(OutputDevice, float, float)"/>.</exception>
    public AudioStream Play(MixBus bus, float volume = 1.0f, float pan = 0.0f)
    {
        ArgumentNullException.ThrowIfNull(bus);
        return PlayOn(bus.Device, bus, volume, pan);
    }

    private AudioStream PlayOn(OutputDevice output, MixBus? bus, float volume, float pan)
    {
        if (!(volume >= 0)) throw new ArgumentOutOfRangeException(nameof(volume), "Volume must be 0 or more.");
        if (float.IsNaN(pan)) throw new ArgumentOutOfRangeException(nameof(pan));
        var dh = output.Handle;
        var bh = bus?.Handle;
        output.Start();
        lock (_voices)
        {
            bool added = false;
            nint native;
            int rc;
            try
            {
                bh?.DangerousAddRef(ref added);
                rc = NativeMethods.fa_sound_play(Handle, dh, bh?.DangerousGetHandle() ?? 0, volume, pan, out native);
            }
            finally
            {
                if (added) bh!.DangerousRelease();
            }
            FatzAudioException.ThrowIfFailed(rc);
            var voice = VoiceFor(native);
            voice.JoinedVoice(output, bus);
            return voice;
        }
    }

    // The wrapper of a native voice: one per voice, made the first time it is seen (lock held).
    private AudioStream VoiceFor(nint native)
    {
        if (_voices.TryGetValue(native, out var known) && !known.IsDisposed) return known;
        var h = new StreamHandle();
        h.Initialize(native, Engine.Handle);
        h.SetOwner(_handle);   // the sound stays open while a voice wrapper is alive
        var voice = new AudioStream(Engine, h, null);
        _voices[native] = voice;
        return voice;
    }

    /// <summary>Stops every copy (BASS_SampleStop).</summary>
    public void Stop() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_sound_stop(Handle));

    /// <summary>The voices busy now, oldest first (BASS_SampleGetChannels).</summary>
    /// <returns>A snapshot; voices may end at any time after.</returns>
    public IReadOnlyList<AudioStream> GetPlayingVoices()
    {
        lock (_voices)
        {
            var h = Handle;
            uint count;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_sound_get_voices(h, null, 0, out count));
            if (count == 0) return Array.Empty<AudioStream>();
            var buf = new nint[count];
            fixed (nint* p = buf)
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_sound_get_voices(h, p, count, out count));
            int n = (int)Math.Min(count, (uint)buf.Length);   // fewer if some ended between the calls
            var list = new List<AudioStream>(n);
            for (int i = 0; i < n; i++) list.Add(VoiceFor(buf[i]));
            return list;
        }
    }

    /// <summary>Stops and disposes every voice, then frees the sound.</summary>
    public void Dispose()
    {
        lock (_voices)
        {
            foreach (var v in _voices.Values) v.Dispose();
            _voices.Clear();
        }
        _handle.Dispose();
    }
}
