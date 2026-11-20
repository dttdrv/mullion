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

// d3d11.dll's creation entry, which this build's own generations reach it through: Direct3D 10 is implemented over
// the Direct3D 11 device and the Direct3D 9 device makes one to draw over (docs/ARCHITECTURE.md section 1). This
// tree's DirectX headers do not declare it, and it is not the API a title calls for its own device -- that is
// D3D11CreateDevice, which the router decides. A bit in Flags says who is asking, because a value carried in the
// call is the only thing that crosses the boundary between two of this build's DLLs (d3d11.cpp's
// MULLION_INTERNAL_DEVICE, MULLION_D3D10_DEVICE).
extern "C" HRESULT WINAPI
D3D11CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, const D3D_FEATURE_LEVEL *levels,
                      UINT level_count, ID3D11Device **device);

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

  ULONG AddRef() {
    const ULONG now = ++references_;
    // the application's first reference on an implicit object is what gives it its device reference (Wine's
    // test_refcount: the Get that hands the object out is what the device's count rises with)
    if (implicit_.load() && now == 1)
      on_implicit_acquire();
    return now;
  }

  // the references an application has on the object, apart from the private ones this build's objects hold on their
  // parents: a call that needs the application to have released a resource (Reset on its swap chain) counts these
  ULONG public_references() const {
    return references_.load();
  }

  ULONG Release() {
    ULONG remaining = references_.load();
    while (remaining) {
      if (references_.compare_exchange_weak(remaining, remaining - 1))
        break;
    }
    // D3D9's implicit objects live at reference count 0: a Release that finds one there returns 0 without
    // underflowing, as the reference D3D9 returns does
    if (!remaining)
      return 0;
    remaining--;
    if (!remaining) {
      if (implicit_.load()) {
        // the application's last reference gives up the device reference the first one took, and the object stays:
        // the device frees its implicit chain and back buffer with itself
        on_implicit_release();
        return 0;
      }
      // on_last_reference may unbind and ReleasePrivate; that must not delete underneath this Release
      releasing_public_ = true;
      on_last_reference();
      releasing_public_ = false;
      // an object the device keeps in a cache (SetFVF's declarations) is freed by that cache, and one the device bound
      // (a render target or depth-stencil surface) is kept alive by the private reference the device holds: the
      // application's last reference frees only an object no one else holds
      if (!device_owned_ && !private_references_.load())
        delete this;
    }
    return remaining;
  }

  // a cached object whose public reference count may reach zero while the device still uses it
  void set_device_owned(bool owned) { device_owned_ = owned; }
  void destroy_device_owned() {
    device_owned_ = false;
    if (!references_.load())
      delete this;
  }

  // an object D3D9 makes at reference count 0: the implicit swap chain and its back and depth-stencil surfaces. It is
  // not freed by a public Release, and its first public reference adds the one device reference it holds until its last
  // public reference goes (Wine's test_refcount, device.c:1540)
  void set_implicit() {
    implicit_ = true;
    references_.store(0);
  }

  // the device frees an implicit object with itself, whatever public references the application still has
  void destroy_implicit() {
    implicit_ = false;
    delete this;
  }

  // D3D9Ex Reset: an application-held implicit surface becomes a normal object (Wine test_backbuffer_resize).
  void clear_implicit() { implicit_ = false; }

  bool implicit() const { return implicit_.load(); }

  void AddRefPrivate() { ++private_references_; }

  ULONG ReleasePrivate() {
    ULONG remaining = private_references_.load();
    if (!remaining) {
      // Implicit objects (chain back buffers, auto depth-stencil) live until destroy_implicit, even at public 0.
      // While public Release runs on_last_reference, that path owns final deletion — do not delete here.
      if (!references_.load() && !implicit_.load() && !device_owned_ && !releasing_public_)
        delete this;
      return 0;
    }
    remaining = --private_references_;
    if (!remaining && !references_.load() && !implicit_.load() && !device_owned_ && !releasing_public_)
      delete this;
    return remaining;
  }

protected:
  virtual ~Object9() = default;
  // the last public reference is gone: an object that owns children lets go of them here, before its own private
  // reference is dropped (the device's children hold private references on it)
  virtual void on_last_reference() {}
  // an implicit object's public reference count crossed 0->1 or 1->0: its device reference is taken or given back
  virtual void on_implicit_acquire() {}
  virtual void on_implicit_release() {}

private:
  std::atomic<ULONG> references_{1};
  std::atomic<ULONG> private_references_{0};
  std::atomic<bool> device_owned_{false};
  std::atomic<bool> implicit_{false};
  // true while Object9::Release runs on_last_reference (unbind may ReleasePrivate without deleting)
  bool releasing_public_ = false;
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
      // D3DSPD_IUNKNOWN's data is the IUnknown itself, not a pointer to the pointer: Wine's device test passes a
      // stack object's address and reads the same pointer back from GetPrivateData
      entry.unknown = static_cast<IUnknown *>(const_cast<void *>(data));
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
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = data_.find(guid);
    // Missing GUID: NOTFOUND even when size is NULL (Wine device.c private-data tests).
    if (found == data_.end())
      return D3DERR_NOTFOUND;
    if (!size)
      return D3DERR_INVALIDCALL;
    const Entry &entry = found->second;
    if (!data) {
      *size = entry.size;
      return S_OK;
    }
    if (*size < entry.size) {
      *size = entry.size;
      return D3DERR_MOREDATA;
    }
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

struct IMTLD3D9Buffer;
struct IMTLD3D9Surface;
struct IMTLD3D9Texture;

struct IMTLD3D9DeviceResources {
  virtual void track_reset_target(IMTLD3D9ResetTarget *target) = 0;
  virtual void untrack_reset_target(IMTLD3D9ResetTarget *target) = 0;
  // the private reference QueryInterface for IID_IMTLD3D9DeviceResources added: the same reference a child's
  // DeviceChildRef holds, so a resource registered with the device keeps the device object alive for as long as it is
  virtual void release_device_ref() = 0;
  // D3D9 destroys a buffer on its last public Release even while bound: drop stream/index private refs so the
  // object can free and the device's own count can reach zero (Wine device.c test_pinned_buffers)
  virtual void unbind_buffer(IMTLD3D9Buffer *buffer) = 0;
  // last public Release of a bound render-target / depth-stencil / SetTexture resource drops the binding (Wine
  // device.c: Release(rt) while still SetRenderTarget(0, rt) then Release(device) expects 0)
  virtual void unbind_surface(IMTLD3D9Surface *surface) = 0;
  virtual void unbind_texture(IMTLD3D9Texture *texture) = 0;

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
  // `counted` is the reference D3D9's device count sees: every resource a Create* call makes holds one, while the
  // device's own implicit objects (the implicit chain, its back buffer, the automatic depth-stencil surface) hold the
  // private kind
  explicit DeviceChildRef(IDirect3DDevice9 *device, bool counted = false) {
    attach(device, counted);
  }
  ~DeviceChildRef() {
    detach();
  }
  DeviceChildRef(const DeviceChildRef &) = delete;
  DeviceChildRef &operator=(const DeviceChildRef &) = delete;

  void attach(IDirect3DDevice9 *device, bool counted = false) {
    detach();
    if (!device)
      return;
    if (counted) {
      device->AddRef();
      counted_device_ = device;
      return;
    }
    IMTLD3D9DeviceLifetime *lifetime = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_IMTLD3D9DeviceLifetime, reinterpret_cast<void **>(&lifetime))))
      lifetime_ = lifetime;
  }

  void detach() {
    if (counted_device_) {
      counted_device_->Release();
      counted_device_ = nullptr;
    }
    if (lifetime_) {
      lifetime_->release_device_ref();
      lifetime_ = nullptr;
    }
  }

private:
  IMTLD3D9DeviceLifetime *lifetime_ = nullptr;
  IDirect3DDevice9 *counted_device_ = nullptr;
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
  // D3D9's A8B8G8R8 stores R, G, B, A in ascending bytes, which is DXGI's R8G8B8A8 (D3DFMT_A8R8G8B8 is the BGRA one)
  case D3DFMT_A8B8G8R8:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case D3DFMT_R5G6B5:
    return DXGI_FORMAT_B5G6R5_UNORM;
  case D3DFMT_A16B16G16R16F:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case D3DFMT_R32F:
    return DXGI_FORMAT_R32_FLOAT;
  case D3DFMT_L8:
    return DXGI_FORMAT_R8_UNORM;
  // Bump / signed formats used by Wine visual emboss/bumpenv tests (D3DFMT_V8U8).
  case D3DFMT_V8U8:
    return DXGI_FORMAT_R8G8_SNORM;
  case D3DFMT_Q8W8V8U8:
    return DXGI_FORMAT_R8G8B8A8_SNORM;
  case D3DFMT_V16U16:
    return DXGI_FORMAT_R16G16_SNORM;
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
  // YUY2/UYVY volumes/surfaces are CPU-authored in SYSTEMMEM/SCRATCH; Metal has no Texture3D for them.
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// D3D9's ATI2 (BC5 / 3Dc) four-character code, which d3d9.h has no D3DFMT_ name for. It is a four-by-four block format
// like the DXT ones, and a SCRATCH volume texture holds it on the CPU (Wine d3d9ex.c test_resource_access test 29).
// ATI2 is a FOURCC, which D3D9 spells without a D3DFMT_ name: D3DFMT_ATI2 does not exist (Wine d3d9ex.c
// test_resource_access makes one from MAKEFOURCC('A', 'T', 'I', '2'))
inline constexpr D3DFORMAT kATI2Format = static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'I', '2'));

// how many bytes a four-by-four block of a block-compressed format takes, and 0 for a format that is not one: DXT1's
// block is two colours and two bits a pixel, the others add the alpha bit masks. YUY2/UYVY are not included here —
// Wine does not enforce their block sizes on Create* (create_size_checked is false).
inline UINT block_size(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_DXT1:
    return 8;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
  case kATI2Format:
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

// the typeless format a depth-stencil texture is made as, so that one resource can be a depth-stencil view and a
// shader resource at once: D3D11 lets a resource carry both only when its own format names neither. The read format is
// what a shader that samples it sees (Wine visual.c shadow_test samples a D24S8 depth texture).
inline DXGI_FORMAT to_typeless_depth_format(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_D16_UNORM:
    return DXGI_FORMAT_R16_TYPELESS;
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    return DXGI_FORMAT_R24G8_TYPELESS;
  case DXGI_FORMAT_D32_FLOAT:
    return DXGI_FORMAT_R32_TYPELESS;
  default:
    return format;
  }
}

inline DXGI_FORMAT to_depth_read_format(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_D16_UNORM:
    return DXGI_FORMAT_R16_UNORM;
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  case DXGI_FORMAT_D32_FLOAT:
    return DXGI_FORMAT_R32_FLOAT;
  default:
    return format;
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
  case D3DFMT_R8G8B8:
    return 3;
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
  case D3DFMT_R32F:
    return 4;
  case D3DFMT_A16B16G16R16F:
    return 8;
  case D3DFMT_L8:
  case D3DFMT_A8:
    return 1;
  case D3DFMT_V8U8:
  case D3DFMT_L6V5U5:
    return 2;
  case D3DFMT_Q8W8V8U8:
  case D3DFMT_V16U16:
  case D3DFMT_A2W10V10U10:
  case D3DFMT_X8L8V8U8:
    return 4;
  case D3DFMT_YUY2:
  case D3DFMT_UYVY:
    return 2; // average; row pitch uses format_row_pitch
  default:
    return 0;
  }
}

// Row pitch in bytes for a lock of `width` texels (Wine device.c volume/surface block lock expectations).
inline UINT format_row_pitch(D3DFORMAT format, UINT width) {
  switch (format) {
  case D3DFMT_DXT1:
    return ((width + 3) / 4) * 8;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
  case kATI2Format:
    return ((width + 3) / 4) * 16;
  case D3DFMT_YUY2:
  case D3DFMT_UYVY:
    return ((width + 1) / 2) * 4;
  default: {
    const UINT bytes = pixel_size(format);
    return bytes ? width * bytes : 0;
  }
  }
}

inline UINT format_height_blocks(D3DFORMAT format, UINT height) {
  switch (format) {
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
  case kATI2Format:
    return (height + 3) / 4;
  default:
    return height;
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

// The viewport a D3D9 viewport becomes on the D3D11 device that draws it. Direct3D 9's rasterizer samples at integer
// pixel centers, where Direct3D 10's and Metal's sample at half-integer ones (Direct3D 9 "Rasterization Rules":
// "Pixel centers are at integer coordinates"; wined3d answers the same with WINED3D_PIXEL_CENTER_INTEGER, whose
// <= d3d9 projection carries the half pixel). A D3D9 draw is therefore placed half a pixel down and right of the
// viewport a game set, so that a sample Metal takes at a half-integer coordinate lands where the D3D9 sample at an
// integer coordinate did; in clip space this is the Direct3D 9 half-pixel offset, clip.xy += inv_size * w. The D3D9
// viewport itself, which the scissor rectangle and this layer's screen-space conversions read, keeps the integers the
// game set (D3DVIEWPORT9's members are integers).
inline D3D11_VIEWPORT to_d3d11_viewport(const D3DVIEWPORT9 &viewport, float min_depth, float max_depth) {
  constexpr float half_pixel = 0.5f;
  // wined3d's compatibility rule for a degenerate range: max_z = max(max_z, min_z + 0.001f). Wine visual.c
  // test_viewport passes MinZ == MaxZ (0 and 0, then 0.5 and 0) and expects the draw at 0.001 and 0.501. D3D11
  // requires 0 <= MinDepth <= MaxDepth <= 1, so clamp only after applying the rule.
  min_depth = std::min(1.0f, std::max(0.0f, min_depth));
  max_depth = std::max(max_depth, min_depth + 0.001f);
  max_depth = std::min(1.0f, std::max(min_depth, max_depth));
  return D3D11_VIEWPORT{(float)viewport.X + half_pixel, (float)viewport.Y + half_pixel, (float)viewport.Width,
                        (float)viewport.Height, min_depth, max_depth};
}

} // namespace mullion::d3d9
