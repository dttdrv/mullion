# Mullion

Mullion is a Direct3D 10, 11 and 12 implementation on Metal, for Windows games running under Wine on Apple silicon.

This is a pre-release. Expect problems, and please report them.

## Features

- Direct3D 12 with shader models 6.0 to 6.6, through a DXIL front end written for Mullion.
- Tessellation, geometry shaders, stream output, and mesh and amplification shaders, also through `ExecuteIndirect`.
- Ray tracing at tier 1.1: acceleration structures, inline ray queries and DXR pipelines.
- Sampler feedback, tiled resources, 64-bit atomics, and the loader and core split that Agility SDK games look for.
- Direct3D 10 and 11 with stream output through geometry shaders and `DrawAuto`, and a tessellator that matches Microsoft's reference tessellator bit for bit.
- Runs in an unmodified Wine.
- A test suite of 70 programs. Each checks a rule of the Direct3D specification against what was drawn.

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

## Reporting problems

Open an issue with your Mac model and macOS version, the game, how you ran it, and the log. A screenshot of the problem helps most.

## Credits

Mullion is built on [DXMT](https://github.com/3Shain/dxmt) by Feifan He and CodeWeavers, starting from commit `7c8dee1` (16 September 2026). DXMT provides the Direct3D 10 and 11 foundation, the DXBC shader converter and the Metal bridge. On top of it Mullion adds its DXIL front end and most of its Direct3D 12: about 18,000 lines of source and 21,000 lines of tests.

Mullion is a separate project. Please do not report its problems to DXMT.

Thanks also to [DXVK](https://github.com/doitsujin/dxvk), whose utility code DXMT carries; to [Wine](https://www.winehq.org) and [LLVM](https://llvm.org); to Microsoft for the [DirectX specifications](https://microsoft.github.io/DirectX-Specs/) and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler); to Apple for the [Metal documentation](https://developer.apple.com/documentation/metal); and to [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) and [Mesa](https://www.mesa3d.org), whose tests and drivers we learned from.

## License

Mullion is licensed under the LGPL, version 2.1 or later, like DXMT. See `LICENSE`. `LICENSE.OLD` is the license DXMT had before April 2026, which still covers the code from then.
