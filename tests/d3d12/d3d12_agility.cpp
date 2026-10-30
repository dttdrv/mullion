// contract: D3D12GetInterface returns ID3D12SDKConfiguration for CLSID_D3D12SDKConfiguration (Microsoft Learn,
// D3D12GetInterface; GitHub #63). unknown CLSID/IID and a null output are E_NOINTERFACE / E_POINTER. SetSDKVersion
// succeeds so a title that enables the Agility SDK still creates a device of this implementation.
#include "d3d12_test.hpp"

#ifndef __ID3D12SDKConfiguration_INTERFACE_DEFINED__
#define __ID3D12SDKConfiguration_INTERFACE_DEFINED__
MIDL_INTERFACE("e9eb5314-2692-4bea-872b-3c135d71eec4")
ID3D12SDKConfiguration : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion, LPCSTR SDKPath) = 0;
};
#endif

static const GUID kCLSID_D3D12SDKConfiguration = {
    0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xd5}};

int
main() {
  // mingw's d3d12 import library has no D3D12GetInterface; the export is this build's (d3d12.def)
  auto get_interface = (HRESULT(WINAPI *)(REFCLSID, REFIID, void **))GetProcAddress(
      GetModuleHandleA("d3d12.dll"), "D3D12GetInterface");
  expect(get_interface != nullptr, "d3d12.dll has no D3D12GetInterface export");
  if (!get_interface)
    return verdict();

  expect(get_interface(kCLSID_D3D12SDKConfiguration, __uuidof(ID3D12SDKConfiguration), nullptr) == E_POINTER,
         "a null output was accepted");

  void *unknown_clsid = reinterpret_cast<void *>(1);
  GUID nobody = {0x11111111, 0x2222, 0x3333, {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}};
  expect(get_interface(nobody, __uuidof(IUnknown), &unknown_clsid) == E_NOINTERFACE && unknown_clsid == nullptr,
         "an unknown CLSID did not fail cleanly");

  ComPtr<ID3D12SDKConfiguration> config;
  CHECK(get_interface(kCLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config)));
  expect(config != nullptr, "CLSID_D3D12SDKConfiguration made no object");
  CHECK(config->SetSDKVersion(616, ".\\D3D12"));
  ComPtr<IUnknown> unknown;
  CHECK(config.As(&unknown));
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  void *wrong_iid = reinterpret_cast<void *>(1);
  expect(get_interface(kCLSID_D3D12SDKConfiguration, nobody, &wrong_iid) == E_NOINTERFACE && wrong_iid == nullptr,
         "an unknown IID of the SDK configuration did not fail cleanly");

  return verdict();
}
