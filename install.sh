#!/bin/sh
# installs Mullion from a release archive: install.sh <wine> [prefix ...]
# <wine> is the directory that holds bin/wine. the libraries go where Wine keeps its own, in place of its Direct3D,
# and into each prefix named: a prefix made before them does not know the ones Wine did not have
set -e
[ -d "$1/lib/wine" ] || { echo "usage: $0 <wine directory> [wine prefix ...]" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
wine=$1
shift
for kind in "$here"/*-windows "$here"/*-unix; do
  cp "$kind"/* "$wine/lib/wine/$(basename "$kind")/"
done
for prefix; do
  cp "$here"/x86_64-windows/*.dll "$prefix/drive_c/windows/system32/"
done
echo "Mullion is installed in $wine"
