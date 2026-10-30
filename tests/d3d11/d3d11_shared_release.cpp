// contract: a shared texture may be released on one thread while another opens it from its handle. the handle is
// valid while a resource of the share exists anywhere ("If no resource objects exist on any devices that refer to this
// resource, the handle is no longer valid", IDXGIResource::GetSharedHandle), so an open made as the last resource goes
// gives a texture of the share or fails, and neither thread touches the share after the other freed it.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
// each round, the opener opens the share and releases it again while the creator releases the share's last texture.
#include "d3d11_test.hpp"
#include <atomic>
#include <thread>

int
main() {
  ComPtr<ID3D11Device> creator, opener;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &creator,
                          nullptr, nullptr));
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &opener,
                          nullptr, nullptr));
  const D3D11_TEXTURE2D_DESC desc{16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
  constexpr UINT rounds = 2000;
  std::atomic<UINT> opened = 0;
  for (UINT round = 0; round < rounds; round++) {
    step("round %u", round);
    ComPtr<ID3D11Texture2D> texture;
    CHECK(creator->CreateTexture2D(&desc, nullptr, &texture));
    ComPtr<IDXGIResource> resource;
    CHECK(texture.As(&resource));
    HANDLE handle;
    CHECK(resource->GetSharedHandle(&handle));
    resource.Reset();
    std::thread open([&] {
      ComPtr<ID3D11Texture2D> other;
      if (FAILED(opener->OpenSharedResource(handle, IID_PPV_ARGS(&other))))
        return;
      D3D11_TEXTURE2D_DESC got;
      other->GetDesc(&got);
      expect(got.Width == desc.Width && got.Height == desc.Height && got.Format == desc.Format,
             "the opened texture is %ux%u of format %u", got.Width, got.Height, got.Format);
      opened++;
    });
    texture.Reset();
    open.join();
  }
  printf("%u of %u opens found the share before its last texture went\n", opened.load(), rounds);
  return verdict();
}
