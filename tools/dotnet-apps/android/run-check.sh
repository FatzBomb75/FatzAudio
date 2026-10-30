#!/usr/bin/env bash
# Installs the .NET for Android check app on the running emulator, starts it, and waits for its verdict
# in logcat (tag FatzAudioApp). Usage: run-check.sh APK FILE_ON_DEVICE [URL]
# For an HTTPS URL on 127.0.0.1:8443, the caller runs the test server on the host; "adb reverse" makes the
# emulator's 127.0.0.1:8443 reach it.
set -euo pipefail
apk=$1 file=$2 url=${3:-}
adb install -r "$apk"
adb logcat -c
adb reverse tcp:8443 tcp:8443 || true
args=(--es file "$file")
[ -n "$url" ] && args+=(--es url "$url")
adb shell am start -W -n net.fatzaudio.ciapp/net.fatzaudio.ciapp.MainActivity "${args[@]}"
for _ in $(seq 120); do
    out=$(adb logcat -d -s FatzAudioApp:I)
    if grep -q 'FATZAUDIO-APP-' <<<"$out"; then
        echo "$out"
        grep -q 'FATZAUDIO-APP-OK' <<<"$out"
        exit $?
    fi
    sleep 1
done
echo "timed out"; adb logcat -d | tail -100
exit 1
