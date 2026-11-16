// contract: a committed texture on a shared heap is shared by ID3D12Device::CreateSharedHandle, and the handle opens
// the same texture on another Direct3D 12 device (OpenSharedHandle), by its name (OpenSharedHandleByName), and in
// Direct3D 11 (ID3D11Device1::OpenSharedResource1); a texture Direct3D 11 shared opens in Direct3D 12.
// "You can create shared handles to resources (committed resources only), heaps, and fences"; "The only value this
// parameter accepts is GENERIC_ALL" (ID3D12Device::CreateSharedHandle).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
// "Resources shared between Direct3D 11 and Direct3D 12 ... must be committed resources created with
// D3D12_HEAP_FLAG_SHARED" (Direct3D 11 on 12, sharing surfaces).
// each consumer reads the texels the producer wrote, through its own copy, so a share that opened some other memory,
// or nothing, reads wrong; MetalVR reads the same share from another process.
#include "d3d12_test.hpp"
#include <d3d11_4.h>

namespace {

constexpr UINT kSize = 16;
constexpr UINT kPitch = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
constexpr float kColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
constexpr uint8_t kTexel[4] = {64, 128, 191, 255}; // kColor in R8G8B8A8_UNORM, rounded
constexpr uint8_t kTexel11[4] = {10, 200, 30, 255}; // what Direct3D 11 writes

bool
texel_matches(const uint8_t *got, const uint8_t *want) {
  for (int c = 0; c < 4; ++c)
    if (abs(int(got[c]) - int(want[c])) > 1)
      return false;
  return true;
}

struct Device12 {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;

  bool create() {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
      return false;
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    return SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
           SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
           SUCCEEDED(device->CreateCommandList(
               0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)
           )) &&
           SUCCEEDED(list->Close());
  }

  void reset() {
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
  }

  // the texel at (0, 0) of a texture in the COMMON state, copied through a readback buffer
  bool read(ID3D12Resource *texture, uint8_t out[4]) {
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kPitch * kSize, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback || FAILED(forget(readback.Get())))
      return false;
    reset();
    transition(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(submit(device.Get(), queue.Get(), list.Get())))
      return false;
    void *bytes = nullptr;
    if (FAILED(readback->Map(0, nullptr, &bytes)))
      return false;
    memcpy(out, bytes, 4);
    readback->Unmap(0, nullptr);
    return true;
  }
};

D3D12_RESOURCE_DESC
TextureDesc() {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kSize;
  desc.Height = kSize;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc = {1, 0};
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  return desc;
}

} // namespace

int
main(int argc, char **argv) {
  Device12 a, b;
  if (!expect(a.create() && b.create(), "two Direct3D 12 devices"))
    return verdict();
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device11_1;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device11,
                          nullptr, &context));
  CHECK(device11.As(&device11_1));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC desc = TextureDesc();

  step("an unshared texture, and access other than GENERIC_ALL, are refused");
  ComPtr<ID3D12Resource> ordinary;
  CHECK(a.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&ordinary)));
  HANDLE handle = nullptr;
  expect(FAILED(a.device->CreateSharedHandle(ordinary.Get(), nullptr, GENERIC_ALL, nullptr, &handle)) && !handle,
         "a texture on a heap that is not shared was shared");

  ComPtr<ID3D12Resource> shared;
  CHECK(a.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&shared)));
  expect(a.device->CreateSharedHandle(shared.Get(), nullptr, GENERIC_READ, nullptr, &handle) == E_INVALIDARG &&
             !handle,
         "access other than GENERIC_ALL was taken");

  step("Direct3D 12 writes a shared texture and shares it, with a name");
  {
    ComPtr<ID3D12DescriptorHeap> rtvs;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(a.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    a.device->CreateRenderTargetView(shared.Get(), nullptr, rtv);
    a.reset();
    a.list->ClearRenderTargetView(rtv, kColor, 0, nullptr);
    transition(a.list.Get(), shared.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    CHECK(submit(a.device.Get(), a.queue.Get(), a.list.Get()));
  }
  WCHAR name[MAX_PATH];
  swprintf(name, ARRAYSIZE(name), L"mullion_shared_texture_%lu", GetCurrentProcessId());
  CHECK(a.device->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, name, &handle));
  expect(handle != nullptr, "no handle");
  uint8_t texel[4] = {};
  expect(a.read(shared.Get(), texel) && texel_matches(texel, kTexel), "the producer reads %u %u %u %u", texel[0], texel[1],
         texel[2], texel[3]);

  step("another Direct3D 12 device opens it by handle and reads what was written");
  ComPtr<ID3D12Resource> opened;
  CHECK(b.device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened)));
  if (opened) {
    D3D12_RESOURCE_DESC got = opened->GetDesc();
    expect(got.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && got.Width == kSize && got.Height == kSize &&
               got.Format == DXGI_FORMAT_R8G8B8A8_UNORM && (got.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
           "the opened texture's description is %ux%u format %u flags %#x", unsigned(got.Width), got.Height,
           got.Format, got.Flags);
    memset(texel, 0, sizeof(texel));
    expect(b.read(opened.Get(), texel) && texel_matches(texel, kTexel), "the other device reads %u %u %u %u", texel[0],
           texel[1], texel[2], texel[3]);
  }

  step("the name opens it too");
  HANDLE by_name = nullptr;
  CHECK(b.device->OpenSharedHandleByName(name, GENERIC_ALL, &by_name));
  ComPtr<ID3D12Resource> named;
  if (by_name) {
    CHECK(b.device->OpenSharedHandle(by_name, IID_PPV_ARGS(&named)));
    memset(texel, 0, sizeof(texel));
    expect(named && b.read(named.Get(), texel) && texel_matches(texel, kTexel), "opened by name, it reads %u %u %u %u",
           texel[0], texel[1], texel[2], texel[3]);
    CloseHandle(by_name);
  }

  step("Direct3D 11 opens the Direct3D 12 share and reads what was written");
  ComPtr<ID3D11Texture2D> texture11;
  CHECK(device11_1->OpenSharedResource1(handle, IID_PPV_ARGS(&texture11)));
  if (texture11) {
    D3D11_TEXTURE2D_DESC got{};
    texture11->GetDesc(&got);
    expect(got.Width == kSize && got.Height == kSize && got.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
           "Direct3D 11 sees %ux%u format %u", got.Width, got.Height, got.Format);
    D3D11_TEXTURE2D_DESC staging_desc = got;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(device11->CreateTexture2D(&staging_desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture11.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    expect(texel_matches(static_cast<uint8_t *>(mapped.pData), kTexel), "Direct3D 11 reads %u %u %u %u",
           static_cast<uint8_t *>(mapped.pData)[0], static_cast<uint8_t *>(mapped.pData)[1],
           static_cast<uint8_t *>(mapped.pData)[2], static_cast<uint8_t *>(mapped.pData)[3]);
    context->Unmap(staging.Get(), 0);
  }
  CloseHandle(handle);

  step("Direct3D 12 opens a texture Direct3D 11 shared and reads what Direct3D 11 wrote");
  {
    D3D11_TEXTURE2D_DESC desc11{kSize, kSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                                D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
    std::vector<uint8_t> texels(kSize * kSize * 4);
    for (size_t i = 0; i < texels.size(); i += 4)
      memcpy(&texels[i], kTexel11, 4);
    D3D11_SUBRESOURCE_DATA data{texels.data(), kSize * 4, 0};
    ComPtr<ID3D11Texture2D> made11;
    CHECK(device11->CreateTexture2D(&desc11, &data, &made11));
    ComPtr<IDXGIResource1> resource;
    HANDLE handle11 = nullptr;
    if (made11 && SUCCEEDED(made11.As(&resource)))
      CHECK(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                         &handle11));
    ComPtr<ID3D12Resource> from11;
    if (handle11) {
      CHECK(a.device->OpenSharedHandle(handle11, IID_PPV_ARGS(&from11)));
      memset(texel, 0, sizeof(texel));
      expect(from11 && a.read(from11.Get(), texel) && texel_matches(texel, kTexel11),
             "Direct3D 12 reads Direct3D 11's texture as %u %u %u %u", texel[0], texel[1], texel[2], texel[3]);
      CloseHandle(handle11);
    }
  }

  step("a shared fence still opens as a fence");
  {
    ComPtr<ID3D12Fence> fence, fence_b;
    CHECK(a.device->CreateFence(7, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    HANDLE fence_handle = nullptr;
    CHECK(a.device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle));
    CHECK(b.device->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&fence_b)));
    expect(fence_b && fence_b->GetCompletedValue() == 7, "the fence opened with the wrong value");
    ComPtr<ID3D12Resource> not_texture;
    expect(FAILED(b.device->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&not_texture))) && !not_texture,
           "a fence's handle opened as a texture");
    CloseHandle(fence_handle);
  }
  return verdict();
}
