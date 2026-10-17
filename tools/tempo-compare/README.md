# tempo-compare

Black-box listening and measurement tools for tempo and pitch ([feature guide](../../docs/features/tempo-pitch.md)).

| Tool | What it does |
|---|---|
| `fa_tempo_render` | Renders a file through FatzAudio to a 32-bit float WAV: `fa_tempo_render in out.wav [--tempo x] [--pitch semitones] [--rate x] [--fast] [--formants]` |
| `bassfx_render` (Windows) | The same through bass_fx, loading `bass.dll` and `bass_fx.dll` from a folder at run time and using only their public API: `bassfx_render <bass folder> in out.wav [--tempo x] [--pitch semitones] [--rate x]` |
| `compare.py` | Generates test signals (sine, chord, harmonic tone, drum hits), renders them through every engine it finds and scores each result against the exactly known ideal. Writes `report.md` and all renders. Needs Python 3 with numpy |

Neither BASS DLL is in the repository or linked at build time; point `--bass-dir` (or the first `bassfx_render` argument) at your own copy.

## Windows

```bat
cmake -S . -B build
cmake --build build --config Release
pip install numpy
python tools\tempo-compare\compare.py --fa build\bin\Release\fa_tempo_render.exe ^
    --bassfx build\bin\Release\bassfx_render.exe --bass-dir C:\path\to\legacy --out tempo-results
```

SoundTouch (`soundstretch`) and Rubber Band (`rubberband`) are included when found on `PATH`, or with `--soundstretch` / `--rubberband`.

## Linux

`fa_tempo_render` builds everywhere. `bassfx_render.exe` can be cross-built with MinGW-w64 and run with `--wine wine64`.

## Scores

Each metric is described at the top of `compare.py`. In short: tone, chord and voice are signal-to-error dB against clean sines at the expected frequencies (higher is better); cents is the pitch error; attack is the pre-echo before drum hits and timing how far the hits moved; length is the output length against `input / (tempo × rate)`.
