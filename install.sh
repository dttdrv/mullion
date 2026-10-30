#!/bin/sh
# installs Mullion from a release archive: install.sh <wine> [prefix ...], or install.sh --crossover <bottle> [app]
# <wine> is the directory that holds bin/wine. the libraries go where Wine keeps its own, in place of its Direct3D,
# and into each prefix named: a prefix made before them does not know the ones Wine did not have.
#
# A prefix has two Windows DLL directories when its Wine is WoW64: system32 for the prefix's own architecture and
# syswow64 for 32-bit Windows code. A 32-bit-only prefix has one, system32, holding i386 DLLs. The Windows halves in
# the archive are named for their machine (x86_64-windows, i386-windows, aarch64-windows), so the prefix is asked
# which directory each belongs in: the PE machine of its kernel32.dll, or the directories it has when that is not
# there. The Unix halves stay in the Wine's lib/wine/<machine>-unix, where the bridge looks for them; an i386 Windows
# half has no i386 Unix half of its own and uses the host's 64-bit Unix half through WoW64.
#
# a CrossOver bottle gets a root of its own instead, which its CX_ROOT names: links to the files of CrossOver.app (or
# app), with Mullion as the DXMT the bottle chooses. CrossOver.app stays as it is, so its updates keep this; to undo,
# put back the bottle's cxbottle.conf.before-mullion, and run this again after an update that adds files
set -e
here=$(cd "$(dirname "$0")" && pwd)

# the PE machine of an image, as the number its COFF header holds (0x14c i386, 0x8664 x86_64, 0xaa64 arm64), or
# nothing when the file is not a PE image this can read
pe_machine() {
  [ -f "$1" ] || return 1
  # e_lfanew at 0x3c, then Machine after the "PE\0\0" signature
  offset=$(od -An -tu4 -j60 -N4 "$1" 2>/dev/null | tr -d ' \n')
  [ -n "$offset" ] || return 1
  od -An -tx2 -j$((offset + 4)) -N2 "$1" 2>/dev/null | tr -d ' \n'
}

machine_name() {
  case "$1" in
  14c) echo i386 ;;
  8664) echo x86_64 ;;
  aa64) echo aarch64 ;;
  *) echo unknown ;;
  esac
}

if [ "$1" = --crossover ]; then
  conf="$HOME/Library/Application Support/CrossOver/Bottles/$2/cxbottle.conf"
  app=$3
  [ -n "$app" ] || for app in /Applications/CrossOver.app "$HOME/Applications/CrossOver.app"; do
    [ ! -d "$app" ] || break
  done
  cx="$app/Contents/SharedSupport/CrossOver"
  [ -n "$2" ] && [ -f "$conf" ] && [ -d "$cx/lib/dxmt" ] ||
    { echo "usage: $0 --crossover <bottle> [CrossOver.app]" >&2; exit 1; }
  root="$HOME/Library/Mullion/CrossOver"
  rm -rf "$root"
  mkdir -p "$root/lib/dxmt"
  for entry in "$cx"/* "$cx"/lib/*; do
    case ${entry#"$cx"/} in lib | lib/dxmt) ;; *) ln -s "$entry" "$root/${entry#"$cx"/}" ;; esac
  done
  cp -R "$here"/*-windows "$here"/*-unix "$root/lib/dxmt/"
  [ -f "$conf.before-mullion" ] || cp "$conf" "$conf.before-mullion"
  # the bottle's environment chooses DXMT and names the root, in place of what it said of either
  awk -v root="$root" '
    function ours() { print "\"CX_GRAPHICS_BACKEND\" = \"dxmt\""; print "\"CX_ROOT\" = \"" root "\""; set = 1 }
    /^"(CX_ROOT|CX_GRAPHICS_BACKEND)"/ { next }
    { print }
    $0 == "[EnvironmentVariables]" { ours() }
    END { if (!set) { print "[EnvironmentVariables]"; ours() } }
  ' "$conf" > "$conf.mullion"
  mv "$conf.mullion" "$conf"
  echo "Mullion is installed for the bottle $2; restart it if it runs"
  exit
fi

[ -d "$1/lib/wine" ] || { echo "usage: $0 <wine directory> [wine prefix ...]" >&2; exit 1; }
wine=$1
shift

# the Windows halves, by the machine their directory names them
for kind in "$here"/*-windows; do
  [ -d "$kind" ] || continue
  cp -R "$kind"/. "$wine/lib/wine/$(basename "$kind")/"
done
# the Unix halves, which a 32-bit Windows half reaches through the host's WoW64
for kind in "$here"/*-unix; do
  [ -d "$kind" ] || continue
  cp -R "$kind"/. "$wine/lib/wine/$(basename "$kind")/"
done

for prefix; do
  windows="$prefix/drive_c/windows"
  [ -d "$windows" ] || { echo "note: $prefix has no drive_c/windows; skipped" >&2; continue; }

  # the prefix's own architecture: its kernel32, or the directories it was made with. A 64-bit prefix has
  # syswow64; a 32-bit one does not.
  machine=$(pe_machine "$windows/system32/kernel32.dll" 2>/dev/null || true)
  prefix_arch=$(machine_name "${machine:-}")
  if [ "$prefix_arch" = unknown ]; then
    if [ -d "$windows/syswow64" ]; then
      prefix_arch=x86_64
    else
      prefix_arch=i386
    fi
  fi

  for kind in "$here"/*-windows; do
    [ -d "$kind" ] || continue
    name=$(basename "$kind")
    arch=${name%-windows}
    case "$arch" in
    i386 | x86)
      if [ "$prefix_arch" = i386 ]; then
        target=system32
      else
        target=syswow64
      fi
      ;;
    x86_64 | amd64 | aarch64 | arm64)
      if [ "$prefix_arch" = i386 ]; then
        echo "note: $prefix is 32-bit; $name is not installed in it" >&2
        continue
      fi
      target=system32
      ;;
    *)
      echo "note: $name is not an architecture this installer knows; skipped" >&2
      continue
      ;;
    esac

    [ -d "$windows/$target" ] || {
      echo "note: $prefix has no windows/$target, so $name is not installed in it" >&2
      continue
    }
    cp "$kind"/*.dll "$windows/$target/"
  done
done
echo "Mullion is installed in $wine"
