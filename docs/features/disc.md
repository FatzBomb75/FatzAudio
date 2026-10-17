# Disc playback (audio CDs)

FatzAudio plays audio CDs from the computer's optical drive, and disc images (`.cue` + `.bin`/`.wav`) the same way. A track opens as an ordinary stream, so everything else works on it unchanged: crossfades, gapless queues, markers, normalization, buses, the presentation clock. It replaces basscd: there is no add-on to load, and the same code runs on Windows, macOS and Linux.

| What | How |
|---|---|
| List drives | `fa_disc_drive_count` + `fa_disc_drive_get_info` |
| Open a drive, or a `.cue` image | `fa_disc_open(id, options, &disc)` (`NULL` = the first drive) |
| Is there a disc? Was it changed? | `fa_disc_get_status`: state and a change counter |
| Tracks, lengths, disc ids | `fa_disc_get_toc` (freedb/CDDB and MusicBrainz ids included) |
| Album and track titles | `fa_disc_get_text` (CD-Text, plus UPC/EAN and ISRC codes) |
| Play a track | `fa_stream_open_disc(engine, disc, track, options, &stream)` |
| Windows `D:\Track01.cda` files | `fa_stream_open_path` opens them as their track |
| Tray, eject lock, read speed | `fa_disc_eject`, `fa_disc_load`, `fa_disc_set_locked`, `fa_disc_set_speed` |
| CD+G from the disc | Next step of this feature (🗓️); CD+G from files is ✅ ([CD+G](cdg.md)) |

## C

```c
#include <fatzaudio/fatzaudio.h>

fa_disc* disc;
if (fa_disc_open(NULL, NULL, &disc) != FA_OK) { /* no drive */ }

fa_disc_toc toc;
fa_result r = fa_disc_get_toc(disc, &toc);
if (r == FA_ERROR_NO_DISC) { /* ask for a disc */ }

char album[256], title[256];
if (fa_disc_get_text(disc, 0, FA_DISC_TEXT_TITLE, album, sizeof(album)) != FA_OK)
    snprintf(album, sizeof(album), "Unknown album");    /* no CD-Text: look it up online */
printf("%s, MusicBrainz id %s\n", album, toc.musicbrainz_id);

for (int i = 0; i < toc.track_count; i++) {
    const fa_disc_track* t = &toc.tracks[i];
    if (!t->is_audio) continue;                          /* the data part of an Enhanced CD */
    if (fa_disc_get_text(disc, t->number, FA_DISC_TEXT_TITLE, title, sizeof(title)) != FA_OK)
        snprintf(title, sizeof(title), "Track %d", t->number);
    printf("%2d  %s  %lld s\n", t->number, title, (long long)(t->duration / 1000000000));
}

/* Play track 1 now and queue track 2 right behind it, without a gap (as on the
 * disc). Queue each following track when the previous one starts (FA_EVENT_STARTED),
 * so only two tracks read from the drive at a time. */
fa_stream *now, *next;
fa_stream_open_disc(engine, disc, 1, NULL, &now);
fa_stream_open_disc(engine, disc, 2, NULL, &next);
fa_stream_attach(now, device, NULL);
fa_stream_set_next(now, next, 0, NULL);   /* 0 = gapless; attaches `next` too */
fa_stream_play(now);

fa_disc_close(disc);   /* the streams keep the drive open until they are closed */
```

Opening a stream reads no audio; an attached one fills its decode-ahead buffer from the disc, so queue one track ahead rather than the whole disc. Use `fa_crossfade` or `fa_stream_set_next(..., crossfade, ...)` for a DJ-style mix instead.

## C#

```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();

foreach (var drive in Disc.Drives) Console.WriteLine($"{drive.Id}: {drive.Name}");

using var disc = Disc.Open();                          // or Disc.Open("E:"), Disc.Open("album.cue")
var toc = disc.Toc;                                    // FatzAudioException(NoDisc) without a disc
Console.WriteLine($"{disc.GetText(0, DiscTextField.Title) ?? "Unknown album"} ({toc.MusicBrainzId})");

foreach (var t in toc.Tracks.Where(t => t.IsAudio))
    Console.WriteLine($"{t.Number,2}  {disc.GetText(t.Number, DiscTextField.Title) ?? $"Track {t.Number}"}  {t.Duration:mm\\:ss}");

using var track = engine.OpenDisc(disc, 1);
track.PlayOn(output);
```

Other members: `disc.Status` (`State`, `Locked`, `ChangeCount`), `disc.Eject()`, `disc.Load()`, `disc.Locked`, `disc.Speed`, `DiscOptions` (`ReadOffset`, `Retries`, `DeEmphasis`, `UseC2`), `toc[number]`, `toc.FreedbId`, `toc.FreedbQuery`.

## Drives

| Platform | Drive ids | Notes |
|---|---|---|
| Windows | `"D:"` (also `"D:\"`, `"d"`, `"\\.\D:"`) | No administrator rights needed |
| Linux | `"/dev/sr0"`, `"/dev/sr1"`, ... | The user needs read/write access to the device node, usually by being in the `cdrom` group (`FA_ERROR_DEVICE` otherwise) |
| macOS | `"disk2"` (also `"/dev/disk2"`, `"rdisk2"`) | Only drives with a disc in them are listed (macOS has no device node for an empty drive). Eject fails with `FA_ERROR_BUSY` while Finder has the disc mounted; closing the tray and the eject lock are not available |
| iOS, Android | none | No optical drives; disc images still open |

`fa_disc_open` opens a drive with or without a disc in it. Keep the `fa_disc*` for as long as your app shows the drive, and poll `fa_disc_get_status` about once a second: `change_count` goes up whenever a disc is removed or inserted, so a new disc is noticed even if it was swapped between two polls. After a change, call `fa_disc_get_toc` again. Status and TOC calls take a few milliseconds (the TOC is cached per disc, so repeated calls are free); don't call them on the audio thread.

A disc track stream belongs to the disc it was opened on. When the disc is taken out (or replaced, even by the same disc), the stream's next read fails with `FA_ERROR_NO_DISC` and the stream ends with `FA_EVENT_ENDED`.

## Table of contents and disc ids

`fa_disc_toc` lists every track with its number, start, length in sectors, frames and time, and its flags: `is_audio` (data tracks of Enhanced CDs cannot be played), `preemphasis`, `copy_permitted`, `four_channel`. 75 sectors make a second; a sector holds 588 stereo frames (`FA_DISC_SECTOR_FRAMES`).

On an Enhanced CD (audio session, then a data session) the last audio track ends before the gap between the sessions, so its length is the real one.

| Field | Use |
|---|---|
| `musicbrainz_id` | `https://musicbrainz.org/cdtoc/<id>` or the MusicBrainz web service. Computed from the audio session only, as MusicBrainz expects |
| `freedb_id` | The classic CDDB/freedb id (8 hex digits), also used by gnudb.org |
| `freedb_query` | The arguments of a CDDB `cddb query` command: id, track count, track offsets, length in seconds |

FatzAudio does not contact any server itself; your app looks the ids up where it wants to.

## CD-Text

`fa_disc_get_text(disc, track, field, ...)` returns the album's text for track 0 and a track's text for its number, in UTF-8: `FA_DISC_TEXT_TITLE`, `PERFORMER`, `SONGWRITER`, `COMPOSER`, `ARRANGER`, `MESSAGE`, and `GENRE` (album only). `FA_ERROR_NOT_FOUND` means the disc has no such text (most discs have no CD-Text at all).

`FA_DISC_TEXT_CODE` is the album's UPC/EAN (media catalog number) and a track's ISRC. They come from CD-Text when it has them, else from the disc's subcode; that costs one short disc access per code the first time.

CD-Text in ISO 8859-1 and ASCII is read (that covers Western discs). A disc with several languages gives the first block in one of those character sets; Japanese (MS-JIS) blocks are skipped. A "same as the previous track" entry is resolved.

## Reading: errors, offset, speed

Reads go through the drive's error correction, and FatzAudio adds:

- **Retries with C2 error pointers.** Drives that support it report which bytes of a sector they could not correct. Such a sector is read again (after reading a distant sector so the drive's cache can't return the same bad copy) up to `retries` times (default 5). If it never reads clean it plays as read, and a sector that cannot be read at all plays as silence. Either way playback continues, and the stream raises `FA_EVENT_DECODE_ERROR` with value `FA_ERROR_IO`. Drives without C2 support are detected on the first read and read without it. `FA_DISC_NO_C2` turns it off for drives that report C2 wrongly.
- **Read offset correction.** Most drives deliver audio a few samples early or late. Set `fa_disc_options.read_offset` to your drive's correction from the AccurateRip drive database (for example `+6`, common for many current drives), and every sample comes from its exact place on the disc. Samples the offset would take from before the first audio track, or past the last audio of a session (into a data track or the gap before the next session), read as silence.
- **De-emphasis.** Tracks mastered with pre-emphasis (the `PRE` flag, common on 1980s discs) are de-emphasized with the standard 50/15 µs curve. `FA_DISC_NO_DEEMPHASIS` plays them as recorded.
- **Speed.** `fa_disc_set_speed(disc, 4)` reads at 4x; `0` lets the drive run at full speed. Slower is quieter and often reads scratched discs better. `fa_disc_get_speed` reports what the drive picked.

Audio is read about 0.4 s at a time on the stream's decode worker, never on the audio thread; the decode-ahead buffer (`buffer_ms`) covers drive spin-ups and retries. For very damaged discs, raise `buffer_ms` (for example 3000) so retries never reach the speakers.

Normalization uses its running estimate for disc tracks (a disc isn't read twice for a loudness scan). If your app stores loudness per disc (by MusicBrainz id), give it back with `fa_stream_set_loudness`.

## Disc images

`fa_disc_open("album.cue", ...)` opens an image like a drive: same TOC, ids, CD-Text and streams. Supported cue sheets:

- `FILE` with `BINARY` (raw 2352-byte sectors, little-endian samples), `MOTOROLA` (big-endian samples) or `WAVE` (44.1 kHz 16-bit stereo `.wav`, as EAC writes it). Several files are fine (one per track, or one for the whole disc).
- `TRACK nn AUDIO`, and `MODE1/2352`, `MODE1/2048`, `MODE2/2352` data tracks.
- `INDEX 00` / `INDEX 01`, `PREGAP` (silence that is not in the file), `FLAGS PRE DCP 4CH`.
- `TITLE`, `PERFORMER`, `SONGWRITER`, `COMPOSER`, `ARRANGER`, `MESSAGE` (album or track), `REM GENRE`: returned as CD-Text.
- `CATALOG` and `ISRC`: returned as codes.
- `REM SESSION 02` before a track starts a second session (Enhanced CD images).

Images have no tray, lock or speed: those calls return `FA_ERROR_NOT_IMPLEMENTED`.

## Pitfalls

- **Analog CD playback** (basscd's `BASS_CD_Analog_*`, the drive's own DAC and headphone socket) is not provided; modern drives have no analog output.
- **Open the stream on the thread you like**, but don't call disc functions from an event callback that must return quickly: a status or TOC call waits for the drive.
- **Two streams of one disc** (a crossfade between two tracks) share the drive; reads are serialized and work, but the drive head moves back and forth, so keep crossfades on a disc short or raise `buffer_ms`.
- **Tracks are numbered as on the disc.** Most discs start at 1, but use `toc.first_track` and `tracks[i].number`, never the array index.
