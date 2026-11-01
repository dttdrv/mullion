# Testing

This document describes how Mullion's tests are built, written, run and reported. The rules for when a change needs a
test are in [CODE.md 6.1](CODE.md#6-tests), and what a pull request reports is in [CONTRIBUTING.md
3](../CONTRIBUTING.md#3-showing-that-it-works). The suite is being reworked, so the commands, options and formats it
will provide are marked `{suite: ...}` until they exist. They are filled in with each update from v0.5 on.

## 1. What each check shows

Each kind of check establishes something different, and none stands in for another.

- A host test checks code that runs without the GPU, such as parsing, arithmetic or ownership. It does not show
  that the GPU path is correct.
- A shader conversion check shows that the converter produced valid output. A shader that compiles has not been
  shown to compute the right result.
- A Direct3D test runs the API on the GPU and reads the result back. It shows what was rendered or computed, not
  what a window displayed.
- A presentation test shows what reached the screen through the presentation path it names.
- A conformance run executes another project's Direct3D tests against Mullion
  ([section 9](#9-conformance-coverage-and-faults)).
- A performance run measures time, and means something only when the output it measured was correct
  ([section 10](#10-performance)).

What a contributor without a Mac can build and run: {suite: checks available without a Mac, supported hosts,
prerequisites and build commands}. The pull request then lists the checks still missing, as [CONTRIBUTING.md
3.6](../CONTRIBUTING.md#3-showing-that-it-works) describes, and they are run on a Mac: {suite: how a draft asks for the
checks it is missing}.

## 2. Building the tests

The tests build with the rest of Mullion, as described in the [README](../README.md#building), with the test options
added: {suite: prerequisites and build commands}.

**2.1 Build with assertions enabled.** Correctness runs use a build in which assertions are compiled in, in every
component the change reaches. The effective build options are what count, not the name of the build directory. Meson
controls assertions with its [`b_ndebug`](https://mesonbuild.com/Builtin-options.html) option, separately from the build
type.

**2.2 Build every variant the change reaches.** A change is built for each ABI it reaches ([CODE.md
7.4](CODE.md#7-form)). A combination that is not built is reported as not built.

**2.3 Build first, then run.** Building and installing finish before tests run, and nothing replaces the libraries under
test while they run. A test that loads a library from a build in progress shows nothing about either build.

## 3. A clean environment

Tests run in a Wine installation and a Wine prefix used for nothing else. Both start without other graphics libraries or
DLL overrides, and every addition is recorded: the Mullion build, the shader compilers and the fixtures. Mullion is
installed into the Wine installation as well as the prefix, so a fresh prefix alone is not a clean environment: {suite:
clean Wine and prefix setup}.

An arm64 Wine runs Windows programs on macOS only with the 4KB-page entitlement, which a Wine built and signed ad hoc
cannot have: macOS ends such a Wine's processes. The tests then run in a signed Wine of another product, such as
CrossOver's or MetalSharp's, without changing its files: `tests/run.py --clone` copies the Wine once, with the whole app
bundle it is in (macOS honours the entitlement only in the bundle whose provisioning profile grants it) and each of its
binaries' signatures, and installs the build into the copy (in `tests-wine` of the first build, or the directory
`--clone` names). A Wine that keeps its Direct3D libraries apart from `lib/wine` (MetalSharp's
`lib/mullion`, CrossOver's `lib/dxmt`) has the build installed there too, since it loads them from there.

What needs no such Wine still runs without one. An x86_64 Wine runs under Rosetta with no entitlement, and runs an
x86_64 build's tests (`build-win64.txt`). A test program of an architecture the Wine does not run (an arm64 build's in
an x86_64 Wine, x86_64 programs in an arm64 Wine without FEX) is not run, and the tests of the host's own still run when
macOS ends the Wine: the run lists each test it did not run, with why, apart from its results.

A run records what it actually loaded, so a result can be traced to the source that produced it: {suite: loaded artifact
manifest}. The record covers the source revision and any local changes, the build options, the Wine version, and for
each test executable, library and shader compiler loaded, its resolved path, its content hash and the build that
produced it, with the effective DLL overrides and runtime options.

## 4. Shader compilers and generated shaders

Tests compile HLSL with Microsoft's compilers: the DXBC compiler, available through
[Winetricks](https://github.com/Winetricks/winetricks), and the DXIL compiler from
[DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler). A compiler counts as present when it
matches the architecture of the test process, loads, and compiles every profile the tests use: {suite: compiler packages
by architecture, setup and verification}. A missing compiler is missing coverage, never a pass.

`tests/compilers.py <build>/tests-compilers` downloads both, once, from Microsoft's releases (DXC's release and the
Windows SDK's `Redist\D3D`, at the versions and SHA-256 hashes it pins), for arm64, x64 and x86. `run.py` puts those of
the machines the Wine runs into the prefix: from `--compilers <directory>` when given, which replaces the prefix's,
or else from the first build's `tests-compilers` and a Wine's own `share/wine/shadercompilers`, for what the prefix
lacks. A test whose compiler the prefix lacks for its architecture is listed as not run, with the compiler it needs;
Wine's own `d3dcompiler_47` does not count, as it compiles fewer of the tests' shaders.

The LLVM IR Mullion generates passes the LLVM verifier before it is optimized or compiled for Metal ([CODE.md
4.1](CODE.md#4-shaders)): {suite: verification, unoptimized conversion included}. A failure that follows unverified
output is read only after the output passes. A change to cached shader output is tested without the cache, with output
reused by a second process, and with output stored by an older version rejected ([CODE.md 4.2](CODE.md#4-shaders)).

## 5. Writing a test

**5.1 State the contract.** A test opens with a comment naming the API and version, the behaviour it checks, and the
section of the specification that defines it, with a link: the [D3D11.3 functional
specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm) for Direct3D 10 and 11,
and the [DirectX specifications](https://microsoft.github.io/DirectX-Specs/) for Direct3D 12. Where the specification
leaves a behaviour undefined, the test says so and does not check it.

**5.2 Take expected results from the contract.** Expected values come from the specification, its reference
implementation, or a recorded observation on Windows that names the runtime and the device. They never come from what
Mullion produces.

**5.3 Choose inputs that can tell cases apart.** Inputs cover every decision the code makes, and sizes below, at and
above each internal limit: a batch, a SIMD group, a threadgroup, a page, the end of a buffer. Two quantities that are
often equal differ in at least one case. A change to lifetime also tests release, cleanup and reuse, and a change to
order uses values that show which operation came last. Generated inputs record their seed.

**5.4 Observe the result itself.** A test reads back what was written and reports each wrong value with its location.
Each location the test expects to change starts with a value it cannot be expected to hold, so a missing write cannot
pass as a correct one. A checksum alone does not do, since reordered or cancelling errors keep it. A presentation test
names the presentation path it uses and checks the pixels that path displayed.

**5.5 Register every variant.** A test is registered for each API version, shader front end, ABI and capability it
applies to, with its time limit and whether it can share the GPU with other tests: {suite: test helper and registration
example}. Every registered variant appears when the tests are listed.

## 6. Running tests and reading results

A run lists the tests and configurations it needs, runs them, and accounts for each one: {suite: discovery, selection
and run commands}. An unknown test name, a missing binary or an empty selection stops the run with an error.

A test passes when it observed the expected result, completed, and raised no unexpected Metal validation or device
error. A test of error handling declares the error it expects, and checks the outputs, the cleanup and the completion
state the API defines for it, with the test process exiting normally. Results are reported as passed, failed, skipped,
unsupported, crashed, timed out or not run because setup failed: {suite: result format and exit status}. A required test
that did not run leaves the run incomplete.

The suite's own failure handling is tested too: missing inputs, unknown tests, contradictory results, interrupted
processes, stale builds, a changed failure report and a failure followed by a passing retry: {suite: runner checks and
CI coverage}.

**6.1 Run under validation.** Correctness runs enable Metal's [API
validation](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage).
[DEBUGGING.md](DEBUGGING.md) describes captures and other tools for reading a failure.

**6.2 Give every skip its reason.** A skip names what is missing and where. An optional capability the configuration
lacks is a skip. Support that the API version, feature level or reported tier requires is never skipped: when it is
missing, the test fails. A capability Mullion reports and then fails to provide is a failure as well, and a support
check does not hide it.

## 7. Failing controls and repeats

**7.1 A control fails for the defect.** A failing control runs the same test with the same expectations on the code
without the change, or with the repaired behaviour deliberately broken in isolation. It reaches the changed operation
and gives the wrong result the defect causes. A crash or a timeout counts only when it is traced to that defect, and a
build, compiler, fixture or loader failure does not count: {suite: failing control and maintained fault}.

**7.2 Repeat what can vary.** New tests, and tests of timing, lifetime, threading or order, run several times, in fresh
processes and, where it applies, repeatedly within one process: {suite: repeats and retry reporting}. Every attempt is
reported. A failure followed by a passing retry is still a failure, and a run repeated alone can help find interference
but does not replace the first result. Passing a fixed number of repeats does not show that a race is absent.

## 8. GPU families

Each result names the physical GPU and the capabilities exposed during the run, with the macOS and Wine versions, the
ABI and the shader front end. `MULLION_GPU_FAMILY` ([CODE.md 1.5](CODE.md#1-design)) masks selected capability queries,
so the fallback paths an older Mac takes can run on a newer one: {suite: GPU family and host matrix}.

Other queries still describe the physical GPU, so a masked run is not a run on the older GPU. It does not reproduce an
older GPU's scheduling, compiler, memory, timing or driver behaviour, and cannot measure its performance. A change to
one GPU's own behaviour is tested on that GPU.

## 9. Conformance, coverage and faults

**9.1 Known failures stay failures.** A known failure is listed with its test and case, its configuration, what it
reported, its cause and the issue that tracks it: {suite: known-failure format and comparison}. A run compares its
results with that list. A changed report from a listed test, a new failure, an unexpected pass and a missing result each
show as a difference. A change that fixes a listed failure removes it. A crash, a timeout or a test that stopped early
is never a known failure, since it hides the cases after it. The list compared against is an identified, reviewed
baseline, and a change to it, or to an expected result, needs evidence independent of the run being compared. "No new
failures" and "every test passed" are reported as different results.

**9.2 Conformance runs account for every case.** The [Wine](https://gitlab.winehq.org/wine/wine) and
[vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) Direct3D tests run against Mullion from pinned
revisions, with any patches kept and explained: {suite: conformance inputs, commands and report}. Each upstream check
keeps its own result, a test that ran only partly is reported as partial, and an absent or empty suite fails the run.

**9.3 Coverage and fault scores are measurements, not proof.** Coverage states what was counted and what it was counted
against. A deliberate fault is caught only when a test gives the wrong answer the fault causes. Faults that survive, do
not build or give no answer are reported as such: {suite: coverage and mutation commands and report}.

## 10. Performance

A performance run starts from the work the change removes ([CODE.md 5.1](CODE.md#5-performance)) and compares the same
source with and without the change, after both have shown correct output: {suite: performance commands and report}.
Inputs, build options, the machine's load, shader compilers, cache state and warm-up match between the two, and runs
alternate between them. Timed runs have validation off, record any other diagnostics that were on, and last long enough
to measure the effect claimed. Every repetition is kept, with CPU submission time, GPU time and total time reported
separately, along with the number of runs, their spread and their slowest cases. Shader compilation and steady execution
are measured separately. A run excluded from the result stays in the report with its reason.

## 11. What a pull request reports

A pull request reports the evidence [CONTRIBUTING.md 3](../CONTRIBUTING.md#3-showing-that-it-works) asks for, in the
suite's report format: {suite: pull request evidence}.