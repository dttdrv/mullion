// contract: a shared texture owes its contents to the share before any handle to the share exists, and owes
// nothing before that: creating the texture only records the initialisation, and the call that first makes a
// handle out of it waits for those bytes to land. a share carries no ordering of its own (that is what the keyed
// mutex is for), so a reader on another device or in another process cannot order against the queue that wrote
// it: D3D11.3 6.1 "D3D11 will continue to rely on shared resources to achieve fully parallel GPU usage or
// multi-GPU usage", and D3D11_SUBRESOURCE_DATA is what a creation initialises the resource with.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-resource-flags
// the measurement is the initialisation's own GPU time, which nothing else in a creation pays for: a quarter
// gigabyte of zeros costs the CPU one command and the GPU milliseconds, and a hundred megabytes of initial data
// cost the GPU its upload, so whichever of the creation and the handle's making takes that time is the one that
// waited. the bytes themselves are then read back on a second device through both kinds of handle.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <vector>

static double
milliseconds(LONGLONG ticks) {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  return double(ticks) * 1000.0 / double(frequency.QuadPart);
}

static LONGLONG
ticks() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

// the texture's bytes, read back through a staging copy of the same description
static std::vector<BYTE>
contents(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture, UINT width, UINT height) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  std::vector<BYTE> out(size_t(width) * height * 4);
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
    return {};
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return {};
  for (UINT y = 0; y < height; y++)
    memcpy(out.data() + size_t(y) * width * 4, (BYTE *)mapped.pData + size_t(y) * mapped.RowPitch, size_t(width) * 4);
  context->Unmap(staging.Get(), 0);
  return out;
}

int
main() {
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device, other;
  ComPtr<ID3D11DeviceContext> context, other_context;
  ComPtr<ID3D11Device1> other1;
  step("device and a second device to read the share back on");
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other,
                          nullptr, &other_context));
  CHECK(other.As(&other1));

  // the largest 2D texture a feature level 11_0 device must take, in the smallest format: 256 MiB of contents
  // that cost the CPU one command to initialise and the GPU milliseconds to land
  const UINT side = 16384;
  D3D11_TEXTURE2D_DESC large{side, side, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
  step("create a shared %ux%u texture with no handle and no initial data", side, side);
  LONGLONG before = ticks();
  ComPtr<ID3D11Texture2D> filled;
  CHECK(device->CreateTexture2D(&large, nullptr, &filled));
  LONGLONG created = ticks();
  ComPtr<IDXGIResource> resource;
  CHECK(filled.As(&resource));
  HANDLE handle = nullptr;
  CHECK(resource->GetSharedHandle(&handle));
  LONGLONG shared = ticks();
  printf(
      "256 MiB of initialised zeros: creation %0.3f ms, first handle %0.3f ms\n",
      milliseconds(created - before), milliseconds(shared - created)
  );

  // the GPU's work has to be paid somewhere, and the share is the only reason it has to be paid at all: the fill
  // shows up in the call that makes the handle, never in the creation that did not have to wait for it
  expect(milliseconds(shared - created) > milliseconds(created - before),
         "creation took %0.3f ms and the first handle %0.3f ms: creation is still paying the GPU wait",
         milliseconds(created - before), milliseconds(shared - created));
  expect(milliseconds(shared - created) > 1.0,
         "the first handle came back in %0.3f ms, before 256 MiB could have landed",
         milliseconds(shared - created));
  resource = nullptr;
  filled = nullptr;

  // the same for a creation given initial data: its upload is the GPU's to complete, and the handle is where that
  // completion is waited for
  const UINT wide = 16384, tall = 8192;
  std::vector<BYTE> fill(size_t(wide) * tall, 0x5a);
  D3D11_TEXTURE2D_DESC filled_desc{wide, tall, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
  D3D11_SUBRESOURCE_DATA data{fill.data(), wide, 0};
  step("create a shared %ux%u texture with %u MiB of initial data and no handle", wide, tall,
       (unsigned)(fill.size() >> 20));
  before = ticks();
  ComPtr<ID3D11Texture2D> uploaded;
  CHECK(device->CreateTexture2D(&filled_desc, &data, &uploaded));
  created = ticks();
  ComPtr<IDXGIResource> uploaded_resource;
  CHECK(uploaded.As(&uploaded_resource));
  HANDLE uploaded_handle = nullptr;
  CHECK(uploaded_resource->GetSharedHandle(&uploaded_handle));
  shared = ticks();
  printf(
      "%u MiB of initial data: creation %0.3f ms, first handle %0.3f ms\n", (unsigned)(fill.size() >> 20),
      milliseconds(created - before), milliseconds(shared - created)
  );
  expect(milliseconds(shared - created) > 1.0,
         "the first handle came back in %0.3f ms, before %u MiB of data could have been uploaded",
         milliseconds(shared - created), (unsigned)(fill.size() >> 20));
  uploaded_resource = nullptr;
  uploaded = nullptr;
  fill = {};

  // the bytes a handle publishes are the texture's own, read on another device that cannot order against the
  // queue that wrote them; the handle is what makes that safe
  const UINT width = 256, height = 64;
  std::vector<BYTE> pattern(size_t(width) * height * 4);
  for (size_t i = 0; i < pattern.size(); i++)
    pattern[i] = BYTE((i * 7 + 3) & 0xff);
  // "D3D11_RESOURCE_MISC_SHARED_NTHANDLE requires D3D11_RESOURCE_MISC_SHARED or
  // D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX" (D3D11_RESOURCE_MISC_FLAG); such a texture reports no legacy handle
  D3D11_TEXTURE2D_DESC described{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_SHADER_RESOURCE, 0,
                                 D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  D3D11_SUBRESOURCE_DATA initial{pattern.data(), width * 4, 0};
  step("create a shared texture with initial data and share it as an NT handle");
  ComPtr<ID3D11Texture2D> source;
  CHECK(device->CreateTexture2D(&described, &initial, &source));
  ComPtr<IDXGIResource1> source_resource;
  CHECK(source.As(&source_resource));
  HANDLE nt_handle = nullptr;
  CHECK(source_resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &nt_handle));
  ComPtr<ID3D11Texture2D> imported;
  CHECK(other1->OpenSharedResource1(nt_handle, IID_PPV_ARGS(&imported)));
  CloseHandle(nt_handle);
  step("read the shared texture back on the second device");
  auto seen = contents(other.Get(), other_context.Get(), imported.Get(), width, height);
  if (!expect(seen.size() == pattern.size() && !memcmp(seen.data(), pattern.data(), pattern.size()),
              "the second device read %u bytes of the share, not the %u the creation initialised it with",
              (unsigned)seen.size(), (unsigned)pattern.size()))
    return verdict();

  // the legacy handle is the other way a share becomes visible, and it publishes the same contents
  step("the same texture shared as a legacy handle");
  D3D11_TEXTURE2D_DESC legacy_desc = described;
  legacy_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  ComPtr<ID3D11Texture2D> legacy_source;
  CHECK(device->CreateTexture2D(&legacy_desc, &initial, &legacy_source));
  ComPtr<IDXGIResource> legacy_resource;
  CHECK(legacy_source.As(&legacy_resource));
  HANDLE legacy = nullptr;
  CHECK(legacy_resource->GetSharedHandle(&legacy));
  ComPtr<ID3D11Texture2D> legacy_imported;
  CHECK(other->OpenSharedResource(legacy, IID_PPV_ARGS(&legacy_imported)));
  auto legacy_seen = contents(other.Get(), other_context.Get(), legacy_imported.Get(), width, height);
  if (!expect(legacy_seen == pattern, "the legacy handle published %u bytes of the texture, not %u",
              (unsigned)legacy_seen.size(), (unsigned)pattern.size()))
    return verdict();

  return verdict();
}
