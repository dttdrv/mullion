// contract: private bytes are copied, GUIDs are independent, and private
// interfaces own balanced references. "Its reference count is incremented when
// set" (SetPrivateDataInterface, pUnknown).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiobject-setprivatedatainterface
// GetPrivateData: "you must call ::Release() on the pointer". Windows'
// missing/short-buffer HRESULT and size contracts are also exercised by Wine's
// dlls/dxgi/tests/dxgi.c, test_private_data.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiobject-getprivatedata
#include "dxgi_test.hpp"
#include <algorithm>
#include <array>
#include <limits>

struct Reference : IUnknown {
  ULONG references = 1;
  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE
  AddRef() override {
    return ++references;
  }
  ULONG STDMETHODCALLTYPE
  Release() override {
    return --references;
  }
};

static int
storage(IDXGIObject *object) {
  const GUID &bytes = __uuidof(IDXGIObject), &other = __uuidof(IDXGIAdapter);
  constexpr BYTE guard = 0xa5;
  constexpr UINT byte_values = UINT(std::numeric_limits<BYTE>::max()) + 1;
  std::array<BYTE, byte_values> original{};
  for (UINT i = 0; i < original.size(); i++)
    original[i] = BYTE(i);
  const UINT independent = 0x12345678;
  CHECK(object->SetPrivateData(other, sizeof(independent), &independent));
  for (UINT length = 0; length <= original.size(); length++) {
    step("private bytes length %u", length);
    auto input = original;
    CHECK(object->SetPrivateData(bytes, length, input.data()));
    input.fill(guard);
    UINT required = 0;
    CHECK(object->GetPrivateData(bytes, &required, nullptr));
    expect(required == length, "size query %u instead of %u", required, length);
    for (UINT capacity : {0u, length ? length - 1 : 0u, length, length + 1}) {
      std::array<BYTE, byte_values + 2> output;
      output.fill(guard);
      UINT size = capacity;
      HRESULT hr = object->GetPrivateData(bytes, &size, output.data() + 1);
      expect(
          hr == (capacity < length ? DXGI_ERROR_MORE_DATA : S_OK) && size == length, "capacity %u: %#lx, size %u",
          capacity, hr, size
      );
      if (capacity >= length)
        expect(std::equal(original.begin(), original.begin() + length, output.begin() + 1), "stored bytes changed");
      expect(
          output.front() == guard && std::all_of(
                                         output.begin() + 1 + (capacity >= length ? length : 0), output.end(),
                                         [](BYTE b) { return b == guard; }
                                     ),
          "private data wrote outside its result"
      );
    }
    UINT size = sizeof(independent), got = 0;
    CHECK(object->GetPrivateData(other, &size, &got));
    expect(size == sizeof(independent) && got == independent, "another GUID changed");
  }
  Reference first, second;
  CHECK(object->SetPrivateDataInterface(bytes, &first));
  expect(first.references == 2, "setting interface did not AddRef");
  IUnknown *got = nullptr;
  UINT size = sizeof(got);
  expect(object->GetPrivateData(bytes, nullptr, &got) == E_INVALIDARG, "NULL private-data size");
  size = sizeof(got) - 1;
  expect(
      object->GetPrivateData(bytes, &size, &got) == DXGI_ERROR_MORE_DATA && size == sizeof(got) && !got &&
          first.references == 2,
      "short interface read changed pointer or reference count"
  );
  size = sizeof(got);
  CHECK(object->GetPrivateData(bytes, &size, &got));
  expect(size == sizeof(got) && got == &first && first.references == 3, "getting interface did not AddRef");
  if (got)
    got->Release();
  CHECK(object->SetPrivateDataInterface(bytes, &second));
  expect(first.references == 1 && second.references == 2, "interface replacement unbalanced");
  CHECK(object->SetPrivateData(bytes, sizeof(independent), &independent));
  expect(second.references == 1, "byte replacement did not release interface");
  CHECK(object->SetPrivateDataInterface(bytes, &first));
  CHECK(object->SetPrivateData(bytes, 0, nullptr));
  expect(first.references == 1, "removing interface did not release it");
  size = sizeof(got);
  expect(object->GetPrivateData(bytes, &size, &got) == DXGI_ERROR_NOT_FOUND, "removed GUID still found");
  expect(object->SetPrivateData(bytes, 0, nullptr) == S_FALSE, "redundant removal");
  CHECK(object->SetPrivateDataInterface(bytes, nullptr));
  got = &first;
  size = sizeof(got);
  CHECK(object->GetPrivateData(bytes, &size, &got));
  expect(size == sizeof(got) && !got, "NULL interface treated as absent data");
  CHECK(object->SetPrivateData(bytes, 0, nullptr));
  CHECK(object->SetPrivateData(other, 0, nullptr));
  return 0;
}

int
main() {
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  if (storage(factory.Get()))
    return 1;
  ComPtr<IDXGIAdapter1> adapter;
  CHECK(factory->EnumAdapters1(0, &adapter));
  if (storage(adapter.Get()))
    return 1;
  for (UINT index = 0;; index++) {
    ComPtr<IDXGIOutput> output;
    HRESULT hr = adapter->EnumOutputs(index, &output);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    CHECK(hr);
    if (storage(output.Get()))
      return 1;
  }
  HWND window = CreateWindowA(
      "static", "dxgi_object", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  for (UINT api : dxgi_apis()) {
    step("D3D%u swap-chain private storage", api);
    DxgiDevice device;
    CHECK(device.create(api));
    DXGI_SWAP_CHAIN_DESC1 desc{
        3,
        5,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        FALSE,
        {1, 0},
        DXGI_USAGE_RENDER_TARGET_OUTPUT,
        2,
        DXGI_SCALING_STRETCH,
        DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
    };
    Reference held;
    {
      ComPtr<IDXGISwapChain1> chain;
      CHECK(factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &chain));
      if (storage(chain.Get()))
        return 1;
      CHECK(chain->SetPrivateDataInterface(__uuidof(IUnknown), &held));
      expect(held.references == 2, "destructor fixture did not retain interface");
    }
    expect(held.references == 1, "destroyed swap chain retained its private interface");
  }
  DestroyWindow(window);
  return verdict();
}
