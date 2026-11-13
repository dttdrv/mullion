# Debugging

This document describes how to investigate a problem in Mullion, from the first failing run to a test that
reproduces it. What a bug report contains is in [CONTRIBUTING.md 1](../CONTRIBUTING.md#1-reporting-a-problem), and
how a fix is shown to work is in [TESTING.md](TESTING.md). Mullion's diagnostic tools are being extended, so the
parts that wait on that work are marked `{logging: ...}`, and those that wait on the test suite `{suite: ...}`. They
are filled in with each update from v0.5 on.

## 1. Establish the failing run

An investigation starts from one run that fails and can be repeated. Its record names the executable, the Mullion
revision, the Wine version, the configuration and settings, the steps that lead to the failure, and what was seen.

The kind of failure decides the evidence it needs:

- a game that does not start: the logs and Wine's console output ([section 2](#2-collect-and-read-the-logs))
- wrong rendering: a screenshot, the shader involved and, where possible, a capture
  ([section 5](#5-capture-a-metal-frame))
- a crash: the crash report with its stack, and the flight record ([section 7](#7-read-a-hang-or-a-slow-frame))
- a hang: the thread stacks of both sides of the bridge, taken while it hangs
  ([section 7](#7-read-a-hang-or-a-slow-frame))
- slow rendering: a profile of the frames ([section 7](#7-read-a-hang-or-a-slow-frame))

Evidence of one kind does not settle another. A shader compiler error does not show that the shader caused a visible
defect, and a thread waiting for the GPU does not show that the GPU is stuck.

A contributor without a Mac can still read supplied logs, follow the source locations they name, examine the shaders
involved and prepare a reduced case ([section 9](#9-reduce-the-problem-to-a-test)). A claim about how Metal executes
something stays unconfirmed until the affected path runs on a suitable Mac.

## 2. Collect and read the logs

Two environment variables control Mullion's logs ([`log.cpp`](../src/util/log/log.cpp)):

- `MULLION_LOG_LEVEL` is one of `trace`, `debug`, `info`, `warn`, `error` and `none`, in lowercase. Any other value,
  or the variable unset, selects `info`.
- `MULLION_LOG_PATH` names a directory, which must already exist, as a path the Windows process can open. Each DLL
  writes its own file there, named after the executable and the DLL, such as `Game_d3d11.log` and `Game_d3d12.log`.
  The value `none` turns files off. Without the variable, Mullion writes to Wine's console output only.

A file is created at the first message it accepts, and the next run of the same executable replaces it. Each attempt
therefore gets a directory of its own, and its logs and Wine's console output are kept before the next attempt
starts. If a file is missing, either nothing reached the level, output was turned off, or the directory could not be
written. A missing file alone does not show that Mullion was not loaded.

A refusal reported through `FAIL(code)` ([`log.hpp`](../src/util/log/log.hpp)) is logged as a warning with the
function, the error code and the source location, so the level `error` hides refusals. Wine's console output is kept
beside the files, since the native shader cache reports there and not in them. {logging: the values that decided a
refusal, and a time, process and thread on every line}.

## 3. Check the configuration

The options are listed in [`mullion.conf`](../mullion.conf) and read by
[`config.cpp`](../src/util/config/config.cpp) in this order:

1. the file `MULLION_CONFIG_FILE` names, or else `mullion.conf` in the process's working directory, which is usually
   but not always the executable's directory, or `dxmt.conf` there when `mullion.conf` cannot be opened
2. `MULLION_CONFIG`, which holds `option=value` pairs and replaces values from the file
3. Mullion's built-in settings for known applications, for the options neither of the first two set

A section of the file applies to the executable whose file name it names. The `DXMT_` names of variables still work,
but empty and unset values are not treated the same way everywhere, so an investigation of an override records both
names.

Changing an option is a comparison: it shows that the option changes the result, not that it fixes the defect. The
options a run used belong in its record, above all those that change generated shaders, such as
`d3d11.sampleNaNToZero`, `d3d11.defuseFma` and `mullion.shaderMetalVersion`. {logging: the effective value and
source of every option, written once at start-up, with malformed and unknown options reported}.

## 4. Use Wine and Metal diagnostics

Wine, Metal and Mullion each have their own diagnostics, set separately.

**Wine.** `WINEDEBUG` selects Wine's debug channels
([wine(1)](https://github.com/wine-mirror/wine/blob/master/tools/wine/wine.man.in)).
`WINEDEBUG=+timestamp,+pid,+seh,+loaddll` shows exceptions and the modules each process loaded, which is where a
failure to start begins. `+all` produces too much to read and changes timing. `WINEDEBUG` does not change Mullion's
own level, which `MULLION_LOG_LEVEL` sets.

**Metal API validation.** `MTL_DEBUG_LAYER=1`, set in the environment Wine starts with, enables Metal's API
validation, which by default logs a detected error and stops
([Apple](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage)). Validation is slow,
so performance comparisons run without it. Metal's shader validation
([Apple](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage)) also exists, and
has not been verified with Mullion's shaders.

**Metal HUD.** `MTL_HUD_ENABLED=1` shows frame times and GPU time over the game, `MTL_HUD_LOG_ENABLED=1` writes them
to the console each second, and `MTL_HUD_LOG_SHADER_ENABLED=1` logs each shader compilation
([Apple](https://developer.apple.com/documentation/xcode/monitoring-your-metal-apps-graphics-performance)). These
logs show whether frame time spikes coincide with shader compilation.

Validation of the test suite is described in [TESTING.md 6.1](TESTING.md#6-running-tests-and-reading-results):
{suite: the validation command and the diagnostics it records}.

## 5. Capture a Metal frame

Mullion has a path for writing a GPU trace that Xcode opens
([`mullion_capture.cpp`](../src/mullion/mullion_capture.cpp)). It has not yet been verified as a procedure under Wine:

- `MTL_CAPTURE_ENABLED=1` allows capture, as Apple
  [describes](https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-programmatically)
- `MULLION_CAPTURE_EXECUTABLE` names the executable to capture, without `.exe`
- `MULLION_CAPTURE_FRAME` names the frame to capture, or F10 captures the next one
- `MULLION_CAPTURE_TRIGGER` names a file (a Unix or a Windows path) whose appearance captures the next frames, from
  outside the game (`echo 4 > /tmp/mullion-capture`); it may hold the number of frames, and Mullion deletes it
- `MULLION_CAPTURE_FRAMES` is how many frames a capture holds, 1 by default (frame generation needs at least 2)
- with `MTL_CAPTURE_ENABLED=1` a queue encodes on one thread (`MULLION_D3D12_ENCODE_THREADS` overrides it), because a
  capture copies every buffer and heap the CPU writes for each command buffer that uses them. For the same reason the
  D3D12 arguments the GPU reads are Metal buffers of 16 MB, then 32 MB and so on as a program uses more, rather than
  one of 1 GB
- the trace is written as a `.gputrace` in `MULLION_CAPTURE_PATH`, else in `MULLION_LOG_PATH`, or in the working
  directory when both are unset (or `MULLION_LOG_PATH` is `none`)

Both the D3D11 and the D3D12 queues take part. Metal captures only command buffers created and committed while the
capture runs, and a message naming the trace file does not show that the capture succeeded: {logging: capture start
and stop reported with Metal's result and error, verified by a trace opened in Xcode with the intended commands and
resources}. Capturing a Wine process from Xcode itself has not been verified.

## 6. Inspect shader conversion and caches

Three different things can be examined: the Direct3D bytecode the game supplied, the Metal IR Mullion converted it
to, and the pipelines Metal compiled from that.

**Dumps.** `MULLION_SHADER_DUMP_PATH` names a directory where Direct3D 12 writes, on selected failure paths and for
pipelines named at `trace`, the containers of the shaders involved, named by hash with a `.dxbc` extension whether
they hold DXBC or DXIL ([`d3d12_pipeline_graphics.cpp`](../src/d3d12/d3d12_pipeline_graphics.cpp)). Direct3D 11
writes `shader_dump_<hash>.cso` into the working directory on selected failure paths, only in builds with the
`mullion_debug` option ([`d3d11_pipeline_cache.cpp`](../src/d3d11/d3d11_pipeline_cache.cpp)). Some refusals happen
before any dump, and a dump file is checked before it is used as evidence. {logging: one dump format for both APIs,
holding the bytecode, the stages, the root signature, the conversion options and the converted output}.

**The converter.** With the `build_airconv_for_windows` option, the build also makes
[`airconv`](../src/airconv/airconv_cli.cpp), a Windows program that converts a shader outside the game:
`airconv <shader> -S -o <output>` writes LLVM assembly, and `-A` a Metal library. Its conversion options are fixed,
while a game's come from the device and the configuration, so its output is not necessarily what the game ran.
Converted IR passes the LLVM verifier before optimization ([CODE.md 4.1](CODE.md#4-shaders)), except with `-O0`,
whose output is unverified and is checked with the verifier before anything else reads it.

**What the converter leaves out.** `MULLION_AIRCONV_SKIP` holds names, separated by commas, of paths the converter
leaves out of every shader, to find which one a shader goes wrong with or what one costs; each name is described where
it is read in the source. It is part of the shader cache's key. `custom-border` leaves out the path that samples a
border color Metal lacks, so such a sampler reads a border Metal has, and `anisotropic-gradients` lets an anisotropic
sampler with a bias read Metal's own level of detail, whose bias Metal drops once the footprint passes the sampler's
anisotropy. `MULLION_TRACE_SHADER=1` names the samplers that need either path.

**Caches.** Mullion keeps its converted shaders in a cache of its own
([`mullion_shader_cache.cpp`](../src/mullion/mullion_shader_cache.cpp)). `MULLION_SHADER_CACHE=0` turns it off, and
`MULLION_SHADER_CACHE_PATH` moves it to another absolute macOS directory. Metal keeps a separate cache of compiled
pipelines, which `MULLION_USE_DEFAULT_METAL_CACHE=1` leaves at Metal's default ([`dxgi.cpp`](../src/dxgi/dxgi.cpp)).
Turning Mullion's cache off therefore does not make every compilation cold. A comparison uses a separate cache
directory, so that shared caches are not deleted.

## 7. Read a hang or a slow frame

**A hang** is read from its thread stacks before its code. They are taken while the hang lasts, from both sides of
the bridge, with the loaded modules recorded so that the stacks can be matched to their binaries:

- Windows threads: `winedbg`, attached to the game's Windows process ID, then `info thread` to list the threads and
  `bt <thread-id>` for the stack of each
  ([winedbg(1)](https://github.com/wine-mirror/wine/blob/master/programs/winedbg/winedbg.man.in))
- native threads: the game's macOS process, found by its process ID, sampled with Activity Monitor's
  [Sample Process](https://support.apple.com/en-us/guide/activity-monitor/actmntr2225/mac), or `thread backtrace all`
  in [LLDB](https://lldb.llvm.org/use/map.html)

{suite: collecting every Windows and macOS thread stack, symbolized against the loaded binaries}.

Mullion adds its own observations. At `trace`, the D3D12 queue logs each submission with its time and thread, and a
command buffer still awaited after ten seconds is reported with its Metal status
([`d3d12_command_queue.cpp`](../src/d3d12/d3d12_command_queue.cpp)). That report shows a long wait, not necessarily a
deadlock. `MULLION_D3D12_GPU_ERRORS=1` asks Metal for each encoder's execution status, for D3D11 as well despite its
name ([`winemetal_unix.c`](../src/winemetal/unix/winemetal_unix.c)), and `MULLION_TRACE_ENCODERS=1` logs each D3D11
render pass at `info` ([`mullion_diag.hpp`](../src/mullion/mullion_diag.hpp)). `MULLION_D3D12_ISOLATE=1`, with
`MULLION_LOG_LEVEL=trace`, waits after each pass, which can locate a failing pass but also changes scheduling, and
can hide the synchronization fault being sought.

**The flight recorder** is always on ([`winemetal_thunks.c`](../src/winemetal/winemetal_thunks.c)). Each thread keeps
its last records in memory: D3D12 submissions, command buffers with their Metal identities and completion status,
fence signals and waits, presents, D3D11 command buffers, the device's removal and exceptions. A record costs its
thread about 10 ns, and nothing is written while the game runs. A removed device, a Metal command buffer error, a D3D12
command buffer still awaited after ten seconds, a feature level `D3D12CreateDevice` refuses (the record says which
level the device reaches), or an exception that ends a process unless it is caught writes the records in time order to `~/Library/Logs/Mullion/<executable>.flight`, or to `MULLION_DIAG_PATH` when it is set, and
Wine's console output names the file. A process that ends after such a dump adds what was recorded since, and
`MULLION_FLIGHT=exit` writes one at the end of every process, for a failure Mullion does not see.

**A slow frame** is profiled with `MULLION_DIAG=profile` and `MULLION_DIAG_PATH`, a macOS directory, and read with
[`tools/mullion-diag`](../tools/mullion-diag): `mullion-diag report <directory>` lists frame times and where their
time goes, and `mullion-diag frames <directory>` summarizes the present intervals of each recording. Its own
description lists what the records do not cover. {logging: one offline report that gathers a run's logs, profiles,
stacks and shader dumps}.

## 8. Bisect a regression

A bisection needs a failure that repeats and a revision known to be good. The application, the workload, the
configuration and the diagnostics stay the same throughout, in a checkout and build used for nothing else.
[`git bisect`](https://git-scm.com/docs/git-bisect) does the search: a revision that does not build or cannot run the
reproduction is marked with `git bisect skip`, never as good, and `git bisect log` keeps the record, skips included.
Each revision is built with its own dependencies, the libraries it loaded are checked, and each runs with Mullion's
shader cache off or fresh, since the cache is versioned by the converter and not by the revision. A faster revision
counts as good only when it also renders correctly ([TESTING.md 10](TESTING.md#10-performance)).
{suite: running one fixed case against each revision}.

The first bad commit is where the observed behaviour changed. It is not necessarily the whole cause, and skipped
revisions next to it can leave several candidates.

## 9. Reduce the problem to a test

A reduction keeps what causes the failure and removes the rest: the API calls, descriptors, shader inputs, resource
relationships, queue order, reuse and sizes that tell failure from success. A rendering reduction reads the pixels
back, and a synchronization reduction keeps the dependency that fails, not a delay that happens to expose it.

`MULLION_D3D12_DUMP_FRAMES=<first>:<count>` writes the back buffers of a run's frames as raw `.frame` files under
`MULLION_LOG_PATH`, and [`tools/frames.py`](../tools/frames.py) finds regions that change for one frame and change
back in the next. It does not report corruption that persists, or changes that are small, so empty output does not
show that rendering was correct. Neither shows the commands that drew a frame.

The reduction becomes a test written as [TESTING.md 5](TESTING.md#5-writing-a-test) describes, seen failing before
the fix: {suite: adding a reduced case, with its control}.
