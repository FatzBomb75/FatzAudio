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
# FA_ANDROID_TEST_VERBOSE=1 prints the output of passing tests too.
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
# The test programs also run from here: the linker gives programs under
# /data/local/tmp the namespaces of a shell tool, as adb-pushed tests get.
bind "$build" /data/local/tmp/fatzaudio-build

# The image's APEX modules are flattened folders; a booted device mounts each on
# /apex/<name> (the name in its manifest), and so does this. (Earlier versions of
# this script linked them instead; the links go.)
apex_info="<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<apex-info-list>\n"
for d in "$root"/system/apex/*; do
    [ -f "$d/apex_manifest.pb" ] || continue
    # Field 1 of the ApexManifest protobuf: the module name.
    n=$(python3 -c 'import sys; b=open(sys.argv[1],"rb").read(); print(b[2:2+b[1]].decode() if b[:1]==b"\n" else "")' "$d/apex_manifest.pb")
    [ -n "$n" ] || n=$(basename "$d")
    [ ! -L "$root/apex/$(basename "$d")" ] || rm "$root/apex/$(basename "$d")"
    bind "$d" "/apex/$n"
    apex_info+="  <apex-info moduleName=\"$n\" modulePath=\"/system/apex/$n.apex\" preinstalledModulePath=\"/system/apex/$n.apex\" versionCode=\"1\" versionName=\"1\" isFactory=\"true\" isActive=\"true\"/>\n"
done
printf "%b</apex-info-list>\n" "$apex_info" > "$root/apex/apex-info-list.xml"

# The linker namespaces of a booted device, which the Java runtime needs (the
# HTTPS tests start one): linkerconfig writes them at boot from the system
# properties. There is no property service here, so a small preloaded library
# (props.c, built with the build's own NDK compiler for the image's 64-bit ABI)
# answers from the image's build.prop files. Without a compiler the tests still
# run; the ones that need a Java VM skip.
if [ ! -s "$root/linkerconfig/ld.config.txt" ]; then
    # Starts over: a half-written config would also apply to linkerconfig itself.
    rm -rf "$root/linkerconfig" && mkdir "$root/linkerconfig"
    cc=$(sed -n 's/^set(CMAKE_C_COMPILER "\(.*\)")$/\1/p' "$build"/CMakeFiles/*/CMakeCCompiler.cmake 2>/dev/null | head -1)
    case $image in x86) triple=x86_64-linux-android24 ;; *) triple=aarch64-linux-android24 ;; esac
    if [ -x "$cc" ] && "$cc" --target=$triple -shared -fPIC -O2 -o "$root/linkerconfig/props.so" "$src/tools/android-test/props.c"; then
        chroot "$root" $shell_runner /system/bin/sh -c \
            "LD_PRELOAD=/linkerconfig/props.so $shell_runner /apex/com.android.runtime/bin/linkerconfig --target /linkerconfig" ||
            echo "linkerconfig failed; tests that need a Java VM will skip" >&2
    else
        echo "no NDK compiler found in $build/CMakeFiles; tests that need a Java VM will skip" >&2
    fi
fi

ctest --test-dir "$build" --show-only=json-v1 > "$build/android-tests.json"
python3 - "$build" "$filter" "$root" "$shell_runner" "$test_runner" <<'EOF'
import json, os, re, subprocess, sys, time
verbose = os.environ.get("FA_ANDROID_TEST_VERBOSE") == "1"
build, pattern, root = sys.argv[1:4]
shell_runner, test_runner = [r for r in sys.argv[4:5] if r], [r for r in sys.argv[5:6] if r]
# What init exports on a device, for a test that starts a Java VM.
jars = ["/apex/com.android.art/javalib/%s.jar" % j for j in ("core-oj", "core-libart", "okhttp", "bouncycastle", "apache-xml")]
jars += ["/apex/com.android.i18n/javalib/core-icu4j.jar", "/apex/com.android.conscrypt/javalib/conscrypt.jar"]
java_env = ("BOOTCLASSPATH=%s ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art "
            "ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata" % ":".join(jars))
run_dir = "/data/local/tmp/fatzaudio-build"
failed = []
for t in json.load(open(build + "/android-tests.json"))["tests"]:
    if not re.search(pattern, t["name"]):
        continue
    props = {p["name"]: p["value"] for p in t.get("properties", [])}
    cwd = props.get("WORKING_DIRECTORY", build)
    timeout = float(props.get("TIMEOUT", 1500))
    # Android's CMake builds carry no rpath: the tests find libfatzaudio.so here.
    command = list(t["command"])
    if command[0].startswith(build + "/"):
        command[0] = run_dir + command[0][len(build):]
    cmd = "cd '%s' && %s LD_LIBRARY_PATH='%s/bin' exec %s" % (cwd, java_env, run_dir, " ".join(test_runner + ["'%s'" % c for c in command]))
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
    if not ok or verbose:
        print(out)
print("%d failed: %s" % (len(failed), " ".join(failed)) if failed else "all Android tests passed")
sys.exit(1 if failed else 0)
EOF
