<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/mullion-dark.svg">
  <img src="docs/mullion.svg" width="88" alt="">
</picture>

# Mullion

Mullion translates Direct3D 10, 11 and 12 to Metal, so Windows games run on Apple silicon Macs through Wine. It is free
and open source, and built on [DXMT](https://github.com/3Shain/dxmt).

Windows games draw with Direct3D and Macs draw with Metal, so something has to translate. Mullion does that, straight
to Metal, without Vulkan in between. It only does the graphics: [Wine](https://www.winehq.org) or
[CrossOver](https://www.codeweavers.com/crossover) runs the game itself, and Mullion is installed into one of them.

Mullion is a pre-release. Get it from [releases](https://github.com/dttdrv/mullion/releases), and
[report](https://github.com/dttdrv/mullion/issues/new/choose) the games that break.

## What you need

- A Mac with Apple silicon, M1 or later, on macOS 15 or later.
- Wine or CrossOver. Stock Wine works, without patches or DLL overrides.
- A 64-bit game, or a 32-bit one that uses Direct3D 10 or 11. Direct3D 12 needs a 64-bit game.

Wine for x86_64 and CrossOver need [Rosetta](https://support.apple.com/en-us/102527). Wine for arm64 works too: its
x86_64 programs load Mullion's ARM64X libraries as ARM64EC. A 32-bit game needs a Wine that runs 32-bit programs.

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

[Games](https://github.com/dttdrv/mullion/wiki/Games) lists the games people have reported, and a game that works goes
in [Game reports](https://github.com/dttdrv/mullion/discussions/categories/game-reports).

A game that breaks gets an [issue](https://github.com/dttdrv/mullion/issues/new/choose), with the logs. Start the game
with `MULLION_LOG_PATH=<folder>` set and Mullion writes a log for each of its DLLs into that folder. After a crash there
is also a file in `~/Library/Logs/Mullion`, named after the game's executable and ending in `.flight`.

## Results

On the same Mac, the Black Myth: Wukong benchmark at the High preset runs at 37 FPS on Mullion at 1800 × 1169, and at
34 FPS on D3DMetal 4.0 beta 3 at 1512 × 982, about 30 % fewer pixels. [Results](https://github.com/dttdrv/mullion/wiki/Results)
has God of War Ragnarök and 3DMark too, with screenshots.

## What it supports

| API | Feature level | |
|---|---|---|
| Direct3D 12 | up to 12_2 | On Metal 4 where the GPU has it, and on Metal 3 otherwise. |
| Direct3D 11 | 11_1 | |
| Direct3D 10 and 10.1 | 10_1 | Runs on the Direct3D 11 implementation. |

What Direct3D 12 reports to a game:

| Feature | Reported | |
|---|---|---|
| Feature level | 12_2 | 12_0 and above need macOS 26.4 or later, for Metal's placement sparse resources. On earlier versions Direct3D 12 reports 11_1. |
| Shader model | 6.7 | Its advanced texture operations are not done yet. |
| Resource binding | Tier 3 | |
| Resource heaps | Tier 2 | |
| Tiled and reserved resources | Tier 3 | macOS 26.4 or later. |
| Conservative rasterization | Tier 3 | |
| Variable rate shading | Tier 2 | |
| Ray tracing | Tier 1.1 | `MULLION_D3D12_RAYTRACING=0` reports none, for games that fail with it. |
| Mesh and amplification shaders | Tier 1 | |
| Sampler feedback | Tier 1.0 | |
| Programmable sample positions | Tier 1 | |
| Depth bounds test | Yes | |
| Tessellation, geometry shaders, stream output | Yes | Stream output after tessellation is not done yet. |
| Pipeline libraries, planar video formats, D3D11On12 | Yes | |
| Writable MSAA textures | Not yet | |
| View instancing | No | |

This describes the main branch. Feature levels 12_1 and 12_2 and Metal 4 are in the v0.5 alphas, and come to a full
release with v0.5. What comes next is in the
[roadmap](https://github.com/dttdrv/mullion/discussions/categories/announcements).

## How it is checked

| Check | Size | What it does |
|---|---|---|
| Mullion's own tests | over 700 | Each runs Direct3D on the GPU and reads the result back. |
| Deliberate faults | over 1,000 | Each breaks one thing in the code, and a test has to give the wrong answer that break causes. |
| [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton)'s tests | 575 test functions | Its Direct3D 12 tests run against Mullion, and every result is recorded. |
| [Wine](https://gitlab.winehq.org/wine/wine)'s tests | 327 test functions | Its Direct3D 10, 11, 12 and DXGI tests, recorded the same way. |

The tests are in [`tests`](tests), the faults in [`tests/breaks`](tests/breaks) and the records in
[`tests/conformance`](tests/conformance). [TESTING.md](docs/TESTING.md) says what each kind of check shows and what it
does not.

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

Mullion would have gotten nowhere without the people and projects below.

It is built on [DXMT](https://github.com/3Shain/dxmt) by Feifan He and CodeWeavers, from commit
[`7c8dee1`](https://github.com/3Shain/dxmt/commit/7c8dee1c2d73415301ceb7d1fa810861cef4cd67) (16 September 2026). DXMT
provides the Direct3D 10 and 11 foundation, the DXBC shader converter and the Metal bridge. The DXIL front end and most
of Direct3D 12 were written for Mullion.

Many of its fixes and features are the work of [aaf2tbz](https://github.com/aaf2tbz),
[partprogramming09](https://github.com/partprogramming09), [OmniTroid](https://github.com/OmniTroid) and
[cyyever](https://github.com/cyyever), and every report of a game that ran or broke helped. The
[contributors](https://github.com/dttdrv/mullion/graphs/contributors) page lists everyone who sent a change.

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