using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>The DJ mixer and its decks (the native white-box test checks the audio itself).</summary>
public class DjMixerTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    [Fact]
    public void Create_HasTwoDecksOnTheirSides()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Latency = LatencyMode.Normal });
        Assert.Equal(2, mixer.Decks.Count);
        Assert.Equal(CrossfaderSide.A, mixer.Decks[0].Side);
        Assert.Equal(CrossfaderSide.B, mixer.Decks[1].Side);
        Assert.Equal(0.5f, mixer.Crossfader.Position);
        Assert.Equal(CrossfaderCurve.ConstantPower, mixer.Crossfader.Curve);
        Assert.Equal(2, mixer.OutputChannels);
        Assert.Null(mixer.CueChannel);
        Assert.True(mixer.Output.IsStarted);
    }

    [Fact]
    public void Channels34_OpensFourChannels()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Channels34 });
        Assert.Equal(4, mixer.OutputChannels);
        Assert.Equal(2, mixer.CueChannel);
    }

    [Fact]
    public void Planned_ModesReportNotImplemented()
    {
        using var engine = CreateHeadless();
        var ex = Assert.Throws<FatzAudioException>(() => engine.CreateDjMixer(new DjMixerOptions { Decks = 4 }));
        Assert.Equal(ErrorCode.NotImplemented, ex.ErrorCode);
        ex = Assert.Throws<FatzAudioException>(() => engine.CreateDjMixer(new DjMixerOptions { Latency = LatencyMode.Exclusive }));
        Assert.Equal(ErrorCode.NotImplemented, ex.ErrorCode);
    }

    [Fact]
    public void Parameters_RoundTripAndClamp()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer();
        var a = mixer.Decks[0];
        a.Gain = 0.25f;
        a.TrimDb = 30f;
        a.Eq.Low = -100f;
        a.Eq.High = 3f;
        a.Side = CrossfaderSide.Thru;
        a.Cue = true;
        mixer.Crossfader.Position = 1f;
        mixer.Crossfader.Curve = CrossfaderCurve.Linear;
        mixer.MasterGain = 0.5f;
        mixer.CueMix = 0.25f;
        mixer.CueGain = 2f;
        Assert.Equal(0.25f, a.Gain);
        Assert.Equal(12f, a.TrimDb);
        Assert.Equal(DeckEq.Kill, a.Eq.Low);
        Assert.Equal(3f, a.Eq.High);
        Assert.Equal(CrossfaderSide.Thru, a.Side);
        Assert.True(a.Cue);
        Assert.Equal(1f, mixer.Crossfader.Position);
        Assert.Equal(CrossfaderCurve.Linear, mixer.Crossfader.Curve);
        Assert.Equal(0.5f, mixer.MasterGain);
        Assert.Equal(0.25f, mixer.CueMix);
        Assert.Equal(2f, mixer.CueGain);
        mixer.Crossfader.Ramp(0f, TimeSpan.FromSeconds(1));
        a.Ramp(DeckParameter.EqMidDb, -6f, TimeSpan.FromMilliseconds(100));
        Assert.Throws<ArgumentOutOfRangeException>(() => a.Ramp(DeckParameter.Gain, 1f, TimeSpan.FromSeconds(-1)));
    }

    [Fact]
    public void Load_MovesStreamsAndUnloadLeavesThemOpen()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 4));
        var mixer = engine.CreateDjMixer();
        DjDeck a = mixer.Decks[0], b = mixer.Decks[1];
        a.Load(s);
        Assert.Same(s, a.Stream);
        Assert.Same(mixer.Output, s.Device);
        b.Load(s);                       // moves to B
        Assert.Null(a.Stream);
        Assert.Same(s, b.Stream);
        b.CuePoint = 1000;
        Assert.Equal(1000, b.CuePoint);
        b.Loop = new DeckLoop(2000, 9000);
        Assert.Equal(new DeckLoop(2000, 9000), b.Loop);
        s.Play();
        mixer.Crossfader.Position = 1f;  // B heard
        var ex = Assert.Throws<FatzAudioException>(() => b.Unload());
        Assert.Equal(ErrorCode.Busy, ex.ErrorCode);
        b.ReturnToCuePoint();
        Assert.Equal(StreamState.Paused, s.State);
        Assert.Equal(1000, s.PositionFrames);
        var levels = b.GetLevels();
        Assert.Equal(2, levels.Peak.Length);
        mixer.Dispose();                 // unloads: the stream stays open, detached
        Assert.Null(s.Device);
        Assert.Equal(1000, s.PositionFrames);
        using var empty = engine.CreateDjMixer();
        var ex2 = Assert.Throws<FatzAudioException>(() => empty.Decks[0].ReturnToCuePoint());
        Assert.Equal(ErrorCode.NotFound, ex2.ErrorCode);
    }

    [Fact]
    public void DisposedStream_LeavesTheDeck()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer();
        var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1));
        mixer.Decks[0].Load(s);
        s.Dispose();
        Assert.Null(mixer.Decks[0].Stream);
        Assert.Null(mixer.Decks[0].Loop);
    }
}
