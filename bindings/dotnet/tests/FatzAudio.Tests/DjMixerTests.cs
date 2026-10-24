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
    public void CueDevice_OpensTheHeadphonesAndLinesUp()
    {
        using var engine = CreateHeadless();
        var output = new AudioDeviceInfo("x", "x", DeviceKind.Capture, false, 48000, 2);
        Assert.Throws<ArgumentException>(() => CueOutput.Device(output));
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Device(), Latency = LatencyMode.Normal });
        Assert.Equal(2, mixer.OutputChannels);
        Assert.Null(mixer.CueChannel);
        mixer.CueTrim = TimeSpan.FromMilliseconds(400);
        Assert.Equal(TimeSpan.FromMilliseconds(250), mixer.CueTrim);
        mixer.CueTrim = TimeSpan.FromMilliseconds(-12);
        Assert.Equal(TimeSpan.FromMilliseconds(-12), mixer.CueTrim);
        Assert.True(Enum.IsDefined(mixer.Latency.CueAligned));
        Assert.True(mixer.Latency.CueAlignment >= TimeSpan.Zero);
        mixer.SetCueDevice(null);   // the default output again
        Assert.Throws<ArgumentException>(() => mixer.SetCueDevice(output));
        var ex = Assert.Throws<FatzAudioException>(() => mixer.SetCueDevice(new AudioDeviceInfo("zz", "zz", DeviceKind.Playback, false, 48000, 2)));
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
    }

    [Fact]
    public void CueDevice_LineUpOffReportsOff()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Device(lineUp: false) });
        Assert.Equal(CueAlign.Off, mixer.Latency.CueAligned);
        Assert.Equal(TimeSpan.Zero, mixer.Latency.CueAlignment);
        using var plain = engine.CreateDjMixer();
        Assert.Equal(CueAlign.None, plain.Latency.CueAligned);
        var ex = Assert.Throws<FatzAudioException>(() => plain.SetCueDevice(null));
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
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

    [Fact]
    public void Mics_AddRoundTripAndRemove()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer();
        Assert.Empty(mixer.Mics);
        Assert.Null(mixer.Latency.MicInput[0]);
        using var mic = mixer.AddMic(new MicOptions { Channel = 0 });
        using var second = mixer.AddMic(new MicOptions { Channel = 1, Monitor = false });
        Assert.Equal(new[] { mic, second }, mixer.Mics);
        Assert.Equal(0, mic.Index);
        Assert.Equal(1, second.Index);
        var ex = Assert.Throws<FatzAudioException>(() => mixer.AddMic());
        Assert.Equal(ErrorCode.Busy, ex.ErrorCode);

        Assert.Equal(-50f, mic.GateDb);
        Assert.True(mic.LowCut);
        Assert.True(mic.Monitor);
        Assert.False(second.Monitor);
        mic.GainDb = 100f;
        mic.Pan = -0.5f;
        mic.Muted = true;
        mic.GateDb = -200f;
        mic.LowCut = false;
        Assert.Equal(40f, mic.GainDb);
        Assert.Equal(-0.5f, mic.Pan);
        Assert.True(mic.Muted);
        Assert.Equal(-100f, mic.GateDb);
        Assert.False(mic.LowCut);

        Assert.Empty(mic.GetEqualizer());
        mic.SetEqualizer(EqualizerPreset.LiveVocal);
        Assert.Equal(Equalizer.GetPreset(EqualizerPreset.LiveVocal), mic.GetEqualizer());
        Assert.Equal("Live vocal", Equalizer.GetPresetName(EqualizerPreset.LiveVocal));
        var levels = mic.GetLevels();
        Assert.Equal(0f, levels.Peak);

        var latency = mixer.Latency;
        Assert.True(latency.Master > TimeSpan.Zero);
        Assert.True(latency.Cue <= latency.Master);   // the master adds the limiter's look-ahead
        Assert.True(mic.InputLatency > TimeSpan.Zero);
        Assert.Equal(latency.MicInput[0]!.Value + latency.Master, latency.MicMonitor[0]);

        mic.Dispose();
        Assert.Equal(new[] { second }, mixer.Mics);
        Assert.Throws<ObjectDisposedException>(() => mic.GainDb);
        Assert.Null(mixer.Latency.MicInput[0]);
        using var again = mixer.AddMic();
        Assert.Equal(0, again.Index);
    }

    [Fact]
    public void Mics_ConfigErrors()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer();
        var ex = Assert.Throws<FatzAudioException>(() => mixer.AddMic(new MicOptions { Channel = 31 }));
        Assert.Equal(ErrorCode.NotEnoughChannels, ex.ErrorCode);
        Assert.Throws<ArgumentOutOfRangeException>(() => mixer.AddMic(new MicOptions { Channel = -1 }));
        var output = new AudioDeviceInfo("x", "x", DeviceKind.Playback, false, 48000, 2);
        Assert.Throws<ArgumentException>(() => mixer.AddMic(new MicOptions { Device = output }));
        Assert.Empty(mixer.Mics);
    }

    [Fact]
    public void LatencyMode_SwitchesAndKeepsTheMics()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Latency = LatencyMode.Normal });
        using var mic = mixer.AddMic();
        Assert.Equal(LatencyMode.Normal, mixer.LatencyMode);
        mixer.LatencyMode = LatencyMode.Low;
        Assert.Equal(LatencyMode.Low, mixer.LatencyMode);
        Assert.True(mixer.Output.IsStarted);
        Assert.Equal(new[] { mic }, mixer.Mics);
        var ex = Assert.Throws<FatzAudioException>(() => mixer.LatencyMode = LatencyMode.Exclusive);
        Assert.Equal(ErrorCode.NotImplemented, ex.ErrorCode);   // the null output isn't WASAPI
        Assert.Equal(LatencyMode.Low, mixer.LatencyMode);
    }

    [Fact]
    public async Task MeasureLatency_SilentMicIsNotHeard()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Latency = LatencyMode.Normal });
        using var other = engine.CreateDjMixer();
        using var mic = mixer.AddMic();
        using var stranger = other.AddMic();
        await Assert.ThrowsAsync<ArgumentNullException>(() => mixer.MeasureLatencyAsync(null!));
        await Assert.ThrowsAsync<ArgumentException>(() => mixer.MeasureLatencyAsync(stranger));

        var check = await mixer.MeasureLatencyAsync(mic, new LatencyCheckOptions { MaxLevelDb = -30f });
        Assert.Equal(CheckAdvice.NotHeard, check.Advice);
        var mode = Assert.Single(check.Modes);
        Assert.Equal(LatencyMode.Normal, mode.Mode);
        Assert.Equal(ErrorCode.NotFound, mode.Error);
        Assert.True(mode.Reported > TimeSpan.Zero);
    }

    [Fact]
    public void Mics_GoWithTheMixer()
    {
        using var engine = CreateHeadless();
        var mixer = engine.CreateDjMixer();
        var mic = mixer.AddMic();
        mixer.InputLost += (_, e) => e.Mic.Dispose();
        mixer.Dispose();
        Assert.True(mixer.Output.IsDisposed);
        Assert.Throws<ObjectDisposedException>(() => mic.GainDb);
        mic.Dispose();                   // twice is fine
    }

    [Fact]
    public void MicEffectsAndDucking_RoundTripAndClamp()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer();
        using var mic = mixer.AddMic();
        Assert.Equal(0.2f, mic.ReverbSend);
        Assert.Equal(0f, mic.EchoSend);
        mic.ReverbSend = 2f;
        mic.EchoSend = 0.5f;
        Assert.Equal(1f, mic.ReverbSend);
        Assert.Equal(0.5f, mic.EchoSend);
        Assert.Equal(new MicEffects(), mixer.MicEffects);
        mixer.MicEffects = new MicEffects { ReverbSize = 0.8f, EchoTime = TimeSpan.FromSeconds(5), EchoFeedback = 0.5f };
        var fx = mixer.MicEffects;
        Assert.Equal(0.8f, fx.ReverbSize);
        Assert.Equal(TimeSpan.FromSeconds(1), fx.EchoTime);
        Assert.Equal(0.5f, fx.EchoFeedback);
        Assert.Equal(new Ducking(), mixer.Ducking);
        mixer.Ducking = new Ducking { DepthDb = -6f, Release = TimeSpan.FromSeconds(1) };
        Assert.Equal(-6f, mixer.Ducking!.DepthDb);
        Assert.Equal(TimeSpan.FromSeconds(1), mixer.Ducking.Release);
        mixer.Ducking = null;
        Assert.Null(mixer.Ducking);
    }

    [Fact]
    public void Recording_MasterAndVocalsStartTogether()
    {
        using var engine = CreateHeadless();
        using var mixer = engine.CreateDjMixer(new DjMixerOptions { Latency = LatencyMode.Normal });
        using var mic = mixer.AddMic();
        var masterFile = new MemoryStream();
        var vocalsFile = new MemoryStream();
        using var master = AudioEncoder.Create(masterFile, leaveOpen: true);
        using var vocals = AudioEncoder.Create(vocalsFile, leaveOpen: true);
        master.Record(mixer, MixerTap.Master);
        vocals.Record(mixer, MixerTap.Vocals);
        Thread.Sleep(100);
        Assert.Equal(0, master.Status.Frames);   // waits for StartRecording
        mixer.StartRecording([master, vocals]);
        var ex = Assert.Throws<FatzAudioException>(() => mixer.StartRecording([master]));
        Assert.Equal(ErrorCode.Busy, ex.ErrorCode);
        Thread.Sleep(400);
        master.Close();
        vocals.Close();
        Assert.True(masterFile.Length > 44);
        Assert.True(vocalsFile.Length > 44);
        using var other = AudioEncoder.Create(new MemoryStream());
        ex = Assert.Throws<FatzAudioException>(() => mixer.StartRecording([other]));
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
    }
}
