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
#include "mullion_buffer.hpp"
#include "mullion_command.hpp"
#include "mullion_diag.hpp"
#include "mullion_fence.hpp"
#include "mullion_format.hpp"
#include "mullion_info.hpp"
#include "mullion_presenter.hpp"
#include "mullion_ring_bump_allocator.hpp"
#include "mullion_texture.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#define IMPLEMENT_ME                                                                                                   \
  do {                                                                                                                 \
    Logger::err(str::format(__FILE__, ":", __FUNCTION__, "(", __LINE__, ") is not implemented."));                     \
    abort();                                                                                                           \
    __builtin_unreachable();                                                                                           \
  } while (0);

// ID3D12DeviceConfiguration1 from DirectX-Headers, d3d12.h; the installed SDK declares only its base.
#ifndef __ID3D12DeviceConfiguration1_INTERFACE_DEFINED__
MIDL_INTERFACE("ed342442-6343-4e16-bb82-a3a577874e56")
ID3D12DeviceConfiguration1 : public ID3D12DeviceConfiguration {
  virtual HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *Library, SIZE_T Size, LPCWSTR Name, REFIID riid, void **Deserializer
  ) = 0;
};
__CRT_UUID_DECL(ID3D12DeviceConfiguration1, 0xed342442, 0x6343, 0x4e16, 0xbb, 0x82, 0xa3, 0xa5, 0x77, 0x87, 0x4e, 0x56)
#endif

#ifndef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
/** ID3D12GraphicsCommandList8 as the D3D12 headers declare it: ID3D12GraphicsCommandList7 plus per-face stencil references. */
MIDL_INTERFACE("ee936ef9-599d-4d28-938e-23c4ad05ce51")
ID3D12GraphicsCommandList8 : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51)
// DirectX-Headers, d3d12.idl; absent from the installed SDK together with interface 8
constexpr auto D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2 = (D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)26;
struct D3D12_DEPTH_STENCILOP_DESC1 {
  D3D12_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp;
  D3D12_COMPARISON_FUNC StencilFunc;
  UINT8 StencilReadMask, StencilWriteMask;
};
struct D3D12_DEPTH_STENCIL_DESC2 {
  BOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  BOOL StencilEnable;
  D3D12_DEPTH_STENCILOP_DESC1 FrontFace, BackFace;
  BOOL DepthBoundsTestEnable;
};
#endif

#ifndef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
/** ID3D12GraphicsCommandList9 as the D3D12 headers declare it: dynamic depth bias and index buffer strip cut. */
MIDL_INTERFACE("34ed2808-ffe6-4c2b-b11a-cabd2b0c59e1")
ID3D12GraphicsCommandList9 : public ID3D12GraphicsCommandList8 {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1)
#endif

namespace mullion {

bool ExperimentalShaderModelsEnabled();
HRESULT DeserializeLibraryRootSignature(const void *Library, SIZE_T Size, LPCWSTR Name, REFIID riid, void **Deserializer);

// a subobject a DXIL library defines, of the kinds a state object takes from one: their numbers are D3D12's
// subobject types
struct LibrarySubobject {
  D3D12_STATE_SUBOBJECT_TYPE type;
  std::wstring name;
  // a root signature, serialized
  std::span<const uint8_t> bytes;
  // an association's subobject, then its exports; a hit group's any hit, closest hit and intersection shaders
  std::vector<std::wstring> names;
  // a shader config's payload and attribute sizes, a pipeline config's depth and flags, or state object flags
  uint32_t values[2]{};
};
std::vector<LibrarySubobject> ReadSubobjects(const D3D12_SHADER_BYTECODE &Library);


// Metal's GPU timestamps count nanoseconds on Apple GPUs: sampleTimestamps advances them by 1e9 a second, as D3D11
// reports for its timestamp queries
constexpr uint64_t kGPUTimestampFrequency = 1'000'000'000;

class MTLD3D12CommandAllocator;
class MTLD3D12Device;

class MTLD3D12GraphicsCommandList : public ID3D12GraphicsCommandList9 {
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
  // `swapchain` owns the presenter and what it draws with: a present the queue holds back keeps it. `copies` take
  // `previous` onto every slice of `backbuffer` first; `view` of it is shown
  virtual HRESULT Present(
      IUnknown *swapchain, Presenter *presenter, ID3D12Resource *backbuffer, TextureViewKey view,
      HANDLE hLantecyWaitable, double after, uint64_t id, uint64_t number, ID3D12Resource *previous = nullptr,
      std::vector<PresentCopy> copies = {}
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

inline D3D12_RESOURCE_DESC
WithoutMipRegion(const D3D12_RESOURCE_DESC1 &desc1) {
  return {desc1.Dimension, desc1.Alignment, desc1.Width,      desc1.Height, desc1.DepthOrArraySize,
          desc1.MipLevels, desc1.Format,    desc1.SampleDesc, desc1.Layout, desc1.Flags};
}

class MTLD3D12Resource : public ID3D12Resource2 {
public:
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  Rc<Texture> texture;
  Rc<Buffer> buffer;
  // which of a reserved texture's tiles are mapped, as D3D12 numbers them (MTLD3D12CommandQueue::Settle)
  std::vector<bool> mapped_tiles;
  // a sampler feedback map's description as its application gave it: `texture` has one R32_UINT texel per mip region
  // (TranscodeFeedback in mullion_command_feedback.hpp)
  std::optional<D3D12_RESOURCE_DESC1> feedback;

  HRESULT STDMETHODCALLTYPE
  GetProtectedResourceSession(REFIID riid, void **ppProtectedSession) override {
    if (ppProtectedSession)
      *ppProtectedSession = nullptr;
    return E_NOINTERFACE;
  }

  D3D12_RESOURCE_DESC1 *STDMETHODCALLTYPE
  GetDesc1(D3D12_RESOURCE_DESC1 *__ret) override {
    D3D12_RESOURCE_DESC desc;
    GetDesc(&desc);
    *__ret = feedback.value_or(D3D12_RESOURCE_DESC1{
        desc.Dimension, desc.Alignment, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels,
        desc.Format, desc.SampleDesc, desc.Layout, desc.Flags, {}
    });
    return __ret;
  }

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

// GetResourceTiling of a resource that is not reserved, which has no tiles: no tiles, a zero packed mip description
// and tile shape, and no subresource tilings, as vkd3d-proton gives
inline void
NoTiling(UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
         UINT *SubresourceTilingCount) {
  if (TotalTileCount)
    *TotalTileCount = 0;
  if (PackedMipInfo)
    *PackedMipInfo = {};
  if (StandardTileShape)
    *StandardTileShape = {};
  if (SubresourceTilingCount)
    *SubresourceTilingCount = 0;
}

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
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  WMT::Reference<WMT::Heap> heap;
  std::shared_ptr<void> memory;
  // a heap of textures alone that the CPU never reaches is private, as its textures then are (Metal places only
  // resources of the heap's storage mode)
  bool private_storage = false;
};

// the event object an app's handle names, for a signal that may come after the app has closed its handle: Windows
// references the object, so a closed handle's value, which a new object may have taken, is never signaled. a handle
// of its own, closed with the last copy; null for a handle that names no object
inline std::shared_ptr<void>
EventObject(HANDLE event) {
  HANDLE own;
  if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return nullptr;
  return std::shared_ptr<void>(own, CloseHandle);
}

struct Reached {
  std::atomic<uint64_t> value;

  virtual ~Reached() = default;

  // a signal runs those that wait for its value or less, each once and before it returns. what waited is not asked
  // again when the value is taken back: a queue's Wait is over once the fence has had its value (vkd3d-proton's
  // test_cpu_signal_fence and test_fence_signal_availability have both from Windows)
  void
  Reach(uint64_t reached, bool terminal = false) {
    decltype(waiting_) done; // ends after the lock: what a waiter kept alive may come back in here as it goes
    std::lock_guard<mullion::mutex> lock(mutex_);
    if (terminal_)
      return;
    terminal_ = terminal;
    value = reached;
    std::erase_if(waiting_, [&](auto &wait) {
      return wait.first <= reached && (wait.second(), done.push_back(std::move(wait)), true);
    });
  }

  void
  Expect(uint64_t expected, std::function<void()> &&reached) {
    std::lock_guard<mullion::mutex> lock(mutex_);
    if (expected <= value || Has(expected))
      return reached();
    waiting_.emplace_back(expected, std::move(reached));
    Waiting(expected);
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

protected:
  // whether others have already given the fence a value that this object has not had yet
  virtual bool
  Has(uint64_t expected) {
    return false;
  }

  // a value that others can reach too is watched for once something waits for it
  virtual void
  Waiting(uint64_t expected) {}

private:
  mullion::mutex mutex_;
  bool terminal_ = false;
  std::vector<std::pair<uint64_t, std::function<void()>>> waiting_;
};

class MTLD3D12Fence : public ID3D12Fence1 {
public:
  std::shared_ptr<Reached> completion = std::make_shared<Reached>();
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  // the fence's value from now on, which a shared fence also gives the devices it is shared with
  virtual void Signaled(uint64_t value) = 0;
  // a shared fence's D3DKMT allocation, which a handle shares; 0 for a fence that is not shared
  virtual D3DKMT_HANDLE SharedAllocation() = 0;
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
  virtual WMT::ComputePipelineState StatisticsResolver() = 0;
  WMT::Reference<WMT::ComputePipelineState> compute_resolver;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12QueryHeap : public ID3D12QueryHeap {
public:
  D3D12_QUERY_HEAP_DESC desc;
  // occlusion results, one 64-bit value per query, stream output statistics (two) or pipeline statistics
  WMT::Reference<WMT::Buffer> results;
  uint64_t results_address = 0;
  uint32_t result_size = sizeof(uint64_t);
  // a timestamp heap's queries: the clock each sampled, which the queue reads from its counter sample buffers when
  // the command buffer completes. Metal has few of those at once, and few samples in one
  std::vector<uint64_t> timestamps;
};

using PipelineShaderCache = std::unordered_map<Sha1Digest, WMT::Reference<WMT::DispatchData>>;
struct PipelineStateStream;

class MTLD3D12PipelineState : public ID3D12PipelineState {
public:
  UINT IsComputePipelineState;
  Sha1Digest library_key;
  mullion::mutex shader_mutex;
  PipelineShaderCache cached_shaders;
  // made while no sampler had a border color Metal lacks (MTLD3D12Device::custom_borders), its shaders leave out the
  // emulation of one (SM50_SHADER_FLAG_NO_CUSTOM_BORDER): what it is made of, to make it again with the emulation
  // once a draw or dispatch can read such a sampler
  std::shared_ptr<const PipelineStateStream> borderless;
  Com<ID3D12PipelineState> bordered;
  ID3D12PipelineState *Bordered(MTLD3D12Device *Device);
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;

  static HRESULT
  InitializeShader(
      MTLD3D12Device *Device, D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl
  );

  // writes shaders that failed to compile to MULLION_SHADER_DUMP_PATH, when set, as <sha1>.dxbc for airconv's CLI
  static void DumpShaders(std::initializer_list<D3D12_SHADER_BYTECODE> Shaders);

  // the SHA-1 of each of its shaders, which is what logs call the pipeline. with MULLION_LOG_LEVEL=trace or
  // MULLION_SHADER_DUMP_ALL=1 every shader is also written as DumpShaders writes them, so that a pass a log or a
  // profile names can be looked at
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
  // a stage reads or writes a UAV: only then can a UAV barrier order anything a draw of it does inside a render pass
  bool uses_uav = false;
  // its pixel shader blends a target itself, reading the blend factor after the depth ranges (SM50_SHADER_BLEND)
  bool shader_blend = false;
  WMT::Reference<WMT::RenderPipelineState> pso_strip;
  // the vertex shader's output registers, which size a geometry pipeline's object threadgroups (get_gs_vertex_count)
  uint32_t vertex_registers = 0;
  // with stream output, the pipelines of its counting pass, as `pso` and `pso_strip` (airconv_public.h,
  // SM50_STREAM_OUTPUT_TARGETS)
  bool stream_output = false;
  WMT::Reference<WMT::RenderPipelineState> so_count, so_count_strip;
  // the geometry shader's instances, 1 without one
  uint32_t gs_instances = 1;
  // the mesh threadgroups an object threadgroup starts for each primitive: the geometry shader's instances, 1 when
  // the mesh stage only passes primitives on, 0 when it starts none (stream output alone)
  uint32_t mesh_instances = 0;
  // a pipeline without a geometry shader, whose topologies with adjacency run as geometry pipelines that pass the
  // primitives on; and one of stream output alone of patches, whose control points only the topology tells. their
  // Metal pipelines are made by topology, and for stream output's counting pass. null when it cannot be made
  bool adjacency = false, patches = false;
  virtual WMT::RenderPipelineState TopologyPipeline(D3D12_PRIMITIVE_TOPOLOGY topology, bool counting) = 0;
  // a mesh shader pipeline: its amplification shader's threadgroup size (ones without one) and its mesh shader's
  bool mesh_shader = false;
  uint32_t object_threads[3] = {1, 1, 1}, mesh_threads[3] = {1, 1, 1};
  // whether it has a pixel shader, which PSInvocations count
  bool pixel_shader = false;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  uint32_t slot_mask = 0;
  uint32_t rtv_mask = 0, dsv_mask = 0;
  // a pipeline whose DSVFormat is unknown but that tests depth or stencil tests the view bound, whatever its format
  // (dsv_mask has both planes): its variants take the format (RenderTargets)
  bool dsv_any_format = false;
  enum WMTTriangleFillMode fill_mode;
  enum WMTCullMode cull_mode;
  enum WMTDepthClipMode depth_clip_mode;
  enum WMTWinding winding;
  float depth_bias;
  float scole_scale;
  float depth_bias_clamp;
  uint32_t forced_sample_count;
  // D3D12_DEPTH_STENCIL_DESC1::DepthBoundsTestEnable, on a device that has the test, and on one that has not, where the
  // pixel shader tests the samples a kernel finds in the bounds before the draw (SM50_SHADER_PSO_PIXEL_SHADER_DATA's
  // depth_bounds); and whether the pipeline writes depth, which changes what passes
  bool depth_bounds = false;
  bool depth_bounds_emulated = false;
  bool writes_depth = false;
  // conservative rasterization of triangles: the mesh stage reads the viewports (SM50_BINDING_INDEX_VIEWPORTS)
  bool conservative = false;

  // whether its pixel shader has a variant of coarse pixels (MTL_PIXEL_SHADER_REFLECTION's FineShading)
  bool coarse_shading = false;

  virtual WMT::RenderPipelineState RenderTargets(
      WMT::RenderPipelineState Pipeline, uint32_t Mask, uint32_t DSVPlanar, bool Statistics = false,
      bool Occluded = false, bool Coarse = false, WMTPixelFormat DSVFormat = WMTPixelFormatInvalid
  ) = 0;
  virtual WMT::DepthStencilState GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) = 0;
};

class MTLD3D12ComputePipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::ComputePipelineState> pso;
  WMTSize threadgroup_size;
  // see MTL_SHADER_REFLECTION::GroupsWorkTogether and DeviceAtomics64
  bool groups_work_together = false;
};

// timestamp queries sample the GPU's clock into counter sample buffers. Metal makes few of them, with few samples
// each, so they go round the device's command buffers, one each: a command buffer that needs one while the others hold
// them all waits for the first to complete
using TimestampSamples = diag::TimestampSamples;

class MTLD3D12Device : public ID3D12Device10 {
public:
  virtual WMT::Device GetMTLDevice() = 0;
  TimestampSamples timestamp_samples;
  struct {
    mullion::mutex mutex;
    std::unordered_set<std::shared_ptr<Reached>> values;
  } fences;

  // a sampler has a border color Metal lacks (PopulateWMTSamplerInfo), so pipelines made from now on emulate it
  // (MTLD3D12PipelineState::borderless)
  std::atomic<bool> custom_borders{false};

  // ExecuteIndirect's indirect command buffers that no allocator holds: what an allocator's last use left unused, and
  // what an allocator held when it was destroyed. an allocator takes from its own pool first, then from this one, so
  // that a title whose threads take any free allocator each frame still finds them (dttdrv/mullion#32: Starfield makes
  // about 450 a frame from many allocators). at most kCommands commands in all, the oldest given back to Metal first:
  // keeping every one ran Starfield out of memory
  struct IndirectCommandBuffers {
    struct Pooled {
      WMT::Reference<WMT::IndirectCommandBuffer> icb;
      uint64_t gpu_resource_id;
      uint64_t key;
      // what it was made with: the kind (with no padding and no resource ID), its size and its storage
      WMTIndirectCommandBufferInfo kind;
      uint64_t capacity;
      WMTResourceOptions options;
      bool
      Is(uint64_t key_, const WMTIndirectCommandBufferInfo &kind_, uint64_t capacity_, WMTResourceOptions options_) const {
        return key == key_ && capacity == capacity_ && options == options_ && !memcmp(&kind, &kind_, sizeof(kind));
      }
    };
    static constexpr uint64_t kCommands = 1 << 18;

    bool
    Take(uint64_t key, const WMTIndirectCommandBufferInfo &kind, uint64_t capacity, WMTResourceOptions options,
         Pooled &out) {
      std::lock_guard lock(mutex);
      for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->Is(key, kind, capacity, options)) {
          out = std::move(*it);
          commands -= out.capacity;
          entries.erase(it);
          return true;
        }
      return false;
    }
    void
    Give(Pooled &&pooled) {
      if (!pooled.icb || pooled.capacity > kCommands)
        return;
      std::lock_guard lock(mutex);
      commands += pooled.capacity;
      entries.push_back(std::move(pooled));
      while (commands > kCommands) {
        commands -= entries.front().capacity;
        entries.pop_front();
      }
    }

  private:
    mullion::mutex mutex;
    // oldest first
    std::deque<Pooled> entries;
    uint64_t commands = 0;
  } indirect_command_buffers;

  // the most mesh threadgroups an object threadgroup may start, which a pipeline tells; 0 until one has
  std::atomic<uint32_t> max_mesh_threadgroups{0};
  // the mesh memory a draw's mesh threadgroups share (MeshBytesPerDraw)
  uint64_t mesh_bytes_per_draw = -1ull;
  // what the device's command allocators record into: commands, and what the GPU reads of them, through one buffer
  struct {
    RecordingArena commands, arguments;
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t address = 0;
    // blocks of `arguments` nothing writes: the zeros an indirect draw's indices past their view read
    std::optional<RecordingArena::Run> zeros;
    mullion::mutex mutex;
  } recording;

  // the width of the GPU's SIMD groups, which run D3D12's waves
  virtual uint32_t GetSIMDWidth() = 0;
  // later GPU errors must not replace the original removal reason (ID3D12Device::GetDeviceRemovedReason)
  virtual void LoseDevice(HRESULT Reason = DXGI_ERROR_DEVICE_HUNG) = 0;
  // the removal reason as it is, for Mullion's own checks. GetDeviceRemovedReason first reads what Metal said of the
  // command buffers whose work has finished (SettleCommandQueueFeedback): their fences are signalled before that comes
  virtual HRESULT RemovedReason() = 0;
  // how many threads of a compute shader go to the GPU at once when its threadgroups wait for one another: the
  // lanes it runs at once (the estimate behind TotalLaneCount)
  virtual uint64_t ThreadsAtOnce() = 0;
  // the Metal language version the device's shaders are converted for (ShaderMetalVersion)
  virtual SM50_SHADER_METAL_VERSION GetMetalVersion() = 0;
  // the shader compiler's flags for what this GPU lacks (GPUShaderFlags)
  SM50_SHADER_FLAG
  GetShaderFlags() {
    return GPUShaderFlags(GetMTLDevice());
  }

  // the GPU address of the lock words of 64-bit atomics, which every root signature carries (AtomicLocksQword)
  virtual uint64_t GetAtomicLocks() = 0;
  // the GPU address of zeros as many as a constant buffer holds, where a null buffer view points: a shader's load
  // past a view's end reads the view's start and drops what it read (dxbc_converter_base.cpp LoadBufferValue), and
  // an Apple GPU may fault on memory that is not there
  virtual uint64_t GetNullBuffer() = 0;
  // the buffer of those zeros
  virtual obj_handle_t GetNullBufferHandle() = 0;
  // removes the device if a 64-bit atomic gave up waiting for its lock (dxil_converter.cpp)
  virtual void CheckAtomicLocks() = 0;

  virtual D3D_FEATURE_LEVEL GetFeatureLevel() = 0;

  virtual HRESULT GetAdapter(REFIID riid, void **ppAdapter) = 0;

  virtual WMT::ResidencySet GetGlobalResidencySet() = 0;
  // what shared fences need, made with the first: a D3DKMT device, and a listener for their events
  virtual D3DKMT_HANDLE GetLocalD3DKMT() = 0;
  virtual EventListener &GetEventListener() = 0;
  // commits the global residency set's changes; queues call it before committing a command buffer
  virtual void CommitResidency() = 0;
  // MULLION_D3D12_GPU_ERRORS: passes carry the names of their pipelines to Metal, whose report of a command buffer
  // the GPU failed then says which pass it was. `NamePass` adds a pipeline to a pass's name, `PassName` takes it
  virtual bool NamesPasses() = 0;
  virtual void NamePass(const EncoderData *pass, const std::string &pipeline, obj_handle_t metal) = 0;
  virtual std::string PassName(const EncoderData *pass, bool consume = true, obj_handle_t metal = 0) = 0;
  // shared by every queue and swap chain: the first non-test Present accepted by a queue
  std::atomic<std::chrono::steady_clock::time_point> first_present{};
  virtual void PipelineMade(const std::string &name, const char *kind, std::chrono::steady_clock::time_point start) = 0;
  virtual void ReportPipelineTotals() = 0;

  bool
  UsesMetal4() {
    static const bool enabled = env::getEnvVar("MULLION_D3D12_METAL4") != "0";
    return enabled && GetMTLDevice().supportsFamily(WMTGPUFamilyMetal4);
  }

  virtual HRESULT RegisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT UnregisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT RegisterResidencyAndVA(BufferAllocation *allocation) = 0;

  virtual HRESULT UnregisterResidencyAndVA(BufferAllocation *allocation) = 0;

  // pEnd, when given, is where the addresses that find the same buffer end: its end, or the next buffer's start
  virtual BufferAllocation *
  LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset, uint64_t *pEnd = nullptr) = 0;

  virtual void UpdateSparseBufferMappings(
      BufferAllocation *buffer, const std::vector<WMTSparseBufferMapping> &ranges
  ) = 0;
  virtual void CopySparseBufferMappings(
      BufferAllocation *src, BufferAllocation *dst, const std::vector<WMTSparseBufferMappingCopy> &ranges
  ) = 0;
  virtual std::vector<uint64_t> SparseBufferMappings() = 0;
  // whether a reserved buffer exists, which SparseBufferMappings has an entry for whatever it maps
  virtual bool HasSparseBuffers() = 0;

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
  // writes pipeline statistics queries' results into their heaps (sum_pipeline_statistics)
  WMT::Reference<WMT::ComputePipelineState> pipeline_statistics_sum;
  // applies a predication region to its draws' and dispatches' arguments (predicate)
  WMT::Reference<WMT::ComputePipelineState> predicate;
  // converts a top-level acceleration structure's instances to Metal's (acceleration_structure_instances)
  WMT::Reference<WMT::ComputePipelineState> acceleration_structure_instances;
  // the samples of a depth attachment in the depth bounds, for a GPU without the test (depth_bounds_samples_*): of a
  // 2D, 2D array, multisampled and multisampled array view
  WMT::Reference<WMT::ComputePipelineState> depth_bounds_samples[4];

  WMT::Reference<WMT::DepthStencilState> default_depth_stencil_state;

  // whether acceleration structures keep what they were built from, which serializing and decoding them gives back
  // (d3d12.keepAccelerationStructureInputs)
  bool keep_structure_inputs = true;

  // the scratch area each list's indirect stream output commands share, from D3D11's start (kStagingBlockSize): a
  // command that lacked room was not run, and grows it for lists recorded later
  std::atomic<uint64_t> so_scratch_budget = kStagingBlockSize;
};

HRESULT CreateD3D12Device(IMTLDXGIAdapter *adapter, REFIID riid, void **ppDevice);

// `pParent` is the state object an addition grows from, or null
HRESULT CreateStateObject(
    MTLD3D12Device *pDevice, const D3D12_STATE_OBJECT_DESC *pDesc, ID3D12StateObject *pParent, REFIID riid,
    void **ppStateObject
);

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue);
// reads the feedback of the device's command buffers whose work has finished: an error in one removes the device
void SettleCommandQueueFeedback(MTLD3D12Device *pDevice);

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
  mullion::mutex mutex;
};

// pTexture: the texture it is, which another module made
HRESULT CreateCommittedTexture(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource, D3D12SwapChainBufferRefs *pBufferRefs = nullptr,
    const MullionMetalResource *pTexture = nullptr
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
OpenHeap(MTLD3D12Device *pDevice, const void *Address, HANDLE Mapping, REFIID riid, void **ppHeap);

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
    bool depth_bounds = false, D3D12_SHADER_BYTECODE AS = {}, D3D12_SHADER_BYTECODE MS = {},
    const PipelineShaderCache *cached = nullptr, const D3D12_DEPTH_STENCIL_DESC2 *depth_stencil2 = nullptr
);

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState,
    const PipelineShaderCache *cached = nullptr
);

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain
);

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence);

// the fence a handle shares (ID3D12Device::OpenSharedHandle), whose event and D3DKMT allocation openSharedEvent gave
HRESULT OpenFence(
    MTLD3D12Device *pDevice, WMT::Reference<WMT::SharedEvent> &&Event, D3DKMT_HANDLE Allocation, REFIID riid,
    void **ppFence
);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_STATIC_SAMPLER_DESC const &Desc);

// true when the sampler needs a custom border color (SM50_SAMPLER_BORDER)
bool PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_SAMPLER_DESC const &Desc);

inline std::tuple<MTLD3D12RenderTargetDescriptorHeap *, UINT>
GetRenderTargetHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef MULLION_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12RenderTargetDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetRenderTargetDescriptor(MTLD3D12RenderTargetDescriptorHeap *pHeap, UINT Index) {
#ifdef MULLION_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline std::tuple<MTLD3D12DescriptorHeap *, UINT>
GetShaderVisibleDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef MULLION_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12DescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetShaderVisibleDescriptor(MTLD3D12DescriptorHeap *pHeap, UINT Index) {
#ifdef MULLION_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
  //
}

inline std::tuple<MTLD3D12SamplerDescriptorHeap *, UINT>
GetSamplerDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef MULLION_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12SamplerDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetSamplerDescriptor(MTLD3D12SamplerDescriptorHeap *pHeap, UINT Index) {
#ifdef MULLION_USE_EMBEDDED_HEAP_POINTER
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

namespace sdk {
// GPU upload heaps (DirectX-Specs, D3D12 GPU Upload Heaps), which the toolchain's d3d12.h lacks
constexpr auto D3D12_HEAP_TYPE_GPU_UPLOAD = D3D12_HEAP_TYPE(5);
// render passes' local accesses (DirectX-Specs, Render Passes), which the toolchain's d3d12.h lacks too, past the
// range of its D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE
constexpr UINT D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_SRV = 5;
constexpr UINT D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_UAV = 6;
} // namespace sdk

bool IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES *pHeapProps);

// whether these bytes are a container (of a shader, a library or a root signature) that is what its hash says, or
// has none while experimental shader models are on (D3D12EnableExperimentalFeatures). Direct3D checks that before
// it reads anything else of one, and refuses what is not with E_INVALIDARG
bool ShaderContainerHolds(const void *container, size_t size);

// whether `device` has the features a shader container requires: the low bits of its feature info part (SFI0,
// DxilContainer.h), which DxilConstants.h calls feature requirements
bool ShaderFeaturesSupported(MTLD3D12Device *device, const void *container, size_t size);

bool IsD3D12BoxInBounds(D3D12_BOX &box, D3D12_BOX &bounds);

} // namespace mullion
