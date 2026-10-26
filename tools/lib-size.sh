#!/usr/bin/env bash
# Prints the size of built FatzAudio libraries as an app ships them: ELF and Mach-O
# copies are stripped first (Linux and Android packages, Xcode's archive strip); a
# Windows DLL already carries no debug info. In GitHub Actions each line also goes
# to the job summary and becomes a notice, so sizes are tracked from run to run.
#
# Usage: tools/lib-size.sh LABEL LIBRARY [LABEL LIBRARY ...]
#   STRIP: the strip tool for ELF files (default: strip; Android: llvm-strip).
set -euo pipefail

if [ $# -eq 0 ] || [ $(($# % 2)) -ne 0 ]; then
    echo "usage: $0 LABEL LIBRARY [LABEL LIBRARY ...]" >&2
    exit 2
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
while [ $# -ge 2 ]; do
    label=$1 lib=$2
    shift 2
    copy="$tmp/lib"
    case "$lib" in
        *.dll) cp "$lib" "$copy" ;;
        *.so|*.so.*) "${STRIP:-strip}" --strip-unneeded -o "$copy" "$lib" ;;
        *) strip -x -o "$copy" "$lib" ;;   # Mach-O: a .dylib or a framework binary
    esac
    bytes=$(wc -c < "$copy" | tr -d ' ')
    line=$(awk -v l="$label" -v b="$bytes" 'BEGIN { printf "%s: %d bytes (%.2f MiB)", l, b, b / 1048576 }')
    echo "$line"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        echo "- $line" >> "$GITHUB_STEP_SUMMARY"
        echo "::notice title=Library size::$line"
    fi
done
