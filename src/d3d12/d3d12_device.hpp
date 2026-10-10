/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once
#include "d3d12.h"
#include "d3d12_command_encoder.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_recording_arena.hpp"
#include "dxgi1_2.h"
#include "dxgi_interfaces.h"
#include "airconv_public.h"
#include "dxmt_buffer.hpp"
#include "dxmt_command.hpp"
#include "dxmt_diag.hpp"
#include "dxmt_fence.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_texture.hpp"
#include "log/log.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#define IMPLEMENT_ME                                                                                                   \
  do {                                                                                                                 \
    Logger::err(str::format(__FILE__, ":", __FUNCTION__, "(", __LINE__, ") is not implemented."));                     \
    abort();                                                                                                           \
    __builtin_unreachable();                                                                                           \
  } while (0);

namespace dxmt {

// Metal's GPU timestamps count nanoseconds on Apple GPUs: sampleTimestamps advances them by 1e9 a second, as D3D11
// reports for its timestamp queries
constexpr uint64_t kGPUTimestampFrequency = 1'000'000'000;

class MTLD3D12CommandAllocator;

class MTLD3D12GraphicsCommandList : public ID3D12GraphicsCommandList7 {
public:
  EncoderData *entry;
  size_t encoder_count;
  // the allocator the closed list recorded into, which a queue keeps until the list has run
  MTLD3D12CommandAllocator *recorded_on = nullptr;
  // whether a command on an acceleration structure that is not there is left for the queue (LaterEncoderData),
  // which records it on a list that leaves nothing
  bool leaves_for_later = true;
};

class MTLD3D12CommandAllocator : public ID3D12CommandAllocator {
public:
  // lists of the allocator's that queues have taken and that have not completed: their recording must stay
  std::atomic<uint32_t> pending{0};
  virtual HRESULT STDMETHODCALLTYPE CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
      void **ppCommandList
  ) = 0;
};

extern const std::string dump_frames;

class MTLD3D12CommandQueue : public ID3D12CommandQueue {
public:
  // `swapchain` owns the presenter and what it draws with: a present the queue holds back keeps it
  virtual HRESULT Present(
      IUnknown *swapchain, Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after,
      uint64_t id, uint64_t number
  ) = 0;
};

// dxgiformat.h of the toolchain ends before the sampler feedback formats
constexpr auto DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = (DXGI_FORMAT)189;
constexpr auto DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = (DXGI_FORMAT)190;

constexpr bool
IsSamplerFeedback(DXGI_FORMAT Format) {
  return Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ||
         Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE;
}

struct AccelerationStructure;
struct AccelerationStructureInputs;
struct CompactedSize;

class MTLD3D12Resource : public ID3D12Resource {
public:
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  Rc<Texture> texture;
  Rc<Buffer> buffer;
  // a sampler feedback map's description as its application gave it: `texture` has one R32_UINT texel per mip region
  // (TranscodeFeedback in dxmt_command_feedback.hpp)
  std::optional<D3D12_RESOURCE_DESC> feedback;

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) = 0;
};

// a resource's tiles as D3D12 numbers them (MTLD3D12Resource::GetResourceTiling)
struct Tiling {
  std::vector<D3D12_SUBRESOURCE_TILING> subresources;
  D3D12_PACKED_MIP_INFO packed{};
  D3D12_TILE_SHAPE shape{};
  UINT total = 0, mips = 1, per_slice = 0;
  bool texture, volume;

  Tiling(ID3D12Resource *pResource) {
    D3D12_RESOURCE_DESC desc;
    pResource->GetDesc(&desc);
    texture = desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
    volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    mips = texture ? desc.MipLevels : 1;
    UINT slices = texture && desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
    UINT count = mips * slices;
    subresources.resize(count);
    static_cast<MTLD3D12Resource *>(pResource)->GetResourceTiling(
        &total, &packed, &shape, &count, 0, subresources.data()
    );
    per_slice = total / slices;
    // where a slice's packed tiles start, or its end without any
    if (!packed.NumPackedMips)
      packed.StartTileIndexInOverallResource = per_slice;
  }

  // the tile a region's `n`th tile is: in a box, walking x, then y, then z; otherwise the next ones in number
  UINT
  index(const D3D12_TILED_RESOURCE_COORDINATE &c, const D3D12_TILE_REGION_SIZE &size, UINT n) const {
    auto &sub = subresources[c.Subresource];
    UINT slice_start = c.Subresource / mips * per_slice;
    if (sub.StartTileIndexInOverallResource == D3D12_PACKED_TILE)
      return slice_start + packed.StartTileIndexInOverallResource + c.X + n;
    UINT x = c.X, y = c.Y, z = c.Z;
    if (size.UseBox) {
      x += n % size.Width, y += n / size.Width % size.Height, z += n / (size.Width * size.Height);
      n = 0;
    }
    // an array's depth is the same mip of the next slices; a volume's is its own
    auto &at = volume ? sub : subresources[c.Subresource + z * mips];
    return at.StartTileIndexInOverallResource + x + at.WidthInTiles * (y + (volume ? at.HeightInTiles * z : 0)) + n;
  }

  // where a tile is in Metal's terms: level, slice and tile coordinates; packed tiles are the tail's, along x
  WMTSparseTextureMapping
  place(UINT tile) const {
    UINT slice = tile / per_slice, r = tile % per_slice;
    if (r >= packed.StartTileIndexInOverallResource)
      return {0, {r - packed.StartTileIndexInOverallResource, 0, 0}, {1, 1, 1}, packed.NumStandardMips, slice};
    UINT mip = 0;
    while (mip + 1 < packed.NumStandardMips && subresources[mip + 1].StartTileIndexInOverallResource <= r)
      mip++;
    auto &sub = subresources[mip];
    r -= sub.StartTileIndexInOverallResource;
    return {0, {r % sub.WidthInTiles, r / sub.WidthInTiles % sub.HeightInTiles, r / (sub.WidthInTiles * sub.HeightInTiles)},
            {1, 1, 1}, mip, slice};
  }
};

class MTLD3D12Heap : public ID3D12Heap {
public:
  WMT::Reference<WMT::Heap> heap;
};

class MTLD3D12Fence : public ID3D12Fence1 {
public:
  std::atomic<uint64_t> value;

  // a signal runs those that wait for its value or less, each once and before it returns. what waited is not asked
  // again when the value is taken back: a queue's Wait is over once the fence has had its value (vkd3d-proton's
  // test_cpu_signal_fence and test_fence_signal_availability have both from Windows)
  void
  Reach(uint64_t reached) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    value = reached;
    std::erase_if(waiting_, [&](auto &wait) { return wait.first <= reached && (wait.second(), true); });
  }

  void
  Expect(uint64_t expected, std::function<void()> &&reached) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    expected <= value ? reached() : (void)waiting_.emplace_back(expected, std::move(reached));
  }

  // a flag for a thread to wait on
  std::shared_ptr<std::atomic<bool>>
  Expect(uint64_t expected) {
    auto reached = std::make_shared<std::atomic<bool>>();
    Expect(expected, [reached] {
      *reached = true;
      reached->notify_all();
    });
    return reached;
  }

private:
  dxmt::mutex mutex_;
  std::vector<std::pair<uint64_t, std::function<void()>>> waiting_;
};

class MTLD3D12RootSignature : public ID3D12RootSignature {
public:
  virtual UINT GetBlob(const void **ppBlob) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;

  uint32_t UploadQwords;
  uint32_t ParameterSlots;
  uint32_t const *SlotQwordOffsets;
  // the root arguments holding the CBV/SRV/UAV and sampler heaps' GPU addresses, for directly indexed heaps
  // (D3D12_ROOT_SIGNATURE_FLAG_*_HEAP_DIRECTLY_INDEXED); ~0u without
  uint32_t ResourceHeapQword = ~0u;
  uint32_t SamplerHeapQword = ~0u;
  // the root argument holding the lock table of 64-bit atomics (MTLD3D12Device::GetAtomicLocks), in every one
  uint32_t AtomicLocksQword;
  // the most any root signature uploads: a qword per parameter (a DWORD of root cost at least), then the two heaps and
  // the lock table
  static constexpr uint32_t kMaxUploadQwords = D3D12_MAX_ROOT_COST + 3;

  size_t NumStaticSamplers;
  uint64_t const *EncodedStaticSamplers;
  // a local root signature's, in a buffer of its own: its shaders have no argument for them, and are compiled with
  // their GPU address
  uint64_t StaticSamplersAddress = 0;
};

class MTLD3D12StateObject : public ID3D12StateObject {
public:
  std::string name;
  // the pipeline that dispatches rays through the state object's shaders, its function table, and the ray flags
  // every TraceRay of it has
  virtual bool
  GetPipeline(WMT::ComputePipelineState &Pipeline, WMT::VisibleFunctionTable &Table, uint32_t &Flags) = 0;
};

class MTLD3D12CommandSignature : public ID3D12CommandSignature {
public:
  D3D12_INDIRECT_ARGUMENT_TYPE CommandType;
  UINT UpdateRootArguments : 1;
  UINT UpdateVertexBuffers : 1;
  UINT UpdateIndexBuffer   : 1;
  // what D3D12 resets after ExecuteIndirect: the root argument dwords the signature sets (constants become 0, root
  // views NULL), as (offset, count), and the vertex buffer slots it binds (NULL); the index buffer when it binds one
  std::vector<std::pair<uint32_t, uint32_t>> ResetRootDwords;
  uint32_t ResetVertexBuffers = 0;

  // both run as kernels: the one of draws in a compute pass before the render pass that runs its commands
  WMT::Reference<WMT::ComputePipelineState> render_resolver;
  WMT::Reference<WMT::ComputePipelineState> compute_resolver;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12QueryHeap : public ID3D12QueryHeap {
public:
  // occlusion results, one 64-bit value per query, or stream output statistics (two)
  WMT::Reference<WMT::Buffer> results;
  uint64_t results_address = 0;
  uint32_t result_size = sizeof(uint64_t);
  // a timestamp heap's queries: the clock each sampled, which the queue reads from its counter sample buffers when
  // the command buffer completes. Metal has few of those at once, and few samples in one
  std::vector<uint64_t> timestamps;
};

class MTLD3D12PipelineState : public ID3D12PipelineState {
public:
  UINT IsComputePipelineState;

  static HRESULT
  InitializeShader(D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl);

  // writes shaders that failed to compile to DXMT_SHADER_DUMP_PATH, when set, as <sha1>.dxbc for airconv's CLI
  static void DumpShaders(std::initializer_list<D3D12_SHADER_BYTECODE> Shaders);

  // the SHA-1 of each of its shaders, which is what logs call the pipeline. with DXMT_LOG_LEVEL=trace every shader
  // is also written as DumpShaders writes them, so that a pass a log names can be looked at
  std::string name;
  void Name(std::initializer_list<D3D12_SHADER_BYTECODE> Shaders);
};

class MTLD3D12GraphicsPipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::RenderPipelineState> pso;
  // the threads each patch takes in a tessellation pipeline's object stage; zero for ordinary pipelines
  uint32_t threads_per_patch = 0;
  // the object threadgroups that share a patch group's mesh threadgroups (tessellation_parts)
  uint32_t tessellation_parts = 1;
  // a geometry-shader pipeline runs its vertex shader in the object stage and its geometry shader in the mesh stage,
  // in `pso` for list topologies and in `pso_strip` for strips (none for points)
  bool geometry_shader = false;
  WMT::Reference<WMT::RenderPipelineState> pso_strip;
  // the vertex shader's output registers, which size a geometry pipeline's object threadgroups (get_gs_vertex_count)
  uint32_t vertex_registers = 0;
  // with stream output, the pipelines of its counting pass, as `pso` and `pso_strip`, and the geometry shader's
  // instances (airconv_public.h, SM50_STREAM_OUTPUT_TARGETS)
  bool stream_output = false;
  WMT::Reference<WMT::RenderPipelineState> so_count, so_count_strip;
  uint32_t gs_instances = 1;
  // stream output without a geometry shader streams from the object stage, and rasterizes with this ordinary pipeline
  WMT::Reference<WMT::RenderPipelineState> so_raster;
  // a pipeline without a geometry shader, whose topologies with adjacency run as geometry pipelines that pass the
  // primitives on: by strip, and for stream output's counting pass. null when it cannot be made
  bool adjacency = false;
  virtual WMT::RenderPipelineState AdjacencyPipeline(bool strip, bool counting) = 0;
  // a mesh shader pipeline: its amplification shader's threadgroup size (ones without one) and its mesh shader's
  bool mesh_shader = false;
  uint32_t object_threads[3] = {1, 1, 1}, mesh_threads[3] = {1, 1, 1};
  uint32_t slot_mask = 0;
  enum WMTTriangleFillMode fill_mode;
  enum WMTCullMode cull_mode;
  enum WMTDepthClipMode depth_clip_mode;
  enum WMTWinding winding;
  float depth_bias;
  float scole_scale;
  float depth_bias_clamp;
  uint32_t forced_sample_count;
  // D3D12_DEPTH_STENCIL_DESC1::DepthBoundsTestEnable, on a device that has the test
  bool depth_bounds = false;

  virtual WMT::DepthStencilState GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12ComputePipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::ComputePipelineState> pso;
  WMTSize threadgroup_size;
  // see MTL_SHADER_REFLECTION::GroupsWorkTogether
  bool groups_work_together = false;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

// timestamp queries sample the GPU's clock into counter sample buffers. Metal makes few of them, with few samples
// each, so they go round the device's command buffers, one each: a command buffer that needs one while the others hold
// them all waits for the first to complete
using TimestampSamples = diag::TimestampSamples;

class MTLD3D12Device : public ID3D12Device10 {
public:
  virtual WMT::Device GetMTLDevice() = 0;
  TimestampSamples timestamp_samples;

  // the most mesh threadgroups an object threadgroup may start, which a pipeline tells; 0 until one has
  std::atomic<uint32_t> max_mesh_threadgroups{0};
  // what the device's command allocators record into: commands, and what the GPU reads of them, through one buffer
  struct {
    RecordingArena commands, arguments;
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t address = 0;
    // blocks of `arguments` nothing writes: the zeros an indirect draw's indices past their view read
    std::optional<RecordingArena::Run> zeros;
    dxmt::mutex mutex;
  } recording;

  // the width of the GPU's SIMD groups, which run D3D12's waves
  virtual uint32_t GetSIMDWidth() = 0;
  // the GPU failed a command buffer: what it was to do is lost, and the device is removed, as after a timeout
  // detection and recovery on Windows (GetDeviceRemovedReason)
  virtual void LoseDevice() = 0;
  // how many threads of a compute shader go to the GPU at once when its threadgroups wait for one another: the
  // lanes it runs at once (the estimate behind TotalLaneCount)
  virtual uint64_t ThreadsAtOnce() = 0;
  // the Metal language version the device's shaders are converted for (ShaderMetalVersion)
  virtual SM50_SHADER_METAL_VERSION GetMetalVersion() = 0;

  // the GPU address of the lock words of 64-bit atomics, which every root signature carries (AtomicLocksQword)
  virtual uint64_t GetAtomicLocks() = 0;
  // reports, once per occurrence, a 64-bit atomic that gave up waiting for its lock (dxil_converter.cpp)
  virtual void CheckAtomicLocks() = 0;

  virtual D3D_FEATURE_LEVEL GetFeatureLevel() = 0;

  virtual HRESULT GetAdapter(REFIID riid, void **ppAdapter) = 0;

  virtual WMT::ResidencySet GetGlobalResidencySet() = 0;
  // DXMT_D3D12_GPU_ERRORS: passes carry the names of their pipelines to Metal, whose report of a command buffer
  // the GPU failed then says which pass it was. `NamePass` adds a pipeline to a pass's name, `PassName` takes it
  virtual bool NamesPasses() = 0;
  virtual void NamePass(const EncoderData *pass, const std::string &pipeline, obj_handle_t metal) = 0;
  virtual std::string PassName(const EncoderData *pass, bool consume = true, obj_handle_t metal = 0) = 0;
  // shared by every queue and swap chain: the first non-test Present accepted by a queue
  std::atomic<std::chrono::steady_clock::time_point> first_present{};
  virtual void PipelineMade(const std::string &name, const char *kind, std::chrono::steady_clock::time_point start) = 0;
  virtual void ReportPipelineTotals() = 0;

  virtual HRESULT RegisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT UnregisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT RegisterResidencyAndVA(BufferAllocation *allocation) = 0;

  virtual HRESULT UnregisterResidencyAndVA(BufferAllocation *allocation) = 0;

  virtual BufferAllocation *LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset) = 0;

  virtual InternalCommandLibrary& GetLib() = 0;

  // acceleration structures, by the GPU address of their memory (d3d12_acceleration_structure.hpp). one set at an
  // address replaces the one that was there; a buffer's go with it
  virtual std::shared_ptr<AccelerationStructure> LookupAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS VA) = 0;
  virtual void
  SetAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS VA, std::shared_ptr<AccelerationStructure> Structure) = 0;
  // gives a structure its compacted size's place, once
  virtual void AllocateCompactedSize(CompactedSize &Size) = 0;
  virtual void FreeCompactedSize(const CompactedSize &Size) = 0;

  // a ray tracing shader's slot in every pipeline's function table (airconv_ray.h), until it is freed
  virtual uint32_t AllocateRayFunction() = 0;
  virtual void FreeRayFunction(uint32_t Slot) = 0;

  // the placement sparse page size whose tiles are D3D12's, or 0 without placement sparse resources (tiled resources)
  virtual WMTSparsePageSize GetSparsePageSize() = 0;

  virtual FormatCapability GetMTLPixelFormatCapability(WMTPixelFormat Format) = 0;

  // sums occlusion queries' visibility counts into their heaps (sum_occlusion)
  WMT::Reference<WMT::ComputePipelineState> occlusion_sum;
  // applies a predication region to its draws' and dispatches' arguments (predicate)
  WMT::Reference<WMT::ComputePipelineState> predicate;
  // converts a top-level acceleration structure's instances to Metal's (acceleration_structure_instances)
  WMT::Reference<WMT::ComputePipelineState> acceleration_structure_instances;

  WMT::Reference<WMT::DepthStencilState> default_depth_stencil_state;

  // whether acceleration structures keep what they were built from, which serializing and decoding them gives back
  // (d3d12.keepAccelerationStructureInputs)
  bool keep_structure_inputs = true;
};

HRESULT CreateD3D12Device(IMTLDXGIAdapter *adapter, REFIID riid, void **ppDevice);

// `pParent` is the state object an addition grows from, or null
HRESULT CreateStateObject(
    MTLD3D12Device *pDevice, const D3D12_STATE_OBJECT_DESC *pDesc, ID3D12StateObject *pParent, REFIID riid,
    void **ppStateObject
);

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue);

HRESULT
CreateCommandAllocator(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator);

HRESULT
CreateDescriptorHeap(
    MTLD3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap
);

HRESULT CreateReservedTexture(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, REFIID riid,
    void **ppResource
);

HRESULT CreateReservedBuffer(MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, REFIID riid, void **ppResource);

HRESULT
CreateQueryHeap(MTLD3D12Device *pDevice, const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppQueryHeap);

HRESULT
CreateClosedCommandList(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandList);

struct D3D12SwapChainBufferRefs {
  IDXGISwapChain4 *swapchain;
  uint32_t referenced_buffers = 0;
  dxmt::mutex mutex;
};

HRESULT CreateCommittedTexture(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource, D3D12SwapChainBufferRefs *pBufferRefs = nullptr
);

HRESULT
CreatePlacedTexture(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT CreateCommittedBuffer(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
);

HRESULT
CreatePlacedBuffer(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap);

HRESULT
CreateRootSignature(
    MTLD3D12Device *pDevice, UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid,
    void **ppRootSignature
);

HRESULT
CreateCommandSignature(
    MTLD3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature,
    REFIID riid, void **ppCommandSignature
);

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState,
    bool depth_bounds = false, D3D12_SHADER_BYTECODE AS = {}, D3D12_SHADER_BYTECODE MS = {}
);

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
);

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain
);

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_STATIC_SAMPLER_DESC const &Desc);

// true when the sampler needs a custom border color (SM50_SAMPLER_BORDER)
bool PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_SAMPLER_DESC const &Desc);

inline std::tuple<MTLD3D12RenderTargetDescriptorHeap *, UINT>
GetRenderTargetHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12RenderTargetDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetRenderTargetDescriptor(MTLD3D12RenderTargetDescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline std::tuple<MTLD3D12DescriptorHeap *, UINT>
GetShaderVisibleDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12DescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetShaderVisibleDescriptor(MTLD3D12DescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
  //
}

inline std::tuple<MTLD3D12SamplerDescriptorHeap *, UINT>
GetSamplerDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12SamplerDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetSamplerDescriptor(MTLD3D12SamplerDescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

template <typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const D3D12_RESOURCE_DESC &ResourceDesc, VIEW_DESC *pViewDescOut);

constexpr auto kDefaultShader4Component = 0b1'011'010'001'000;

HRESULT ValidateResourceStates(D3D12_RESOURCE_STATES State, const D3D12_HEAP_PROPERTIES *pHeapProps);

HRESULT ValidateResourceDescs(const D3D12_RESOURCE_DESC *pDesc, const D3D12_HEAP_PROPERTIES *pHeapProps);

HRESULT ValidateHeapProperties(const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS Flags, bool AdapterIsNUMA);

D3D12_BOX GetResourceExtent(const D3D12_RESOURCE_DESC &Desc, UINT MipSlice);

UINT DecomposeSubresource(
    const D3D12_RESOURCE_DESC &Desc, UINT Subresource = 0, UINT *pMipSlice = NULL, UINT *pArraySlice = NULL,
    UINT *pPlaneSlice = NULL
);

bool IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES *pHeapProps);

// whether these bytes are a container (of a shader, a library or a root signature) that is what its hash says, or
// has none while experimental shader models are on (D3D12EnableExperimentalFeatures). Direct3D checks that before
// it reads anything else of one, and refuses what is not with E_INVALIDARG
bool ShaderContainerHolds(const void *container, size_t size);

bool IsD3D12BoxInBounds(D3D12_BOX &box, D3D12_BOX &bounds);

} // namespace dxmt
