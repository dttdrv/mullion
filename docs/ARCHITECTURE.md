# Architecture

This document is a map of Mullion's code for someone about to change it: where each part lives, what it owns, what
crosses each boundary, and the invariants each layer keeps. How a change is prepared is in
[CONTRIBUTING.md](../CONTRIBUTING.md), the rules for the code are in [CODE.md](CODE.md), how a change is tested is in
[TESTING.md](TESTING.md), and how a failure is investigated is in [DEBUGGING.md](DEBUGGING.md).

## 1. The overall shape

Mullion is a set of Windows DLLs that a game loads through Wine in place of Direct3D, and a native macOS library
that talks to Metal. Each API records work in its own form before the GPU runs it:

```
Direct3D 11 call -> captured operation -> encoding thread --+
                                                            +-> bridge command records -> Metal command buffer
Direct3D 12 call -> bridge command records in the allocator +
                                    ^
shader bytecode (DXBC or DXIL) -> airconv -> Metal library
```

Direct3D 11 and Direct3D 12 record and submit work in two separate ways
([section 2](#2-recording-submission-and-completion)). They share resources, shader conversion, the bridge to Metal
and presentation. Direct3D 10 is implemented on Direct3D 11
([`d3d10_device.cpp`](../src/d3d10/d3d10_device.cpp)), and its device is created through Direct3D 11
([`d3d10core.cpp`](../src/d3d10/d3d10core.cpp)).

| Directory | Owns |
|---|---|
| [`src/dxgi`](../src/dxgi) | adapters, outputs, factories and swap chain creation |
| [`src/d3d11`](../src/d3d11), [`src/d3d10`](../src/d3d10) | the Direct3D 11 and 10 interfaces, their contexts and D3D11On12 |
| [`src/d3d9`](../src/d3d9) | the Direct3D 9 interfaces and fixed-function pipeline, built with the `enable_d3d9` option |
| [`src/d3d12`](../src/d3d12) | the Direct3D 12 device, command lists, queues, descriptors, pipelines and resources |
| [`src/mullion`](../src/mullion) | what the APIs share: the device, Direct3D 11's command encoding, buffers and textures, presentation, the shader cache |
| [`src/airconv`](../src/airconv) | shader conversion from DXBC and DXIL to Metal IR |
| [`src/winemetal`](../src/winemetal) | the bridge: Metal objects on the Windows side, and their native implementation in [`unix/`](../src/winemetal/unix) |

Each device finds the Metal device through DXGI's adapter: Direct3D 11 through the shared Mullion device in
[`mullion_device.cpp`](../src/mullion/mullion_device.cpp), Direct3D 12 through a device of its own in
[`d3d12_device.cpp`](../src/d3d12/d3d12_device.cpp).

D3D11On12 joins the two paths. It creates a Direct3D 11 device with its own queue, shares resources with the
Direct3D 12 device, and orders the two queues through fences and shared events
([`d3d11_on12.cpp`](../src/d3d11/d3d11_on12.cpp)).

## 2. Recording, submission and completion

Recording, submission to Metal, completion on the GPU and the moment the CPU can see the result are four separate
events, and a change keeps them apart.

**Direct3D 11.** A context call validates the state it needs and records an operation, which captures that state
([`d3d11_context_impl.cpp`](../src/d3d11/d3d11_context_impl.cpp)). Draws with geometry shaders or tessellation choose
their own paths here. The immediate context appends operations to its current command chunk
([`d3d11_context_imm.cpp`](../src/d3d11/d3d11_context_imm.cpp)). A deferred context records the same operations into a
command list that the immediate context later executes ([`d3d11_context_def.cpp`](../src/d3d11/d3d11_context_def.cpp)).

The queue's encoding thread runs a chunk's operations against `ArgumentEncodingContext`, which encodes them into a
command buffer that the queue then commits ([`mullion_context.cpp`](../src/mullion/mullion_context.cpp),
[`mullion_command_queue.cpp`](../src/mullion/mullion_command_queue.cpp)). A second thread waits for each command buffer
to complete, performs the readbacks and callbacks the chunk asked for, resets the chunk, and advances the coherent
sequence number from which buffer allocations are reused.

**Direct3D 12.** A command list records its commands into memory its allocator owns, and `Close` ends the list
([`d3d12_command_list.cpp`](../src/d3d12/d3d12_command_list.cpp),
[`d3d12_command_allocator.hpp`](../src/d3d12/d3d12_command_allocator.hpp)). A bundle keeps its calls and replays them
against the state of the list that executes it.

`ExecuteCommandLists` takes what the lists recorded, keeps their allocators alive, and hands the encoding to the queue's
own thread, so that the application's thread goes on recording
([`d3d12_command_queue.cpp`](../src/d3d12/d3d12_command_queue.cpp)). That thread creates Metal encoders, applies the
barriers, and passes the recorded commands to the bridge. A queue that waits on a fence holds its later work back until
the fence is signalled. Destroying the queue can cancel held work instead: its allocators are then released without the
completion callbacks, and that work never retires. Another thread waits for completion, resolves results the CPU reads,
releases the allocators, and signals the fences that follow.

Keeping an allocator alive does not make it safe to reset while the GPU still reads it. Waiting first is the
application's duty, as [`ID3D12CommandAllocator::Reset`][allocator-reset] specifies.

[allocator-reset]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset

**Threads.** Direct3D 11's queue owns an encoding thread, a completion thread and a thread that listens for shared
events. Direct3D 12's queue owns a thread for held work, a completion thread and a watchdog that reports command
buffers awaited too long. Direct3D 11 compiles shader variants on worker threads, while Direct3D 12 compiles in the
thread that creates the pipeline ([section 5](#5-shader-conversion-and-caches)). The bridge attaches a layer to a
window on the main queue, and drawing a frame through GDI runs after the GPU completes it
([section 7](#7-presentation)). [DEBUGGING.md](DEBUGGING.md) describes how to read these threads in a hang.

## 3. Resources and memory

A resource, its current allocation, its views, its GPU address and its residency are five different things.

A Direct3D 11 buffer can replace its allocation and keep its identity, and a dynamic buffer reuses old allocations
once the GPU has passed them, by the coherent sequence number
([`mullion_buffer.cpp`](../src/mullion/mullion_buffer.cpp),
[`mullion_dynamic.cpp`](../src/mullion/mullion_dynamic.cpp)). Replacing an allocation is itself recorded, in order
with the other operations. An operation resolves the buffer's current allocation when it is encoded, after the
replacements recorded before it, and the chunk keeps that allocation alive until the GPU has completed it
([`mullion_context.hpp`](../src/mullion/mullion_context.hpp)).

Direct3D 12 creates committed, placed and reserved resources ([`d3d12_device.cpp`](../src/d3d12/d3d12_device.cpp)).
A placed resource is checked against its heap's bounds and alignment, and a reserved resource needs Metal's
placement sparse support, which sets the tiled resources tier. A buffer registers its GPU address and residency
when it is created and removes them when it is destroyed ([`d3d12_buffer.cpp`](../src/d3d12/d3d12_buffer.cpp)).
Whether it can be mapped depends on its heap's CPU access, not on the Mac's shared memory.

Residency changes collect at the device and are committed before each command buffer is submitted, held ones
included. Sparse mappings run on a separate Metal 4 queue, ordered explicitly against the work around them.

## 4. Descriptors and root arguments

This boundary connects the API's state, the CPU's encoding, GPU memory and the loads the converted shaders make.

A descriptor heap holds a CPU description of each descriptor. Shader-visible resource and sampler heaps also have
the GPU storage that shaders read, and render target and depth stencil heaps hold only CPU descriptions. In that
storage, a raw or structured buffer view carries its address, its metadata and its counter's address, and a
constant buffer view its address and size ([`d3d12_descriptor_heap.hpp`](../src/d3d12/d3d12_descriptor_heap.hpp),
[`d3d12_descriptor_heap.cpp`](../src/d3d12/d3d12_descriptor_heap.cpp)). A root signature decides the offset of each
root argument, how static samplers are stored, and the slots for heaps that shaders index directly
([`d3d12_root_signature.cpp`](../src/d3d12/d3d12_root_signature.cpp)). Recording fills those slots and copies the
argument table into memory the allocator owns.

The converter builds the shader side of the same layout on its own
([`dxbc_binding_rootsig.cpp`](../src/airconv/dxbc_binding_rootsig.cpp)). Both sides agree on layout, units, offsets,
metadata and how addresses are read, so a change to one is followed to the other. Direct3D 11 binds by slot instead,
and conversion chooses between the two layouts ([`dxbc_converter.cpp`](../src/airconv/dxbc_converter.cpp)).

## 5. Shader conversion and caches

[`airconv_public.h`](../src/airconv/airconv_public.h) is the converter's interface. `SM50Initialize` reads a DirectX
container, takes its DXBC tokens or its DXIL part, and produces the shader's state and reflection: the `SM50` and
`dxbc` names are historical and cover both formats. DXBC is decoded into control flow from its tokens. DXIL keeps its
LLVM bitcode, whose resources and operations are read, and its body is joined to the stage around it
([`dxil_converter.cpp`](../src/airconv/dxil_converter.cpp)). Both become an LLVM module for Metal's AIR target, which
is verified, optimized and written as a Metal library ([`airconv_context.cpp`](../src/airconv/airconv_context.cpp)).

What crosses this boundary is the bytecode, the reflection and the compilation arguments together. Geometry,
tessellation, mesh and ray tracing stages each have their own entry points, since Metal runs them differently.

Converted shaders are cached by each API, and in a persistent cache both share:

- Direct3D 11 keeps the variants of each shader and compiles them through its task scheduler
  ([`d3d11_pipeline_cache.cpp`](../src/d3d11/d3d11_pipeline_cache.cpp)).
- Direct3D 12 keys each compilation on the shader, the converter's version, the device and the arguments
  ([`d3d12_pipeline_graphics.cpp`](../src/d3d12/d3d12_pipeline_graphics.cpp)). When the persistent cache is
  available, threads that miss on the same key wait for one another, and the cache is checked again before
  conversion. With it disabled, each miss converts.
- Both share a persistent cache in SQLite on the native side, versioned by `AIRCONV_VERSION`
  ([`mullion_shader_cache.cpp`](../src/mullion/mullion_shader_cache.cpp),
  [`cache.c`](../src/winemetal/unix/cache.c)).

[CODE.md 4](CODE.md#4-shaders) gives the rules for changing converted output and cache keys.

## 6. The bridge

A call from the Windows side to Metal goes through four steps: a method of a `WMT` wrapper
([`Metal.hpp`](../src/winemetal/Metal.hpp)), a C thunk that fills a parameter structure
([`winemetal_thunks.c`](../src/winemetal/winemetal_thunks.c)), Wine's unix call, and the native function that calls
Metal's Objective-C API ([`winemetal_unix.c`](../src/winemetal/unix/winemetal_unix.c)). Encoding passes an encoder
and a linked list of command records (`wmtcmd_base` in [`winemetal.h`](../src/winemetal/winemetal.h)), which the
native side walks.

The bridge keeps three contracts:

- **Representation.** Handles, pointers, parameter layouts and call indices are the same on both sides.
- **Ownership.** `WMT::Reference` either retains a handle it is given or takes over one a call returned already
  retained.
- **Conversion.** Structures that hold pointers are converted for 32-bit callers, nested ones included
  ([`airconv_thunks.h`](../src/winemetal/airconv_thunks.h)). Matching structure sizes do not show that a pointer,
  the lifetime of what it points to, or a result crossed correctly.

When Mullion is not cross-compiled, [`src/nativemetal`](../src/nativemetal) builds the same native sources into a
macOS build without Wine.

## 7. Presentation

A swap chain is created through DXGI ([`dxgi_factory.cpp`](../src/dxgi/dxgi_factory.cpp)), with one for each API
([`d3d11_swapchain.cpp`](../src/d3d11/d3d11_swapchain.cpp), [`d3d12_swapchain.cpp`](../src/d3d12/d3d12_swapchain.cpp)),
and both present through the shared presenter ([`mullion_presenter.cpp`](../src/mullion/mullion_presenter.cpp)). The
presenter scales and applies gamma, so presentation has visible behaviour of its own.

A window presents through a Metal layer or through GDI. The layer comes from Wine's exported Metal view functions,
or from a `CAMetalLayer` the bridge attaches to Wine's window when those functions are missing. Attaching a layer runs
on the main queue without waiting for it, since a synchronous wait there can deadlock with the thread that asked. A
window without a layer gets its frames through GDI: each frame is copied to a buffer and drawn into the window once
the GPU has completed it.

## 8. Capabilities

What Mullion reports is decided in four steps, which a change follows in order:

1. **What the device provides.** The bridge asks Metal for GPU families, sample counts, filtering and sparse
   placement. [`MULLION_GPU_FAMILY`](TESTING.md#8-gpu-families) masks the family answers and selected limits,
   never extending them, while other answers still describe the physical GPU.
2. **What Mullion derives.** Shared code derives the Metal language version and the format support
   ([`mullion_info.cpp`](../src/mullion/mullion_info.cpp), [`mullion_format.cpp`](../src/mullion/mullion_format.cpp)).
3. **What the API reports.** Each API answers its own feature queries. Direct3D 11 selects its feature level in
   [`d3d11.cpp`](../src/d3d11/d3d11.cpp) and fills its option reports in
   [`d3d11_inspection.cpp`](../src/d3d11/d3d11_inspection.cpp). Direct3D 12 answers in
   [`d3d12_device.cpp`](../src/d3d12/d3d12_device.cpp), and derives its maximum feature level from the options it
   reports.
4. **What creation accepts, and which path runs.** Resource and pipeline creation check the same capabilities, and
   choose between native and emulated paths.

A GPU family, a Metal language version, a Direct3D feature level and a single feature's support are four separate
things, and a change keeps them apart. [CODE.md 2.2](CODE.md#2-the-direct3d-contract) gives the rule they serve: a
capability is reported only when everything it promises works.
