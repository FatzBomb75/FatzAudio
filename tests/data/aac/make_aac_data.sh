#!/bin/sh
# Regenerates the AAC test files of test_aac.c and FFmpeg's decodes of them (the
# references, as FLAC). Needs python3 with numpy, the ffmpeg CLI, a C compiler,
# and fdkaac built on a libfdk-aac with HE-AAC (SBR and PS; some distributions
# ship it without).
set -e
cd "$(dirname "$0")"
root=../../..
tmp=$(mktemp -d)
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

python3 signal.py "$tmp/st44.wav" 44100 2 0.25
python3 signal.py "$tmp/m48.wav" 48000 1 0.25
python3 signal.py "$tmp/c51.wav" 48000 6 0.3
python3 signal.py "$tmp/q48.wav" 48000 4 0.2
python3 signal.py "$tmp/st44h.wav" 44100 2 0.5

fdkaac -S -p 5 -b 48000 -f 2 -o he_stereo.aac "$tmp/st44.wav"     # HE-AAC, SBR coupling
fdkaac -S -p 29 -b 32000 -f 2 -o he_v2.aac "$tmp/st44.wav"        # HE-AAC v2 (PS, 20 bands)
fdkaac -S -p 5 -b 32000 -f 2 -o he_mono.aac "$tmp/m48.wav"        # mono HE-AAC (plays as stereo)
fdkaac -S -p 5 -b 160000 -f 2 -o he_51.aac "$tmp/c51.wav"         # 5.1 HE-AAC, coupled 1.5 dB envelopes
# AAC-LC quadraphonic: the layout in a program config element.
ff -i "$tmp/q48.wav" -af "channelmap=channel_layout=quad" -c:a aac -b:a 128k -aac_pce 1 \
   -fflags +bitexact -f adts lc_pce_quad.aac
# AAC Main with backward-adaptive prediction.
ff -i "$tmp/st44h.wav" -c:a aac -profile:a aac_main -aac_pred 1 -b:a 96k -fflags +bitexact -f adts main.aac

# PS modes no common encoder writes: he_v2.aac with its PS data replaced.
cc -O2 -DFA_AAC_TRACE -I$root/src -I$root/include -I$root/third_party/miniaudio -o "$tmp/trace" trace_main.c \
   $root/src/aac.c $root/src/aac_sbr.c $root/src/aac_ps.c $root/src/aac_tables.c $root/src/mdct.c -lm
"$tmp/trace" he_v2.aac 2> "$tmp/trace.txt"
python3 ps_rewrite.py he_v2.aac "$tmp/trace.txt" ps34.aac 5 5 5 0    # 34 bands, fine IID, R_B
python3 ps_rewrite.py he_v2.aac "$tmp/trace.txt" ps_ipd.aac 13 1 4 1 # 20 bands, R_B, IPD/OPD
# AAC-LTP: FFmpeg's encoder writes the profile but no prediction, so add some.
ff -i "$tmp/st44h.wav" -c:a aac -profile:a aac_ltp -strict -2 -b:a 96k -fflags +bitexact -f adts "$tmp/ltp.aac"
"$tmp/trace" "$tmp/ltp.aac" 2> "$tmp/trace.txt"
python3 ltp_rewrite.py "$tmp/ltp.aac" "$tmp/trace.txt" ltp.aac 1

for f in he_stereo he_v2 he_mono he_51 lc_pce_quad main ps34 ps_ipd ltp; do
    ff -i $f.aac -c:a flac -compression_level 12 -fflags +bitexact -map_metadata -1 $f.ref.flac
done
rm -rf "$tmp"
ls -l *.aac *.ref.flac
