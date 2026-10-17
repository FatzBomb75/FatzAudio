# Signalsmith Stretch and Signalsmith Linear

Vendored, unmodified, MIT licensed (see the LICENSE.txt in each folder).

| Library | Version | Files | Source |
|---|---|---|---|
| Signalsmith Stretch | 1.4.0 (header reports 1.3.2) | `signalsmith-stretch/signalsmith-stretch.h` | https://github.com/Signalsmith-Audio/signalsmith-stretch |
| Signalsmith Linear | 0.6.4 (the version Stretch 1.4.0 pins) | `signalsmith-linear/stft.h`, `signalsmith-linear/fft.h` | https://github.com/Signalsmith-Audio/linear |

Only the files Stretch needs with the built-in FFT are copied. FatzAudio uses them
from `src/stretch.cpp`, the only C++ file in the library.
