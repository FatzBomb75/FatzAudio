#!/bin/sh
# Linux: libfatzaudio.so must need only the C library (no libstdc++ or libgcc_s:
# one self-contained binary per platform) and export only the fa_* C API.
#
# Usage: check_deps_linux.sh path/to/libfatzaudio.so
so=$1
bad=0
needed=$(readelf -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
for lib in $needed; do
    case $lib in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|ld-linux*.so.*) ;;
        *) echo "needs $lib"; bad=1 ;;
    esac
done
exports=$(readelf --dyn-syms -W "$so" | awk '($5=="GLOBAL"||$5=="WEAK") && $7!="UND" {print $8}' | grep -v '^fa_')
if [ -n "$exports" ]; then
    echo "exports symbols outside the C API:"
    echo "$exports"
    bad=1
fi
[ $bad -eq 0 ] && echo "needs only:" $needed
exit $bad
