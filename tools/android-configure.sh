#!/usr/bin/env bash
# CMake configure for Android with the NDK toolchain on a CI runner (Ninja, android-24, Release, no tools).
# Usage: tools/android-configure.sh <source dir> <build dir> <abi> [more cmake arguments]
set -eu
src=${1:?usage: android-configure.sh <source dir> <build dir> <abi> [cmake args]}
build=${2:?}
abi=${3:?}
shift 3
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_LATEST_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release \
  -DFA_BUILD_TOOLS=OFF "$@"
