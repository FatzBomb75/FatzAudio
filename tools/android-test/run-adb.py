#!/usr/bin/env python3
"""Runs an Android build's whole ctest suite on a device or emulator over adb: the
real system, with its audio output (AAudio), MediaCodec and Java runtime, which
run.sh's chroot cannot offer.

The tests find their data and write their outputs by absolute path, and Android's
root file system is read-only, so the source and build folders must live under
/data/local/tmp on the build machine too; they are copied to the same paths on the
device. The CI job (.github/workflows/devices.yml) builds that way:
  cmake -S /data/local/tmp/fa/src -B /data/local/tmp/fa/build ...   (Android toolchain)
  tools/android-test/run-adb.py /data/local/tmp/fa/build [ctest-name-regex]

Variables named FA_* are passed on to the tests (FA_TEST_REAL_SYSTEM=1 makes the
ones that need the system's services fail rather than skip). ADB_SERIAL picks a
device when several are connected. FA_ANDROID_TEST_VERBOSE=1 prints the output of
passing tests too.
"""
import json, os, re, shlex, subprocess, sys, time

if len(sys.argv) < 2:
    sys.exit(__doc__)
build = os.path.abspath(sys.argv[1])
pattern = sys.argv[2] if len(sys.argv) > 2 else "."
if not build.startswith("/data/local/tmp/"):
    sys.exit("the build folder must be under /data/local/tmp (see the comment at the top)")
verbose = os.environ.get("FA_ANDROID_TEST_VERBOSE") == "1"
adb = ["adb"] + (["-s", os.environ["ADB_SERIAL"]] if os.environ.get("ADB_SERIAL") else [])

tests = json.loads(subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"], check=True,
                                  capture_output=True, text=True).stdout)["tests"]
src = None
for line in open(os.path.join(build, "CMakeCache.txt")):
    if line.startswith("CMAKE_HOME_DIRECTORY:"):
        src = line.split("=", 1)[1].strip()
if not src or not src.startswith("/data/local/tmp/"):
    sys.exit("the source folder must be under /data/local/tmp too (it is %s)" % src)


def props(t):
    return {p["name"]: p["value"] for p in t.get("properties", [])}


# The programs and the library, the test data, and every folder a test runs in.
dirs = sorted({props(t).get("WORKING_DIRECTORY", build) for t in tests})
subprocess.run(adb + ["shell", "mkdir -p " + " ".join(shlex.quote(d) for d in dirs + [build, src + "/tests"])], check=True)
subprocess.run(adb + ["push", "--sync", build + "/bin", build], check=True, stdout=subprocess.DEVNULL)
subprocess.run(adb + ["push", "--sync", src + "/tests/data", src + "/tests"], check=True, stdout=subprocess.DEVNULL)

env = " ".join("%s=%s" % (k, shlex.quote(v)) for k, v in sorted(os.environ.items()) if k.startswith("FA_"))
failed = []
for t in tests:
    if not re.search(pattern, t["name"]):
        continue
    cwd = props(t).get("WORKING_DIRECTORY", build)
    timeout = float(props(t).get("TIMEOUT", 1500))
    cmd = "cd %s && %s LD_LIBRARY_PATH=%s exec %s" % (shlex.quote(cwd), env, shlex.quote(build + "/bin"),
                                                     " ".join(shlex.quote(c) for c in t["command"]))
    start = time.time()
    try:
        p = subprocess.run(adb + ["shell", cmd], capture_output=True, text=True, timeout=timeout)
        rc, out = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        rc, out = "timeout", "timed out after %.0f s" % timeout
    ok = rc == 0
    print("%-4s %-12s %6.1fs" % ("ok" if ok else "FAIL", t["name"], time.time() - start), flush=True)
    if not ok:
        failed.append(t["name"])
    if not ok or verbose:
        print(out, flush=True)
print("%d failed: %s" % (len(failed), " ".join(failed)) if failed else "all Android tests passed")
sys.exit(1 if failed else 0)
