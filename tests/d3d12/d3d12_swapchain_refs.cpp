// contract: public references to any of a Direct3D 12 swap chain's buffers collectively hold one reference on
// the swap chain; internal ownership is absent from each buffer's public count, and the last public release
// drops that shared reference (Wine dlls/d3d12/tests/d3d12.c, test_swapchain_refcount, Windows expectations).
// "The method returns the new reference count." (Microsoft Learn, IUnknown::AddRef, Return value:
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-addref).
// "The order of buffers returned by GetBuffer never changes." (Microsoft Learn, Swap Chains, Buffer lifetime:
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains). the same objects must therefore survive
// zero public references and be returned again, until ResizeBuffers replaces them.
// "You can't resize a swap chain unless you release all outstanding references to its back buffers."
// (Microsoft Learn, IDXGISwapChain::ResizeBuffers, Remarks, also required by ResizeBuffers1:
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers).
// a buffer held only through another buffer's private data still blocks resize; rejection preserves both buffers.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <thread>

int
main(int, char **) {
  auto references = [](IUnknown *object) {
    auto added = object->AddRef();
    auto count = object->Release();
    expect(added == count + 1, "AddRef and Release disagree on the public count");
    return count;
  };
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  auto window = CreateWindowA(
      "static", "d3d12_swapchain_refs", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr),
      nullptr
  );
  if (!expect(window != nullptr, "could not create window"))
    return verdict();
  RECT client{};
  if (!expect(GetClientRect(window, &client), "could not read client size")) {
    DestroyWindow(window);
    return verdict();
  }
  const auto factory_refs = references(factory.Get());
  // flip-model counts are from two through DXGI_MAX_SWAP_CHAIN_BUFFERS (DXGI_SWAP_CHAIN_DESC1, BufferCount).
  for (UINT count = 2; count <= DXGI_MAX_SWAP_CHAIN_BUFFERS; count++) {
    step("count=%u create, acquire, alias and release every buffer", count);
    DXGI_SWAP_CHAIN_DESC1 desc{
        UINT(client.right),
        UINT(client.bottom),
        DXGI_FORMAT_B8G8R8A8_UNORM,
        FALSE,
        {1, 0},
        DXGI_USAGE_RENDER_TARGET_OUTPUT,
        count,
        DXGI_SCALING_STRETCH,
        DXGI_SWAP_EFFECT_FLIP_DISCARD
    };
    ComPtr<IDXGISwapChain1> swapchain;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swapchain));
    expect(references(swapchain.Get()) == 1, "unheld buffers retained swap chain");
    std::vector<ComPtr<ID3D12Resource>> buffers(count);
    std::vector<ID3D12Resource *> identities(count);
    for (UINT i = 0; i < count; i++) {
      step("count=%u buffer=%u acquire and alias", count, i);
      CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
      identities[i] = buffers[i].Get();
      expect(references(swapchain.Get()) == 2, "held buffers must add exactly one swap chain reference");
      expect(buffers[i]->AddRef() == 2, "buffer includes internal ownership or aliases another buffer's count");
      expect(buffers[i]->Release() == 1, "extra buffer reference was not released");
      ComPtr<IUnknown> alias;
      CHECK(buffers[i].As(&alias));
      expect(references(buffers[i].Get()) == 2, "QueryInterface did not add a buffer reference");
      expect(buffers[i]->AddRef() == 3, "third buffer reference has the wrong count");
      expect(buffers[i]->Release() == 2, "third buffer reference was not released");
      alias.Reset();
      ComPtr<ID3D12Resource> again;
      CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&again)));
      expect(again.Get() == identities[i], "GetBuffer changed identity");
      expect(references(buffers[i].Get()) == 2, "GetBuffer did not add a buffer reference");
      expect(references(swapchain.Get()) == 2, "aliases added more swap chain references");
    }
    step("count=%u ordinary committed texture references", count);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto texture_desc = buffers.front()->GetDesc();
    ComPtr<ID3D12Resource> ordinary;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ordinary)
    ));
    expect(references(ordinary.Get()) == 1, "ordinary texture has the wrong public count");
    expect(ordinary->AddRef() == 2, "ordinary texture alias has the wrong count");
    expect(ordinary->Release() == 1, "ordinary texture alias was not released");
    expect(ordinary.Detach()->Release() == 0, "ordinary texture retained public references");
    expect(references(swapchain.Get()) == 2, "ordinary texture changed swap chain's count");
    expect(swapchain->AddRef() == 3, "explicit swap chain reference was not counted");
    expect(swapchain->Release() == 2, "explicit release dropped the buffers' reference");
    for (UINT i = 0; i < count; i++) {
      step("count=%u buffer=%u reject resize then last public release", count, i);
      expect(
          swapchain->ResizeBuffers(0, desc.Width + 1, desc.Height + 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0) ==
              DXGI_ERROR_INVALID_CALL,
          "resize accepted a held buffer"
      );
      expect(buffers[i].Detach()->Release() == 0, "internal ownership appears in buffer's public count");
      expect(references(swapchain.Get()) == (i + 1 == count ? 1u : 2u), "wrong shared buffer reference lifetime");
    }
    for (UINT i = count; i-- > 0;) {
      step("count=%u buffer=%u reacquire after zero public references", count, i);
      CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
      expect(buffers[i].Get() == identities[i], "zero public references destroyed buffer identity");
      expect(references(buffers[i].Get()) == 1, "reacquired buffer has the wrong count");
      expect(references(swapchain.Get()) == 2, "reacquired buffers do not share one reference");
    }
    buffers.clear();
    step("count=%u failed queries and resize with no held buffers", count);
    ComPtr<ID3D12Resource> invalid;
    expect(
        swapchain->GetBuffer(count, IID_PPV_ARGS(&invalid)) == DXGI_ERROR_NOT_FOUND,
        "index past last buffer was accepted"
    );
    ComPtr<ID3D12Device> unsupported;
    expect(swapchain->GetBuffer(0, IID_PPV_ARGS(&unsupported)) == E_NOINTERFACE, "wrong buffer interface accepted");
    expect(references(swapchain.Get()) == 1, "failed queries retained swap chain");

    step("count=%u reject resize with another buffer held through private data", count);
    buffers.resize(count);
    CHECK(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffers.front())));
    CHECK(swapchain->GetBuffer(count - 1, IID_PPV_ARGS(&buffers.back())));
    CHECK(buffers.front()->SetPrivateDataInterface(__uuidof(ID3D12Resource), buffers.back().Get()));
    if (!expect(
            swapchain->ResizeBuffers(count, desc.Width + 1, desc.Height + 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0) ==
                DXGI_ERROR_INVALID_CALL,
            "resize accepted buffers with a private-data reference"
        )) {
      CHECK(buffers.front()->SetPrivateDataInterface(__uuidof(ID3D12Resource), nullptr));
      buffers.clear();
      DestroyWindow(window);
      return verdict();
    }
    expect(buffers.back().Detach()->Release() == 1, "private data did not retain the other buffer");
    expect(buffers.front().Detach()->Release() == 0, "buffer with private data retained public references");
    expect(references(swapchain.Get()) == 2, "indirect buffer reference did not retain swap chain");
    ComPtr<IDXGISwapChain3> swapchain3;
    CHECK(swapchain.As(&swapchain3));
    // default buffers use node 1 (IDXGISwapChain3::ResizeBuffers1, Remarks).
    std::vector<UINT> node_masks(count, 1);
    std::vector<IUnknown *> queues(count, queue.Get());
    expect(
        swapchain3->ResizeBuffers1(
            count, desc.Width + 1, desc.Height + 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node_masks.data(), queues.data()
        ) == DXGI_ERROR_INVALID_CALL,
        "ResizeBuffers1 accepted a buffer held only through private data"
    );
    swapchain3.Reset();
    DXGI_SWAP_CHAIN_DESC1 unchanged{};
    CHECK(swapchain->GetDesc1(&unchanged));
    expect(
        unchanged.Width == desc.Width && unchanged.Height == desc.Height && unchanged.Format == desc.Format &&
            unchanged.BufferCount == count,
        "rejected resize changed swap chain description"
    );
    CHECK(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffers.front())));
    expect(buffers.front().Get() == identities.front(), "rejected resize changed first buffer identity");
    ComPtr<IUnknown> linked;
    UINT size = sizeof(IUnknown *);
    CHECK(buffers.front()->GetPrivateData(__uuidof(ID3D12Resource), &size, linked.GetAddressOf()));
    expect(size == sizeof(IUnknown *) && linked.Get() == identities.back(), "rejected resize lost private data");
    expect(linked.Detach()->Release() == 1, "private-data query left the wrong public count");
    CHECK(buffers.front()->SetPrivateDataInterface(__uuidof(ID3D12Resource), nullptr));
    expect(buffers.front().Detach()->Release() == 0, "cleared private data left public references");
    expect(references(swapchain.Get()) == 1, "private-data cleanup leaked swap chain references");
    buffers.clear();
    step("count=%u resize after all direct and indirect references are released", count);
    CHECK(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
    expect(references(swapchain.Get()) == 1, "resized buffers retained swap chain");

    step("count=%u concurrent same and different buffer references", count);
    std::vector<UINT> failed(count * 2);
    std::vector<std::thread> threads;
    for (UINT t = 0; t < failed.size(); t++)
      threads.emplace_back([&, t] {
        for (UINT n = 0; n < count * count; n++) {
          ComPtr<ID3D12Resource> resource;
          auto hr = swapchain->GetBuffer(t % count, IID_PPV_ARGS(&resource));
          if (FAILED(hr)) {
            failed[t] = n + 1;
            break;
          }
          resource->AddRef();
          resource->Release();
        }
      });
    for (auto &thread : threads)
      thread.join();
    for (UINT t = 0; t < failed.size(); t++)
      expect(!failed[t], "thread=%u buffer=%u iteration=%u GetBuffer failed", t, t % count, failed[t] - 1);
    expect(references(swapchain.Get()) == 1, "concurrent releases leaked swap chain references");

    step("count=%u only buffers retain swap chain, then release everything", count);
    buffers.resize(count);
    for (UINT i = 0; i < count; i++)
      CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
    auto retained = swapchain.Detach()->Release();
    if (!expect(retained == 1, "buffers did not keep swap chain alive after its last explicit release")) {
      buffers.clear();
      DestroyWindow(window);
      return verdict();
    }
    expect(references(factory.Get()) == factory_refs + 1, "buffers lost swap chain's factory");
    for (UINT i = 0; i < count; i++) {
      step("count=%u buffer=%u release after last explicit swap chain release", count, i);
      expect(buffers[i].Detach()->Release() == 0, "buffer still has public references");
      expect(references(factory.Get()) == factory_refs + (i + 1 < count), "swap chain destruction was early or leaked");
    }
  }
  DestroyWindow(window);
  return verdict();
}
