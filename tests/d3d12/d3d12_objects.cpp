// contract: device children obey COM identity/reference rules and private data owns exactly the references it returns.
// GetDevice: "Any returned interfaces have their reference count incremented by one"; GetPrivateData: a stored
// interface "will have its reference count incremented before the private data is returned" (Microsoft Learn).
// QueryInterface must return one canonical IUnknown, clear unsupported outputs, and reject a null output.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12devicechild-getdevice
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12object-getprivatedata
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12object-setprivatedatainterface
#include "d3d12_test.hpp"
#include <array>

static ULONG references(IUnknown *object) {
  object->AddRef();
  return object->Release();
}

int main(int, char **) {
  ComPtr<ID3D12Device1> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  struct Entry { const char *name; ComPtr<ID3D12Object> object; bool child; };
  std::vector<Entry> objects;
  auto keep = [&](const char *name, auto *object, bool child = true) -> HRESULT {
    Entry entry{name, {}, child};
    HRESULT hr = object->QueryInterface(IID_PPV_ARGS(&entry.object));
    if (SUCCEEDED(hr)) {
      ComPtr<IUnknown> original, base;
      hr = object->QueryInterface(IID_PPV_ARGS(&original));
      if (FAILED(hr))
        return hr;
      hr = entry.object.As(&base);
      if (FAILED(hr))
        return hr;
      if (!expect(original == base, "%s changed identity through ID3D12Object", name))
        return E_FAIL;
      objects.push_back(std::move(entry));
    }
    return hr;
  };
  CHECK(keep("device", device.Get(), false));
  auto resource = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(GUID), D3D12_RESOURCE_STATE_COMMON);
  if (!expect(resource != nullptr, "resource"))
    return verdict();
  CHECK(keep("committed resource", resource.Get()));
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC hd{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, {D3D12_HEAP_TYPE_DEFAULT}, 0,
                     D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  CHECK(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
  CHECK(keep("heap", heap.Get()));
  ComPtr<ID3D12DescriptorHeap> descriptors;
  D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1};
  CHECK(device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&descriptors)));
  CHECK(keep("descriptor heap", descriptors.Get()));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  CHECK(keep("allocator", allocator.Get()));
  CHECK(keep("command list", list.Get()));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(keep("queue", queue.Get()));
  ComPtr<ID3D12Fence> fence, probe;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probe)));
  CHECK(keep("fence", fence.Get()));
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1};
  CHECK(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)));
  CHECK(keep("query heap", queries.Get()));
  auto root = root_signature(device.Get(), {});
  if (!expect(root != nullptr, "root signature"))
    return verdict();
  CHECK(keep("root signature", root.Get()));
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC operation{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC sd{sizeof(D3D12_DRAW_ARGUMENTS), 1, &operation};
  CHECK(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)));
  CHECK(keep("command signature", signature.Get()));
  D3D12_FEATURE_DATA_SHADER_CACHE cache{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache)));
  ComPtr<ID3D12PipelineLibrary> library;
  if (cache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) {
    CHECK(device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)));
    CHECK(keep("pipeline library", library.Get()));
  }
  const GUID data_id = __uuidof(ID3D12Resource), interface_id = __uuidof(ID3D12Fence), unsupported{};
  for (auto &entry : objects) {
    step("%s identity and private data", entry.name);
    auto object = entry.object.Get();
    ComPtr<IUnknown> identity, via_base;
    CHECK(object->QueryInterface(IID_PPV_ARGS(&identity)));
    CHECK(identity->QueryInterface(IID_PPV_ARGS(&via_base)));
    expect(identity == via_base, "IUnknown identity changed");
    ULONG before = references(object);
    ULONG retained = object->AddRef(), released = object->Release();
    expect(retained == before + 1 && released == before, "AddRef/Release did not balance");
    void *unknown = object;
    expect(object->QueryInterface(unsupported, &unknown) == E_NOINTERFACE && !unknown,
           "unsupported QueryInterface changed its failure/output contract");
    expect(object->QueryInterface(__uuidof(IUnknown), nullptr) == E_POINTER, "null QueryInterface output accepted");
    if (entry.child) {
      ComPtr<ID3D12DeviceChild> child;
      CHECK(object->QueryInterface(IID_PPV_ARGS(&child)));
      before = references(device.Get());
      ComPtr<ID3D12Device> owner;
      CHECK(child->GetDevice(IID_PPV_ARGS(&owner)));
      expect(references(device.Get()) == before + 1, "GetDevice did not retain the device");
      ComPtr<IUnknown> expected, got;
      CHECK(device.As(&expected));
      CHECK(owner.As(&got));
      expect(expected == got, "GetDevice returned a different device");
    }
    std::array<UINT8, 2 * sizeof(GUID)> data;
    for (UINT i = 0; i < data.size(); i++)
      data[i] = UINT8(i ^ data.size());
    for (UINT size : {1u, UINT(sizeof(GUID)), UINT(data.size())}) {
      auto expected = data;
      CHECK(object->SetPrivateData(data_id, size, data.data()));
      for (auto &byte : data)
        byte = ~byte;
      UINT required = 0;
      CHECK(object->GetPrivateData(data_id, &required, nullptr));
      expect(required == size, "private size %u, want %u", required, size);
      for (UINT available : {0u, size - 1, size, size + 1}) {
        std::array<UINT8, 2 * sizeof(GUID) + 1> got;
        got.fill(UINT8(~0u));
        UINT returned = available;
        HRESULT hr = object->GetPrivateData(data_id, &returned, got.data());
        expect(hr == (available < size ? DXGI_ERROR_MORE_DATA : S_OK) && returned == size,
               "private data capacity %u: %08lx, size %u", available, hr, returned);
        for (UINT i = 0; i < got.size(); i++)
          expect(got[i] == (available >= size && i < size ? expected[i] : UINT8(~0u)),
                 "private data capacity %u changed byte %u incorrectly", available, i);
      }
    }
    before = references(probe.Get());
    CHECK(object->SetPrivateDataInterface(interface_id, probe.Get()));
    expect(references(probe.Get()) == before + 1, "private interface was not retained");
    IUnknown *returned = nullptr;
    UINT size = sizeof(returned);
    CHECK(object->GetPrivateData(interface_id, &size, &returned));
    if (!expect(returned == probe.Get() && size == sizeof(returned) && references(probe.Get()) == before + 2,
                "returned private interface did not own its reference"))
      return verdict();
    returned->Release();
    CHECK(object->SetPrivateDataInterface(interface_id, probe.Get()));
    expect(references(probe.Get()) == before + 1, "replacing the same private interface leaked a reference");
    CHECK(object->SetPrivateData(interface_id, data.size(), data.data()));
    expect(references(probe.Get()) == before, "byte replacement retained the old interface");
    CHECK(object->SetPrivateDataInterface(interface_id, probe.Get()));
    CHECK(object->SetPrivateDataInterface(interface_id, nullptr));
    expect(references(probe.Get()) == before, "clearing the private interface retained it");
    CHECK(object->SetPrivateData(data_id, 0, nullptr));
    UINT missing = sizeof(data);
    expect(object->GetPrivateData(data_id, &missing, data.data()) == DXGI_ERROR_NOT_FOUND && !missing,
           "deleted private data remained present");
  }
  return verdict();
}
