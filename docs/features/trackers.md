# Tracker modules

FatzAudio plays tracker modules itself, with [libopenmpt](https://lib.openmpt.org) 0.8.9 built into the library. They open with the ordinary `fa_stream_open_*` calls and play, seek, loop, crossfade, normalize and stretch like any other stream. It replaces `BASS_MusicLoad`.

| What | Supported |
|---|---|
| Formats | MOD, S3M, XM, IT, MPTM, MTM, UMX (Unreal music), 669, AMF, AMS, DBM, DIGI, DMF, DSM, FAR, GDM, IMF, J2B, MDL, MED, MO3, MT2, OKT, PLM, PSM, PTM, SFX, STM, ULT, WOW and the other formats libopenmpt reads |
| Module information | Format, tracker program, title, artist, song message, channel, order, pattern, instrument, sample and subsong counts, restart position, duration |
| Names | Song message, instrument, sample, channel, pattern and subsong names (UTF-8) |
| Subsongs | Choosing the subsong to play, the subsong count and names |
| Order and row | The time a row first plays, seeking to an order and row, the order and row heard now, and an event for each row (or each order) |
| Live controls | Interpolation, stereo separation, volume ramping, Amiga resampler emulation, channel mute, plus every stream parameter (volume, tempo, pitch). The module's own state: tempo and speed, global volume, channel volume, instrument mute, amplification |
| Order list | The pattern of each order |
| Events | A row, a note with an instrument, a Zxx command |
| Tags | Title, artist and song message as `FA_TAG_FORMAT_TRACKER` items |

Files are detected from their content, never from the extension, so a module with the wrong extension, from memory, from your own I/O callbacks or from a URL still plays. `fa_stream_get_info` reports stereo F32 audio at the engine's `mix_sample_rate`, or 44100 Hz when that is 0 (device native), like MIDI. No add-on library and no sample bank are needed: modules carry their own samples.

MO3 files whose samples are MP3 or Ogg Vorbis compressed are not supported (`FA_ERROR_UNSUPPORTED_FORMAT`); MO3 files with plain samples play.

## Playing and looping

A module plays **once**. The length is libopenmpt's duration of the song (capped at 6 hours), and the stream plays exactly that many frames. The module's own loop (the jump back to its restart position at the end) is not followed, so the length is always known and seeking, `FA_EVENT_ENDED`, crossfades and queues work as for a file. A module with several subsongs plays its first subsong unless you pick another (see [Subsongs](#subsongs)). To repeat the song, use the stream's loop toggle: it starts the whole song again from the beginning (`fa_stream_set_loop`, .NET `Loop`), not at the module's restart order. The restart position is reported in the module information if you want to build that yourself with an order/row seek.

Seeking is exact to the frame. libopenmpt restores the state at the row that holds the target (playing samples included) and the engine renders on to the frame without playing it. The result is very close to playing through but not always identical: an effect that depends on everything before it, such as a long pitch slide, may sound slightly different for a moment.

### C

```c
fa_stream* song = NULL;
fa_stream_open_path(engine, "axelf.mod", NULL, &song);   /* detected from the content */
fa_stream_attach(song, device, NULL);
fa_stream_play(song);

fa_stream_set_loop(song, 1);                              /* the whole song again, from the start */
```

### C#

```csharp
using var song = engine.OpenFile("axelf.mod");
song.PlayOn(output);
song.Loop = true;
```

## Information and texts

`fa_stream_get_tracker_info` fills a struct (set `struct_size` first). It returns `FA_ERROR_INVALID_ARG` for a stream that is not a module (.NET: `TrackerInfo` is `null`).

| Field | Meaning |
|---|---|
| `type`, `type_name` | The format's usual extension (`mod`, `xm`, `it`, `s3m`, ...) and its name ("ProTracker MOD", "Impulse Tracker", ...) |
| `tracker` | The program that saved it, when known  |
| `title`, `artist` | UTF-8, `""` when the file has none |
| `channels`, `orders`, `patterns`, `instruments`, `samples`, `subsongs` | Counts. MOD has samples but no instruments; XM and IT have both |
| `restart_order`, `restart_row` | Where the song's own loop goes back to |
| `duration` | Seconds, the subsong played once |
| `subsong` | The subsong playing: the `audio_track` the stream was opened with, 0 by default |

`subsong` was added at the end of the struct: a shorter, older `struct_size` is still accepted and filled up to `duration`.

The longer texts come from `fa_stream_get_tracker_text`: the type picks the list and the index the entry. The text is UTF-8, cut at a character boundary to fit the buffer, and `out_length` is its full length in bytes, so a call with capacity 0 asks for the size. An index out of range returns `FA_ERROR_NOT_FOUND`.

| `fa_tracker_text_type` | Index |
|---|---|
| `FA_TRACKER_TEXT_MESSAGE` | 0 only: the song message, `""` when it has none |
| `FA_TRACKER_TEXT_INSTRUMENT` | 0 to `instruments - 1` |
| `FA_TRACKER_TEXT_SAMPLE` | 0 to `samples - 1` |
| `FA_TRACKER_TEXT_CHANNEL` | 0 to `channels - 1` (most formats leave the names empty) |
| `FA_TRACKER_TEXT_PATTERN` | 0 to `patterns - 1` (most formats leave the names empty) |
| `FA_TRACKER_TEXT_SUBSONG` | 0 to `subsongs - 1` |

### C

```c
fa_tracker_info ti;
ti.struct_size = sizeof ti;
if (fa_stream_get_tracker_info(song, &ti) == FA_OK)
    printf("%s (%s) by %s: %u channels, %u orders, %.1f s\n",
           ti.title, ti.type_name, ti.artist, ti.channels, ti.orders, ti.duration);

size_t len = 0;
fa_stream_get_tracker_text(song, FA_TRACKER_TEXT_MESSAGE, 0, NULL, 0, &len);     /* ask the size */
char* msg = malloc(len + 1);
fa_stream_get_tracker_text(song, FA_TRACKER_TEXT_MESSAGE, 0, msg, len + 1, NULL);

char name[64];
fa_stream_get_tracker_text(song, FA_TRACKER_TEXT_SAMPLE, 0, name, sizeof name, NULL);
```

### C#

`TrackerInfo` carries all the texts at once, so there is no text call to make.

```csharp
if (song.TrackerInfo is { } t)
{
    Console.WriteLine($"{t.Title} ({t.TypeName}) by {t.Artist}: {t.Channels} channels, {t.Orders} orders, {t.Duration}");
    Console.WriteLine(t.Message);
    foreach (var name in t.Samples) Console.WriteLine(name);
    // also t.Instruments, t.ChannelNames, t.PatternNames, t.Subsongs
}
```

## Order and row

A module is a list of orders (each names a pattern) of rows. `fa_stream_get_tracker_time` gives the time, in seconds from the start, where the song first plays a row; `fa_stream_seek_tracker` seeks there. Both count from 0. `FA_ERROR_NOT_FOUND` means the song never plays that row: an order that is skipped by a jump, a row past the end of that order's pattern, or an order that belongs to another subsong than the one playing (.NET: `GetTrackerTime` returns `null`, `SeekTracker` throws).

```c
double t;
if (fa_stream_get_tracker_time(song, 4, 0, &t) == FA_OK)     /* where order 4 starts */
    printf("order 4 starts at %.2f s\n", t);
fa_stream_seek_tracker(song, 4, 16);                          /* order 4, row 16 */
```

```csharp
TimeSpan? at = song.GetTrackerTime(4, 0);     // null when the song never plays it
song.SeekTracker(4, 16);
```

This is a map from the order list to time, for building a pattern jump list or a seek bar with order marks. The times count from the start of the subsong that plays; a row of another subsong returns `FA_ERROR_NOT_FOUND` (see [Subsongs](#subsongs)).

### The order and row heard now

`fa_stream_get_tracker_position` gives the order and row that are **heard** at this moment, that is at `fa_stream_get_clock`, not where the decoder has got to (it runs ahead of the speaker). It is accurate to within about 2 ms of the row start (`FA_TRACKER_STEP`, 64 frames), cheap and lock-free like the clock, so it is fine to call every video frame. It returns `FA_ERROR_NOT_FOUND` while that audio is not decoded yet: a stream that was never played or read, or just after a seek.

```c
uint32_t order, row;
if (fa_stream_get_tracker_position(song, &order, &row) == FA_OK)
    printf("order %u, row %u\n", order, row);
```

```csharp
if (song.TrackerPosition is { } pos)               // null while not known
    Console.WriteLine($"order {pos.Order}, row {pos.Row}");
```

### Row events

Set `FA_TRACKER_ROW_EVENTS` to have `FA_EVENT_TRACKER_ROW` raised as rows are heard: 1 for every row, 2 for the first row of each order, 0 (the default) for none. The event's `frame` is the row's first frame and `value` is `order << 16 | row`. Like markers, the events are raised as the audio is mixed and are best effort (see [events](events.md)): a display that must follow every row should read the position above each frame instead. This is the `BASS_SYNC_MUSICPOS` equivalent; BASS lets you pick one position, here you filter in the handler. In [reverse playback](tempo-pitch.md#reverse-playback) the rows come last first, each as playback reaches its first frame going down.

A row counts as the first of its order when the order changes or the song jumps back to row 0 of the same order. A row that repeats itself in place (a pattern loop on a single row, such as `E6x` on that row) raises one event, not one per pass.

```c
fa_stream_set_tracker_param(song, FA_TRACKER_ROW_EVENTS, 1);     /* every row */
/* in the event callback */
if (ev->type == FA_EVENT_TRACKER_ROW) {
    uint32_t order = (uint32_t)(ev->value >> 16), row = (uint32_t)(ev->value & 0xFFFF);
    if (order == 4 && row == 0) { /* the chorus starts */ }
}
```

```csharp
song.TrackerRowEvents = TrackerRowEvents.EveryRow;     // Off, EveryRow, Orders
song.TrackerRowReached += (_, e) =>
{
    if (e.Position.Order == 4 && e.Position.Row == 0)   // also e.Time, e.Frame
        Console.WriteLine("the chorus starts");
};
```

## Module tempo, volumes and the order list

These change the **song's own state**, as `BASS_ATTRIB_MUSIC_*` did. They are not a time-stretch: `FA_PARAM_TEMPO` (`Tempo`) stretches the finished audio and leaves the song as it is, while the calls below change the song's tempo and speed, so a pattern's notes land on different frames. They take effect after the decode-ahead buffer.

| What | C | .NET | Range |
|---|---|---|---|
| Tempo | `fa_stream_set_tracker_tempo` / `_get_` | `TrackerTempo` | 32 to 512 beats per minute (the exact meaning depends on the format's tempo mode) |
| Speed | `fa_stream_set_tracker_speed` / `_get_` | `TrackerSpeed` | 1 to 255 ticks per row |
| Global volume | `fa_stream_set_tracker_global_volume` / `_get_` | `TrackerGlobalVolume` | 0.0 to 1.0 |
| Channel volume | `fa_stream_set_tracker_channel_volume` / `_get_` | `SetTrackerChannelVolume`, `GetTrackerChannelVolume` | 0.0 to 1.0, the first 64 channels |
| Instrument mute | `fa_stream_set_tracker_instrument_mute` / `_get_` | `SetTrackerInstrumentMuted`, `IsTrackerInstrumentMuted` | the 0-based instrument, or the sample in formats without instruments (MOD, S3M) |
| Amplification | `fa_stream_set_tracker_param(FA_TRACKER_AMPLIFY, millibel)` | `TrackerAmplificationDb` | -60 to 60 dB, 0 by default |

- Tempo, speed and the global volume are the song's *current* values: a tempo or speed command (`Fxx`, `Axx`, `Txx`) or a global volume command later in the song sets them again, and a seek, or a loop that starts the song again, puts the module back in its own state at the target. Set them again after a seek if you want to keep them.
- Channel volumes and instrument mutes **stay** through seeks, and the song's own channel volume commands do not undo them until they are met again. Amplification is an output gain on top of everything and always stays.
- The stream keeps its length (the song played once at its own tempo): a song made faster ends early and silence follows to the old end, a slower one is cut at it. A seek by frame therefore lands at a different place in the song than before the change. Row events and `fa_stream_get_tracker_position` follow the song as it plays.
- The getters read the song as decoded so far, which is ahead of what is heard by the decode-ahead buffer. A call right after a set returns the new value.
- libopenmpt has no volume per instrument: `BASS_ATTRIB_MUSIC_VOL_INST` maps to the instrument mute. Mute and unmute give the on/off case; for a mix level per instrument use the channel volume of the channel it plays on.
- A module without the interactive controls (libopenmpt always has them for the formats it plays) returns `FA_ERROR_NOT_IMPLEMENTED`.

```c
fa_stream_set_tracker_tempo(song, 150.0);                  /* the song's own tempo */
fa_stream_set_tracker_channel_volume(song, 2, 0.25);       /* quiet channel 2 */
fa_stream_set_tracker_instrument_mute(song, 5, 1);         /* no drums */
fa_stream_set_tracker_param(song, FA_TRACKER_AMPLIFY, 300);   /* +3 dB */
```

```csharp
song.TrackerTempo = 150;
song.SetTrackerChannelVolume(2, 0.25);
song.SetTrackerInstrumentMuted(5, true);
song.TrackerAmplificationDb = 3;
```

### The order list

`fa_stream_get_tracker_orders` gives the pattern played at each order (`BASS_TAG_MUSIC_ORDERS`); call it with capacity 0 to ask for the count. 65534 is a skipped order ("+++") and 65535 the end of the song ("---"). With the pattern names from `FA_TRACKER_TEXT_PATTERN` and the time of each order from `fa_stream_get_tracker_time` this is a full pattern jump list.

```c
uint32_t count, patterns[256];
fa_stream_get_tracker_orders(song, patterns, 256, &count);
```

```csharp
IReadOnlyList<int> patterns = song.GetTrackerOrders();
```

### Instrument and sync events

`FA_TRACKER_CELL_EVENTS` (.NET `TrackerCellEvents`) picks what is reported as the rows are heard, read from the pattern data:

- bit 1, `FA_EVENT_TRACKER_INSTRUMENT` (.NET `TrackerInstrumentPlayed`; replaces `BASS_SYNC_MUSICINST`): a note with an instrument number. `value` is `channel << 16 | instrument`, the instrument 0-based. A note without an instrument number (it plays the channel's last one) and an instrument number without a note do not count.
- bit 2, `FA_EVENT_TRACKER_SYNC` (.NET `TrackerSyncReached`; replaces `BASS_SYNC_MUSICFX`): the `Zxx` command (IT, S3M and the other formats libopenmpt reads it from). `value` is `channel << 16 | xx`. MOD and XM have no `Zxx`; the command is meant for songs that carry cues for the app.

The event's `frame` is the first frame of the row. Like the row events they are raised as the audio is mixed and are best effort (see [events](events.md)); a row with more than 256 such cells loses the rest. In [reverse playback](tempo-pitch.md#reverse-playback) they come last first, with the rows. Only rows decoded after the setting is turned on report cells.

```c
fa_stream_set_tracker_param(song, FA_TRACKER_CELL_EVENTS, 3);   /* both */
/* in the event callback */
if (ev->type == FA_EVENT_TRACKER_SYNC && (ev->value & 0xFFFF) == 0x42) { /* a cue */ }
```

```csharp
song.TrackerCellEvents = TrackerCellEvents.Instruments | TrackerCellEvents.Sync;
song.TrackerInstrumentPlayed += (_, e) => Light(e.Channel, e.Value);   // e.Value: the instrument
song.TrackerSyncReached += (_, e) => { if (e.Value == 0x42) Cue(); };
```

## Subsongs

Some modules hold several songs. They are the stream's audio tracks: `fa_stream_info.audio_track_count` is the subsong count, and `fa_open_options.audio_track` (.NET `StreamOptions.AudioTrack`) picks the one to play (0 by default). A subsong past the last returns `FA_ERROR_NOT_FOUND` from the open. `fa_tracker_info.subsong` (.NET `TrackerInfo.Subsong`) says which one plays, the names are in `FA_TRACKER_TEXT_SUBSONG` (.NET `TrackerInfo.Subsongs`), and the restart position and the duration are the playing subsong's.

Orders and rows count in the module as a whole, but times count from the start of the playing subsong. `fa_stream_get_tracker_time` and `fa_stream_seek_tracker` return `FA_ERROR_NOT_FOUND` for a row that belongs to another subsong.

```c
fa_open_options opts;
fa_open_options_init(&opts);
opts.audio_track = 2;               /* the third subsong */
fa_stream* song = NULL;
if (fa_stream_open_path(engine, "multi.it", &opts, &song) == FA_ERROR_NOT_FOUND)
    puts("the module has fewer than three subsongs");
```

```csharp
using var song = engine.OpenFile("multi.it", new StreamOptions { AudioTrack = 2 });
Console.WriteLine($"subsong {song.TrackerInfo!.Subsong} of {song.TrackerInfo.Subsongs.Count}");
```

## Render settings

Settings change while the song plays and are heard after the decode-ahead buffer (150 ms by default, as for MIDI): libopenmpt applies them at the start of each block of 512 frames. They are set per stream; a value outside its range returns `FA_ERROR_INVALID_ARG` (.NET: `ArgumentOutOfRangeException`), as does a stream that is not a module.

| `fa_tracker_param` | .NET | Values | Default |
|---|---|---|---|
| `FA_TRACKER_INTERPOLATION` | `TrackerInterpolation` | 0 libopenmpt's default (8-tap sinc), 1 none (zero-order hold, the crunchy sound of early trackers), 2 linear, 4 cubic, 8 sinc | 0 |
| `FA_TRACKER_STEREO_SEPARATION` | `TrackerStereoSeparation` | percent, 0 (mono) to 200 | 100 |
| `FA_TRACKER_VOLUME_RAMPING` | `TrackerVolumeRamping` | -1 default, 0 off (may click), 1 to 10 softer ramps | -1 |
| `FA_TRACKER_AMIGA_RESAMPLER` | `TrackerAmigaResampler` | 1 Amiga MOD files sound as on an Amiga's Paula chip (about 19 times the CPU on those files), 0 off (the ordinary interpolation) | 0 (off, as in libopenmpt) |
| `FA_TRACKER_ROW_EVENTS` | `TrackerRowEvents` | 0 none, 1 an event for every row, 2 only the first row of each order (see [Row events](#row-events)) | 0 |

```c
fa_stream_set_tracker_param(song, FA_TRACKER_INTERPOLATION, 2);         /* linear */
fa_stream_set_tracker_param(song, FA_TRACKER_STEREO_SEPARATION, 50);
int32_t ramping;
fa_stream_get_tracker_param(song, FA_TRACKER_VOLUME_RAMPING, &ramping);
```

```csharp
song.TrackerInterpolation = TrackerInterpolation.Cubic;   // Default, None, Linear, Cubic, Sinc
song.TrackerStereoSeparation = 50;
song.TrackerVolumeRamping = 5;
song.TrackerAmigaResampler = true;      // the vintage Paula sound
```

## Channel mute

`fa_stream_set_tracker_channel_mute` silences one pattern channel (0 to `channels - 1`), live; the module keeps running, so notes sound again when you unmute. Only the first **64** channels can be muted: a higher channel returns `FA_ERROR_INVALID_ARG`, like an unknown one. Mutes are per stream and start off. If libopenmpt offers no channel control for a module, the call returns `FA_ERROR_NOT_IMPLEMENTED` (.NET throws).

```c
fa_stream_set_tracker_channel_mute(song, 2, 1);
int32_t muted;
fa_stream_get_tracker_channel_mute(song, 2, &muted);
```

```csharp
song.SetTrackerChannelMuted(2, true);
bool muted = song.IsTrackerChannelMuted(2);
```

## Tags

`fa_tags_*` and `fa_stream_get_tags` read a module's title (`FA_TAG_TITLE`), artist (`FA_TAG_ARTIST`) and song message (`FA_TAG_COMMENT`) as `FA_TAG_FORMAT_TRACKER` items (.NET `TagFormat.Tracker`). Reading tags from a path needs no engine and no playback; see [tags.md](tags.md).

```csharp
var tags = song.GetTags(TagReadOptions.NoInfo);
Console.WriteLine($"{tags.Title} / {tags.Artist}");
```

## Limits and pitfalls

- Files over **128 MB** are refused with `FA_ERROR_UNSUPPORTED_FORMAT`. libopenmpt reads the file through the stream's source while it opens, so the file is not held twice in memory (a module from memory, from your own I/O callbacks or from a URL works the same).
- A song longer than 6 hours is cut at 6 hours. A file libopenmpt reads with no playable length fails to open.
- Output is always stereo; there is no surround mode.
- Streams that are not modules return `FA_ERROR_INVALID_ARG` from every tracker call.
- libopenmpt renders differently from BASS's own MOD player (resampling, filtering, levels), so a song does not sound bit for bit as it did.
- There is no volume per instrument (libopenmpt has none): use the instrument mute, or the volume of the channel it plays on. See the [BASS migration table](../migration/bass.md#tracker-music).
