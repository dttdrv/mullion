#!/bin/bash
# Copies this tree's Mullion DLLs into the test runtime run.sh set up, and into its prefix's copies when the prefix exists.
# Usage: tests/d3d9/install.sh   (from the repository root, after building build/ and build32/)
set -uo pipefail

root="$(pwd)"
work="$root/toolchains/test"
runtime="$work/wine"
install_if_changed() { [ -d "$(dirname "$2")" ] && { cmp -s "$1" "$2" || cp "$1" "$2"; }; }

for arch in x86_64 i386; do
  build="$root/build"
  system="system32"
  if [ $arch = i386 ]; then
    build="$root/build32"
    system="syswow64"
  fi
  for dll in d3d9/d3d9 d3d11/d3d11 d3d10/d3d10core dxgi/dxgi winemetal/winemetal; do
    name="$(basename "$dll").dll"
    install_if_changed "$build/src/$dll.dll" "$runtime/lib/wine/$arch-windows/$name"
    # Wine may load the prefix's copy instead of the runtime's
    [ -f "$work/pfx/drive_c/windows/$system/$name" ] && install_if_changed "$build/src/$dll.dll" "$work/pfx/drive_c/windows/$system/$name"
  done
done
install_if_changed "$root/build/src/winemetal/unix/winemetal.so" "$runtime/lib/wine/x86_64-unix/winemetal.so"
exit 0
