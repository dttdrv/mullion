# Mullion

Mullion runs Direct3D 10, 11 and 12 games on Metal on Apple silicon. It is free and open source.

We built Mullion because we wanted to play Direct3D 12 games on our Macs with software that anyone can read, fix and ship. Until now that has meant a closed translator or a detour through Vulkan. Mullion goes from Direct3D straight to Metal, in the open.

This is a pre-release. Things will break. Tell us when they do, and tell us what works too: that is how Mullion gets better.

## Why Mullion

- It covers the Direct3D 12 that current games use: shader models up to 6.6, ray tracing at tier 1.1, mesh and amplification shaders, tessellation, geometry shaders, stream output, sampler feedback and tiled resources.
- It talks to Metal directly. Nothing sits between Direct3D and the GPU's own API.
- One install covers Direct3D 10, 11 and 12.
- It works with the Wine you already have. It needs no patched Wine and no DLL overrides, and it installs the way DXMT does.
- It is held to the Direct3D specification. 99 test programs draw, read the result back and compare it with what the specification says.
- It is open source under the LGPL. You can read it, build it, and ship it with your own tools.

## Requirements

- A Mac with an M3 or later. Support for M1 and M2 is in progress.
- macOS 15 or later.
- A Wine for x86_64. Mullion runs 64-bit games.

## Installation

Download the latest [release](https://github.com/dttdrv/mullion/releases). The archive has the layout of DXMT's builds, `x86_64-windows` and `x86_64-unix`, so it goes wherever your tool keeps DXMT or Wine keeps its own libraries.

For a Wine of your own, unpack the archive and run:

```sh
./install.sh <wine> [prefix ...]
```

`<wine>` is the directory that contains `bin/wine`. The script copies the libraries into `<wine>/lib/wine`, where they take the place of Wine's Direct3D, and into each prefix you name. A prefix created afterwards needs nothing, and no DLL overrides are needed.

To install by hand, copy the contents of `x86_64-windows` and `x86_64-unix` into the directories of the same names in `<wine>/lib/wine`, and the DLLs of `x86_64-windows` into `drive_c/windows/system32` of each existing prefix.

## Configuration

Mullion reports the GPU's own name with AMD's vendor ID, because some games exit at startup when they do not know the vendor. `DXMT_CONFIG="dxgi.customVendorId=106b"` reports Apple's.

`DXMT_LOG_PATH=<directory>` writes a log file for each DLL.

## Building

Mullion builds on macOS with Meson 1.3 or later, Xcode with its Metal toolchain, LLVM 15 as static libraries for x86_64, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and a Wine build tree.

```sh
git submodule update --init
meson setup build --cross-file build-win64.txt \
  -Dnative_llvm_path=<llvm> -Dwine_build_path=<wine build tree> \
  --buildtype release --prefix "$PWD/install"
meson install -C build
```

`install/` then has the layout of a release. With `-Dwine_builtin_dll=false` the DLLs are built for a prefix's `system32` instead, and need `WINEDLLOVERRIDES="d3d12,d3d12core,d3d11,d3d10core,dxgi=n"`.

## Tests

Configure with `-Denable_tests=true`, then run the suite in a Wine, once as each GPU family:

```sh
python3 tests/run.py --build build --wine <wine> --prefix <prefix> --family native,9
```

The tests compile their own shaders, so the prefix needs `d3dcompiler_47.dll` (from [winetricks](https://github.com/Winetricks/winetricks)) and `dxcompiler.dll` (from [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)) in its `system32`.

## Tell us how it went

Open an issue for a game that breaks, with your Mac model and macOS version, how you ran it, and the log. A screenshot of the problem helps most. We also want to hear about the games that just work.

## Credits

We did not start from nothing. Mullion is built on [DXMT](https://github.com/3Shain/dxmt) by Feifan He and CodeWeavers, from commit `7c8dee1` (16 September 2026). DXMT gave us the Direct3D 10 and 11 foundation, the DXBC shader converter and the Metal bridge. On top of it we wrote the DXIL front end and most of the Direct3D 12: about 20,000 lines of source and 34,000 lines of tests.

Mullion is a separate project. Please do not report its problems to DXMT.

Thanks also to [DXVK](https://github.com/doitsujin/dxvk), whose utility code DXMT carries; to [Wine](https://www.winehq.org) and [LLVM](https://llvm.org); to Microsoft for the [DirectX specifications](https://microsoft.github.io/DirectX-Specs/) and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler); to Apple for the [Metal documentation](https://developer.apple.com/documentation/metal); and to [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) and [Mesa](https://www.mesa3d.org), whose tests and drivers we learned from.

## License

Mullion is licensed under the LGPL, version 2.1 or later, like DXMT. See `LICENSE`. `LICENSE.OLD` is the license DXMT had before April 2026, which still covers the code from then.
