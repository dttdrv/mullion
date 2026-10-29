#!/bin/sh
# Fixtures for install.sh's architecture mapping: a 64-bit prefix gets x86_64 DLLs in system32 and i386 DLLs in
# syswow64, a 32-bit-only prefix gets i386 DLLs in its system32, and a 64-bit prefix without syswow64 does not silently
# put 32-bit DLLs in system32. Run with `sh tests/install/install_test.sh`; it builds its fixtures in a temporary
# directory and needs python3 only to write PE headers.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/mullion-install-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# a PE image with `machine` in its COFF header and optional-header `magic` (0x10b PE32, 0x20b PE32+)
pe() {
  python3 - "$1" "$2" "$3" <<'PY'
import struct, sys
path, machine, magic = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
data = bytearray(0x200)
data[0:2] = b"MZ"
struct.pack_into("<I", data, 0x3C, 0x40)
data[0x40:0x44] = b"PE\0\0"
struct.pack_into("<H", data, 0x44, machine)
struct.pack_into("<H", data, 0x58, magic)
struct.pack_into("<I" if magic == 0x10B else "<Q", data, 0x78, 0x400000 if magic == 0x10B else 0x140000000)
open(path, "wb").write(data)
PY
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# the archive: one Windows half of each architecture, and the installer beside them
archive="$work/archive"
mkdir -p "$archive/x86_64-windows" "$archive/i386-windows" "$archive/x86_64-unix" "$work/wine/lib/wine"
cp "$root/install.sh" "$archive/install.sh"
printf 'x64\n' > "$archive/x86_64-windows/mullion64.dll"
printf 'x86\n' > "$archive/i386-windows/mullion32.dll"
printf 'unix\n' > "$archive/x86_64-unix/mullion_unix.so"

# a 64-bit prefix: both directories, and its kernel32 says x86_64
p64="$work/prefix64"
mkdir -p "$p64/drive_c/windows/system32" "$p64/drive_c/windows/syswow64"
pe "$p64/drive_c/windows/system32/kernel32.dll" 0x8664 0x20b

# a 32-bit-only prefix: one directory, and its kernel32 says i386
p32="$work/prefix32"
mkdir -p "$p32/drive_c/windows/system32"
pe "$p32/drive_c/windows/system32/kernel32.dll" 0x14c 0x10b

# a 64-bit prefix whose syswow64 is missing (the optional architecture is not there)
pnosys="$work/prefix64-no-syswow64"
mkdir -p "$pnosys/drive_c/windows/system32"
pe "$pnosys/drive_c/windows/system32/kernel32.dll" 0x8664 0x20b

# a prefix with no readable kernel32: the directories it has decide
pfallback="$work/prefix-fallback"
mkdir -p "$pfallback/drive_c/windows/system32" "$pfallback/drive_c/windows/syswow64"
printf 'not a PE image\n' > "$pfallback/drive_c/windows/system32/kernel32.dll"

sh "$archive/install.sh" "$work/wine" "$p64" "$p32" "$pnosys" "$pfallback"

[ -f "$p64/drive_c/windows/system32/mullion64.dll" ] || fail "x86_64 DLL not in the 64-bit system32"
[ -f "$p64/drive_c/windows/syswow64/mullion32.dll" ] || fail "i386 DLL not in syswow64"
[ ! -e "$p64/drive_c/windows/system32/mullion32.dll" ] || fail "i386 DLL was put in the 64-bit system32"

[ -f "$p32/drive_c/windows/system32/mullion32.dll" ] || fail "i386 DLL not in the 32-bit system32"
[ ! -e "$p32/drive_c/windows/system32/mullion64.dll" ] || fail "x86_64 DLL was put in a 32-bit prefix"

[ ! -e "$pnosys/drive_c/windows/system32/mullion32.dll" ] || fail "i386 DLL was put in a prefix without syswow64"

[ -f "$pfallback/drive_c/windows/syswow64/mullion32.dll" ] || fail "directory fallback did not choose syswow64"
[ -f "$pfallback/drive_c/windows/system32/mullion64.dll" ] || fail "directory fallback did not choose system32"

[ -f "$work/wine/lib/wine/x86_64-windows/mullion64.dll" ] || fail "x86_64 Windows half not installed in the Wine"
[ -f "$work/wine/lib/wine/i386-windows/mullion32.dll" ] || fail "i386 Windows half not installed in the Wine"
[ -f "$work/wine/lib/wine/x86_64-unix/mullion_unix.so" ] || fail "Unix half not installed in the Wine"

echo "install fixtures passed"
