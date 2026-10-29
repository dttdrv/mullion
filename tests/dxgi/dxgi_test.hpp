// the same DXGI contract is reached with a D3D10/11 device or a D3D12 direct
// queue. A build without D3D12 (x86, or -Denable_d3d12=false) compiles the
// D3D12 half out, so the D3D10/11 contracts still run there.
#pragma once
#include "../d3d12/d3d12_test.hpp"
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <span>

// the APIs a DXGI test loops over: D3D12's direct queue is only reached when the build has D3D12. The array is
// static, so the span does not point at a function-local temporary (an initializer_list returned by value does).
inline std::span<const UINT>
dxgi_apis() {
  static constexpr UINT apis[] = {10u, 101u, 11u
#ifdef MULLION_TEST_NO_D3D12
  };
#else
                                  , 12u};
#endif
  return apis;
}

struct DxgiDevice {
  ComPtr<ID3D10Device> d10;
  ComPtr<ID3D11Device> d11;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
#ifndef MULLION_TEST_NO_D3D12
  ComPtr<ID3D12Device> d12;
  ComPtr<ID3D12CommandQueue> queue;
#endif
  ComPtr<IUnknown> input;

  HRESULT
  create(UINT api) {
    HRESULT hr;
    if (api == 10) {
      hr = D3D10CreateDevice(
          nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, D3D10_CREATE_DEVICE_BGRA_SUPPORT, D3D10_SDK_VERSION, &d10
      );
      return FAILED(hr) ? hr : d10.As(&input);
    }
    if (api == 101) {
      ComPtr<ID3D10Device1> extended;
      // d3d10_1 has no import library here
      auto create1 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
          (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
      );
      if (!create1)
        return E_NOINTERFACE;
      hr = create1(
          nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, D3D10_CREATE_DEVICE_BGRA_SUPPORT, D3D10_FEATURE_LEVEL_10_1,
          D3D10_1_SDK_VERSION, &extended
      );
      if (FAILED(hr) || FAILED(hr = extended.As(&d10)))
        return hr;
      return d10.As(&input);
    }
    if (api == 11) {
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      hr = D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
          &d11, nullptr, &immediate
      );
      if (FAILED(hr))
        return hr;
      hr = d11->CreateDeferredContext(0, &deferred);
      return FAILED(hr) ? hr : d11.As(&input);
    }
#ifdef MULLION_TEST_NO_D3D12
    return E_INVALIDARG;
#else
    if (api != 12)
      return E_INVALIDARG;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    if (FAILED(hr))
      return hr;
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    hr = d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    return FAILED(hr) ? hr : queue.As(&input);
#endif
  }

  HRESULT
  idle() {
#ifndef MULLION_TEST_NO_D3D12
    if (d12) {
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      HRESULT hr = d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
      if (FAILED(hr))
        return hr;
      hr = d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
      return FAILED(hr) ? hr : submit(d12.Get(), queue.Get(), list.Get());
    }
#endif
    // staging Map waits for CPU-coherent retirement, including GDI presentation
    // callbacks; EVENT queries do not.
    const UINT value = 0x12345678;
    HRESULT hr;
    if (d11) {
      D3D11_BUFFER_DESC desc{sizeof(value), D3D11_USAGE_DEFAULT};
      D3D11_SUBRESOURCE_DATA initial{&value};
      ComPtr<ID3D11Buffer> source, staging;
      if (FAILED(hr = d11->CreateBuffer(&desc, &initial, &source)))
        return hr;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(hr = d11->CreateBuffer(&desc, nullptr, &staging)))
        return hr;
      immediate->CopyResource(staging.Get(), source.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(hr = immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return hr;
      bool same = !memcmp(mapped.pData, &value, sizeof(value));
      immediate->Unmap(staging.Get(), 0);
      return same ? d11->GetDeviceRemovedReason() : E_FAIL;
    }
    D3D10_BUFFER_DESC desc{sizeof(value), D3D10_USAGE_DEFAULT};
    D3D10_SUBRESOURCE_DATA initial{&value};
    ComPtr<ID3D10Buffer> source, staging;
    if (FAILED(hr = d10->CreateBuffer(&desc, &initial, &source)))
      return hr;
    desc.Usage = D3D10_USAGE_STAGING;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (FAILED(hr = d10->CreateBuffer(&desc, nullptr, &staging)))
      return hr;
    d10->CopyResource(staging.Get(), source.Get());
    void *mapped = nullptr;
    if (FAILED(hr = staging->Map(D3D10_MAP_READ, 0, &mapped)))
      return hr;
    bool same = !memcmp(mapped, &value, sizeof(value));
    staging->Unmap();
    return same ? d10->GetDeviceRemovedReason() : E_FAIL;
  }
};
