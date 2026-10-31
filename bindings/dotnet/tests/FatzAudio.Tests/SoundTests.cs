using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Sounds: overlapping voices, the voice limit and steal rules, voice reuse, on the null output.</summary>
public class SoundTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    private static float[] Tone(int frames, float level = 0.2f)
    {
        var s = new float[frames];
        Array.Fill(s, level);
        return s;
    }

    [Fact]
    public void LoadSound_FromWav_ReportsItsFormat()
    {
        using var engine = AudioEngine.Create();
        using var sound = engine.LoadSound(TestAudio.SineWav(0.5, 22050));
        Assert.Equal(22050, sound.Info.SampleRate);
        Assert.Equal(11025, sound.Info.Frames);
        Assert.Equal(11025L * sound.Info.Channels * 4, sound.Info.MemoryBytes);
        Assert.Equal(TimeSpan.FromSeconds(0.5), sound.Info.Duration);
        Assert.Equal(8, sound.MaxVoices);
        Assert.Equal(SoundSteal.Oldest, sound.Steal);
        Assert.Equal(0, sound.VoiceCount);
    }

    [Fact]
    public void Play_OverlappingCopies_EachHaveTheirOwnVolumeAndPan()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var sound = engine.CreateSound(Tone(48000), 48000, 1);
        var a = sound.Play(output, 1.0f, -0.5f);
        var b = sound.Play(output, 0.25f, 0.5f);
        Assert.NotSame(a, b);
        Assert.Equal(1.0f, a.Volume);
        Assert.Equal(0.25f, b.Volume);
        Assert.Equal(-0.5f, a.Pan);
        Assert.Equal(0.5f, b.Pan);
        Assert.Equal(2, sound.VoiceCount);
        Assert.Equal(2, sound.PlayingCount);
        var playing = sound.GetPlayingVoices();
        Assert.Equal(2, playing.Count);
        Assert.Same(a, playing[0]);   // oldest first
        Assert.Same(b, playing[1]);
        sound.Stop();
        Assert.Equal(0, sound.PlayingCount);
        Assert.Equal(StreamState.Stopped, a.State);
    }

    [Fact]
    public void Play_AtTheLimit_StealsByTheRule()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var sound = engine.CreateSound(Tone(96000), 48000, 1, new SoundOptions { MaxVoices = 2 });
        var a = sound.Play(output);
        var b = sound.Play(output, 0.1f);
        Assert.Same(a, sound.Play(output));            // the oldest
        sound.Steal = SoundSteal.Quietest;
        Assert.Same(b, sound.Play(output, 0.5f));      // the quietest (0.1)
        sound.Steal = SoundSteal.None;
        var e = Assert.Throws<FatzAudioException>(() => sound.Play(output));
        Assert.Equal(ErrorCode.Busy, e.ErrorCode);
        a.Stop();
        Assert.Same(a, sound.Play(output));            // a stopped voice is free again
    }

    [Fact]
    public void MaxVoices_Lowered_RetiresVoices()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var sound = engine.CreateSound(Tone(96000), 48000, 1);
        var first = sound.Play(output);
        for (int i = 0; i < 3; i++) sound.Play(output);
        Assert.Equal(4, sound.VoiceCount);
        sound.MaxVoices = 1;
        Assert.Equal(StreamState.Stopped, first.State);   // retired: stopped, still usable
        Assert.Equal(1, sound.MaxVoices);
        Assert.Equal(1, sound.VoiceCount);
        Assert.Throws<ArgumentOutOfRangeException>(() => sound.MaxVoices = 0);
    }

    [Fact]
    public void DisposedVoice_IsReplaced_AndDisposeClosesEverything()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        var sound = engine.CreateSound(Tone(4800), 48000, 1);
        var a = sound.Play(output);
        a.Dispose();
        Assert.Equal(0, sound.VoiceCount);
        var b = sound.Play(output);
        Assert.NotSame(a, b);
        sound.Dispose();
        Assert.Throws<ObjectDisposedException>(() => b.Volume);
        Assert.Throws<ObjectDisposedException>(() => sound.Play(output));
    }

    [Fact]
    public async Task EndedVoice_IsReused()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var sound = engine.CreateSound(Tone(480), 48000, 1);   // 10 ms
        var a = sound.Play(output);
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (a.State != StreamState.Ended && DateTime.UtcNow < deadline) await Task.Delay(10);
        Assert.Equal(StreamState.Ended, a.State);
        Assert.Same(a, sound.Play(output, 0.5f));
        Assert.Equal(0.5f, a.Volume);
        Assert.Equal(1, sound.VoiceCount);
    }

    [Fact]
    public void CreateSound_RejectsBadInput()
    {
        using var engine = AudioEngine.Create();
        Assert.Throws<ArgumentException>(() => engine.CreateSound(new float[3], 48000, 2));
        Assert.Throws<ArgumentException>(() => engine.CreateSound(ReadOnlySpan<float>.Empty, 48000, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => engine.CreateSound(new float[4], 48000, 1, new SoundOptions { MaxVoices = 0 }));
        var e = Assert.Throws<FatzAudioException>(() => engine.CreateSound(new float[4], 10, 1));
        Assert.Equal(ErrorCode.InvalidArgument, e.ErrorCode);
    }
}
