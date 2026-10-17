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

## Pitfalls

- Close every stream and device before `fa_engine_destroy`. In .NET the handles do this for you in any dispose order.
- Following the default device when it changes (headset plugged in) is planned with R14 and isn't wired up yet.
