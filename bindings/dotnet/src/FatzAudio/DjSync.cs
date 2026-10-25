using FatzAudio.Native;

namespace FatzAudio;

/// <summary>What <see cref="DjDeck.SyncTo"/> and <see cref="DjDeck.PlaySynced"/> match.</summary>
[Flags]
public enum SyncMode
{
    /// <summary>Nothing (ends a sync).</summary>
    None = 0,
    /// <summary>Match the leader's heard BPM (half or double time when that keeps the deck within <see cref="DjDeck.SyncRange"/>).</summary>
    Tempo = 1,
    /// <summary>Align beats.</summary>
    Beat = 2,
    /// <summary>Align bars (implies <see cref="Beat"/>). Bars are known for exact grids; other grids count beat 0 as a downbeat.</summary>
    Bar = 4,
    /// <summary>Keep it so: follows the leader's tempo moves and tempo maps, about 20 times a second.</summary>
    Lock = 8,
}

/// <summary>A deck's beat position as heard now (<see cref="DjDeck.BeatInfo"/>).</summary>
/// <param name="Bpm">Heard tempo: the grid's tempo where it plays x tempo x rate.</param>
/// <param name="Beat">Heard position in beats (fractional; beat 0 is the grid's first beat).</param>
/// <param name="Bar">Heard position in bars.</param>
/// <param name="PhaseErrorMs">Against the leader while synced with <see cref="SyncMode.Beat"/> or <see cref="SyncMode.Bar"/>
/// (positive = this deck is ahead), else 0.</param>
/// <param name="Locked">Synced with <see cref="SyncMode.Lock"/>.</param>
/// <param name="Synced">Synced to a leader.</param>
public readonly record struct DeckBeatInfo(double Bpm, double Beat, double Bar, double PhaseErrorMs, bool Locked, bool Synced);

public sealed partial class DjDeck
{
    private const int ParamSyncRange = 7;

    /// <summary>
    /// The most sync may move the deck's tempo away from 1: 0.01..1, default 0.16 (+-16%). Auto-mix uses
    /// <see cref="AutoMixOptions.MaxTempoChange"/> instead.
    /// </summary>
    public float SyncRange { get => GetRaw(ParamSyncRange); set => SetRaw(ParamSyncRange, value); }

    /// <summary>
    /// Syncs this deck to <paramref name="leader"/>. <see cref="SyncMode.Tempo"/> sets the stream's tempo (pitch is
    /// kept). <see cref="SyncMode.Beat"/> on a playing deck pulls a small phase error (under 30 ms) in by bending the
    /// tempo by up to 2% for about two beats, and jumps a larger one away on the deck's next beat; on a deck that is
    /// not playing it applies once it plays (or use <see cref="PlaySynced"/>). Without <see cref="SyncMode.Lock"/> it
    /// is done once; with it the deck's tempo moves by itself to stay on the leader's beat. Loading a stream on either
    /// deck ends the sync. Both decks need a beat grid (<see cref="AudioStream.BeatGrid"/>).
    /// </summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when a deck is empty or has no grid;
    /// <see cref="ErrorCode.OutOfRange"/> when the tempo is out of reach (nothing changes).</exception>
    public void SyncTo(DjDeck leader, SyncMode mode = SyncMode.Tempo | SyncMode.Beat)
    {
        ArgumentNullException.ThrowIfNull(leader);
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_sync(_deck, leader._deck, (uint)mode));
    }

    /// <summary>Ends a sync; the tempo stays where it is.</summary>
    public void Unsync()
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_sync(_deck, 0, 0));
    }

    /// <summary>
    /// Starts this deck (not playing) so that its next beat (<see cref="SyncMode.Bar"/>: downbeat) lands exactly on one
    /// of the leader's, at least 50 ms from now: a stopped deck from its <see cref="CuePoint"/>, a paused one from
    /// where it is. <see cref="SyncMode.Tempo"/> matches the tempo first; <see cref="SyncMode.Lock"/> keeps it locked.
    /// A deck that plays already is synced as <see cref="SyncTo"/> does.
    /// </summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.InvalidArgument"/> when the leader is not playing;
    /// <see cref="ErrorCode.NotFound"/> and <see cref="ErrorCode.OutOfRange"/> as for <see cref="SyncTo"/>.</exception>
    public void PlaySynced(DjDeck leader, SyncMode mode = SyncMode.Tempo | SyncMode.Beat)
    {
        ArgumentNullException.ThrowIfNull(leader);
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_play_synced(_deck, leader._deck, (uint)mode));
    }

    /// <summary>
    /// A loop of <paramref name="beats"/> beats on the grid (1/16..32, e.g. 1, 2, 4, 8, 16) from the beat heard now,
    /// or, while a loop is on, from its in point (so halving and doubling keep its start). 0 turns the loop off.
    /// The <see cref="Loop"/> property reads it back in frames.
    /// </summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when the deck is empty or has no grid.</exception>
    public void LoopBeats(double beats)
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_set_loop_beats(_deck, beats));
    }

    /// <summary>
    /// Beat jump: moves the deck <paramref name="beats"/> beats on its grid (negative = back; 1/16..1024 either way,
    /// e.g. 1, 4, 16, 32), at once and seamlessly. Whole beats keep the beat where it was, so a synced deck stays on
    /// the leader's beat. While a loop is on, the loop moves with it. On a paused or stopped deck it moves the
    /// position. A locked deck pulls a jump back unless it is whole beats (whole bars with <see cref="SyncMode.Bar"/>).
    /// </summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when the deck is empty or has no grid;
    /// <see cref="ErrorCode.Busy"/> while the previous jump waits to be decoded (try again a moment later).</exception>
    public void BeatJump(double beats)
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_beat_jump(_deck, beats));
    }

    /// <summary>The beat position heard now; null when the deck is empty or its stream has no beat grid.</summary>
    public unsafe DeckBeatInfo? BeatInfo
    {
        get
        {
            var info = new FaDeckBeatInfo { StructSize = (uint)sizeof(FaDeckBeatInfo) };
            using var _ = _mixer.Lease();
            int r = NativeMethods.fa_deck_get_beat_info(_deck, &info);
            if (r == (int)ErrorCode.NotFound) return null;
            FatzAudioException.ThrowIfFailed(r);
            return new DeckBeatInfo(info.Bpm, info.Beat, info.Bar, info.PhaseErrorMs, info.Locked != 0, info.Synced != 0);
        }
    }
}

/// <summary>Options for <see cref="DjAutoMix.Start"/>.</summary>
public sealed class AutoMixOptions
{
    /// <summary>The transition's length, 0.5..60 s (default 8 s).</summary>
    public TimeSpan Length { get; init; } = TimeSpan.FromSeconds(8);

    /// <summary>The length in beats instead (1..256, e.g. 32) when both decks have good grids; 0 = use <see cref="Length"/>.</summary>
    public float LengthBeats { get; init; }

    /// <summary>Bring the incoming deck to the outgoing BPM, within <see cref="MaxTempoChange"/> (default true).</summary>
    public bool MatchTempo { get; init; } = true;

    /// <summary>Start on a bar, length in whole bars, when both grids are good (default true).</summary>
    public bool AlignBeats { get; init; } = true;

    /// <summary>The most <see cref="MatchTempo"/> moves the tempo from 1, 0..0.5 (default 0.08).</summary>
    public float MaxTempoChange { get; init; } = 0.08f;

    /// <summary>Grids below this confidence count as unknown, 0..1 (default 0.5).</summary>
    public float MinConfidence { get; init; } = 0.5f;

    /// <summary>
    /// Bass swap (default false): the incoming deck's low band is killed until the middle of the transition, then the
    /// two decks' lows swap over one beat (0.5 s when not beat-aligned). Auto-mix owns both decks'
    /// <see cref="DeckEq.Low"/> meanwhile and puts them back afterwards.
    /// </summary>
    public bool BassSwap { get; init; }

    /// <summary>
    /// After the transition, an incoming deck whose tempo <see cref="MatchTempo"/> moved glides back to its own
    /// tempo over <see cref="RestoreTime"/> (default false). Setting the tempo, a sync with <see cref="SyncMode.Tempo"/> or a new stream takes over from the glide.
    /// </summary>
    public bool RestoreTempo { get; init; }

    /// <summary>How long the <see cref="RestoreTempo"/> glide takes, 0..300 s (default 20 s; 0 = at once).</summary>
    public TimeSpan RestoreTime { get; init; } = TimeSpan.FromSeconds(20);
}

/// <summary>A deck in a <see cref="DjAutoMix"/> event.</summary>
public sealed class DeckEventArgs : EventArgs
{
    internal DeckEventArgs(DjDeck deck) => Deck = deck;

    /// <summary>The incoming deck (<see cref="DjAutoMix.Started"/>, <see cref="DjAutoMix.Done"/>), or the idle one (<see cref="DjAutoMix.DeckEmpty"/>).</summary>
    public DjDeck Deck { get; }
}

/// <summary>
/// Auto-mix: the two decks take turns. While a deck plays, the other starts at the playing one's mix point (its
/// <see cref="StreamMarkers.MixPoint"/>, else its fade-out start, else its end minus the length) from its cue
/// point, and the crossfader moves across, all scheduled on the device clock. Beat-aligned when both grids are good
/// and the tempos are within reach, otherwise a plain timed crossfade: it never fails unattended. The outgoing deck
/// is stopped when the transition is done. The app owns the playlist: load the next song in <see cref="DeckEmpty"/>.
/// Events are raised on the engine's event thread.
/// </summary>
/// <example>
/// <code>
/// mixer.AutoMix.DeckEmpty += (s, e) => e.Deck.Load(engine.OpenFile(queue.Dequeue()), force: true);
/// mixer.AutoMix.Start(new AutoMixOptions { LengthBeats = 32 });
/// mixer.AutoMix.Skip();   // the next transition now
/// </code>
/// </example>
public sealed class DjAutoMix
{
    private const int EvStarted = 18, EvDone = 19, EvDeckEmpty = 20;
    private readonly DjMixer _mixer;

    internal DjAutoMix(DjMixer mixer) => _mixer = mixer;

    /// <summary>Whether auto-mix is on.</summary>
    public bool IsOn { get; private set; }

    /// <summary>Turns auto-mix on (or changes its options for the next transition).</summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.InvalidArgument"/> for options out of range.</exception>
    public unsafe void Start(AutoMixOptions? options = null)
    {
        var o = options ?? new AutoMixOptions();
        var n = new FaAutomixOptions();
        NativeMethods.fa_automix_options_init(&n);
        n.Length = TimeConvert.ToNanoseconds(o.Length);
        n.LengthBeats = o.LengthBeats;
        n.Flags = (o.MatchTempo ? 1u : 0u) | (o.AlignBeats ? 2u : 0u) | (o.BassSwap ? 4u : 0u) | (o.RestoreTempo ? 8u : 0u);
        n.RestoreTime = TimeConvert.ToNanoseconds(o.RestoreTime);
        n.MaxTempoChange = o.MaxTempoChange;
        n.MinConfidence = o.MinConfidence;
        using (_mixer.Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_automix(_mixer.Native, &n));
        IsOn = true;
    }

    /// <summary>Turns auto-mix off; a transition already scheduled finishes, without events.</summary>
    public unsafe void Stop()
    {
        using (_mixer.Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_automix(_mixer.Native, null));
        IsOn = false;
    }

    /// <summary>Starts the next transition now (on the next bar when beat-aligned).</summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when auto-mix is off, no deck plays or the
    /// other deck has nothing new; <see cref="ErrorCode.Busy"/> while a transition runs.</exception>
    public void Skip()
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_automix_now(_mixer.Native));
    }

    /// <summary>The incoming deck is heard.</summary>
    public event EventHandler<DeckEventArgs>? Started;

    /// <summary>The transition is done (the crossfader is across).</summary>
    public event EventHandler<DeckEventArgs>? Done;

    /// <summary>
    /// The idle deck needs its next song: when a transition is done, and again 30 s before the next mix point while
    /// it still holds the song mixed out (or nothing).
    /// </summary>
    public event EventHandler<DeckEventArgs>? DeckEmpty;

    internal bool Raise(int type, uint index)
    {
        var h = type switch { EvStarted => Started, EvDone => Done, EvDeckEmpty => DeckEmpty, _ => null };
        if (type is not (EvStarted or EvDone or EvDeckEmpty)) return false;
        if (index >= _mixer.Decks.Count) return true;
        try
        {
            h?.Invoke(_mixer, new DeckEventArgs(_mixer.Decks[(int)index]));
        }
        catch
        {
            // a handler's exception must not stop the event thread
        }
        return true;
    }
}

public sealed partial class DjMixer
{
    /// <summary>Auto-mix between the two decks.</summary>
    public DjAutoMix AutoMix { get; }
}
