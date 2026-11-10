// contract: the device answers ID3D12Device11 to ID3D12Device15 with their methods in Microsoft's slots (d3d12.idl of
// microsoft/DirectX-Headers, SDK 620: declared below from it, not from Mullion's own copy), and ID3D12Device15's
// TryCreate* return S_OK, or E_INVALIDARG for a view "out of range" (DirectX-Specs, D3D12 Revised Create Views). a view
// made through a slot is then used as its kind: a clear through the render target view and through the unordered
// access view must reach its texture, which a method in another's slot would not do.
// https://microsoft.github.io/DirectX-Specs/d3d/D3D12RevisedCreateViews.html
#include "d3d12_test.hpp"

#ifndef __ID3D12Device11_INTERFACE_DEFINED__
#define __ID3D12Device11_INTERFACE_DEFINED__
typedef enum D3D12_SAMPLER_FLAGS { D3D12_SAMPLER_FLAG_NONE = 0 } D3D12_SAMPLER_FLAGS;
typedef struct D3D12_SAMPLER_DESC2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union {
    FLOAT FloatBorderColor[4];
    UINT UintBorderColor[4];
  };
  FLOAT MinLOD, MaxLOD;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_SAMPLER_DESC2;
MIDL_INTERFACE("5405c344-d457-444e-b4dd-2366e45aee39")
ID3D12Device11 : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2 *, D3D12_CPU_DESCRIPTOR_HANDLE) = 0;
};
__CRT_UUID_DECL(ID3D12Device11, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39)
MIDL_INTERFACE("5af5c532-4c91-4cd0-b541-15a405395fc5")
ID3D12Device12 : public ID3D12Device11 {
  // GetResourceAllocationInfo3, which returns a structure: only its slot is needed here
  virtual void STDMETHODCALLTYPE GetResourceAllocationInfo3Slot() = 0;
};
__CRT_UUID_DECL(ID3D12Device12, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5)
MIDL_INTERFACE("14eecffc-4df8-40f7-a118-5c816f45695e")
ID3D12Device13 : public ID3D12Device12 {
  virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(const void *, SIZE_T, REFIID, void **) = 0;
};
__CRT_UUID_DECL(ID3D12Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e)
MIDL_INTERFACE("5f6e592d-d895-44c2-8e4a-88ad4926d323")
ID3D12Device14 : public ID3D12Device13 {
  virtual HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT, const void *, SIZE_T, LPCWSTR, REFIID, void **
  ) = 0;
};
__CRT_UUID_DECL(ID3D12Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23)
typedef struct D3D12_TRIM_NOTIFICATION {
  void *pContext;
  UINT Flags;
  UINT64 NumBytesToTrim;
} D3D12_TRIM_NOTIFICATION;
typedef void(__stdcall *D3D12_PFN_TRIM_NOTIFICATION_CALLBACK)(const D3D12_TRIM_NOTIFICATION *);
typedef struct D3D12_REGISTER_TRIM_NOTIFICATION {
  D3D12_PFN_TRIM_NOTIFICATION_CALLBACK pfnCallback;
  void *pContext;
  DWORD CallbackCookie;
} D3D12_REGISTER_TRIM_NOTIFICATION;
typedef enum D3D12_QUERY_HEAP_FLAGS { D3D12_QUERY_HEAP_FLAG_NONE = 0 } D3D12_QUERY_HEAP_FLAGS;
MIDL_INTERFACE("76cff76f-1e9b-4450-8cdc-34f1af788e5b")
ID3D12Device15 : public ID3D12Device14 {
  virtual HRESULT STDMETHODCALLTYPE RegisterTrimNotificationCallback(D3D12_REGISTER_TRIM_NOTIFICATION *) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnregisterTrimNotificationCallback(DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateShaderResourceView(
      ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateUnorderedAccessView(
      ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateConstantBufferView(
      const D3D12_CONSTANT_BUFFER_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateSampler2(const D3D12_SAMPLER_DESC2 *, D3D12_CPU_DESCRIPTOR_HANDLE) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateRenderTargetView(
      ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateDepthStencilView(
      ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *, ID3D12Resource *, D3D12_CPU_DESCRIPTOR_HANDLE
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateQueryHeap1(
      const D3D12_QUERY_HEAP_DESC *, D3D12_QUERY_HEAP_FLAGS, REFIID, void **
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT, UINT, void *) = 0;
};
__CRT_UUID_DECL(ID3D12Device15, 0x76cff76f, 0x1e9b, 0x4450, 0x8c, 0xdc, 0x34, 0xf1, 0xaf, 0x78, 0x8e, 0x5b)
#endif

static void __stdcall
trimmed(const D3D12_TRIM_NOTIFICATION *) {}

int
main() {
  ComPtr<ID3D12Device> base;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&base)));
  UINT failed = 0;
  auto expect = [&](bool holds, const char *what) {
    if (!holds)
      printf("%s\n", what), failed++;
  };
  auto answers = [&](HRESULT got, HRESULT want, const char *what) {
    if (got != want)
      printf("%s: %#lx, want %#lx\n", what, (unsigned long)got, (unsigned long)want), failed++;
  };
  ComPtr<ID3D12Device11> device11;
  ComPtr<ID3D12Device12> device12;
  ComPtr<ID3D12Device13> device13;
  ComPtr<ID3D12Device14> device14;
  ComPtr<ID3D12Device15> device;
  expect(SUCCEEDED(base.As(&device11)), "no ID3D12Device11");
  expect(SUCCEEDED(base.As(&device12)), "no ID3D12Device12");
  expect(SUCCEEDED(base.As(&device13)), "no ID3D12Device13");
  expect(SUCCEEDED(base.As(&device14)), "no ID3D12Device14");
  if (FAILED(base.As(&device))) {
    printf("failed: no ID3D12Device15\n");
    return 1;
  }

  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_DESCRIPTOR_HEAP_FLAGS flags) {
    ComPtr<ID3D12DescriptorHeap> made;
    D3D12_DESCRIPTOR_HEAP_DESC desc{type, 4, flags};
    device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&made));
    return made;
  };
  auto slot = [&](ID3D12DescriptorHeap *of, UINT index) {
    auto handle = of->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * device->GetDescriptorHandleIncrementSize(of->GetDesc().Type);
    return handle;
  };
  constexpr UINT side = 4;
  auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,    side, side, 1, 1, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,       flags};
    ComPtr<ID3D12Resource> made;
    device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&made));
    return made;
  };
  auto views = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
  auto cpu_views = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
  auto targets = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
  auto depths = heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
  auto samplers = heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
  CHECK(views && cpu_views && targets && depths && samplers ? S_OK : E_FAIL);

  auto color =
      texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto words =
      texture(DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto depth =
      texture(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  auto constants = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  CHECK(color && words && depth && constants ? S_OK : E_FAIL);

  // every view of the textures' one mip, then a view of a mip they do not have
  answers(
      device->TryCreateRenderTargetView(color.Get(), nullptr, slot(targets.Get(), 0)), S_OK,
      "TryCreateRenderTargetView refused a texture's own view"
  );
  D3D12_RENDER_TARGET_VIEW_DESC far_target{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2D};
  far_target.Texture2D.MipSlice = 3;
  answers(
      device->TryCreateRenderTargetView(color.Get(), &far_target, slot(targets.Get(), 1)), E_INVALIDARG,
      "TryCreateRenderTargetView of mip 3 of 1 did not return E_INVALIDARG"
  );

  answers(
      device->TryCreateUnorderedAccessView(words.Get(), nullptr, nullptr, slot(views.Get(), 0)), S_OK,
      "TryCreateUnorderedAccessView refused a texture's own view"
  );
  answers(
      device->TryCreateUnorderedAccessView(words.Get(), nullptr, nullptr, slot(cpu_views.Get(), 0)), S_OK,
      "TryCreateUnorderedAccessView refused a texture's own view in a heap shaders do not see"
  );
  D3D12_UNORDERED_ACCESS_VIEW_DESC far_words{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  far_words.Texture2D.MipSlice = 3;
  answers(
      device->TryCreateUnorderedAccessView(words.Get(), nullptr, &far_words, slot(views.Get(), 1)), E_INVALIDARG,
      "TryCreateUnorderedAccessView of mip 3 of 1 did not return E_INVALIDARG"
  );

  answers(
      device->TryCreateShaderResourceView(color.Get(), nullptr, slot(views.Get(), 2)), S_OK,
      "TryCreateShaderResourceView refused a texture's own view"
  );
  D3D12_SHADER_RESOURCE_VIEW_DESC far_color{
      DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
  };
  far_color.Texture2D = {3, 1};
  answers(
      device->TryCreateShaderResourceView(color.Get(), &far_color, slot(views.Get(), 2)), E_INVALIDARG,
      "TryCreateShaderResourceView of mip 3 of 1 did not return E_INVALIDARG"
  );

  answers(
      device->TryCreateDepthStencilView(depth.Get(), nullptr, slot(depths.Get(), 0)), S_OK,
      "TryCreateDepthStencilView refused a texture's own view"
  );
  D3D12_DEPTH_STENCIL_VIEW_DESC far_depth{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
  far_depth.Texture2D.MipSlice = 3;
  answers(
      device->TryCreateDepthStencilView(depth.Get(), &far_depth, slot(depths.Get(), 1)), E_INVALIDARG,
      "TryCreateDepthStencilView of mip 3 of 1 did not return E_INVALIDARG"
  );

  const D3D12_CONSTANT_BUFFER_VIEW_DESC whole{constants->GetGPUVirtualAddress(), 256};
  answers(
      device->TryCreateConstantBufferView(&whole, slot(views.Get(), 3)), S_OK,
      "TryCreateConstantBufferView refused a buffer's 256 bytes"
  );

  D3D12_SAMPLER_DESC2 point{
      D3D12_FILTER_MIN_MAG_MIP_POINT,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      0,
      1,
      D3D12_COMPARISON_FUNC_NEVER
  };
  point.MaxLOD = D3D12_FLOAT32_MAX;
  answers(
      device->TryCreateSampler2(&point, slot(samplers.Get(), 0)), S_OK, "TryCreateSampler2 refused a point sampler"
  );
  device->CreateSampler2(&point, slot(samplers.Get(), 1));

  const D3D12_QUERY_HEAP_DESC occlusion{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1};
  ComPtr<ID3D12QueryHeap> queries;
  answers(
      device->CreateQueryHeap1(&occlusion, D3D12_QUERY_HEAP_FLAG_NONE, IID_PPV_ARGS(&queries)), S_OK,
      "CreateQueryHeap1 without flags"
  );
  expect(queries, "CreateQueryHeap1 without flags made no heap");
  D3D12_REGISTER_TRIM_NOTIFICATION trim{trimmed};
  answers(device->RegisterTrimNotificationCallback(&trim), S_OK, "RegisterTrimNotificationCallback failed");
  answers(
      device->UnregisterTrimNotificationCallback(trim.CallbackCookie), S_OK,
      "UnregisterTrimNotificationCallback of the cookie just given failed"
  );

  // the views are what their slots say: a clear through each reaches its texture
  const float quarter[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  const UINT word[4] = {0x12345678};
  ID3D12DescriptorHeap *bound[] = {views.Get()};
  list->SetDescriptorHeaps(1, bound);
  list->ClearRenderTargetView(slot(targets.Get(), 0), quarter, 0, nullptr);
  list->ClearUnorderedAccessViewUint(
      views->GetGPUDescriptorHandleForHeapStart(), slot(cpu_views.Get(), 0), words.Get(), word, 0, nullptr
  );
  transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), words.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  auto described = color->GetDesc();
  device->GetCopyableFootprints(&described, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
  ComPtr<ID3D12Resource> read[2];
  ID3D12Resource *sources[2] = {color.Get(), words.Get()};
  for (UINT i = 0; i < 2; i++) {
    read[i] = buffer(
        device.Get(), D3D12_HEAP_TYPE_READBACK, footprint.Footprint.RowPitch * side, D3D12_RESOURCE_STATE_COPY_DEST
    );
    CHECK(forget(read[i].Get()));
    D3D12_TEXTURE_COPY_LOCATION dst{read[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{sources[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = footprint;
    dst.PlacedFootprint.Footprint.Format = sources[i]->GetDesc().Format;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  // 0.25, 0.5, 0.75 and 1 as 8-bit unorm, red in the lowest byte
  const uint32_t want[2] = {0xffbf8040, 0x12345678};
  const char *through[2] = {"TryCreateRenderTargetView", "TryCreateUnorderedAccessView"};
  for (UINT i = 0; i < 2; i++) {
    uint32_t *texels;
    CHECK(read[i]->Map(0, nullptr, (void **)&texels));
    UINT last = (side - 1) * footprint.Footprint.RowPitch / 4 + side - 1;
    if (texels[0] != want[i] || texels[last] != want[i])
      printf(
          "a clear through the view of %s left %08x and %08x, want %08x\n", through[i], texels[0], texels[last], want[i]
      ),
          failed++;
    read[i]->Unmap(0, nullptr);
  }
  printf("%s: %u wrong answers\n", failed ? "failed" : "passed", failed);
  return failed != 0;
}
