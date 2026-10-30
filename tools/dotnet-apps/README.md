# .NET app checks (CI)

Two small apps that load the .NET binding inside a real .NET for iOS / .NET for Android app process,
which the plain-C test programs of the `devices` workflow cannot do. Each opens a test file, decodes it,
and (when a URL is given) opens an HTTPS stream through the platform's own stack, then prints
`FATZAUDIO-APP-OK` or `FATZAUDIO-APP-FAIL` and exits. They are built and run by the `dotnet-apps` jobs of
`.github/workflows/dotnet-apps.yml`; nothing here ships.

- `shared/AppCheck.cs`: the checks, linked into both apps.
- `ios/`: `net10.0-ios` app for the simulator. The native `fatzaudio.xcframework` is linked as a framework, which
  answers whether `LibraryImport("fatzaudio")` finds it (the binding also tries `@rpath/fatzaudio.framework/fatzaudio`).
  Inputs come from `SIMCTL_CHILD_FA_APP_FILE` and `SIMCTL_CHILD_FA_APP_URL`.
- `android/`: `net10.0-android` app for the emulator. It runs the checks on start-up and logs to logcat under the tag
  `FatzAudioApp`. Inputs come from intent extras (`--es file ...`, `--es url ...`). Its network security config trusts
  the CI test certificate authority copied to `Resources/raw/test_ca.crt`.

Run by hand: see the jobs in `dotnet-apps.yml`.

The Android app needs `android/Resources/raw/test_ca.crt` (the CA from `tools/test-https/serve.py`, gitignored) to build.
