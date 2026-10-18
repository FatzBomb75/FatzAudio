# Tracker modules

FatzAudio plays tracker modules itself, with [libopenmpt](https://lib.openmpt.org) 0.8.9 built into the library. They open with the ordinary `fa_stream_open_*` calls and play, seek, loop, crossfade, normalize and stretch like any other stream. It replaces `BASS_MusicLoad`.

| What | Supported |
|---|---|
| Formats | MOD, S3M, XM, IT, MPTM, MTM, UMX (Unreal music), 669, AMF, AMS, DBM, DIGI, DMF, DSM, FAR, GDM, IMF, J2B, MDL, MED, MO3, MT2, OKT, PLM, PSM, PTM, SFX, STM, ULT, WOW and the other formats libopenmpt reads |
| Module information | Format, tracker program, title, artist, song message, channel, order, pattern, instrument, sample and subsong counts, restart position, duration |
| Names | Song message, instrument, sample, channel, pattern and subsong names (UTF-8) |
| Order and row | The time a row first plays, and seeking to an order and row |
| Live controls | Interpolation, stereo separation, volume ramping, Amiga resampler emulation, channel mute, plus every stream parameter (volume, tempo, pitch) |
| Tags | Title, artist and song message as `FA_TAG_FORMAT_TRACKER` items |

Files are detected from their content, never from the extension, so a module with the wrong extension, from memory, from your own I/O callbacks or from a URL still plays. `fa_stream_get_info` reports stereo F32 audio at the engine's `mix_sample_rate`, or 44100 Hz when that is 0 (device native), like MIDI. No add-on library and no sample bank are needed: modules carry their own samples.

MO3 files whose samples are MP3 or Ogg Vorbis compressed are not supported (`FA_ERROR_UNSUPPORTED_FORMAT`); MO3 files with plain samples play.

## Playing and looping

A module plays **once**. The length is libopenmpt's duration of the song (capped at 6 hours), and the stream plays exactly that many frames. The module's own loop (the jump back to its restart position at the end) is not followed, so the length is always known and seeking, `FA_EVENT_ENDED`, crossfades and queues work as for a file. A module with several subsongs plays its first subsong; the others are listed by name but cannot be selected yet. To repeat the song, use the stream's loop toggle: it starts the whole song again from the beginning (`fa_stream_set_loop`, .NET `Loop`), not at the module's restart order. The restart position is reported in the module information if you want to build that yourself with an order/row seek.

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
| `duration` | Seconds, the song played once |

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

A module is a list of orders (each names a pattern) of rows. `fa_stream_get_tracker_time` gives the time, in seconds from the start, where the song first plays a row; `fa_stream_seek_tracker` seeks there. Both count from 0. `FA_ERROR_NOT_FOUND` means the song never plays that row: an order that is skipped by a jump, a row past the end of that order's pattern, or an order that belongs to a later subsong (.NET: `GetTrackerTime` returns `null`, `SeekTracker` throws).

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

This is a map from the order list to time, for building a pattern jump list or a seek bar with order marks. Which order and row is being **heard** at this moment is not available yet 🗓️, nor are events raised at each row 🗓️ (planned: a "current position" call driven by the device clock, and a row sync event like `BASS_SYNC_MUSICPOS`). Until then you can follow the position yourself: build a table of `GetTrackerTime` for every order, and find the order with the clock.

## Render settings

Settings change while the song plays and are heard after the decode-ahead buffer (150 ms by default, as for MIDI): libopenmpt applies them at the start of each block of 512 frames. They are set per stream; a value outside its range returns `FA_ERROR_INVALID_ARG` (.NET: `ArgumentOutOfRangeException`), as does a stream that is not a module.

| `fa_tracker_param` | .NET | Values | Default |
|---|---|---|---|
| `FA_TRACKER_INTERPOLATION` | `TrackerInterpolation` | 0 libopenmpt's default (8-tap sinc), 1 none (zero-order hold, the crunchy sound of early trackers), 2 linear, 4 cubic, 8 sinc | 0 |
| `FA_TRACKER_STEREO_SEPARATION` | `TrackerStereoSeparation` | percent, 0 (mono) to 200 | 100 |
| `FA_TRACKER_VOLUME_RAMPING` | `TrackerVolumeRamping` | -1 default, 0 off (may click), 1 to 10 softer ramps | -1 |
| `FA_TRACKER_AMIGA_RESAMPLER` | `TrackerAmigaResampler` | 1 Amiga MOD files sound as on an Amiga's Paula chip, 0 off (the ordinary interpolation) | 1 (on) |

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
song.TrackerAmigaResampler = false;
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

- Files over **128 MB** are refused with `FA_ERROR_UNSUPPORTED_FORMAT`; a module is read into memory before it opens, and libopenmpt keeps what it needs.
- A song longer than 6 hours is cut at 6 hours. A file libopenmpt reads with no playable length fails to open.
- Output is always stereo; there is no surround mode.
- Streams that are not modules return `FA_ERROR_INVALID_ARG` from every tracker call.
- libopenmpt renders differently from BASS's own MOD player (resampling, filtering, levels), so a song does not sound bit for bit as it did.
- Not available yet 🗓️: the order and row being heard, row sync events, and the live module controls BASS has as `BASS_ATTRIB_MUSIC_*` (tempo, speed, per-channel and per-instrument volume). See the [BASS migration table](../migration/bass.md#tracker-music).
