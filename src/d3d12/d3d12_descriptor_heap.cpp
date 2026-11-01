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

#include "d3d12_device.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pageable.hpp"
#include "com/com_pointer.hpp"
#include "airconv_public.h"
#include "mullion_sampler.hpp"
#include "log/log.hpp"

namespace mullion {

struct SRVTextureGPUStorage {
  uint64_t resource_id;
  uint64_t metadata;
  uint64_t padding[2];
};

static_assert(D3D12_REQ_MIP_LEVELS <= (1u << SM50_TEXTURE_VIEW_METADATA_FIRST_MIP_BITS));

using UAVTextureGPUStorage = SRVTextureGPUStorage;

struct UAVTexelBufferGPUStorage {
  uint64_t resource_id;
  uint64_t metadata;
  uint64_t padding[2];
};

using SRVTexelBufferGPUStorage = UAVTexelBufferGPUStorage;

struct UAVBufferGPUStorage {
  uint64_t pointer;
  uint64_t metadata;
  uint64_t counter_pointer;
  uint64_t padding;
};

using SRVBufferGPUStorage = UAVBufferGPUStorage;

struct ShaderVisibleDescriptorGPUStorage {
  union {
    SRVTextureGPUStorage SRVTexture;
    CBVCommonStorage ConstantBuffer;
    UAVTextureGPUStorage UAVTexture;
    UAVTexelBufferGPUStorage UAVTexelBuffer;
    UAVBufferGPUStorage UAVBuffer;
    SRVTexelBufferGPUStorage SRVTexelBuffer;
    SRVBufferGPUStorage SRVBuffer;
    std::array<uint64_t, 4> ZeroFilled;
  };

  ShaderVisibleDescriptorGPUStorage();
};

// a GPU handle is the descriptor's address, so both kinds of handle step by its size
static_assert(sizeof(ShaderVisibleDescriptorGPUStorage) == 1 << EMBEDDED_DESCRIPTOR_HANDLE::kIncrementBits);

inline uint64_t
TextureMetadata(uint32_t array_length, float min_lod) {
  return ((uint64_t)array_length << 32) | (uint64_t)std::bit_cast<uint32_t>(min_lod);
}

// the Metal texture type a shader reads a view of `dimension` as: 1D and 2D views as 2D arrays
// (lowering_texture_to_array). an unordered access view's dimensions have the same values
static WMTTextureType
TextureType(D3D12_SRV_DIMENSION dimension) {
  static_assert(UINT(D3D12_UAV_DIMENSION_TEXTURE2DARRAY) == D3D12_SRV_DIMENSION_TEXTURE2DARRAY &&
                UINT(D3D12_UAV_DIMENSION_TEXTURE3D) == D3D12_SRV_DIMENSION_TEXTURE3D);
  switch (dimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
    return WMTTextureTypeTextureBuffer;
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
    return WMTTextureType2DMultisample;
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    return WMTTextureType2DMultisampleArray;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    return WMTTextureType3D;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    return WMTTextureTypeCube;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return WMTTextureTypeCubeArray;
  default:
    return WMTTextureType2DArray;
  }
}

class MTLD3D12DescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12DescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<ShaderVisibleDescriptorCPUStorage> descriptors_;
  Rc<Buffer> buffer_;
  ShaderVisibleDescriptorGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;

public:
  MTLD3D12DescriptorHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12DescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return FAIL(E_INVALIDARG);
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }
    descriptors_.resize(pDesc->NumDescriptors);

    if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      buffer_ = new Buffer(descriptors_.size() * sizeof(ShaderVisibleDescriptorGPUStorage), device_->GetMTLDevice());

      Flags<BufferAllocationFlag> flags;
#ifdef __i386__
      IMPLEMENT_ME
#endif
      buffer_->rename(buffer_->allocate(flags));
      mapped_argument_buffer_ =
          reinterpret_cast<ShaderVisibleDescriptorGPUStorage *>(buffer_->current()->mappedMemory(0));
      argument_buffer_gpu_address_ = buffer_->current()->gpuAddress();
      device_->RegisterResidencyAndVA(buffer_->current());
    } else {
      mapped_argument_buffer_ = reinterpret_cast<ShaderVisibleDescriptorGPUStorage *>(
          malloc(descriptors_.size() * sizeof(ShaderVisibleDescriptorGPUStorage))
      );
    }

    return S_OK;
  };

  ~MTLD3D12DescriptorHeapImpl() {
    if (buffer_) {
      device_->UnregisterResidencyAndVA(buffer_->current());
    } else {
      free(mapped_argument_buffer_);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) {
    *__ret = GetShaderVisibleDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) {
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }

  virtual HRESULT
  AddShaderResourceView(
      UINT Index, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp, std::array<float, 2> BorderRange,
      bool Multisample
  ) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexture;
    cpu_storage.SRVTexture.texture = Texture;
    cpu_storage.SRVTexture.view = View;
    if (mapped_argument_buffer_) {
      auto &texture_view = Texture->view(View);
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.SRVTexture.resource_id = texture_view.gpuResourceID;
      gpu_storage.SRVTexture.metadata = TextureMetadata(Texture->arrayLength(View), ResourceMinLODClamp);
      if (Multisample)
        gpu_storage.SRVTexture.metadata =
            TextureMetadata(Texture->arrayLength(View), 0) |
            (Texture->sampleCount() == 1 ? SM50_TEXTURE_MULTISAMPLE_METADATA_SINGLE : 0) |
            (Texture->centered ? SM50_TEXTURE_MULTISAMPLE_METADATA_CENTERED : 0);
      gpu_storage.SRVTexture.padding[0] = std::bit_cast<uint64_t>(BorderRange);
      // the view's place in its resource, which sampler feedback is recorded for: the first mip and how many mips the
      // resource has, a byte each, then 16 bits each for the first slice and the resource's width and height
      gpu_storage.SRVTexture.padding[1] = View.mip_start | Texture->miplevelCount() << 8 | View.array_start << 16 |
                                          (uint64_t)Texture->width() << 32 | (uint64_t)Texture->height() << 48;
      if (Texture->isSparse() && (Texture->usage() & WMTTextureUsageShaderWrite))
        gpu_storage.SRVTexture.padding[1] |= SM50_TEXTURE_VIEW_METADATA_SPARSE_WRITE;
    }
    return S_OK;
  }
    virtual HRESULT
  AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::ConstantBuffer;
    cpu_storage.ConstantBuffer.address = VA;
    cpu_storage.ConstantBuffer.size = SizeInBytes;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.ConstantBuffer.address = VA && SizeInBytes ? VA : device_->GetNullBuffer();
      gpu_storage.ConstantBuffer.size = SizeInBytes;
    }
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(
      UINT Index, Texture *Texture, TextureViewKey View, UINT FirstWSlice, UINT WSize, bool MinMipFeedback
  ) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexture;
    cpu_storage.UAVTexture.texture = Texture; // 
    cpu_storage.UAVTexture.view = View;
    cpu_storage.UAVTexture.first_slice = FirstWSlice;
    cpu_storage.UAVTexture.slice_count = WSize;
    if (mapped_argument_buffer_) {
      auto &texture_view = Texture->view(View);
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.UAVTexture.resource_id = texture_view.gpuResourceID;
      gpu_storage.UAVTexture.metadata =
          WSize ? (uint64_t)FirstWSlice << 32 | WSize : TextureMetadata(Texture->arrayLength(View), 0) | MinMipFeedback;
      // backing allocation, then first slice and mip as DWORDs, for aliased 64-bit atomics
      gpu_storage.UAVTexture.padding[0] = texture_view.allocation->gpuResourceID;
      gpu_storage.UAVTexture.padding[1] = std::bit_cast<uint64_t>(
          std::array<uint32_t, 2>{uint32_t(View.array_start), uint32_t(View.mip_start)}
      );
    }
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferViewKey View, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexelBuffer;
    cpu_storage.UAVTexelBuffer.buffer = UAVBuffer;
    cpu_storage.UAVTexelBuffer.slice = Slice;
    cpu_storage.UAVTexelBuffer.view = View;
    if (mapped_argument_buffer_) { 
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (UAVBuffer) {
        auto [buffer_view, first] = UAVBuffer->view_(View, UAVBuffer->current(), Slice.firstElement);
        gpu_storage.UAVTexelBuffer.resource_id = buffer_view.gpu_resource_id;
        gpu_storage.UAVTexelBuffer.metadata = ((uint64_t)Slice.elementCount << 32) | first;
        gpu_storage.UAVTexelBuffer.padding[0] = UAVBuffer->current()->gpuAddress();
      } else {
        gpu_storage.UAVTexelBuffer.resource_id = 0;
        gpu_storage.UAVTexelBuffer.metadata = 0;
        gpu_storage.UAVTexelBuffer.padding[0] = 0;
      }
    }
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT CounterOffsetInBytes) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::UAVBuffer;
    cpu_storage.UAVBuffer.buffer = UAVBuffer;
    cpu_storage.UAVBuffer.slice = Slice;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (UAVBuffer) {
        gpu_storage.UAVBuffer.pointer = UAVBuffer->current()->gpuAddress() + Slice.byteOffset;
        gpu_storage.UAVBuffer.metadata = Slice.byteLength;
        gpu_storage.UAVBuffer.counter_pointer = Counter ? Counter->current()->gpuAddress() + CounterOffsetInBytes : 0;
      } else {
        gpu_storage.UAVBuffer.pointer = device_->GetNullBuffer();
        gpu_storage.UAVBuffer.metadata = 0;
        gpu_storage.UAVBuffer.counter_pointer = 0;
      }
    }
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexelBuffer;
    cpu_storage.SRVTexelBuffer.buffer = Buffer;
    cpu_storage.SRVTexelBuffer.slice = Slice;
    cpu_storage.SRVTexelBuffer.view = View;
    if (mapped_argument_buffer_) { 
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (Buffer) {
        auto [buffer_view, first] = Buffer->view_(View, Buffer->current(), Slice.firstElement);
        gpu_storage.UAVTexelBuffer.resource_id = buffer_view.gpu_resource_id;
        gpu_storage.UAVTexelBuffer.metadata = ((uint64_t)Slice.elementCount << 32) | first;
      } else {
        gpu_storage.UAVTexelBuffer.resource_id = 0;
        gpu_storage.UAVTexelBuffer.metadata = 0;
      }
    }
    return S_OK;
  }

  virtual HRESULT
  AddAccelerationStructureView(UINT Index, UINT64 VA) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    descriptors_[Index].type = ShaderVisibleDescriptorType::Null;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.SRVBuffer.pointer = VA;
      gpu_storage.SRVBuffer.metadata = 0;
    }
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::SRVBuffer;
    cpu_storage.SRVBuffer.buffer = Buffer;
    cpu_storage.SRVBuffer.slice = Slice;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (Buffer) {
        gpu_storage.SRVBuffer.pointer = Buffer->current()->gpuAddress() + Slice.byteOffset;
        gpu_storage.SRVBuffer.metadata = Slice.byteLength;
      } else {
        gpu_storage.SRVBuffer.pointer = device_->GetNullBuffer();
        gpu_storage.SRVBuffer.metadata = 0;
      }
    }
    return S_OK;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, D3D12_SHADER_RESOURCE_VIEW_DESC const *pDesc) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    if (!pDesc)
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::Null;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.ZeroFilled = {{}};
      if (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
          (pDesc->Format == DXGI_FORMAT_UNKNOWN || pDesc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW))
        gpu_storage.SRVBuffer.pointer = device_->GetNullBuffer();
      else
        AddUnboundTexture(gpu_storage, TextureType(pDesc->ViewDimension), pDesc->Format, false);
    }
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, D3D12_UNORDERED_ACCESS_VIEW_DESC const *pDesc) {
    if (Index >= descriptors_.size())
      return FAIL(E_INVALIDARG);
    if (!pDesc)
      return FAIL(E_INVALIDARG);
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::Null;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.ZeroFilled = {{}};
      if (pDesc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER &&
          (pDesc->Format == DXGI_FORMAT_UNKNOWN || pDesc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW))
        gpu_storage.UAVBuffer.pointer = device_->GetNullBuffer();
      else
        AddUnboundTexture(gpu_storage, TextureType(D3D12_SRV_DIMENSION(pDesc->ViewDimension)), pDesc->Format, true);
    }
    return S_OK;
  }

  // a null texture view's stand-in, of `type` and of the type a shader reads `Format` as
  // (InternalCommandLibrary::UnboundTexture). a texture and a texel buffer descriptor have one layout
  void
  AddUnboundTexture(ShaderVisibleDescriptorGPUStorage &gpu_storage, WMTTextureType type, DXGI_FORMAT Format, bool write) {
    static_assert(sizeof(SRVTextureGPUStorage) == sizeof(UAVTexelBufferGPUStorage));
    MTL_DXGI_FORMAT_DESC format;
    auto pixel = SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format))
                     ? WMTPixelFormat(ORIGINAL_FORMAT(format.PixelFormat))
                     : WMTPixelFormatInvalid;
    auto sample = !IsIntegerFormat(pixel)                       ? SM50_SAMPLE_FLOAT
                  : MTLGetUnsignedIntegerFormat(pixel) == pixel ? SM50_SAMPLE_UINT
                                                                : SM50_SAMPLE_INT;
    auto stand_in = device_->GetLib().UnboundTexture(type, sample, write);
    if (stand_in.fresh) {
      device_->RegisterResidency(stand_in.texture);
      if (stand_in.backing)
        device_->RegisterResidency(stand_in.backing);
    }
    gpu_storage.SRVTexture.resource_id = stand_in.id;
    gpu_storage.SRVTexture.metadata = SM50_TEXTURE_METADATA_UNBOUND;
  }

  virtual ShaderVisibleDescriptorCPUStorage const &
  GetDescriptor(UINT Index) {
    return descriptors_[Index];
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12DescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) {
    for (unsigned i = 0; i < CopyCount; i++) {
      static_cast<MTLD3D12DescriptorHeapImpl *>(pHeapTo)->descriptors_[DescriptorTo + i] = descriptors_[From + i];
      static_cast<MTLD3D12DescriptorHeapImpl *>(pHeapTo)->mapped_argument_buffer_[DescriptorTo + i] =
          mapped_argument_buffer_[From + i];
    }
  }
};

class MTLD3D12RenderTargetDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<MTL_RENDER_TARGET_DESC> render_targets_;

public:
  MTLD3D12RenderTargetDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return FAIL(E_INVALIDARG);
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: {
      if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        return FAIL(E_INVALIDARG);
      render_targets_.resize(pDesc->NumDescriptors);
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) {
    *__ret = GetRenderTargetDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) {
    __ret->ptr = 0;
    return __ret;
  }

  virtual HRESULT
  AddRenderTarget(UINT Index, MTL_RENDER_TARGET_DESC const *pDesc) {
    if (Index >= render_targets_.size())
      return FAIL(E_INVALIDARG);
    if (pDesc)
      render_targets_[Index] = *pDesc;
    else
      render_targets_[Index] = {};
    return S_OK;
  }

  virtual MTL_RENDER_TARGET_DESC
  GetRenderTarget(UINT Index) {
    return render_targets_[Index];
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12RenderTargetDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) {
    for (unsigned i = 0; i < CopyCount; i++) {
      static_cast<MTLD3D12RenderTargetDescriptorHeapImpl *>(pHeapTo)->render_targets_[DescriptorTo + i] =
          render_targets_[From + i];
    }
  }
};

struct SamplerGPUStorage {
  uint64_t sampler;
  uint64_t cube_sampler;
  uint64_t metadata;
  uint64_t border;
};

static_assert(sizeof(SamplerGPUStorage) == sizeof(SM50_SAMPLER_BORDER));

class MTLD3D12SamplerDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<Rc<Sampler>> samplers_;

  Rc<Buffer> buffer_;
  SamplerGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;
  // the descriptors' borders follow them in the same memory
  SM50_SAMPLER_BORDER *borders_ = nullptr;

  void
  SetBorder(UINT Index, const SM50_SAMPLER_BORDER &Border) {
    borders_[Index] = Border;
    mapped_argument_buffer_[Index].border =
        Border.white_sampler && argument_buffer_gpu_address_
            ? argument_buffer_gpu_address_ + (uintptr_t)&borders_[Index] - (uintptr_t)mapped_argument_buffer_
            : 0;
  }

public:
  MTLD3D12SamplerDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return FAIL(E_INVALIDARG);
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }
    samplers_.resize(pDesc->NumDescriptors);

    if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      buffer_ = new Buffer(samplers_.size() * 2 * sizeof(SamplerGPUStorage), device_->GetMTLDevice());

      Flags<BufferAllocationFlag> flags;
#ifdef __i386__
      IMPLEMENT_ME
#endif
      buffer_->rename(buffer_->allocate(flags));
      mapped_argument_buffer_ = reinterpret_cast<SamplerGPUStorage *>(buffer_->current()->mappedMemory(0));
      argument_buffer_gpu_address_ = buffer_->current()->gpuAddress();
      // FIXME: is residency required for descriptor heap? Should be the case for Metal 4
      device_->RegisterResidencyAndVA(buffer_->current());
    } else {
      mapped_argument_buffer_ =
          reinterpret_cast<SamplerGPUStorage *>(malloc(samplers_.size() * 2 * sizeof(SamplerGPUStorage)));
    }
    borders_ = reinterpret_cast<SM50_SAMPLER_BORDER *>(mapped_argument_buffer_ + samplers_.size());

    return S_OK;
  };

  ~MTLD3D12SamplerDescriptorHeapImpl() {
     if (buffer_) {
      device_->UnregisterResidencyAndVA(buffer_->current());
    } else {
      free(mapped_argument_buffer_);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) {
    *__ret = GetSamplerDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) {
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }


  virtual HRESULT
  AddSampler(UINT Index, const D3D12_SAMPLER_DESC *pDesc) {
    if (!pDesc)
      return FAIL(E_INVALIDARG);
    if (Index >= samplers_.size())
      return FAIL(E_INVALIDARG);
  
    WMTSamplerInfo info;
    bool custom = PopulateWMTSamplerInfo(device_->GetMTLDevice(), info, *pDesc);
    if (custom)
      device_->custom_borders = true;
    auto sampler = Sampler::createSampler(
        device_->GetMTLDevice(), info, pDesc->MipLODBias, custom ? pDesc->BorderColor : nullptr,
        SamplerReduction(pDesc->Filter)
    );

    samplers_[Index] = sampler;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.sampler = sampler->sampler_state_handle;
      gpu_storage.cube_sampler = sampler->sampler_state_cube_handle;
      gpu_storage.metadata = sampler->metadata;
      SM50_SAMPLER_BORDER border{sampler->sampler_state_white_handle};
      std::copy_n(sampler->border_color, 4, border.color);
      SetBorder(Index, border);
    }

    return S_OK;
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12SamplerDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) {
    auto to = static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(pHeapTo);
    for (unsigned i = 0; i < CopyCount; i++) {
      to->samplers_[DescriptorTo + i] = samplers_[From + i];
      to->mapped_argument_buffer_[DescriptorTo + i] = mapped_argument_buffer_[From + i];
      to->SetBorder(DescriptorTo + i, borders_[From + i]);
    }
  }
};

HRESULT
CreateDescriptorHeap(
    MTLD3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap
) {
  InitReturnPtr(ppDescriptorHeap);
  if (!pDesc)
    return FAIL(E_INVALIDARG);
  if (pDesc->NumDescriptors >> EMBEDDED_DESCRIPTOR_HANDLE::kDescriptorBits) {
    ERR("CreateDescriptorHeap: ", pDesc->NumDescriptors, " descriptors of type ", pDesc->Type, " are too many");
    return E_INVALIDARG;
  }
  switch (pDesc->Type) {
  case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
    auto descriptor_heap = Com(new MTLD3D12DescriptorHeapImpl(pDevice));
    HRESULT hr = descriptor_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return descriptor_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
    auto sampler_heap = Com(new MTLD3D12SamplerDescriptorHeapImpl(pDevice));
    HRESULT hr = sampler_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return sampler_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
  case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: {
    auto descriptor_heap = Com(new MTLD3D12RenderTargetDescriptorHeapImpl(pDevice));
    HRESULT hr = descriptor_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return descriptor_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  default:
    break;
  }
  return FAIL(E_INVALIDARG);
}

} // namespace mullion