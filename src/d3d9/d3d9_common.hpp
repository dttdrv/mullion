// what every object of Direct3D 9's has in common: a reference count that frees
// it with its last reference (IUnknown), and the conversions between the
// formats and presentation intervals D3D9 names and the ones DXGI and D3D11 do.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include "log/log.hpp"
#include "util_string.hpp"

namespace mullion::d3d9 {

using Microsoft::WRL::ComPtr;

// The count an object's interfaces share, freed with its last reference, as IUnknown's Release frees it. A second,
// private count is the library's own: a child of the D3D9 device holds one on the device, so a resource that the
// application kept can outlive the application's device reference without the two keeping each other alive. The last
// public reference calls on_last_reference first, where the device lets go of the children it owned; the object is
// then freed when both counts are zero.
class Object9 {
public:
  Object9() = default;
  Object9(const Object9 &) = delete;
  Object9 &operator=(const Object9 &) = delete;

  ULONG AddRef() { return ++references_; }

  // the references an application has on the object, apart from the private ones this build's objects hold on their
  // parents: a call that needs the application to have released a resource (Reset on its swap chain) counts these
  ULONG public_references() const {
    return references_.load();
  }

  ULONG Release() {
    const ULONG remaining = --references_;
    if (!remaining) {
      on_last_reference();
      ReleasePrivate();
    }
    return remaining;
  }

  void AddRefPrivate() { ++private_references_; }

  ULONG ReleasePrivate() {
    ULONG remaining = private_references_.load();
    if (!remaining) {
      if (!references_.load())
        delete this;
      return 0;
    }
    remaining = --private_references_;
    if (!remaining && !references_.load())
      delete this;
    return remaining;
  }

protected:
  virtual ~Object9() = default;
  // the last public reference is gone: an object that owns children lets go of them here, before its own private
  // reference is dropped (the device's children hold private references on it)
  virtual void on_last_reference() {}

private:
  std::atomic<ULONG> references_{1};
  std::atomic<ULONG> private_references_{0};
};

// a D3D9 resource's private data (IDirect3DResource9::SetPrivateData and its siblings): bytes by GUID, or an IUnknown
// that the resource holds a reference on. D3DSPD_IUNKNOWN stores the pointer the caller passed, AddRef'd; GetPrivateData
// returns it AddRef'd, so the caller releases it as it would any COM pointer.
struct GuidLess {
  bool operator()(const GUID &a, const GUID &b) const {
    return std::memcmp(&a, &b, sizeof(GUID)) < 0;
  }
};

class PrivateData9 {
public:
  PrivateData9() = default;
  PrivateData9(const PrivateData9 &) = delete;
  PrivateData9 &operator=(const PrivateData9 &) = delete;
  ~PrivateData9() {
    for (auto &pair : data_)
      if (pair.second.unknown)
        pair.second.unknown->Release();
  }

  HRESULT set(REFGUID guid, const void *data, DWORD size, DWORD flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((flags & D3DSPD_IUNKNOWN) && (!data || size != sizeof(IUnknown *)))
      return D3DERR_INVALIDCALL;
    if (!(flags & D3DSPD_IUNKNOWN) && size && !data)
      return D3DERR_INVALIDCALL;

    Entry &entry = data_[guid];
    if (entry.unknown)
      entry.unknown->Release();
    entry = {};
    if (flags & D3DSPD_IUNKNOWN) {
      entry.unknown = *static_cast<IUnknown *const *>(data);
      if (entry.unknown)
        entry.unknown->AddRef();
      entry.size = sizeof(IUnknown *);
    } else {
      entry.bytes.assign(static_cast<const char *>(data), static_cast<const char *>(data) + size);
      entry.size = size;
    }
    return S_OK;
  }

  HRESULT get(REFGUID guid, void *data, DWORD *size) const {
    if (!size)
      return D3DERR_INVALIDCALL;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = data_.find(guid);
    if (found == data_.end())
      return D3DERR_NOTFOUND;
    const Entry &entry = found->second;
    if (!data) {
      *size = entry.size;
      return S_OK;
    }
    if (*size < entry.size)
      return D3DERR_MOREDATA;
    if (entry.unknown) {
      *static_cast<IUnknown **>(data) = entry.unknown;
      entry.unknown->AddRef();
    } else if (entry.size) {
      std::memcpy(data, entry.bytes.data(), entry.size);
    }
    *size = entry.size;
    return S_OK;
  }

  HRESULT free(REFGUID guid) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = data_.find(guid);
    if (found == data_.end())
      return D3DERR_NOTFOUND;
    if (found->second.unknown)
      found->second.unknown->Release();
    data_.erase(found);
    return S_OK;
  }

private:
  struct Entry {
    std::vector<char> bytes;
    IUnknown *unknown = nullptr;
    DWORD size = 0;
  };

  std::map<GUID, Entry, GuidLess> data_;
  mutable std::mutex mutex_;
};

// the device's own interface for the children it makes: a child holds a private reference on the device through it, and
// QueryInterface for this id adds that reference (the returned pointer releases it through release_device_ref)
extern const IID IID_IMTLD3D9DeviceLifetime;

struct IMTLD3D9DeviceLifetime {
  virtual void release_device_ref() = 0;

protected:
  ~IMTLD3D9DeviceLifetime() = default;
};

// a resource a Reset rebuilds on the device it makes: the managed pool survives a Reset (IDirect3DDevice9::Reset:
// "D3DPOOL_MANAGED resources ... are not affected by a device reset"), so what such a resource holds on the CPU is put
// back on the new device. A default-pool resource has no such content: the application releases it before the Reset
// ("an application should release any ... D3DPOOL_DEFAULT resources"), and a stale one is left alone rather than
// rebuilt as something the application did not make.
struct IMTLD3D9ResetTarget {
  virtual void reset_device(ID3D11Device *device, const ComPtr<ID3D11DeviceContext> &context) = 0;

protected:
  ~IMTLD3D9ResetTarget() = default;
};

// the device's own interface for the resources that follow it through a Reset: the device keeps the ones it made and
// asks them to rebuild on the device a Reset makes
extern const IID IID_IMTLD3D9DeviceResources;

struct IMTLD3D9DeviceResources {
  virtual void track_reset_target(IMTLD3D9ResetTarget *target) = 0;
  virtual void untrack_reset_target(IMTLD3D9ResetTarget *target) = 0;
  // the private reference QueryInterface for IID_IMTLD3D9DeviceResources added: the same reference a child's
  // DeviceChildRef holds, so a resource registered with the device keeps the device object alive for as long as it is
  virtual void release_device_ref() = 0;

protected:
  ~IMTLD3D9DeviceResources() = default;
};

// a resource's registration with its device, so a Reset finds it: attached in the resource's constructor and released
// when the resource is destroyed, so the device never holds a pointer to one that is gone
class DeviceResourceRef {
public:
  DeviceResourceRef() = default;
  DeviceResourceRef(IDirect3DDevice9 *device, IMTLD3D9ResetTarget *target) {
    attach(device, target);
  }
  ~DeviceResourceRef() {
    detach();
  }
  DeviceResourceRef(const DeviceResourceRef &) = delete;
  DeviceResourceRef &operator=(const DeviceResourceRef &) = delete;

  void attach(IDirect3DDevice9 *device, IMTLD3D9ResetTarget *target) {
    detach();
    IMTLD3D9DeviceResources *resources = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(IID_IMTLD3D9DeviceResources, reinterpret_cast<void **>(&resources)))) {
      resources_ = resources;
      target_ = target;
      resources_->track_reset_target(target_);
    }
  }

  void detach() {
    if (resources_) {
      resources_->untrack_reset_target(target_);
      resources_->release_device_ref();
      resources_ = nullptr;
      target_ = nullptr;
    }
  }

private:
  IMTLD3D9DeviceResources *resources_ = nullptr;
  IMTLD3D9ResetTarget *target_ = nullptr;
};

// a child resource's private reference on its device: attached in the child's constructor, released when the child is
// destroyed. QueryInterface for IID_IMTLD3D9DeviceLifetime has already added the reference; this releases it.
class DeviceChildRef {
public:
  DeviceChildRef() = default;
  explicit DeviceChildRef(IDirect3DDevice9 *device) {
    attach(device);
  }
  ~DeviceChildRef() {
    detach();
  }
  DeviceChildRef(const DeviceChildRef &) = delete;
  DeviceChildRef &operator=(const DeviceChildRef &) = delete;

  void attach(IDirect3DDevice9 *device) {
    detach();
    IMTLD3D9DeviceLifetime *lifetime = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(IID_IMTLD3D9DeviceLifetime, reinterpret_cast<void **>(&lifetime))))
      lifetime_ = lifetime;
  }

  void detach() {
    if (lifetime_) {
      lifetime_->release_device_ref();
      lifetime_ = nullptr;
    }
  }

private:
  IMTLD3D9DeviceLifetime *lifetime_ = nullptr;
};

// D3D9's identity matrix, which a device's world, view and projection transforms start as
// (IDirect3DDevice9::SetTransform: "the default transform is the identity matrix")
inline D3DMATRIX identity_matrix() {
  D3DMATRIX matrix{};
  for (int at = 0; at < 4; at++)
    matrix.m[at][at] = 1.0f;
  return matrix;
}

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
  // the DXT formats are the block-compressed ones D3D11 names BC1 to BC3: a game's compressed textures are uploaded as
  // the blocks they were authored as, and sampled decompressed
  case D3DFMT_DXT1:
    return DXGI_FORMAT_BC1_UNORM;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
    return DXGI_FORMAT_BC2_UNORM;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return DXGI_FORMAT_BC3_UNORM;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// how many bytes a four-by-four block of a block-compressed format takes, and 0 for a format that is not one: DXT1's
// block is two colours and two bits a pixel, the others add the alpha bit masks
inline UINT block_size(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_DXT1:
    return 8;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return 16;
  default:
    return 0;
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

// the description a surface of a texture has in Direct3D 9's terms: its pool is
// the pool of the resource it belongs to (a level of a managed texture is
// managed, a target is default, a readback surface is system memory), and this
// build's surfaces carry no usage of their own
inline D3DSURFACE_DESC to_surface_desc(const D3D11_TEXTURE2D_DESC &texture, D3DFORMAT format, D3DPOOL pool,
                                       UINT level = 0) {
  D3DSURFACE_DESC desc{};
  desc.Format = format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = 0;
  desc.Pool = pool;
  desc.MultiSampleType = texture.SampleDesc.Count > 1 ? (D3DMULTISAMPLE_TYPE)texture.SampleDesc.Count
                                                      : D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = texture.SampleDesc.Quality;
  // a level of a texture is as many pixels as the chain halves down to it
  desc.Width = std::max(1u, texture.Width >> level);
  desc.Height = std::max(1u, texture.Height >> level);
  return desc;
}

} // namespace mullion::d3d9
