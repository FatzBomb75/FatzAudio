#!/usr/bin/env bash
# Boots the newest available iPhone simulator and prints its UDID (CI: `echo "FA_IOS_SIM=$(tools/ios-sim/boot.sh)" >> "$GITHUB_ENV"`).
set -eu
udid=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
d = json.load(sys.stdin)["devices"]
print(next(x["udid"] for k in sorted(d, reverse=True) if ".iOS-" in k for x in d[k] if x["name"].startswith("iPhone")))')
xcrun simctl boot "$udid" >&2
xcrun simctl bootstatus "$udid" -b >&2
echo "$udid"
