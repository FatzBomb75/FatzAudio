# Changes to miniaudio.h

`miniaudio.h` is miniaudio 0.11.25 (public domain or MIT No Attribution, see `LICENSE`),
https://github.com/mackron/miniaudio. It is unchanged except for the function
below. Re-apply it when updating, or drop it if upstream has an equivalent.

| Where | What and why |
|---|---|
| `ma_dr_flac__seek_to_pcm_frame__binary_search_internal` (dr_flac, marked "FatzAudio patch") | Seeking a `.flac` without a SEEKTABLE (FatzAudio's own encoder writes none, nor does FFmpeg's) used a bisection that took up to 11 probes (one frame decode each) per seek on a 14 s test file, and up to 16 on real music. The replacement interpolates between the closest known frames either side, aiming one block before the target, and bisects twice whenever a probe fails to halve the range, so typical files seek in 2 to 4 probes. The same function searches between the points of a SEEKTABLE, which gets faster too (8 probes down to 5 with a point every 3.7 s). Probes still decode and CRC-check a whole frame, so a seek lands on exactly the same samples. Long digital silence before music is the worst case: up to 15 probes against upstream's 11. Tested by `test_flac_seek_search` in `tests/test_core.c`. |
