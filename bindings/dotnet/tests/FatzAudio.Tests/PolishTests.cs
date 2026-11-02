using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>The additive .NET polish of final audit step 10: time companions, async opens, raw formats, error args.</summary>
public class PolishTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    [Fact]
    public void DeckTimes_FollowTheStreamsRate()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 4));   // 44.1 kHz
        using var mixer = engine.CreateDjMixer();
        var deck = mixer.Decks[0];
        Assert.Null(deck.LoopTime);
        Assert.Throws<InvalidOperationException>(() => deck.CuePointTime = TimeSpan.FromSeconds(1));
        deck.Load(s);
        deck.CuePointTime = TimeSpan.FromSeconds(1);
        Assert.Equal(44100, deck.CuePoint);
        Assert.Equal(TimeSpan.FromSeconds(1), deck.CuePointTime);
        deck.LoopTime = new DeckLoopTime(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2));
        Assert.Equal(new DeckLoop(44100, 88200), deck.Loop);
        Assert.Equal(new DeckLoopTime(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2)), deck.LoopTime);
        deck.LoopTime = null;
        Assert.Null(deck.Loop);
        Assert.Null(deck.SlipPositionTime);
    }

    [Fact]
    public void MidiEvent_FrameAndTime()
    {
        long f = MidiEvent.FrameAt(TimeSpan.FromSeconds(0.5), 44100);
        Assert.Equal(22050, f);
        Assert.Equal(TimeSpan.FromSeconds(0.5), MidiEvent.NoteOn(0, 60, 100, f).TimeAt(44100));
        Assert.Equal(TimeSpan.Zero, MidiEvent.NoteOn(0, 60, 100).TimeAt(44100));
    }

    [Fact]
    public async Task OpenMemoryAsync_OpensAndCancels()
    {
        using var engine = CreateHeadless();
        using var s = await engine.OpenMemoryAsync(TestAudio.SineWav(1));
        Assert.Equal(44100, s.Info.Format.SampleRate);
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => engine.OpenMemoryAsync(TestAudio.SineWav(1), null, cts.Token));
    }

    [Fact]
    public async Task OpenMidiAsync_OpensASynth()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var s = await engine.OpenMidiAsync(new MidiStreamOptions { SampleRate = 44100 }, new StreamOptions { DecodeOnly = true, SoundFont = font });
        Assert.True(s.TrySendMidi(MidiEvent.NoteOn(0, 69, 127), out var error));
        Assert.Null(error);
        using var e = await engine.OpenMidiEventsAsync([MidiEvent.NoteOn(0, 69, 127, 0), MidiEvent.NoteOff(0, 69, 4410)],
            new MidiStreamOptions { SampleRate = 44100 }, new StreamOptions { DecodeOnly = true, SoundFont = font });
        Assert.True(e.Info.LengthFrames > 0);
    }

    [Fact]
    public void TrySendMidi_ReportsInsteadOfThrowing()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var s = engine.OpenMidi(new MidiStreamOptions { SampleRate = 44100 }, new StreamOptions { DecodeOnly = true, SoundFont = font });
        Assert.False(s.TrySendMidi(MidiEvent.NoteOn(16, 60, 100), out var error));   // channel out of range
        Assert.Equal(ErrorCode.InvalidArgument, error);
        // Not played, the queue fills up: Busy, nothing sent.
        var many = new MidiEvent[4096];
        Array.Fill(many, MidiEvent.NoteOn(0, 60, 100, 1_000_000));
        Assert.True(s.TrySendMidi(many, out error));
        Assert.True(s.TrySendMidi(many, out error));
        Assert.False(s.TrySendMidi(many, out error));
        Assert.Equal(ErrorCode.Busy, error);
        using var plain = engine.OpenMemory(TestAudio.SineWav(1));
        Assert.False(plain.TrySendMidi(MidiEvent.NoteOn(0, 60, 100), out error));
        Assert.Equal(ErrorCode.InvalidArgument, error);
    }

    [Fact]
    public async Task CrossfadeToAsync_CancelStopsWaitingNotTheFade()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var a = engine.OpenMemory(TestAudio.SineWav(5));
        using var b = engine.OpenMemory(TestAudio.SineWav(5));
        a.PlayOn(output);
        using var cts = new CancellationTokenSource();
        var task = a.CrossfadeToAsync(b, TimeSpan.FromSeconds(3), null, cts.Token);
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
        Assert.NotEqual(StreamState.Stopped, b.State);   // the fade carries on
        using var done = new CancellationTokenSource();
        done.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => a.CrossfadeToAsync(b, TimeSpan.FromSeconds(1), null, done.Token));
    }

    [Fact]
    public void CreateSound_FromInt16Bytes()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        var bytes = new byte[4410 * 2 * 2];   // 0.1 s of stereo 16-bit
        for (int i = 0; i < bytes.Length; i += 2) BitConverter.TryWriteBytes(bytes.AsSpan(i), (short)8000);
        using var sound = engine.CreateSound(bytes, AudioFormat.Int16(44100, 2));
        Assert.Equal(4410, sound.Info.Frames);
        Assert.Equal(2, sound.Info.Channels);
        Assert.Throws<ArgumentException>(() => engine.CreateSound(bytes.AsSpan(0, 5), AudioFormat.Int16(44100, 2)));
        Assert.Throws<ArgumentException>(() => engine.CreateSound(bytes, new AudioFormat(SampleFormat.Float32Planar, 44100, 2)));
        Assert.NotNull(sound.Play(output));
    }

    [Fact]
    public void OpenCallback_Int16()
    {
        using var engine = CreateHeadless();
        int calls = 0;
        using var s = engine.OpenCallback(AudioFormat.Int16(44100, 1), buffer =>
        {
            if (calls++ > 3) return AudioEngine.EndOfStream;
            for (int i = 0; i < buffer.Length; i += 2) BitConverter.TryWriteBytes(buffer.Slice(i, 2), (short)16384);
            return buffer.Length / 2;
        }, new StreamOptions { DecodeOnly = true });
        var buf = new float[1024];
        int n = s.Read(buf);
        Assert.True(n > 0);
        Assert.Equal(0.5f, buf[0], 0.001f);
    }

    [Fact]
    public async Task SeekFailed_CarriesTheError()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenCallback(44100, 1, b => { b.Clear(); return b.Length; });
        // A callback stream cannot seek; the async request raises SeekFailed with the reason.
        var got = new TaskCompletionSource<StreamEventArgs>(TaskCreationOptions.RunContinuationsAsynchronously);
        s.SeekFailed += (_, e) => got.TrySetResult(e);
        try { s.RequestSeek(TimeSpan.FromSeconds(1)); } catch (FatzAudioException) { return; }   // refused at once is fine too
        var e = await got.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.NotNull(e.Error);
        Assert.Equal((long)e.Error!.Value, e.Value);
    }
}
