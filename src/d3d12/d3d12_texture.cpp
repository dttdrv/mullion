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
#include "d3d12_pageable.hpp"
#include <bit>
#include "mullion_format.hpp"
#include "com/com_pointer.hpp"

namespace mullion {

// the size D3D12 estimates a texture's most-detailed mip level of one slice at, for 4KB alignment: its width and height
// in elements (blocks of a block-compressed format) times the element's bytes. "Windows uses the slice size to
// determine small alignment eligibility. DepthOrArraySize is ignored" (vkd3d-proton, libs/vkd3d/resource.c,
// d3d12_resource_supports_small_resource_alignment, whose tests run on Windows too). UINT64_MAX past 64 bits
static uint64_t
SmallResourceEstimate(const D3D12_RESOURCE_DESC &Desc, const MTL_DXGI_FORMAT_DESC &Format) {
  uint32_t block = Format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
  unsigned __int128 size = (unsigned __int128)Desc.Width * Desc.Height * Format.BytesPerTexel / (block * block);
  return size > UINT64_MAX ? UINT64_MAX : uint64_t(size);
}

HRESULT
PopulateWMTTextureInfo(MTLD3D12Device *Device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc, UINT Plane) {
  MTL_DXGI_FORMAT_DESC Format;
  HRESULT hr = MTLQueryDXGIFormat(Device->GetMTLDevice(), Desc.Format, Format);
  if (FAILED(hr))
    return hr;
  if (Desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      (Desc.SampleDesc.Count != 1 || Desc.SampleDesc.Quality))
    return E_INVALIDARG;

  if (Format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    if (Desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !Desc.Width || !Desc.Height || !Desc.DepthOrArraySize ||
        ((Desc.Width | Desc.Height) & 1) || Desc.MipLevels != 1 || Desc.SampleDesc.Count != 1 ||
        Desc.SampleDesc.Quality || Desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
        (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
      return FAIL(E_INVALIDARG);
    if (Plane != ~0u && FAILED(MTLQueryDXGIFormat(Device->GetMTLDevice(), Desc.Format, Format, Plane)))
      return FAIL(E_INVALIDARG);
  }
  InfoOut.pixel_format = Format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32
                             ? WMTPixelFormat(ORIGINAL_FORMAT(Format.PixelFormat)) : Format.PixelFormat;
  if ((Format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32) &&
      (Desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
    return FAIL(E_INVALIDARG);

  if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
    switch (Format.PixelFormat) {
    case WMTPixelFormatR32Uint:
    case WMTPixelFormatR32Sint:
    case WMTPixelFormatR32Float:
      InfoOut.pixel_format = WMTPixelFormatDepth32Float;
      break;
    case WMTPixelFormatR16Uint:
    case WMTPixelFormatR16Sint:
    case WMTPixelFormatR16Float:
    case WMTPixelFormatR16Unorm:
    case WMTPixelFormatR16Snorm:
      InfoOut.pixel_format = WMTPixelFormatDepth16Unorm;
      break;
    default:
      break;
    }
  }

  // a stencil view's format is not one Metal makes textures in: the texture is the depth stencil one it is a view of
  if (ORIGINAL_FORMAT(InfoOut.pixel_format) == WMTPixelFormatX32_Stencil8)
    InfoOut.pixel_format = WMTPixelFormatDepth32Float_Stencil8;

  // a format makes the textures its row names (Microsoft Learn, "Format support for Direct3D Feature Level 12.0
  // hardware": Texture1D, Texture2D, Texture3D)
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{Desc.Format};
  Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
  if (!(support.Support1 & (Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D   ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                            : Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? D3D12_FORMAT_SUPPORT1_TEXTURE2D
                            : Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                                                                    : D3D12_FORMAT_SUPPORT1_NONE)))
    return FAIL(E_INVALIDARG);

  switch (Desc.Dimension) {
  default:
    return FAIL(E_INVALIDARG);
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    InfoOut.width = Desc.Width;
    InfoOut.height = 1;
    InfoOut.depth = 1;
    InfoOut.array_length = Desc.DepthOrArraySize;
    if (Desc.DepthOrArraySize > 1)
      InfoOut.type = WMTTextureType2DArray;
    else
      InfoOut.type = WMTTextureType2D;
    InfoOut.sample_count = 1;
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    InfoOut.width = Desc.Width;
    InfoOut.height = Desc.Height;
    InfoOut.depth = 1;
    InfoOut.array_length = Desc.DepthOrArraySize;
    if (Desc.SampleDesc.Count != 1 || Desc.SampleDesc.Quality) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{Desc.Format, Desc.SampleDesc.Count};
      if (FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality))) ||
          !quality.NumQualityLevels ||
          (Desc.SampleDesc.Quality >= quality.NumQualityLevels &&
           Desc.SampleDesc.Quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN &&
           Desc.SampleDesc.Quality != DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN))
        return FAIL(E_INVALIDARG);
    }
    if (Desc.SampleDesc.Count > 1) {
      if (Desc.DepthOrArraySize > 1)
        InfoOut.type = WMTTextureType2DMultisampleArray;
      else
        InfoOut.type = WMTTextureType2DMultisample;
      InfoOut.sample_count = Desc.SampleDesc.Count;
    } else {
      if (Desc.DepthOrArraySize > 1)
        InfoOut.type = WMTTextureType2DArray;
      else
        InfoOut.type = WMTTextureType2D;
      InfoOut.sample_count = 1;
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    InfoOut.width = Desc.Width;
    InfoOut.height = Desc.Height;
    InfoOut.depth = Desc.DepthOrArraySize;
    InfoOut.array_length = 1;
    InfoOut.type = WMTTextureType3D;
    InfoOut.sample_count = 1;
    break;
  }
  }

  InfoOut.width /= Format.SubsampleWidth;
  InfoOut.height /= Format.SubsampleHeight;
  InfoOut.mipmap_level_count = 32 - __builtin_clz(InfoOut.width | InfoOut.height | InfoOut.depth);
  if (Desc.MipLevels)
    InfoOut.mipmap_level_count = std::min<uint32_t>(InfoOut.mipmap_level_count, Desc.MipLevels);

  // Metal lays texels out its own way; D3D12's standard swizzle is not one of them
  if (Desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)
    return FAIL(E_INVALIDARG);

  WMTTextureUsage Usage = ORIGINAL_FORMAT(InfoOut.pixel_format) == WMTPixelFormatDepth32Float_Stencil8
                              ? WMTTextureUsagePixelFormatView : WMTTextureUsageUnknown;
  // a view in another layout needs Metal's format view, which takes the texture's lossless compression: only a format
  // D3D12 lets a view cast across layouts gets one (its typeless family, or Relaxed Format Casting's), and never a
  // depth texture, which every view reads as itself
  if ((Format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32) ||
      (!DepthStencilPlanarFlags(WMTPixelFormat(ORIGINAL_FORMAT(InfoOut.pixel_format))) &&
       CastsAcrossLayouts(Device->GetMTLDevice(), Desc.Format, !(Format.Flag & MTL_DXGI_FORMAT_TYPELESS))))
    Usage |= WMTTextureUsagePixelFormatView;
  if (Desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    Usage |= WMTTextureUsageRenderTarget;
  if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    Usage |= WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
    // an R32 view of the texture is a view in another layout (CastsToR32UnorderedAccess)
    if (CastsToR32UnorderedAccess(Desc.Format))
      Usage |= WMTTextureUsagePixelFormatView;
    // and so does a uint clear of a format whose values cannot hold all its bit patterns (GetUIntClearWrite)
    auto pixel = WMTPixelFormat(ORIGINAL_FORMAT(InfoOut.pixel_format));
    if (GetUIntClearWrite(pixel) == UIntClearWrite::IntegerView && MTLGetUnsignedIntegerFormat(pixel) != pixel)
      Usage |= WMTTextureUsagePixelFormatView;
    // Metal runs a shader's atomic functions only on a texture made for them
    if (any_bit_set(Device->GetMTLPixelFormatCapability(InfoOut.pixel_format) & FormatCapability::Atomic))
      Usage |= WMTTextureUsageShaderAtomic;
  }
  // a resolve reads its source's samples in a shader, whatever the resource allows the application's; and so does the
  // depth bounds test of a GPU without one its depth (d3d12_command_list.cpp, DepthBoundsSamples)
  if (!(Desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || InfoOut.sample_count > 1 ||
      ((Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && !Device->GetMTLDevice().supportsFamily(WMTGPUFamilyApple10)))
    Usage |= WMTTextureUsageShaderRead;
  // and renders its destination, which D3D12 lets be any single-sampled texture of a format that resolves
  if (InfoOut.type == WMTTextureType2D || InfoOut.type == WMTTextureType2DArray)
    if (any_bit_set(Device->GetMTLPixelFormatCapability(InfoOut.pixel_format) & FormatCapability::Color))
      Usage |= WMTTextureUsageRenderTarget;
  // and a kernel decodes sampler feedback into any single-sampled 2D texture of the transcode format, whatever its
  // flags (Sampler Feedback, "General semantics" of transcoding)
  if (Desc.Format == DXGI_FORMAT_R8_UINT && (InfoOut.type == WMTTextureType2D || InfoOut.type == WMTTextureType2DArray))
    Usage |= WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
  InfoOut.usage = Usage;

  // TODO: decide storage mode
  InfoOut.options = WMTResourceHazardTrackingModeUntracked;

  if (Desc.Alignment) {
    if (Desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        Desc.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
        (Desc.SampleDesc.Count == 1 || Desc.Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT))
      return FAIL(E_INVALIDARG);

    if (Desc.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
      // Textures with UNKNOWN layout without MSAA and without render-target nor depth-stencil flags may be created with
      // 4KB Alignment
      if (Desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN)
        return FAIL(E_INVALIDARG);
      if (Desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return FAIL(E_INVALIDARG);
      if (Desc.SampleDesc.Count > 1)
        return FAIL(E_INVALIDARG);

      // "applications can create smaller aligned resources when the estimated size of the most-detailed mip level is a
      // total of the larger alignment restriction or less. The runtime will use an architecture-independent mechanism
      // of size-estimation" (D3D12_RESOURCE_DESC, Alignment): D3D12's estimate (SmallResourceEstimate), not Metal's
      // size, which may be larger. GetResourceAllocationInfo then gives Metal's size, at Metal's alignment rounded up
      // to one D3D12 has (Starfield's 320x180 R8_TYPELESS, dttdrv/mullion#32). one plane of a planar format, asked for
      // by itself, was decided with its texture
      if (Plane != ~0u)
        return S_OK;
      if (!(Format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) {
        if (SmallResourceEstimate(Desc, Format) > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
          return FAIL(E_INVALIDARG);
        return S_OK;
      }
      // a planar format's estimate is Metal's size of its planes
      WMTTextureInfo info_one_slice = InfoOut;
      info_one_slice.mipmap_level_count = 1;
      info_one_slice.array_length = 1;
      auto size_and_align = Device->GetMTLDevice().heapTextureSizeAndAlign(info_one_slice);
      MTL_DXGI_FORMAT_DESC chroma;
      MTLQueryDXGIFormat(Device->GetMTLDevice(), Desc.Format, chroma, 1);
      info_one_slice.width /= chroma.SubsampleWidth;
      info_one_slice.height /= chroma.SubsampleHeight;
      info_one_slice.pixel_format = chroma.PixelFormat;
      auto chroma_size = Device->GetMTLDevice().heapTextureSizeAndAlign(info_one_slice);
      if (align(size_and_align.size, chroma_size.align) + chroma_size.size > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        return FAIL(E_INVALIDARG);
    }
  }

  return S_OK;
};

WMTPixelFormat
EncodeComponentMapping(UINT Shader4ComponentMapping, WMTPixelFormat Format) {

#define DX2MTL_SWIZZLE(x) ((x) + 2) % 6

  auto MappingR = DX2MTL_SWIZZLE(Shader4ComponentMapping & 0x7);
  auto MappingG = DX2MTL_SWIZZLE((Shader4ComponentMapping >> 3) & 0x7);
  auto MappingB = DX2MTL_SWIZZLE((Shader4ComponentMapping >> 6) & 0x7);
  auto MappingA = DX2MTL_SWIZZLE((Shader4ComponentMapping >> 9) & 0x7);
  auto DecodedR = GET_SWIZZLE_RED(Format);
  auto DecodedG = GET_SWIZZLE_GREEN(Format);
  auto DecodedB = GET_SWIZZLE_BLUE(Format);
  auto DecodedA = GET_SWIZZLE_ALPHA(Format);
  uint32_t Map[6] = {0, 1, DecodedR, DecodedG, DecodedB, DecodedA};
  return WMTPixelFormat(ENCODE_FORMAT_SWIZZLE(Format, Map[MappingR], Map[MappingG], Map[MappingB], Map[MappingA]));
}

// whether `count` slices from slice `first` lie in the `length` a resource has: a render target or depth stencil view
// that runs past the end is refused, as Metal makes no view and no render pass of slices that are not there
static bool
WithinSlices(UINT first, UINT count, UINT length) {
  return first < length && count <= length - first;
}

class MTLD3D12Texture : public MTLD3D12Pageable<MTLD3D12Resource> {
  D3D12_RESOURCE_DESC desc_;
  D3D12_HEAP_PROPERTIES heap_props_;
  D3D12_HEAP_FLAGS heap_flags_;
  bool reserved_ = false; // its tiles are mapped to heaps (UpdateTileMappings)
  // the heap a placed texture is on, which it keeps: the heap's residency is the texture's
  // ("A placed resource object holds a reference on the heap it is created on", ID3D12Device::CreateHeap)
  Com<MTLD3D12Heap, false> heap_;

public:
  MTLD3D12Texture(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Resource>(pDevice) {}

  HRESULT
  Initialize(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, MTLD3D12Heap *pHeap, UINT64 HeapOffset, bool Reserved = false,
      const MullionMetalResource *pTexture = nullptr
  ) {
    // TODO: validate and normalize
    desc_ = *pDesc;
    reserved_ = Reserved;
    heap_props_ = *pHeapProps;
    heap_flags_ = HeapFlags;
    heap_ = pHeap;

    switch (InitialState) {
    case D3D12_RESOURCE_STATE_RENDER_TARGET: {
      if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        break;
      return FAIL(E_INVALIDARG);
    }
    default:
      break;
    }

    WMTTextureInfo texture_info{};
    HRESULT hr = PopulateWMTTextureInfo(device_, texture_info, desc_, ~0u);
    if (FAILED(hr))
      return hr;

    if (!desc_.MipLevels)
      desc_.MipLevels = texture_info.mipmap_level_count;

    if (Reserved) {
      if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
          desc_.Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE ||
          (DepthStencilPlanarFlags(texture_info.pixel_format) & DepthStencilPlanarFlags(WMTPixelFormatStencil8)))
        return FAIL(E_INVALIDARG);
      if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc_.DepthOrArraySize > 1) {
        auto size = device_->GetMTLDevice().sparseTileSize(
            texture_info.type, texture_info.pixel_format, texture_info.sample_count, device_->GetSparsePageSize()
        );
        auto extent = GetResourceExtent(desc_, texture_info.mipmap_level_count - 1);
        // tier 2 arrays cannot contain packed mips (D3D11.3 5.9.7.2)
        if (extent.right < size.width || extent.bottom < size.height)
          return FAIL(E_INVALIDARG);
      }
    }

    if (!desc_.Alignment) {
      desc_.Alignment = desc_.SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                   : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }

    MTL_DXGI_FORMAT_DESC format;
    MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, format);
    UINT planes = format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR ? format.PlanarCount : 1;
    uint64_t offset = HeapOffset;
    for (UINT plane = 0; plane < planes; plane++) {
      if (plane && FAILED(PopulateWMTTextureInfo(device_, texture_info, desc_, plane)))
        return FAIL(E_INVALIDARG);
      Flags<TextureAllocationFlag> flags = {};
      // a committed texture the CPU never reaches is private: Metal keeps its contents compressed from the start, where
      // a shared texture is so only once the GPU writes it (a placed texture has its heap's storage mode)
      if ((!pHeap && !pTexture && !IsCpuVisibleHeap(pHeapProps)) || (pHeap && pHeap->private_storage))
        flags.set(TextureAllocationFlag::CpuInvisible);
      if (Reserved) {
        if (planes > 1)
          return FAIL(E_INVALIDARG);
        texture_info.placement_sparse_page_size = device_->GetSparsePageSize();
        flags.set(TextureAllocationFlag::CpuInvisible);
      }
      WMTSizeAndAlign size_and_align{};
      if (pHeap) {
        size_and_align = device_->GetMTLDevice().heapTextureSizeAndAlign(texture_info);
        if (planes > 1 && !plane && offset % std::max<uint64_t>(desc_.Alignment, size_and_align.align))
          return FAIL(E_INVALIDARG);
        if (plane)
          offset = align(offset, size_and_align.align);
        if (offset > pHeap->GetDesc().SizeInBytes || size_and_align.size > pHeap->GetDesc().SizeInBytes - offset)
          return FAIL(E_INVALIDARG);
      }
      if (pTexture) {
        if (planes > 1)
          return E_INVALIDARG;
        texture_info = pTexture->texture_info;
      }
      if (pTexture) {
        if (planes != 1)
          return E_INVALIDARG;
        texture_info = pTexture->texture_info;
      }
      Rc<Texture> storage = new Texture(texture_info, device_->GetMTLDevice());
      WMT::Texture adopted;
      adopted.handle = pTexture ? pTexture->texture : NULL_OBJECT_HANDLE;
      storage->rename(pTexture ? storage->wrap(WMT::Reference<WMT::Texture>(adopted), texture_info, pTexture->flags)
                      : pHeap ? storage->allocate(flags, pHeap->heap, offset) : storage->allocate(flags));
      if (!storage->current()->texture())
        return FAIL(E_OUTOFMEMORY);
      if (!pHeap)
        device_->RegisterResidency(storage->current()->texture());
      storage->centered = desc_.SampleDesc.Quality == DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN;
      if (plane)
        texture->chroma = std::move(storage);
      else
        texture = std::move(storage);
      offset += size_and_align.size;
    }

    return S_OK;
  };

  ~MTLD3D12Texture() {
    for (auto storage = texture.ptr(); storage; storage = storage->chroma.ptr())
      if (storage->current() && !storage->current()->flags().test(TextureAllocationFlag::AllocatedOnHeap))
        device_->UnregisterResidency(storage->current()->texture());
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource) || riid == __uuidof(ID3D12Resource1) ||
        riid == __uuidof(ID3D12Resource2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Texture: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  virtual HRESULT STDMETHODCALLTYPE
  Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData) {
    if (!IsCpuVisibleHeap(&heap_props_) || (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        desc_.SampleDesc.Count != 1)
      return FAIL(E_INVALIDARG);
    UINT subresource_count = desc_.MipLevels;
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      subresource_count *= desc_.DepthOrArraySize;
    MTL_DXGI_FORMAT_DESC format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, format)) ||
        !std::has_single_bit(format.BytesPerTexel))
      return FAIL(E_INVALIDARG);
    if (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)
      subresource_count *= format.PlanarCount;
    if (Subresource >= subresource_count)
      return FAIL(E_INVALIDARG);
    if (ppData || (pReadRange && pReadRange->End > pReadRange->Begin) ||
        (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc_.MipLevels > 1))
      return FAIL(E_INVALIDARG);
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange) {};

  virtual D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) {
    *__ret = feedback ? WithoutMipRegion(*feedback) : desc_;
    return __ret;
  };

  virtual D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() {
    return 0;
  };

  virtual HRESULT STDMETHODCALLTYPE
  WriteToSubresource(
      UINT DstSubresource, const D3D12_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcSlicePitch
  ) {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      SrcSlicePitch = 0;
    if ((desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || desc_.SampleDesc.Count != 1 ||
        (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc_.MipLevels > 1))
      return FAIL(E_INVALIDARG);
    uint32_t Level = 0, Slice = 0, Plane = 0;
    DecomposeSubresource(desc_, DstSubresource, &Level, &Slice, &Plane);
    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, Format)) ||
        !std::has_single_bit(Format.BytesPerTexel))
      return FAIL(E_INVALIDARG);
    if (Plane >= Format.PlanarCount || (Plane && !(Format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) ||
        !IsCpuVisibleHeap(&heap_props_))
      return FAIL(E_INVALIDARG);
    auto storage = texture->plane(Plane);
    D3D12_BOX full_box = GetResourceExtent(desc_, Level);
    full_box.right = std::max(1u, storage->width() >> Level);
    full_box.bottom = std::max(1u, storage->height() >> Level);
    // D3D11.3 19.5.1 defines BC regions in full 4-texel blocks, including the physical extent of small mips.
    constexpr UINT block_width = 4;
    if (Format.Flag & MTL_DXGI_FORMAT_BC) {
      full_box.right = align(full_box.right, block_width);
      full_box.bottom = align(full_box.bottom, block_width);
    }
    D3D12_BOX box = pDstBox ? *pDstBox : full_box;

    if (!IsD3D12BoxInBounds(box, full_box))
      return FAIL(E_INVALIDARG);

    if (box.left == box.right || box.top == box.bottom || box.front == box.back)
      return S_OK;

    if (Format.Flag & MTL_DXGI_FORMAT_BC) {
      if ((box.left | box.right | box.top | box.bottom) & (block_width - 1))
        return FAIL(E_INVALIDARG);
      box.right = std::min(box.right, std::max(1u, storage->width() >> Level));
      box.bottom = std::min(box.bottom, std::max(1u, storage->height() >> Level));
    }

    storage->current()->texture().replaceRegion(
        {box.left, box.top, box.front}, {box.right - box.left, box.bottom - box.top, box.back - box.front}, Level,
        Slice, pSrcData, SrcRowPitch, SrcSlicePitch
    );
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstSlicePitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox
  ) {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      DstSlicePitch = 0;
    if ((desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || desc_.SampleDesc.Count != 1 ||
        (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc_.MipLevels > 1))
      return FAIL(E_INVALIDARG);
    uint32_t Level = 0, Slice = 0, Plane = 0;
    DecomposeSubresource(desc_, SrcSubresource, &Level, &Slice, &Plane);
    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, Format)) ||
        !std::has_single_bit(Format.BytesPerTexel))
      return FAIL(E_INVALIDARG);
    if (Plane >= Format.PlanarCount || (Plane && !(Format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) ||
        !IsCpuVisibleHeap(&heap_props_))
      return FAIL(E_INVALIDARG);
    auto storage = texture->plane(Plane);
    D3D12_BOX full_box = GetResourceExtent(desc_, Level);
    full_box.right = std::max(1u, storage->width() >> Level);
    full_box.bottom = std::max(1u, storage->height() >> Level);
    // D3D11.3 19.5.1 defines BC regions in full 4-texel blocks, including the physical extent of small mips.
    constexpr UINT block_width = 4;
    if (Format.Flag & MTL_DXGI_FORMAT_BC) {
      full_box.right = align(full_box.right, block_width);
      full_box.bottom = align(full_box.bottom, block_width);
    }
    D3D12_BOX box = pSrcBox ? *pSrcBox : full_box;

    if (!IsD3D12BoxInBounds(box, full_box))
      return FAIL(E_INVALIDARG);

    if (box.left == box.right || box.top == box.bottom || box.front == box.back)
      return S_OK;

    if (Format.Flag & MTL_DXGI_FORMAT_BC) {
      if ((box.left | box.right | box.top | box.bottom) & (block_width - 1))
        return FAIL(E_INVALIDARG);
      box.right = std::min(box.right, std::max(1u, storage->width() >> Level));
      box.bottom = std::min(box.bottom, std::max(1u, storage->height() >> Level));
    }

    storage->current()->texture().getBytes(
        {box.left, box.top, box.front}, {box.right - box.left, box.bottom - box.top, box.back - box.front}, Level,
        Slice, pDstData, DstRowPitch, DstSlicePitch
    );
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS *pFlags) {
    if (reserved_)
      return FAIL(E_INVALIDARG);
    if (pHeapProps)
      *pHeapProps = heap_props_;
    if (pFlags)
      *pFlags = heap_flags_;
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_SHADER_RESOURCE_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN)
        ViewDesc.Format = desc_.Format;
    }

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    UINT plane = ViewDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D ? ViewDesc.Texture2D.PlaneSlice
                 : ViewDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                     ? ViewDesc.Texture2DArray.PlaneSlice : 0;
    auto texture = this->texture->plane(plane);
    if (this->texture->chroma) {
      if (plane >= 2 || (plane ? ViewDesc.Format != DXGI_FORMAT_R8G8_UNORM && ViewDesc.Format != DXGI_FORMAT_R8G8_UINT
                              : ViewDesc.Format != DXGI_FORMAT_R8_UNORM && ViewDesc.Format != DXGI_FORMAT_R8_UINT))
        return FAIL(E_INVALIDARG);
    }
    TextureViewKey View = texture->fullView;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr) || (!this->texture->chroma && DXGIFormatType(ViewDesc.Format) != DXGIFormatType(desc_.Format)))
      return FAILED(hr) ? hr : FAIL(E_INVALIDARG);

    view_descriptor.format = EncodeComponentMapping(ViewDesc.Shader4ComponentMapping, metal_format.PixelFormat);

    if (texture->pixelFormat() == WMTPixelFormatDepth32Float || texture->pixelFormat() == WMTPixelFormatDepth16Unorm) {
      view_descriptor.format = EncodeComponentMapping(
          ViewDesc.Shader4ComponentMapping, WMTPixelFormat(texture->pixelFormat() | WMTPixelFormatR001Swizzle));
    }

    FLOAT ResourceMinLODClamp = 0;

    switch (ViewDesc.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: {
      view_descriptor.type = WMTTextureType2DArray; // as every declaration reads it (lowering_texture_to_array)
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MostDetailedMip;
      if (ViewDesc.Texture1D.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.Texture1D.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.Texture1D.MipLevels;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.Texture1D.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MostDetailedMip;
      if (ViewDesc.Texture1DArray.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.Texture1DArray.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.Texture1DArray.MipLevels;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      if (ViewDesc.Texture1DArray.ArraySize == ~0u)
        view_descriptor.arraySize = desc_.DepthOrArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
      else
        view_descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.Texture1DArray.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2D: {
      view_descriptor.type = WMTTextureType2DArray; // as every declaration reads it (lowering_texture_to_array)
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MostDetailedMip;
      if (ViewDesc.Texture2D.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.Texture2D.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.Texture2D.MipLevels;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.Texture2D.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MostDetailedMip;
      if (ViewDesc.Texture2DArray.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.Texture2DArray.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.Texture2DArray.MipLevels;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      if (ViewDesc.Texture2DArray.ArraySize == ~0u)
        view_descriptor.arraySize = desc_.DepthOrArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
      else
        view_descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.Texture2DArray.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURECUBE: {
      view_descriptor.type = WMTTextureTypeCube; // FIXME: lowering to cube array
      view_descriptor.firstMiplevel = ViewDesc.TextureCube.MostDetailedMip;
      if (ViewDesc.TextureCube.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.TextureCube.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.TextureCube.MipLevels;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 6;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.TextureCube.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: {
      view_descriptor.type = WMTTextureTypeCubeArray;
      view_descriptor.firstMiplevel = ViewDesc.TextureCubeArray.MostDetailedMip;
      if (ViewDesc.TextureCubeArray.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.TextureCubeArray.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.TextureCubeArray.MipLevels;
      // the cubes from the face the view starts at: as many as asked, or the whole ones that are left
      const UINT faces = 6;
      view_descriptor.firstArraySlice = ViewDesc.TextureCubeArray.First2DArrayFace;
      if (ViewDesc.TextureCubeArray.NumCubes == ~0u)
        view_descriptor.arraySize = (desc_.DepthOrArraySize - ViewDesc.TextureCubeArray.First2DArrayFace) / faces * faces;
      else
        view_descriptor.arraySize = ViewDesc.TextureCubeArray.NumCubes * faces;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.TextureCubeArray.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2DMS: {
      view_descriptor.type = texture->sampleCount() == 1 ? WMTTextureType2D : WMTTextureType2DMultisample;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: {
      view_descriptor.type = texture->sampleCount() == 1 ? WMTTextureType2DArray : WMTTextureType2DMultisampleArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      if (ViewDesc.Texture2DMSArray.ArraySize == ~0u)
        view_descriptor.arraySize = desc_.DepthOrArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
      else
        view_descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE3D: {
      view_descriptor.type = WMTTextureType3D;
      view_descriptor.firstMiplevel = ViewDesc.Texture3D.MostDetailedMip;
      if (ViewDesc.Texture3D.MipLevels == ~0u)
        view_descriptor.miplevelCount = desc_.MipLevels - ViewDesc.Texture3D.MostDetailedMip;
      else
        view_descriptor.miplevelCount = ViewDesc.Texture3D.MipLevels;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      ResourceMinLODClamp = ViewDesc.Texture3D.ResourceMinLODClamp;
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }

    // into the view's mip numbering, keeping the fraction (D3D12_TEX2D_SRV: a clamp of 0.5 is not 0)
    ResourceMinLODClamp -= view_descriptor.firstMiplevel;

    return Heap->AddShaderResourceView(
        Index, texture, View, ResourceMinLODClamp, BorderColorRange(metal_format),
        ViewDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS ||
            ViewDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
    );
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    HRESULT hr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN)
        ViewDesc.Format = desc_.Format;
    }

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    UINT plane = ViewDesc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D ? ViewDesc.Texture2D.PlaneSlice
                 : ViewDesc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                     ? ViewDesc.Texture2DArray.PlaneSlice : 0;
    auto texture = this->texture->plane(plane);
    if (this->texture->chroma) {
      if (plane >= 2 || (plane ? ViewDesc.Format != DXGI_FORMAT_R8G8_UNORM && ViewDesc.Format != DXGI_FORMAT_R8G8_UINT
                              : ViewDesc.Format != DXGI_FORMAT_R8_UNORM && ViewDesc.Format != DXGI_FORMAT_R8_UINT))
        return FAIL(E_INVALIDARG);
    }
    TextureViewKey View = texture->fullView;
    UINT FirstWSlice = 0, WSize = 0; // the W range of a 3D view

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    const bool r32 = ViewDesc.Format == DXGI_FORMAT_R32_UINT || ViewDesc.Format == DXGI_FORMAT_R32_SINT ||
                     ViewDesc.Format == DXGI_FORMAT_R32_FLOAT;
    if (FAILED(hr) || !(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        (!this->texture->chroma && DXGIFormatType(ViewDesc.Format) != DXGIFormatType(desc_.Format) &&
         !(r32 && CastsToR32UnorderedAccess(desc_.Format)))) {
      WARN("CreateUnorderedAccessView: format ", ViewDesc.Format, ", dimension ", ViewDesc.ViewDimension,
           " of a texture of format ", desc_.Format, ", flags ", desc_.Flags);
      return FAILED(hr) ? hr : FAIL(E_INVALIDARG);
    }

    view_descriptor.format = metal_format.PixelFormat;

    switch (ViewDesc.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE1D: {
      view_descriptor.type = WMTTextureType2DArray; // as every declaration reads it (lowering_texture_to_array)
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE2D: {
      view_descriptor.type = WMTTextureType2DArray; // as every declaration reads it (lowering_texture_to_array)
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      if (ViewDesc.Texture1DArray.ArraySize == ~0u)
        view_descriptor.arraySize = desc_.DepthOrArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
      else
        view_descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      if (ViewDesc.Texture2DArray.ArraySize == ~0u)
        view_descriptor.arraySize = desc_.DepthOrArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
      else
        view_descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE3D: {
      view_descriptor.type = WMTTextureType3D;
      view_descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      auto depth = std::max(desc_.DepthOrArraySize >> ViewDesc.Texture3D.MipSlice, 1);
      FirstWSlice = ViewDesc.Texture3D.FirstWSlice;
      WSize = ViewDesc.Texture3D.WSize == ~0u ? depth - FirstWSlice : ViewDesc.Texture3D.WSize;
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }

    return Heap->AddUnorderedAccessView(Index, texture, View, FirstWSlice, WSize);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_RENDER_TARGET_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN)
        ViewDesc.Format = desc_.Format;
    }

    auto [Heap, Index] = GetRenderTargetHeap(device_, Descriptor);
    UINT plane = ViewDesc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D ? ViewDesc.Texture2D.PlaneSlice
                 : ViewDesc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY
                     ? ViewDesc.Texture2DArray.PlaneSlice : 0;
    auto texture = this->texture->plane(plane);
    if (this->texture->chroma) {
      if (plane >= 2 || (plane ? ViewDesc.Format != DXGI_FORMAT_R8G8_UNORM && ViewDesc.Format != DXGI_FORMAT_R8G8_UINT
                              : ViewDesc.Format != DXGI_FORMAT_R8_UNORM && ViewDesc.Format != DXGI_FORMAT_R8_UINT))
        return FAIL(E_INVALIDARG);
    }
    TextureViewKey View = texture->fullView;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr) || (!this->texture->chroma && DXGIFormatType(ViewDesc.Format) != DXGIFormatType(desc_.Format)))
      return FAILED(hr) ? hr : FAIL(E_INVALIDARG);

    // a swizzled view is read only: an attachment stores the channels as they are (RenderTargetChannels)
    view_descriptor.format = WMTPixelFormat(ORIGINAL_FORMAT(metal_format.PixelFormat));

    MTL_RENDER_TARGET_DESC RenderTargetDesc;
    RenderTargetDesc.DepthPlane = 0;
    RenderTargetDesc.RenderTargetArrayLength = 0;
    RenderTargetDesc.Flags = 0;

    switch (ViewDesc.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE1D: {
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      view_descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize == ~0u
                                      ? texture->arrayLength() - ViewDesc.Texture1DArray.FirstArraySlice
                                      : ViewDesc.Texture1DArray.ArraySize;
      if (!WithinSlices(view_descriptor.firstArraySlice, view_descriptor.arraySize, texture->arrayLength()))
        return FAIL(E_INVALIDARG);
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2D: {
      if (ViewDesc.Texture2D.PlaneSlice && !this->texture->chroma)
        return FAIL(E_INVALIDARG);
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY: {
      if (ViewDesc.Texture2DArray.PlaneSlice && !this->texture->chroma)
        return FAIL(E_INVALIDARG);
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      view_descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize == ~0u
                                      ? texture->arrayLength() - ViewDesc.Texture2DArray.FirstArraySlice
                                      : ViewDesc.Texture2DArray.ArraySize;
      if (!WithinSlices(view_descriptor.firstArraySlice, view_descriptor.arraySize, texture->arrayLength()))
        return FAIL(E_INVALIDARG);
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2DMS: {
      view_descriptor.type = WMTTextureType2DMultisample; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: {
      view_descriptor.type = WMTTextureType2DMultisampleArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      view_descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize == ~0u
                                      ? texture->arrayLength() - ViewDesc.Texture2DMSArray.FirstArraySlice
                                      : ViewDesc.Texture2DMSArray.ArraySize;
      if (!WithinSlices(view_descriptor.firstArraySlice, view_descriptor.arraySize, texture->arrayLength()))
        return FAIL(E_INVALIDARG);
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE3D: {
      view_descriptor.type = WMTTextureType3D;
      view_descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      // -1 is the W slices from FirstWSlice to the end of the mip (D3D12_TEX3D_RTV)
      auto ArraySize = std::max<UINT16>(desc_.DepthOrArraySize >> ViewDesc.Texture3D.MipSlice, 1);
      RenderTargetDesc.DepthPlane = ViewDesc.Texture3D.FirstWSlice;
      RenderTargetDesc.RenderTargetArrayLength =
          ViewDesc.Texture3D.WSize == ~0u ? ArraySize - ViewDesc.Texture3D.FirstWSlice : ViewDesc.Texture3D.WSize;
      if (!WithinSlices(ViewDesc.Texture3D.FirstWSlice, RenderTargetDesc.RenderTargetArrayLength, ArraySize))
        return FAIL(E_INVALIDARG);
      View = texture->createView(view_descriptor);
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }

    RenderTargetDesc.Texture = texture;
    RenderTargetDesc.View = View;
    RenderTargetDesc.Format = metal_format.PixelFormat;
    RenderTargetDesc.Width = std::max<uint32_t>(1u, texture->width() >> view_descriptor.firstMiplevel);
    RenderTargetDesc.Height = std::max<uint32_t>(1u, texture->height() >> view_descriptor.firstMiplevel);

    return Heap->AddRenderTarget(Index, &RenderTargetDesc);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    if (texture->chroma)
      return FAIL(E_INVALIDARG);
    HRESULT hr;
    D3D12_DEPTH_STENCIL_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN)
        ViewDesc.Format = desc_.Format;
    }

    auto [Heap, Index] = GetRenderTargetHeap(device_, Descriptor);
    TextureViewKey View = texture->fullView;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr) || !(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        DXGIFormatType(ViewDesc.Format) != DXGIFormatType(desc_.Format))
      return FAILED(hr) ? hr : FAIL(E_INVALIDARG);
    view_descriptor.format = metal_format.PixelFormat;

    MTL_RENDER_TARGET_DESC RenderTargetDesc;
    RenderTargetDesc.DepthPlane = 0;
    RenderTargetDesc.RenderTargetArrayLength = 0;
    RenderTargetDesc.Flags = ViewDesc.Flags;
    RenderTargetDesc.DepthFormat = ViewDesc.Format;

    switch (ViewDesc.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D: {
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      view_descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize == ~0u
                                      ? texture->arrayLength() - ViewDesc.Texture1DArray.FirstArraySlice
                                      : ViewDesc.Texture1DArray.ArraySize;
      if (!WithinSlices(view_descriptor.firstArraySlice, view_descriptor.arraySize, texture->arrayLength()))
        return FAIL(E_INVALIDARG);
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2D: {
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY: {
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      view_descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize == ~0u
                                      ? texture->arrayLength() - ViewDesc.Texture2DArray.FirstArraySlice
                                      : ViewDesc.Texture2DArray.ArraySize;
      if (!WithinSlices(view_descriptor.firstArraySlice, view_descriptor.arraySize, texture->arrayLength()))
        return FAIL(E_INVALIDARG);
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2DMS: {
      view_descriptor.type = WMTTextureType2DMultisample; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY: {
      view_descriptor.type = WMTTextureType2DMultisampleArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      view_descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize == ~0u
                                      ? texture->arrayLength() - ViewDesc.Texture2DMSArray.FirstArraySlice
                                      : ViewDesc.Texture2DMSArray.ArraySize;
      if (!WithinSlices(view_descriptor.firstArraySlice, view_descriptor.arraySize, texture->arrayLength()))
        return FAIL(E_INVALIDARG);
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    default:
      return FAIL(E_INVALIDARG);
    }

    RenderTargetDesc.Texture = texture.ptr();
    RenderTargetDesc.View = View;
    RenderTargetDesc.Width = std::max<uint32_t>(1u, texture->width() >> view_descriptor.firstMiplevel);
    RenderTargetDesc.Height = std::max<uint32_t>(1u, texture->height() >> view_descriptor.firstMiplevel);

    return Heap->AddRenderTarget(Index, &RenderTargetDesc);
  };

  // tiles as D3D12 numbers them, which the queue's mapping updates walk: each array slice's standard mips in order,
  // then its packed mips, which are Metal's mip tail. shapes and the tail are Metal's
  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    if (!reserved_)
      return NoTiling(TotalTileCount, PackedMipInfo, StandardTileShape, SubresourceTilingCount);
    UINT mips = desc_.MipLevels, slices = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc_.DepthOrArraySize;
    D3D12_TILE_SHAPE shape{};
    D3D12_PACKED_MIP_INFO packed{UINT8(mips)};
    UINT per_slice = 0;
    auto tiles = [&](UINT mip) {
      auto extent = [&](uint64_t size, UINT tile) { return UINT((std::max(size >> mip, uint64_t(1)) + tile - 1) / tile); };
      return D3D12_SUBRESOURCE_TILING{
          extent(desc_.Width, shape.WidthInTexels), UINT16(extent(desc_.Height, shape.HeightInTexels)),
          UINT16(desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? extent(desc_.DepthOrArraySize, shape.DepthInTexels) : 1)
      };
    };
    auto size = device_->GetMTLDevice().sparseTileSize(
        texture->textureType(), texture->pixelFormat(), texture->sampleCount(), device_->GetSparsePageSize()
    );
    shape = {UINT(size.width), UINT(size.height), UINT(size.depth)};
    auto [first, bytes] = texture->current()->texture().sparseTail();
    packed.NumStandardMips = UINT8(std::min<uint64_t>(first, mips));
    packed.NumPackedMips = UINT8(mips - packed.NumStandardMips);
    packed.NumTilesForPackedMips =
        packed.NumPackedMips ? UINT((bytes + D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES - 1) / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) : 0;
    for (UINT mip = 0; mip < packed.NumStandardMips; mip++) {
      auto t = tiles(mip);
      per_slice += t.WidthInTiles * t.HeightInTiles * t.DepthInTiles;
    }
    // without packed mips the start is 0 (D3D12_PACKED_MIP_INFO)
    packed.StartTileIndexInOverallResource = packed.NumPackedMips ? per_slice : 0;
    per_slice += packed.NumTilesForPackedMips;
    if (TotalTileCount)
      *TotalTileCount = per_slice * slices;
    if (PackedMipInfo)
      *PackedMipInfo = packed;
    if (StandardTileShape)
      *StandardTileShape = shape;
    if (!SubresourceTilingCount)
      return;
    auto count = std::min(*SubresourceTilingCount, mips * slices - std::min(FirstSubresourceTiling, mips * slices));
    for (UINT i = 0; i < count; i++) {
      UINT subresource = FirstSubresourceTiling + i, mip = subresource % mips, start = subresource / mips * per_slice;
      if (mip >= packed.NumStandardMips) {
        SubresourceTilings[i] = {0, 0, 0, D3D12_PACKED_TILE};
        continue;
      }
      for (UINT m = 0; m < mip; m++) {
        auto t = tiles(m);
        start += t.WidthInTiles * t.HeightInTiles * t.DepthInTiles;
      }
      SubresourceTilings[i] = tiles(mip);
      SubresourceTilings[i].StartTileIndexInOverallResource = start;
    }
    *SubresourceTilingCount = count;
  };
};

class MTLD3D12BackBuffer : public MTLD3D12Texture {
  D3D12SwapChainBufferRefs &buffers_;

public:
  MTLD3D12BackBuffer(MTLD3D12Device *pDevice, D3D12SwapChainBufferRefs &buffers) :
      MTLD3D12Texture(pDevice), buffers_(buffers) {}

  ULONG STDMETHODCALLTYPE
  AddRef() override {
    std::lock_guard<mullion::mutex> lock(buffers_.mutex);
    auto count = MTLD3D12Texture::AddRef();
    if (count == 1 && !buffers_.referenced_buffers++)
      buffers_.swapchain->AddRef();
    return count;
  }

  ULONG STDMETHODCALLTYPE
  Release() override {
    auto &buffers = buffers_;
    auto swapchain = buffers.swapchain;
    std::unique_lock<mullion::mutex> lock(buffers.mutex);
    auto count = MTLD3D12Texture::Release();
    bool last = !count && !--buffers.referenced_buffers;
    if (last) {
      lock.unlock();
      swapchain->Release();
    }
    return count;
  }
};

HRESULT
CreateCommittedTexture(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource, D3D12SwapChainBufferRefs *pBufferRefs, const MullionMetalResource *pTexture
) {
  auto texture = Com<MTLD3D12Texture>(
      pBufferRefs ? new MTLD3D12BackBuffer(pDevice, *pBufferRefs) : new MTLD3D12Texture(pDevice)
  );
  HRESULT hr = texture->Initialize(pHeapProps, HeapFlags, pDesc, InitialState, nullptr, 0, false, pTexture);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return texture->QueryInterface(riid, ppResource);
}

HRESULT
CreateReservedTexture(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, REFIID riid,
    void **ppResource
) {
  auto texture = Com(new MTLD3D12Texture(pDevice));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  HRESULT hr = texture->Initialize(&heap, D3D12_HEAP_FLAG_NONE, pDesc, InitialState, nullptr, 0, true);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return texture->QueryInterface(riid, ppResource);
}

HRESULT
CreatePlacedTexture(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  auto texture = Com(new MTLD3D12Texture(pDevice));
  D3D12_HEAP_DESC heap_desc = pHeap->GetDesc();

  if (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
    if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES)
      return FAIL(E_INVALIDARG);
  } else {
    if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES)
      return FAIL(E_INVALIDARG);
  }

  HRESULT hr = texture->Initialize(&heap_desc.Properties, heap_desc.Flags, pDesc, InitialState, pHeap, HeapOffset);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return texture->QueryInterface(riid, ppResource);
}

} // namespace mullion
