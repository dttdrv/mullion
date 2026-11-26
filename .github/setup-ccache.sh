#!/bin/sh
# Meson auto-wraps only the native compiler with ccache, not compilers named in --cross-file.
# Prepending ccache masquerade symlinks puts every cross-compiler (GCC and llvm-mingw) through ccache too.
mkdir -p /tmp/ccache-bin
CCACHE_BIN=$(command -v ccache)
for tool in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ \
            i686-w64-mingw32-gcc i686-w64-mingw32-g++ \
            arm64ec-w64-mingw32-gcc arm64ec-w64-mingw32-g++; do
  ln -sf "$CCACHE_BIN" "/tmp/ccache-bin/$tool"
done
export PATH="/tmp/ccache-bin:$PATH"
export CCACHE_BASEDIR="$GITHUB_WORKSPACE"
export CCACHE_COMPILERCHECK=content
# Depend mode skips running the compiler with -E (which fails for llvm-mingw -marm64x)
# and reads the .d depfiles Meson already generates via -MD -MF.
export CCACHE_DEPEND=true
