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
#include "airconv_ray.h"
#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "Metal.hpp"
#include "com/com_pointer.hpp"
#include "com/com_object.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_format.hpp"
#include "util_env.hpp"
#include "dxmt_info.hpp"
#include "log/log.hpp"
#include "config/config.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

const GUID kD3D12DeviceDownlevelUUID = {0x74eaee3f, 0x2f4b, 0x476d, {0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10}};

HRESULT PopulateWMTTextureInfo(MTLD3D12Device *Device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc);

template <typename T>
static HRESULT
NotSupported(void *pFeatureData, UINT DataSize) {
  if (DataSize != sizeof(T))
    return E_INVALIDARG;
  *reinterpret_cast<T *>(pFeatureData) = {};
  return S_OK;
}

/* the same for a query whose first member is the node asked about */
template <typename T>
static HRESULT
NotSupportedOnNode(void *pFeatureData, UINT DataSize) {
  if (DataSize != sizeof(T) || reinterpret_cast<T *>(pFeatureData)->NodeIndex)
    return E_INVALIDARG;
  *reinterpret_cast<T *>(pFeatureData) = {};
  return S_OK;
}

class MTLD3D12DeviceImpl : public MTLD3D12Object<ComObject<MTLD3D12Device>> {
  uint32_t simd_width_ = 0, total_lanes_ = 0;
  WMT::Reference<WMT::Buffer> atomic_locks_;
  uint64_t atomic_locks_address_ = 0;
  uint32_t *atomic_locks_given_up_ = nullptr;
  WMTSparsePageSize sparse_page_size_ = {};

  Com<IMTLDXGIAdapter> adapter_;

  bool advertise_numa_ = false;

  dxmt::mutex residency_lock_;
  std::atomic<HRESULT> removed_ = S_OK;
  dxmt::mutex pass_names_lock_;
  std::unordered_map<uint64_t, std::string> pass_names_;
  dxmt::mutex acceleration_structure_lock_;
  std::map<uint64_t, std::shared_ptr<AccelerationStructure>> acceleration_structures_;
  // compacted sizes are uint64s of shared buffers the GPU writes
  std::vector<WMT::Reference<WMT::Buffer>> compacted_size_buffers_;
  std::vector<CompactedSize> free_compacted_sizes_;
  // ray tracing shaders' table slots: the ones freed, and the next new one
  std::vector<uint32_t> free_ray_functions_;
  uint32_t next_ray_function_ = SM50_RAY_FUNCTION_FIRST_SHADER;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  std::map<uint64_t, BufferAllocation *> interval_map_;
  // what a null acceleration structure is to a ray query: a top-level structure of nothing
  std::unique_ptr<AccelerationStructure> null_scene_;

  InternalCommandLibrary command_library;
  FormatCapabilityInspector format_inspector_;

public:
  MTLD3D12DeviceImpl(IMTLDXGIAdapter *adapter) : adapter_(adapter), command_library(adapter_->GetMTLDevice()) {}

  ~MTLD3D12DeviceImpl() {}

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
    predicate = GetMTLDevice().newComputePipelineState(command_library.getLibrary().newFunction("predicate"), err);
    acceleration_structure_instances = GetMTLDevice().newComputePipelineState(
        command_library.getLibrary().newFunction("acceleration_structure_instances"), err
    );
    if (!occlusion_sum || !predicate || !acceleration_structure_instances) {
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

  void
  CheckAtomicLocks() {
    if (__atomic_exchange_n(atomic_locks_given_up_, 0u, __ATOMIC_RELAXED))
      ERR("A 64-bit atomic gave up waiting for its lock, and its result is wrong: a lock holder did not run again");
  }

  uint32_t
  GetSIMDWidth() {
    return simd_width_;
  }

  uint64_t
  ThreadsAtOnce() {
    // DXMT_D3D12_THREADS_AT_ONCE gives the number, for a test or to tell what a part's size does
    static const uint64_t given = strtoull(env::getEnvVar("DXMT_D3D12_THREADS_AT_ONCE").c_str(), nullptr, 10);
    return given ? given : total_lanes_;
  }

  SM50_SHADER_METAL_VERSION
  GetMetalVersion() {
    static_assert(SM50_SHADER_METAL_310 == uint32_t(WMTMetal310) && SM50_SHADER_METAL_320 == uint32_t(WMTMetal320));
    return SM50_SHADER_METAL_VERSION(ShaderMetalVersion(GetMTLDevice()));
  }

  WMT::Device
  GetMTLDevice() {
    return adapter_->GetMTLDevice();
  };

  D3D_FEATURE_LEVEL
  GetFeatureLevel() {
    return D3D_FEATURE_LEVEL_11_0; // FIXME
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
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device) ||
        riid == __uuidof(ID3D12Device1) || riid == __uuidof(ID3D12Device2) || riid == __uuidof(ID3D12Device3) ||
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5) || riid == __uuidof(ID3D12Device6) ||
        riid == __uuidof(ID3D12Device7) || riid == __uuidof(ID3D12Device8) || riid == __uuidof(ID3D12Device9) ||
        riid == __uuidof(ID3D12Device10)) {
      *ppvObject = ref(this);
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

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
    if (pDesc->Flags)
      WARN("CreateCommandQueue: flags ignored: ", pDesc->Flags);
    return dxmt::CreateCommandQueue(this, pDesc, riid, ppCommandQueue);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
    return dxmt::CreateCommandAllocator(this, Type, riid, ppCommandAllocator);
  };

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    return dxmt::CreateGraphicsPipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    return dxmt::CreateComputePipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator *pCommandAllocator,
      ID3D12PipelineState *pInitialPipelineState, REFIID riid, void **ppCommandList
  ) {
    if (!pCommandAllocator)
      return E_INVALIDARG;
    auto allocator = static_cast<MTLD3D12CommandAllocator *>(pCommandAllocator);
    return allocator->CreateCommandList(NodeMask, Type, pInitialPipelineState, riid, ppCommandList);
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D12_FEATURE Feature, void *pFeatureData, UINT DataSize) {
    auto metal = GetMTLDevice();
    switch (Feature) {
    case D3D12_FEATURE_ARCHITECTURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(pFeatureData);
      if (out->NodeIndex > 0)
        return E_INVALIDARG;
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(pFeatureData);
      if (out->NodeIndex > 0)
        return E_INVALIDARG;
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      out->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(pFeatureData);

      if (out->SampleCount == 0) {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = 0;
        return E_FAIL;
      }

      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->NumQualityLevels = out->SampleCount == 0 ? 1 : 0;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format_desc;
      HRESULT hr = MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (SUCCEEDED(hr) && out->SampleCount) {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = metal.supportsTextureSampleCount(out->SampleCount) ? 1 : 0;
      } else {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = 0;
        return E_FAIL;
      }
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(pFeatureData);
      /* the highest version both know: newer ones (1.2) come down to 1.1, as the runtime answers */
      if (out->HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1)
        return E_INVALIDARG;
      out->HighestVersion = std::min(out->HighestVersion, D3D_ROOT_SIGNATURE_VERSION_1_1);
      return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(pFeatureData);
      if (!out->NumFeatureLevels)
        return E_INVALIDARG;
      // the highest level the options make up, as D3D12 defines the levels: 11_1 with logic ops, 12_0 with tiled
      // resources tier 2, binding tier 2 and typed UAV loads, 12_1 with ROVs and conservative rasterization
      D3D12_FEATURE_DATA_D3D12_OPTIONS o;
      CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
      auto max_level = !o.OutputMergerLogicOp ? D3D_FEATURE_LEVEL_11_0
                       : o.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2 ||
                               o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2 ||
                               !o.TypedUAVLoadAdditionalFormats
                           ? D3D_FEATURE_LEVEL_11_1
                       : !o.ROVsSupported || !o.ConservativeRasterizationTier ? D3D_FEATURE_LEVEL_12_0
                                                                             : D3D_FEATURE_LEVEL_12_1;
      out->MaxSupportedFeatureLevel = {};
      for (unsigned i = 0; i < out->NumFeatureLevels; i++)
        if (out->pFeatureLevelsRequested[i] <= max_level)
          out->MaxSupportedFeatureLevel = std::max(out->MaxSupportedFeatureLevel, out->pFeatureLevelsRequested[i]);
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO:  {
       if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(pFeatureData);
      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->PlaneCount = 1;
        return S_OK;
      }
      MTL_DXGI_FORMAT_DESC format_desc;
      HRESULT hr = MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (FAILED(hr))
        return E_FAIL;

      out->PlaneCount = format_desc.PlanarCount;
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(pFeatureData);
      out->MaxGPUVirtualAddressBitsPerProcess = 48;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(pFeatureData);
      out->HighestShaderModel = std::min(out->HighestShaderModel, D3D_SHADER_MODEL_6_6);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(pFeatureData);
      out->DoublePrecisionFloatShaderOps = FALSE;
#ifdef DXMT_NO_PRIVATE_API
      out->OutputMergerLogicOp = FALSE;
#else
      out->OutputMergerLogicOp = TRUE;
#endif
      out->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
      // placement sparse resources carry tier 2; tier 3 needs D3D12's standard 3D tile shapes, which Metal's differ from
      out->TiledResourcesTier = sparse_page_size_ ? D3D12_TILED_RESOURCES_TIER_2 : D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      out->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
      out->PSSpecifiedStencilRefSupported = TRUE;
      out->TypedUAVLoadAdditionalFormats = TRUE;
      out->ROVsSupported = TRUE;
      out->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
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
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16 *>(pFeatureData);
      out->GPUUploadHeapSupported = FALSE;    // TODO(d3d12): gpu upload heap
      out->DynamicDepthBiasSupported = FALSE; // TODO(d3d12): ID3D12GraphicsCommandList9::RSSetDepthBias
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(pFeatureData);
      out->DepthBoundsTestSupported = GetMTLDevice().supportsFamily(WMTGPUFamilyApple10);
      out->ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(pFeatureData);
      out->CastingFullyTypedFormatSupported = TRUE;
      out->BarycentricsSupported = FALSE;
      out->CopyQueueTimestampQueriesSupported = FALSE;
      out->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      out->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return E_INVALIDARG;
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
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(pFeatureData);
      out->RelaxedFormatCastingSupported = FALSE;
      // ID3D12Device10's layouts and ID3D12GraphicsCommandList7::Barrier
      out->EnhancedBarriersSupported = TRUE;
      out->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(pFeatureData);
      out->MSAA64KBAlignedTextureSupported = TRUE;
      out->Native16BitShaderOpsSupported = TRUE;
      // TODO(d3d12): revise when d3d12 shared resource is implemented
      out->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(pFeatureData);
      out->SRVOnlyTiledResourceTier3 = FALSE;
      out->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      out->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(pFeatureData);
      out->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
      // 1.0 would need the first mip and slice of a paired texture's view in the shader
      out->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_1_0;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(pFeatureData);
      out->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_INPROC_CACHE |
                          D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureData);
      // D3D12_FORMAT_SUPPORT1 and 2 share D3D11's bit values
      return MTLQueryDXGIFormatSupport(
          GetMTLDevice(), out->Format, [this](WMTPixelFormat f) { return GetMTLPixelFormatCapability(f); },
          *reinterpret_cast<uint32_t *>(&out->Support1), *reinterpret_cast<uint32_t *>(&out->Support2)
      );
    }
    // 64-bit atomics on typed resources, groupshared memory and descriptor heap resources, which the shader compiler
    // builds from locks as it does on root buffers
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS9>(pFeatureData, DataSize) != S_OK)
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9 *>(pFeatureData);
      out->AtomicInt64OnTypedResourceSupported = TRUE;
      out->AtomicInt64OnGroupSharedSupported = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS11: {
      if (NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS11>(pFeatureData, DataSize) != S_OK)
        return E_INVALIDARG;
      reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11 *>(pFeatureData)->AtomicInt64OnDescriptorHeapResourceSupported = TRUE;
      return S_OK;
    }
    /* capabilities with no complete path yet: D3D12 defines an all-zero answer as unsupported, or tier 0, in each */
    case D3D12_FEATURE_D3D12_OPTIONS6:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS6>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS8:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS8>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS10:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS10>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS13:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS13>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS14:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS14>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS15:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS15>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS17:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS17>(pFeatureData, DataSize);
    case D3D12_FEATURE_D3D12_OPTIONS18:
      return NotSupported<D3D12_FEATURE_DATA_D3D12_OPTIONS18>(pFeatureData, DataSize);
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
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES))
        return E_INVALIDARG;
      auto *in = reinterpret_cast<D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES *>(pFeatureData);
      /* there are no types, so the count asked for must be 0 */
      return in->NodeIndex || in->Count ? E_INVALIDARG : S_OK;
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
    return dxmt::CreateDescriptorHeap(this, pDesc, riid, ppDescriptorHeap);
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
    return dxmt::CreateRootSignature(this, NodeMask, pBytecode, BytecodeLength, riid, ppRootSignature);
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
    if (!pResource) {
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
    case D3D12_HEAP_TYPE_UPLOAD:
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
      return CreateCommittedTexture(
          this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
      );
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return CreateCommittedBuffer(
          this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
      );
    default:
      break;
    }
    return E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&pDesc->Properties, pDesc->Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    return dxmt::CreateHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource(
      ID3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (!pHeap)
      return E_INVALIDARG;
    auto d3d12heap = static_cast<MTLD3D12Heap *>(pHeap);
    auto heap_desc = d3d12heap->GetDesc();
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&heap_desc.Properties, heap_desc.Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return CreatePlacedTexture(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return CreatePlacedBuffer(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    default:
      break;
    }
    return E_INVALIDARG;
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
    if (HRESULT hr = ValidateResourceStates(InitialState, &heap); FAILED(hr))
      return hr;
    return pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
               ? CreateReservedBuffer(this, pDesc, riid, resource)
               : CreateReservedTexture(this, pDesc, InitialState, riid, resource);
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle
  ) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandle(HANDLE handle, REFIID riid, void **object) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
    return E_NOTIMPL;
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
    return dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

  HRESULT STDMETHODCALLTYPE
  GetDeviceRemovedReason() {
    return removed_;
  };

  void
  LoseDevice() {
    removed_ = DXGI_ERROR_DEVICE_HUNG;
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

        if (FormatDesc.PlanarCount > 1) {
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
        auto Width = align(Extent.right, BlockWidth);
        auto Height = align(Extent.bottom, BlockWidth);
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
    return dxmt::CreateQueryHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  SetStablePowerState(WINBOOL Enable) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandSignature(
      const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature, REFIID riid,
      void **ppCommandSignature
  ) {
    return dxmt::CreateCommandSignature(this, pDesc, pRootSignature, riid, ppCommandSignature);
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
    *ret = std::bit_cast<LUID>(__builtin_bswap64(adapter_->GetMTLDevice().registryID()));
    return ret;
  }

  HRESULT STDMETHODCALLTYPE
  CreatePipelineLibrary(const void *blob, SIZE_T blob_size, REFIID iid, void **lib) {
    // D3D12_FEATURE_SHADER_CACHE reports no library support; this is the answer applications check for
    return DXGI_ERROR_UNSUPPORTED;
  };

  HRESULT STDMETHODCALLTYPE
  SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *pFences, const UINT64 *pValues, UINT FenceCount, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
      HANDLE hEvent
  ) {
    auto set = std::make_shared<std::atomic<bool>>();
    auto done = [=] {
      if (hEvent)
        SetEvent(hEvent);
      *set = true;
      set->notify_all();
    };
    // each fence counts down as it has its value. the count is one when any of them is enough
    auto left = std::make_shared<std::atomic<UINT>>(Flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY ? 1 : FenceCount);
    if (!FenceCount)
      done();
    for (UINT i = 0; i < FenceCount; i++)
      static_cast<MTLD3D12Fence *>(pFences[i])->Expect(pValues[i], [=] {
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
    const char *stream_start = reinterpret_cast<const char *>(pDesc->pPipelineStateSubobjectStream);
    const char *stream_end = stream_start + pDesc->SizeInBytes;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc_cs{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_graphics{};
    D3D12_SHADER_BYTECODE desc_as{}, desc_ms{};
    bool depth_bounds = false;
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
    }

    uint32_t defined_type = 0;

    while (stream_start < stream_end) {
      if (stream_start + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > stream_end) {
        ERR("CreatePipelineState: invalid stream");
        return E_INVALIDARG;
      }
      auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(stream_start);

      if (defined_type & (1 << type)) {
        ERR("CreatePipelineState: duplicated subobejct type ", type);
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
    if (desc_cs.CS.pShaderBytecode) {
      if (desc_graphics.VS.pShaderBytecode) {
        ERR("CreatePipelineState: invalid compute pipeline state stream");
        return E_INVALIDARG;
      }
      return CreateComputePipelineState(&desc_cs, riid, ppPipelineState);
    }

    return dxmt::CreateGraphicsPipelineState(this, &desc_graphics, riid, ppPipelineState, depth_bounds, desc_as, desc_ms);
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromAddress(const void *pAddress, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  EnqueueMakeResident(
      D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable *const *ppObjects, ID3D12Fence *pFence,
      UINT64 FenceValue
  ) {
    // every object is resident while it exists
    return pFence ? pFence->Signal(FenceValue) : E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandList1(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, D3D12_COMMAND_LIST_FLAGS Flags, REFIID riid, void **ppCommandList
  ) {
    if (NodeMask > 1 || Flags != D3D12_COMMAND_LIST_FLAG_NONE)
      return E_INVALIDARG;
    return CreateClosedCommandList(this, Type, riid, ppCommandList);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC *pDesc, REFIID riid, void **ppSession) {
    return E_NOTIMPL;
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
      return E_INVALIDARG;
    return CreateCommittedResource(pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
  CreateHeap1(const D3D12_HEAP_DESC *pDesc, ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppHeap) {
    InitReturnPtr(ppHeap);
    if (pSession)
      return E_INVALIDARG;
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
      return E_INVALIDARG;
    return CreateReservedResource(pDesc, InitialState, OptimizedClearValue, riid, ppResource);
  }

  /* ID3D12Device5: no ray tracing or meta commands yet, as D3D12_FEATURE_D3D12_OPTIONS5 reports */
  HRESULT STDMETHODCALLTYPE
  CreateLifetimeTracker(ID3D12LifetimeOwner *pOwner, REFIID riid, void **ppvTracker) {
    InitReturnPtr(ppvTracker);
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  RemoveDevice() {}

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommands(UINT *pNumMetaCommands, D3D12_META_COMMAND_DESC *pDescs) {
    if (!pNumMetaCommands)
      return E_INVALIDARG;
    *pNumMetaCommands = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommandParameters(
      REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage, UINT *pTotalStructureSizeInBytes,
      UINT *pParameterCount, D3D12_META_COMMAND_PARAMETER_DESC *pParameterDescs
  ) {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateMetaCommand(
      REFGUID CommandId, UINT NodeMask, const void *pCreationParametersData, SIZE_T CreationParametersDataSizeInBytes,
      REFIID riid, void **ppMetaCommand
  ) {
    InitReturnPtr(ppMetaCommand);
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateStateObject(const D3D12_STATE_OBJECT_DESC *pDesc, REFIID riid, void **ppStateObject) {
    return dxmt::CreateStateObject(this, pDesc, nullptr, riid, ppStateObject);
  }

  void STDMETHODCALLTYPE
  GetRaytracingAccelerationStructurePrebuildInfo(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *pDesc,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *pInfo
  ) {
    // the sizes are Metal's: the structure's memory holds its header and stands for a Metal structure of that
    // size, and the scratch memory is what Metal builds in
    bool top = pDesc->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    std::vector<WMTAccelerationStructureGeometry> geometries(top ? 0 : pDesc->NumDescs);
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
    return dxmt::CreateStateObject(this, pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession1(
      const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *pDesc, REFIID riid, void **ppSession
  ) {
    InitReturnPtr(ppSession);
    return E_NOTIMPL;
  }

  /* ID3D12Device8: D3D12_RESOURCE_DESC1 adds only a sampler feedback mip region */
  static D3D12_RESOURCE_DESC
  WithoutMipRegion(const D3D12_RESOURCE_DESC1 &desc1) {
    return {desc1.Dimension, desc1.Alignment, desc1.Width,      desc1.Height, desc1.DepthOrArraySize,
            desc1.MipLevels, desc1.Format,    desc1.SampleDesc, desc1.Layout, desc1.Flags};
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
      return E_INVALIDARG;
    if (!IsSamplerFeedback(pDesc->Format) || !ppResource)
      return create(&desc, riid, ppResource);
    Com<ID3D12Resource> map;
    HRESULT hr = create(&desc, IID_PPV_ARGS(&map));
    if (FAILED(hr))
      return hr;
    auto asked = WithoutMipRegion(*pDesc);
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

  /* ID3D12Device10: resources start in a barrier layout instead of a state. states only check the render target
     flag here, and every texture can be viewed in the formats D3D12 lets it cast to (no relaxed casting) */
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
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS Kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS Control) {
    return E_NOTIMPL;
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
        if (desc->Alignment && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
          DEBUG("GetResourceAllocationInfo: invalid alignment ", desc->Alignment, " for buffer resource.\n");
          goto invalid;
        }
        resource_info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        auto size_and_align = GetMTLDevice().heapBufferSizeAndAlign(desc->Width, {});
        resource_info.SizeInBytes = size_and_align.size;
      } else {
        WMTTextureInfo texture_info{};
        if (FAILED(PopulateWMTTextureInfo(this, texture_info, *desc))) {
          DEBUG("GetResourceAllocationInfo: invalid texture descriptor\n");
          goto invalid;
        }
        auto size_and_align = GetMTLDevice().heapTextureSizeAndAlign(texture_info);
        resource_info.SizeInBytes = size_and_align.size;
        resource_info.Alignment = size_and_align.align;
        auto requested_alignment = desc->Alignment              ? desc->Alignment
                                   : desc->SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                                : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        resource_info.Alignment = std::max(resource_info.Alignment, requested_alignment);
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

  WMT::ResidencySet
  GetGlobalResidencySet() {
    return residency_set_;
  };

  bool
  NamesPasses() {
    static const bool names = !env::getEnvVar("DXMT_D3D12_GPU_ERRORS").empty();
    return names;
  }

  void
  NamePass(uint64_t id, const std::string &pipeline) {
    std::lock_guard<dxmt::mutex> lock(pass_names_lock_);
    pass_names_[id] += pipeline;
  }

  std::string
  PassName(uint64_t id) {
    std::lock_guard<dxmt::mutex> lock(pass_names_lock_);
    auto name = pass_names_.extract(id);
    return name ? std::move(name.mapped()) : std::string();
  }

  HRESULT
  RegisterResidency(WMT::Allocation allocation) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    residency_set_.addAllocations(&allocation, 1);
    residency_set_.commit();
    return S_OK;
  }

  HRESULT
  UnregisterResidency(WMT::Allocation allocation) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    residency_set_.removeAllocations(&allocation, 1);
    residency_set_.commit();
    return S_OK;
  }

  HRESULT
  RegisterResidencyAndVA(BufferAllocation *allocation) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    interval_map_.emplace(allocation->gpuAddress(), allocation);
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    auto buffer = allocation->buffer();
    residency_set_.addAllocations(&buffer, 1);
    residency_set_.commit();
    return S_OK;
  }

  HRESULT
  UnregisterResidencyAndVA(BufferAllocation *allocation) {
    // the buffer's acceleration structures, up to the next buffer's address, end with it and after the locks: they
    // give back what they hold
    std::vector<std::shared_ptr<AccelerationStructure>> ended;
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    auto next = interval_map_.upper_bound(allocation->gpuAddress());
    {
      std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
      auto first = acceleration_structures_.lower_bound(allocation->gpuAddress());
      auto last = next == interval_map_.end() ? acceleration_structures_.end()
                                              : acceleration_structures_.lower_bound(next->first);
      for (auto it = first; it != last; ++it)
        ended.push_back(std::move(it->second));
      acceleration_structures_.erase(first, last);
    }
    interval_map_.erase(allocation->gpuAddress());
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    auto buffer = allocation->buffer();
    residency_set_.removeAllocations(&buffer, 1);
    residency_set_.commit();
    return S_OK;
  }

  BufferAllocation *
  LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    auto iter = interval_map_.upper_bound(VA);
    if (iter == interval_map_.begin()) {
      *pOffset = 0;
      return {};
    }
    --iter;
    *pOffset = VA - iter->first;
    return iter->second;
  }

  std::shared_ptr<AccelerationStructure>
  LookupAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS VA) {
    std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
    auto it = acceleration_structures_.find(VA);
    return it == acceleration_structures_.end() ? nullptr : it->second;
  }

  void
  SetAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS VA, std::shared_ptr<AccelerationStructure> Structure) {
    std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
    // the one replaced ends after the lock: it gives back what it holds
    std::swap(acceleration_structures_[VA], Structure);
    lock.unlock();
  }

  void
  AllocateCompactedSize(CompactedSize &Size) {
    std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
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
    std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
    free_compacted_sizes_.push_back(Size);
  }

  uint32_t
  AllocateRayFunction() {
    std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
    if (free_ray_functions_.empty())
      return next_ray_function_++;
    auto slot = free_ray_functions_.back();
    free_ray_functions_.pop_back();
    return slot;
  }

  void
  FreeRayFunction(uint32_t Slot) {
    std::unique_lock<dxmt::mutex> lock(acceleration_structure_lock_);
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
    Format = ORIGINAL_FORMAT(Format);
    if (!format_inspector_.textureCapabilities.contains(Format))
      return FormatCapability(0);
    return format_inspector_.textureCapabilities.at(Format);
  };
};

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

} // namespace dxmt