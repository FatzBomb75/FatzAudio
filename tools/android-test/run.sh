#!/usr/bin/env bash
# Runs an Android build's test suite on a Linux PC or CI runner, without an
# emulator: the tests run inside an Android 14 user space (bionic, the real
# dynamic linker and system libraries) entered with chroot. x86_64 and x86 builds
# run natively; arm64-v8a and armeabi-v7a builds run under qemu-user (slower, but
# the same code).
#
# What this cannot cover: anything that needs Android's system services (audio
# output through AAudio/OpenSL ES, MediaCodec), which need binder and a booted
# device. Those still need an emulator or a phone (docs/building.md).
#
# Usage (as root, or through sudo):
#   tools/android-test/run.sh BUILD_DIR [ctest-name-regex]
# BUILD_DIR is an Android build of this repository (docs/building.md). The
# Android image is downloaded once into BUILD_DIR/android-rootfs (about 870 MB
# compressed, 2 GB unpacked, pinned by digest) and reused after that;
# FA_ANDROID_ROOTFS=DIR keeps it elsewhere (for example a CI cache).
set -euo pipefail

build=$(cd "${1:?usage: run.sh BUILD_DIR [test-regex]}" && pwd)
filter=${2:-.}
src=$(cd "$(dirname "$0")/../.." && pwd)
[ "$(id -u)" = 0 ] || { echo "run.sh needs root (chroot and bind mounts); try sudo" >&2; exit 2; }

# The ABI comes from the built library itself (the ELF machine field).
case "$(od -An -tx1 -j18 -N2 "$build/bin/libfatzaudio.so" | tr -d ' ')" in
    3e00) abi=x86_64 image=x86 qemu= ;;
    0300) abi=x86 image=x86 qemu= ;;
    b700) abi=arm64-v8a image=arm qemu=aarch64 ;;
    2800) abi=armeabi-v7a image=arm qemu=arm ;;
    *) echo "$build/bin/libfatzaudio.so is not an Android x86_64, x86, arm64 or arm library" >&2; exit 2 ;;
esac
# An arm64 host runs arm64 tests natively (32-bit ARM still goes through qemu:
# many arm64 CPUs cannot run 32-bit code).
[ "$abi" != arm64-v8a ] || case "$(uname -m)" in aarch64|arm64) qemu= ;; esac

# redroid/redroid:14.0.0 (Docker Hub), per-architecture image manifests, pinned.
# Both images also carry the 32-bit libraries.
if [ "$image" = x86 ]; then
    manifest=sha256:11d58a64bfbde2253d1cce81bff409ff58174980222d1bada232d9ef59181191
else
    manifest=sha256:d9ef29d08c5e7ff6d4771a9894d4df47725d9a733dc251c47bc40782cc86359a
fi
root=${FA_ANDROID_ROOTFS:-$build/android-rootfs}/$image
mkdir -p "$root" && root=$(cd "$root" && pwd)
# A run that was killed can leave its bind mounts (of the source tree!) inside.
# (The kernel writes spaces and other characters in mount points as \ooo escapes.)
if python3 -c '
import re, sys
root = sys.argv[1] + "/"
for line in open("/proc/self/mountinfo"):
    point = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), line.split()[4])
    if (point + "/").startswith(root):
        sys.exit(0)
sys.exit(1)' "$root"; then
    echo "something is still mounted under $root (an interrupted run?); unmount it first" >&2
    exit 2
fi

if [ ! -e "$root/.complete-$manifest" ]; then
    if [ -n "$(ls -A "$root")" ] && [ ! -e "$root/.fa-android-rootfs" ]; then
        echo "$root is not empty and not an Android test root; not replacing it" >&2
        exit 2
    fi
    echo "Downloading the Android 14 user space for $abi (once)..."
    rm -rf "$root" && mkdir -p "$root" && touch "$root/.fa-android-rootfs"
    registry=https://registry-1.docker.io/v2/redroid/redroid
    token=$(curl -fsS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:redroid/redroid:pull" |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
    curl -fsS -H "Authorization: Bearer $token" \
        -H "Accept: application/vnd.oci.image.manifest.v1+json,application/vnd.docker.distribution.manifest.v2+json" \
        "$registry/manifests/$manifest" -o "$root/manifest.json"
    [ "sha256:$(sha256sum "$root/manifest.json" | cut -d' ' -f1)" = "$manifest" ] ||
        { echo "image manifest does not match its pinned digest" >&2; exit 1; }
    for layer in $(python3 -c 'import json,sys; [print(l["digest"]) for l in json.load(open(sys.argv[1]))["layers"]]' "$root/manifest.json"); do
        curl -fsSL -H "Authorization: Bearer $token" "$registry/blobs/$layer" -o "$root/layer.tgz"
        [ "sha256:$(sha256sum "$root/layer.tgz" | cut -d' ' -f1)" = "$layer" ] ||
            { echo "image layer $layer does not match its digest" >&2; exit 1; }
        tar xzf "$root/layer.tgz" -C "$root"
        rm "$root/layer.tgz"
    done
    # The image's APEX modules are flattened folders; a booted device mounts them
    # on /apex, here a link does.
    for d in "$root"/system/apex/*; do
        n=$(basename "$d")
        [ -e "$root/apex/$n" ] || ln -s "/system/apex/$n" "$root/apex/$n"
    done
    touch "$root/.complete-$manifest"
fi

# The image's own shell is 64-bit: under emulation it runs with qemu-aarch64,
# and the tests with the emulator for their ABI.
shell_runner= test_runner=
if [ -n "$qemu" ]; then
    for q in aarch64 "$qemu"; do
        exe=$(command -v "qemu-$q-static" || true)
        [ -n "$exe" ] || { echo "$abi tests need qemu-$q-static (apt install qemu-user-static)" >&2; exit 2; }
        cp "$exe" "$root/qemu-$q"
    done
    shell_runner=/qemu-aarch64 test_runner=/qemu-$qemu
fi

# Tests find their data and outputs by absolute path, so the source and build
# folders appear at the same paths inside.
mounts=()
cleanup() {
    local i
    for (( i=${#mounts[@]}-1; i>=0; i-- )); do umount "${mounts[$i]}" 2>/dev/null || umount -l "${mounts[$i]}" || true; done
}
trap cleanup EXIT
bind() {
    mkdir -p "$root$2"
    mount --bind "$1" "$root$2"
    mounts+=("$root$2")
}
bind /proc /proc
bind /dev /dev
bind "$src" "$src"
[ "${build#"$src"/}" != "$build" ] || bind "$build" "$build"

ctest --test-dir "$build" --show-only=json-v1 > "$build/android-tests.json"
python3 - "$build" "$filter" "$root" "$shell_runner" "$test_runner" <<'EOF'
import json, re, subprocess, sys, time
build, pattern, root = sys.argv[1:4]
shell_runner, test_runner = [r for r in sys.argv[4:5] if r], [r for r in sys.argv[5:6] if r]
failed = []
for t in json.load(open(build + "/android-tests.json"))["tests"]:
    if not re.search(pattern, t["name"]):
        continue
    props = {p["name"]: p["value"] for p in t.get("properties", [])}
    cwd = props.get("WORKING_DIRECTORY", build)
    timeout = float(props.get("TIMEOUT", 1500))
    # Android's CMake builds carry no rpath: the tests find libfatzaudio.so here.
    cmd = "cd '%s' && LD_LIBRARY_PATH='%s/bin' exec %s" % (cwd, build, " ".join(test_runner + ["'%s'" % c for c in t["command"]]))
    start = time.time()
    try:
        p = subprocess.run(["chroot", root] + shell_runner + ["/system/bin/sh", "-c", cmd],
                           capture_output=True, text=True, timeout=timeout)
        rc, out = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        rc, out = "timeout", "timed out after %.0f s" % timeout
    # Outside a booted device the linker has no generated namespace config; harmless.
    out = "\n".join(l for l in out.splitlines() if "linkerconfig" not in l)
    ok = rc == 0
    print("%-4s %-12s %6.1fs" % ("ok" if ok else "FAIL", t["name"], time.time() - start), flush=True)
    if not ok:
        failed.append(t["name"])
        print(out)
print("%d failed: %s" % (len(failed), " ".join(failed)) if failed else "all Android tests passed")
sys.exit(1 if failed else 0)
EOF
