#include "d3d11_device.hpp"
#include "d3d11_private.h"
#include "mullion_format.hpp"
#include "d3d11_resource.hpp"
#include "d3d11_1.h"
#include "util_math.hpp"
#include <bit>

namespace mullion {

template <>
HRESULT
ExtractEntireResourceViewDescription(const D3D11_BUFFER_DESC *, D3D11_RENDER_TARGET_VIEW_DESC1 *) {
  return E_INVALIDARG;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_BUFFER_DESC,
                                             D3D11_SHADER_RESOURCE_VIEW_DESC1>(
    const D3D11_BUFFER_DESC *pResourceDesc,
    D3D11_SHADER_RESOURCE_VIEW_DESC1 *pViewDescOut) {
  if (pResourceDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
    pViewDescOut->Format = DXGI_FORMAT_UNKNOWN;
    pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    pViewDescOut->Buffer.FirstElement = 0;
    pViewDescOut->Buffer.NumElements =
        pResourceDesc->ByteWidth / pResourceDesc->StructureByteStride;
  } else {
    return E_FAIL;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_BUFFER_DESC,
                                             D3D11_UNORDERED_ACCESS_VIEW_DESC1>(
    const D3D11_BUFFER_DESC *pResourceDesc,
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pViewDescOut) {
  if (pResourceDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
    pViewDescOut->Format = DXGI_FORMAT_UNKNOWN;
    pViewDescOut->ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    pViewDescOut->Buffer.FirstElement = 0;
    pViewDescOut->Buffer.NumElements =
        pResourceDesc->ByteWidth / pResourceDesc->StructureByteStride;
    pViewDescOut->Buffer.Flags = 0;
  } else {
    return E_FAIL;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE1D_DESC,
                                             D3D11_SHADER_RESOURCE_VIEW_DESC1>(
    const D3D11_TEXTURE1D_DESC *pResourceDesc,
    D3D11_SHADER_RESOURCE_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;

  if (pResourceDesc->ArraySize == 1) {
    pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
    pViewDescOut->Texture1D.MostDetailedMip = 0;
    pViewDescOut->Texture1D.MipLevels = pResourceDesc->MipLevels;
  } else {
    pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
    pViewDescOut->Texture1DArray.MostDetailedMip = 0;
    pViewDescOut->Texture1DArray.MipLevels = pResourceDesc->MipLevels;
    pViewDescOut->Texture1DArray.FirstArraySlice = 0;
    pViewDescOut->Texture1DArray.ArraySize = pResourceDesc->ArraySize;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE1D_DESC,
                                             D3D11_UNORDERED_ACCESS_VIEW_DESC1>(
    const D3D11_TEXTURE1D_DESC *pResourceDesc,
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;

  if (pResourceDesc->ArraySize == 1) {
    pViewDescOut->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
    pViewDescOut->Texture1D.MipSlice = 0;
  } else {
    pViewDescOut->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
    pViewDescOut->Texture1DArray.MipSlice = 0;
    pViewDescOut->Texture1DArray.FirstArraySlice = 0;
    pViewDescOut->Texture1DArray.ArraySize = pResourceDesc->ArraySize;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE1D_DESC,
                                             D3D11_RENDER_TARGET_VIEW_DESC1>(
    const D3D11_TEXTURE1D_DESC *pResourceDesc,
    D3D11_RENDER_TARGET_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;
  if (pResourceDesc->ArraySize == 1) {
    pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
    pViewDescOut->Texture1D.MipSlice = 0;
  } else {
    pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
    pViewDescOut->Texture1DArray.MipSlice = 0;
    pViewDescOut->Texture1DArray.FirstArraySlice = 0;
    pViewDescOut->Texture1DArray.ArraySize = pResourceDesc->ArraySize;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE1D_DESC,
                                             D3D11_DEPTH_STENCIL_VIEW_DESC>(
    const D3D11_TEXTURE1D_DESC *pResourceDesc,
    D3D11_DEPTH_STENCIL_VIEW_DESC *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;

  if (pResourceDesc->ArraySize == 1) {
    pViewDescOut->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
    pViewDescOut->Texture1D.MipSlice = 0;
  } else {
    pViewDescOut->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
    pViewDescOut->Texture1DArray.MipSlice = 0;
    pViewDescOut->Texture1DArray.FirstArraySlice = 0;
    pViewDescOut->Texture1DArray.ArraySize = pResourceDesc->ArraySize;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE2D_DESC1,
                                             D3D11_SHADER_RESOURCE_VIEW_DESC1>(
    const D3D11_TEXTURE2D_DESC1 *pResourceDesc,
    D3D11_SHADER_RESOURCE_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;

  if (pResourceDesc->SampleDesc.Count == 1) {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      pViewDescOut->Texture2D.MostDetailedMip = 0;
      pViewDescOut->Texture2D.MipLevels = pResourceDesc->MipLevels;
      pViewDescOut->Texture2D.PlaneSlice = 0;
    } else if (pResourceDesc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
      auto texture_cube_array_length = pResourceDesc->ArraySize / 6;
      if (texture_cube_array_length > 1) {
        pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        pViewDescOut->TextureCubeArray.MostDetailedMip = 0;
        pViewDescOut->TextureCubeArray.MipLevels = pResourceDesc->MipLevels;
        pViewDescOut->TextureCubeArray.First2DArrayFace = 0;
        pViewDescOut->TextureCubeArray.NumCubes = texture_cube_array_length;
      } else {
        pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        pViewDescOut->TextureCube.MostDetailedMip = 0;
        pViewDescOut->TextureCube.MipLevels = pResourceDesc->MipLevels;
      }
    } else {
      pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      pViewDescOut->Texture2DArray.MostDetailedMip = 0;
      pViewDescOut->Texture2DArray.MipLevels = pResourceDesc->MipLevels;
      pViewDescOut->Texture2DArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DArray.ArraySize = pResourceDesc->ArraySize;
      pViewDescOut->Texture2DArray.PlaneSlice = 0;
    }
  } else {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
      pViewDescOut->Texture2DMSArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DMSArray.ArraySize = pResourceDesc->ArraySize;
    }
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE2D_DESC1,
                                             D3D11_UNORDERED_ACCESS_VIEW_DESC1>(
    const D3D11_TEXTURE2D_DESC1 *pResourceDesc,
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;

  if (pResourceDesc->SampleDesc.Count == 1) {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      pViewDescOut->Texture2D.MipSlice = 0;
      pViewDescOut->Texture2D.PlaneSlice = 0;
    } else {
      pViewDescOut->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
      pViewDescOut->Texture2DArray.MipSlice = 0;
      pViewDescOut->Texture2DArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DArray.ArraySize = pResourceDesc->ArraySize;
      pViewDescOut->Texture2DArray.PlaneSlice = 0;
    }
  } else {
    ERR("Invalid to create DSV from muiltisampled texture");
    return E_FAIL;
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE2D_DESC1,
                                             D3D11_DEPTH_STENCIL_VIEW_DESC>(
    const D3D11_TEXTURE2D_DESC1 *pResourceDesc,
    D3D11_DEPTH_STENCIL_VIEW_DESC *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;
  if (pResourceDesc->SampleDesc.Count == 1) {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
      pViewDescOut->Texture2D.MipSlice = 0;
    } else {
      pViewDescOut->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
      pViewDescOut->Texture2DArray.MipSlice = 0;
      pViewDescOut->Texture2DArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DArray.ArraySize = pResourceDesc->ArraySize;
    }
  } else {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    } else {
      pViewDescOut->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
      pViewDescOut->Texture2DMSArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DMSArray.ArraySize = pResourceDesc->ArraySize;
    }
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE2D_DESC1,
                                             D3D11_RENDER_TARGET_VIEW_DESC1>(
    const D3D11_TEXTURE2D_DESC1 *pResourceDesc,
    D3D11_RENDER_TARGET_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;
  if (pResourceDesc->SampleDesc.Count == 1) {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      pViewDescOut->Texture2D.MipSlice = 0;
      pViewDescOut->Texture2D.PlaneSlice = 0;
    } else {
      pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
      pViewDescOut->Texture2DArray.MipSlice = 0;
      pViewDescOut->Texture2DArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DArray.ArraySize = pResourceDesc->ArraySize;
      pViewDescOut->Texture2DArray.PlaneSlice = 0;
    }
  } else {
    if (pResourceDesc->ArraySize == 1) {
      pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
      pViewDescOut->Texture2DMSArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DMSArray.ArraySize = pResourceDesc->ArraySize;
    }
  }
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE3D_DESC1,
                                             D3D11_SHADER_RESOURCE_VIEW_DESC1>(
    const D3D11_TEXTURE3D_DESC1 *pResourceDesc,
    D3D11_SHADER_RESOURCE_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;

  pViewDescOut->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
  pViewDescOut->Texture3D.MostDetailedMip = 0;
  pViewDescOut->Texture3D.MipLevels = pResourceDesc->MipLevels;
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE3D_DESC1,
                                             D3D11_UNORDERED_ACCESS_VIEW_DESC1>(
    const D3D11_TEXTURE3D_DESC1 *pResourceDesc,
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;
  pViewDescOut->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
  pViewDescOut->Texture3D.MipSlice = 0;
  pViewDescOut->Texture3D.FirstWSlice = 0;
  pViewDescOut->Texture3D.WSize = pResourceDesc->Depth;
  return S_OK;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE3D_DESC1,
                                             D3D11_DEPTH_STENCIL_VIEW_DESC>(
    const D3D11_TEXTURE3D_DESC1 *pResourceDesc,
    D3D11_DEPTH_STENCIL_VIEW_DESC *pViewDescOut) {
  ERR("Invalid to create DSV from Texture3D");
  return E_FAIL;
}

template <>
HRESULT ExtractEntireResourceViewDescription<D3D11_TEXTURE3D_DESC1,
                                             D3D11_RENDER_TARGET_VIEW_DESC1>(
    const D3D11_TEXTURE3D_DESC1 *pResourceDesc,
    D3D11_RENDER_TARGET_VIEW_DESC1 *pViewDescOut) {
  pViewDescOut->Format = pResourceDesc->Format;
  pViewDescOut->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
  pViewDescOut->Texture3D.MipSlice = 0;
  pViewDescOut->Texture3D.FirstWSlice = 0;
  pViewDescOut->Texture3D.WSize = pResourceDesc->Depth;
  return S_OK;
}

HRESULT
CreateMTLTextureDescriptorInternal(
    MTLD3D11Device *pDevice, D3D11_RESOURCE_DIMENSION Dimension, UINT Width,
    UINT Height, UINT Depth, UINT ArraySize, UINT SampleCount, UINT BindFlags,
    UINT CPUAccessFlags, UINT MiscFlags, D3D11_USAGE Usage, UINT MipLevels,
    DXGI_FORMAT Format, WMTTextureInfo *pDescOut) {
  if (Width == 0 || Height == 0 || Depth == 0 || ArraySize == 0 || SampleCount == 0)
    return E_INVALIDARG;

  const bool d3d11 = pDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;
  const UINT dimension_limit =
      Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D
          ? (d3d11 ? D3D11_REQ_TEXTURE1D_U_DIMENSION : D3D10_REQ_TEXTURE1D_U_DIMENSION)
      : Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D
          ? (d3d11 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION)
          : (d3d11 ? D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : D3D10_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
  if (Width > dimension_limit || Height > dimension_limit || Depth > dimension_limit)
    return E_INVALIDARG;

  UINT array_limit = pDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ?
                       (Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? D3D11_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION :
                                                                         D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) :
                       (Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? D3D10_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION :
                                                                         D3D10_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
  UINT mip_limit = std::bit_width(Width | Height | Depth);
  if (ArraySize > array_limit || MipLevels > mip_limit ||
      (SampleCount > 1 && (MipLevels != 1 || Usage != D3D11_USAGE_DEFAULT)) ||
      (Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D && (BindFlags & D3D11_BIND_DEPTH_STENCIL)))
    return E_INVALIDARG;
  // D3D11.3 5.3.8: a cube view spans six square faces; Windows also accepts trailing slices at 10.1 and above
  constexpr UINT cube_faces = 6;
  if ((MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) &&
      (Dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || Width != Height || ArraySize < cube_faces ||
       SampleCount != 1 || (pDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_1 && ArraySize != cube_faces)))
    return E_INVALIDARG;

  MTL_DXGI_FORMAT_DESC metal_format;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), Format, metal_format)) ||
      metal_format.PixelFormat == WMTPixelFormatInvalid) {
    ERR("CreateMTLTextureDescriptorInternal: creating a texture of invalid "
        "format: ",
        Format);
    return E_FAIL;
  }

  // a format makes the textures its row names (Microsoft Learn, "Format support for Direct3D Feature Level 11.0
  // hardware": Texture1D, Texture2D, Texture3D, TextureCube)
  UINT support = 0;
  pDevice->CheckFormatSupport(Format, &support);
  if (!(support & ((MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)    ? D3D11_FORMAT_SUPPORT_TEXTURECUBE
                   : Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? D3D11_FORMAT_SUPPORT_TEXTURE1D
                   : Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? D3D11_FORMAT_SUPPORT_TEXTURE2D
                                                                     : D3D11_FORMAT_SUPPORT_TEXTURE3D)))
    return E_INVALIDARG;

  if (metal_format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    if (((Width | Height) & 1) || MipLevels != 1 ||
        SampleCount != 1 || (MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ||
        (BindFlags & D3D11_BIND_DEPTH_STENCIL) || (Usage == D3D11_USAGE_STAGING && ArraySize > 1))
      return E_INVALIDARG;
  }

  auto shared_flag = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  // 10Level9 ID3D11Device::CreateTexture2D excludes shared RGBA8 textures below feature level 10_0.
  if ((MiscFlags & shared_flag) == shared_flag ||
      ((MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) && !(MiscFlags & shared_flag)) ||
      ((MiscFlags & shared_flag) && pDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_0 &&
       (Format == DXGI_FORMAT_R8G8B8A8_UNORM || Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)))
    return E_INVALIDARG;

  if (BindFlags & D3D11_BIND_DEPTH_STENCIL) {
    switch (Format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      metal_format.PixelFormat = WMTPixelFormatDepth32Float;
      break;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      metal_format.PixelFormat = WMTPixelFormatDepth16Unorm;
      break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
      break;
    default:
      return E_INVALIDARG;
    }
  }
  pDescOut->pixel_format = ORIGINAL_FORMAT(metal_format.PixelFormat);
  // a stencil view's format is not one Metal makes textures in: the texture is the depth stencil one it is a view of
  if (pDescOut->pixel_format == WMTPixelFormatX32_Stencil8)
    pDescOut->pixel_format = WMTPixelFormatDepth32Float_Stencil8;

  auto capability =
      MTLGetDXGIFormatCapability(metal_format, pDevice->GetMTLPixelFormatCapability(pDescOut->pixel_format));
  if ((BindFlags & D3D11_BIND_RENDER_TARGET) && !any_bit_set(capability & FormatCapability::Color))
    return E_INVALIDARG;
  if ((BindFlags & D3D11_BIND_UNORDERED_ACCESS) &&
      ((BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
       !TypedUnorderedAccess(capability, pDevice->GetMTLPixelFormatCapability(metal_format.BufferPixelFormat))))
    return E_INVALIDARG;
  // a plane's format is the one a shader reads the depth stencil texture in
  if ((BindFlags & D3D11_BIND_SHADER_RESOURCE) && any_bit_set(capability & FormatCapability::DepthStencil) &&
      !(metal_format.Flag & MTL_DXGI_FORMAT_TYPELESS) && metal_format.PixelFormat == pDescOut->pixel_format)
    return E_INVALIDARG;
  // D3D11.3 19.1.4 only permits shader reads of the shared-exponent format, even when Metal can render it
  if (Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP && (BindFlags & ~D3D11_BIND_SHADER_RESOURCE))
    return E_INVALIDARG;

  WMTTextureUsage metal_usage = WMTTextureUsageShaderRead;

  if (BindFlags & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL |
                    D3D11_BIND_UNORDERED_ACCESS)) {
    ERR("CreateMTLTextureDescriptorInternal: invalid bind flags");
    return E_INVALIDARG;
  } else {
    if (BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))
      metal_usage |= WMTTextureUsageRenderTarget;
    if (BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
      // a uint clear writes through a uint view; a written texture is not compressed anyway
      metal_usage |= WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite | WMTTextureUsagePixelFormatView;
      // NOTE: R32/RG32_TYPELESS format should be mapped to uint pixel format
      if (metal_format.PixelFormat == WMTPixelFormatR32Uint ||
          metal_format.PixelFormat == WMTPixelFormatR32Sint ||
          (metal_format.PixelFormat == WMTPixelFormatRG32Uint &&
           pDevice->GetMTLDevice().supportsFamily(WMTGPUFamilyApple8))) {
        metal_usage |= WMTTextureUsageShaderAtomic;
      }
    }
    // decoder not supported: D3D11_BIND_DECODER, D3D11_BIND_VIDEO_ENCODER
  }

  // a copy into a staging depth stencil reads the stencil through an X32_Stencil8 view, as D3D12 does
  if ((metal_format.Flag & (MTL_DXGI_FORMAT_TYPELESS | MTL_DXGI_FORMAT_VIDEO_PLANAR)) ||
      pDescOut->pixel_format == WMTPixelFormatDepth32Float_Stencil8) {
    // well this is necessary for passing validation layer
    metal_usage |= WMTTextureUsagePixelFormatView;
  }

  // a resolve Metal cannot do renders its destination, any single-sampled default texture (ResolveSubresource)
  if (Usage == D3D11_USAGE_DEFAULT && Dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount == 1 &&
      any_bit_set(capability & FormatCapability::Color))
    metal_usage |= WMTTextureUsageRenderTarget;

  if ((MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) && MipmapFormat(pDescOut->pixel_format) != pDescOut->pixel_format)
    metal_usage |= WMTTextureUsagePixelFormatView;

  if (metal_format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)) {
    metal_usage |= WMTTextureUsageRenderTarget;
  }

  pDescOut->usage = metal_usage;

  WMTResourceOptions options = (WMTResourceOptions)0;
  switch (Usage) {
  case D3D11_USAGE_DEFAULT:
    options |= WMTResourceStorageModeManaged;
    break;
  case D3D11_USAGE_IMMUTABLE:
    options |= WMTResourceStorageModeManaged | // FIXME: switch to
                                                 // ResourceStorageModePrivate
               WMTResourceHazardTrackingModeUntracked;
    break;
  case D3D11_USAGE_DYNAMIC:
    options |=
        WMTResourceStorageModeShared | WMTResourceCPUCacheModeWriteCombined;
    break;
  case D3D11_USAGE_STAGING:
    options |= WMTResourceStorageModeShared;
    break;
  }

  pDescOut->options = options;

  if (MipLevels == 0) {
    pDescOut->mipmap_level_count = mip_limit;
  } else {
    pDescOut->mipmap_level_count = MipLevels;
  }

  pDescOut->sample_count = 1;
  pDescOut->array_length = 1;

  switch (Dimension) {
  default: {
    ERR("CreateMTLTextureDescriptorInternal: invalid texture dimension");
    return E_FAIL;
  }
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    if (ArraySize > 1) {
      pDescOut->type = WMTTextureType2DArray;
      pDescOut->array_length = ArraySize;
    } else {
      pDescOut->type = WMTTextureType2D;
    }
    break;
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    if (SampleCount > 1) {
      pDescOut->sample_count = RasterSampleCount(pDevice->GetMTLDevice(), SampleCount);
      if (!pDescOut->sample_count) {
        ERR("CreateMTLTextureDescriptorInternal: sample count ", SampleCount,
            " is not supported.");
        return E_INVALIDARG;
      }
      if (ArraySize > 1) {
        pDescOut->type = WMTTextureType2DMultisampleArray;
        pDescOut->array_length = ArraySize;
      } else {
        pDescOut->type = WMTTextureType2DMultisample;
      }
    } else {
      if (ArraySize > 1) {
        pDescOut->type = WMTTextureType2DArray;
        pDescOut->array_length = ArraySize;
      } else {
        pDescOut->type = WMTTextureType2D;
      }
    }
    break;
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    pDescOut->type = WMTTextureType3D;
    break;
  }
  pDescOut->width = Width;
  pDescOut->height = Height;
  pDescOut->depth = Depth;

  return S_OK;
};

template <>
HRESULT CreateMTLTextureDescriptor(MTLD3D11Device *pDevice,
                                   const D3D11_TEXTURE1D_DESC *pDesc,
                                   D3D11_TEXTURE1D_DESC *pOutDesc,
                                   WMTTextureInfo *pMtlDescOut) {
  auto hr = CreateMTLTextureDescriptorInternal(
      pDevice, D3D11_RESOURCE_DIMENSION_TEXTURE1D, pDesc->Width, 1, 1,
      pDesc->ArraySize, 1, pDesc->BindFlags, pDesc->CPUAccessFlags,
      pDesc->MiscFlags, pDesc->Usage, pDesc->MipLevels, pDesc->Format,
      pMtlDescOut);
  if (SUCCEEDED(hr)) {
    *pOutDesc = *pDesc;
    pOutDesc->MipLevels = (*pMtlDescOut).mipmap_level_count;
  }
  return hr;
}

template <>
HRESULT CreateMTLTextureDescriptor(MTLD3D11Device *pDevice,
                                   const D3D11_TEXTURE2D_DESC1 *pDesc,
                                   D3D11_TEXTURE2D_DESC1 *pOutDesc,
                                   WMTTextureInfo *pMtlDescOut) {
  if (pDesc->Format == DXGI_FORMAT_NV12 && pDesc->SampleDesc.Quality)
    return E_INVALIDARG;
  UINT quality_levels = 0;
  if (FAILED(pDevice->CheckMultisampleQualityLevels(pDesc->Format, pDesc->SampleDesc.Count, &quality_levels)) ||
      !quality_levels ||
      (pDesc->SampleDesc.Quality >= quality_levels &&
       (pDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_1 ||
        (pDesc->SampleDesc.Quality != D3D11_STANDARD_MULTISAMPLE_PATTERN &&
         pDesc->SampleDesc.Quality != D3D11_CENTER_MULTISAMPLE_PATTERN))))
    return E_INVALIDARG;
  auto hr = CreateMTLTextureDescriptorInternal(
      pDevice, D3D11_RESOURCE_DIMENSION_TEXTURE2D, pDesc->Width, pDesc->Height,
      1, pDesc->ArraySize, pDesc->SampleDesc.Count, pDesc->BindFlags,
      pDesc->CPUAccessFlags, pDesc->MiscFlags, pDesc->Usage, pDesc->MipLevels,
      pDesc->Format, pMtlDescOut);
  if (SUCCEEDED(hr)) {
    *pOutDesc = *pDesc;
    pOutDesc->MipLevels = (*pMtlDescOut).mipmap_level_count;
  }
  return hr;
}

template <>
HRESULT CreateMTLTextureDescriptor(MTLD3D11Device *pDevice,
                                   const D3D11_TEXTURE3D_DESC1 *pDesc,
                                   D3D11_TEXTURE3D_DESC1 *pOutDesc,
                                   WMTTextureInfo *pMtlDescOut) {
  auto hr = CreateMTLTextureDescriptorInternal(
      pDevice, D3D11_RESOURCE_DIMENSION_TEXTURE3D, pDesc->Width, pDesc->Height,
      pDesc->Depth, 1, 1, pDesc->BindFlags, pDesc->CPUAccessFlags,
      pDesc->MiscFlags, pDesc->Usage, pDesc->MipLevels, pDesc->Format,
      pMtlDescOut);
  if (SUCCEEDED(hr)) {
    *pOutDesc = *pDesc;
    pOutDesc->MipLevels = (*pMtlDescOut).mipmap_level_count;
  }
  return hr;
}

template <>
void GetMipmapSize(const D3D11_TEXTURE1D_DESC *pDesc, uint32_t level,
                   uint32_t *pWidth, uint32_t *pHeight, uint32_t *pDepth) {
  *pHeight = 1;
  *pDepth = 1;
  *pWidth = std::max(1u, pDesc->Width >> level);
}

template <>
void GetMipmapSize(const D3D11_TEXTURE2D_DESC *pDesc, uint32_t level,
                   uint32_t *pWidth, uint32_t *pHeight, uint32_t *pDepth) {
  *pDepth = 1;
  *pWidth = std::max(1u, pDesc->Width >> level);
  *pHeight = std::max(1u, pDesc->Height >> level);
}

template <>
void GetMipmapSize(const D3D11_TEXTURE2D_DESC1 *pDesc, uint32_t level,
                   uint32_t *pWidth, uint32_t *pHeight, uint32_t *pDepth) {
  *pDepth = 1;
  *pWidth = std::max(1u, pDesc->Width >> level);
  *pHeight = std::max(1u, pDesc->Height >> level);
}

template <>
void GetMipmapSize(const D3D11_TEXTURE3D_DESC *pDesc, uint32_t level,
                   uint32_t *pWidth, uint32_t *pHeight, uint32_t *pDepth) {
  *pWidth = std::max(1u, pDesc->Width >> level);
  *pHeight = std::max(1u, pDesc->Height >> level);
  *pDepth = std::max(1u, pDesc->Depth >> level);
}

template <>
void GetMipmapSize(const D3D11_TEXTURE3D_DESC1 *pDesc, uint32_t level,
                   uint32_t *pWidth, uint32_t *pHeight, uint32_t *pDepth) {
  *pWidth = std::max(1u, pDesc->Width >> level);
  *pHeight = std::max(1u, pDesc->Height >> level);
  *pDepth = std::max(1u, pDesc->Depth >> level);
}

template <>
HRESULT
GetLinearTextureLayout(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE1D_DESC &Desc, uint32_t level, uint32_t &BytesPerRow,
    uint32_t &BytesPerImage, uint32_t &BytesPerSlice, bool Aligned
) {
  auto metal = pDevice->GetMTLDevice();
  MTL_DXGI_FORMAT_DESC metal_format;

  if (FAILED(MTLQueryDXGIFormat(metal, Desc.Format, metal_format))) {
    ERR("GetLinearTextureLayout: invalid format: ", Desc.Format);
    return E_FAIL;
  }
  if (metal_format.PixelFormat == WMTPixelFormatInvalid) {
    ERR("GetLinearTextureLayout: invalid format: ", Desc.Format);
    return E_FAIL;
  }
  if (metal_format.BytesPerTexel == 0) {
    ERR("GetLinearTextureLayout: not an ordinary or packed format: ", Desc.Format);
    return E_FAIL;
  }
  uint32_t w, h, d;
  GetMipmapSize(&Desc, level, &w, &h, &d);
  auto bytes_per_row_unaligned = metal_format.BytesPerTexel * w;
  auto alignment = std::max(1u, metal_format.BytesPerTexel);
  if (Aligned && !(metal_format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    alignment = metal.minimumLinearTextureAlignmentForPixelFormat(metal_format.PixelFormat);
  }
  auto aligned_bytes_per_row = align(bytes_per_row_unaligned, alignment);
  BytesPerRow = aligned_bytes_per_row;
  BytesPerImage = aligned_bytes_per_row;
  BytesPerSlice = aligned_bytes_per_row;
  return S_OK;
};

template <>
HRESULT
GetLinearTextureLayout(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 &Desc, uint32_t level, uint32_t &BytesPerRow,
    uint32_t &BytesPerImage, uint32_t &BytesPerSlice, bool Aligned
) {
  auto metal = pDevice->GetMTLDevice();
  MTL_DXGI_FORMAT_DESC metal_format;

  if (FAILED(MTLQueryDXGIFormat(metal, Desc.Format, metal_format))) {
    ERR("GetLinearTextureLayout: invalid format: ", Desc.Format);
    return E_FAIL;
  }
  if (metal_format.PixelFormat == WMTPixelFormatInvalid) {
    ERR("GetLinearTextureLayout: invalid format: ", Desc.Format);
    return E_FAIL;
  }
  if (metal_format.BytesPerTexel == 0) {
    ERR("GetLinearTextureLayout: not an ordinary or packed format: ", Desc.Format);
    return E_FAIL;
  }
  uint32_t w, h, d;
  GetMipmapSize(&Desc, level, &w, &h, &d);
  if (metal_format.Flag & MTL_DXGI_FORMAT_BC) {
    D3D11_ASSERT(w != 0 && h != 0);
    auto w_physical = align(w, 4);
    auto h_physical = align(h, 4);
    auto aligned_bytes_per_row = metal_format.BytesPerTexel * w_physical >> 2;
    BytesPerRow = aligned_bytes_per_row; // 1 row of block is 4 row of pixel
    BytesPerImage = aligned_bytes_per_row * h_physical >> 2;
    BytesPerSlice = aligned_bytes_per_row * h_physical >> 2;
    return S_OK;
  }
  auto bytes_per_row_unaligned = metal_format.BytesPerTexel * w;
  uint64_t alignment = std::max(1u, metal_format.BytesPerTexel);
  if (Aligned && !(metal_format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    alignment = metal.minimumLinearTextureAlignmentForPixelFormat(metal_format.PixelFormat);
  }
  if (metal_format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    MTLQueryDXGIFormat(metal, Desc.Format, metal_format, 1);
    if (Aligned)
      alignment = std::max(alignment, metal.minimumLinearTextureAlignmentForPixelFormat(metal_format.PixelFormat));
    h += h / metal_format.SubsampleHeight;
  }
  auto aligned_bytes_per_row = align(bytes_per_row_unaligned, alignment);
  BytesPerRow = aligned_bytes_per_row;
  BytesPerImage = aligned_bytes_per_row * h;
  BytesPerSlice = aligned_bytes_per_row * h;
  return S_OK;
};

template <>
HRESULT
GetLinearTextureLayout(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE3D_DESC1 &Desc, uint32_t level, uint32_t &BytesPerRow,
    uint32_t &BytesPerImage, uint32_t &BytesPerSlice, bool Aligned
) {
  auto metal = pDevice->GetMTLDevice();
  MTL_DXGI_FORMAT_DESC metal_format;

  if (FAILED(MTLQueryDXGIFormat(metal, Desc.Format, metal_format))) {
    ERR("GetLinearTextureLayout: invalid format: ", Desc.Format);
    return E_FAIL;
  }
  if (metal_format.PixelFormat == WMTPixelFormatInvalid) {
    ERR("GetLinearTextureLayout: invalid format: ", Desc.Format);
    return E_FAIL;
  }
  if (metal_format.BytesPerTexel == 0) {
    ERR("GetLinearTextureLayout: not an ordinary or packed format: ", Desc.Format);
    return E_FAIL;
  }
  uint32_t w, h, d;
  GetMipmapSize(&Desc, level, &w, &h, &d);
  if (metal_format.Flag & MTL_DXGI_FORMAT_BC) {
    D3D11_ASSERT(w != 0 && h != 0);
    auto w_physical = align(w, 4);
    auto h_physical = align(h, 4);
    auto aligned_bytes_per_row = metal_format.BytesPerTexel * w_physical >> 2;
    BytesPerRow = aligned_bytes_per_row; // 1 row of block is 4 row of pixel
    BytesPerImage = aligned_bytes_per_row * h_physical >> 2;
    BytesPerSlice = aligned_bytes_per_row * h_physical * d >> 2;
    return S_OK;
  }
  auto bytes_per_row_unaligned = metal_format.BytesPerTexel * w;
  auto alignment = std::max(1u, metal_format.BytesPerTexel);
  if (Aligned && !(metal_format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    alignment = metal.minimumLinearTextureAlignmentForPixelFormat(metal_format.PixelFormat);
  }
  auto aligned_bytes_per_row = align(bytes_per_row_unaligned, alignment);
  BytesPerRow = aligned_bytes_per_row;
  BytesPerImage = aligned_bytes_per_row * h;
  BytesPerSlice = aligned_bytes_per_row * h * d;
  return S_OK;
};

template <>
void DowngradeResourceDescription<D3D11_TEXTURE2D_DESC1, D3D11_TEXTURE2D_DESC>(
    D3D11_TEXTURE2D_DESC1 const &source, D3D11_TEXTURE2D_DESC *dst) {
  dst->Width = source.Width;
  dst->Height = source.Height;
  dst->MipLevels = source.MipLevels;
  dst->ArraySize = source.ArraySize;
  dst->Format = source.Format;
  dst->SampleDesc = source.SampleDesc;
  dst->Usage = source.Usage;
  dst->BindFlags = source.BindFlags;
  dst->CPUAccessFlags = source.CPUAccessFlags;
  dst->MiscFlags = source.MiscFlags;
}

template <>
void UpgradeResourceDescription<D3D11_TEXTURE2D_DESC, D3D11_TEXTURE2D_DESC1>(
    D3D11_TEXTURE2D_DESC const *source, D3D11_TEXTURE2D_DESC1 &dst) {
  dst.Width = source->Width;
  dst.Height = source->Height;
  dst.MipLevels = source->MipLevels;
  dst.ArraySize = source->ArraySize;
  dst.Format = source->Format;
  dst.SampleDesc = source->SampleDesc;
  dst.Usage = source->Usage;
  dst.BindFlags = source->BindFlags;
  dst.CPUAccessFlags = source->CPUAccessFlags;
  dst.MiscFlags = source->MiscFlags;
  dst.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
}

template <>
void DowngradeResourceDescription<D3D11_TEXTURE3D_DESC1, D3D11_TEXTURE3D_DESC>(
    D3D11_TEXTURE3D_DESC1 const &source, D3D11_TEXTURE3D_DESC *dst) {
  dst->Width = source.Width;
  dst->Height = source.Height;
  dst->Depth = source.Depth;
  dst->MipLevels = source.MipLevels;
  dst->Format = source.Format;
  dst->Usage = source.Usage;
  dst->BindFlags = source.BindFlags;
  dst->CPUAccessFlags = source.CPUAccessFlags;
  dst->MiscFlags = source.MiscFlags;
}

template <>
void UpgradeResourceDescription<D3D11_TEXTURE3D_DESC, D3D11_TEXTURE3D_DESC1>(
    D3D11_TEXTURE3D_DESC const *source, D3D11_TEXTURE3D_DESC1 &dst) {
  dst.Width = source->Width;
  dst.Height = source->Height;
  dst.Depth = source->Depth;
  dst.MipLevels = source->MipLevels;
  dst.Format = source->Format;
  dst.Usage = source->Usage;
  dst.BindFlags = source->BindFlags;
  dst.CPUAccessFlags = source->CPUAccessFlags;
  dst.MiscFlags = source->MiscFlags;
  dst.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
}

}; // namespace mullion