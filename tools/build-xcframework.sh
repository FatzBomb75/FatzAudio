#!/usr/bin/env bash
# Builds fatzaudio.xcframework for iOS on a Mac with Xcode: one dynamic
# fatzaudio.framework for devices (arm64) and one for the simulator (arm64 and
# x86_64), combined into BUILD_DIR/fatzaudio.xcframework.
#
# Usage: tools/build-xcframework.sh [BUILD_DIR]   (default: build-ios)
# FA_IOS_MIN sets the minimum iOS version (default 13.0).
set -euo pipefail

src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$src/build-ios}
min=${FA_IOS_MIN:-13.0}
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

build() {   # build <sdk> <architectures>
    # A single-config generator, so the frameworks land in bin/ (not bin/Release/).
    cmake -S "$src" -B "$out/$1" -G "Unix Makefiles" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$1" \
        -DCMAKE_OSX_ARCHITECTURES="$2" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$min" \
        -DCMAKE_BUILD_TYPE=Release
    # An explicit job count: a bare --parallel lets make start every compile at
    # once (500 clang processes for libopenmpt), which thrashes for hours.
    cmake --build "$out/$1" --config Release --parallel "$jobs"
}
build iphoneos arm64
build iphonesimulator "arm64;x86_64"

rm -rf "$out/fatzaudio.xcframework"
xcodebuild -create-xcframework \
    -framework "$out/iphoneos/bin/fatzaudio.framework" \
    -framework "$out/iphonesimulator/bin/fatzaudio.framework" \
    -output "$out/fatzaudio.xcframework"
echo "Built $out/fatzaudio.xcframework"
