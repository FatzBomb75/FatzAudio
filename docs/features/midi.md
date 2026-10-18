# MIDI and karaoke files

FatzAudio plays MIDI files itself, with a built-in SoundFont 2 synthesizer. They open with the ordinary `fa_stream_open_*` calls and play, seek, loop, crossfade, normalize and stretch like any other stream. It replaces bassmidi.

| What | Supported |
|---|---|
| Standard MIDI Files (`.mid`, `.midi`, `.smf`) | Format 0, 1 and 2, tempo maps, SMPTE time division, running status |
| Karaoke (`.kar`) | Lyrics from the "Words" track, `@T` title and artist lines, line and paragraph marks |
| RIFF MIDI (`.rmi`) | The Standard MIDI File inside it |
| Instruments | A SoundFont 2 (`.sf2`) or DLS (`.dls`) bank you load, else the one the OS ships (Windows `gm.dls`, macOS `gs_instruments.dls`, a Linux GM `.sf2` package): General MIDI; GS/XG variation banks through bank select (falling back to bank 0 when the soundfont lacks them); drums on channel 10 |
| Lyrics and other text | Lyric, text, marker, cue, track and instrument name, copyright: listed and raised as events |
| Live controls | Transpose (the synthesizer plays the new notes), channel mute, plus every stream parameter (volume, tempo, pitch) |

Files are detected from their content (`MThd`, or `RIFF....RMID`), never from the extension. `fa_stream_get_info` reports codec `midi`, container `smf` or `rmid`, and stereo F32 audio at the engine's `mix_sample_rate`, or 44100 Hz when that is 0 (device native).

## Soundfonts

No instruments are built in: you load a bank once and share it. A General MIDI SoundFont 2 such as GeneralUser GS or FluidR3_GM covers almost every file, and DLS banks (`.dls`, Level 1 and 2) load the same way. Set it on the engine and every MIDI stream opened from then on uses it, or give one stream its own in `fa_open_options.soundfont`.

### The OS's instruments

When the app sets no soundfont, a MIDI file plays with the bank the operating system ships for its own MIDI synthesizer, so MIDI works out of the box where BASSMIDI needed a soundfont:

| System | Bank |
|---|---|
| Windows | `%SystemRoot%\System32\drivers\gm.dls` (the Microsoft GS Wavetable set, on every Windows since 98) |
| macOS | `gs_instruments.dls` in `/System/Library/Components/CoreAudio.component/Contents/Resources` |
| Linux | The first of `default-GM.sf2`, `default.sf2`, `FluidR3_GM.sf2`, `TimGM6mb.sf2` and `FluidR3_GS.sf2` in `/usr/share/sounds/sf2` or `/usr/share/soundfonts` (packages such as `fluid-soundfont-gm` or `timgm6mb-soundfont`) |
| Android, iOS | None: load a soundfont |

The engine loads it the first time a MIDI stream or loudness scan needs it (gm.dls takes a moment: its 3.4 MB are converted and held as about 7 MB of samples) and shares it with every later one; other formats never load it. A soundfont the app sets always wins. `FA_ENGINE_NO_SYSTEM_SOUNDFONT` in `fa_engine_config.flags` (.NET `AudioEngineOptions.NoSystemSoundFont`) turns the fallback off, and then a MIDI file without a soundfont fails with `FA_ERROR_NO_SOUNDFONT`, as it also does where the OS has none. `fa_soundfont_open_system` (.NET `SoundFont.FromSystem()`) loads the OS's bank yourself, for example to show its name or check that there is one.

The OS banks are small and General MIDI only: gm.dls sounds like the Windows MIDI player, which is fine for karaoke files and background music. For better instruments, load a bigger SoundFont 2.

```c
fa_soundfont* sys = NULL;
if (fa_soundfont_open_system(&sys) == FA_OK) {
    fa_soundfont_info info;
    fa_soundfont_get_info(sys, &info);          /* e.g. "Microsoft GS Wavetable" */
    fa_soundfont_close(sys);
}                                               /* FA_ERROR_NOT_FOUND: this OS has none */

fa_engine_config cfg;
fa_engine_config_init(&cfg);
cfg.flags = FA_ENGINE_NO_SYSTEM_SOUNDFONT;      /* only the app's soundfonts */
```

```csharp
using var sys = SoundFont.FromSystem();        // null where the OS has none
Console.WriteLine(sys?.Info.Name);
using var strict = AudioEngine.Create(new AudioEngineOptions { NoSystemSoundFont = true });
```

A soundfont is reference-counted. The engine and every stream that uses it hold their own reference, so you can close yours right after setting it, and replace the engine's at any time (open streams keep theirs). It's freed when the last user is done. Its samples are kept in memory as 32-bit float, about twice the size of the `.sf2` file (DLS banks' 8-bit samples take four times their size); `fa_soundfont_get_info` gives the figure.

Unlike BASSMIDI there are no font handles per preset, no font stacks and no sample loading on demand: one call loads the whole bank, and it's ready.

### C

```c
fa_soundfont* gm = NULL;
if (fa_soundfont_open_path("GeneralUser.sf2", &gm) != FA_OK) { /* missing or not SF2 */ }
fa_engine_set_soundfont(engine, gm);
fa_soundfont_close(gm);                  /* the engine keeps its own reference */

fa_stream* song = NULL;
fa_stream_open_path(engine, "song.kar", NULL, &song);
fa_stream_attach(song, device, NULL);
fa_stream_play(song);

/* One stream with other instruments: */
fa_soundfont* piano = NULL;
fa_soundfont_open_path("piano.sf2", &piano);
fa_open_options o;
fa_open_options_init(&o);
o.soundfont = piano;
fa_stream_open_path(engine, "etude.mid", &o, &etude);
fa_soundfont_close(piano);               /* etude keeps it */
```

### C#

```csharp
using (var gm = SoundFont.FromFile("GeneralUser.sf2"))
    engine.DefaultSoundFont = gm;        // the engine keeps its own reference
Console.WriteLine(engine.DefaultSoundFont!.Info.Name);

using var song = engine.OpenFile("song.kar");
song.PlayOn(output);

using var piano = SoundFont.FromFile("piano.sf2");
using var etude = engine.OpenFile("etude.mid", new StreamOptions { SoundFont = piano });
```

## Lyrics and text events

Every text event of the file is listed in time order by `fa_stream_get_midi_text` (`AudioStream.MidiTexts`), with its frame, time, type, track and UTF-8 text. While the stream plays on a device, `FA_EVENT_MIDI_TEXT` (`AudioStream.MidiTextReached`) is raised as each one is heard, with `value` = its index.

Lyrics come cleaned up for display. The marks karaoke files use for layout (`/` and `\` before a syllable in `.kar` files; CR and LF in lyric events) are removed from the text and given as `FA_MIDI_NEW_LINE` and `FA_MIDI_NEW_PARAGRAPH` flags on the syllable that starts the new line. A karaoke screen joins the syllables of a line as they are and breaks before the flagged ones. In a `.kar` file the words are `FA_MIDI_LYRIC` and its `@` header lines are `FA_MIDI_TEXT`; `fa_stream_get_midi_info` gives the title and artist from the `@T` lines, and the copyright. Text that isn't valid UTF-8 is read as Windows-1252 (Latin-1), which is what most older files use.

### C

```c
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    if (ev->type != FA_EVENT_MIDI_TEXT) return;
    fa_midi_text t;
    fa_stream_get_midi_text(ev->stream, (uint32_t)ev->value, &t);
    if (t.type == FA_MIDI_LYRIC)
        show_syllable(t.text, (t.flags & FA_MIDI_NEW_LINE) != 0);
}

fa_midi_info info;
fa_stream_get_midi_info(song, &info);
printf("%s - %s, %u syllables\n", info.artist, info.title, info.lyric_count);
fa_stream_set_event_callback(song, on_event, NULL);
```

### C#

```csharp
var info = song.MidiInfo!;                       // null for streams that aren't MIDI
Console.WriteLine($"{info.Artist} - {info.Title}");

// The whole text up front, one line per NewLine mark:
var line = new StringBuilder();
foreach (var t in song.MidiTexts.Where(t => t.Type == MidiTextType.Lyric))
{
    if (t.Flags.HasFlag(MidiTextFlags.NewLine)) { Console.WriteLine(line); line.Clear(); }
    line.Append(t.Text);
}

// As it is sung:
song.MidiTextReached += (_, e) =>
{
    if (e.Text.Type == MidiTextType.Lyric) HighlightSyllable(e.Text.Index);
};
```

Text events are best effort, like markers: under heavy load an event can be dropped. A screen that must never miss a syllable follows the stream's presentation clock (`fa_stream_get_clock`, `AudioStream.Clock`) through the list, which also keeps it in sync with the speaker latency.

## Live controls

| Control | C | C# | Notes |
|---|---|---|---|
| Transpose | `fa_stream_set_midi_transpose(s, semitones)` | `MidiTranspose` | -48..48 semitones on every channel but the drums, from the next note on. The synthesizer plays the new notes, so the sound stays as clean as the original. |
| Channel mute | `fa_stream_set_midi_channel_mute(s, channel, 1)` | `SetMidiChannelMuted(channel, true)` | Channel 0..15 (MIDI 1..16) goes silent at once; for a karaoke "guide melody off". |
| Master volume | `FA_PARAM_VOLUME` | `Volume` | As for every stream, with ramps. |
| Tempo | `FA_PARAM_TEMPO` | `Tempo` | The stream's time-stretch: lyrics, markers and the clock follow. |
| Key without resynthesis | `FA_PARAM_PITCH` | `Pitch` | Shifts the rendered audio, drums included. Prefer transpose for MIDI. |

Transpose and mute change the audio where it's rendered, so they're heard after the decode-ahead buffer. MIDI streams default to a 150 ms buffer (instead of 500 ms) to keep that short; set `buffer_ms` to change it. They may be called from any thread.

```c
fa_stream_set_midi_transpose(song, -2);          /* down a whole tone */
fa_stream_set_midi_channel_mute(song, 3, 1);     /* guide melody on MIDI channel 4 off */
```

```csharp
song.MidiTranspose = -2;
song.SetMidiChannelMuted(3, true);
```

## Seeking and length

Seeking is sample-exact and fast: the events up to the target are replayed into a fresh synthesizer without rendering any audio, so programs, controllers, pitch bends and the tempo are exactly as if the song had played up to there. Notes held across the target aren't restarted: a chord that started before the target sounds again from its next note-on (as in most MIDI players).

The length is the time of the last event plus one second, so the final notes ring out. Audio from a given position is the same whatever the read size or the seeks before it, so loudness scans are cached like those of any other file (the cache key includes the soundfont and the sample rate).

## Threads

Rendering runs on the engine's worker threads like decoding of any other format. Soundfonts may be opened, closed and set from any thread, and shared by any number of streams and engines.

## Limits and pitfalls

- MIDI files up to 16 MB; events later than 6 hours are dropped. Soundfonts up to 1 GB.
- 256 voices per stream. When a dense passage needs more, the voices furthest into their release are cut; if all 256 are held, the new notes are skipped.
- Damaged files play what can be read: a track cut short plays up to the cut. A file with no usable track fails with `FA_ERROR_UNSUPPORTED_FORMAT`.
- DLS banks play their regions, samples, loops, envelopes, LFOs, filter and gains. Articulation scaled by a controller (other than the default mod wheel vibrato) and DLS 2 conditional chunks are ignored.
- Each instrument's level (`initialAttenuation`) is applied as the SoundFont 2 spec states it, 0.1 dB per unit. FluidSynth applies 0.4 of that (an EMU hardware habit), so a bank tuned by ear in FluidSynth can sound with a wider gap between its quiet and loud instruments here.
- SF3 (compressed samples) and SFZ soundfonts, MIDI input from devices, and per-channel effects (reverb and chorus send levels) aren't supported yet. See the [engineering notes](../engineering-notes.md).
- Shift-JIS lyrics (Japanese files) are read as Windows-1252 and come out garbled.
- Drum parts on channels other than 10 (set by GS SysEx, or XG bank 127) play as melodic instruments: only channel 10 plays drums. SysEx is ignored altogether.
