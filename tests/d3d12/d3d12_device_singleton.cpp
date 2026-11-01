// contract: D3D12CreateDevice returns the process's existing device for an adapter while one exists, and fails once
// that device is removed. "Direct3D 12 devices are singletons per adapter. If a Direct3D 12 device already exists in
// the current process for a given adapter, then a subsequent call to D3D12CreateDevice returns the existing device. If
// the current Direct3D 12 device is in a removed state (that is, ID3D12Device::GetDeviceRemovedReason returns a
// failing HRESULT), then D3D12CreateDevice fails instead of returning the existing device. The sameness of two
// adapters (that is, they have the same identity) is determined by comparing their LUIDs, not their pointers."
// (Microsoft Learn, D3D12CreateDevice, Remarks)
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
// vkd3d-proton checks the same of two calls: "Expected singletons." (tests/d3d12_device.c, check_create_devices)
// the calls here reach the adapter by the default (null), by an adapter of a second DXGI factory with the same LUID,
// and as a probe (a null output, S_FALSE), and ask for other interfaces and feature levels; each must give the first
// device. after the application releases every reference, the next call gives a new, usable device; after RemoveDevice
// a call fails until the removed device is released.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>

int
main(int, char **) {
  ComPtr<ID3D12Device> first;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&first)));

  step("a second call for the default adapter");
  ComPtr<ID3D12Device> second;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&second)));
  expect(second.Get() == first.Get(), "a second call gave device %p, not the existing %p", second.Get(), first.Get());

  step("an adapter object of another factory, with the same LUID");
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter> adapter;
  CHECK(factory->EnumAdapterByLuid(first->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
  ComPtr<ID3D12Device> by_adapter;
  CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&by_adapter)));
  expect(by_adapter.Get() == first.Get(), "the adapter of the same LUID gave device %p, not the existing %p",
         by_adapter.Get(), first.Get());

  step("another interface and feature level of the same device");
  ComPtr<ID3D12Device1> first1, as_device1;
  CHECK(first.As(&first1));
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&as_device1)));
  expect(as_device1.Get() == first1.Get(), "ID3D12Device1 at 11_1 is %p, not the existing device's %p",
         as_device1.Get(), first1.Get());
  HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
  expect(hr == S_FALSE, "a probe with an existing device returned %#lx, not S_FALSE", hr);
  // a level above the device's still fails with a device in place (D3D12CreateDevice, MinimumFeatureLevel)
  ComPtr<ID3D12Device> too_high;
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL(0xf000), IID_PPV_ARGS(&too_high));
  expect(FAILED(hr) && !too_high, "an unknown feature level returned %#lx", hr);

  step("a new device once every reference is released");
  // an object of the device keeps it: the application's references are what the singleton is about
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(first->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  second.Reset(), by_adapter.Reset(), as_device1.Reset(), first1.Reset();
  ULONG left = first.Detach()->Release();
  ComPtr<ID3D12Device> held;
  CHECK(queue->GetDevice(IID_PPV_ARGS(&held)));
  ComPtr<ID3D12Device> while_held;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&while_held)));
  expect(while_held.Get() == held.Get(), "with a queue's device held (%lu references left at release), a call gave %p, "
         "not %p", left, while_held.Get(), held.Get());
  while_held.Reset(), held.Reset(), queue.Reset();
  ComPtr<ID3D12Device5> fresh;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&fresh)));
  expect(fresh->GetDeviceRemovedReason() == S_OK, "the new device is removed: %#lx", fresh->GetDeviceRemovedReason());
  CHECK(fresh->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  queue.Reset();

  step("a removed device");
  fresh->RemoveDevice();
  ComPtr<ID3D12Device> after_removal;
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&after_removal));
  expect(FAILED(hr), "a call after RemoveDevice returned %#lx and device %p (the removed one is %p)", hr,
         after_removal.Get(), fresh.Get());
  expect(!after_removal, "a failed call left a device in its output");
  after_removal.Reset();
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
  expect(FAILED(hr), "a probe after RemoveDevice returned %#lx", hr);

  step("a new device once the removed one is released");
  fresh.Reset();
  ComPtr<ID3D12Device> replacement;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&replacement)));
  expect(replacement->GetDeviceRemovedReason() == S_OK, "the replacement device is removed: %#lx",
         replacement->GetDeviceRemovedReason());
  CHECK(replacement->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  return verdict();
}
