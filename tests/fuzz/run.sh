#!/bin/sh
# Runs the fuzz targets built in a FA_FUZZ tree (see CMakeLists.txt).
#   tests/fuzz/run.sh BUILD_DIR OUT_DIR [EXECS] [target...]
# Targets: stream_<fmt> (aac mp3 flac mkv mp4 caf ogg asf dolby, seeded from tests/data; wav midi
# tracker, seeded from seeds/), tags, zip, hls. Needs python3 for make_seeds.py.
set -e
B=${1:?build dir}; OUT=${2:?out dir}; N=${3:-1000000}; shift 3 2>/dev/null || shift $#
D=$(cd "$(dirname "$0")" && pwd); DATA=$D/../data
python3 "$D/make_seeds.py"
export ASAN_OPTIONS=detect_leaks=1:max_allocation_size_mb=1024:abort_on_error=1:handle_abort=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:abort_on_error=1
[ $# -gt 0 ] || set -- stream_aac stream_mp3 stream_flac stream_mkv stream_mp4 stream_caf stream_ogg stream_asf \
  stream_dolby stream_wav stream_midi stream_tracker tags zip hls
for t in "$@"; do
  case $t in
    stream_aac) bin=fuzz_stream; seeds="$DATA/aac $DATA/net/tone.aac $DATA/tone.aac $DATA/aac.m4a $DATA/aac51.m4a $DATA/aac_switch.aac" ;;
    stream_mp3) bin=fuzz_stream; seeds="$DATA/mp3 $DATA/tone.mp3 $DATA/net" ;;
    stream_flac) bin=fuzz_stream; seeds="$DATA/flac $DATA/alac" ;;
    stream_mkv) bin=fuzz_stream; seeds="$DATA/av_tracks.mkv $DATA/opus.webm $DATA/flac/tone_flac.mkv $DATA/alac/alac16.mkv" ;;
    stream_mp4) bin=fuzz_stream; seeds="$DATA/av_tracks.mp4 $DATA/frag.mp4 $DATA/pcm.mov $DATA/alac.m4a $DATA/flac/tone_flac.mp4" ;;
    stream_caf) bin=fuzz_stream; seeds="$DATA/caf" ;;
    stream_ogg) bin=fuzz_stream; seeds="$DATA/ogg $DATA/flac/tone.oga $DATA/flac/surround.oga $DATA/tags/tags.opus" ;;
    stream_asf) bin=fuzz_stream; seeds="$DATA/asf" ;;
    stream_dolby) bin=fuzz_stream; seeds="$DATA/tone.ac3 $DATA/tone.eac3 $DATA/dolby/tone_ac3.mp4 $DATA/dolby/tone_eac3.mkv $DATA/dolby/*.ec3" ;;
    stream_wav) bin=fuzz_stream; seeds="$D/seeds/wav $DATA/tags/tags.aiff" ;;
    stream_midi) bin=fuzz_stream; seeds="$D/seeds/midi" ;;
    stream_tracker) bin=fuzz_stream; seeds="$D/seeds/tracker" ;;
    tags) bin=fuzz_tags; seeds="$DATA/tags $D/seeds/tags $D/seeds/midi $DATA/mp3 $DATA/flac $DATA/caf" ;;
    zip) bin=fuzz_zip; seeds="$D/seeds/zip" ;;
    hls) bin=fuzz_hls; seeds="$D/seeds/hls" ;;
    *) echo "unknown target $t"; exit 2 ;;
  esac
  echo "== $t"
  "$B/bin/$bin" -n "$N" -m "${FUZZ_MAXLEN:-131072}" -s "${FUZZ_SEED:-1}" -o "$OUT/$t" $seeds
done
