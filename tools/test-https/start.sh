#!/usr/bin/env bash
# Starts the local HTTPS test server (serve.py) in the background and waits until it listens on 127.0.0.1:8443.
# Usage: tools/test-https/start.sh <work dir>   (the CA certificate lands in <work dir>/ca.crt, the server
# log in <work dir>.log). Used by the CI workflows; each one then trusts ca.crt its own platform's way.
set -eu
dir=${1:?usage: start.sh <work dir>}
py=python3; [ "${RUNNER_OS:-}" = "Windows" ] && py=python
here=$(cd "$(dirname "$0")" && pwd)
$py "$here/serve.py" "$here/../../tests/data" "$dir" > "$dir.log" 2>&1 &
for _ in $(seq 150); do
  $py -c "import socket; socket.create_connection(('127.0.0.1', 8443), 1)" 2>/dev/null && exit 0
  sleep 0.2
done
echo "HTTPS test server did not start" >&2
cat "$dir.log" >&2 || true
exit 1
