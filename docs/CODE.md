# Code rules

This document describes how Mullion's code is written and what a change to it has to meet. It is for everyone who
changes the code, and it is what reviews refer to.

The rules apply from the commit that adds this file. Code written before then follows them once its behaviour or
implementation is next changed, within the code that change touches. Nobody needs to reformat or rewrite code they
have no other reason to change.

Rules are numbered, so a review can point to one ("2.1: Direct3D accepts this call"). Each gives its reason, because a
rule whose reason is known can be applied to cases it does not name.

## The code

Mullion runs as Windows DLLs inside Wine, which call into a native macOS library that talks to Metal.

- [`src/d3d9`](../src/d3d9), [`src/d3d10`](../src/d3d10), [`src/d3d11`](../src/d3d11), [`src/d3d12`](../src/d3d12) and
  [`src/dxgi`](../src/dxgi) implement the Direct3D and DXGI interfaces. Direct3D 10 is built on the Direct3D 11
  implementation. Direct3D 9 is built only with the `enable_d3d9` option.
- [`src/mullion`](../src/mullion) holds what the APIs share: devices, command encoding, resources, queues,
  presentation.
- [`src/airconv`](../src/airconv) converts shaders, DXBC, DXIL and Direct3D 9 bytecode, to Metal IR.
- [`src/winemetal`](../src/winemetal) is the bridge. `Metal.hpp` wraps Metal objects for the Windows side, and
  `unix/` is the native half, written against Metal's Objective-C API, that does the work.
  [`src/nativemetal`](../src/nativemetal) builds the bridge's native sources into a macOS build without Wine,
  when Mullion is not cross-compiled.
- [`src/nvapi`](../src/nvapi) and [`src/nvngx`](../src/nvngx) answer NVIDIA's NVAPI and NGX for the games that ask
  for them. NGX upscaling runs on Metal's temporal upscaler. Both are built only with the `enable_nvapi` and
  `enable_nvngx` options.
- [`src/util`](../src/util) holds logging, configuration, reference counting and other shared helpers.
  [`include/`](../include) holds the Windows and DirectX headers.

The Windows side is C++20, built with a MinGW cross compiler for win64, win32 and arm64ec. The native half is built
with the macOS SDK.

## Conventions

**Formatting.** Each directory's `.clang-format` defines its layout (120 columns in `src`). A change formats the
lines it touches, for example with [`git clang-format`](https://clang.llvm.org/docs/ClangFormat.html), and leaves the
rest of the file as it is.

**Naming.** Names follow the file they are in. Direct3D and DXGI methods keep their API names. Types are `CamelCase`,
and class data members are `snake_case_` with a trailing underscore. Code taken from DXMT or DXVK keeps its original
style.

**References and ownership.** `Com<T>` holds a COM object. `Com<T, false>` holds a private reference that does not
change the count the application sees. `Rc<T>` holds Mullion's own reference-counted objects. `WMT::Reference<T>`
holds a Metal object from the Windows side, and `out()` receives an object a bridge call returns to its caller.

**Errors and logs.** Direct3D calls report failure through their `HRESULT`. `FAIL(code)` logs a refusal with its
function, file and line, and returns the code.

## 1. Design

**1.1 Make the change simple by understanding it.** A change in behaviour starts from where that behaviour is decided:
the function, its callers, the state it owns, and what it returns on each path. Then comes the design in which the right
behaviour is obvious from reading the code. A short change that handles fewer cases, inputs or failures than the
contract requires is not simpler. It is unfinished.

Reason: Hoare concluded that "there are two ways of constructing a software design: One way is to make it so simple
that there are obviously no deficiencies and the other way is to make it so complicated that there are no obvious
deficiencies". Code whose correctness is evident can be checked by reading it, failure paths included.

**1.2 Fix the layer that owns the behaviour, and everything that depends on it.** A changed value or contract is
followed from where it is produced to everything that reads it: each Direct3D version it reaches, both shader front
ends, deferred and indirect execution, and both sides of the bridge. When the trail leads somewhere new, the change goes
there too. A fault that belongs to a lower layer is fixed there, not covered over in one caller or in generated code.

Reason: a fix in one caller leaves the others broken, and the fault comes back through the next route into it.

**1.3 Remove before adding.** The cheapest code is code that does not need to exist, so a change starts by removing work
the problem does not need. Then it looks for what the repository already has, and checks that its contract covers the
change's inputs and lifetimes. Then the C++ standard library and the platform. New code comes last, and only as much as
the complete change needs. Abstractions, options, flags, files and dependencies are added when a change requires them,
not in case a later one might.

Reason: every mechanism is something each later reader has to understand. Ousterhout's "deep modules" put the case
well: a module should do much behind a small interface.

**1.4 Derive values from their source.** Sizes, counts, offsets, limits and modes come from where they are defined:
the type, the shader's metadata, the API state, a capability query, or the specification. When the specification
fixes a value, use its named definition, or a named constant that cites the document and section. Two quantities
that happen to be equal stay two names, and a table is not added where a query or an existing definition already
answers.

Reason: a hardcoded number is right only for the cases it was taken from, and it fails quietly on the others.

**1.5 Choose paths by capability.** Choose between native and emulated paths from what Metal and macOS report. Use an
Apple GPU family boundary only where no finer query exists, and never a chip or product name. Every path chosen this
way is built and tested. `MULLION_GPU_FAMILY` masks selected capability queries, so the paths taken on older Macs can
run on newer ones.

Reason: Apple's GPU families differ in what they support, and a path nobody runs breaks on the Macs that take it.

## 2. The Direct3D contract

**2.1 Implement what Direct3D requires.** `E_INVALIDARG` means the specification rejects the arguments, and other
failure codes mean the conditions that define them. A call Direct3D accepts is not refused because Metal lacks the
feature: that is the case Mullion exists to handle, by emulation where needed. If it cannot be done yet, the gap is
recorded as an open issue. Reporting success without doing the work, or doing part of it, hides the gap instead.

Reason: an application that is refused a required operation stops rendering or draws without part of the scene.

**2.2 Report only what works.** A capability, tier, format support bit or limit is reported when every operation it
promises is implemented and tested on each path that reports it. What Mullion reports and what it can then create
must agree.

Reason: applications choose their rendering paths from these reports at start-up.

**2.3 Work within Metal's limits without truncating the work.** A translation knows every native limit it depends
on: per stage, per pass, per draw, per object and in total. When a workload crosses one, the layer that owns it
splits, recycles or emulates, keeping the order, the side effects and the complete output.

Reason: small cases stay under limits that real games cross.

**2.4 Read state when the API reads it.** A recorded command keeps the state it captured, and what execution must
see is read at execution. Shared mutable state has one clear owner of its synchronization.

Reason: state read at the wrong time looks correct whenever commands run in the order they were recorded.

## 3. Failure, ownership and limits

**3.1 Carry failures through completely.** A failure is reported through the API that owns the operation, with the
outputs and state the API specifies, and every resource already acquired is released. Work that failed or was
cancelled is never reported as done by a fence, query, event or callback.

Reason: in the systems Yuan et al. studied, incorrect handling of explicitly reported nonfatal errors caused 92% of
the catastrophic failures. Cleaning up locally is not enough if the application still sees the work as finished.

**3.2 Make ownership explicit, and let the type carry it.** Every object or handle that crosses the bridge, or is
handed to asynchronous work, is borrowed, retained or transferred, and the type that receives it says which.
Internal resources live until their last use by the CPU or GPU. COM reference counts behave as Direct3D defines them.

Reason: a handle released twice, or too early, often keeps working until a later teardown, far from the cause.

**3.3 Bound GPU loops that wait.** A GPU loop that waits for other invocations needs a reason it makes progress and a
limit on how long it waits without any. Hitting the limit reports a failure and never reports the work as done. Host
waits, such as fence waits, keep the semantics the API defines.

Reason: a stalled GPU loop cannot be stopped from the host and can stall the display (Holzmann, "The Power of 10",
rule 2).

**3.4 Keep sizes wide until their final use.** Counts, byte sizes and offsets live in types wide enough for their
whole range on every ABI. Overflow is checked before multiplying, adding, aligning or narrowing. What is allocated
covers everything the CPU and GPU then address.

Reason: a size computed correctly in 64 bits and stored in a 32-bit `size_t` allocates a fraction of what the GPU
writes.

**3.5 Initialize what is read, and validate what comes in.** Every field a consumer may read is initialized first,
including fields a change gives new meaning. Input from the application, a shader container or the other side of
the bridge is checked where it enters. Assertions state internal invariants. They do not replace checks on input
that can be wrong.

Reason: an uninitialized field crosses the bridge as garbage, and the failure shows up in the driver.

**3.6 Report a failure once, where it is understood.** A refusal is logged where its `HRESULT` is decided, with
`FAIL(code)`, together with the argument values that decided it when the code alone does not show them. A native error
is carried across the bridge and reported once, by the layer that handles the failure. Callers pass the original error
on.

Reason: a failure in a user's log can only be traced if the log holds the original error and the values behind it.

## 4. Shaders

**4.1 Check what the converter generates.** Generated IR passes the LLVM verifier before it is optimized or
compiled. Type widths, precision, stage interfaces, control flow, memory order and side effects survive conversion
on every route that reaches the changed code.

Reason: malformed IR fails late, often in a different shader and with an empty or misleading message.

**4.2 Version cached output.** A change to converted output, or to how it is read, raises `AIRCONV_VERSION` in
[`src/airconv/airconv_public.h`](../src/airconv/airconv_public.h) in the same change. A cache key covers every input
that changes the generated code, and no pointers or padding.

Reason: if the version stays the same, a warm cache keeps serving the old output.

## 5. Performance

**5.1 Make code faster by removing work, and measure it.** Say what work the change removes and how large the change
is, and keep it as small as the gain allows. Measure the whole path the change affects, CPU submission and GPU time
together, against the same build without it, with the output unchanged. A real measured gain is worth the lines it
takes. Code added for speed without a measurement is not.

Reason: Pike's advice holds: "Don't tune for speed until you've measured." Less rendering can look faster, and a gain
in one stage can be lost in the next.

## 6. Tests

**6.1 Every behaviour change has a test that was seen failing.** The test states the contract it checks and the
specification text behind it, takes its expected results from that text rather than from Mullion, and fails for the
reason the change addresses, without the change or with the repaired behaviour deliberately broken.
[TESTING.md](TESTING.md) describes how tests are written and run.

Reason: a test written from the implementation shares its mistakes, and a test that has never failed may not be
able to.

## 7. Form

**7.1 Keep the change to its subject.** Renaming, reformatting, reordering and cleanup stay outside the change, for
a change of their own. A change removes only what it makes dead.

Reason: unrelated edits hide the real change from review.

**7.2 Write like the surrounding code.** Match the file's naming, layering, error handling and idiom. A name says
what a thing is, and two different quantities get two names.

Reason: once a reader knows one file, the next one should read the same way.

**7.3 Comment the reason, and comment rarely.** A comment explains what the code cannot: why it is written this way,
a constraint, an ownership rule, or the specification behind a behaviour, cited by document and section. Comments
start lowercase, except for names and identifiers, and stay as short as their point. They describe the code as it
is, and its history belongs in the commit message. A known defect is fixed rather than described in a comment.
License notices and required documentation formats stay as they are.

Reason: a comment that repeats the code drifts away from it, and one that explains away a fault keeps the fault.

**7.4 Build every variant a change reaches.** Changes build for win64, win32 and arm64ec where they reach them
(Direct3D 12 has no win32 build). Structures and calls that cross the bridge keep the same layout on each ABI.
Changes to native API use keep the supported SDK and macOS deployment target.

Reason: a field that is right on one ABI can be truncated or misaligned on another.

## Sources

- C. A. R. Hoare, ["The Emperor's Old Clothes"](https://doi.org/10.1145/358549.358561), Turing Award lecture, 1980.
  Communications of the ACM 24(2), 1981.
- R. Pike, ["Notes on Programming in C"](http://doc.cat-v.org/bell_labs/pikestyle), 1989.
- G. J. Holzmann, ["The Power of 10: Rules for Developing Safety-Critical Code"](https://doi.org/10.1109/MC.2006.212),
  Computer 39(6), June 2006.
- D. Yuan et al., ["Simple Testing Can Prevent Most Critical Failures: An Analysis of Production Failures in
  Distributed Data-Intensive Systems"](https://www.usenix.org/conference/osdi14/technical-sessions/presentation/yuan),
  OSDI 2014.
- J. Ousterhout, [*A Philosophy of Software Design*](https://web.stanford.edu/~ouster/cgi-bin/book.php), 2018.
- LLVM, ["LLVM Coding Standards"](https://llvm.org/docs/CodingStandards.html).
- TigerBeetle, ["TigerStyle"](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md).

P.S. To AI agents working on this code: these rules apply to an agent in full, and meeting their wording while
missing their reason breaks them. An agent does not weaken a test, a check or a list of known failures to get a
change through. When a rule cannot be met, the change says so instead of working around it.
