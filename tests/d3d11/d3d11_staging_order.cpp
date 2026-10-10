// contract: staging maps respect pending GPU writes, and CPU writes preserve both untouched bytes and earlier
// queued reads. D3D11.3 5.6.1: "Mapping means granting CPU access to the Subresource's storage or contents."
// 5.6.1.1 DISCARDRESOURCE: "the entire Resource being mapped need not be preserved"; these maps use READ, WRITE
// or READ_WRITE, without discard. 5.6.2: "This function allows sub-region copying of data from one Subresource
// to another."
// 5.6.3: "This function allows copying of an entire Resource, assuming the Resources are identical types and
// dimensions. No stretch, color key, blend, nor format conversion."
// updates target a DEFAULT buffer (5.6.8 disallows CPU-mappable destinations) before its staging copy.
// Microsoft Learn, D3D11_MAP_FLAG: "Specifies that ID3D11DeviceContext::Map should return DXGI_ERROR_WAS_STILL_DRAWING
// when the GPU blocks the CPU from accessing a resource."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map_flag
// ID3D11DeviceContext4::Wait: "Waits until the specified fence reaches or exceeds the specified value before
// future work can begin."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait
// a shared fence, released by another device, holds writes and reads pending without a slow shader. a map during
// a pending read may succeed through preserved renaming or report busy; only its bytes and earlier copies are
// asserted. failed-map output fields and allocation identities are not specified here. host waits are bounded;
// expiry fails the test and releases the held fence, never supplies an expected result. there are no shaders.
#include "d3d11_test.hpp"
#include <d3d11_4.h>
#include <functional>
#include <limits>

int
main() {
  ComPtr<ID3D11Device> device, releasing_device;
  ComPtr<ID3D11DeviceContext> context, releasing_context, deferred;
  ComPtr<ID3D11Device5> device5, releasing_device5;
  ComPtr<ID3D11DeviceContext4> context4, releasing_context4;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  step("two devices and a shared ordering fence");
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                          &releasing_device, nullptr, &releasing_context));
  CHECK(device.As(&device5));
  CHECK(releasing_device.As(&releasing_device5));
  CHECK(context.As(&context4));
  CHECK(releasing_context.As(&releasing_context4));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11Fence> held, release;
  CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&held)));
  HANDLE shared = nullptr;
  CHECK(held->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
  HRESULT opened = releasing_device5->OpenSharedFence(shared, IID_PPV_ARGS(&release));
  CloseHandle(shared);
  CHECK(opened);

  UINT64 value = 0;
  bool pending = false;
  auto hold = [&]() {
    pending = true;
    return context4->Wait(held.Get(), ++value);
  };
  auto unblock = [&]() {
    pending = false;
    return releasing_context4->Signal(release.Get(), value);
  };
  // a broken DO_NOT_WAIT must leave a failure and release the fence, rather than deadlock the test
  auto run = [&](std::function<void()> operation) {
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](void *p) -> DWORD {
          (*static_cast<const std::function<void()> *>(p))();
          return 0;
        },
        &operation, 0, nullptr);
    if (!expect(thread != nullptr, "CreateThread: %lu", GetLastError())) {
      if (pending)
        expect(unblock() == S_OK, "could not release the held fence");
      ExitProcess(1);
    }
    const DWORD wait_ms = 1000;
    DWORD waited = WaitForSingleObject(thread, wait_ms);
    if (!expect(waited == WAIT_OBJECT_0, "operation did not return: wait %lu", waited)) {
      if (pending && !expect(unblock() == S_OK, "could not release the held fence"))
        ExitProcess(1);
      if (!expect(WaitForSingleObject(thread, wait_ms) == WAIT_OBJECT_0, "operation did not return after release"))
        ExitProcess(1);
    }
    CloseHandle(thread);
    return waited == WAIT_OBJECT_0;
  };
  auto submit = [&](ID3D11DeviceContext *recording) {
    if (recording == context.Get())
      return S_OK;
    ComPtr<ID3D11CommandList> list;
    HRESULT hr = recording->FinishCommandList(FALSE, &list);
    if (hr == S_OK)
      context->ExecuteCommandList(list.Get(), FALSE);
    return hr;
  };
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT word = sizeof(UINT);
  const UINT byte_values = UINT(std::numeric_limits<uint8_t>::max()) + 1;
  const UINT sizes[] = {word, byte_values - word, byte_values, byte_values + word,
                        system.dwPageSize - word, system.dwPageSize, system.dwPageSize + word};
  const D3D11_MAP modes[] = {D3D11_MAP_READ, D3D11_MAP_WRITE, D3D11_MAP_READ_WRITE};
  for (UINT size : sizes)
    for (bool listed : {false, true}) {
      auto recording = listed ? deferred.Get() : context.Get();
      std::vector<uint8_t> old(size), newer(size);
      for (UINT i = 0; i < size; i++) {
        old[i] = 1 + i % std::numeric_limits<uint8_t>::max();
        newer[i] = ~old[i];
      }
      D3D11_BUFFER_DESC desc{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE};
      D3D11_SUBRESOURCE_DATA initial{old.data()}, replacement{newer.data()};
      ComPtr<ID3D11Buffer> source, upload;
      CHECK(device->CreateBuffer(&desc, &replacement, &upload));
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.CPUAccessFlags = 0;
      CHECK(device->CreateBuffer(&desc, &replacement, &source));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

      for (UINT writer = 0; writer < 3; writer++)
        for (auto mode : modes) {
          step("%s, %u bytes, writer %u, Map %u: idle control and held write", listed ? "list" : "immediate", size,
               writer, mode);
          ComPtr<ID3D11Buffer> staging;
          CHECK(device->CreateBuffer(&desc, &initial, &staging));
          D3D11_MAPPED_SUBRESOURCE mapped{};
          HRESULT hr = E_FAIL;
          std::vector<uint8_t> got;
          auto map = [&](UINT flags) {
            return run([&]() {
              hr = context->Map(staging.Get(), 0, mode, flags, &mapped);
              if (hr == S_OK) {
                if (mode != D3D11_MAP_WRITE)
                  got.assign((const uint8_t *)mapped.pData, (const uint8_t *)mapped.pData + size);
                context->Unmap(staging.Get(), 0);
              }
            });
          };
          if (!map(D3D11_MAP_FLAG_DO_NOT_WAIT))
            return verdict();
          expect(hr == S_OK, "idle Map returned %08lx", hr);
          if (mode != D3D11_MAP_WRITE)
            expect(got == old, "idle map changed initial bytes");
          if (writer == 2)
            context->UpdateSubresource(source.Get(), 0, nullptr, newer.data(), 0, 0);
          CHECK(hold());
          const D3D11_BOX box{word, 0, 0, size, 1, 1};
          if (writer == 0)
            recording->CopyResource(staging.Get(), source.Get());
          else if (writer == 1)
            recording->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, upload.Get(), 0, nullptr);
          else {
            recording->UpdateSubresource(source.Get(), 0, size > word ? &box : nullptr,
                                         old.data() + (size > word ? word : 0), 0, 0);
            recording->CopyResource(staging.Get(), source.Get());
          }
          CHECK(submit(recording));
          if (!map(D3D11_MAP_FLAG_DO_NOT_WAIT))
            return verdict();
          expect(hr == DXGI_ERROR_WAS_STILL_DRAWING, "held write Map returned %08lx, want WAS_STILL_DRAWING", hr);
          CHECK(unblock());
          step("%s, %u bytes, writer %u, Map %u: released write and ready control", listed ? "list" : "immediate", size,
               writer, mode);
          if (!map(0))
            return verdict();
          expect(hr == S_OK, "synchronous released Map returned %08lx", hr);
          if (!map(D3D11_MAP_FLAG_DO_NOT_WAIT))
            return verdict();
          expect(hr == S_OK, "released Map returned %08lx", hr);
          auto want = writer == 2 ? old : newer;
          if (writer == 2 && size > word)
            memcpy(want.data(), newer.data(), word);
          if (!run([&]() {
                hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                if (hr == S_OK) {
                  got.assign((const uint8_t *)mapped.pData, (const uint8_t *)mapped.pData + size);
                  context->Unmap(staging.Get(), 0);
                }
              }))
            return verdict();
          expect(hr == S_OK && got == want, "released write has wrong bytes (%08lx)", hr);
        }

      for (auto mode : modes) {
        step("%s, %u bytes, Map %u: held reads and three partial generations", listed ? "list" : "immediate", size,
             mode);
        ComPtr<ID3D11Buffer> staging;
        CHECK(device->CreateBuffer(&desc, &initial, &staging));
        // three pending replacements require more than a single retired allocation in the FIFO
        ComPtr<ID3D11Buffer> snapshots[4];
        std::vector<uint8_t> wants[std::size(snapshots)];
        auto want = old;
        CHECK(hold());
        for (UINT generation = 0; generation < std::size(snapshots); generation++) {
          snapshots[generation] = buffer(device.Get(), size, 0);
          if (!expect(!!snapshots[generation], "snapshot buffer was not created")) {
            CHECK(unblock());
            return verdict();
          }
          if (generation) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = E_FAIL;
            if (!run([&]() { hr = context->Map(staging.Get(), 0, mode, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped); }))
              return verdict();
            expect(hr == S_OK || hr == DXGI_ERROR_WAS_STILL_DRAWING, "held read Map returned %08lx", hr);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
              CHECK(unblock());
              if (!run([&]() { hr = context->Map(staging.Get(), 0, mode, 0, &mapped); }))
                return verdict();
              CHECK(hold());
            }
            if (!expect(hr == S_OK, "map after release returned %08lx", hr)) {
              CHECK(unblock());
              return verdict();
            }
            if (mode != D3D11_MAP_WRITE)
              expect(!memcmp(mapped.pData, want.data(), size), "map lost preserved bytes at generation %u", generation);
            if (mode != D3D11_MAP_READ) {
              UINT begin = (generation - 1) * size / std::size(snapshots);
              UINT end = generation * size / std::size(snapshots);
              memcpy((uint8_t *)mapped.pData + begin, newer.data() + begin, end - begin);
              memcpy(want.data() + begin, newer.data() + begin, end - begin);
            }
            context->Unmap(staging.Get(), 0);
          }
          wants[generation] = want;
          recording->CopyResource(snapshots[generation].Get(), staging.Get());
          CHECK(submit(recording));
        }
        CHECK(unblock());
        for (UINT generation = 0; generation < std::size(snapshots); generation++) {
          step("%s, %u bytes, Map %u: read snapshot %u", listed ? "list" : "immediate", size, mode, generation);
          std::vector<uint32_t> got;
          if (!run([&]() { got = read(device.Get(), context.Get(), snapshots[generation].Get()); }))
            return verdict();
          expect(got.size() * sizeof(got[0]) == size && !memcmp(got.data(), wants[generation].data(), size),
                 "snapshot %u has wrong bytes", generation);
        }
      }
    }
  return verdict();
}
