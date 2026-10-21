// what every object of Direct3D 9's has in common: a reference count that frees
// it with its last reference (IUnknown), and the conversions between the
// formats and presentation intervals D3D9 names and the ones DXGI and D3D11 do.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include "log/log.hpp"
#include "util_string.hpp"

namespace mullion::d3d9 {

using Microsoft::WRL::ComPtr;

// the count an object's interfaces share, freed with its last reference, as
// IUnknown's Release frees it
class Object9 {
public:
  Object9() = default;
  Object9(const Object9 &) = delete;
  Object9 &operator=(const Object9 &) = delete;

  ULONG AddRef() { return ++references_; }

  ULONG Release() {
    const ULONG remaining = --references_;
    if (!remaining)
      delete this;
    return remaining;
  }

protected:
  virtual ~Object9() = default;

private:
  std::atomic<ULONG> references_{1};
};

// the DXGI format a D3D9 format is presented and read back as,
// DXGI_FORMAT_UNKNOWN when this build has none
inline DXGI_FORMAT to_dxgi_format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case D3DFMT_R5G6B5:
    return DXGI_FORMAT_B5G6R5_UNORM;
  case D3DFMT_A16B16G16R16F:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case D3DFMT_R32F:
    return DXGI_FORMAT_R32_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// the DXGI format a D3D9 depth-stencil format is made as, DXGI_FORMAT_UNKNOWN when this build has none. D3D9's D24X8
// and D24X4S4 hold no stencil, but the depth they hold is the top 24 bits of the 32 a D24S8 surface has, which is what
// this build's depth surfaces are.
inline DXGI_FORMAT to_dxgi_depth_format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D16:
    return DXGI_FORMAT_D16_UNORM;
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D24S8:
    return DXGI_FORMAT_D24_UNORM_S8_UINT;
  case D3DFMT_D32:
    return DXGI_FORMAT_D32_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// the D3D9 format a DXGI format is read back as
inline D3DFORMAT from_dxgi_format(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return D3DFMT_X8R8G8B8;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return D3DFMT_A2R10G10B10;
  case DXGI_FORMAT_B5G6R5_UNORM:
    return D3DFMT_R5G6B5;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return D3DFMT_A16B16G16R16F;
  case DXGI_FORMAT_R32_FLOAT:
    return D3DFMT_R32F;
  default:
    return D3DFMT_UNKNOWN;
  }
}

// how many refreshes a presentation waits for: D3DPRESENT_INTERVAL_IMMEDIATE
// shows a frame at once, the count of D3DPRESENT_INTERVAL_ONE to FOUR waits for
// that many, and the default is one (IDXGISwapChain::Present's SyncInterval)
inline UINT to_sync_interval(DWORD interval) {
  switch (interval) {
  case D3DPRESENT_INTERVAL_IMMEDIATE:
    return 0;
  case D3DPRESENT_INTERVAL_TWO:
    return 2;
  case D3DPRESENT_INTERVAL_THREE:
    return 3;
  case D3DPRESENT_INTERVAL_FOUR:
    return 4;
  default:
    return 1;
  }
}

// how many bytes a pixel of a format takes, 0 without one
inline UINT pixel_size(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_R5G6B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_A8R3G3B2:
  case D3DFMT_X4R4G4B4:
    return 2;
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
  case D3DFMT_R32F:
    return 4;
  case D3DFMT_A16B16G16R16F:
    return 8;
  default:
    return 0;
  }
}

// the description a surface of a texture has in Direct3D 9's terms: a lockable
// one is one whose bytes are on the CPU
inline D3DSURFACE_DESC to_surface_desc(const D3D11_TEXTURE2D_DESC &texture, D3DFORMAT format, bool lockable) {
  D3DSURFACE_DESC desc{};
  desc.Format = format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = 0;
  desc.Pool = lockable ? D3DPOOL_SYSTEMMEM : D3DPOOL_DEFAULT;
  desc.MultiSampleType = D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = 0;
  desc.Width = texture.Width;
  desc.Height = texture.Height;
  return desc;
}

} // namespace mullion::d3d9
