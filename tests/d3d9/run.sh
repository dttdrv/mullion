#!/bin/bash
# Runs the d3d9 tests (64- and 32-bit) in a fresh Wine runtime with this tree's Mullion DLLs installed.
# Usage: tests/d3d9/run.sh <wine-install-prefix>   (from the repository root, after building build/ and build32/)
set -uo pipefail

wine_install="$(cd "${1:?wine install prefix}" && pwd)"
root="$(pwd)"
work="$root/toolchains/test"
runtime="$work/wine"

rm -rf "$work/pfx"
mkdir -p "$work/logs" "$runtime"
rsync -a --delete --exclude '*-windows/d3d9.dll' --exclude '*-windows/d3d11.dll' --exclude '*-windows/d3d10core.dll' \
  --exclude '*-windows/dxgi.dll' --exclude '*-windows/winemetal.dll' --exclude 'x86_64-unix/winemetal.so' \
  "$wine_install/" "$runtime/"
"$(dirname "$0")/install.sh"

export WINEPREFIX="$work/pfx" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml=" MULLION_LOG_PATH="$work/logs"
"$runtime/bin/wine" wineboot -i > /dev/null 2>&1
# a crash dialog would block the run forever
"$runtime/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f > /dev/null 2>&1
# a test started while prefix setup is still settling can stall for minutes
"$runtime/bin/wineserver" -w

# While the screen is locked, macOS delays Wine's first window for minutes or indefinitely.
python3 -I -c '
import ctypes, ctypes.util, sys
cg = ctypes.CDLL(ctypes.util.find_library("CoreGraphics"))
cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
cg.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
cf.CFDictionaryGetValue.restype = ctypes.c_void_p
cf.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
cf.CFStringCreateWithCString.restype = ctypes.c_void_p
cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
session = cg.CGSessionCopyCurrentDictionary()
key = cf.CFStringCreateWithCString(None, b"CGSSessionScreenIsLocked", 0x08000100)
sys.exit(0 if session and cf.CFDictionaryGetValue(session, key) else 1)
' && echo "warning: the screen is locked; tests may time out"

timeout="${D3D9_TEST_TIMEOUT:-30}"
failed=0
for build in ${D3D9_TEST_BUILDS:-build build32}; do
  for exe in "$root/$build"/tests/d3d9/d3d9_*.exe; do
    name="$(basename "$exe" .exe)"
    [ "$name" = d3d9_clear ] && continue
    [ -n "${D3D9_TESTS:-}" ] && [[ " $D3D9_TESTS " != *" $name "* ]] && continue
    # takes over the display
    [ "$name" = d3d9_fullscreen ] && [ "${D3D9_TEST_FULLSCREEN:-0}" != 1 ] && continue
    out="$work/logs/$build-$name.out"
    "$runtime/bin/wine" "$exe" > "$out" 2>&1 &
    pid=$!
    for ((i = 0; i < timeout; i++)); do
      kill -0 $pid 2>/dev/null || break
      sleep 1
    done
    if kill -0 $pid 2>/dev/null; then
      "$runtime/bin/wineserver" -k
      wait $pid 2>/dev/null
      echo "$build/$name: TIMEOUT after ${timeout}s"
      failed=1
      continue
    fi
    wait $pid
    result="$(grep -E '^(FAIL|(passed|failed): [0-9]+ failure)' "$out")"
    echo "$build/$name: ${result:-no output}"
    echo "$result" | grep -q '^passed: 0 failure' || failed=1
  done
done
"$runtime/bin/wineserver" -k
exit $failed
