// contract: D3D12GetInterface with CLSID_D3D12SDKConfiguration gives an ID3D12SDKConfiguration whose SetSDKVersion
// accepts the SDK an Agility SDK title names, and devices are created as before. "Sets the SDK version and the path
// to the Direct3D 12 runtime that you want to use" and "Agility SDK" apps call it before D3D12CreateDevice
// (Microsoft Learn, ID3D12SDKConfiguration::SetSDKVersion). D3D12GetInterface "gets a debug interface or SDK
// configuration interface" and takes "the class ID (CLSID) of the object" (Microsoft Learn, D3D12GetInterface).
#include "d3d12_test.hpp"

// CLSID_D3D12SDKConfiguration, d3d12.h (Microsoft DirectX-Headers)
static const GUID clsid_sdk_configuration = {
    0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};

// mingw's d3d12 import library has no D3D12GetInterface; applications find it in d3d12.dll
typedef HRESULT(WINAPI *get_interface_fn)(REFCLSID, REFIID, void **);

int
main(int argc, char **argv) {
  ComPtr<ID3D12SDKConfiguration> config;
  ComPtr<IUnknown> unknown;
  ComPtr<ID3D12Device> device;
  get_interface_fn D3D12GetInterface =
      (get_interface_fn)(void *)GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12GetInterface");

  if (!D3D12GetInterface) {
    printf("failed: d3d12.dll exports no D3D12GetInterface\n");
    return 1;
  }
  CHECK(D3D12GetInterface(clsid_sdk_configuration, IID_PPV_ARGS(&config)));
  if (!config) {
    printf("failed: no ID3D12SDKConfiguration\n");
    return 1;
  }
  // the Agility SDK's version and the folder its D3D12Core.dll would be in, as an application gives them
  CHECK(config->SetSDKVersion(616, ".\\D3D12\\"));
  CHECK(config->QueryInterface(IID_PPV_ARGS(&unknown)));

  if (D3D12GetInterface(clsid_sdk_configuration, __uuidof(ID3D12SDKConfiguration), nullptr) != E_POINTER) {
    printf("failed: a null output was accepted\n");
    return 1;
  }

  // the configuration changes nothing about device creation
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  printf("passed: ID3D12SDKConfiguration accepted SetSDKVersion, device created\n");
  return 0;
}
