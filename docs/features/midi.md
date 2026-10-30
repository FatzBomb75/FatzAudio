# MIDI and karaoke files

FatzAudio plays MIDI files itself, with a built-in SoundFont 2 synthesizer. They open with the ordinary `fa_stream_open_*` calls and play, seek, loop, crossfade, normalize and stretch like any other stream. It replaces bassmidi.

| What | Supported |
|---|---|
| Standard MIDI Files (`.mid`, `.midi`, `.smf`) | Format 0, 1 and 2, tempo maps, SMPTE time division, running status |
| Karaoke (`.kar`) | Lyrics from the "Words" track, `@T` title and artist lines, line and paragraph marks |
| RIFF MIDI (`.rmi`) | The Standard MIDI File inside it |
| Instruments | A SoundFont 2 (`.sf2`, also SF3 with compressed samples) or DLS (`.dls`) bank you load, else the one the OS ships (Windows `gm.dls`, macOS `gs_instruments.dls`, a Linux GM `.sf2` package): General MIDI; GS/XG variation banks through bank select (falling back to bank 0 when the soundfont lacks them); drums on channel 10 and on the parts GS, XG and GM2 files make drum parts |
| SysEx | GM, GM2, GS and XG resets, GS rhythm parts, XG drum parts and drum banks (127, and 120 in GM2), master volume and master tuning, GS and XG reverb and chorus settings, part volume, pan and effect sends ([below](#sysex)) |
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

Every text event of the file is listed in time order by `fa_stream_get_midi_text` (`AudioStream.MidiTexts`), with its frame, time, type, track and UTF-8 text. While the stream plays on a device, `FA_EVENT_MIDI_TEXT` (`AudioStream.MidiTextReached`) is raised as each one is heard, with `value` = its index.

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
| Chorus type | Macro `38`: Chorus 1-4, Feedback Chorus, Flanger, Short Delay, Short Delay (FB) | Type `20`/`21`: Chorus, Celeste, Flanger (the later types play as a chorus), or no effect | Sets the delay, depth, rate and feedback below |
| Chorus level | `3A` | Return `2C` | As the reverb level |
| Chorus pre-LPF, feedback, delay, rate, depth | `39`, `3B`, `3C`, `3D`, `3E` | | Delay about 1 to 50 ms (80 is 13 ms), rate up to 10 Hz (3 is 0.4 Hz), depth up to 20 ms (19 is 3 ms) |

Messages that carry several data bytes set consecutive addresses, as on the hardware. Roland and Yamaha don't publish the exact curves, so times and ranges are close approximations rather than a copy of a particular module. Not followed: GS reverb pre-delay and chorus send to reverb, the XG chorus's own parameters (LFO, feedback, delay offset) and the XG variation and insertion effects.

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
| GS and XG reverb and chorus (`40 01 30`-`3E`, `02 01 00`-`2C`) | Effect type, level and time ([reverb and chorus](#reverb-and-chorus)) |
| GS part level, pan, chorus and reverb send (`40 1x 19`, `1C`, `21`, `22`); XG part volume, pan, chorus and reverb send (`08 nn 0B`, `0E`, `12`, `13`) | The same as CC7, CC10, CC93 and CC91 on that part's channel. Pan 0 ("random") is the middle |
| GS part vibrato, filter and envelope offsets (`40 1x 30`-`37`); XG multi part (`08 nn 15`-`1C`) | The same as the sound controllers CC71-78 ([controllers](#controllers-and-modulators)) |

Without a reset, channel 10 is the drum channel and bank 127 is an ordinary (GS MT-32) melodic bank. A drum kit the soundfont doesn't have falls back to the standard kit (bank 128, program 0). A melodic program missing from the channel's bank comes from bank 0; when bank 0 lacks it too, the channel plays program 0 (with a soundfont that has no program 0, its first preset), never the instrument it played before.

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
- SysEx other than the messages listed [above](#sysex) is ignored: insertion and variation effects, scale tuning, drum setup (per-note drum edits) and the part parameters not listed have no effect.
- The reverb's tail is cut at the end of the song (a second after the last event), so a long reverb time set by the file doesn't ring out fully at the very end.
