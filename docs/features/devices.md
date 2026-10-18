# Output devices

An `fa_device` is an opened playback device. Streams are attached to it and mixed into it. You always hold the device handle and pass it explicitly. There's no "current device" per thread as in BASS.

## C

```c
fa_engine* engine;
fa_engine_create(NULL, &engine);

/* List playback devices */
uint32_t n = 0;
fa_device_count(engine, FA_DEVICE_PLAYBACK, &n);
for (uint32_t i = 0; i < n; i++) {
    fa_device_info di;
    fa_device_get_info(engine, FA_DEVICE_PLAYBACK, i, &di);
    printf("%s%s\n", di.name, di.is_default ? " (default)" : "");
}

/* Open the system default (NULL), or a specific one by di.id */
fa_device* out;
fa_device_open(engine, NULL, &out);

fa_device_start(out);          /* idempotent: starting twice is fine */
int32_t running;
fa_device_is_started(out, &running);
fa_device_stop(out);           /* idempotent; attached streams keep their position */

fa_device_close(out);          /* detaches any streams still on it */
```

## C#

```csharp
using var engine = AudioEngine.Create();
foreach (var d in engine.OutputDevices)
    Console.WriteLine($"{d.Name}{(d.IsDefault ? " (default)" : "")}");

using var output = engine.OpenOutput();      // system default
output.Start();
Console.WriteLine(output.IsStarted);         // true
output.Stop();
```

## Cue output: previewing on a second device

A media player or karaoke host often previews the next song on headphones while the room hears the current one. Open a second device for the headphones and attach the preview stream to it. Each device has its own mix, volume, buses, limiter and levels, and the two never affect each other.

Open the cue device by its id from the device list, not with NULL, so it stays on the headphones whatever the system default is.

```c
fa_device *room, *cue;
fa_device_open(engine, NULL, &room);                    /* the main output */
fa_device_open(engine, headphones_info.id, &cue);       /* from fa_device_get_info */
fa_device_start(room);
fa_device_start(cue);

fa_stream_attach(now_playing, room, NULL);
fa_stream_play(now_playing);

fa_stream_attach(next_song, cue, NULL);                 /* preview it */
fa_stream_play(next_song);

/* Put the previewed song on air where it is: attaching to the other device
 * keeps its position (and it keeps playing). */
fa_stream_attach(next_song, room, NULL);
```

```csharp
var headphones = engine.OutputDevices.First(d => d.Name.Contains("Headphones"));
using var room = engine.OpenOutput();               // the main output
using var cue = engine.OpenOutput(headphones);      // stays on the headphones

nowPlaying.PlayOn(room);
nextSong.PlayOn(cue);                               // preview
nextSong.PlayOn(room);                              // on air, from where it is
```

A stream plays on one device at a time. To hear the same song in both places (the room and the host's headphones), open it twice and play one copy on each device. Two devices run on separate hardware clocks, so the copies can drift apart by a few milliseconds over a long song. For a monitor that follows the room exactly, use a headphone socket that mirrors the speakers on the same sound card.

## Silent output for tests and servers

Set `FA_ENGINE_NULL_OUTPUT` in `fa_engine_config.flags` (in .NET, `new AudioEngineOptions { NullOutput = true }`). Devices then play into a silent output that still runs in real time on its own thread. Streams advance, end and raise events exactly as they would on speakers. Use it for automated tests, CI machines without sound hardware, and headless services. It replaces BASS's "no sound" device (`BASS_Init(0, ...)`).

```c
fa_engine_config cfg;
fa_engine_config_init(&cfg);
cfg.flags = FA_ENGINE_NULL_OUTPUT;
fa_engine_create(&cfg, &engine);
```

```csharp
using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
```

## Behaviour

| Call | Behaviour |
|---|---|
| `fa_device_start` on a running device | `FA_OK`, nothing happens |
| `fa_device_stop` on a stopped device | `FA_OK`, nothing happens |
| `fa_device_stop` with streams playing | Output goes silent. Streams keep their state and position, and continue on the next start |
| `fa_device_close` with streams attached | Streams are detached, keep their position, and can be attached to another device |

## When the device changes or goes away

| What happens | What FatzAudio does |
|---|---|
| The system default changes (a headset plugged in) and the device was opened as the default (`device_id` NULL, .NET `OpenOutput()`) | Windows (WASAPI) and macOS/iOS (Core Audio) move the output to the new default by themselves; every stream on it gets `FA_EVENT_DEVICE_CHANGED` with value 0 (.NET `DeviceChanged`). On Linux, PulseAudio and PipeWire move the stream themselves without telling apps, so no event; plain ALSA doesn't follow |
| The device is unplugged or disabled while started (a chosen USB interface, or the default on a system that doesn't reroute) | The engine reopens the output on the system default at the same mix rate. Streams stay attached and play on from where they were, the device clock keeps counting, and every stream gets `FA_EVENT_DEVICE_CHANGED` with value 1. The `fa_device*` stays valid: from then on it is the default device, with the master volume you set (`fa_device_set_volume`). The system gets about 0.3 s to reroute a default device itself first, so a default output is never reopened twice |
| No output is available at all | Silence; the engine tries again every second, and the device starts by itself once an output appears (`fa_device_is_started` keeps reporting what you asked for, and `fa_device_set_volume` still takes a volume for it) |

The latency can differ on the new output; `fa_device_get_latency` and the presentation clock pick it up at once. A device that comes back (the USB interface plugged in again) is not switched back to: reopen it with `fa_device_open` and move the streams with `fa_stream_attach` if you want it.

## Pitfalls

- Close every stream and device before `fa_engine_destroy`. In .NET the handles do this for you in any dispose order.
