# CD+G karaoke graphics

CD+G songs pair an audio track with a stream of graphics that draws the lyrics, 300 small "packs" per second. FatzAudio plays the graphics as a side stream of the audio: you open the song as usual, ask a **surface** for the picture from your render loop, and copy its pixels to whatever you draw with. The picture always matches what is heard, so pausing, seeking, looping, crossfades and tempo changes need no sync code in the app.

| Where the graphics come from | What to do |
|---|---|
| MP3+G pairs (`song.mp3` + `song.cdg` in one folder), or any other audio format with a `.cdg` of the same name | Nothing: `fa_stream_open_path` loads the `.cdg` automatically (any case: `.cdg`, `.CDG`) |
| A ZIP archive holding the audio and the `.cdg` | Nothing: `fa_stream_open_path` or `fa_stream_open_memory` of the `.zip` plays the audio inside and loads the graphics |
| Anything else: another file name, a web stream, the FFmpeg hand-off, a `.cdg` your app already has in memory | `fa_stream_load_cdg_path` / `fa_stream_load_cdg_memory` on the opened stream |
| Audio CDs with CD+G (a drive, or a `.cue` image with a `.sub` file) | Nothing: `fa_stream_open_disc` reads the graphics from the disc's subcode while the track plays ([disc playback](disc.md#cdg-karaoke-discs)) |

`fa_stream_info.has_cdg` is 1 when a stream has graphics. On a disc track it turns 1 once the graphics are found, usually in the first second of play, so create the surface anyway: a surface of a stream without graphics stays blank until they arrive.

## C

```c
#include <fatzaudio/fatzaudio.h>

fa_stream* song;
fa_stream_open_path(engine, "Artist - Song.mp3", NULL, &song);   /* finds "Artist - Song.cdg" */

fa_stream_info info;
fa_stream_get_info(song, &info);
if (!info.has_cdg) { /* show a cover image instead */ }

fa_cdg_surface_config cfg;
fa_cdg_surface_config_init(&cfg);
cfg.pixel_format = FA_CDG_PIXELS_BGRA32;   /* Direct3D, WPF, SkiaSharp, GDI */
cfg.width = 1200; cfg.height = 864;        /* 0 = native 300x216 */

fa_cdg_surface* cdg;
fa_cdg_surface_create(song, &cfg, &cdg);

fa_stream_attach(song, device, NULL);
fa_stream_play(song);

/* In your render loop (each vsync, or a 60 Hz timer): */
fa_cdg_frame frame;
fa_cdg_surface_update(cdg, &frame);
if (frame.changed) {
    /* Upload only the part that changed. */
    const uint8_t* first = (const uint8_t*)frame.pixels
                         + frame.dirty_y * frame.stride + frame.dirty_x * 4;
    upload_to_texture(first, frame.stride,
                      frame.dirty_x, frame.dirty_y, frame.dirty_width, frame.dirty_height);
}

/* ... */
fa_cdg_surface_destroy(cdg);   /* before or after closing the stream */
fa_stream_close(song);
```

### Direct3D 11

```c
D3D11_BOX box = { frame.dirty_x, frame.dirty_y, 0,
                  frame.dirty_x + frame.dirty_width, frame.dirty_y + frame.dirty_height, 1 };
const uint8_t* src = (const uint8_t*)frame.pixels + frame.dirty_y * frame.stride + frame.dirty_x * 4;
ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)texture, 0, &box, src, frame.stride, 0);
/* texture: DXGI_FORMAT_B8G8R8A8_UNORM, 300x216 (or the size you configured) */
```

### OpenGL

```c
/* cfg.pixel_format = FA_CDG_PIXELS_RGBA32 */
glBindTexture(GL_TEXTURE_2D, tex);
glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride / 4);
glTexSubImage2D(GL_TEXTURE_2D, 0, frame.dirty_x, frame.dirty_y, frame.dirty_width, frame.dirty_height,
                GL_RGBA, GL_UNSIGNED_BYTE,
                (const uint8_t*)frame.pixels + frame.dirty_y * frame.stride + frame.dirty_x * 4);
glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
```

### Graphics from elsewhere

```c
fa_stream* radio;
fa_stream_open_url(engine, "https://example.com/karaoke/song.mp3", NULL, &radio);
fa_stream_load_cdg_memory(radio, cdg_bytes, cdg_size);   /* copied */

fa_stream_load_cdg_path(song, "other-name.cdg");          /* replaces what it had */
fa_stream_load_cdg_path(song, NULL);                      /* removes the graphics */
```

## C#

```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();
using var song = engine.OpenFile("Artist - Song.zip");     // audio + .cdg inside
using var cdg = song.CreateCdgSurface(new CdgSurfaceOptions { Premultiplied = true });

var bitmap = new WriteableBitmap(300, 216, 96, 96, PixelFormats.Pbgra32, null);
lyrics.Source = bitmap;                                    // an <Image> in your window
song.PlayOn(output);

CompositionTarget.Rendering += (_, _) =>                   // WPF, once per frame
{
    var frame = cdg.Update();
    if (!frame.Changed) return;
    var d = frame.Dirty;
    bitmap.WritePixels(new Int32Rect(d.X, d.Y, d.Width, d.Height),
        frame.PixelPointer + d.Y * frame.Stride + d.X * 4,
        frame.Stride * frame.Height, frame.Stride);
};
```

Other members: `song.HasCdg`, `song.LoadCdg(path)`, `song.LoadCdg(bytes)`, `song.RemoveCdg()`, `song.CdgOffset`, `cdg.UpdateAt(time)`. `StreamOptions.NoCdg` skips the automatic lookup. `CdgFrame` is a `ref struct`: read its pixels before the next `Update` call.

SkiaSharp:

```csharp
var info = new SKImageInfo(frame.Width, frame.Height, SKColorType.Bgra8888, SKAlphaType.Premul);
bitmap.InstallPixels(info, frame.PixelPointer, frame.Stride);   // or copy with frame.Pixels
```

## Surfaces

A surface turns the graphics into pixels for one output. Each has its own pixel format, size and options, so a small preview window and a full-screen TV output can show the same stream.

| Option | Values |
|---|---|
| `pixel_format` | `FA_CDG_PIXELS_BGRA32` (default: Direct3D/DXGI B8G8R8A8, WPF Bgra32/Pbgra32, SkiaSharp Bgra8888, GDI DIBs), `FA_CDG_PIXELS_RGBA32` (OpenGL, Vulkan, Metal RGBA8, Android bitmaps), `FA_CDG_PIXELS_INDEXED8` (one byte per pixel, index into `frame.palette`) |
| `width`, `height` | Output size, up to 8192. 0 = native: 300x216 with the border, 288x192 without |
| `FA_CDG_NO_BORDER` | Only the 288x192 picture area. The border is usually a plain colour; leave it out when you letterbox yourself |
| `FA_CDG_TRANSPARENT` | The background colour (the colour of the last screen clear) is fully transparent, so the lyrics can be drawn over video. Colours a disc marks with the "transparent colour" instruction get that transparency (0 opaque .. 63 invisible) |
| `FA_CDG_PREMULTIPLIED` | Colour multiplied by alpha, for WPF `Pbgra32`, Direct2D and most compositors. Without it alpha is straight |
| `FA_CDG_SMOOTH` | Bilinear scaling. Without it, scaling keeps sharp square pixels (the classic look). Ignored for INDEXED8 |
| `FA_CDG_TRANSPARENT_BACKGROUND_ONLY` | Like `FA_CDG_TRANSPARENT`, but only the background colour is transparent: transparent-colour packs are ignored. Some discs set them in ways that punch holes in the lyrics. Wins over `FA_CDG_TRANSPARENT` |
| `FA_CDG_HIDE_UNTIL_PRESET` | Every colour is fully transparent until the first screen clear (memory preset), so the picture or video underneath shows instead of a black screen at the start of a song |
| `FA_CDG_BACKGROUND_COLOR` + `background_color` | Replaces the background colour with your own, `0xAARRGGBB`, alpha included |
| `FA_CDG_BACKGROUND_OPACITY` + `background_opacity` | How opaque the background is, 0..100 % of 255 (or of `background_color`'s alpha), whatever the transparency flags make it. A host's "background opacity" slider |

None of these change what a surface draws without them. Apps built against the older, shorter `fa_cdg_surface_config` keep working: pass its `struct_size` as before.

`fa_cdg_surface_update` returns:

| Field | Meaning |
|---|---|
| `pixels`, `stride`, `width`, `height` | The picture. Owned by the surface, unchanged until its next update or destroy |
| `changed` | 1 when the pixels or the palette differ from the previous update. Always 1 the first time |
| `dirty_x`, `dirty_y`, `dirty_width`, `dirty_height` | The pixels that changed, in output pixels. Usually one 6x12 tile per pack while lyrics are being coloured in, so uploads stay tiny |
| `palette_changed` | 1 when `palette` changed. With INDEXED8 a colour-table change recolours the screen without touching a pixel, so the dirty rectangle is empty: update your palette (or shader lookup) instead |
| `palette[16][4]` | R, G, B, A of the 16 colours (straight alpha). A is 255 unless a transparency or background option is set, and RGB never has alpha folded in |
| `time` | The position on the graphics timeline that is shown |
| `background_index` | Colour of the last memory preset (screen clear); -1 before the first one and for streams without graphics |
| `border_index` | Colour of the last border preset; -1 before one |
| `transparency[16]` | The values of the last transparent-colour pack, 0 opaque .. 63 invisible, whatever the surface's flags |
| `background[4]` | R, G, B, A the background is drawn with, options applied: fill the area around a scaled picture with it. All 0 while `background_index` is -1 and no `FA_CDG_BACKGROUND_COLOR` is set |
| `revision` | Goes up by one whenever an update changes a pixel (1 after the first update). Never goes down, also not on seeks |
| `palette_revision` | Goes up whenever `palette`, `background_index`, `border_index` or `transparency` change |

### Your own compositor (INDEXED8)

Apps that scale, colour and blend the picture themselves take INDEXED8 pixels and the raw state, and apply their own policy. A typical setup: one INDEXED8 surface per stream at native size, flags 0, updated with your own presentation clock.

```c
cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;   /* width = height = 0, flags = 0 */
fa_cdg_surface_create(song, &cfg, &cdg);

/* Presentation thread, once per displayed frame: */
fa_cdg_surface_update_at(cdg, my_presentation_time, &frame);
for (each render target t) {
    if (t->revision != frame.revision) { upload_indices(t, &frame); t->revision = frame.revision; }
    if (t->palette_revision != frame.palette_revision) {
        upload_palette(t, frame.palette);    /* RGB; alpha is 255 with flags 0 */
        t->palette_revision = frame.palette_revision;
    }
}
/* In the shader: skip pixels equal to frame.background_index (draw nothing at all
 * while it is -1), draw every other index opaque. transparency[] is there if you
 * want to honour the disc's transparent-colour packs. */
```

The revisions let one surface feed several render targets (a main window and a projector window, each with its own textures): each target compares them with what it last uploaded. `changed` and the dirty rectangle are relative to the previous update and suit a single target.

### Graphics info

`fa_stream_get_cdg_info` says where a stream's graphics come from (`FA_CDG_SOURCE_FILE` for a `.cdg`, ZIP entry or memory block, `FA_CDG_SOURCE_DISC` for a disc's subcode), how many packs there are, and how far apart the seek checkpoints are. For a disc track, `disc_parity` is 1 when the subcode layout was chosen by the packs' P/Q parity and 0 when it was chosen by counting instructions (drives that return the parity zeroed). Either way the packs are applied as read: there is no error correction. In .NET this is `song.CdgInfo`.

## Sync

Pack *n* of a `.cdg` belongs at *n* / 300 seconds of the audio. `fa_cdg_surface_update` reads the stream's clock (`fa_stream_get_clock`: the audio leaving the speaker right now, device latency included) and draws every pack up to that moment. There is nothing to keep in step:

- **Pause, seek, loop, crossfade, queue:** the clock follows the audio, and so do the graphics.
- **Tempo and pitch:** the clock is a position in the song, so slower or faster playback moves the graphics with it.
- **Out-of-sync files:** `fa_stream_set_cdg_offset(song, 150000000)` shows the graphics 150 ms later (negative: earlier).
- **Your own clock** (rendering a karaoke video, a scrub preview): `fa_cdg_surface_update_at(cdg, time, &frame)` draws the picture at any time you choose. The CD+G offset still applies.

The exact rule, for apps that compare their own decoder frame for frame: at time *t* (in seconds, after subtracting the offset) the picture shows packs [0, ceil(*t* × 300)) applied. Pack *n* occupies [*n*/300, (*n*+1)/300) and counts as soon as its slot starts, so at *t* = 1 s exactly packs 0..299 are applied, and at 1 s + 1 ns packs 0..300. *t* ≤ 0 shows no pack.

Drawing runs on the thread that calls update, not on the audio thread, and costs microseconds per frame during playback. Only the packs are read under the stream's lock; each surface draws on its own, so a large TV surface never holds up a small preview.

### Seeking

The graphics carry checkpoints, the decoder state every 10 s of packs, shared by all surfaces of the stream. A `.cdg` file, ZIP entry or memory block gets all of them when it is loaded (about 5 ms for a dense 5-minute song); a disc track gets them as its packs arrive, and redoes them if a part is read again with different packs. Any seek, back or forward, the first one too and into parts never played, restores the checkpoint before the target and replays at most 10 s of packs: well under a millisecond, so a seek-bar drag that sends many seeks per second stays cheap. A seek also uploads only the area that differs from what was shown. Graphics longer than 30 minutes space the checkpoints further apart (20 s up to an hour, and so on), which keeps their memory under about 12 MB.

## ZIP archives

`fa_stream_open_path`, `fa_stream_open_memory` and `fa_stream_open_io` recognise a ZIP by its content. Inside, the first `.cdg` is the graphics, and the audio is the entry with the same name (`Song.mp3` next to `Song.cdg`), else the first other entry FatzAudio can decode. Stored and deflate-compressed entries are read (the decompressor is built in). A damaged `.cdg` entry leaves a playable song without graphics. ZIP64, encrypted and multi-part archives are not supported (`FA_ERROR_UNSUPPORTED_FORMAT`); karaoke downloads never use them.

Only what is needed is read: the archive's directory at the end of the file, then the entries used. The whole file is never loaded.

| Audio entry | How it plays | Memory |
|---|---|---|
| Stored (uncompressed, the usual case) | Straight from the archive, like a plain file | None beyond a plain file's |
| Compressed, up to the memory limit (256 MB unless `fa_engine_config.zip_memory_limit_mb` says otherwise) | Unpacked into memory at open, CRC checked | The song's unpacked size |
| Compressed, bigger than the limit (or memory ran out), with `fa_engine_config.temp_directory` set | Unpacked into a temp file at open, CRC checked, then played from it | About 1 MB while unpacking |
| Compressed, bigger than the limit, no temp directory | Not opened: `FA_ERROR_TOO_LARGE` (.NET `ErrorCode.TooLarge`). Tell the user to extract the archive, or set a temp directory | None |

The `.cdg` is always unpacked into memory (a song's graphics are a few MB).

A stored entry played in place is not CRC checked at open, since that would read the whole song first. A damaged byte then plays like a damaged byte in an unzipped MP3: a short glitch, never a crash. Compressed entries are always checked while they are unpacked.

The temp directory is off by default, so FatzAudio never writes to disk unless the app asks. When it is set, FatzAudio works in a `fatzaudio-tmp` folder inside it and never touches anything else there. The directory and that folder are created if missing (not the directory's parents); if they cannot be written when a song needs them, the open fails with `FA_ERROR_IO`. Each unpacked song is deleted when its stream closes, and also if the app crashes: Windows opens the file with delete-on-close, and Linux, macOS, iOS and Android remove its name as soon as it exists. Files left by a power cut are deleted the next time an engine is created with that directory. On Android and iOS, pass the app's cache directory.

```c
fa_engine_config cfg;
fa_engine_config_init(&cfg);
cfg.temp_directory = "C:\\Users\\me\\AppData\\Local\\MyPlayer";   /* opt in */
fa_engine_create(&cfg, &engine);
```

```csharp
using var engine = AudioEngine.Create(new AudioEngineOptions { TempDirectory = Path.GetTempPath() });
```

`fa_stream_open_io` needs `seek` and `length` callbacks for a ZIP, because its directory is at the end (`FA_ERROR_NOT_SEEKABLE` without them). When the audio was unpacked, the archive is no longer read, but `close` is still called once, when the stream closes. Loudness scans and `fa_stream_get_tags` read a stored entry again from the archive file or block; a song unpacked into a temp file gets the running loudness estimate instead of a scan (as with app callbacks).

## The CD+G format

Every instruction of the CD+G "TV graphics" mode is implemented: memory preset (clear the screen), border preset, tile block in normal and XOR mode, scroll preset and scroll copy (horizontal and vertical, with the smooth-scroll offsets), define transparent colour, and both halves of the colour table. Repeated memory-preset packs (repeat count above 0) are skipped when the screen is still clean in that colour, so they cause no redraws. The palette starts black until the first colour-table load. CD+EG (extended graphics) packs are ignored, and so are packs of other subcode modes.

## Threading

- Surfaces can be created, updated and destroyed from any thread; one surface is updated from one thread at a time. Different surfaces, even of the same stream, may be updated from different threads.
- Loading or removing graphics while surfaces are being updated is safe: each surface starts over on the new graphics at its next update.
- An update never waits for the device or decode threads. It shares one short lock per stream with graphics loads (only the hand-over: checkpoints are built before it), a disc's pack writes and the stream's other surfaces. `fa_cdg_surface_update` also reads `fa_stream_get_clock`; `fa_cdg_surface_update_at` does not.
- An update with nothing new compares the palette and returns: no pixels are copied, so calling it at 240 Hz costs next to nothing.
- Destroy every surface you create with `fa_cdg_surface_destroy`, before or after closing its stream (not while `fa_stream_close` is running on another thread). After the stream is closed, updates return `FA_ERROR_INVALID_ARG`. In .NET, disposing the stream also disposes its surfaces, and `Update` then throws `ObjectDisposedException`.

## Pitfalls

- Read or copy `pixels` before the next update of the same surface; it is redrawn in place.
- Upload in the surface's pixel format. A BGRA surface into an RGBA texture swaps red and blue.
- Leave `FA_CDG_TRANSPARENT` off unless something is drawn behind the lyrics: on its own, the background turns black or invisible depending on the renderer.

## Not provided yet

- CD+EG extended graphics.
- A push model (the engine updating a surface and raising an event when the picture changed). Update from your render loop instead.
- Ready-made helpers for specific renderers (a D3D11 texture updater, a CVPixelBuffer on Apple). The pixel buffer above covers them all today.
