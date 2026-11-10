#pragma once
#include <array>
#include <functional>

#include "Metal.hpp"
#include "dxgi.h"
#include <map>

namespace mullion {

// dxgiformat.h of the toolchain ends before the Agility SDK's 4-bit format
constexpr auto DXGI_FORMAT_A4B4G4R4_UNORM = (DXGI_FORMAT)191;
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat ConvertSwapChainFormat(DXGI_FORMAT format);
bool IsFlipSwapChainFormat(DXGI_FORMAT format);

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

uint32_t DXGIFormatType(uint32_t format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

// mipmap generation of a8unorm is borked, so it goes through an r8unorm view
inline WMTPixelFormat
MipmapFormat(WMTPixelFormat format) {
  return format == WMTPixelFormatA8Unorm ? WMTPixelFormatR8Unorm : format;
}

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
  MTL_DXGI_FORMAT_VIDEO_PLANAR = 1024,
  MTL_DXGI_FORMAT_EMULATED_RGB32 = 2048,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTPixelFormat BufferPixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint16_t PlanarCount;
  uint16_t Flag;
  uint32_t PlaneFormat = 0;
  uint16_t SubsampleWidth = 1;
  uint16_t SubsampleHeight = 1;
};

int32_t MTLQueryDXGIFormat(
    WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description, uint32_t plane = ~0u
);

int32_t MTLQueryDXGIFormatSupport(
    WMT::Device device, uint32_t format, const std::function<FormatCapability(WMTPixelFormat)> &GetMTLPixelFormatCapability,
    uint32_t &support1, uint32_t &support2
);

void CopyRGB32(void *dst, const void *src, size_t count, bool expand);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

FormatCapability MTLGetDXGIFormatCapability(const MTL_DXGI_FORMAT_DESC &format, FormatCapability capability);

// a typed unordered access view is of a buffer as well as a texture: `texture` is the format's capability, `buffer`
// its buffer pixel format's
bool TypedUnorderedAccess(FormatCapability texture, FormatCapability buffer);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

// how a uint clear's channel bits (D3D11.3 5.2.3.2) are written through a texture's own format to store bit for bit,
// where the texture has no integer view (Metal's format view, which takes the texture's lossless compression): as the
// values unorm bits encode, as halves, as floats, or as the floats RG11B10Float's bits stand for (SmallFloat, whose
// NaNs Direct3D 12 writes with a blit: BlitNaNClear). other formats lose patterns that way (snorm's -128 has -127's
// value) and need an integer view
enum class UIntClearWrite { IntegerView, Unorm, Half, Float, SmallFloat };
UIntClearWrite GetUIntClearWrite(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

// whether a texture of the format may be viewed in another of its type group that Metal reinterprets only through a
// pixel format view, which it waives for sRGB twins. `fully_typed` takes D3D12's casts of fully typed resources, which
// keep _FLOAT and other formats apart (DirectX-Specs, D3D12 Relaxed Format Casting Rules)
bool CastsAcrossLayouts(WMT::Device device, uint32_t format, bool fully_typed);

// whether a texture of the format takes an R32_UINT, R32_SINT or R32_FLOAT unordered access view, though R32 is not
// of its type group: "D3D allows Texture1D/2D/3D resources created with any of the following small set of 32-bit per
// element formats to have UAVs created from them with R32_UINT/_SINT/_FLOAT as the type" (D3D11.3 5.3.9.5, which
// D3D12 keeps: vkd3d-proton's test_typed_srv_uav_cast). R32_TYPELESS, the set's sixth, is R32's own group
bool CastsToR32UnorderedAccess(uint32_t format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

// the stored channel red, green, blue and alpha each land in: a view's component swizzle is undone for render target
// writes, clears and write masks, which bypass it (B4G4R4A4 stores its channels rotated)
std::array<uint8_t, 4> RenderTargetChannels(WMTPixelFormat format);

WMTColorWriteMask RenderTargetWriteMask(WMTPixelFormat format, WMTColorWriteMask mask);

// a clear color in the channels a render target stores
std::array<float, 4> RenderTargetColor(WMTPixelFormat format, const float color[4]);

// the range a border color clamps to before filtering (D3D11.3 7.18.9.1): normalized formats' own, the rest as their
// clear colors clamp
std::array<float, 2> BorderColorRange(const MTL_DXGI_FORMAT_DESC &description);

} // namespace mullion