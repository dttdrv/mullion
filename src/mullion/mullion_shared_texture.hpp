#pragma once
// The format of a shared texture, which Direct3D 11 and Direct3D 12 share and which other processes read: a texture
// made by either, or by another process, opens in either. MetalVR reads the bootstrap name in `mach_port_name` from
// a handle's mapping to show an application's swapchain images (MetalVR's docs/METALSHARP_WINE.md).
#include "mullion_keyed_mutex.hpp"
#include "d3d11_3.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>

namespace mullion {

// a shared texture's handle is an ordinary file mapping whose bytes say which shared texture it belongs to: the
// carrier pattern of d3d11_fence.cpp with a tag of its own, so "a shared texture still opens as a texture, and
// cannot open as a fence" (tests/d3d11/d3d11_shared_fence.cpp). the texture itself travels as the mach port of
// its Metal allocation under a unique bootstrap name; a planar texture registers its chroma plane under
// "<name>.uv", and a keyed mutex's shared event carries its own name (tests/d3d11/d3d11_shared_resource.cpp)
struct SharedTextureData {
  char tag[16];
  uint64_t share_id;
  // the carrier's own name, so an opener that holds nothing but the share's identity - a name that resolved to it,
  // or a handle of it - opens the object that holds the state
  char mapping_name[64];
  // the name the share was published under, if one was: every resource of the share holds that name open for as
  // long as it lives, since "a name can be used by only one resource at a time" and the name belongs to the
  // resource and not to the process that happened to create it
  WCHAR published_name[MAX_PATH + 1];
  // the access this share was published with: "dwAccess ... one or more of the following values: DXGI_SHARED_RESOURCE_
  // READ 0x80000000, DXGI_SHARED_RESOURCE_WRITE 0x1" (Microsoft Learn, IDXGIResource1::CreateSharedHandle), which a
  // later OpenSharedResourceByName is checked against
  DWORD granted_access;
  char mach_port_name[54];
  char mutex_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
};

// the carrier holds the identity and, past it, the keyed mutex's state, so one object carries both: a handle of
// this share is a handle to this object - "The duplicate handle refers to the same object as the original handle"
// (Microsoft Learn, DuplicateHandle) - and so is a handle to the name the share was published under, which is why
// a handle kept after every resource of the share is released still opens the mutex the share was made with
struct SharedTextureCarrier {
  SharedTextureData data;
  SharedMutexState mutex;
};
inline constexpr size_t shared_texture_mutex_offset = offsetof(SharedTextureCarrier, mutex);

inline constexpr char shared_texture_tag[16] = "MULLION_texture";

// the share's own mapping is named after its id, so a marshalled legacy handle resolves in any process of the
// session: "You can also marshal this handle to another process to share a resource with a device in another
// process" (Microsoft Learn, IDXGIResource::GetSharedHandle). the name also gives the handle the lifetime that
// page spells out: "If no resource objects exist on any devices that refer to this resource, the handle is no
// longer valid. To extend the lifetime of the handle and video memory, you must open the shared resource on a
// device" - every live resource keeps the share's mapping open, and the name dies with the last of them
inline void
ShareMappingName(uint64_t id, char (&out)[64]) {
  snprintf(out, 64, "MULLION_shared_texture_%016llx", (unsigned long long)id);
}

// the marker bit names a legacy handle and is never part of an id. a 32-bit HANDLE and a 64-bit HANDLE of the
// same share must be the same integer: the marker is bit 30, which both widths carry, so a truncated 64-bit
// GetSharedHandle is still a legacy handle a 32-bit OpenSharedResource can name (not a width-local bit 62)
inline constexpr uint64_t legacy_handle_marker = uint64_t(1) << 30;
inline constexpr uint64_t share_id_range = uint64_t(1) << 30;

inline uint64_t
MakeShareId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  return dis(gen) & (share_id_range - 1);
}

// the bootstrap name a shared texture's Metal allocation is registered under
inline void
MakeUniqueSharedName(char (&out)[54]) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  uint64_t r1 = dis(gen);
  uint64_t r2 = dis(gen);
  std::snprintf(
      out, sizeof(out), "MULLION_shared_%016llx%016llx", static_cast<unsigned long long>(r1),
      static_cast<unsigned long long>(r2)
  );
}

} // namespace mullion
