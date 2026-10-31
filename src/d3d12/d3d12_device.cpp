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

#include "d3d12_acceleration_structure.hpp"
#include "mullion_diag.hpp"
#include "airconv_ray.h"
#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_pipeline.hpp"
#include "Metal.hpp"
#include "com/com_pointer.hpp"
#include "com/com_object.hpp"
#include "dxgi_interfaces.h"
#include "mullion_format.hpp"
#include "util_env.hpp"
#include "mullion_info.hpp"
#include "log/log.hpp"
#include "config/config.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <iomanip>
#include <limits>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace mullion {

// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// declarations missing from the toolchain, from DirectX-Headers/include/directx/d3d12.idl
namespace sdk {
constexpr auto D3D_ROOT_SIGNATURE_VERSION_1_2 = (D3D_ROOT_SIGNATURE_VERSION)3;
constexpr auto D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE = (D3D12_BARRIER_LAYOUT)31;
constexpr UINT D3D12_FEATURE_D3D12_OPTIONS19 = 48;
constexpr UINT D3D12_FEATURE_D3D12_OPTIONS20 = 49;
constexpr UINT D3D12_FEATURE_PREDICATION = 50;
constexpr UINT D3D12_FEATURE_HARDWARE_COPY = 52;
constexpr UINT D3D12_FEATURE_D3D12_OPTIONS21 = 53;
constexpr UINT D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT = 54;
constexpr UINT D3D12_FEATURE_APPLICATION_SPECIFIC_DRIVER_STATE = 56;
constexpr UINT D3D12_FEATURE_BYTECODE_BYPASS_HASH_SUPPORTED = 57;
constexpr UINT D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT = 61;
constexpr UINT D3D12_FEATURE_BARRIER_LAYOUT = 64;
constexpr UINT D3D12_FEATURE_D3D12_OPTIONS22 = 65;

struct D3D12_FEATURE_DATA_D3D12_OPTIONS19 {
  BOOL MismatchingOutputDimensionsSupported;
  UINT SupportedSampleCountsWithNoOutputs;
  BOOL PointSamplingAddressesNeverRoundUp;
  BOOL RasterizerDesc2Supported;
  BOOL NarrowQuadrilateralLinesSupported;
  BOOL AnisoFilterWithPointMipSupported;
  UINT MaxSamplerDescriptorHeapSize;
  UINT MaxSamplerDescriptorHeapSizeWithStaticSamplers;
  UINT MaxViewDescriptorHeapSize;
  BOOL ComputeOnlyCustomHeapSupported;
};
struct D3D12_FEATURE_DATA_D3D12_OPTIONS20 {
  BOOL ComputeOnlyWriteWatchSupported;
  UINT RecreateAtTier; // D3D12_RECREATE_AT_TIER
};
struct D3D12_FEATURE_DATA_D3D12_OPTIONS21 {
  UINT WorkGraphsTier;      // D3D12_WORK_GRAPHS_TIER
  UINT ExecuteIndirectTier; // D3D12_EXECUTE_INDIRECT_TIER
  BOOL SampleCmpGradientAndBiasSupported;
  BOOL ExtendedCommandInfoSupported;
};
constexpr UINT D3D12_EXECUTE_INDIRECT_TIER_1_0 = 10;

struct D3D12_FEATURE_DATA_APPLICATION_SPECIFIC_DRIVER_STATE { BOOL Supported; };
struct D3D12_FEATURE_DATA_BYTECODE_BYPASS_HASH_SUPPORTED { BOOL Supported; };
struct D3D12_FEATURE_DATA_D3D12_OPTIONS22 {
  BOOL ShaderExecutionReorderingActuallyReorders;
  BOOL CreateByteOffsetViewsSupported;
  UINT Max1DDispatchSize;
  UINT Max1DDispatchMeshSize;
};
union D3D12_VERSION_NUMBER {
  UINT64 Version;
  UINT16 VersionParts[4];
};
struct D3D12_FEATURE_DATA_SHADERCACHE_ABI_SUPPORT {
  WCHAR szAdapterFamily[128];
  UINT64 MinimumABISupportVersion;
  UINT64 MaximumABISupportVersion;
  D3D12_VERSION_NUMBER CompilerVersion;
  D3D12_VERSION_NUMBER ApplicationProfileVersion;
};
struct D3D12_FEATURE_DATA_BARRIER_LAYOUT {
  D3D12_COMMAND_LIST_TYPE CommandListType;
  D3D12_BARRIER_LAYOUT Layout;
  BOOL Supported;
};
} // namespace sdk

const GUID kD3D12DeviceDownlevelUUID = {0x74eaee3f, 0x2f4b, 0x476d, {0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10}};

HRESULT PopulateWMTTextureInfo(
    MTLD3D12Device *Device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc, UINT Plane = ~0u
);

template <typename T>
static HRESULT
NotSupported(void *pFeatureData, UINT DataSize) {
  if (DataSize != sizeof(T))
    return FAIL(E_INVALIDARG);
  *reinterpret_cast<T *>(pFeatureData) = {};
  return S_OK;
}

/* the same for a query whose first member is the node asked about */
template <typename T>
static HRESULT
NotSupportedOnNode(void *pFeatureData, UINT DataSize) {
  if (DataSize != sizeof(T) || reinterpret_cast<T *>(pFeatureData)->NodeIndex)
    return FAIL(E_INVALIDARG);
  *reinterpret_cast<T *>(pFeatureData) = {};
  return S_OK;
}

// what D3D11On12 needs of the device (IMTLDXGIDevice's IMTLD3D11On12Source), as a part of the device: its own
// reference count is the device's. D3D12 fences are reached on the CPU, after the GPU's work; a D3D11 context waits
// for and signals Metal shared events, so the two meet in callbacks that move one to the other
class D3D11On12Source final : public IMTLD3D11On12Source {
public:
  explicit D3D11On12Source(MTLD3D12Device *device) : device_(device) {}

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    return device_->QueryInterface(riid, ppvObject);
  }
  ULONG STDMETHODCALLTYPE
  AddRef() final {
    return device_->AddRef();
  }
  ULONG STDMETHODCALLTYPE
  Release() final {
    return device_->Release();
  }

  WMT::Device STDMETHODCALLTYPE
  GetMTLDevice() final {
    return device_->GetMTLDevice();
  }
  HRESULT STDMETHODCALLTYPE
  GetAdapter(REFIID riid, void **ppAdapter) final {
    return device_->GetAdapter(riid, ppAdapter);
  }

  HRESULT STDMETHODCALLTYPE
  GetResourceObjects(ID3D12Resource *pResource, MullionMetalResource *pInfo) final {
    *pInfo = {};
    if (!pResource || !Owns(pResource))
      return E_INVALIDARG;
    auto resource = static_cast<MTLD3D12Resource *>(pResource);
    if (resource->feedback || !resource->mapped_tiles.empty())
      return E_INVALIDARG;
    if (resource->texture && resource->texture->current()) {
      auto allocation = resource->texture->current();
      pInfo->texture = allocation->texture().handle;
      pInfo->texture_info = resource->texture->info();
      pInfo->texture_info.gpu_resource_id = allocation->gpuResourceID;
      pInfo->flags = allocation->flags().raw();
      NSObject_retain(pInfo->texture);
      return S_OK;
    }
    if (resource->buffer && resource->buffer->current()) {
      auto allocation = resource->buffer->current();
      pInfo->buffer = allocation->buffer().handle;
      pInfo->buffer_info = allocation->info();
      pInfo->flags = allocation->flags().raw();
      NSObject_retain(pInfo->buffer);
      return S_OK;
    }
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateResourceOfMetal(const D3D12_RESOURCE_DESC *desc, const MullionMetalResource *info,
                        REFIID riid, void **resource) final {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    return CreateCommittedTexture(device_, &heap, D3D12_HEAP_FLAG_NONE, desc, D3D12_RESOURCE_STATE_COMMON,
                                  nullptr, riid, resource, nullptr, info);
  }

  HRESULT STDMETHODCALLTYPE
  SignalEventAtFence(ID3D12Fence *pFence, UINT64 FenceValue, WMT::SharedEvent Event, UINT64 Value) final {
    if (!pFence || !Owns(pFence))
      return E_INVALIDARG;
    static_cast<MTLD3D12Fence *>(pFence)->completion->Expect(FenceValue, [event = WMT::Reference<WMT::SharedEvent>(Event), Value]() mutable {
      event.signalValue(Value);
    });
    return S_OK;
  }

private:
  // whether an object is a child of this device, and so one of Mullion's
  bool
  Owns(ID3D12DeviceChild *child) {
    Com<ID3D12Device> parent;
    return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&parent))) && parent.ptr() == static_cast<ID3D12Device *>(device_);
  }

  MTLD3D12Device *device_;
};

// MULLION_TRACE_RESOURCE: a line for a resource made, with what it is, how and where it was made and what came of it;
// 1 has the reserved resources and the failures, heavy every one (MULLION_TRACE_MEMORY has the heaps)
static HRESULT
TraceResource(HRESULT hr, const char *how, const D3D12_RESOURCE_DESC *desc, void **resource, const std::string &where) {
  if (!trace::On(trace::Resource) || !desc ||
      (SUCCEEDED(hr) && strcmp(how, "reserved") && !trace::Heavy(trace::Resource)))
    return hr;
  TRACE_IN(Resource, how, desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? " buffer " : " texture ",
           desc->Width, "x", desc->Height, "x", desc->DepthOrArraySize, ", dimension ", uint32_t(desc->Dimension),
           ", format ", uint32_t(desc->Format), ", mips ", desc->MipLevels, ", samples ", desc->SampleDesc.Count,
           ", layout ", uint32_t(desc->Layout), ", flags ", uint32_t(desc->Flags), where, ": ",
           SUCCEEDED(hr) && resource ? *resource : nullptr, SUCCEEDED(hr) ? "" : str::format(", failed ", uint32_t(hr)));
  return hr;
}

class MTLD3D12DeviceImpl : public MTLD3D12Object<ComObject<MTLD3D12Device, ID3DDestructionNotifier, ID3D12DeviceConfiguration1>> {
  bool experimental_shader_models_ = ExperimentalShaderModelsEnabled();
  uint32_t simd_width_ = 0, total_lanes_ = 0;
  WMT::Reference<WMT::Buffer> atomic_locks_;
  uint64_t atomic_locks_address_ = 0;
  uint32_t *atomic_locks_given_up_ = nullptr;
  WMT::Reference<WMT::Buffer> null_buffer_;
  uint64_t null_buffer_address_ = 0;
  WMTSparsePageSize sparse_page_size_ = {};

  Com<IMTLDXGIAdapter> adapter_;
  D3D11On12Source on12_source_{this};

  bool advertise_numa_ = false;

  mullion::mutex residency_lock_;
  std::atomic<HRESULT> removed_ = S_OK;
  mullion::mutex pass_names_lock_;
  std::unordered_map<const EncoderData *, std::string> pass_names_;
  std::unordered_map<const EncoderData *, std::unordered_map<obj_handle_t, std::string>> pass_pipelines_;
  uint64_t late_pipelines_ = 0;
  double pipeline_ms_ = 0, longest_pipeline_ms_ = 0;
  mullion::mutex acceleration_structure_lock_;
  std::map<uint64_t, std::shared_ptr<AccelerationStructure>> acceleration_structures_;
  // compacted sizes are uint64s of shared buffers the GPU writes
  std::vector<WMT::Reference<WMT::Buffer>> compacted_size_buffers_;
  std::vector<CompactedSize> free_compacted_sizes_;
  // ray tracing shaders' table slots: the ones freed, and the next new one
  std::vector<uint32_t> free_ray_functions_;
  uint32_t next_ray_function_ = SM50_RAY_FUNCTION_FIRST_SHADER;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  // allocations were added or removed since the set was last committed (CommitResidency)
  bool residency_dirty_ = false;
  // the buffers by GPU address and the sparse ones' mapped tiles, under a lock of their own: every index buffer, root
  // view and immediate write looks an address up, and none of them should wait on residency_lock_, which a queue holds
  // across the residency set's commit (a kernel call)
  mullion::shared_mutex va_lock_;
  std::map<uint64_t, BufferAllocation *> interval_map_;
  std::map<uint64_t, std::vector<uint64_t>> sparse_buffer_mappings_;
  // what a null acceleration structure is to a ray query: a top-level structure of nothing
  std::unique_ptr<AccelerationStructure> null_scene_;

  InternalCommandLibrary command_library;
  FormatCapabilityInspector format_inspector_;
  std::once_flag sharing_;
  D3DKMT_HANDLE local_kmt_ = 0;
  std::unique_ptr<EventListener> event_listener_;

  void
  Share() {
    std::call_once(sharing_, [this] {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (create.hAdapter && !D3DKMTCreateDevice(&create))
        local_kmt_ = create.hDevice;
      else
        WARN("Failed to create D3DKMT device");
      event_listener_ = std::make_unique<EventListener>();
    });
  }

public:
  MTLD3D12DeviceImpl(IMTLDXGIAdapter *adapter) : adapter_(adapter), command_library(adapter_->GetMTLDevice()) {
    mesh_bytes_per_draw = MeshBytesPerDraw(adapter_->GetMTLDevice());
  }

  ~MTLD3D12DeviceImpl() {
    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  D3DKMT_HANDLE
  GetLocalD3DKMT() override {
    Share();
    return local_kmt_;
  }

  EventListener &
  GetEventListener() override {
    Share();
    return *event_listener_;
  }

  HRESULT
  Initialize() {
    WMT::Reference<WMT::Error> err;
    residency_set_ = adapter_->GetMTLDevice().newResidencySet(0, err);
    if (!residency_set_) {
      ERR("Failed to create MTLResidencySet: ", err.description().getUTF8String());
      return E_FAIL;
    }
    format_inspector_.Inspect(GetMTLDevice());
    keep_structure_inputs = Config::getInstance().getOption<bool>("d3d12.keepAccelerationStructureInputs", true);
    
    WMTDepthStencilInfo info{};
    info.depth_compare_function = WMTCompareFunctionAlways;
    default_depth_stencil_state = GetMTLDevice().newDepthStencilState(info);

    occlusion_sum = GetMTLDevice().newComputePipelineState(command_library.getLibrary().newFunction("sum_occlusion"), err);
    for (uint32_t i = 0; i < 4; i++) {
      const char *names[] = {"depth_bounds_samples_2d", "depth_bounds_samples_2d_array", "depth_bounds_samples_2d_ms",
                             "depth_bounds_samples_2d_ms_array"};
      depth_bounds_samples[i] =
          GetMTLDevice().newComputePipelineState(command_library.getLibrary().newFunction(names[i]), err);
    }
    pipeline_statistics_sum = GetMTLDevice().newComputePipelineState(
        command_library.getLibrary().newFunction("sum_pipeline_statistics"), err
    );
    predicate = GetMTLDevice().newComputePipelineState(command_library.getLibrary().newFunction("predicate"), err);
    acceleration_structure_instances = GetMTLDevice().newComputePipelineState(
        command_library.getLibrary().newFunction("acceleration_structure_instances"), err
    );
    if (!occlusion_sum || !pipeline_statistics_sum || !predicate || !acceleration_structure_instances ||
        !depth_bounds_samples[0] || !depth_bounds_samples[1] || !depth_bounds_samples[2] || !depth_bounds_samples[3]) {
      ERR("Failed to create the query pipelines: ", err.description().getUTF8String());
      return E_FAIL;
    }
    // a compute pipeline's thread execution width is the SIMD-group width
    simd_width_ = occlusion_sum.threadExecutionWidth();
    if (GetMTLDevice().supportsPlacementSparse())
      for (auto page : {WMTSparsePageSize16, WMTSparsePageSize64, WMTSparsePageSize256})
        if (GetMTLDevice().sparseTileSizeInBytes(page) == D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)
          sparse_page_size_ = page;
    // each GPU core runs four SIMD groups at once; Metal reports neither, so it is an estimate, as on other drivers
    total_lanes_ = GetMTLDevice().gpuCoreCount() * 4 * simd_width_;
    // the lock words of 64-bit atomics (dxil_converter.cpp): as many as lanes run at once, a power of two, whose mask
    // is word 0, and after them the flag of an atomic that gave up waiting
    // before them, where every shader finds it, the header of the structure of nothing
    // (SM50_NULL_ACCELERATION_STRUCTURE_HEADER_SIZE)
    auto locks = std::bit_ceil(total_lanes_);
    WMTBufferInfo locks_info{
        sizeof(AccelerationStructureHeader) + sizeof(uint32_t) * (locks + 2), WMTResourceStorageModeShared
    };
    atomic_locks_ = GetMTLDevice().newBuffer(locks_info);
    if (!atomic_locks_) {
      ERR("Failed to create the lock table of 64-bit atomics");
      return E_FAIL;
    }
    auto nothing = static_cast<AccelerationStructureHeader *>(locks_info.memory.get());
    *nothing = {};
    auto words = reinterpret_cast<uint32_t *>(nothing + 1);
    words[0] = locks - 1;
    std::fill_n(words + 1, locks + 1, 0u);
    atomic_locks_given_up_ = words + locks + 1;
    atomic_locks_address_ = locks_info.gpu_address + sizeof(AccelerationStructureHeader);
    RegisterResidency(atomic_locks_);
    WMTBufferInfo null_info{D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4 * sizeof(uint32_t), WMTResourceStorageModeShared};
    if (!(null_buffer_ = GetMTLDevice().newBuffer(null_info))) {
      ERR("Failed to create the target of null buffer views");
      return E_FAIL;
    }
    std::memset(null_info.memory.get(), 0, null_info.length);
    null_buffer_address_ = null_info.gpu_address;
    RegisterResidency(null_buffer_);

    // the structure of nothing has no instances: their count is the zero its header has for their address, and the
    // buffer Metal wants them in is one it then reads nothing of. it is built here, on a queue of its own
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS no_instances{D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL};
    auto none = AccelerationStructureInfo(
        this, no_instances, nullptr, false, atomic_locks_, atomic_locks_, offsetof(AccelerationStructureHeader, instances)
    );
    GetMTLDevice().accelerationStructureSizes(none);
    null_scene_ = std::make_unique<AccelerationStructure>(this, none.size, 0);
    WMTBufferInfo scratch_info{none.build_scratch_size, WMTResourceStorageModePrivate};
    auto scratch = GetMTLDevice().newBuffer(scratch_info);
    wmtcmd_accelerationstructure_build build{};
    build.type = WMTAccelerationStructureCommandBuild;
    build.dst = null_scene_->structure;
    build.scratch = scratch;
    build.info = none;
    auto queue = GetMTLDevice().newCommandQueue(1);
    auto cmdbuf = queue.commandBuffer();
    auto encoder = cmdbuf.accelerationStructureCommandEncoder();
    encoder.encodeCommands((const wmtcmd_accelerationstructure_nop *)&build);
    encoder.endEncoding();
    cmdbuf.commit();
    cmdbuf.waitUntilCompleted();
    nothing->structure = null_scene_->header.structure;

    return S_OK;
  };

  uint64_t
  GetAtomicLocks() {
    return atomic_locks_address_;
  }

  uint64_t
  GetNullBuffer() {
    return null_buffer_address_;
  }

  obj_handle_t
  GetNullBufferHandle() {
    return null_buffer_.handle;
  }

  void
  CheckAtomicLocks() {
    if (__atomic_exchange_n(atomic_locks_given_up_, 0u, __ATOMIC_RELAXED)) {
      ERR("A 64-bit atomic gave up waiting for its lock; removing the device");
      LoseDevice(DXGI_ERROR_DEVICE_HUNG);
    }
  }

  uint32_t
  GetSIMDWidth() {
    return simd_width_;
  }

  uint64_t
  ThreadsAtOnce() {
    // MULLION_D3D12_THREADS_AT_ONCE gives the number, for a test or to tell what a part's size does
    static const uint64_t given = strtoull(env::getEnvVar("MULLION_D3D12_THREADS_AT_ONCE").c_str(), nullptr, 10);
    return given ? given : total_lanes_;
  }

  SM50_SHADER_METAL_VERSION
  GetMetalVersion() {
    static_assert(
        SM50_SHADER_METAL_310 == uint32_t(WMTMetal310) && SM50_SHADER_METAL_320 == uint32_t(WMTMetal320) &&
        SM50_SHADER_METAL_400 == uint32_t(WMTMetal400)
    );
    return SM50_SHADER_METAL_VERSION(ShaderMetalVersion(GetMTLDevice()));
  }

  WMT::Device
  GetMTLDevice() {
    return adapter_->GetMTLDevice();
  };

  // the highest level the device supports, as D3D12_FEATURE_FEATURE_LEVELS answers it (the HUD shows it)
  D3D_FEATURE_LEVEL
  GetFeatureLevel() {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                               D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS data{ARRAYSIZE(levels), levels, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data))))
      return D3D_FEATURE_LEVEL_11_0;
    return data.MaxSupportedFeatureLevel;
  };

  HRESULT
  GetAdapter(REFIID riid, void **ppAdapter) {
    return adapter_->QueryInterface(riid, ppAdapter);
  };

  UINT STDMETHODCALLTYPE
  GetNodeCount() {
    return 1; // FIXME
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device) ||
        riid == __uuidof(ID3D12Device1) || riid == __uuidof(ID3D12Device2) || riid == __uuidof(ID3D12Device3) ||
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5) || riid == __uuidof(ID3D12Device6) ||
        riid == __uuidof(ID3D12Device7) || riid == __uuidof(ID3D12Device8) || riid == __uuidof(ID3D12Device9) ||
        riid == __uuidof(ID3D12Device10)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D12DeviceConfiguration) || riid == __uuidof(ID3D12DeviceConfiguration1)) {
      *ppvObject = ref(static_cast<ID3D12DeviceConfiguration1 *>(this));
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11On12Source)) {
      *ppvObject = ref(&on12_source_);
      return S_OK;
    }

    if (riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IDXGIDevice4))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12DeviceDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12Device1), riid)) {
      WARN("D3D12Device: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Object::QueryInterface(riid, ppvObject);
  }

  D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DEVICE_CONFIGURATION_DESC *desc) {
    *desc = {D3D12_DEVICE_FLAG_NONE, 0, D3D12_SDK_VERSION, UINT(experimental_shader_models_)};
    return desc;
  }

  HRESULT STDMETHODCALLTYPE
  GetEnabledExperimentalFeatures(GUID *guids, UINT count) {
    if (count < UINT(experimental_shader_models_) || (experimental_shader_models_ && !guids))
      return FAIL(E_INVALIDARG);
    if (experimental_shader_models_)
      guids[0] = D3D12ExperimentalShaderModels;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **result, ID3DBlob **error) {
    return D3D12SerializeVersionedRootSignature(desc, result, error);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializer(const void *blob, SIZE_T size, REFIID riid, void **deserializer) {
    return D3D12CreateVersionedRootSignatureDeserializer(blob, size, riid, deserializer);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library, SIZE_T size, LPCWSTR name, REFIID riid, void **deserializer
  ) {
    return DeserializeLibraryRootSignature(library, size, name, riid, deserializer);
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
    InitReturnPtr(ppCommandQueue);
    if (FAILED(removed_))
      return FAIL(DXGI_ERROR_DEVICE_REMOVED);
    if (pDesc->Flags)
      WARN("CreateCommandQueue: flags ignored: ", pDesc->Flags);
    // a priority CheckFeatureSupport reports unsupported (D3D12_FEATURE_COMMAND_QUEUE_PRIORITY)
    if (pDesc->Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
      return FAIL(E_INVALIDARG);
    return mullion::CreateCommandQueue(this, pDesc, riid, ppCommandQueue);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
    return mullion::CreateCommandAllocator(this, Type, riid, ppCommandAllocator);
  };

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    InitReturnPtr(ppPipelineState);
    return mullion::CreateGraphicsPipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    InitReturnPtr(ppPipelineState);
    return mullion::CreateComputePipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator *pCommandAllocator,
      ID3D12PipelineState *pInitialPipelineState, REFIID riid, void **ppCommandList
  ) {
    if (!pCommandAllocator)
      return FAIL(E_INVALIDARG);
    auto allocator = static_cast<MTLD3D12CommandAllocator *>(pCommandAllocator);
    return allocator->CreateCommandList(NodeMask, Type, pInitialPipelineState, riid, ppCommandList);
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D12_FEATURE Feature, void *pFeatureData, UINT DataSize) {
    auto metal = GetMTLDevice();
    switch (UINT(Feature)) {
    case D3D12_FEATURE_ARCHITECTURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(pFeatureData);
      if (out->NodeIndex > 0)
        return FAIL(E_INVALIDARG);
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(pFeatureData);
      if (out->NodeIndex > 0)
        return FAIL(E_INVALIDARG);
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      out->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(pFeatureData);

      out->NumQualityLevels = 0;
      if (!out->SampleCount || out->SampleCount > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT)
        return FAIL(E_FAIL);

      MTL_DXGI_FORMAT_DESC format_desc;
      MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (!format_desc.PlanarCount && !IsSamplerFeedback(out->Format))
        return FAIL(E_INVALIDARG);
      // a multisampled texture is a multisampled target: what the format's support says
      uint32_t support1 = 0, support2 = 0;
      MTLQueryDXGIFormatSupport(
          metal, out->Format, [this](WMTPixelFormat f) { return GetMTLPixelFormatCapability(f); }, support1, support2
      );
      bool multisample = support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
      if (out->SampleCount == 1 || (!out->Flags && multisample && metal.supportsTextureSampleCount(out->SampleCount)))
        out->NumQualityLevels = 1;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(pFeatureData);
      /* the highest version both know: newer ones (1.2) come down to 1.1, as the runtime answers */
      if (out->HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1 ||
          out->HighestVersion > sdk::D3D_ROOT_SIGNATURE_VERSION_1_2)
        return FAIL(E_INVALIDARG);
      out->HighestVersion = std::min(out->HighestVersion, D3D_ROOT_SIGNATURE_VERSION_1_1);
      return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(pFeatureData);
      if (!out->NumFeatureLevels)
        return FAIL(E_INVALIDARG);
      // the highest level the options make up, as D3D12 defines the levels: 11_1 with logic ops, 12_0 with tiled
      // resources tier 2, binding tier 2 and typed UAV loads, 12_1 with ROVs and conservative rasterization tier 1,
      // 12_2 (Direct3D feature level 12_2) with shader model 6.5, ray tracing tier 1.1, variable rate shading tier 2,
      // mesh shaders, sampler feedback, binding, tiled resources and conservative rasterization tier 3, root
      // signature 1.1 and the depth bounds test
      D3D12_FEATURE_DATA_D3D12_OPTIONS o;
      D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2;
      D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
      D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6;
      D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7;
      D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
      D3D12_FEATURE_DATA_ROOT_SIGNATURE root{D3D_ROOT_SIGNATURE_VERSION_1_1};
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof(o2));
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6));
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
      CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
      CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root, sizeof(root));
      bool level_12_2 =
          model.HighestShaderModel >= D3D_SHADER_MODEL_6_5 && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
          o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 &&
          o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 && o7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9 &&
          o.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 &&
          o.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_3 &&
          o.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_3 &&
          root.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 && o2.DepthBoundsTestSupported;
      auto max_level = !o.OutputMergerLogicOp ? D3D_FEATURE_LEVEL_11_0
                       : o.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2 ||
                               o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2 || !o.TypedUAVLoadAdditionalFormats
                           ? D3D_FEATURE_LEVEL_11_1
                       : !o.ROVsSupported || !o.ConservativeRasterizationTier ? D3D_FEATURE_LEVEL_12_0
                       : !level_12_2                                          ? D3D_FEATURE_LEVEL_12_1
                                                                              : D3D_FEATURE_LEVEL_12_2;
      out->MaxSupportedFeatureLevel = {};
      for (unsigned i = 0; i < out->NumFeatureLevels; i++)
        if (out->pFeatureLevelsRequested[i] <= max_level)
          out->MaxSupportedFeatureLevel = std::max(out->MaxSupportedFeatureLevel, out->pFeatureLevelsRequested[i]);
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO:  {
       if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(pFeatureData);
      MTL_DXGI_FORMAT_DESC format_desc;
      MTLQueryDXGIFormat(metal, out->Format, format_desc);
      auto planes = IsSamplerFeedback(out->Format) ? 1 : format_desc.PlanarCount;
      if (!planes)
        return FAIL(E_INVALIDARG);

      out->PlaneCount = planes;
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(pFeatureData);
      out->MaxGPUVirtualAddressBitsPerProcess = 48;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(pFeatureData);
      out->HighestShaderModel = std::min(out->HighestShaderModel, D3D_SHADER_MODEL_6_7);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(pFeatureData);
      out->DoublePrecisionFloatShaderOps = FALSE;
#ifdef MULLION_NO_PRIVATE_API
      out->OutputMergerLogicOp = FALSE;
#else
      out->OutputMergerLogicOp = TRUE;
#endif
      out->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
      // placement sparse resources carry tier 3: a reserved 3D texture is a placement sparse 3D texture, whose tiles
      // GetResourceTiling reports, Metal's slabs a texel deep rather than D3D11.3's standard 3D shapes (5.9.4)
      out->TiledResourcesTier = sparse_page_size_ ? D3D12_TILED_RESOURCES_TIER_3 : D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      out->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
      out->PSSpecifiedStencilRefSupported = TRUE;
      out->TypedUAVLoadAdditionalFormats = TRUE;
      out->ROVsSupported = TRUE;
      // emulated: the mesh stage grows triangles, degenerate ones too, and the pixel shader takes the pixels they
      // overlap and knows those inside (airconv)
      out->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_3;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      out->StandardSwizzle64KBSupported = FALSE;
      out->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      out->CrossAdapterRowMajorTextureSupported = FALSE;
      out->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
      out->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS16: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16 *>(pFeatureData);
      out->GPUUploadHeapSupported = TRUE;
      out->DynamicDepthBiasSupported = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(pFeatureData);
      // Metal's on Apple10, the pixel shader's elsewhere (d3d12_command_list.cpp, DepthBoundsSamples)
      out->DepthBoundsTestSupported = TRUE;
      // every Apple GPU family places samples where a render pass says (Metal feature set tables), one pattern for
      // all pixels
      out->ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(pFeatureData);
      out->CastingFullyTypedFormatSupported = TRUE;
      // Metal 4's shading language reads barycentrics and inputs at each vertex (SM50_SHADER_METAL_400)
      out->BarycentricsSupported = GetMTLDevice().supportsFamily(WMTGPUFamilyMetal4) &&
                                   GetMTLDevice().supportsShaderBarycentricCoordinates();
      out->CopyQueueTimestampQueriesSupported = FALSE;
      out->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      // a list's own copies, after the work before them whatever the marker mode (WriteImmediate); bundles record none
      out->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT |
                                              D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
                                              D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(pFeatureData);
      out->WaveOps = TRUE;
      out->WaveLaneCountMin = simd_width_;
      out->WaveLaneCountMax = simd_width_;
      out->TotalLaneCount = total_lanes_;
      // If CheckFeatureSupport succeeds this value will always be true.
      out->ExpandedComputeResourceStates = TRUE;
      // with shader model 6.6 this promises 64-bit atomics on buffers, which the shader compiler builds from locks
      out->Int64ShaderOps = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(pFeatureData);
      out->RelaxedFormatCastingSupported = FALSE;
      // ID3D12Device10's layouts and ID3D12GraphicsCommandList7::Barrier
      out->EnhancedBarriersSupported = TRUE;
      out->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(pFeatureData);
      out->MSAA64KBAlignedTextureSupported = TRUE;
      out->Native16BitShaderOpsSupported = TRUE;
      // TODO(d3d12): revise when d3d12 shared resource is implemented
      out->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(pFeatureData);
      out->SRVOnlyTiledResourceTier3 = FALSE;
      out->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      out->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(pFeatureData);
      out->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
      // 1.0 would need the first mip and slice of a paired texture's view in the shader
      out->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_1_0;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(pFeatureData);
      out->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY |
                          D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_INPROC_CACHE |
                          D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE;
      return S_OK;
    }
    case sdk::D3D12_FEATURE_PREDICATION:
    case sdk::D3D12_FEATURE_HARDWARE_COPY:
      // these reserved queries return E_NOTIMPL on Windows (vkd3d-proton, test_misc_agility_sdk_feature_checks)
      return FAIL(E_NOTIMPL);
    case sdk::D3D12_FEATURE_APPLICATION_SPECIFIC_DRIVER_STATE:
      return NotSupported<sdk::D3D12_FEATURE_DATA_APPLICATION_SPECIFIC_DRIVER_STATE>(pFeatureData, DataSize);
    case sdk::D3D12_FEATURE_BYTECODE_BYPASS_HASH_SUPPORTED:
      return NotSupported<sdk::D3D12_FEATURE_DATA_BYTECODE_BYPASS_HASH_SUPPORTED>(pFeatureData, DataSize);
    case sdk::D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT:
      // no shader-cache ABI is a failure, not an unknown query (vkd3d-proton, test_misc_agility_sdk_feature_checks)
      return DataSize == sizeof(sdk::D3D12_FEATURE_DATA_SHADERCACHE_ABI_SUPPORT) ? FAIL(E_FAIL) : FAIL(E_INVALIDARG);
    case sdk::D3D12_FEATURE_D3D12_OPTIONS22: {
      if (DataSize != sizeof(sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS22))
        return FAIL(E_INVALIDARG);
      D3D12_FEATURE_DATA_D3D12_OPTIONS7 mesh;
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &mesh, sizeof(mesh));
      auto *out = reinterpret_cast<sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS22 *>(pFeatureData);
      // DirectX-Specs, Mesh Shader, DispatchMesh API: each count must be less than 64k
      constexpr UINT mesh_limit = std::numeric_limits<UINT16>::max();
      *out = {FALSE, FALSE, D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION,
              mesh.MeshShaderTier ? mesh_limit : 0u};
      return S_OK;
    }
    case sdk::D3D12_FEATURE_BARRIER_LAYOUT: {
      if (DataSize != sizeof(sdk::D3D12_FEATURE_DATA_BARRIER_LAYOUT))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<sdk::D3D12_FEATURE_DATA_BARRIER_LAYOUT *>(pFeatureData);
      // queue compatibility is defined by DirectX-Specs, Enhanced Barriers, Command Queue Layout Compatibility
      switch (out->Layout) {
      case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
      case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
      case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
      case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
      case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
      case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE:
      case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
      case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
      case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
      case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
      case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
      case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_DIRECT;
        break;
      case D3D12_BARRIER_LAYOUT_GENERIC_READ:
      case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
      case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
      case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
      case D3D12_BARRIER_LAYOUT_COPY_DEST:
      case sdk::D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                         out->CommandListType == D3D12_COMMAND_LIST_TYPE_COMPUTE;
        break;
      case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON:
      case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
      case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
      case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
      case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:
      case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_COMPUTE;
        break;
      case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ:
      case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
        break;
      case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ:
      case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
        break;
      case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ:
      case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
        break;
      case D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON:
        out->Supported = out->CommandListType == D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE ||
                         out->CommandListType == D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS ||
                         out->CommandListType == D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
        break;
      default:
        out->Supported = TRUE;
        break;
      }
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureData);
      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
        out->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
        return S_OK;
      }
      if (IsSamplerFeedback(out->Format)) {
        out->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_MIP;
        out->Support2 = D3D12_FORMAT_SUPPORT2_SAMPLER_FEEDBACK;
        return S_OK;
      }
      // D3D12_FORMAT_SUPPORT1 and 2 share D3D11's bit values, but for D3D11's mip generation, CPU access and sharing
      HRESULT hr = MTLQueryDXGIFormatSupport(
          GetMTLDevice(), out->Format, [this](WMTPixelFormat f) { return GetMTLPixelFormatCapability(f); },
          *reinterpret_cast<uint32_t *>(&out->Support1), *reinterpret_cast<uint32_t *>(&out->Support2)
      );
      out->Support1 &= ~D3D12_FORMAT_SUPPORT1(D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE);
      out->Support2 &= ~D3D12_FORMAT_SUPPORT2(D3D11_FORMAT_SUPPORT2_SHAREABLE);
      return hr;
    }
    // 64-bit atomics on typed resources, groupshared memory and descriptor heap resources, which the shader compiler
    // builds from locks as it does on root buffers
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS9>(pFeatureData, DataSize) != S_OK)
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9 *>(pFeatureData);
      out->AtomicInt64OnTypedResourceSupported = TRUE;
      out->AtomicInt64OnGroupSharedSupported = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS11: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS11>(pFeatureData, DataSize) != S_OK)
        return FAIL(E_INVALIDARG);
      reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11 *>(pFeatureData)->AtomicInt64OnDescriptorHeapResourceSupported = TRUE;
      return S_OK;
    }
    // variable rate shading, which pixel shaders of coarse pixels emulate: per primitive from a vertex or geometry
    // shader, and from a shading rate image of 16x16 tiles; coarse pixels of up to 2x2 (CoarseShading, airconv)
    case D3D12_FEATURE_D3D12_OPTIONS6: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS6>(pFeatureData, DataSize) != S_OK)
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS6 *>(pFeatureData);
      out->AdditionalShadingRatesSupported = FALSE;
      out->PerPrimitiveShadingRateSupportedWithViewportIndexing = FALSE;
      out->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_2;
      out->ShadingRateImageTileSize = 16;
      out->BackgroundProcessingSupported = FALSE;
      return S_OK;
    }
    // the sum combiner; a mesh shader's SV_ShadingRate is not read
    case D3D12_FEATURE_D3D12_OPTIONS10: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS10>(pFeatureData, DataSize) != S_OK)
        return E_INVALIDARG;
      reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS10 *>(pFeatureData)->VariableRateShadingSumCombinerSupported =
          TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS14: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS14>(pFeatureData, DataSize) != S_OK)
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS14 *>(pFeatureData);
      out->IndependentFrontAndBackStencilRefMaskSupported = TRUE;
      return S_OK;
    }
    /* capabilities with no complete path yet: D3D12 defines an all-zero answer as unsupported, or tier 0, in each */
    case D3D12_FEATURE_D3D12_OPTIONS8:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS8>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS13: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13))
        return FAIL(E_INVALIDARG);
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS13 *>(pFeatureData);
      *out = {};
      // the blend factor's alpha is Metal's blend color alpha (WMTBlendFactorBlendAlpha)
      out->AlphaBlendFactorSupported = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS15:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS15>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS17:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS17>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS18: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS18))
        return FAIL(E_INVALIDARG);
      // tier 0: BeginRenderPass binds and clears its surfaces as the runtime does for drivers without render passes
      reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS18 *>(pFeatureData)->RenderPassesValid = TRUE;
      return S_OK;
    }
    // titles ask for these (Onimusha: Way of the Sword, dttdrv/mullion#69), and so does D3DX12's CD3DX12FeatureSupport
    // when it starts (DirectX-Headers, d3dx12_check_feature_support.h). they are answered with what a driver without
    // the features reports (DirectX-Specs, VulkanOn12): SupportedSampleCountsWithNoOutputs "will report a value of 1,
    // indicating simple-sampled only, for drivers that do not support the feature", and older drivers' heap limits
    case sdk::D3D12_FEATURE_D3D12_OPTIONS19: {
      if (DataSize != sizeof(sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS19))
        return FAIL(E_INVALIDARG);
      auto out = reinterpret_cast<sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS19 *>(pFeatureData);
      *out = {};
      // a pass with no targets rasterizes one sample until its pipelines agree with it on more
      out->SupportedSampleCountsWithNoOutputs = 0x1;
      out->MaxSamplerDescriptorHeapSize = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
      out->MaxSamplerDescriptorHeapSizeWithStaticSamplers = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
      // "For old drivers reporting D3D12_BINDING_TIER_3, requests for heaps larger than 1,000,000 will still be sent
      // to the driver" (VulkanOn12), and D3DX12 gives the tier 1 limit's name to the value
      out->MaxViewDescriptorHeapSize = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1;
      return S_OK;
    }
    case sdk::D3D12_FEATURE_D3D12_OPTIONS20:
      return NotSupported<sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS20>(pFeatureData, DataSize);
    case sdk::D3D12_FEATURE_D3D12_OPTIONS21: {
      if (DataSize != sizeof(sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS21))
        return FAIL(E_INVALIDARG);
      // no work graphs, and command signatures have tier 1.0's arguments (no incrementing constant)
      *reinterpret_cast<sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS21 *>(pFeatureData) = {
          0, sdk::D3D12_EXECUTE_INDIRECT_TIER_1_0, FALSE, FALSE
      };
      return S_OK;
    }
    // normal and high are given, and a high-priority queue runs as the others do (as vkd3d-proton's does). global
    // realtime is not, and CreateCommandQueue refuses it: "Requests to create a global realtime command queue won't
    // silently downgrade the priority when it can't be supported; the request succeeds or fails as-is"
    // (D3D12_COMMAND_QUEUE_PRIORITY). only the queue types Mullion makes have a priority
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY))
        return FAIL(E_INVALIDARG);
      auto out = reinterpret_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *>(pFeatureData);
      auto type = out->CommandListType;
      out->PriorityForTypeIsSupported =
          (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
           type == D3D12_COMMAND_LIST_TYPE_COPY) &&
          (out->Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL || out->Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
      return S_OK;
    }
    case D3D12_FEATURE_EXISTING_HEAPS:
      return NotSupported<D3D12_FEATURE_DATA_EXISTING_HEAPS>(pFeatureData, DataSize);
    case D3D12_FEATURE_CROSS_NODE:
      return NotSupported<D3D12_FEATURE_DATA_CROSS_NODE>(pFeatureData, DataSize);
    case D3D12_FEATURE_DISPLAYABLE:
      return NotSupported<D3D12_FEATURE_DATA_DISPLAYABLE>(pFeatureData, DataSize);
    /* the same for queries that name a node, of which there is one */
    case D3D12_FEATURE_SERIALIZATION:
      return NotSupportedOnNode<D3D12_FEATURE_DATA_SERIALIZATION>(pFeatureData, DataSize);
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT:
      return NotSupportedOnNode<D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT>(pFeatureData, DataSize);
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
      return NotSupportedOnNode<D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT>(pFeatureData, DataSize);
    // D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT (Agility SDK 1.716): resources keep their 64 KiB placement alignment. Unity
    // 6.3 asks for it with the Agility SDK runtime and falls back to D3D11 when the query fails (PEAK, 2026-10-24)
    case sdk::D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT: {
      if (DataSize != sizeof(UINT))
        return E_INVALIDARG;
      *reinterpret_cast<UINT *>(pFeatureData) = 0; // D3D12_TIGHT_ALIGNMENT_TIER_NOT_SUPPORTED
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES))
        return FAIL(E_INVALIDARG);
      auto *in = reinterpret_cast<D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES *>(pFeatureData);
      /* there are no types, so the count asked for must be 0 */
      return in->NodeIndex || in->Count ? FAIL(E_INVALIDARG) : S_OK;
    }
    default:
      break;
    }
    /* as the runtime answers a feature it does not know */
    WARN("CheckFeatureSupport: unknown feature ", Feature);
    return E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap) {
    return mullion::CreateDescriptorHeap(this, pDesc, riid, ppDescriptorHeap);
  };

  UINT STDMETHODCALLTYPE
  GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) {
    switch (DescriptorHeapType) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      return 1u << EMBEDDED_DESCRIPTOR_HANDLE::kIncrementBits;
    default:
      break;
    }
    return 0;
  };

  HRESULT STDMETHODCALLTYPE
  CreateRootSignature(
      UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid, void **ppRootSignature
  ) {
    return mullion::CreateRootSignature(this, NodeMask, pBytecode, BytecodeLength, riid, ppRootSignature);
  };

  void STDMETHODCALLTYPE
  CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
    if (pDesc)
      Heap->AddConstantBufferView(Index, pDesc->BufferLocation, pDesc->SizeInBytes);
    else
      Heap->AddConstantBufferView(Index, 0, 0);
  };

  void STDMETHODCALLTYPE
  CreateShaderResourceView(
      ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource || (pDesc && pDesc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)) {
      auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
      if (pDesc && pDesc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
        Heap->AddAccelerationStructureView(Index, pDesc->RaytracingAccelerationStructure.Location);
      else
        Heap->AddShaderResourceView(Index, pDesc);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateShaderResourceView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pResource, ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
      Heap->AddUnorderedAccessView(Index, pDesc);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateUnorderedAccessView(pCounter, pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateRenderTargetView(
      ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
      Heap->AddRenderTarget(Index, nullptr);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateRenderTargetView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateDepthStencilView(
      ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
      Heap->AddRenderTarget(Index, nullptr);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateDepthStencilView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateSampler(const D3D12_SAMPLER_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    auto [Heap, Index] = GetSamplerDescriptorHeap(this, Descriptor);
    Heap->AddSampler(Index, pDesc);
  };

  void STDMETHODCALLTYPE
  CopyDescriptors(
      UINT DstDescriptorRangeCount, const D3D12_CPU_DESCRIPTOR_HANDLE *DstDescriptorRangeOffsets,
      const UINT *DstDescriptorRangeSizes, UINT SrcDescriptorRangeCount,
      const D3D12_CPU_DESCRIPTOR_HANDLE *SrcDescriptorRangeOffsets, const UINT *SrcDescriptorRangeSizes,
      D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType
  ) {
    unsigned int dst_range_idx, dst_idx, src_range_idx, src_idx;
    unsigned int dst_range_size, src_range_size, copy_count;

    dst_range_idx = dst_idx = 0;
    src_range_idx = src_idx = 0;
    while (dst_range_idx < DstDescriptorRangeCount && src_range_idx < SrcDescriptorRangeCount) {
      dst_range_size = DstDescriptorRangeSizes ? DstDescriptorRangeSizes[dst_range_idx] : 1;
      src_range_size = SrcDescriptorRangeSizes ? SrcDescriptorRangeSizes[src_range_idx] : 1;

      copy_count = std::min(dst_range_size - dst_idx, src_range_size - src_idx);

      switch (DescriptorHeapType) {
      case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
        auto [DstRangeHeap, DstRangeIndex] =
            GetShaderVisibleDescriptorHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] =
            GetShaderVisibleDescriptorHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }

      case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
        auto [DstRangeHeap, DstRangeIndex] = GetSamplerDescriptorHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] = GetSamplerDescriptorHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }
      case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
      case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: {
        auto [DstRangeHeap, DstRangeIndex] = GetRenderTargetHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] = GetRenderTargetHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }
      default:
        return;
      }

      dst_idx += copy_count;
      src_idx += copy_count;

      if (dst_idx >= dst_range_size) {
        ++dst_range_idx;
        dst_idx = 0;
      }
      if (src_idx >= src_range_size) {
        ++src_range_idx;
        src_idx = 0;
      }
    }
  };

  void STDMETHODCALLTYPE
  CopyDescriptorsSimple(
      UINT DescriptorCount, const D3D12_CPU_DESCRIPTOR_HANDLE DstDescriptorRangeOffset,
      const D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeOffset, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType
  ) {
    CopyDescriptors(
        1, &DstDescriptorRangeOffset, &DescriptorCount, 1, &SrcDescriptorRangeOffset, &DescriptorCount,
        DescriptorHeapType
    );
  };

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount, const D3D12_RESOURCE_DESC *pDescs
  ) {
    return GetResourceAllocationInfo1(__ret, VisibleMask, ResourceDestCount, pDescs, nullptr);
  };

  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT NodeMask, D3D12_HEAP_TYPE HeapType) {
    __ret->Type = D3D12_HEAP_TYPE_CUSTOM;
    __ret->CreationNodeMask = 1;
    __ret->VisibleNodeMask = 1;
    switch (HeapType) {
    case D3D12_HEAP_TYPE_DEFAULT:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
      __ret->MemoryPoolPreference = advertise_numa_ ? D3D12_MEMORY_POOL_L1 : D3D12_MEMORY_POOL_L0;
      break;
    // a GPU upload heap's are an upload heap's on a UMA adapter (D3D12 GPU Upload Heaps)
    case D3D12_HEAP_TYPE_UPLOAD:
    case sdk::D3D12_HEAP_TYPE_GPU_UPLOAD:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    case D3D12_HEAP_TYPE_READBACK:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    default:
      E_INVALIDARG;
    }

    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(pHeapProps, HeapFlags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, pHeapProps);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, pHeapProps);
    if (FAILED(hr))
      return hr;
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return TraceResource(
          CreateCommittedTexture(this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource),
          "committed", pDesc, ppResource, str::format(", heap type ", uint32_t(pHeapProps->Type))
      );
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return TraceResource(
          CreateCommittedBuffer(this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource),
          "committed", pDesc, ppResource, str::format(", heap type ", uint32_t(pHeapProps->Type))
      );
    default:
      break;
    }
    return FAIL(E_INVALIDARG);
  };

  HRESULT STDMETHODCALLTYPE
  CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&pDesc->Properties, pDesc->Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = mullion::CreateHeap(this, pDesc, riid, ppHeap);
    TRACE_IN(Memory, "heap of ", pDesc->SizeInBytes >> 10, " KB, type ", uint32_t(pDesc->Properties.Type), ", flags ",
             uint32_t(pDesc->Flags), ": ", SUCCEEDED(hr) && ppHeap ? *ppHeap : nullptr,
             SUCCEEDED(hr) ? "" : str::format(", failed ", uint32_t(hr)));
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource(
      ID3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (!pHeap)
      return FAIL(E_INVALIDARG);
    auto d3d12heap = static_cast<MTLD3D12Heap *>(pHeap);
    auto heap_desc = d3d12heap->GetDesc();
    HRESULT hr = S_OK;
    hr = d3d12heap->memory ? S_OK : ValidateHeapProperties(&heap_desc.Properties, heap_desc.Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    D3D12_RESOURCE_ALLOCATION_INFO info;
    GetResourceAllocationInfo(&info, 0, 1, pDesc);
    // "HeapOffset plus the resource size must be smaller than or equal to the heap size" (CreatePlacedResource), and a
    // buffer's size is its width, though its allocation size is given in 64KB (vkd3d-proton checks the width too):
    // Starfield places a small one at the start of a heap of one page it imported (#32)
    UINT64 needed = pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? pDesc->Width : info.SizeInBytes;
    if (heap_desc.Alignment < info.Alignment || Offset % info.Alignment || needed > heap_desc.SizeInBytes ||
        Offset > heap_desc.SizeInBytes - needed) {
      WARN("CreatePlacedResource: ", needed, " bytes at alignment ", info.Alignment, " placed at ", Offset,
           " of a heap of ", heap_desc.SizeInBytes, " at alignment ", heap_desc.Alignment, " (dimension ",
           pDesc->Dimension, ", format ", pDesc->Format, ", alignment ", pDesc->Alignment, ")");
      return FAIL(E_INVALIDARG);
    }
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return TraceResource(
          CreatePlacedTexture(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource),
          "placed", pDesc, ppResource, str::format(", heap ", pHeap, " at ", Offset)
      );
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return TraceResource(
          CreatePlacedBuffer(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource),
          "placed", pDesc, ppResource, str::format(", heap ", pHeap, " at ", Offset)
      );
    default:
      break;
    }
    return FAIL(E_INVALIDARG);
  };

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **resource
  ) {
    InitReturnPtr(resource);
    if (!sparse_page_size_) {
      ERR("CreateReservedResource: tiled resources need placement sparse resources");
      return E_NOTIMPL;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    if (HRESULT hr = ValidateResourceDescs(pDesc, &heap); FAILED(hr))
      return hr;
    if (HRESULT hr = ValidateResourceStates(InitialState, &heap); FAILED(hr))
      return hr;
    if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && OptimizedClearValue)
      return FAIL(E_INVALIDARG);
    return TraceResource(pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                             ? CreateReservedBuffer(this, pDesc, riid, resource)
                             : CreateReservedTexture(this, pDesc, InitialState, riid, resource),
                         "reserved", pDesc, resource, "");
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle
  ) {
    InitReturnPtr(handle);
    Com<ID3D12Fence> fence;
    if (!object || !handle)
      return E_INVALIDARG;
    // only fences are shared so far, not resources and heaps
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&fence))))
      return E_NOTIMPL;
    // a fence made with D3D12_FENCE_FLAG_SHARED, whose Access "is GENERIC_ALL" (ID3D12Device::CreateSharedHandle)
    auto allocation = static_cast<MTLD3D12Fence *>(fence.ptr())->SharedAllocation();
    if (!allocation || access != GENERIC_ALL)
      return E_INVALIDARG;
    return shareHandle(allocation, attributes, access, name, handle);
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandle(HANDLE handle, REFIID riid, void **object) {
    InitReturnPtr(object);
    D3DKMT_HANDLE allocation;
    auto event = openSharedEvent(GetMTLDevice(), GetLocalD3DKMT(), handle, allocation);
    return OpenFence(this, std::move(event), allocation, riid, object);
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
    InitReturnPtr(handle);
    // "Currently the only value this parameter accepts is GENERIC_ALL" (OpenSharedHandleByName, Access)
    return name && handle && access == GENERIC_ALL ? openHandleByName(name, access, handle) : E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  MakeResident(UINT ObjectCount, ID3D12Pageable *const *objects) {
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  Evict(UINT ObjectCount, ID3D12Pageable *const *objects) {
    // residency is a hint: memory that stays resident meets every contract of an evicted object
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
    return mullion::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

  HRESULT STDMETHODCALLTYPE
  GetDeviceRemovedReason() {
    SettleCommandQueueFeedback(this);
    return removed_;
  };

  HRESULT
  RemovedReason() final {
    return removed_;
  }

  void
  LoseDevice(HRESULT Reason) {
    decltype(fences.values) pending;
    {
      std::lock_guard lock(fences.mutex);
      HRESULT present = S_OK;
      if (!removed_.compare_exchange_strong(present, Reason))
        return;
      pending.swap(fences.values);
    }
    FLIGHT_IN(Gpu, "device %p is removed: %#llx", this, uint64_t(uint32_t(Reason)));
    WMTFlightDump("a removed device");
    // RemoveDevice signals all fences to UINT64_MAX; late queue signals must not take them back.
    for (auto &fence : pending)
      fence->Reach(UINT64_MAX, true);
  }

  void STDMETHODCALLTYPE GetCopyableFootprints(
      const D3D12_RESOURCE_DESC *pDesc, UINT FirstSubresource, UINT SubresourceCount, UINT64 BaseOffset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes
  ) {
    UINT64 TotalBytes = 0;
    UINT64 Offset = 0;
    UINT BlockWidth = 1;
    do {
      if (!pDesc)
        break;

      UINT PlaneCount = 1;
      UINT PerPlaneSubresources = DecomposeSubresource(*pDesc, 0);
      DXGI_FORMAT PlaneFormats[2] = {};
      UINT PlaneBytesPerTexel[2] = {};
      UINT SubsampleWidth[2] = {1, 1}, SubsampleHeight[2] = {1, 1};

      MTL_DXGI_FORMAT_DESC FormatDesc;

      if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (pDesc->Format != DXGI_FORMAT_UNKNOWN)
          break;
        PlaneFormats[0] = DXGI_FORMAT_UNKNOWN;
        PlaneBytesPerTexel[0] = 1;
      } else {
        if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), pDesc->Format, FormatDesc)))
          break;

        if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && pDesc->Height != 1)
          break;

        if (FormatDesc.Flag & MTL_DXGI_FORMAT_BC)
          BlockWidth = 4;

        if (FormatDesc.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
          WMTTextureInfo info{};
          if (FAILED(PopulateWMTTextureInfo(this, info, *pDesc)))
            break;
          PlaneCount = FormatDesc.PlanarCount;
          for (UINT plane = 0; plane < PlaneCount; plane++) {
            MTL_DXGI_FORMAT_DESC layout;
            MTLQueryDXGIFormat(GetMTLDevice(), pDesc->Format, layout, plane);
            PlaneFormats[plane] = DXGI_FORMAT(layout.PlaneFormat);
            PlaneBytesPerTexel[plane] = layout.BytesPerTexel;
            SubsampleWidth[plane] = layout.SubsampleWidth;
            SubsampleHeight[plane] = layout.SubsampleHeight;
          }
        } else if (FormatDesc.PlanarCount > 1) {
          assert(FormatDesc.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER));
          PlaneCount = FormatDesc.PlanarCount;
          PlaneFormats[0] = DXGI_FORMAT_R32_TYPELESS;
          PlaneBytesPerTexel[0] = 4;
          PlaneFormats[1] = DXGI_FORMAT_R8_TYPELESS;
          PlaneBytesPerTexel[1] = 1;
        } else {
          PlaneFormats[0] = pDesc->Format;
          PlaneBytesPerTexel[0] = FormatDesc.BytesPerTexel;
          // a format with no size has no footprint
          if (PlaneBytesPerTexel[0] == 0)
            break;
        }
      }

      if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        PerPlaneSubresources *= pDesc->DepthOrArraySize;

      if (FirstSubresource >= PerPlaneSubresources * PlaneCount ||
          SubresourceCount > PerPlaneSubresources * PlaneCount - FirstSubresource) {
        WARN("GetCopyableFootprints: subresource is out of range");
        break;
      }

      for (unsigned i = 0; i < SubresourceCount; i++) {
        auto Subresource = FirstSubresource + i;
        auto Plane = 0u, MipLevel = 0u;
        DecomposeSubresource(*pDesc, Subresource, &MipLevel, NULL, &Plane);
        auto Extent = GetResourceExtent(*pDesc, MipLevel);
        auto Width = align(Extent.right, BlockWidth) / SubsampleWidth[Plane];
        auto Height = align(Extent.bottom, BlockWidth) / SubsampleHeight[Plane];
        auto RowCount = Height / BlockWidth;
        auto Depth = Extent.back;
        auto RowSize = (Width / BlockWidth) * PlaneBytesPerTexel[Plane];
        auto RowPitch = align(RowSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * PlaneCount);
        if (pLayouts) {
          pLayouts[i].Offset = BaseOffset + Offset;
          pLayouts[i].Footprint.Format = PlaneFormats[Plane];
          pLayouts[i].Footprint.Width = Width;
          pLayouts[i].Footprint.Height = Height;
          pLayouts[i].Footprint.Depth = Depth;
          pLayouts[i].Footprint.RowPitch = RowPitch;
        }
        if (pNumRows)
          pNumRows[i] = RowCount;
        if (pRowSizeInBytes)
          pRowSizeInBytes[i] = RowSize;

        auto SubresourceSize = RowPitch * (RowCount - 1) + RowSize;
        SubresourceSize =
            align(SubresourceSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * PlaneCount) * (Depth - 1) + SubresourceSize;

        TotalBytes = Offset + SubresourceSize;
        Offset = align(TotalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }
      if (pTotalBytes)
        *pTotalBytes = TotalBytes;
      return;
    } while (0);
    for (unsigned i = 0; i < SubresourceCount; i++) {
      if (pLayouts) {
        pLayouts[i].Offset = ~0ull;
        pLayouts[i].Footprint.Format = ~(DXGI_FORMAT)0u;
        pLayouts[i].Footprint.Width = ~0u;
        pLayouts[i].Footprint.Height = ~0u;
        pLayouts[i].Footprint.Depth = ~0u;
        pLayouts[i].Footprint.RowPitch = ~0u;
      }
      if (pNumRows)
        pNumRows[i] = ~0u;
      if (pRowSizeInBytes)
        pRowSizeInBytes[i] = ~0ull;
    }
    if (pTotalBytes)
      *pTotalBytes = UINT64_MAX;
  };

  HRESULT STDMETHODCALLTYPE
  CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    return mullion::CreateQueryHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  SetStablePowerState(WINBOOL Enable) {
    return FAIL(E_NOTIMPL);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandSignature(
      const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature, REFIID riid,
      void **ppCommandSignature
  ) {
    return mullion::CreateCommandSignature(this, pDesc, pRootSignature, riid, ppCommandSignature);
  };

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D12Resource *pResource, UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo,
      D3D12_TILE_SHAPE *StandardTileShape, UINT *SubresourceTilingCount, UINT FirstSubresourceTiling,
      D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    static_cast<MTLD3D12Resource *>(pResource)->GetResourceTiling(
        TotalTileCount, PackedMipInfo, StandardTileShape, SubresourceTilingCount, FirstSubresourceTiling,
        SubresourceTilings
    );
  };

  LUID *STDMETHODCALLTYPE
  GetAdapterLuid(LUID *ret) {
    DXGI_ADAPTER_DESC desc;
    adapter_->GetDesc(&desc);
    *ret = desc.AdapterLuid;
    return ret;
  }

  HRESULT STDMETHODCALLTYPE
  CreatePipelineLibrary(const void *blob, SIZE_T blob_size, REFIID iid, void **lib) {
    return mullion::CreatePipelineLibrary(this, blob, blob_size, iid, lib);
  };

  HRESULT STDMETHODCALLTYPE
  SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *pFences, const UINT64 *pValues, UINT FenceCount, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
      HANDLE hEvent
  ) {
    if (!FenceCount || (Flags != D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL && Flags != D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY))
      return FAIL(E_INVALIDARG);
    auto set = std::make_shared<std::atomic<bool>>();
    auto done = [=, began = diag::profile ? diag::Now() : 0, thread = GetCurrentThreadId()] {
      if (began)
        diag::Waited("fence", began, thread);
      if (hEvent)
        SetEvent(hEvent);
      *set = true;
      set->notify_all();
    };
    // each fence counts down as it has its value. the count is one when any of them is enough
    auto left = std::make_shared<std::atomic<UINT>>(Flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY ? 1 : FenceCount);
    for (UINT i = 0; i < FenceCount; i++)
      static_cast<MTLD3D12Fence *>(pFences[i])->completion->Expect(pValues[i], [=] {
        if (left->fetch_sub(1) == 1)
          done();
      });
    if (!hEvent)
      set->wait(false);
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(UINT ObjectCount, ID3D12Pageable *const *pObjects, const D3D12_RESIDENCY_PRIORITY *pPriorities) {
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    InitReturnPtr(ppPipelineState);
    PipelineStateStream desc;
    if (HRESULT hr = desc.Parse(pDesc); FAILED(hr))
      return hr;
    return desc.Create(this, riid, ppPipelineState);
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromAddress(const void *pAddress, REFIID riid, void **ppHeap) {
    return OpenHeap(this, pAddress, nullptr, riid, ppHeap);
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void **ppHeap) {
    return hFileMapping ? OpenHeap(this, nullptr, hFileMapping, riid, ppHeap) : FAIL(E_INVALIDARG);
  }

  HRESULT STDMETHODCALLTYPE
  EnqueueMakeResident(
      D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable *const *ppObjects, ID3D12Fence *pFence,
      UINT64 FenceValue
  ) {
    // every object is resident while it exists
    return pFence ? pFence->Signal(FenceValue) : FAIL(E_INVALIDARG);
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandList1(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, D3D12_COMMAND_LIST_FLAGS Flags, REFIID riid, void **ppCommandList
  ) {
    if (NodeMask > 1 || Flags != D3D12_COMMAND_LIST_FLAG_NONE)
      return FAIL(E_INVALIDARG);
    return CreateClosedCommandList(this, Type, riid, ppCommandList);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC *pDesc, REFIID riid, void **ppSession) {
    return FAIL(E_NOTIMPL);
  }

  /* protected sessions are not supported (CreateProtectedResourceSession fails), so none can be passed */
  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource1(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
      ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (pSession)
      return FAIL(E_INVALIDARG);
    return CreateCommittedResource(pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
  CreateHeap1(const D3D12_HEAP_DESC *pDesc, ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppHeap) {
    InitReturnPtr(ppHeap);
    if (pSession)
      return FAIL(E_INVALIDARG);
    return CreateHeap(pDesc, riid, ppHeap);
  }

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource1(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, ID3D12ProtectedResourceSession *pSession, REFIID riid,
      void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (pSession)
      return FAIL(E_INVALIDARG);
    return CreateReservedResource(pDesc, InitialState, OptimizedClearValue, riid, ppResource);
  }

  /* ID3D12Device5: no ray tracing or meta commands yet, as D3D12_FEATURE_D3D12_OPTIONS5 reports */
  HRESULT STDMETHODCALLTYPE
  CreateLifetimeTracker(ID3D12LifetimeOwner *pOwner, REFIID riid, void **ppvTracker) {
    InitReturnPtr(ppvTracker);
    return FAIL(E_NOTIMPL);
  }

  void STDMETHODCALLTYPE
  RemoveDevice() {
    LoseDevice(DXGI_ERROR_DEVICE_REMOVED);
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommands(UINT *pNumMetaCommands, D3D12_META_COMMAND_DESC *pDescs) {
    if (!pNumMetaCommands)
      return FAIL(E_INVALIDARG);
    *pNumMetaCommands = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommandParameters(
      REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage, UINT *pTotalStructureSizeInBytes,
      UINT *pParameterCount, D3D12_META_COMMAND_PARAMETER_DESC *pParameterDescs
  ) {
    return FAIL(E_INVALIDARG);
  }

  HRESULT STDMETHODCALLTYPE
  CreateMetaCommand(
      REFGUID CommandId, UINT NodeMask, const void *pCreationParametersData, SIZE_T CreationParametersDataSizeInBytes,
      REFIID riid, void **ppMetaCommand
  ) {
    InitReturnPtr(ppMetaCommand);
    return FAIL(E_INVALIDARG);
  }

  HRESULT STDMETHODCALLTYPE
  CreateStateObject(const D3D12_STATE_OBJECT_DESC *pDesc, REFIID riid, void **ppStateObject) {
    return mullion::CreateStateObject(this, pDesc, nullptr, riid, ppStateObject);
  }

  void STDMETHODCALLTYPE
  GetRaytracingAccelerationStructurePrebuildInfo(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *pDesc,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *pInfo
  ) {
    // the sizes are Metal's: the structure's memory holds its header and stands for a Metal structure of that
    // size, and the scratch memory is what Metal builds in
    bool top = pDesc->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    std::vector<WMTAccelerationStructureGeometry> geometries(top ? 0 : std::max(pDesc->NumDescs, 1u));
    auto info = AccelerationStructureInfo(this, *pDesc, geometries.data(), false);
    GetMTLDevice().accelerationStructureSizes(info);
    bool updates = pDesc->Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    *pInfo = {AccelerationStructureSize(info.size), AccelerationStructureSize(info.build_scratch_size),
              updates ? AccelerationStructureSize(info.refit_scratch_size) : 0};
  }

  D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
  CheckDriverMatchingIdentifier(
      D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
      const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *pIdentifierToCheck
  ) {
    if (SerializedDataType != D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE)
      return D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
    auto &ours = kSerializedIdentifier;
    if (memcmp(&pIdentifierToCheck->DriverOpaqueGUID, &ours.DriverOpaqueGUID, sizeof(GUID)))
      return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
    if (memcmp(pIdentifierToCheck->DriverOpaqueVersioningData, ours.DriverOpaqueVersioningData,
               sizeof(ours.DriverOpaqueVersioningData)))
      return D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
    return D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
  }

  /* ID3D12Device6: Metal compiles pipelines when they are created, so there is no background work to steer */
  HRESULT STDMETHODCALLTYPE
  SetBackgroundProcessingMode(
      D3D12_BACKGROUND_PROCESSING_MODE Mode, D3D12_MEASUREMENTS_ACTION MeasurementsAction,
      HANDLE hEventToSignalUponCompletion, WINBOOL *pbFurtherMeasurementsDesired
  ) {
    if (hEventToSignalUponCompletion)
      SetEvent(hEventToSignalUponCompletion);
    if (pbFurtherMeasurementsDesired)
      *pbFurtherMeasurementsDesired = FALSE;
    return S_OK;
  }

  /* ID3D12Device7 */
  HRESULT STDMETHODCALLTYPE
  AddToStateObject(
      const D3D12_STATE_OBJECT_DESC *pAddition, ID3D12StateObject *pStateObjectToGrowFrom, REFIID riid,
      void **ppNewStateObject
  ) {
    return mullion::CreateStateObject(this, pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession1(
      const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *pDesc, REFIID riid, void **ppSession
  ) {
    InitReturnPtr(ppSession);
    return FAIL(E_NOTIMPL);
  }

  /* the resource to create: a sampler feedback map is one R32_UINT texel per mip region of its paired texture's first
     mip. a region is a power of two from 4 texels to half the texture (Sampler Feedback spec, "Mip region
     constraints") */
  static bool
  ResourceDesc(const D3D12_RESOURCE_DESC1 *pDesc1, D3D12_RESOURCE_DESC &desc) {
    if (!pDesc1)
      return false;
    desc = WithoutMipRegion(*pDesc1);
    auto &region = pDesc1->SamplerFeedbackMipRegion;
    if (!IsSamplerFeedback(desc.Format))
      return !region.Width && !region.Height && !region.Depth;
    auto regions = [](UINT64 texels, UINT region) {
      return std::has_single_bit(region) && region >= 4 && region <= texels / 2 ? (texels + region - 1) / region : 0;
    };
    desc.Width = regions(desc.Width, region.Width);
    desc.Height = regions(desc.Height, region.Height);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width && desc.Height;
  }

  /* creates what ResourceDesc makes of a description. a sampler feedback map keeps the description it was asked for */
  template <typename Create>
  HRESULT
  CreateResource(const D3D12_RESOURCE_DESC1 *pDesc, REFIID riid, void **ppResource, Create &&create) {
    D3D12_RESOURCE_DESC desc;

    InitReturnPtr(ppResource);
    if (!ResourceDesc(pDesc, desc))
      return FAIL(E_INVALIDARG);
    if (!IsSamplerFeedback(pDesc->Format) || !ppResource)
      return create(&desc, riid, ppResource);
    Com<ID3D12Resource> map;
    HRESULT hr = create(&desc, IID_PPV_ARGS(&map));
    if (FAILED(hr))
      return hr;
    auto asked = *pDesc;
    // no mip count is the full chain, as of the texture the map is paired with
    if (!asked.MipLevels)
      asked.MipLevels = std::bit_width(std::max<UINT64>(asked.Width, asked.Height));
    static_cast<MTLD3D12Resource *>(map.ptr())->feedback = asked;
    return map->QueryInterface(riid, ppResource);
  }

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo2(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDescsCount,
      const D3D12_RESOURCE_DESC1 *pResourceDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pAllocationInfos
  ) {
    std::vector<D3D12_RESOURCE_DESC> descs(ResourceDescsCount);

    for (unsigned i = 0; i < ResourceDescsCount; i++) {
      if (!ResourceDesc(&pResourceDescs[i], descs[i])) {
        /* the invalid result GetResourceAllocationInfo1 gives */
        descs[i].Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
      }
    }
    return GetResourceAllocationInfo1(__ret, VisibleMask, ResourceDescsCount, descs.data(), pAllocationInfos);
  }

  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo2(
      UINT VisibleMask, UINT ResourceDescsCount, const D3D12_RESOURCE_DESC1 *pResourceDescs,
      D3D12_RESOURCE_ALLOCATION_INFO1 *pAllocationInfos
  ) {
    D3D12_RESOURCE_ALLOCATION_INFO ret;
    return *GetResourceAllocationInfo2(&ret, VisibleMask, ResourceDescsCount, pResourceDescs, pAllocationInfos);
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource2(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
      ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppResource
  ) {
    return CreateResource(pDesc, riid, ppResource, [&](auto desc, REFIID riid, void **ppResource) {
      return CreateCommittedResource1(
          pHeapProps, HeapFlags, desc, InitialState, OptimizedClearValue, pSession, riid, ppResource
      );
    });
  }

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource1(
      ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    return CreateResource(pDesc, riid, ppResource, [&](auto desc, REFIID riid, void **ppResource) {
      return CreatePlacedResource(pHeap, HeapOffset, desc, InitialState, OptimizedClearValue, riid, ppResource);
    });
  }

  /* ID3D12Device10: resources start in a barrier layout instead of a state. format lists require
     RelaxedFormatCastingSupported (VulkanOn12, "Format List Casting") */
  static D3D12_RESOURCE_STATES
  LayoutState(D3D12_BARRIER_LAYOUT Layout) {
    return Layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                        : D3D12_RESOURCE_STATE_COMMON;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource3(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc,
      D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *OptimizedClearValue,
      ID3D12ProtectedResourceSession *pSession, UINT32 NumCastableFormats, DXGI_FORMAT *pCastableFormats, REFIID riid,
      void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (NumCastableFormats)
      return FAIL(E_INVALIDARG);
    return CreateCommittedResource2(
        pHeapProps, HeapFlags, pDesc, LayoutState(InitialLayout), OptimizedClearValue, pSession, riid, ppResource
    );
  }

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource2(
      ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, UINT32 NumCastableFormats, DXGI_FORMAT *pCastableFormats,
      REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (NumCastableFormats)
      return FAIL(E_INVALIDARG);
    return CreatePlacedResource1(
        pHeap, HeapOffset, pDesc, LayoutState(InitialLayout), OptimizedClearValue, riid, ppResource
    );
  }

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource2(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *OptimizedClearValue,
      ID3D12ProtectedResourceSession *pSession, UINT32 NumCastableFormats, DXGI_FORMAT *pCastableFormats, REFIID riid,
      void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (NumCastableFormats)
      return FAIL(E_INVALIDARG);
    return CreateReservedResource1(pDesc, LayoutState(InitialLayout), OptimizedClearValue, pSession, riid, ppResource);
  }

  void STDMETHODCALLTYPE
  CreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *pTargetedResource, ID3D12Resource *pFeedbackResource, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
  ) {
    // without a paired texture the view is a null one, and writes to it do nothing ("Null feedback map binding is
    // permitted")
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, DestDescriptor);
    auto map = static_cast<MTLD3D12Resource *>(pFeedbackResource);
    if (!pTargetedResource || !map || !map->feedback) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC null{};
      Heap->AddUnorderedAccessView(Index, &null);
      return;
    }
    TextureViewDescriptor view{
        .format = WMTPixelFormatR32Uint,
        .type = WMTTextureType2DArray,
        .arraySize = map->feedback->DepthOrArraySize,
    };
    Heap->AddUnorderedAccessView(
        Index, map->texture.ptr(), map->texture->createView(view), 0, 0,
        map->feedback->Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
    );
  }

  void STDMETHODCALLTYPE
  GetCopyableFootprints1(
      const D3D12_RESOURCE_DESC1 *pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes
  ) {
    D3D12_RESOURCE_DESC desc;

    GetCopyableFootprints(
        ResourceDesc(pResourceDesc, desc) ? &desc : nullptr, FirstSubresource, NumSubresources, BaseOffset, pLayouts,
        pNumRows, pRowSizeInBytes, pTotalBytes
    );
  }

  /* ID3D12Device9: no application shader cache sessions (D3D12_FEATURE_SHADER_CACHE reports the kinds that are) */
  HRESULT STDMETHODCALLTYPE
  CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC *pDesc, REFIID riid, void **ppvSession) {
    InitReturnPtr(ppvSession);
    return FAIL(E_NOTIMPL);
  }

  HRESULT STDMETHODCALLTYPE
  ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS Kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS Control) {
    return FAIL(E_NOTIMPL);
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue1(
      const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID CreatorID, REFIID riid, void **ppCommandQueue
  ) {
    return CreateCommandQueue(pDesc, riid, ppCommandQueue);
  }

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo1(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount,
      const D3D12_RESOURCE_DESC *pDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pAllocationInfos
  ) {
    D3D12_RESOURCE_ALLOCATION_INFO1 resource_info;
    bool has_msaa_resource = false;

    __ret->SizeInBytes = 0;
    __ret->Alignment = 1;

    for (unsigned i = 0; i < ResourceDestCount; i++) {
      const D3D12_RESOURCE_DESC *desc = &pDescs[i];
      has_msaa_resource |= desc->SampleDesc.Count > 1;

      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        if (FAILED(ValidateResourceDescs(desc, &heap)))
          goto invalid;
        if (desc->Alignment && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
          WARN("GetResourceAllocationInfo: alignment ", desc->Alignment, " of a buffer");
          goto invalid;
        }
        resource_info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        auto size_and_align = GetMTLDevice().heapBufferSizeAndAlign(desc->Width, {});
        resource_info.SizeInBytes = size_and_align.size;
      } else {
        WMTTextureInfo texture_info{};
        if (FAILED(PopulateWMTTextureInfo(this, texture_info, *desc))) {
          WARN("GetResourceAllocationInfo: texture of dimension ", desc->Dimension, ", format ", desc->Format, ", ",
               desc->Width, "x", desc->Height, "x", desc->DepthOrArraySize, ", ", desc->MipLevels, " levels, ",
               desc->SampleDesc.Count, " samples, layout ", desc->Layout, ", flags ", desc->Flags, " is refused");
          goto invalid;
        }
        MTL_DXGI_FORMAT_DESC format;
        MTLQueryDXGIFormat(GetMTLDevice(), desc->Format, format);
        UINT planes = format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR ? format.PlanarCount : 1;
        resource_info.SizeInBytes = 0;
        resource_info.Alignment = 0;
        for (UINT plane = 0; plane < planes; plane++) {
          if (plane)
            PopulateWMTTextureInfo(this, texture_info, *desc, plane);
          auto size_and_align = GetMTLDevice().heapTextureSizeAndAlign(texture_info);
          resource_info.SizeInBytes = align(resource_info.SizeInBytes, size_and_align.align) + size_and_align.size;
          resource_info.Alignment = std::max<UINT64>(resource_info.Alignment, size_and_align.align);
        }
        auto requested_alignment = desc->Alignment              ? desc->Alignment
                                   : desc->SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                                : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        resource_info.Alignment = std::max(resource_info.Alignment, requested_alignment);
        // an alignment D3D12 has: "Alignment may be one of 0, 4KB, 64KB, or 4MB" (D3D12_RESOURCE_DESC), the next one
        // up from Metal's, which can lie between them where 4KB was asked for
        if (resource_info.Alignment > D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
            resource_info.Alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
          resource_info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        else if (resource_info.Alignment > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
                 resource_info.Alignment < D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
          resource_info.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
      }

      resource_info.SizeInBytes = align(resource_info.SizeInBytes, resource_info.Alignment);
      resource_info.Offset = align(__ret->SizeInBytes, resource_info.Alignment);

      if (pAllocationInfos)
        pAllocationInfos[i] = resource_info;

      __ret->SizeInBytes = resource_info.Offset + resource_info.SizeInBytes;
      __ret->Alignment = std::max(__ret->Alignment, resource_info.Alignment);
    }

    __ret->SizeInBytes = align(__ret->SizeInBytes, __ret->Alignment);
    return __ret;

  invalid:

    __ret->SizeInBytes = ~(uint64_t)0;
    __ret->Alignment = has_msaa_resource ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                         : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return __ret;
  }

  // committing the set is a kernel call: creating or releasing a resource only changes it, and the queues commit
  // the changes before the next command buffer, the first GPU work that can use them
  void
  CommitResidency() final {
    std::unique_lock<mullion::mutex> lock(residency_lock_);
    if (std::exchange(residency_dirty_, false))
      residency_set_.commit();
  }

  WMT::ResidencySet
  GetGlobalResidencySet() {
    return residency_set_;
  };

  bool
  NamesPasses() {
    static const bool names = diag::errors || diag::profile || trace::Heavy(trace::Pass);
    return names;
  }

  void
  NamePass(const EncoderData *pass, const std::string &pipeline, obj_handle_t metal) {
    std::lock_guard<mullion::mutex> lock(pass_names_lock_);
    auto &name = pass_names_[pass];
    if (diag::profile && !name.empty())
      name += '|';
    name += pipeline;
    if (diag::profile)
      pass_pipelines_[pass][metal] = pipeline;
  }

  std::string
  PassName(const EncoderData *pass, bool consume, obj_handle_t metal) {
    std::lock_guard<mullion::mutex> lock(pass_names_lock_);
    if (metal) {
      auto pass_name = pass_pipelines_.find(pass);
      if (pass_name == pass_pipelines_.end())
        return {};
      auto name = pass_name->second.find(metal);
      return name == pass_name->second.end() ? std::string() : name->second;
    }
    if (!consume) {
      auto name = pass_names_.find(pass);
      return name == pass_names_.end() ? std::string() : name->second;
    }
    pass_pipelines_.erase(pass);
    auto name = pass_names_.extract(pass);
    return name ? std::move(name.mapped()) : std::string();
  }

  void
  PipelineMade(const std::string &name, const char *kind, std::chrono::steady_clock::time_point start) {
    auto end = std::chrono::steady_clock::now(), present = first_present.load();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    bool late = present != std::chrono::steady_clock::time_point() && end >= present;
    // MULLION_TRACE_SHADER: 1 has the pipelines that take 20 ms or more or are made after the first present (a
    // stutter), heavy every one
    if (late || ms >= 20 || trace::Heavy(trace::Shader))
      TRACE_IN(Shader, kind, " pipeline ", name, ": ", ms, " ms", late ? ", after the first present" : "");
    if (!late)
      return;
    std::lock_guard<mullion::mutex> lock(pass_names_lock_);
    late_pipelines_++;
    pipeline_ms_ += ms;
    longest_pipeline_ms_ = std::max(longest_pipeline_ms_, ms);
    Logger::info(str::format("D3D12 pipeline made: ", kind, "; ", name, "; ",
        std::setprecision(std::numeric_limits<double>::max_digits10), ms, " ms"));
  }

  void
  ReportPipelineTotals() {
    std::lock_guard<mullion::mutex> lock(pass_names_lock_);
    Logger::info(str::format("D3D12 pipeline totals: ", late_pipelines_, " after first Present; ",
        std::setprecision(std::numeric_limits<double>::max_digits10), pipeline_ms_, " ms total; ",
        longest_pipeline_ms_, " ms longest"));
  }

  HRESULT
  RegisterResidency(WMT::Allocation allocation) {
    std::unique_lock<mullion::mutex> lock(residency_lock_);
    residency_set_.addAllocations(&allocation, 1);
    residency_dirty_ = true;
    return S_OK;
  }

  HRESULT
  UnregisterResidency(WMT::Allocation allocation) {
    std::unique_lock<mullion::mutex> lock(residency_lock_);
    residency_set_.removeAllocations(&allocation, 1);
    residency_dirty_ = true;
    return S_OK;
  }

  HRESULT
  RegisterResidencyAndVA(BufferAllocation *allocation) {
    {
      std::unique_lock<mullion::shared_mutex> lock(va_lock_);
      interval_map_.emplace(allocation->gpuAddress(), allocation);
      if (allocation->sparse_tile_bytes)
        sparse_buffer_mappings_.emplace(
            allocation->gpuAddress(), std::vector<uint64_t>(allocation->sparse_tile_count)
        );
    }
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    std::unique_lock<mullion::mutex> lock(residency_lock_);
    auto buffer = allocation->buffer();
    residency_set_.addAllocations(&buffer, 1);
    residency_dirty_ = true;
    return S_OK;
  }

  HRESULT
  UnregisterResidencyAndVA(BufferAllocation *allocation) {
    // the buffer's acceleration structures, up to the next buffer's address, end with it and after the locks: they
    // give back what they hold
    std::vector<std::shared_ptr<AccelerationStructure>> ended;
    std::unique_lock<mullion::shared_mutex> va_lock(va_lock_);
    auto next = interval_map_.upper_bound(allocation->gpuAddress());
    {
      std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
      auto first = acceleration_structures_.lower_bound(allocation->gpuAddress());
      auto last = next == interval_map_.end() ? acceleration_structures_.end()
                                              : acceleration_structures_.lower_bound(next->first);
      for (auto it = first; it != last; ++it)
        ended.push_back(std::move(it->second));
      acceleration_structures_.erase(first, last);
    }
    interval_map_.erase(allocation->gpuAddress());
    sparse_buffer_mappings_.erase(allocation->gpuAddress());
    va_lock.unlock();
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    std::unique_lock<mullion::mutex> lock(residency_lock_);
    auto buffer = allocation->buffer();
    residency_set_.removeAllocations(&buffer, 1);
    residency_dirty_ = true;
    return S_OK;
  }

  BufferAllocation *
  LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset, uint64_t *pEnd) {
    std::shared_lock<mullion::shared_mutex> lock(va_lock_);
    auto iter = interval_map_.upper_bound(VA);
    if (pEnd)
      *pEnd = iter == interval_map_.end() ? ~0ull : iter->first;
    if (iter == interval_map_.begin()) {
      *pOffset = 0;
      return {};
    }
    --iter;
    if (pEnd)
      *pEnd = std::min(*pEnd, iter->first + iter->second->info().length);
    *pOffset = VA - iter->first;
    return iter->second;
  }

  void
  UpdateSparseBufferMappings(BufferAllocation *buffer, const std::vector<WMTSparseBufferMapping> &ranges) {
    std::unique_lock<mullion::shared_mutex> lock(va_lock_);
    auto &tiles = sparse_buffer_mappings_.at(buffer->gpuAddress());
    for (auto &range : ranges)
      std::fill_n(tiles.begin() + range.first_tile, range.tile_count, range.mode == WMTSparseMappingModeMap);
  }

  void
  CopySparseBufferMappings(
      BufferAllocation *src, BufferAllocation *dst, const std::vector<WMTSparseBufferMappingCopy> &ranges
  ) {
    std::unique_lock<mullion::shared_mutex> lock(va_lock_);
    auto &from = sparse_buffer_mappings_.at(src->gpuAddress());
    auto &to = sparse_buffer_mappings_.at(dst->gpuAddress());
    for (auto &range : ranges)
      memmove(&to[range.destination_tile], &from[range.first_tile], range.tile_count * sizeof(from[0]));
  }

  bool
  HasSparseBuffers() {
    std::shared_lock<mullion::shared_mutex> lock(va_lock_);
    return !sparse_buffer_mappings_.empty();
  }

  std::vector<uint64_t>
  SparseBufferMappings() {
    std::shared_lock<mullion::shared_mutex> lock(va_lock_);
    if (sparse_buffer_mappings_.empty())
      return {};
    std::vector<uint64_t> words{sparse_buffer_mappings_.size()};
    for (auto &[address, tiles] : sparse_buffer_mappings_) {
      SM50_SPARSE_BUFFER_MAPPING header{address, tiles.size(), interval_map_.at(address)->sparse_tile_bytes};
      auto first = words.size();
      words.resize(first + sizeof(header) / sizeof(words[0]));
      memcpy(&words[first], &header, sizeof(header));
      words.insert(words.end(), tiles.begin(), tiles.end());
    }
    return words;
  }

  std::shared_ptr<AccelerationStructure>
  LookupAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS VA) {
    std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
    auto it = acceleration_structures_.find(VA);
    return it == acceleration_structures_.end() ? nullptr : it->second;
  }

  void
  SetAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS VA, std::shared_ptr<AccelerationStructure> Structure) {
    std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
    // the one replaced ends after the lock: it gives back what it holds
    std::swap(acceleration_structures_[VA], Structure);
    lock.unlock();
  }

  void
  AllocateCompactedSize(CompactedSize &Size) {
    std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
    if (Size.value)
      return;
    if (free_compacted_sizes_.empty()) {
      // a block of them, to not pay a page for each
      constexpr uint64_t block = 1 << 16;
      WMTBufferInfo info{block, WMTResourceStorageModeShared};
      auto &buffer = compacted_size_buffers_.emplace_back(GetMTLDevice().newBuffer(info));
      for (uint64_t offset = 0; offset < info.length; offset += sizeof(uint64_t))
        free_compacted_sizes_.push_back({buffer.handle, offset, (uint64_t *)((char *)info.memory.ptr + offset)});
    }
    Size = free_compacted_sizes_.back();
    free_compacted_sizes_.pop_back();
    *Size.value = 0;
  }

  void
  FreeCompactedSize(const CompactedSize &Size) {
    std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
    free_compacted_sizes_.push_back(Size);
  }

  uint32_t
  AllocateRayFunction() {
    std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
    if (free_ray_functions_.empty())
      return next_ray_function_++;
    auto slot = free_ray_functions_.back();
    free_ray_functions_.pop_back();
    return slot;
  }

  void
  FreeRayFunction(uint32_t Slot) {
    std::unique_lock<mullion::mutex> lock(acceleration_structure_lock_);
    free_ray_functions_.push_back(Slot);
  }

  InternalCommandLibrary &
  GetLib() {
    return command_library;
  }

  WMTSparsePageSize
  GetSparsePageSize() {
    return sparse_page_size_;
  }

  virtual FormatCapability
  GetMTLPixelFormatCapability(WMTPixelFormat Format) final {
    auto original = WMTPixelFormat(ORIGINAL_FORMAT(Format));
    if (!format_inspector_.textureCapabilities.contains(original))
      return FormatCapability(0);
    auto capability = format_inspector_.textureCapabilities.at(original);
    // blending takes the stored channels as they are: rotated ones would blend red with alpha's factors
    if (RenderTargetChannels(Format) != RenderTargetChannels(original))
      capability = capability & ~FormatCapability::Blend;
    return capability;
  };
};

HRESULT
PipelineStateStream::Parse(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc) {
  if (!pDesc || !pDesc->pPipelineStateSubobjectStream)
    return FAIL(E_INVALIDARG);
  const char *stream_start = reinterpret_cast<const char *>(pDesc->pPipelineStateSubobjectStream);
  const char *stream_end = stream_start + pDesc->SizeInBytes;

  {
    desc_graphics.DepthStencilState.DepthEnable = TRUE;
    desc_graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc_graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc_graphics.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc_graphics.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc_graphics.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc_graphics.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc_graphics.DepthStencilState.BackFace = desc_graphics.DepthStencilState.FrontFace;
    desc_graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc_graphics.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc_graphics.RasterizerState.DepthClipEnable = TRUE;
    desc_graphics.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc_graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc_graphics.SampleDesc.Count = 1;
    desc_graphics.SampleDesc.Quality = 0;
    desc_graphics.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    desc_graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  }

  uint32_t defined_type = 0;

  while (stream_start < stream_end) {
    if (stream_start + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > stream_end) {
      ERR("CreatePipelineState: invalid stream");
      return E_INVALIDARG;
    }
    auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(stream_start);

    if ((uint32_t(type) >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID &&
         type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2) || (defined_type & (1 << type))) {
      ERR("CreatePipelineState: unknown or duplicated subobject type ", type);
      return E_INVALIDARG;
    }
    defined_type |= (1 << type);

#define GET_STREAM_DATA(data_type)                                                                                     \
  using subobject_t = struct {                                                                                         \
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;                                                                          \
    data_type data;                                                                                                    \
  };                                                                                                                   \
  auto subobject = reinterpret_cast<subobject_t const *>(stream_start);                                                \
  if (stream_start + sizeof(*subobject) > stream_end) {                                                                \
    ERR("CreatePipelineState: invalid stream");                                                                        \
    return E_INVALIDARG;                                                                                               \
  }                                                                                                                    \
  stream_start += align(sizeof(*subobject), sizeof(void *));

    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
      GET_STREAM_DATA(ID3D12RootSignature *);
      desc_cs.pRootSignature = subobject->data;
      desc_graphics.pRootSignature = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_graphics.VS = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_graphics.PS = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_graphics.DS = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_graphics.HS = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_graphics.GS = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_cs.CS = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
      GET_STREAM_DATA(D3D12_STREAM_OUTPUT_DESC);
      desc_graphics.StreamOutput = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: {
      GET_STREAM_DATA(D3D12_BLEND_DESC);
      desc_graphics.BlendState = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: {
      GET_STREAM_DATA(UINT);
      desc_graphics.SampleMask = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: {
      GET_STREAM_DATA(D3D12_RASTERIZER_DESC);
      desc_graphics.RasterizerState = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
      GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC);
      desc_graphics.DepthStencilState = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
      GET_STREAM_DATA(D3D12_INPUT_LAYOUT_DESC);
      desc_graphics.InputLayout = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: {
      GET_STREAM_DATA(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
      desc_graphics.IBStripCutValue = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: {
      GET_STREAM_DATA(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
      desc_graphics.PrimitiveTopologyType = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
      GET_STREAM_DATA(D3D12_RT_FORMAT_ARRAY);
      memcpy(desc_graphics.RTVFormats, subobject->data.RTFormats, sizeof(desc_graphics.RTVFormats));
      desc_graphics.NumRenderTargets = subobject->data.NumRenderTargets;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: {
      GET_STREAM_DATA(DXGI_FORMAT);
      desc_graphics.DSVFormat = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: {
      GET_STREAM_DATA(DXGI_SAMPLE_DESC);
      desc_graphics.SampleDesc = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: {
      GET_STREAM_DATA(UINT);
      desc_graphics.NodeMask = subobject->data;
      desc_cs.NodeMask = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
      GET_STREAM_DATA(D3D12_CACHED_PIPELINE_STATE);
      desc_graphics.CachedPSO = subobject->data;
      desc_cs.CachedPSO = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: {
      GET_STREAM_DATA(D3D12_PIPELINE_STATE_FLAGS);
      desc_graphics.Flags = subobject->data;
      desc_cs.Flags = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
      GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC1);
      desc_graphics.DepthStencilState.StencilEnable = subobject->data.StencilEnable;
      desc_graphics.DepthStencilState.DepthEnable = subobject->data.DepthEnable;
      desc_graphics.DepthStencilState.DepthFunc = subobject->data.DepthFunc;
      desc_graphics.DepthStencilState.DepthWriteMask = subobject->data.DepthWriteMask;
      desc_graphics.DepthStencilState.StencilWriteMask = subobject->data.StencilWriteMask;
      desc_graphics.DepthStencilState.StencilReadMask = subobject->data.StencilReadMask;
      desc_graphics.DepthStencilState.BackFace = subobject->data.BackFace;
      desc_graphics.DepthStencilState.FrontFace = subobject->data.FrontFace;
      depth_bounds = subobject->data.DepthBoundsTestEnable;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: {
      GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC2);
      depth_stencil2 = subobject->data;
      auto &depth = desc_graphics.DepthStencilState;
      depth.DepthEnable = subobject->data.DepthEnable;
      depth.DepthWriteMask = subobject->data.DepthWriteMask;
      depth.DepthFunc = subobject->data.DepthFunc;
      depth.StencilEnable = subobject->data.StencilEnable;
      depth.StencilReadMask = subobject->data.FrontFace.StencilReadMask;
      depth.StencilWriteMask = subobject->data.FrontFace.StencilWriteMask;
      auto &front = subobject->data.FrontFace;
      auto &back = subobject->data.BackFace;
      depth.FrontFace = {front.StencilFailOp, front.StencilDepthFailOp, front.StencilPassOp, front.StencilFunc};
      depth.BackFace = {back.StencilFailOp, back.StencilDepthFailOp, back.StencilPassOp, back.StencilFunc};
      depth_bounds = subobject->data.DepthBoundsTestEnable;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
      GET_STREAM_DATA(D3D12_VIEW_INSTANCING_DESC);
      // view instancing is unsupported (ViewInstancingTier), so one view, and SV_ViewID is 0
      if (subobject->data.ViewInstanceCount > 1) {
        ERR("CreatePipelineState: ", subobject->data.ViewInstanceCount, " view instances");
        return E_INVALIDARG;
      }
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_as = subobject->data;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      desc_ms = subobject->data;
      break;
    }
    default:
      ERR("CreatePipelineState: unhandled subobject type ", type);
      return E_INVALIDARG;
    }
  }

  // a stream may list every shader, the unused ones empty (as D3DX12's stream helper does): the shaders present
  // choose the pipeline
  if (desc_cs.CS.pShaderBytecode && desc_graphics.VS.pShaderBytecode) {
    ERR("CreatePipelineState: invalid compute pipeline state stream");
    return E_INVALIDARG;
  }

  return S_OK;
}

AccelerationStructure::AccelerationStructure(MTLD3D12Device *device, uint64_t size, uint32_t instance_count) :
    device(device),
    size(size),
    instance_count(instance_count) {
  structure = device->GetMTLDevice().newAccelerationStructure(size, header.structure);
  device->RegisterResidency(structure);
  if (!instance_count)
    return;
  WMTBufferInfo info{instance_count * kAccelerationStructureInstanceSize, WMTResourceStorageModePrivate};
  instances = device->GetMTLDevice().newBuffer(info);
  header.instances = info.gpu_address;
  device->RegisterResidency(instances);
}

AccelerationStructureInputs::~AccelerationStructureInputs() {
  if (data)
    device->UnregisterResidency(data);
}

AccelerationStructure::~AccelerationStructure() {
  device->UnregisterResidency(structure);
  if (instances)
    device->UnregisterResidency(instances);
  if (compacted_size.value)
    device->FreeCompactedSize(compacted_size);
}

HRESULT
CreateD3D12Device(IMTLDXGIAdapter *adapter, const IID &riid, void **ppDevice) {
  auto device = Com(new MTLD3D12DeviceImpl(adapter));
  HRESULT hr = device->Initialize();
  if (FAILED(hr))
    return hr;
  return device->QueryInterface(riid, ppDevice);
};

} // namespace mullion
