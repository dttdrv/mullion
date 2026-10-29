// contract: a device whose frames the GPU failed is removed, and the reason it was removed is the first one it had.
// "GetDeviceRemovedReason ... Returns the device removal reason" and a removed device's reason does not change
// (ID3D11Device::GetDeviceRemovedReason; ID3D12Device's says the same of later errors);
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-getdeviceremovedreason
// ID3D11Device4::RegisterDeviceRemovedEvent gives an event that is signaled when the device is removed, and a device
// that is already removed answers the reason it was removed with rather than a registration;
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device4-registerdeviceremovedevent
//
// MULLION_D3D11_FAULT_DEVICE=1 makes the run's first completed command buffer one the GPU failed, so the removal path is
// the one a real command buffer error takes without needing a GPU fault. tests/meson.build runs this test twice: once
// without it, where the device is live, and once with it, where it is removed.
#include "d3d11_test.hpp"
#include <d3d11_4.h>

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));

  const bool faulted = GetEnvironmentVariableA("MULLION_D3D11_FAULT_DEVICE", nullptr, 0) > 0;

  HANDLE removed = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  ComPtr<ID3D11Device4> device4;
  CHECK(device.As(&device4));
  DWORD cookie = 0;
  const HRESULT registered = device4->RegisterDeviceRemovedEvent(removed, &cookie);

  // a frame's work, so the run's first command buffer has something to complete: a clear of a render target, flushed
  // (a clear is a render pass, which is the queue's work; a resource's initial data may be uploaded before it)
  D3D11_TEXTURE2D_DESC desc{4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  ComPtr<ID3D11RenderTargetView> target_view;
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &target_view));
  const float colour[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  context->ClearRenderTargetView(target_view.Get(), colour);
  context->Flush();

  if (!faulted) {
    expect(SUCCEEDED(registered), "a live device refused the removal event");
    expect(cookie != 0, "a live device's removal event has no cookie");
    expect(device->GetDeviceRemovedReason() == S_OK, "a live device is removed");
    expect(WaitForSingleObject(removed, 0) == WAIT_TIMEOUT, "a live device signaled its removal event");
    device4->UnregisterDeviceRemoved(cookie);
    expect(device->GetDeviceRemovedReason() == S_OK, "work on a live device removed it");
    printf("removal: a live device, reason 0x%08lx, event not signaled\n", (unsigned long)device->GetDeviceRemovedReason());
  } else {
    // the fault is taken by the queue's finish thread when a command buffer completes: wait for the reason, then for
    // what a registration was given
    HRESULT reason = S_OK;
    for (unsigned attempt = 0; attempt < 500 && reason == S_OK; attempt++) {
      reason = device->GetDeviceRemovedReason();
      if (reason == S_OK)
        Sleep(10);
    }
    expect(reason == DXGI_ERROR_DEVICE_HUNG, "the removed device's reason is not the fault's");
    // the reason is sticky: later work does not replace it
    context->ClearRenderTargetView(target_view.Get(), colour);
    context->Flush();
    Sleep(20);
    expect(device->GetDeviceRemovedReason() == reason, "a removed device changed the reason it was removed with");

    // a submission after the removal is refused rather than encoded and awaited: the copy this asks for would put the
    // target's red in the staging texture, and the staging texture keeps the pattern it was given instead
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
    D3D11_MAPPED_SUBRESOURCE pattern{};
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &pattern));
    for (UINT y = 0; y < desc.Height; y++)
      memset((char *)pattern.pData + y * pattern.RowPitch, 0x5a, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    context->ClearRenderTargetView(target_view.Get(), colour);
    context->CopyResource(staging.Get(), target.Get());
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE read{};
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &read));
    bool copied = false;
    for (UINT y = 0; y < desc.Height && !copied; y++)
      for (UINT x = 0; x < desc.Width * 4 && !copied; x++)
        copied = ((const char *)read.pData + y * read.RowPitch)[x] != 0x5a;
    context->Unmap(staging.Get(), 0);
    expect(!copied, "work submitted after the removal reached the GPU: the copy is in the staging texture");
    // and the queue still answers for what it holds: another flush of a removed device returns
    context->ClearRenderTargetView(target_view.Get(), colour);
    context->Flush();
    expect(device->GetDeviceRemovedReason() == reason, "flushing a removed device changed its reason");

    if (SUCCEEDED(registered)) {
      expect(cookie != 0, "a removal event has no cookie");
      expect(WaitForSingleObject(removed, 5000) == WAIT_OBJECT_0,
             "the removal did not signal the event a registration gave");
      device4->UnregisterDeviceRemoved(cookie);
    } else {
      expect(registered == reason, "a registration on a removed device did not answer the reason it was removed with");
    }
    // a registration after the removal is answered with the reason, not with an event that will never be signaled
    DWORD late = 0;
    expect(device4->RegisterDeviceRemovedEvent(removed, &late) == reason,
           "a registration on a removed device answered something other than the removal reason");
    expect(late == 0, "a registration on a removed device gave a cookie");
    printf("removal: the device was removed with 0x%08lx\n", (unsigned long)reason);
  }

  device4.Reset();
  device.Reset();
  CloseHandle(removed);
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
