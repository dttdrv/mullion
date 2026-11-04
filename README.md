# Mullion

Mullion translates Direct3D 10, 11 and 12 to Metal, so Windows games run on Apple silicon Macs through Wine. It is free
and open source, and built on [DXMT](https://github.com/3Shain/dxmt).

Mullion is a pre-release. Get it from [releases](https://github.com/dttdrv/mullion/releases), and
[report](https://github.com/dttdrv/mullion/issues/new/choose) the games that break. Games that work go in [Game
reports](https://github.com/dttdrv/mullion/discussions/categories/game-reports), and
[Games](https://github.com/dttdrv/mullion/wiki/Games) lists them.

## What it supports

| API | Feature level | |
|---|---|---|
| Direct3D 12 | up to 12_2 | Shader models up to 6.7, ray tracing tier 1.1, mesh and amplification shaders, conservative rasterization tier 3, variable rate shading tier 2, tiled resources tier 3, the depth bounds test, tessellation, geometry shaders, stream output, sampler feedback, reserved resources, pipeline libraries, planar video formats and D3D11On12. |
| Direct3D 11 | 11_1 | |
| Direct3D 10 and 10.1 | 10_1 | Runs on the Direct3D 11 implementation. |
| Direct3D 9 | | Shader models 1 to 3 and the fixed-function pipeline. Not in releases yet: built with `-Denable_d3d9=true`. |

This describes the main branch. Feature levels 12_1 and 12_2 and Metal 4 come to a release with v0.5.

Direct3D 12 runs on Metal 4 where the GPU has it, and on Metal 3 otherwise. Feature levels 12_0 and above need macOS
26.4 or later, for Metal's placement sparse resources; on earlier versions Direct3D 12 reports 11_1. Not done yet:
shader model 6.7's advanced texture operations, writable MSAA textures, and stream output after tessellation. What comes
next is in the [roadmap](https://github.com/dttdrv/mullion/discussions/categories/announcements).

## Results

On the same Mac, the Black Myth: Wukong benchmark runs at 28 FPS on Mullion and 34 FPS on D3DMetal 4.0 beta 3, at the
High preset and 1512 × 982. [Results](https://github.com/dttdrv/mullion/wiki/Results) has God of War Ragnarök and 3DMark
too, with screenshots.

## Requirements

- A Mac with Apple silicon, M1 or later, on macOS 15 or later.
- Wine for x86_64 or CrossOver, with [Rosetta](https://support.apple.com/en-us/102527), or Wine for arm64, whose x86_64
  programs load Mullion's ARM64X libraries as ARM64EC. Stock Wine works, without patches or DLL overrides.
- A 64-bit game. 32-bit games are not supported yet, other than Direct3D 9 games on a win32 build with `enable_d3d9`.

## Install

Unpack the release and run `install.sh` from its folder, with the game closed: `./install.sh --crossover "<bottle>"` for
a CrossOver bottle, or `./install.sh <wine> [prefix ...]` for Wine, where `<wine>` is the folder that holds `bin/wine`.
[Install](https://github.com/dttdrv/mullion/wiki/Install) covers what each one does, how to undo it, and installing by
hand.

If macOS blocks `winemetal.so` at the first launch, allow it under System Settings, Privacy & Security, and start the
game again.

## Settings

`MULLION_LOG_PATH=<folder>` writes a log for each DLL into the folder. Attach the logs to a bug report.
`MULLION_TRACE_GPU=1` and the other `MULLION_TRACE_` categories log one kind of problem in detail, as
[docs/tracing.md](docs/tracing.md) describes. The other settings are on the
[Settings](https://github.com/dttdrv/mullion/wiki/Settings) page, and [`mullion.conf`](mullion.conf) lists every option.

## Building

Building needs macOS, [Meson](https://mesonbuild.com) 1.3 or later, Xcode with the macOS 26.4 SDK and its Metal
toolchain, [LLVM](https://github.com/llvm/llvm-project) 15 as static libraries for x86_64,
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on `PATH`, and a Wine build tree.

```sh
git submodule update --init
meson setup build --cross-file build-win64.txt \
  -Dnative_llvm_path=<llvm> -Dwine_build_path=<wine build tree> \
  --buildtype release --prefix "$PWD/install"
meson install -C build
```

`install/` then has the layout of a release. The tests are described in [TESTING.md](docs/TESTING.md).

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers reports, changes and review. The rules the code follows are in
[CODE.md](docs/CODE.md), and how it is laid out is in [ARCHITECTURE.md](docs/ARCHITECTURE.md). Questions go to
[Q&A](https://github.com/dttdrv/mullion/discussions/categories/q-a), and problems with Mullion go here, not to DXMT.

## Credits

Mullion is built on [DXMT](https://github.com/3Shain/dxmt) by Feifan He and CodeWeavers, from commit
[`7c8dee1`](https://github.com/3Shain/dxmt/commit/7c8dee1c2d73415301ceb7d1fa810861cef4cd67) (16 September 2026). DXMT
provides the Direct3D 10 and 11 foundation, the DXBC shader converter and the Metal bridge. The DXIL front end and most
of Direct3D 12 were written for Mullion.

Thanks to [DXVK](https://github.com/doitsujin/dxvk), whose utility code DXMT carries, to [Wine](https://www.winehq.org)
and [LLVM](https://llvm.org), to Microsoft for the [DirectX specifications](https://microsoft.github.io/DirectX-Specs/)
and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler), to Apple for the [Metal
documentation](https://developer.apple.com/documentation/metal), and to
[vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) and [Mesa](https://www.mesa3d.org), whose tests and
drivers Mullion learned from.

## License

The [GNU Lesser General Public License](LICENSE), version 2.1 or later, like DXMT. DXMT's own notice is in
[docs/licenses/dxmt.txt](docs/licenses/dxmt.txt), and [docs/licenses/dxmt-mit.txt](docs/licenses/dxmt-mit.txt) is the
MIT license of DXMT's releases up to v0.80, which still covers the code from them.