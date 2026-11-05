#!/bin/bash
# Runs the d3d9 tests in a throwaway macOS VM, so display-mode tests change the VM's display instead of this Mac's.
# Usage: tests/d3d9/tart.sh <wine-install-prefix>   (from the repository root, after building build/ and build32/)
# D3D9_TEST_BUILDS, D3D9_TESTS, D3D9_TEST_TIMEOUT and WINEDEBUG pass through to run.sh.
# The VM's paravirtualized GPU is Mac2-only, so Mullion takes its non-Apple path; 32-bit results there are unreliable (tests/d3d9/README.md).
# Logs land in toolchains/test/tart-logs. TART_IMAGE picks the image (a Cirrus Labs one, for its guest agent).
set -uo pipefail

wine_install="$(cd "${1:?wine install prefix}" && pwd)"
root="$(pwd)"
image="${TART_IMAGE:-ghcr.io/cirruslabs/macos-tahoe-base:latest}"
vm="mullion-test-$$"
logs="$root/toolchains/test/tart-logs"
rm -rf "$logs"
mkdir -p "$logs"

tart clone "$image" "$vm" || exit 1
shared="/Volumes/My Shared Files"
guest() { tart exec "$vm" /bin/bash -c "$1"; }
cleanup() {
  guest "cp -R ~/mullion/toolchains/test/logs/. '$shared/logs/'" > /dev/null 2>&1
  tart stop "$vm" > /dev/null 2>&1
  tart delete "$vm" > /dev/null 2>&1
}
trap cleanup EXIT
tart run "$vm" --no-graphics --dir="mullion:$root:ro" --dir="wine:$wine_install:ro" --dir="logs:$logs" > /dev/null 2>&1 &

for ((i = 0; i < 300; i++)); do
  tart exec "$vm" true > /dev/null 2>&1 && break
  sleep 1
done
guest true || { echo "the VM's guest agent never came up"; exit 1; }

guest "sudo softwareupdate --install-rosetta --agree-to-license > /dev/null" || exit 1
guest "rsync -a '$shared/wine/' ~/wine/ && rsync -a --prune-empty-dirs --include='*/' --include='tests/d3d9/***' \
  --include='build*/src/**.dll' --include='build/src/winemetal/unix/winemetal.so' --include='build*/tests/d3d9/*.exe' \
  --exclude='*' '$shared/mullion/' ~/mullion/" || exit 1

# Wine's loader doesn't search its own lib/ for FreeType and the other host libraries
guest "cd ~/mullion && DYLD_FALLBACK_LIBRARY_PATH=~/wine/lib D3D9_TEST_FULLSCREEN=1 D3D9_TEST_BUILDS='${D3D9_TEST_BUILDS:-build build32}' \
  D3D9_TESTS='${D3D9_TESTS:-}' D3D9_TEST_TIMEOUT='${D3D9_TEST_TIMEOUT:-30}' WINEDEBUG='${WINEDEBUG:--all}' tests/d3d9/run.sh ~/wine" | tee "$logs/run.txt"
exit "${PIPESTATUS[0]}"
