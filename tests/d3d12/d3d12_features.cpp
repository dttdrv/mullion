// contract: format support answers what the device can do. every format that reports render target support
// creates a render target texture, every one that reports depth stencil support creates a depth stencil texture, and
// block-compressed formats (D3D12 forbids rendering to or writing them through a typed UAV) report neither.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  unsigned checked = 0, mismatches = 0;
  auto fail = [&](const char *what, DXGI_FORMAT format) {
    if (mismatches++ < 12)
      printf("format %u: %s\n", format, what);
  };
  for (UINT f = 1; f <= DXGI_FORMAT_B4G4R4A4_UNORM; f++) {
    auto format = DXGI_FORMAT(f);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
      continue;
    checked++;
    bool compressed = (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
                      (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
    if (compressed && (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
      fail("block-compressed but reports render target", format);
    if (compressed && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
      fail("block-compressed but reports typed UAV store", format);
    for (auto [bit, flag, what] : {std::tuple{D3D12_FORMAT_SUPPORT1_RENDER_TARGET,
                                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, "render target not creatable"},
                                   std::tuple{D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL,
                                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, "depth stencil not creatable"}}) {
      if (!(support.Support1 & bit))
        continue;
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, flag};
      ComPtr<ID3D12Resource> resource;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&resource))))
        fail(what, format);
    }
  }
  if (!checked) {
    printf("failed: no format answered\n");
    return 1;
  }
  printf("%s: %u mismatches over %u formats\n", mismatches ? "failed" : "passed", mismatches, checked);
  return mismatches != 0;
}
