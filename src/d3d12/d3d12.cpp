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

#include "d3d12.h"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "dxgi_interfaces.h"
#include "dxcore_interface.h"
#include "log/log.hpp"
#include "util_md5.hpp"
#include "DXBCParser/BlobContainer.h"
#include "util_string.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <optional>

// ID3D12SDKConfiguration (d3d12.idl), which include/native/directx/d3d12.h only forward-declares (mingw's d3d12.h,
// which some targets build with, defines it). "Sets the SDK version and the path to the Direct3D 12 runtime"
// (Microsoft Learn, ID3D12SDKConfiguration::SetSDKVersion); Agility SDK titles call it before creating a device.
#ifndef __ID3D12SDKConfiguration_INTERFACE_DEFINED__
#define __ID3D12SDKConfiguration_INTERFACE_DEFINED__
MIDL_INTERFACE("e9eb5314-33aa-42b2-a718-d77f58b1f1c7")
ID3D12SDKConfiguration : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion, LPCSTR SDKPath) = 0;
};
__CRT_UUID_DECL(ID3D12SDKConfiguration, 0xe9eb5314, 0x33aa, 0x42b2, 0xa7, 0x18, 0xd7, 0x7f, 0x58, 0xb1, 0xf1, 0xc7)
#endif

namespace mullion {

Logger Logger::s_instance("d3d12.log");

extern "C" HRESULT WINAPI
D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice) {

  Com<IDXGIAdapter> dxgi_adapter = nullptr;
  Com<IDXGIFactory4> dxgi_factory = nullptr;
  Com<IMTLDXGIAdapter> dxgi_adapter_mtl = nullptr;

  if (MinimumFeatureLevel < D3D_FEATURE_LEVEL_11_0)
    return FAIL(E_INVALIDARG);

  HRESULT hr;

  if (!pAdapter) {
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));

    if (FAILED(hr)) {
      ERR("D3D12CreateDevice: Failed to create a DXGI factory");
      return hr;
    }

    if (FAILED(hr = dxgi_factory->EnumAdapters(0, &dxgi_adapter))) {
      ERR("D3D12CreateDevice: No default adapter available");
      return hr;
    }
  } else {
    dxgi_adapter = com_cast<IDXGIAdapter>(pAdapter);
    if (!dxgi_adapter) {
      auto dxcore_adapter = com_cast<IDXCoreAdapter>(pAdapter);
      if (!dxcore_adapter)
        return FAIL(E_INVALIDARG);
      LUID luid;
      if (FAILED(hr = dxcore_adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(luid), &luid)) ||
          FAILED(hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory))) ||
          FAILED(hr = dxgi_factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&dxgi_adapter))))
        return hr;
    }
  }

  if (FAILED(hr = dxgi_adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter_mtl)))) {
    ERR("D3D12CreateDevice: Not a Mullion adapter");
    return hr;
  }

  // the device, if it reaches the minimum feature level
  Com<ID3D12Device> device;
  if (FAILED(hr = mullion::CreateD3D12Device(dxgi_adapter_mtl.ptr(), IID_PPV_ARGS(&device))))
    return hr;
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1, &MinimumFeatureLevel};
  if (FAILED(hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))))
    return hr;
  if (levels.MaxSupportedFeatureLevel != MinimumFeatureLevel) {
    WARN("D3D12CreateDevice: feature level ", MinimumFeatureLevel, " is not supported");
    return E_INVALIDARG;
  }
  return ppDevice ? device->QueryInterface(riid, ppDevice) : S_FALSE;
}

static const GUID CLSID_D3D12SDKConfiguration = {
    0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};

// Mullion's d3d12.dll always loads its own d3d12core.dll, so there is no other runtime to select: any SDK
// version and path the application asks for is accepted, and its devices come from Mullion as before.
class SDKConfiguration final : public ComObject<ID3D12SDKConfiguration> {
public:
  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12SDKConfiguration)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    WARN("ID3D12SDKConfiguration: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE
  SetSDKVersion(UINT SDKVersion, LPCSTR SDKPath) override {
    TRACE("ID3D12SDKConfiguration::SetSDKVersion: ", SDKVersion, " ", SDKPath ? SDKPath : "(null)");
    return S_OK;
  }
};

extern "C" HRESULT WINAPI
D3D12GetInterface(REFCLSID rcslid, REFIID iid, void **debug) {
  if (debug)
    *debug = nullptr;
  if (rcslid == CLSID_D3D12SDKConfiguration) {
    if (!debug)
      return E_POINTER;
    Com<ID3D12SDKConfiguration> config = new SDKConfiguration();
    return config->QueryInterface(iid, debug);
  }
  WARN("D3D12GetInterface: Unknown interface query ", mullion::str::format(iid));
  return E_NOINTERFACE;
}

extern "C" HRESULT WINAPI
D3D12GetDebugInterface(REFIID iid, void **debug) {
  if (debug)
    *debug = nullptr;
  WARN("D3D12GetDebugInterface: Unknown interface query ", mullion::str::format(iid));
  return E_NOINTERFACE;
}

static std::atomic<bool> experimental_shader_models;

bool
ExperimentalShaderModelsEnabled() {
  return experimental_shader_models;
}

// the SHA1s of the containers that held, by their own hash and size: once a container's hash is checked against its
// bytes it names them, so pipelines that share shaders hash each with SHA1 once in the process. every creation still
// checks the hash (Direct3D refuses a container whose bytes it does not match). a hash of one byte value 0, 1 or 2 is
// none (md5::checkDxbcHash) and tells containers apart by nothing, so theirs are hashed each time
using OwnHash = std::pair<decltype(md5::Digest::data), size_t>;
static mullion::shared_mutex known_lock;
static std::map<OwnHash, Sha1Digest> known;

static std::optional<OwnHash>
Own(const void *container, size_t size) {
  OwnHash own{{}, size};
  auto &[hash, length] = own;
  if (!container || size < sizeof(uint32_t) + hash.size())
    return {};
  std::memcpy(hash.data(), static_cast<const uint8_t *>(container) + sizeof(uint32_t), hash.size());
  if (hash[0] <= 2 && std::ranges::all_of(hash, [&](uint8_t byte) { return byte == hash[0]; }))
    return {};
  return own;
}

bool
ShaderContainerHolds(const void *container, size_t size) {
  if (FAILED(microsoft::CDXBCParser().ReadDXBC(container, size)))
    return false;
  auto hash = md5::checkDxbcHash(container, size);
  if (auto own = Own(container, size); hash == md5::DxbcHash::Holds && own) {
    {
      std::shared_lock lock(known_lock);
      if (known.contains(*own))
        return true;
    }
    auto digest = Sha1HashState::compute(container, size);
    std::lock_guard lock(known_lock);
    known.emplace(*own, digest);
  }
  return hash == md5::DxbcHash::Holds || (hash == md5::DxbcHash::None && experimental_shader_models);
}

Sha1Digest
ShaderDigest(const void *code, size_t size) {
  if (auto own = Own(code, size)) {
    std::shared_lock lock(known_lock);
    if (auto found = known.find(*own); found != known.end())
      return found->second;
  }
  return Sha1HashState::compute(code, size);
}

bool
ShaderFeaturesSupported(MTLD3D12Device *device, const void *container, size_t size) {
  // DxilConstants.h, ShaderFeatureInfo_AdvancedTextureOps and ShaderFeatureInfo_WriteableMSAATextures (SM 6.7)
  constexpr uint64_t AdvancedTextureOps = 0x20000000, WriteableMSAATextures = 0x40000000;
  microsoft::CDXBCParser parser;
  uint64_t flags = 0;
  if (FAILED(parser.ReadDXBC(container, size)))
    return false;
  auto part = parser.FindNextMatchingBlob((microsoft::DXBCFourCC)MAKEFOURCC('S', 'F', 'I', '0'));
  if (part != DXBC_BLOB_NOT_FOUND && parser.GetBlobSize(part) >= sizeof(flags))
    memcpy(&flags, parser.GetBlob(part), sizeof(flags));
  D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options14, sizeof(options14));
  return !(flags & AdvancedTextureOps && !options14.AdvancedTextureOpsSupported) &&
         !(flags & WriteableMSAATextures && !options14.WriteableMSAATexturesSupported);
}

// features outside the released API, asked for before a device is made and granted all or none
// (D3D12EnableExperimentalFeatures: "S_OK if successful", E_NOINTERFACE for a feature that is not known). the one
// that is here is experimental shader models, with which Direct3D takes shaders that carry no hash (INF-0004)
extern "C" HRESULT WINAPI
D3D12EnableExperimentalFeatures(UINT NumFeatures, const IID *pIIDs, void *pConfigurationStructs, UINT *pConfigurationStructSizes) {
  static const GUID shader_models = {0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f}};
  if (NumFeatures && !pIIDs)
    return FAIL(E_INVALIDARG);
  if (!std::all_of(pIIDs, pIIDs + NumFeatures, [](REFIID iid) { return iid == shader_models; }))
    return E_NOINTERFACE;
  if (NumFeatures)
    experimental_shader_models = true;
  return S_OK;
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return TRUE;
}

} // namespace mullion