# Mullion

Mullion translates Direct3D 10, 11 and 12 to Metal, so Windows games run on Apple silicon through Wine. It is free and open source.

This is a pre-release. Expect games that break, and please report them.

## How it runs today

Game-parity numbers against D3DMetal are **not claimed** until `docs/game-parity.md` has a warm-cache, same-hardware, same-settings row. The Wukong ~28 vs 34 FPS and God of War Ragnarök figures previously in this table were shader-compile screenshots, not a benchmark.

Time Spy's graphics tests render correctly, though 3DMark does not report a score for them yet. Steel Nomad does not start yet. See `docs/game-parity.md` for the matrix (untested titles stay unclaimed) and `docs/advertised-features.md` for advertised D3D12 feature edges.

| Mullion | D3DMetal |
|---|---|
| ![Black Myth: Wukong benchmark result on Mullion](docs/wukong-mullion.jpg) | ![The same benchmark on D3DMetal](docs/wukong-d3dmetal.jpg) |
| ![God of War Ragnarök on Mullion](docs/gow-mullion.jpg) | ![The same place on D3DMetal](docs/gow-d3dmetal.jpg) |

Direct3D 12 is supported at feature level 12_2 with shader models up to 6.7, including ray tracing at tier 1.1, mesh and amplification shaders, conservative rasterization at tier 3, variable rate shading at tier 2, tiled resources at tier 3, the depth bounds test, tessellation, geometry shaders, stream output, sampler feedback, reserved resources, pipeline libraries and planar video formats, and D3D11On12. On macOS 26 it runs on Metal 4, and on Metal 3 before that. Direct3D 10 and 11 are supported at feature level 11_1. Next for Direct3D 12 is more work on performance and reliability.

## Requirements

- A Mac with Apple silicon, M1 or later.
- macOS 15 or later.
- Wine for x86_64, or a WoW64 Wine that also loads i386 PE. Stock Wine works, with no patches and no DLL overrides.
- Direct3D 10 and 11 are built for both PE32 and PE32+. Direct3D 9 is off by default (`-Denable_d3d9=true` builds it for both). Direct3D 12 is 64-bit only: its descriptor handles pack a heap pointer into 64 bits (`MULLION_USE_EMBEDDED_HEAP_POINTER`), so an x86 D3D12 is not a working API. 32-bit D3D9/10/11 support is a runtime claim only after the PE32 GPU tests on that host pass.

## Install

1. Download the latest [release](https://github.com/dttdrv/mullion/releases) and unpack it.
2. Run `./install.sh <wine> [prefix ...]`. `<wine>` is the directory that contains `bin/wine`.

The script copies the libraries into `<wine>/lib/wine`, where they replace Wine's Direct3D, and into each prefix named on the command line. Prefixes created later need nothing.

In CrossOver, `./install.sh --crossover <bottle>` sets up that bottle and leaves CrossOver.app as it is. The bottle gets a CrossOver root of its own in `~/Library/Mullion/CrossOver`, made of links to CrossOver's files with Mullion as its DXMT, and its graphics are set to DXMT. Updates of CrossOver keep it; run the script again after one, so that files it adds are linked too. Give the path of CrossOver.app as a third argument if it is not in `/Applications` or `~/Applications`. To undo, put back the bottle's `cxbottle.conf.before-mullion`.

Without the script, rename `CrossOver.app/Contents/SharedSupport/CrossOver/lib/dxmt` to `dxmt-original`, put the unpacked release folder in its place under the name `dxmt`, and set the bottle's graphics to DXMT. Replace the whole folder: CrossOver's 32-bit DXMT cannot drive Mullion. A release that includes `i386-windows` is installed into `syswow64` by `install.sh`; an archive without that directory still leaves 32-bit games on Wine's own Direct3D. A CrossOver update brings back its own DXMT this way.

Confirm the process loaded Mullion, not DXMT or D3DMetal: after a game starts, the SHA-256 of `d3d9.dll` / `d3d11.dll` / `d3d12.dll` in the prefix's `system32` (and `syswow64` for a 32-bit executable) must match the same files in the unpacked release. `otool -L` on the Unix `winemetal.so` is the other half.

On the first launch macOS blocks `winemetal.so`; allow it under System Settings, Privacy & Security, and start the game again.

The archive has the layout of DXMT's builds (`x86_64-windows` and `x86_64-unix`, and `i386-windows` when a 32-bit PE half is packaged). Tools that install DXMT can install Mullion the same way. To install by hand, copy those directories into `<wine>/lib/wine`, the DLLs of `x86_64-windows` into `drive_c/windows/system32`, and the DLLs of `i386-windows` into `syswow64` of a WoW64 prefix. There is no i386 Unix library: 32-bit PE talks to the host 64-bit `winemetal.so` through WoW64.

## Settings

| | |
|---|---|
| `MULLION_LOG_PATH=<directory>` | Writes a log file for each DLL. Attach it to bug reports. |
| `MULLION_D3D12_METAL4=0` | Keeps Direct3D 12 on Metal 3 on macOS 26. If a game draws differently there than on Metal 3, please report it. |
| `MULLION_CONFIG="dxgi.customVendorId=106b"` | Reports Apple's vendor ID. By default Mullion reports the GPU's own name with AMD's vendor ID, because some games exit at startup when they do not know the vendor. |
| `MULLION_D3D11_FAULT_DEVICE=<n>` | A test facility: the run's nth completed command buffer is treated as one the GPU failed (`1` is the first), so the device-removal contract (the reason a device keeps, its removal event, Direct3D 9's cooperative level and the Reset that recovers it) runs without a GPU fault. Not for playing. |

Options can also go in a `mullion.conf` next to the game's executable, or in the file `MULLION_CONFIG_FILE` names. The `mullion.conf` in this repository lists them. The `DXMT_` names and `dxmt.conf` of earlier builds still work.

## Building

You need macOS, Meson 1.3 or later, Xcode with its Metal toolchain, LLVM 15 as static libraries for x86_64, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and a Wine build tree.

```sh
git submodule update --init
meson setup build --cross-file build-win64.txt \
  -Dnative_llvm_path=<llvm> -Dwine_build_path=<wine build tree> \
  --buildtype release --prefix "$PWD/install"
meson install -C build
```

`install/` then has the layout of a release. With `-Dwine_builtin_dll=false` the DLLs are built for a prefix's `system32` instead, and need `WINEDLLOVERRIDES="d3d12,d3d12core,d3d11,d3d10core,dxgi=n"`.

## Tests

About 200 test programs draw, read the result back and compare it with what the Direct3D specification says, each run as every Apple GPU family. Wine's and vkd3d-proton's Direct3D tests also run against Mullion, and their verdicts are recorded in `tests/conformance`.

Configure with `-Denable_tests=true`, then run the suite in a Wine:

```sh
python3 tests/run.py --build build --wine <wine> --prefix <prefix> --family native,9
```

The tests compile their own shaders, so the prefix needs `d3dcompiler_47.dll` (from [winetricks](https://github.com/Winetricks/winetricks)) and `dxcompiler.dll` (from [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)) in its `system32`.

## Reporting a problem

Open an issue with the Mac model, the macOS version, how the game was run, and the log. A screenshot helps most. Reports of games that work are welcome as well. Patches are covered in `CONTRIBUTING.md`.

## Credits

Mullion is built on [DXMT](https://github.com/3Shain/dxmt) by Feifan He and CodeWeavers, from commit `7c8dee1` (16 September 2026). DXMT provides the Direct3D 10 and 11 foundation, the DXBC shader converter and the Metal bridge. The DXIL front end and most of Direct3D 12 were written for Mullion, about 28,000 lines of source and 78,000 lines of tests so far.

Mullion is a separate project. Please do not report its problems to DXMT.

Thanks to [DXVK](https://github.com/doitsujin/dxvk), whose utility code DXMT carries; to [Wine](https://www.winehq.org) and [LLVM](https://llvm.org); to Microsoft for the [DirectX specifications](https://microsoft.github.io/DirectX-Specs/) and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler); to Apple for the [Metal documentation](https://developer.apple.com/documentation/metal); and to [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) and [Mesa](https://www.mesa3d.org), whose tests and drivers we learned from.

## License

LGPL, version 2.1 or later, like DXMT. See `LICENSE`. `LICENSE.OLD` is the license DXMT had before April 2026, which still covers the code from then.
