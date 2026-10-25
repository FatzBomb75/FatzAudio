#!/usr/bin/env bash
# Runs one iOS-simulator test program in a booted simulator, for ctest: an iOS
# build configured with -DFA_BUILD_TESTS=ON
# -DCMAKE_CROSSCOMPILING_EMULATOR=<this script> runs every test through it.
# The simulator shares the Mac's file system, so the tests find their data at the
# same paths. It gets only the variables passed with the SIMCTL_CHILD_ prefix, so
# this passes on the FA_* ones and the folder ctest runs the test in (FA_TEST_CWD).
#
# FA_IOS_SIM names the simulator (a UDID); default: the booted one.
set -euo pipefail
while IFS='=' read -r name value; do
    case $name in FA_*) export "SIMCTL_CHILD_$name=$value" ;; esac
done < <(env)
export SIMCTL_CHILD_FA_TEST_CWD="$PWD"
exec xcrun simctl spawn "${FA_IOS_SIM:-booted}" "$@"
