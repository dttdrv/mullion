# Mullion

Mullion translates Direct3D 10, 11 and 12 to Metal, so Windows games run on Apple silicon Macs through Wine. It is free
and open source, and built on [DXMT](https://github.com/3Shain/dxmt).

## In plain words

A Windows game draws its picture by asking Direct3D, the graphics part of Windows. A Mac has no Direct3D. It has Metal,
Apple's own way of talking to the graphics chip. Mullion stands between the two: it takes what the game asks of
Direct3D and asks Metal for the same thing, directly, with no other layer in between.

Mullion is only the graphics part. The game itself is run by [Wine](https://www.winehq.org) or
[CrossOver](https://www.codeweavers.com/crossover), which run Windows programs on macOS without Windows, and Mullion is
installed into one of them.

Mullion is a pre-release. Get it from [releases](https://github.com/dttdrv/mullion/releases), and
[report](https://github.com/dttdrv/mullion/issues/new/choose) the games that break.

## What you need

- A Mac with Apple silicon, which means an M1 or any later chip, on macOS 15 or later.
- Wine or CrossOver. Stock Wine works, without patches or DLL overrides.
- A 64-bit game, or a 32-bit one that uses Direct3D 10 or 11. Direct3D 12 needs a 64-bit game.

In more detail: Wine for x86_64 and CrossOver need [Rosetta](https://support.apple.com/en-us/102527), Apple's tool for
running Intel programs. Wine for arm64 works too: its x86_64 programs load Mullion's ARM64X libraries as ARM64EC. A
32-bit game needs a Wine that runs 32-bit programs.

## Install

1. Download the newest [release](https://github.com/dttdrv/mullion/releases) and unpack it.
2. Close the game.
3. Open Terminal in the unpacked folder and run the line for what you use:

   ```sh
   ./install.sh --crossover "<bottle>"   # CrossOver, with the name of your bottle
   ./install.sh <wine> [prefix ...]      # Wine, with the folder that holds bin/wine
   ```

4. Start the game. If macOS blocks `winemetal.so` at the first launch, allow it under System Settings, Privacy &
   Security, and start the game again.

The first launch of a game converts its shaders, which takes a while in large games. Later launches use what was kept.

To update Mullion, do the same with the new release, and run the line again after a CrossOver update.
[Install](https://github.com/dttdrv/mullion/wiki/Install) covers what each line does, how to undo it, and installing by
hand.

## Games

[Games](https://github.com/dttdrv/mullion/wiki/Games) lists the games people have reported. A game that works goes in
[Game reports](https://github.com/dttdrv/mullion/discussions/categories/game-reports), so the next person knows.

When a game breaks, open an [issue](https://github.com/dttdrv/mullion/issues/new/choose). The logs help the most: start
the game with `MULLION_LOG_PATH=<folder>` set, and Mullion writes a log for each of its DLLs into that folder. After a
crash there is also a small file in `~/Library/Logs/Mullion`, named after the game's executable and ending in
`.flight`. Attach both.

## Results

On the same Mac, the Black Myth: Wukong benchmark at the High preset runs at 37 FPS on Mullion at 1800 × 1169, and at
34 FPS on D3DMetal 4.0 beta 3 at 1512 × 982, about 30 % fewer pixels. [Results](https://github.com/dttdrv/mullion/wiki/Results)
has God of War Ragnarök and 3DMark too, with screenshots.

## What it supports

A game asks for a Direct3D version and a feature level, which is a named set of graphics features. Mullion offers these:

| API | Feature level | |
|---|---|---|
| Direct3D 12 | up to 12_2 | Shader models up to 6.7, ray tracing tier 1.1, mesh and amplification shaders, conservative rasterization tier 3, variable rate shading tier 2, tiled resources tier 3, the depth bounds test, tessellation, geometry shaders, stream output, sampler feedback, reserved resources, pipeline libraries, planar video formats and D3D11On12. |
| Direct3D 11 | 11_1 | |
| Direct3D 10 and 10.1 | 10_1 | Runs on the Direct3D 11 implementation. |

This describes the main branch. Feature levels 12_1 and 12_2 and Metal 4 are in the v0.5 alphas, and come to a full
release with v0.5.

Direct3D 12 runs on Metal 4 where the GPU has it, and on Metal 3 otherwise. Feature levels 12_0 and above need macOS
26.4 or later, for Metal's placement sparse resources; on earlier versions Direct3D 12 reports 11_1. Not done yet:
shader model 6.7's advanced texture operations, writable MSAA textures, and stream output after tessellation. What comes
next is in the [roadmap](https://github.com/dttdrv/mullion/discussions/categories/announcements).

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

## Community

The [Discord server](https://discord.gg/azNpVAj34r) is for talk, quick questions and help with a game. A bug still
needs an [issue](https://github.com/dttdrv/mullion/issues/new/choose), where it is not lost in chat, and a game that
works goes in [Game reports](https://github.com/dttdrv/mullion/discussions/categories/game-reports).

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers reports, changes and review. The rules the code follows are in
[CODE.md](docs/CODE.md), and how it is laid out is in [ARCHITECTURE.md](docs/ARCHITECTURE.md). Questions go to
[Q&A](https://github.com/dttdrv/mullion/discussions/categories/q-a), and problems with Mullion go here, not to DXMT.

## Credits

Mullion stands on other people's work, and would not exist without it.

It is built on [DXMT](https://github.com/3Shain/dxmt) by Feifan He and CodeWeavers, from commit
[`7c8dee1`](https://github.com/3Shain/dxmt/commit/7c8dee1c2d73415301ceb7d1fa810861cef4cd67) (16 September 2026). DXMT
provides the Direct3D 10 and 11 foundation, the DXBC shader converter and the Metal bridge. The DXIL front end and most
of Direct3D 12 were written for Mullion.

Many of its fixes and features came from its contributors, [aaf2tbz](https://github.com/aaf2tbz),
[partprogramming09](https://github.com/partprogramming09), [OmniTroid](https://github.com/OmniTroid) and
[cyyever](https://github.com/cyyever), and from everyone who ran a game and reported what happened. Everyone who sent a
change is on the [contributors](https://github.com/dttdrv/mullion/graphs/contributors) page.

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