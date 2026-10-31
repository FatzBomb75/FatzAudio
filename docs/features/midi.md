# MIDI and karaoke files

FatzAudio plays MIDI files itself, with a built-in SoundFont 2 synthesizer. They open with the ordinary `fa_stream_open_*` calls and play, seek, loop, crossfade, normalize and stretch like any other stream. It replaces bassmidi.

| What | Supported |
|---|---|
| Standard MIDI Files (`.mid`, `.midi`, `.smf`) | Format 0, 1 and 2, tempo maps, SMPTE time division, running status |
| Karaoke (`.kar`) | Lyrics from the "Words" track, `@T` title and artist lines, line and paragraph marks |
| RIFF MIDI (`.rmi`) | The Standard MIDI File inside it |
| Instruments | A SoundFont 2 (`.sf2`, also SF3 with compressed samples) or DLS (`.dls`) bank you load, else the one the OS ships (Windows `gm.dls`, macOS `gs_instruments.dls`, a Linux GM `.sf2` package): General MIDI; GS/XG variation banks through bank select (falling back to bank 0 when the soundfont lacks them); drums on channel 10 and on the parts GS, XG and GM2 files make drum parts |
| SysEx | GM, GM2, GS and XG resets, GS rhythm parts, XG drum parts and drum banks (127, and 120 in GM2), master volume and master tuning, GS and XG reverb and chorus settings, part volume, pan and effect sends, scale tuning, GS drum maps and XG drum setups ([below](#sysex)) |
| Lyrics and other text | Lyric, text, marker, cue, track and instrument name, copyright: listed and raised as events |
| Tempo and signatures | Tempo changes, time and key signatures, listed with their numbers |
| Effects | Reverb and chorus, on by default: the soundfont's effect sends plus CC91 and CC93; the file's GS or XG SysEx picks their type, level and time ([below](#reverb-and-chorus)) |
| Live controls | Transpose (the synthesizer plays the new notes), channel mute, plus every stream parameter (volume, tempo, pitch) |

Files are detected from their content (`MThd`, or `RIFF....RMID`), never from the extension. `fa_stream_get_info` reports codec `midi`, container `smf` or `rmid`, and stereo F32 audio at the engine's `mix_sample_rate`, or 44100 Hz when that is 0 (device native).

## Soundfonts

No instruments are built in: you load a bank once and share it. A General MIDI SoundFont 2 such as GeneralUser GS or FluidR3_GM covers almost every file, and DLS banks (`.dls`, Level 1 and 2) load the same way. So do SF3 files, SoundFont 2 files with Ogg Vorbis compressed samples (the format MuseScore ships): the samples are decoded at load by the library's own Vorbis decoder, so a loaded SF3 takes as much memory as the same bank as `.sf2` (many times the size of the `.sf3` file). A sample that fails to decode plays as silence instead of failing the whole bank. SFZ is not supported. Set it on the engine and every MIDI stream opened from then on uses it, or give one stream its own in `fa_open_options.soundfont`.

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
/* fa_soundfont_open_memory(data, size, &gm) loads a bank you already hold in memory; it copies the bytes */
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

Every text event of the file is listed in time order by `fa_stream_get_midi_text` (`AudioStream.MidiTexts`), with its frame, time, type, track and UTF-8 text. While the stream plays on a device, `FA_EVENT_MIDI_TEXT` (`AudioStream.MidiTextReached`) is raised as each one is heard, with `value` = its index. In [reverse playback](tempo-pitch.md#reverse-playback) they come last first, as the position counts down past them.

Lyrics come cleaned up for display. The marks karaoke files use for layout (`/` and `\` before a syllable in `.kar` files; CR and LF in lyric events) are removed from the text and given as `FA_MIDI_NEW_LINE` and `FA_MIDI_NEW_PARAGRAPH` flags on the syllable that starts the new line. A karaoke screen joins the syllables of a line as they are and breaks before the flagged ones. In a `.kar` file the words are `FA_MIDI_LYRIC` and its `@` header lines are `FA_MIDI_TEXT`; `fa_stream_get_midi_info` gives the title and artist from the `@T` lines, and the copyright.

To show the lyrics on screen without drawing them yourself, use the [MIDI lyric surface](lyrics.md): it builds lines and pages from these marks and the gaps between syllables and draws them with the sung syllables coloured in.

A few more clean-ups make old karaoke files display well:

- In `.kar` files a `/` or `\` anywhere in a word event starts a new line or paragraph there (the event is split into two syllables), and a leading `<` (the Soft Karaoke "clear screen" mark) starts a paragraph.
- Chord lines (Tune 1000 lines starting with `%`, and lines made of `<...>` chord names) are given as `FA_MIDI_TEXT`, not lyrics.
- Some files put the whole song in text events and only a handful of words in lyric events. When one track's text events look like syllables (at least 16 short events, four times as many as the file's lyric events, and at least 60% of that track's text events) they are used as the lyrics, and the stray lyric events become `FA_MIDI_TEXT`.

### Text encoding

MIDI files don't say which character set their text uses. FatzAudio picks one per file: UTF-8 when all the text is valid UTF-8 (a byte-order mark is removed); Shift-JIS (code page 932, Japanese) when every non-ASCII byte forms a valid Shift-JIS pair and enough of them are kana or Japanese punctuation; otherwise Windows-1252 (Latin-1), which is what most older Western files use. All text comes out as UTF-8 either way. Files of mixed origin are common, so with UTF-8 (detected or forced) and with detected Windows-1252, each event that is valid UTF-8 is kept as UTF-8 and the others are read as Windows-1252; forced Windows-1252 and Shift-JIS read every event that way. When the guess is wrong, force the code page with `fa_open_options.text_code_page` (`StreamOptions.TextCodePage`): `FA_CODE_PAGE_UTF8`, `FA_CODE_PAGE_WINDOWS_1252` or `FA_CODE_PAGE_SHIFT_JIS`; 0 (the default) detects. Shift-JIS characters with no Unicode mapping (such as user-defined ones) come out as U+FFFD. The code page used is reported in `fa_midi_credits.code_page`.

```c
fa_open_options o = { sizeof(o) };
o.text_code_page = FA_CODE_PAGE_SHIFT_JIS;     /* this one is Japanese */
fa_stream* song = NULL;
fa_stream_open_path(engine, "enka.kar", &o, &song);
```

```csharp
using var song = engine.OpenFile("enka.kar", new StreamOptions { TextCodePage = TextCodePage.ShiftJis });
```

### Song credits

`fa_stream_get_midi_credits` (`AudioStream.MidiCredits`) gathers the song's credits from the header text that karaoke files carry: the `@T` lines of `.kar` files and their `@I` information lines, labelled (`@ILyrics by ...`, `@IMusic by: ...`, `@IYear=1984`, `@IKey - G`) or not. Each field is UTF-8 and empty when the file doesn't give it:

| Field | From |
|---|---|
| `title`, `artist` | The first and second `@T` line; else a `Title` / `Song` / `Artist` / `Performer` / `Performed by` label; else the first track name for the title |
| `writer` | `Written by`, `Writer`, `Composer`, `Music by`, `Words and music by` labels |
| `lyricist` | `Lyrics by`, `Lyricist`, `Lyrics`, `Words and music by` labels |
| `year` | A `Year` / `Date` label, or an `@I` line that is just a year (1800-2200) |
| `copyright` | The copyright event, else an `@I` line or the third `@T` line containing `(c)`, `©` or `Copyright` |
| `key`, `tempo`, `time_signature` | The file's first key signature, tempo and time signature (as in `fa_stream_get_midi_marks`), which is what plays; else a `Key` / `Tempo` / `BPM` / `Time signature` / `Timing` / `Meter` label |
| `other` | Every other `@I` line and `@T` lines after the second (or third, when that is the copyright), one per line, without repeats |

```c
fa_midi_credits c = { sizeof(c) };
if (fa_stream_get_midi_credits(song, &c) == FA_OK)
    printf("%s - %s (%s), words %s, music %s\n", c.artist, c.title, c.year, c.lyricist, c.writer);
```

```csharp
if (song.MidiCredits is { } c)
    Console.WriteLine($"{c.Artist} - {c.Title} ({c.Year}), words {c.Lyricist}, music {c.Writer}");
```

`fa_stream_get_midi_credits` returns `FA_ERROR_INVALID_ARG` for a stream that isn't MIDI (and `MidiCredits` is `null`).

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

### Tempo, time and key signatures

The file's tempo changes and time and key signatures are listed apart from the texts, by `fa_stream_get_midi_marks` (`AudioStream.MidiMarks`), in time order with their numbers in `value` and `value2` and a readable text:

| Type | `value` | `value2` | `text` |
|---|---|---|---|
| `FA_MIDI_MARK_TEMPO` (`MidiMarkType.Tempo`) | microseconds per quarter note (500000) | 0 | `120 bpm` |
| `FA_MIDI_MARK_TIME_SIGNATURE` | numerator (3) | denominator (4) | `3/4` |
| `FA_MIDI_MARK_KEY_SIGNATURE` | sharps, negative for flats (-1) | 1 for minor, 0 for major | `D minor` |

They aren't raised as events; follow the clock through the list to show the bar or key heard now. `FA_PARAM_TEMPO` stretches the stream on top of the file's own tempo, and the frames and times in the list stay those of the file. Malformed signature events are left out.

```c
uint32_t n = 0;
fa_stream_get_midi_marks(song, NULL, 0, &n);              /* how many */
fa_midi_mark* marks = calloc(n ? n : 1, sizeof(*marks));
fa_stream_get_midi_marks(song, marks, n, &n);
for (uint32_t i = 0; i < n; i++) printf("%.2f s: %s\n", marks[i].time / 1e9, marks[i].text);
free(marks);
```

```csharp
var tempo = song.MidiMarks.FirstOrDefault(m => m.Type == MidiMarkType.Tempo);
double bpm = tempo is null ? 120 : 60_000_000.0 / tempo.Value;
```

Text events are best effort, like markers: under heavy load an event can be dropped. A screen that must never miss a syllable follows the stream's presentation clock (`fa_stream_get_clock`, `AudioStream.Clock`) through the list, which also keeps it in sync with the speaker latency.

## Live controls

| Control | C | C# | Notes |
|---|---|---|---|
| Transpose | `fa_stream_set_midi_transpose(s, semitones)` / `fa_stream_get_midi_transpose` | `MidiTranspose` | -48..48 semitones on every channel but the drums, from the next note on. The synthesizer plays the new notes, so the sound stays as clean as the original. |
| Channel mute | `fa_stream_set_midi_channel_mute(s, channel, 1)` / `fa_stream_get_midi_channel_mute` | `SetMidiChannelMuted(channel, true)` / `IsMidiChannelMuted(channel)` | Channel 0..15 (MIDI 1..16) goes silent at once; for a karaoke "guide melody off". |
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

## Controllers and modulators

The file's controllers shape each note the way a GM, GS or XG module would. Nothing needs setting up: they are part of the file. Files that use none of the controllers below sound exactly as they did before these were added.

| Controller | What it does |
|---|---|
| Mod wheel (CC1) | Vibrato, up to ±50 cents at 127, the SoundFont 2 default. RPN 5 (modulation depth range) sets that maximum: data entry MSB in semitones, LSB in 1/128 semitones |
| Channel pressure | The same vibrato, ±50 cents at full pressure. After a GS, XG or GM2 reset it does nothing, as on those modules (BASSMIDI does the same for GS) |
| Poly pressure | Kept per key, for soundfonts whose modulators use it; no effect of its own |
| Brightness (CC74), resonance (CC71) | 64 is the instrument as it is. CC74 moves the filter cutoff 100 cents a step: 0 closes an open filter to about 500 Hz, and above 64 it can only open a filter the instrument closes. CC71 adds or takes 0.3 dB of resonance a step |
| Attack (CC73), decay (CC75), release (CC72) | 64 is the instrument's own time; each step lengthens or shortens it by 1/12 octave (127 is about 38 times as long, 0 about 40 times as short). A time that starts instant is first lifted to 1/64 s, so CC73 = 127 turns a piano's instant attack into a 0.6 s fade-in. They act on both of the instrument's envelopes; a change reaches notes already playing from their next stage on (a new release time applies to held notes when they are let go) |
| Vibrato rate, depth, delay (CC76, CC77, CC78) | Rate: 1/32 octave a step (127 about 4 times as fast). Depth: 1 cent a step on top of the instrument's own vibrato and the mod wheel's (toward 0, not past it). Delay: as the envelope times, for notes started after the change |
| Portamento (CC65), portamento time (CC5), portamento control (CC84) | With CC65 on, each note glides from the channel's previous key to its own, in a time CC5 sets: 5 ms at 0, 0.18 s at 64, 5.9 s at 127, whatever the interval. CC84 names the key the next note glides from, once, also with CC65 off |
| Sostenuto (CC66) | Holds the notes whose keys are down when it is pressed, after their keys are let go, until it is released; notes started later are not held. With the sustain pedal too, a note ends when neither holds it |
| Soft pedal (CC67) | Notes struck while it is down play 3 dB softer and a little darker (cutoff 1200 cents lower) |
| GS and XG part NRPNs (`01 08`, `01 09`, `01 0A`, `01 20`, `01 21`, `01 63`, `01 64`, `01 66`) | The same as CC76, CC77, CC78, CC74, CC71, CC73, CC75 and CC72 (data entry MSB; 64 is normal) |
| GS part SysEx `40 1x 30`-`37`, XG multi part `08 nn 15`-`1C` | The same values again: vibrato rate, depth and delay, cutoff, resonance, attack, decay and release |

CC121 (reset all controllers) sets the mod wheel, pressure, portamento, sostenuto and soft pedal back to off and RPN 5 to 50 cents, and ends any NRPN selection, as MIDI's RP-015 says; the sound controllers CC71-78 keep their values. A GM, GS or XG reset sets everything back, CC71-78 to 64 and the portamento time to 0 included. A seek replays all of it, so the controllers in force at the target are applied to what plays from there, and a note held only by sostenuto at the target is restarted and held by it.

Roland and Yamaha don't publish how far each step goes, so the steps above are close approximations (the same ones for GS and XG files), not a copy of a particular module.

### Soundfont modulators

A soundfont's own modulators (`pmod` and `imod`, SoundFont 2.04) are applied: any controller, velocity, key, pressure or the pitch wheel, through linear, concave, convex or switch curves, with a second source scaling the first, to the vibrato and modulation LFOs (depth, rate, delay), the modulation envelope's pitch and filter depth, filter cutoff and resonance, both envelopes' times and sustain, level, pan, fine and coarse tune and the reverb and chorus sends. They are evaluated when a note starts and again whenever one of their controllers changes. The modulators of an instrument's zones replace the defaults and those of its global zone, and a preset's add to its instrument's, as the spec says.

Two SoundFont 2.04 defaults work as modulators here, so a soundfont can change or turn them off: the mod wheel and channel pressure to vibrato above. The others are built in and keep their fixed amounts: velocity, CC7 and CC11 to level, CC10 to pan, CC91 and CC93 to the effect sends, the pitch wheel; a soundfont modulator identical to one of them is skipped, so a soundfont can't, for example, raise every channel's reverb send fivefold. The default velocity-to-filter modulator is not applied, as in BASSMIDI (which calls it "slightly strange"): it would darken every softly played note of every file. A soundfont that wants it can say so with its own modulator. Not supported: linked modulators (one modulator feeding another), and destinations TinySoundFont doesn't model (sample offsets, key and velocity overrides, scale tuning, exclusive class).

## Reverb and chorus

Reverb and chorus are built in and on by default, as in BASSMIDI. Each voice sends part of its sound to them: the soundfont's own effect-send setting for that sound, plus the channel's CC91 (reverb) and CC93 (chorus) controllers, as the SoundFont 2.04 default modulators say (a controller value of 127 adds 20%, and the total is at most 100%). Channels start at CC91 = 40 and CC93 = 0, the GS and GM2 defaults, also after a GM, GS or XG reset; CC121 (reset all controllers) leaves them alone. A file that sets CC91 or CC93 therefore changes its own reverb and chorus. The master volume SysEx scales the effects too.

The reverb is a Schroeder-Moorer design of comb and allpass filters that dies away in about 1.3 seconds (to -60 dB). The chorus is two delay taps per channel, modulated at 0.4 Hz between 10 and 16 ms. That is how they sound unless the file says otherwise, and again after every GM, GS or XG reset; it is close to a GS module's default Hall 2 reverb and Chorus 3.

GS and XG files can set the effects with SysEx, and FatzAudio follows them (many karaoke files do this):

| Setting | GS (`40 01 xx`) | XG (`02 01 xx`) | What it does here |
|---|---|---|---|
| Reverb type | Macro `30`, character `31`: Room 1-3, Hall 1-2, Plate, Delay, Panning Delay | Type `00`/`01`: Hall 1-2, Room 1-3, Stage 1-2, Plate, White Room, Tunnel, Canyon, Basement, or no effect | Each room, hall and plate type has its own decay time and brightness. Delay is a feedback echo; Panning Delay bounces it between left and right |
| Reverb time | `34` | `02` (0.3 to 30 seconds) | GS: 64 is the type's own time, each 32 steps doubles or halves it. Delay types: the echo time, 3.3 ms a step (up to 0.42 s) |
| Reverb level | `33` | Return `0C` | 64 is the default level, 0 is none, 127 about +6 dB |
| Reverb pre-LPF, delay feedback | `32`, `35` | | Darkens what goes into the reverb; the delay types' repeats |
| Reverb pre-delay | `37` | | 0 to 127 ms before the reverb starts (room, hall and plate types). The macro `30` sets it back to 0 |
| Chorus type | Macro `38`: Chorus 1-4, Feedback Chorus, Flanger, Short Delay, Short Delay (FB) | Type `20`/`21`: Chorus, Celeste, Flanger (the later types play as a chorus), or no effect | Sets the delay, depth, rate and feedback below |
| Chorus level | `3A` | Return `2C` | As the reverb level |
| Chorus pre-LPF, feedback, delay, rate, depth | `39`, `3B`, `3C`, `3D`, `3E` | | Delay about 1 to 50 ms (80 is 13 ms), rate up to 10 Hz (3 is 0.4 Hz), depth up to 20 ms (19 is 3 ms) |
| Chorus LFO frequency, depth, feedback, delay offset | | `22`, `23`, `24`, `25` | Frequency 0 to 39.7 Hz (64 is 2.7 Hz; above that the steps double every 12 values), depth up to 5 ms, feedback -63 to +63 (64 is none), delay offset 0 to 12.7 ms (0.1 ms a step: the shortest delay the LFO reaches). A new chorus type puts its own values back |
| Chorus send to reverb | `3F` | `2E` | The chorus's output goes on into the reverb (0 is none, the default) |
| Chorus send to delay | `40` | | The chorus's output goes on into the delay below |

Messages that carry several data bytes set consecutive addresses, as on the hardware. Roland and Yamaha don't publish the exact curves, so times and ranges are close approximations rather than a copy of a particular module.

### Delay (GS) and variation (XG)

After a GS reset there is a third effect, the SC-88's delay; after an XG reset, the XG variation effect. A file without a reset (GM) or after a GM or GM2 reset has neither, unless it sends delay or variation SysEx itself, so CC94 does nothing there, as on a GM module.

**GS delay.** CC94 (or the part's delay send `40 1x 2C`, the same value) sends a channel to it, 0 by default. The delay has a center tap, heard on both sides, and a left and a right tap at a ratio of the center time; the center tap repeats with the feedback.

| Setting | GS (`40 01 xx`) | What it does here |
|---|---|---|
| Delay macro | `50` | Delay 1-4, Pan Delay 1-4, Delay to Reverb, Pan Repeat: sets the parameters below. Delay 1 (340 ms, one repeat at a quarter) after a GS reset |
| Pre-LPF | `51` | Darkens what goes in (0-7) |
| Time center | `52` | 0.1 ms to 1 s, in steps that grow with the time (0.1 ms below 2 ms, 20 ms between 200 and 500 ms) |
| Time ratio left, right | `53`, `54` | 4% to 500% of the center time (24 is 100%), at most 1 s |
| Level center, left, right | `55`, `56`, `57` | Each tap's level (127 is full) |
| Level | `58` | The delay's return: 64 is unity, 0 is none |
| Feedback | `59` | -64 to +63 (64 is none) of the center tap goes back in; negative inverts each repeat |
| Send to reverb | `5A` | The delay's output goes on into the reverb |

**XG variation.** In system mode (connection `5A` = 1) CC94 (or the part's variation send `08 nn 14`) sends to it, like reverb and chorus. In insertion mode (`5A` = 0, the default) it takes the whole sound of one part (`5B`: 0-15, 127 none, the default), which then goes on to the reverb and chorus at that part's sends; CC94 does nothing in that mode. After an XG reset the type is Delay L,C,R, in insertion mode with no part, so nothing changes until a file asks for it.

| Setting | XG (`02 01 xx`) | What it does here |
|---|---|---|
| Type | `40` (MSB), `41` (LSB) | Below. A type sets its parameters to its defaults |
| Parameters 1-10, 11-16 | `42`-`55` (two bytes each, MSB first), `70`-`75` | As Yamaha's tables number them for each type |
| Return, pan | `56`, `57` | System mode: 64 is unity and the middle |
| Send to reverb, chorus | `58`, `59` | System mode: its output goes on into the reverb and the chorus |
| Connection, part | `5A`, `5B` | Insertion (0) or system (1); the part in insertion mode. Parameter 10 (dry/wet) mixes the part's own sound with the effect's: 64 is half each |

| Types | How they are made |
|---|---|
| Hall 1-2, Room 1-3, Stage 1-2, Plate, White Room, Tunnel, Canyon, Basement | The reverb above with the type's decay time and brightness; parameter 1 is the reverb time |
| Delay L,C,R; Delay L,R; Echo; Cross Delay | Two delay lines with up to four taps and a feedback path each, with the type's times (0.1 ms steps), feedback level, center level and high damp (the repeats darken) |
| Karaoke 1-3 | One echo with feedback (about 3 ms a step of parameter 1), a high-pass and a low-pass |
| Early Reflection 1-2, Gate Reverb, Reverse Gate | 12 or 16 taps after the initial delay, spread over a span the room size sets; the gate types stop dead, the reverse one swells |
| Chorus 1-4, Celeste 1-3, Flanger 1-3, Symphonic | The chorus above with the type's LFO frequency, depth, feedback and delay offset |
| Rotary Speaker, Tremolo, Auto Pan | An LFO (parameter 1) on level, on left against right, or (rotary) on level and a short swinging delay |
| Phaser 1-2 | 6 or 8 allpass stages swept by the LFO over up to 4 octaves from the phase offset, with feedback |
| Distortion, Overdrive, Amp Simulator | Drive into a clipper (the edge parameter sets how hard), a lowpass and EQ (amp simulator: the amp type's cabinet), output level |
| 3-Band EQ, 2-Band EQ | Low and high shelves and a middle peak (Yamaha's frequency table, -12 to +12 dB) |
| Auto Wah | A resonant bandpass swept by the LFO |
| Thru | The sound unchanged |

The MU100's additions (pitch change, aural exciter, touch wah, compressor, noise gate, voice cancel, lo-fi and the others in the [insertion effects](#insertion-effects-xg-insertion-and-gs-efx) list) play in the variation block too, and so do Celeste 4 and the MU100's second Phaser number (`48 08`). The variation's MW, bend, aftertouch and AC1 depths (`5C`-`5F`, 64 is none) move the type's controlled parameter by up to its whole range: the LFO frequency of the modulation types, the drive of the distortions, the wah's cutoff, otherwise dry/wet. The EQ bands of the MU80 modulation types are ignored.

### Insertion effects (XG insertion and GS EFX)

An insertion effect takes the whole sound of the parts assigned to it, as the variation does in insertion mode. FatzAudio has the MU100's two XG insertion blocks and the SC-88 Pro's EFX. Which ones play follows the standard of the last reset or insertion SysEx: after an XG reset the XG insertions, after a GS reset the EFX, after a GM reset neither. After an XG reset both insertion blocks are Distortion with no part; after a GS reset the EFX is Thru and no part is switched to it. So nothing changes until a file assigns a part.

A part can go through several blocks, always in this order: XG insertion 1, insertion 2, the XG variation (insertion mode), the GS EFX. Several parts on one block are summed into it. After the last block the sound goes on to the reverb and chorus: XG at the part's own sends (CC91, CC93, and CC94 when the variation is in system mode); GS at the EFX's own send levels (`17`-`19`), which stand in for those of the parts on it, as on the SC-88 Pro.

| Setting | XG insertion 1, 2 (`03 00 xx`, `03 01 xx`) | GS EFX (`40 03 xx`) |
|---|---|---|
| Type | `00` (MSB), `01` (LSB). A type sets its parameters to its defaults | `00` (MSB), `01` (LSB). The same |
| Parameters | 1-10 at `02`-`0B` (one byte) or `30`-`43` (two bytes, MSB first), 11-16 at `20`-`25`, as Yamaha numbers them for each type | 1-20 at `03`-`16`, as Roland numbers them for each type; most types end with low and high gain (`13`, `14`: 200 Hz and 4 kHz shelves, ±12 dB), pan (`15`, mono types) and level (`16`) |
| Part | `0C`: 0-15, 127 (none, the default) | Each part's EFX switch, `40 4x 22` (1 on; x numbers the parts as in the other part addresses); several parts can be on |
| Sends | The part's own | Reverb, chorus, delay `17`, `18`, `19` (40, 0, 0 after a reset); the delay send reaches the SC-88 delay |
| Controls | MW, bend, aftertouch, AC1, AC2 depths `0D`-`11` (64 is none): each moves the type's controlled parameter by up to its whole range | Control 1 and 2 source (`1B`, `1D`: 0 off, 1-95 that controller, 96 channel aftertouch, 97 bend) and depth (`1C`, `1E`: 64 is none). They move the parameters marked + and # in Roland's table. The source is read on the lowest part switched to the EFX |
| Dry/wet | Parameter 10 for the types that have one (64 is half each); 127 is the effect alone | Each type mixes its own (its balance parameter) |

| GS EFX types (SC-88 Pro) | How they are made |
|---|---|
| Thru | The sound unchanged (the EFX send levels still apply) |
| Stereo-EQ, Spectrum | Two shelves and two peaks; eight fixed peaks (250 Hz to 8 kHz) of one width |
| Enhancer, Humanizer | High harmonics added above about 2.5 kHz; a drive into two formant filters gliding between five vowels |
| Overdrive, Distortion | Drive into a soft or harder clipper and one of four amp tones |
| Phaser, Auto Wah, Rotary | 6 allpass stages swept by an LFO with resonance; a resonant filter swept by the LFO and the input level; a horn and a low rotor turning at their own slow and fast speeds and accelerations, with a speed switch |
| Stereo Flanger, Step Flanger, Hexa Chorus, Tremolo Chorus, Stereo Chorus, Space D, 3D Chorus | Modulated delay lines: one or two with feedback, a stepped LFO, six voices spread across the stereo field, a chorus with a level LFO, two voices in opposite phase |
| Tremolo, Auto Pan | An LFO (five waves) on level, or on left against right |
| Compressor, Limiter | Level detection with attack and release; a sustain/threshold, ratio and make-up level |
| Stereo Delay, Mod Delay, 3 Tap Delay, 4 Tap Delay, Tm Ctrl Delay, 3D Delay | Delay taps with feedback and high damp; modulated; three or four taps placed across the field; a time that glides when it is changed; taps placed outside the speakers |
| Reverb, Gate Reverb | The reverb with its own time, pre-delay and damping; 16 reflections that stop dead (normal, reverse, or sweeping across the field) |
| 2 Pitch Shifter, Fb Pitch Shifter | Two windowed pitch shifters (-24 to +12 semitones, fine ±100 cents) with pre-delay and pan; one with feedback |
| 3D Auto, 3D Manual | A sound placed and turned around the listener with level, delay and filtering per side |
| Lo-Fi 1, Lo-Fi 2 | Fewer bits and a lower sample rate, a filter; noise, hum, crackle and a radio-like band, mono or stereo |
| OD, DS or EH into Chorus, Flanger or Delay; Chorus into Delay; Flanger into Delay; Chorus into Flanger; Rotary Multi | Two or more of the blocks above in series |
| Guitar Multi 1-3, Clean Guitar Multi 1-2, Bass Multi, Rhodes Multi, Keyboard Multi | Compressor or wah, drive or enhancer, EQ, a modulation (chorus, flanger, phaser, tremolo, pan) and a delay in series |
| Cho/Delay, FL/Delay, Cho/Flanger, OD1/OD2, OD/Rotary, OD/Phaser, OD/AutoWah, PH/Rotary, PH/AutoWah | Two blocks side by side, each with its own pan and level |

| XG insertion types (MU100 additions) | How they are made |
|---|---|
| Comp+Distortion; Wah+Distortion, Wah+Overdrive; Touch Wah, Touch Wah 2, Touch Wah+Distortion, +Overdrive | A compressor or a wah swept by the input level (touch) or an LFO, into the drive, with the XG EQ |
| Pitch Change 1-2 | Two pitch shifters (±24 semitones, ±50 cents each) with an initial delay, feedback, pan and level per voice |
| Aural Exciter | High harmonics added above a set frequency |
| Compressor, Noise Gate | Attack, release, threshold, ratio and output level; a gate that closes below its threshold |
| Voice Cancel | Takes out what is the same on both sides between a low and a high frequency (a centred voice) |
| 2-way Rotary Speaker, Ensemble Detune, Ambience | A horn and a rotor; two slightly detuned copies, one on each side; a short room reverb with its own filters |
| Talking Modulation | Two formant filters gliding between the vowels a, i, u, e, o |
| Lo-Fi | Fewer bits and a lower sample rate, a resonant filter, mono or stereo |
| Dist+Delay, OD+Delay, Comp+Dist+Delay, Comp+OD+Delay, Wah+Dist+Delay, Wah+OD+Delay | The blocks above in series, into a two-tap delay |

The XG insertion blocks also play every variation type above (they are the same effect). A type that is not in these lists is dry: on XG an unknown LSB plays the MSB's first type (LSB 0, as the variation does) and an unknown MSB leaves the part dry; on GS an unknown type leaves the parts as if they were not switched to the EFX (their own sends included).

The output is deterministic (the LFOs follow the song position, not the read size), silence stays silent (the lo-fi noise and hum follow the input), and an idle block costs nothing. A seek renders the second before the new position, effects included, as for the other effects (about 5-10 ms with an insertion effect playing, against about 4 ms without).

The defaults and curves (LFO frequencies, delay offsets, room sizes, drive) are our reading of the published GS and XG parameter tables, not a copy of a module; how close each type sounds to a real SC-88 or MU module is still to be heard.

`fa_stream_set_midi_effects` (`MidiEffects`) turns both off for a stream, which plays the dry synthesizer (BASSMIDI's `BASS_MIDI_NOFX`); the tails stop at once. It may be called from any thread.

```c
fa_stream_set_midi_effects(song, 0);             /* dry, no reverb or chorus */
int32_t on = 1;
fa_stream_get_midi_effects(song, &on);           /* on again: 1 */
```

```csharp
song.MidiEffects = false;                        // dry
bool wet = song.MidiEffects;
```

A channel mute silences the channel's dry sound at once, but reverb it has already sent keeps ringing out. The output is deterministic and does not depend on the read size.

## SysEx

The SysEx messages that change how a file sounds are applied; the rest are ignored. Checksums aren't checked, and any device number is accepted.

| Message | Effect |
|---|---|
| GM System On, GS reset (and GS system mode set), XG System On, GM2 System On | Held notes are released and every channel goes back to its defaults: program 0, controllers reset, volume 100, master volume and tuning cleared. Picks how drum channels are chosen (below) |
| GS "use for rhythm part" (`40 1x 15`) | Makes any part a drum part, or channel 10 a melodic one |
| XG part mode (`08 nn 07`) | Makes part nn a drum part |
| Bank select MSB 127 (and 126) after an XG reset, 120 after a GM2 reset | The channel plays drum kits from its next program change; other banks make it melodic, channel 10 included |
| Master volume: universal (`7F 04 01`), GS (`40 00 04`), XG (`00 00 04`) | Scales the whole song, without a click |
| Master fine and coarse tuning (`7F 04 03`, `7F 04 04`) | Retunes every channel, drums included |
| GS reverb, chorus and delay (`40 01 30`-`5A`), XG reverb, chorus and variation (`02 01 00`-`5F`, `70`-`75`) | Effect type, level, time and the rest ([reverb and chorus](#reverb-and-chorus), [delay and variation](#delay-gs-and-variation-xg)) |
| GS EFX (`40 03 00`-`1E`) and part EFX switch (`40 4x 22`); XG insertion 1 and 2 (`03 00 xx`, `03 01 xx`) | Insertion effects on chosen parts ([insertion effects](#insertion-effects-xg-insertion-and-gs-efx)) |
| GS part level, pan, chorus, reverb and delay send (`40 1x 19`, `1C`, `21`, `22`, `2C`); XG part volume, pan, chorus, reverb and variation send (`08 nn 0B`, `0E`, `12`, `13`, `14`) | The same as CC7, CC10, CC93, CC91 and CC94 on that part's channel. Pan 0 ("random") is the middle |
| GS part vibrato, filter and envelope offsets (`40 1x 30`-`37`); XG multi part (`08 nn 15`-`1C`) | The same as the sound controllers CC71-78 ([controllers](#controllers-and-modulators)) |
| GS scale tuning (`40 1x 40`-`4B`), XG scale tuning (`08 nn 41`-`4C`) | Moves each note of the octave, C to B, by -64 to +63 cents (64 is in tune) on that part, notes already sounding included ([scale tuning and drum setup](#scale-tuning-and-drum-setup)) |
| GS drum maps (`41 m1`-`m8 rr`), XG drum setups (`3n rr 00`-`0F`), XG drum setup reset (`00 00 7D`) | Per-key edits of a drum kit: pitch, level, pan, sends, exclusive group, note-off and note-on reception, and on XG filter and envelope ([below](#scale-tuning-and-drum-setup)) |

Without a reset, channel 10 is the drum channel and bank 127 is an ordinary (GS MT-32) melodic bank. A drum kit the soundfont doesn't have falls back to the standard kit (bank 128, program 0). An XG SFX kit (bank 126) comes from the soundfont's bank 126 (where XG soundfonts keep their SFX kits), else from the GS SFX kit (drum kit 56), which has the same kind of sounds, else it falls back like any other drum kit (to the standard kit). A melodic program missing from the channel's bank comes from bank 0; when bank 0 lacks it too, the channel plays program 0 (with a soundfont that has no program 0, its first preset), never the instrument it played before.

### Scale tuning and drum setup

Scale tuning retunes the twelve notes of the octave on one part, each by -64 to +63 cents, for the Arabic, Indian or historical temperaments some GS and XG files ask for. It applies to the notes sounding when it changes too, and to drum parts as well as melodic ones.

Drum setup edits a drum kit key by key. GS has two drum maps: channel 10 and the parts set to "map 1" use the first, the parts set to "map 2" (`40 1x 15` = 2) the second. XG has four drum setups: drum parts and part mode "DrumS1" use the first, "DrumS2" to "DrumS4" the others. The drum NRPNs on a drum part edit the map or setup that part plays. Edits take effect from the key's next note.

| Edit | GS map (`41 mp rr`, rr = key) | XG setup (`3n rr pp`) | NRPN (MSB, LSB = key) | What it does |
|---|---|---|---|---|
| Pitch | Play note number `p=1` (the key it sounds as) | Coarse `00`, fine `01` (64 = as is) | Coarse `18`, fine `19` (XG) | Semitones and cents |
| Level | `2` | `02` | `1A` | 127 is the soundfont's level, lower is softer like velocity (64 is 11.9 dB down), 0 is silent |
| Pan | `4` | `04` | `1C` | 1 to 127, left to right, in place of the soundfont's pan; 0 ("random") is the middle. The part's pan moves it as usual |
| Reverb, chorus and delay/variation send | `5`, `6`, `9` | `05`, `06`, `07` | `1D`, `1E`, `1F` | 127 is the key's full send, lower scales it down |
| Exclusive group | Assign group `3` | Alternate group `03` | | Keys of one group (1 to 127) cut each other, as open and closed hi-hats do; 0 takes the key out of any group. It replaces the soundfont's own exclusive class for that key |
| Note-off and note-on reception | `7`, `8` | `09`, `0A` | | 0 ignores the key's note-offs (it rings until its sound ends) or its note-ons (it is silent) |
| Filter | | Cutoff `0B`, resonance `0C` | `14`, `15` (XG) | 64 is as is; the same steps as CC74 and CC71 |
| Envelope | | Attack rate `0D`, decay 1 rate `0E`, decay 2 rate `0F` | `16`, `17` (XG) | Rates: above 64 is faster, below slower, in the steps of CC73 and CC75; decay 2 acts as the release |

GM, GS and XG resets clear scale tuning and every drum edit; the XG drum setup reset (`00 00 7D`, data = setup 0-3) clears one setup. A seek replays them all. Not followed: XG key assign (single or multi).

## Seeking and length

Seeking is sample-exact and fast: the events up to the target are replayed into a fresh synthesizer without rendering any audio, so programs, controllers, pitch bends, SysEx settings and the tempo are exactly as if the song had played up to there. Notes held across the target, by their key or by the sustain pedal, start again from their attack (as in BASSMIDI), so a chord or pad held across a seek is heard right away. A key struck several times and still held comes back as one note, at the loudest of those strikes, and ends with the last of their note-offs. Drum notes aren't restarted, since they'd sound as a new hit. With effects off, where no note is held, the audio after a seek is exactly what playing through gives.

With effects on (the default), a seek also renders the second before the target without playing it, so the reverb and chorus of what was playing just before ring on into the target instead of starting from silence. The replay then stops a second before the target, and notes held there start again at that point, so at the target they are a second into their sound. Within the first second of the song this makes a seek exactly the same as playing through; later, only the reverb of what played more than a second before is missing (at least 45 dB down with the default reverb). This makes a seek take longer: about 20 ms for a dense eight-part song on a desktop CPU, against well under a millisecond with effects off.

The length is the time of the last event plus one second, so the final notes ring out. Audio from a given position is the same whatever the read size or the seeks before it, so loudness scans are cached like those of any other file (the cache key includes the soundfont and the sample rate).

## Threads

Rendering runs on the engine's worker threads like decoding of any other format. Soundfonts may be opened, closed and set from any thread, and shared by any number of streams and engines.

## Limits and pitfalls

- MIDI files up to 16 MB; events later than 6 hours are dropped. Soundfonts up to 1 GB.
- 256 voices per stream. When a dense passage needs more, the voices furthest into their release are cut; if all 256 are held, the new notes are skipped.
- Damaged files play what can be read: a track cut short plays up to the cut. A file with no usable track fails with `FA_ERROR_UNSUPPORTED_FORMAT`.
- DLS banks play their regions, samples, loops, envelopes, LFOs, filter and gains. Articulation scaled by a controller (other than the default mod wheel vibrato) and DLS 2 conditional chunks are ignored. A region with its own articulation uses it instead of the instrument's, as the DLS spec says. A region's net gain above 0 dB plays at 0 dB (SoundFont 2 attenuation can't be negative).
- Note velocity sets the level by the SoundFont 2 default curve (concave, 96 dB range): 40·log10(velocity/127) dB, so velocity 64 is 11.9 dB below 127, as in FluidSynth and DLS synths. A soundfont's own modulators are applied ([modulators](#soundfont-modulators)), except where they repeat a built-in default.
- Mono and legato modes (CC126/127), GM2 controller destination settings (what pressure and the mod wheel drive, `7F 09 01`-`03`) and the SoundFont NRPNs (MSB 120) are ignored; portamento glides in every mode.
- Each instrument's level (`initialAttenuation`) is applied as the SoundFont 2 spec states it, 0.1 dB per unit. FluidSynth applies 0.4 of that (an EMU hardware habit), so a bank tuned by ear in FluidSynth can sound with a wider gap between its quiet and loud instruments here.
- SFZ soundfonts and MIDI input from devices aren't supported yet. See the [engineering notes](../engineering-notes.md).
- Text in code pages other than UTF-8, Windows-1252 and Shift-JIS (Korean, Chinese, Cyrillic files) comes out as Windows-1252. Shift-JIS user-defined characters (lead bytes F0-F9) come out as U+FFFD.
- SysEx other than the messages listed [above](#sysex) is ignored: the part parameters not listed have no effect.
- The insertion effect types' curves and defaults (drive and amp tones, formants, lo-fi, the multi-effects' blocks, where the manuals give no default) are our reading of the published SC-88 Pro and MU100 tables, not a copy of a module; nobody has compared them with real hardware yet.
- A seek keeps the effects of the second before the target. A long GS delay (up to 1 s) or an XG echo with strong feedback can still ring past that second, and those repeats are missing after a seek.
- The reverb's tail is cut at the end of the song (a second after the last event), so a long reverb time set by the file doesn't ring out fully at the very end.
