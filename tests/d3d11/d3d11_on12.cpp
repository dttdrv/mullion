// contract: an 11-on-12 device shares its D3D12 device, queues and resources,
// with ordered, byte-exact transfers between the APIs. "The queues must be of
// the 3D command queue type." (D3D11On12CreateDevice, ppCommandQueues).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-d3d11on12createdevice
// each case is a separate process: a missing interface or a failed creation
// must not hide the other contracts. Device1 and Device2 cases target Windows
// 10 version 2004 or later, the minimum for Device2.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nn-d3d11on12-id3d11on12device1
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nn-d3d11on12-id3d11on12device2
#include "d3d11_test.hpp"
#include "../d3d12/d3d12_test.hpp"
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <stop_token>
#include <thread>

int
main(int argc, char **argv) {
  const char *part = argc > 1 ? argv[1] : "creation";
  if (!expect(!strcmp(part, "creation") || !strcmp(part, "wrapped") || !strcmp(part, "wait") ||
                  !strcmp(part, "unwrap") || !strcmp(part, "swapchain") || !strcmp(part, "lifetime") ||
                  !strcmp(part, "ordering") || !strcmp(part, "teardown") || !strcmp(part, "longwait") ||
                  !strcmp(part, "dimensions") || !strcmp(part, "cpu") || !strcmp(part, "flags") || !strcmp(part, "aliases") ||
                  !strcmp(part, "predication") || !strcmp(part, "independent"),
              "unknown case: %s", part))
    return verdict();
  const char hlsl[] = R"(
    cbuffer Pattern { uint4 seed; };
    float4 vs(uint id : SV_VertexID) : SV_Position {
      float2 vertices[3] = {float2(-1, -1), float2(-1, 3), float2(3, -1)};
      return float4(vertices[id], 0, 1);
    }
    float4 ps(float4 location : SV_Position) : SV_Target {
      uint2 xy = uint2(location.xy);
      return float4((xy.x + seed.x) & 1, (xy.y + (seed.x >> 1)) & 1,
                    ((xy.x >> 1) ^ (xy.y >> 1) ^ (seed.x >> 2)) & 1, 1);
    }
  )";
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!vs_code || !ps_code)
    return 1;

  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  ComPtr<ID3D12Device> device12;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)));
  ComPtr<ID3D12CommandQueue> queues[2];
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  for (auto &queue : queues)
    CHECK(device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  IUnknown *queue_list[] = {queues[0].Get(), queues[1].Get()};
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

  if (!strcmp(part, "creation")) {
    // "Creation will fail if no acceptable feature levels are provided." NULL
    // instead chooses the D3D12 level; for a list the first level no greater
    // than that level wins (D3D11On12CreateDevice, pFeatureLevels, above).
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                        D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    D3D12_FEATURE_DATA_FEATURE_LEVELS supported{ARRAYSIZE(levels), levels};
    CHECK(device12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &supported, sizeof(supported)));
    // the D3D11 device has the levels of D3D11 on that adapter, which on Windows are the D3D12 device's
    D3D_FEATURE_LEVEL level11{};
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                            D3D11_SDK_VERSION, nullptr, &level11, nullptr));
    const auto highest = std::min(supported.MaxSupportedFeatureLevel, level11);
    for (UINT count : {1u, UINT(ARRAYSIZE(queue_list))}) {
      for (UINT flags : {0u, UINT(D3D11_CREATE_DEVICE_BGRA_SUPPORT)}) {
        for (UINT node : {0u, 1u}) {
          for (UINT request = 0; request <= ARRAYSIZE(levels); request++) {
            const auto *requested = request == ARRAYSIZE(levels) ? nullptr : &levels[request];
            const auto wanted = requested ? *requested : highest;
            step("creation: %u queues flags %#x node %#x requested %#x", count, flags, node, wanted);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL chosen{};
            HRESULT hr = D3D11On12CreateDevice(device12.Get(), flags, requested, requested ? 1 : 0, queue_list, count,
                                               node, &device, &context, &chosen);
            if (wanted > highest) {
              expect(FAILED(hr) && hr != E_NOTIMPL, "unsupported level: %#lx", hr);
              continue;
            }
            if (!expect(hr == S_OK && device && context, "creation: %#lx, device %p context %p", hr, device.Get(),
                        context.Get()))
              continue;
            expect(chosen == wanted && device->GetFeatureLevel() == wanted, "chosen %#x, device %#x, wanted %#x",
                   chosen, device->GetFeatureLevel(), wanted);
            expect(device->GetCreationFlags() == flags, "creation flags differ");
            expect(context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE, "returned context is deferred");
            ComPtr<ID3D11Device> owner;
            context->GetDevice(&owner);
            expect(owner == device, "context belongs to another device");
            ComPtr<ID3D11DeviceContext> immediate;
            device->GetImmediateContext(&immediate);
            expect(immediate == context, "GetImmediateContext returned another context");
            ComPtr<ID3D11On12Device> interop;
            CHECK(device.As(&interop));
            ComPtr<ID3D11On12Device1> interop1;
            ComPtr<ID3D11On12Device2> interop2;
            CHECK(device.As(&interop1));
            CHECK(device.As(&interop2));
            // "Retrieves the Direct3D 12 device being interoperated with."
            // (GetD3D12Device).
            // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device1-getd3d12device
            ComPtr<ID3D12Device> underlying;
            CHECK(interop1->GetD3D12Device(__uuidof(ID3D12Device), &underlying));
            ComPtr<IUnknown> original, returned;
            CHECK(device12.As(&original));
            CHECK(underlying.As(&returned));
            expect(original == returned, "GetD3D12Device returned another device");
          }
        }
      }
    }
    const D3D_FEATURE_LEVEL ordered[] = {D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_11_0};
    for (bool descending : {false, true})
      for (bool device_output : {false, true})
        for (bool context_output : {false, true})
          for (bool level_output : {false, true}) {
            step("creation: %s levels and optional outputs %u/%u/%u", descending ? "descending" : "ascending",
                 device_output, context_output, level_output);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL chosen{};
            const auto *requested = descending ? levels : ordered;
            const UINT count = descending ? ARRAYSIZE(levels) : ARRAYSIZE(ordered);
            const auto wanted = descending ? highest : ordered[0];
            HRESULT hr = D3D11On12CreateDevice(device12.Get(), 0, requested, count, queue_list, 1, 0,
                                               device_output ? device.ReleaseAndGetAddressOf() : nullptr,
                                               context_output ? context.ReleaseAndGetAddressOf() : nullptr,
                                               level_output ? &chosen : nullptr);
            expect(SUCCEEDED(hr), "optional outputs: %#lx", hr);
            expect(!device_output || device, "device output missing");
            expect(!context_output || context, "context output missing");
            expect(!level_output || chosen == wanted, "first acceptable level: %#x", chosen);
            if (device)
              expect(device->GetFeatureLevel() == wanted, "device ignored the first acceptable level");
          }
    // pDevice "May not be NULL." queues must be unique direct queues, and
    // NodeMask may have only one bit set (D3D11On12CreateDevice, above). the
    // failure must be validation, rather than an unimplemented entry point.
    auto invalid = [&](const char *name, IUnknown *device, IUnknown *const *list, UINT count,
                       const D3D_FEATURE_LEVEL *requested, UINT requested_count, UINT node) {
      step("creation: reject %s", name);
      ComPtr<ID3D11Device> out;
      ComPtr<ID3D11DeviceContext> context;
      HRESULT hr =
          D3D11On12CreateDevice(device, 0, requested, requested_count, list, count, node, &out, &context, nullptr);
      expect(FAILED(hr) && hr != E_NOTIMPL, "%s: %#lx", name, hr);
      expect(!out && !context, "%s returned a device or context", name);
    };
    invalid("null device", nullptr, queue_list, 1, &level, 1, 0);
    invalid("missing queue array", device12.Get(), nullptr, 1, &level, 1, 0);
    // zero-queue rejection is a compatibility requirement; the API page leaves
    // the empty-array case unspecified.
    invalid("no queues", device12.Get(), nullptr, 0, &level, 1, 0);
    invalid("empty feature-level list", device12.Get(), queue_list, 1, &level, 0, 0);
    IUnknown *null_queue[] = {nullptr};
    invalid("null queue", device12.Get(), null_queue, ARRAYSIZE(null_queue), &level, 1, 0);
    IUnknown *duplicates[] = {queues[0].Get(), queues[0].Get()};
    invalid("duplicate queues", device12.Get(), duplicates, ARRAYSIZE(duplicates), &level, 1, 0);
    invalid("multiple node bits", device12.Get(), queue_list, 1, &level, 1, (1u << 0) | (1u << 1));
    for (auto type : {D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY}) {
      queue_desc.Type = type;
      ComPtr<ID3D12CommandQueue> queue;
      CHECK(device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
      IUnknown *wrong_type[] = {queue.Get()};
      invalid("non-direct queue", device12.Get(), wrong_type, ARRAYSIZE(wrong_type), &level, 1, 0);
    }
    ComPtr<ID3D12Device> other;
    CHECK(D3D12CreateDevice(nullptr, level, IID_PPV_ARGS(&other)));
    ComPtr<IUnknown> original, alternate;
    CHECK(device12.As(&original));
    CHECK(other.As(&alternate));
    if (original == alternate) {
      // D3D12CreateDevice returns the existing device for an adapter; WARP
      // supplies a distinct adapter on Windows.
      // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
      ComPtr<IDXGIFactory4> factory;
      ComPtr<IDXGIAdapter> warp;
      CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
      CHECK(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
      other.Reset();
      CHECK(D3D12CreateDevice(warp.Get(), level, IID_PPV_ARGS(&other)));
      alternate.Reset();
      CHECK(other.As(&alternate));
    }
    // with one adapter, WARP included, there is one device (D3D12CreateDevice returns it), so no queue of another
    // device can reach D3D11On12CreateDevice
    if (original == alternate) {
      printf("note: one D3D12 device per adapter and one adapter, so no other device's queue to reject\n");
      return verdict();
    }
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> foreign;
    CHECK(other->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&foreign)));
    IUnknown *foreign_list[] = {foreign.Get()};
    invalid("another device's queue", device12.Get(), foreign_list, ARRAYSIZE(foreign_list), &level, 1, 0);
    return verdict();
  }

  step("%s: create the 11-on-12 device on queue 0", part);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  HRESULT hr = D3D11On12CreateDevice(device12.Get(), 0, &level, 1, queue_list, 1, 0, &device, &context, nullptr);
  if (!expect(hr == S_OK && device && context, "D3D11On12CreateDevice: %#lx", hr))
    return verdict();
  ComPtr<ID3D11On12Device> interop;
  CHECK(device.As(&interop));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  using Pixel = std::array<BYTE, 4>;
  auto pixel = [](UINT x, UINT y, UINT phase) -> Pixel {
    const auto full = std::numeric_limits<BYTE>::max();
    return {BYTE(((x + phase) % 2) * full), BYTE(((y + phase / 2) % 2) * full),
            BYTE((((x / 2) ^ (y / 2) ^ (phase / 4)) % 2) * full), full};
  };
  auto pixels = [&](const BYTE *data, UINT pitch, UINT width, UINT height, UINT phase) {
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const auto wanted = pixel(x, y, phase);
        const BYTE *got = data + y * pitch + x * sizeof(Pixel);
        if (!expect(!memcmp(got, wanted.data(), sizeof(Pixel)), "pixel %u,%u: %u,%u,%u,%u expected %u,%u,%u,%u", x, y,
                    got[0], got[1], got[2], got[3], wanted[0], wanted[1], wanted[2], wanted[3]))
          return;
      }
  };
  auto draw = [&](ID3D11Texture2D *texture, UINT phase) -> HRESULT {
    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT result = device->CreateRenderTargetView(texture, nullptr, &rtv);
    if (FAILED(result))
      return result;
    const UINT seed[4] = {phase};
    D3D11_BUFFER_DESC desc{sizeof(seed), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
    D3D11_SUBRESOURCE_DATA initial{seed};
    ComPtr<ID3D11Buffer> constants;
    if (FAILED(result = device->CreateBuffer(&desc, &initial, &constants)))
      return result;
    D3D11_TEXTURE2D_DESC dimensions;
    texture->GetDesc(&dimensions);
    D3D11_VIEWPORT viewport{0, 0, float(dimensions.Width), float(dimensions.Height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return S_OK;
  };
  auto read11 = [&](ID3D11Texture2D *texture, UINT phase) -> HRESULT {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(result))
      return result;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return result;
    pixels(static_cast<const BYTE *>(mapped.pData), mapped.RowPitch, desc.Width, desc.Height, phase);
    context->Unmap(staging.Get(), 0);
    return S_OK;
  };
  auto read12 = [&](ID3D12Resource *texture, ID3D12CommandQueue *queue, D3D12_RESOURCE_STATES state,
                    UINT phase) -> HRESULT {
    auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes;
    device12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto staging = buffer(device12.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!staging)
      return E_FAIL;
    HRESULT result = forget(staging.Get());
    if (FAILED(result))
      return result;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(result = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(result = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&list))))
      return result;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
      transition(list.Get(), texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION target{
        staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
      transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (FAILED(result = submit(device12.Get(), queue, list.Get())))
      return result;
    BYTE *mapped;
    if (FAILED(result = staging->Map(0, nullptr, reinterpret_cast<void **>(&mapped))))
      return result;
    pixels(mapped + footprint.Offset, footprint.Footprint.RowPitch, UINT(desc.Width), desc.Height, phase);
    staging->Unmap(0, nullptr);
    return S_OK;
  };
  const UINT aligned_width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / sizeof(Pixel);
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                   0,
                                   aligned_width + 1,
                                   3,
                                   1,
                                   1,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,
                                   {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

  if (!strcmp(part, "independent")) {
    // Return waits belong to the allocation first used, including through another wrapper or a deferred list.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-returnunderlyingresource
    ComPtr<ID3D11On12Device2> on12;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    CHECK(device.As(&on12));
    CHECK(device.As(&device5));
    CHECK(context.As(&context4));
    for (UINT phase : {1u, 2u, 3u}) {
      step("independent: B completes while A waits, then A's deferred alias survives its original wrapper, phase %u", phase);
      ComPtr<ID3D12Resource> original[2], underlying[2];
      ComPtr<ID3D11Texture2D> wrapped[2][2], staging[2];
      D3D11_RESOURCE_FLAGS flags{D3D11_BIND_RENDER_TARGET};
      D3D11_TEXTURE2D_DESC desc{UINT(texture_desc.Width), texture_desc.Height, 1, 1, texture_desc.Format,
                              {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
      std::vector<Pixel> zeroes(desc.Width * desc.Height);
      D3D11_SUBRESOURCE_DATA initial{zeroes.data(), UINT(desc.Width * sizeof(Pixel))};
      for (UINT i = 0; i < std::size(original); ++i) {
        CHECK(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&original[i])));
        for (auto &alias : wrapped[i])
          CHECK(interop->CreateWrappedResource(original[i].Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&alias)));
        CHECK(device->CreateTexture2D(&desc, &initial, &staging[i]));
        CHECK(draw(wrapped[i][0].Get(), i ? phase + 4 : 0));
        CHECK(on12->UnwrapUnderlyingResource(wrapped[i][0].Get(), queues[1].Get(), IID_PPV_ARGS(&underlying[i])));
      }
      context->Flush();
      CHECK(read12(underlying[0].Get(), queues[1].Get(), D3D12_RESOURCE_STATE_COMMON, 0));
      CHECK(read12(underlying[1].Get(), queues[1].Get(), D3D12_RESOURCE_STATE_COMMON, phase + 4));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 bytes;
      device12->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto upload = buffer(device12.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!expect(upload != nullptr, "upload buffer was not created"))
        return verdict();
      BYTE *mapped;
      CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
      for (UINT y = 0; y < desc.Height; ++y)
        for (UINT x = 0; x < desc.Width; ++x)
          memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(Pixel),
                 pixel(x, y, phase).data(), sizeof(Pixel));
      upload->Unmap(0, nullptr);
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> producer;
      CHECK(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&producer)));
      transition(producer.Get(), underlying[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                       {.PlacedFootprint = footprint}},
          to{underlying[0].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      producer->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(producer.Get(), underlying[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      CHECK(producer->Close());
      ComPtr<ID3D11DeviceContext> deferred;
      ComPtr<ID3D11CommandList> consumer;
      CHECK(device->CreateDeferredContext(0, &deferred));
      deferred->CopyResource(staging[0].Get(), wrapped[0][1].Get());
      CHECK(deferred->FinishCommandList(FALSE, &consumer));
      ComPtr<ID3D12Fence> gate, produced, available, retired;
      CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
      CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&produced)));
      CHECK(device12->CreateFence(1, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&available)));
      CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retired)));
      ComPtr<ID3D11Fence> completed;
      CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)));
      std::unique_ptr<void, decltype(&CloseHandle)> release(CreateEventA(nullptr, TRUE, FALSE, nullptr), &CloseHandle),
          done(CreateEventA(nullptr, FALSE, FALSE, nullptr), &CloseHandle),
          finished(CreateEventA(nullptr, TRUE, FALSE, nullptr), &CloseHandle),
          drained(CreateEventA(nullptr, FALSE, FALSE, nullptr), &CloseHandle);
      if (!expect(release && done && finished && drained, "completion events were not created"))
        return verdict();
      std::atomic<bool> opened = false;
      HRESULT released = E_FAIL;
      std::jthread watchdog([&](std::stop_token stop) {
        std::stop_callback finished_submission(stop, [&] { SetEvent(finished.get()); });
        WaitForSingleObject(release.get(), 5000);
        opened = true;
        released = gate->Signal(1);
        // retire after submission ends, including when a failing API call unwinds this scope.
        WaitForSingleObject(finished.get(), INFINITE);
        HRESULT hr = queues[1]->Signal(retired.Get(), 1);
        if (SUCCEEDED(hr))
          hr = retired->SetEventOnCompletion(1, drained.get());
        if (FAILED(hr) || WaitForSingleObject(drained.get(), 5000) != WAIT_OBJECT_0) {
          printf("failed: independent producer cleanup did not finish, %#lx\n", hr);
          ExitProcess(1);
        }
      });
      CHECK(queues[1]->Wait(gate.Get(), 1));
      ID3D12CommandList *lists[] = {producer.Get()};
      queues[1]->ExecuteCommandLists(1, lists);
      CHECK(queues[1]->Signal(produced.Get(), 1));
      UINT64 one = 1;
      ID3D12Fence *a[] = {produced.Get()}, *b[] = {available.Get()};
      CHECK(on12->ReturnUnderlyingResource(wrapped[0][0].Get(), 1, &one, a));
      CHECK(on12->ReturnUnderlyingResource(wrapped[1][0].Get(), 1, &one, b));
      produced.Reset();
      available.Reset();
      for (UINT i = 0; i < std::size(original); ++i) {
        wrapped[i][0].Reset();
        underlying[i].Reset();
        original[i].Reset();
      }
      context->CopyResource(staging[1].Get(), wrapped[1][1].Get());
      CHECK(completed->SetEventOnCompletion(1, done.get()));
      CHECK(context4->Signal(completed.Get(), 1));
      context->Flush();
      expect(WaitForSingleObject(done.get(), 1000) == WAIT_OBJECT_0 && !opened,
             "B inherited A's pending return fence");
      CHECK(completed->SetEventOnCompletion(2, done.get()));
      context->ExecuteCommandList(consumer.Get(), FALSE);
      CHECK(context4->Signal(completed.Get(), 2));
      context->Flush();
      const DWORD early = WaitForSingleObject(done.get(), 100);
      expect(early == WAIT_TIMEOUT && !opened,
             "A's deferred alias bypassed its released return fence");
      SetEvent(release.get());
      watchdog.request_stop();
      watchdog.join();
      CHECK(released);
      if (!expect(early == WAIT_OBJECT_0 || WaitForSingleObject(done.get(), 5000) == WAIT_OBJECT_0,
                  "A's deferred alias did not finish"))
        return verdict();
      for (UINT i = 0; i < std::size(staging); ++i) {
        D3D11_MAPPED_SUBRESOURCE result;
        CHECK(context->Map(staging[i].Get(), 0, D3D11_MAP_READ, 0, &result));
        pixels(static_cast<const BYTE *>(result.pData), result.RowPitch, desc.Width, desc.Height, i ? phase + 4 : phase);
        context->Unmap(staging[i].Get(), 0);
      }
      CHECK(device->GetDeviceRemovedReason());
      CHECK(device12->GetDeviceRemovedReason());
    }
    return verdict();
  }

  if (!strcmp(part, "cpu") || !strcmp(part, "aliases") || !strcmp(part, "predication")) {
    const bool aliases = strcmp(part, "cpu") != 0;
    // CPU flags constrain the heap's pages; wrappers stay DEFAULT, Map synchronizes their D3D11 uses, and DISCARD
    // and NO_OVERWRITE are excluded.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/ns-d3d11on12-d3d11_resource_flags
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    ComPtr<ID3D11Device3> device3;
    CHECK(device.As(&device3));
    auto mapping = [&](const D3D12_HEAP_PROPERTIES &properties, const D3D12_RESOURCE_DESC &desc) -> HRESULT {
      auto state = properties.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                   : properties.Type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                               : D3D12_RESOURCE_STATE_COMMON;
      ComPtr<ID3D12Resource> original;
      HRESULT result = device12->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(&original));
      if (FAILED(result))
        return result;
      auto resolved = properties.Type == D3D12_HEAP_TYPE_CUSTOM
                          ? properties : device12->GetCustomHeapProperties(0, properties.Type);
      UINT allowed = resolved.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
                         ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE
                     : resolved.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ? D3D11_CPU_ACCESS_WRITE : 0;
      bool is_buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
      UINT width = is_buffer ? UINT(desc.Width) / sizeof(UINT) : UINT(desc.Width);
      UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
      UINT row = width * sizeof(UINT), image = row * desc.Height;
      for (UINT access : {0u, UINT(D3D11_CPU_ACCESS_READ), UINT(D3D11_CPU_ACCESS_WRITE),
                          UINT(D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE),
                          ~UINT(D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)}) {
        step("CPU: heap %u pages %u dimension %u width %u access %#x", properties.Type, resolved.CPUPageProperty,
             desc.Dimension, width, access);
        D3D11_RESOURCE_FLAGS flags{0, 0, access};
        ComPtr<ID3D11Resource> wrapped;
        result = interop->CreateWrappedResource(original.Get(), &flags, state, state, IID_PPV_ARGS(&wrapped));
        bool valid = !(access & ~allowed);
        expect(result == (valid ? S_OK : E_INVALIDARG) && bool(wrapped) == valid,
               "wrapping returned %#lx, expected %#lx", result, valid ? S_OK : E_INVALIDARG);
        expect(interop->CreateWrappedResource(original.Get(), &flags, state, state, __uuidof(ID3D11Resource), nullptr) ==
                   (valid ? S_FALSE : E_INVALIDARG), "validation-only CPU wrapping changed validation");
        if (!wrapped)
          continue;
        if (is_buffer) {
          ComPtr<ID3D11Buffer> data;
          CHECK(wrapped.As(&data));
          D3D11_BUFFER_DESC got;
          data->GetDesc(&got);
          expect(got.Usage == D3D11_USAGE_DEFAULT && got.CPUAccessFlags == access,
                 "wrapper changed DEFAULT usage or CPU access flags");
        }
        for (auto mode : {D3D11_MAP_READ, D3D11_MAP_WRITE, D3D11_MAP_READ_WRITE,
                          D3D11_MAP_WRITE_DISCARD, D3D11_MAP_WRITE_NO_OVERWRITE}) {
          bool reading = mode == D3D11_MAP_READ || mode == D3D11_MAP_READ_WRITE;
          bool writing = mode == D3D11_MAP_WRITE || mode == D3D11_MAP_READ_WRITE;
          bool permitted = mode <= D3D11_MAP_READ_WRITE && (!reading || (access & D3D11_CPU_ACCESS_READ)) &&
                           (!writing || (access & D3D11_CPU_ACCESS_WRITE));
          std::vector<UINT> wanted(size_t(width) * desc.Height * depth), got(wanted.size());
          for (UINT i = 0; i < wanted.size(); i++)
            wanted[i] = i + 1;
          if (permitted && reading) {
            void *data;
            if (FAILED(result = original->Map(0, nullptr, is_buffer ? &data : nullptr)))
              return result;
            if (is_buffer)
              memcpy(data, wanted.data(), row);
            else if (FAILED(result = original->WriteToSubresource(0, nullptr, wanted.data(), row, image)))
              return result;
            original->Unmap(0, nullptr);
          }
          D3D11_MAPPED_SUBRESOURCE mapped{};
          result = context->Map(wrapped.Get(), 0, mode, 0, is_buffer ? &mapped : nullptr);
          expect(result == (permitted ? S_OK : E_INVALIDARG), "Map mode %u: %#lx", mode, result);
          if (FAILED(result))
            continue;
          if (reading) {
            if (is_buffer)
              memcpy(got.data(), mapped.pData, row);
            else
              device3->ReadFromSubresource(got.data(), row, image, wrapped.Get(), 0, nullptr);
            expect(got == wanted, "Map did not expose the underlying allocation's bytes");
          }
          if (writing) {
            for (auto &word : wanted)
              word += width;
            if (is_buffer)
              memcpy(mapped.pData, wanted.data(), row);
            else
              device3->WriteToSubresource(wrapped.Get(), 0, nullptr, wanted.data(), row, image);
          }
          context->Unmap(wrapped.Get(), 0);
          if (writing) {
            void *data;
            if (FAILED(result = original->Map(0, nullptr, is_buffer ? &data : nullptr)))
              return result;
            if (is_buffer)
              memcpy(got.data(), data, row);
            else if (FAILED(result = original->ReadFromSubresource(got.data(), row, image, 0, nullptr)))
              return result;
            original->Unmap(0, nullptr);
            expect(got == wanted, "D3D11 Map wrote a different allocation");
          }
        }
      }
      return S_OK;
    };
    if (!aliases) {
      for (auto type : {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK,
                        D3D12_HEAP_TYPE_CUSTOM}) {
        for (auto pages : {D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE,
                           D3D12_CPU_PAGE_PROPERTY_WRITE_BACK}) {
          if (type != D3D12_HEAP_TYPE_CUSTOM && pages != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
            continue;
          D3D12_HEAP_PROPERTIES properties{type};
          if (type == D3D12_HEAP_TYPE_CUSTOM) {
            properties.CPUPageProperty = pages;
            properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
          }
          for (int delta : {-1, 0, 1}) {
            D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0,
                                     UINT64(system.dwPageSize + delta * int(sizeof(UINT))), 1, 1, 1,
                                     DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
            CHECK(mapping(properties, desc));
          }
        }
      }
      for (auto pages : {D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK}) {
        D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_CUSTOM, pages, D3D12_MEMORY_POOL_L0};
        for (auto dimension : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
          D3D12_RESOURCE_DESC desc{dimension, 0, 3, dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : 3u,
                                   UINT16(dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 2 : 1), 1,
                                   DXGI_FORMAT_R32_UINT, {1, 0}};
          CHECK(mapping(properties, desc));
        }
      }
    }
    // a native D3D11 shared fence gates actual GPU work independently of the On12 acquire sequence.
    ComPtr<ID3D11Device> producer_device;
    ComPtr<ID3D11DeviceContext> producer_context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                            &producer_device, nullptr, &producer_context));
    ComPtr<ID3D11Device5> device5, producer5;
    ComPtr<ID3D11DeviceContext4> context4, producer4;
    CHECK(device.As(&device5));
    CHECK(context.As(&context4));
    CHECK(producer_device.As(&producer5));
    CHECK(producer_context.As(&producer4));
    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
                                     D3D12_MEMORY_POOL_L0};
    D3D12_RESOURCE_DESC order_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, system.dwPageSize, 1, 1, 1,
                                   DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> original;
    CHECK(device12->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &order_desc,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&original)));
    ComPtr<ID3D11Buffer> wrapped;
    D3D11_RESOURCE_FLAGS flags{D3D11_BIND_CONSTANT_BUFFER, 0,
                                aliases ? 0u : UINT(D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)};
    CHECK(interop->CreateWrappedResource(original.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                         D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&wrapped)));
    auto source = buffer(device.Get(), system.dwPageSize, 0, system.dwPageSize);
    if (!expect(source != nullptr, "CPU ordering copy buffer failed"))
      return verdict();
    D3D11_TEXTURE2D_DESC target_desc{1, 1, 1, 1, texture_desc.Format, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> render_target;
    ComPtr<ID3D11RenderTargetView> render_view;
    CHECK(device->CreateTexture2D(&target_desc, nullptr, &render_target));
    CHECK(device->CreateRenderTargetView(render_target.Get(), nullptr, &render_view));
    auto workload = [&](ID3D11DeviceContext *record, bool gpu_write) {
      if (gpu_write) {
        record->CopyResource(wrapped.Get(), source.Get());
      } else {
        D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        record->RSSetViewports(1, &viewport);
        record->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        record->VSSetShader(vs.Get(), nullptr, 0);
        record->PSSetShader(ps.Get(), nullptr, 0);
        record->PSSetConstantBuffers(0, 1, wrapped.GetAddressOf());
        record->OMSetRenderTargets(1, render_view.GetAddressOf(), nullptr);
        record->Draw(3, 0);
        record->OMSetRenderTargets(0, nullptr, nullptr);
      }
    };
    ComPtr<ID3D11Buffer> cpu_wrapped = wrapped;
    ComPtr<ID3D11CommandList> recorded[2];
    if (aliases) {
      // work recorded through A must remain visible to a later CPU-enabled wrapper of the same resource.
      for (bool gpu_write : {false, true}) {
        ComPtr<ID3D11DeviceContext> record;
        CHECK(device->CreateDeferredContext(0, &record));
        workload(record.Get(), gpu_write);
        CHECK(record->FinishCommandList(FALSE, &recorded[gpu_write]));
      }
      flags.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      cpu_wrapped.Reset();
      CHECK(interop->CreateWrappedResource(original.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&cpu_wrapped)));
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CHECK(context->Map(cpu_wrapped.Get(), 0, D3D11_MAP_WRITE, 0, &mapped));
    if (!strcmp(part, "predication"))
      memset(mapped.pData, 0, system.dwPageSize);
    context->Unmap(cpu_wrapped.Get(), 0);
    if (!strcmp(part, "predication")) {
      // resolving a pending predicate can submit a chunk; the copy's Map must wait for its actual later chunk.
      // https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication
      D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION_PREDICATE};
      ComPtr<ID3D11Predicate> predicate;
      CHECK(device->CreatePredicate(&desc, &predicate));
      context->Begin(predicate.Get());
      CHECK(draw(render_target.Get(), 0));
      context->End(predicate.Get());
      expect(context->GetData(predicate.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE,
             "predicate must still require its queued draw");
      ComPtr<ID3D11DeviceContext> record;
      ComPtr<ID3D11CommandList> list;
      CHECK(device->CreateDeferredContext(0, &record));
      record->SetPredication(predicate.Get(), FALSE);
      record->CopyResource(wrapped.Get(), source.Get());
      CHECK(record->FinishCommandList(FALSE, &list));
      context->ExecuteCommandList(list.Get(), TRUE);
      CHECK(context->Map(cpu_wrapped.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      auto words = static_cast<const UINT *>(mapped.pData);
      expect(std::all_of(words, words + system.dwPageSize / sizeof(UINT),
                         [&](UINT word) { return word == system.dwPageSize; }),
             "Map missed the copy queued after resolving its predicate");
      context->Unmap(cpu_wrapped.Get(), 0);
      context->ClearState();
      context->Flush();
      return verdict();
    }
    for (bool deferred : {false, true}) {
      for (bool gpu_write : {false, true}) {
        ComPtr<ID3D11CommandList> list;
        if (deferred) {
          if (aliases) {
            list = recorded[gpu_write];
          } else {
            ComPtr<ID3D11DeviceContext> record;
            CHECK(device->CreateDeferredContext(0, &record));
            workload(record.Get(), gpu_write);
            CHECK(record->FinishCommandList(FALSE, &list));
          }
        }
        for (UINT replay = 0; replay < (aliases ? 2u : 1u); replay++) {
          step("CPU: %s GPU %s ordering, %s replay %u", deferred ? "deferred" : "immediate",
               gpu_write ? "write" : "read", aliases ? "alias" : "same wrapper", replay);
          ComPtr<ID3D11Fence> held, opened;
          CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&held)));
          HANDLE shared;
          CHECK(held->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
          CHECK(producer5->OpenSharedFence(shared, IID_PPV_ARGS(&opened)));
          CloseHandle(shared);
          CHECK(context4->Wait(held.Get(), 1));
          if (deferred)
            context->ExecuteCommandList(list.Get(), TRUE);
          else
            workload(context.Get(), gpu_write);
          for (auto mode : {D3D11_MAP_READ, D3D11_MAP_WRITE, D3D11_MAP_READ_WRITE}) {
            HRESULT result = context->Map(cpu_wrapped.Get(), 0, mode, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            expect(result == (!gpu_write && mode == D3D11_MAP_READ ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING),
                   "GPU %s, CPU map %u: %#lx", gpu_write ? "write" : "read", mode, result);
            if (SUCCEEDED(result))
              context->Unmap(cpu_wrapped.Get(), 0);
          }
          CHECK(producer4->Signal(opened.Get(), 1));
          CHECK(context->Map(cpu_wrapped.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped));
          if (gpu_write) {
            auto words = static_cast<const UINT *>(mapped.pData);
            expect(std::all_of(words, words + system.dwPageSize / sizeof(UINT),
                               [&](UINT word) { return word == system.dwPageSize; }),
                   "CPU alias did not observe the completed GPU copy");
          }
          context->Unmap(cpu_wrapped.Get(), 0);
        }
      }
    }
    if (aliases) {
      ComPtr<ID3D12Resource> original_texture;
      CHECK(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&original_texture)));
      ComPtr<ID3D11Texture2D> a, b;
      D3D11_RESOURCE_FLAGS texture_flags{D3D11_BIND_RENDER_TARGET};
      CHECK(interop->CreateWrappedResource(original_texture.Get(), &texture_flags, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&a)));
      CHECK(interop->CreateWrappedResource(original_texture.Get(), &texture_flags, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&b)));
      ComPtr<ID3D11On12Device2> on12;
      CHECK(device.As(&on12));
      ComPtr<ID3D11RenderTargetView> view;
      CHECK(device->CreateRenderTargetView(b.Get(), nullptr, &view));
      D3D11_TEXTURE2D_DESC desc;
      b->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Texture2D> staging;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      ComPtr<ID3D11Fence> completed;
      CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)));
      for (UINT replay : {0u, 1u}) {
        step("aliases: return A, use B, replay %u", replay);
        ComPtr<ID3D12Resource> underlying;
        CHECK(on12->UnwrapUnderlyingResource(a.Get(), queues[1].Get(), IID_PPV_ARGS(&underlying)));
        ComPtr<ID3D12Fence> held;
        CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
        HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!expect(done != nullptr, "CreateEvent: %lu", GetLastError()))
          return verdict();
        CHECK(completed->SetEventOnCompletion(replay + 1, done));
        UINT64 one = 1;
        ID3D12Fence *fences[] = {held.Get()};
        CHECK(on12->ReturnUnderlyingResource(a.Get(), 1, &one, fences));
        const FLOAT magenta[] = {1, 0, 1, 1};
        context->ClearRenderTargetView(view.Get(), magenta);
        context->CopyResource(staging.Get(), b.Get());
        HRESULT signaled = context4->Signal(completed.Get(), replay + 1);
        context->Flush();
        DWORD early = WaitForSingleObject(done, 100);
        CHECK(held->Signal(1));
        CHECK(signaled);
        expect(early == WAIT_TIMEOUT, "wrapper B bypassed wrapper A's returned fence");
        expect(early == WAIT_OBJECT_0 || WaitForSingleObject(done, 5000) == WAIT_OBJECT_0,
               "alias consumer did not finish");
        CloseHandle(done);
        CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        BYTE full = std::numeric_limits<BYTE>::max();
        const Pixel wanted{full, 0, full, full};
        for (UINT y = 0; y < desc.Height; y++)
          for (UINT x = 0; x < desc.Width; x++)
            expect(!memcmp(static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(Pixel),
                           wanted.data(), sizeof(Pixel)), "alias copy lost a cleared pixel");
        context->Unmap(staging.Get(), 0);
      }
    }
    if (!aliases) {
      for (UINT arrays : {UINT(std::numeric_limits<UINT64>::digits / 2 - 1),
                           UINT(std::numeric_limits<UINT64>::digits / 2)}) {
        auto desc = texture_desc;
        desc.DepthOrArraySize = arrays;
        desc.MipLevels = 2;
        ComPtr<ID3D12Resource> original_texture;
        CHECK(device12->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&original_texture)));
        ComPtr<ID3D11Texture2D> texture;
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(interop->CreateWrappedResource(original_texture.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                             D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&texture)));
        CHECK(context->Map(texture.Get(), 0, D3D11_MAP_READ, 0, nullptr));
        context->Unmap(texture.Get(), 0);
        D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
        view_desc.Format = desc.Format;
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view_desc.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11RenderTargetView> view;
        CHECK(device->CreateRenderTargetView(texture.Get(), &view_desc, &view));
        ComPtr<ID3D11DeviceContext> record;
        CHECK(device->CreateDeferredContext(0, &record));
        const FLOAT magenta[] = {1, 0, 1, 1};
        record->ClearRenderTargetView(view.Get(), magenta);
        ComPtr<ID3D11CommandList> list;
        CHECK(record->FinishCommandList(FALSE, &list));
        for (UINT replay : {0u, 1u}) {
          step("CPU: opaque texture %u subresources, replay %u", arrays * desc.MipLevels, replay);
          ComPtr<ID3D11Fence> held, opened;
          CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&held)));
          HANDLE shared;
          CHECK(held->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
          CHECK(producer5->OpenSharedFence(shared, IID_PPV_ARGS(&opened)));
          CloseHandle(shared);
          CHECK(context4->Wait(held.Get(), 1));
          context->ExecuteCommandList(list.Get(), TRUE);
          D3D11_MAPPED_SUBRESOURCE invalid{};
          expect(context->Map(texture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &invalid) == E_INVALIDARG,
                 "invalid opaque pointer mapping reported a synchronization error");
          for (UINT sub : {0u, arrays * desc.MipLevels - 1}) {
            HRESULT result = context->Map(texture.Get(), sub, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, nullptr);
            expect(result == (sub ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING), "subresource %u: %#lx", sub, result);
            if (SUCCEEDED(result))
              context->Unmap(texture.Get(), sub);
          }
          CHECK(producer4->Signal(opened.Get(), 1));
          CHECK(context->Map(texture.Get(), 0, D3D11_MAP_READ, 0, nullptr));
          std::vector<Pixel> got(desc.Width * desc.Height);
          device3->ReadFromSubresource(got.data(), UINT(desc.Width) * sizeof(Pixel), 0, texture.Get(), 0, nullptr);
          context->Unmap(texture.Get(), 0);
          BYTE full = std::numeric_limits<BYTE>::max();
          const Pixel expected{full, 0, full, full};
          expect(std::all_of(got.begin(), got.end(), [&](const Pixel &p) { return p == expected; }),
                 "opaque texture Map lost the deferred clear");
        }
      }
    }
  }

  if (!strcmp(part, "flags")) {
    // SHARED and NTHANDLE follow the heap; the D3D12 owner still owns export support.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/ns-d3d11on12-d3d11_resource_flags
    for (bool texture : {false, true}) {
      auto desc = texture_desc;
      if (!texture)
        desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, sizeof(UINT), 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                  {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
      for (auto sharing : {D3D12_HEAP_FLAG_NONE, D3D12_HEAP_FLAG_SHARED}) {
        ComPtr<ID3D12Resource> original;
        CHECK(device12->CreateCommittedResource(&heap, sharing, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(&original)));
        for (UINT misc : {0u, UINT(D3D11_RESOURCE_MISC_SHARED_NTHANDLE)}) {
          D3D11_RESOURCE_FLAGS flags{0, misc};
          ComPtr<ID3D11Resource> wrapped;
          HRESULT result = interop->CreateWrappedResource(original.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                                           D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&wrapped));
          bool valid = !misc || sharing;
          expect(result == (valid ? S_OK : E_INVALIDARG) && bool(wrapped) == valid, "shared wrapping: %#lx", result);
          expect(interop->CreateWrappedResource(original.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                                  D3D12_RESOURCE_STATE_COMMON, __uuidof(ID3D11Resource), nullptr) ==
                     (valid ? S_FALSE : E_INVALIDARG), "validation-only shared wrapping changed validation");
          if (!wrapped)
            continue;
          UINT actual;
          if (texture) {
            ComPtr<ID3D11Texture2D> data;
            CHECK(wrapped.As(&data));
            D3D11_TEXTURE2D_DESC got;
            data->GetDesc(&got);
            actual = got.MiscFlags;
          } else {
            ComPtr<ID3D11Buffer> data;
            CHECK(wrapped.As(&data));
            D3D11_BUFFER_DESC got;
            data->GetDesc(&got);
            actual = got.MiscFlags;
          }
          const UINT inferred = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
          expect((actual & inferred) == (sharing ? inferred : 0), "wrapper lost inferred shared flags");
          if (sharing) {
            HANDLE native = nullptr, alias = nullptr;
            auto owner_result = device12->CreateSharedHandle(original.Get(), nullptr, GENERIC_ALL, nullptr, &native);
            ComPtr<IDXGIResource1> dxgi;
            CHECK(wrapped.As(&dxgi));
            expect(dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &alias) == owner_result,
                   "wrapper did not return its D3D12 owner's sharing result");
            if (native)
              CloseHandle(native);
            if (alias)
              CloseHandle(alias);
          }
        }
      }
    }
  }

  if (!strcmp(part, "dimensions")) {
    // unwrap excludes buffers, keyed mutexes and GDI resources; wrapped 1D arrays and 3D textures are textures too.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-unwrapunderlyingresource
    ComPtr<ID3D11On12Device2> on12;
    CHECK(device.As(&on12));
    for (bool volume : {false, true}) {
      for (bool wrapped : {false, true}) {
        step("dimensions: %s %s", volume ? "3D" : "1D array", wrapped ? "wrapped" : "native");
        D3D12_RESOURCE_DESC desc{volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE1D,
                                 0, 3, volume ? 2u : 1u, 3, 1, texture_desc.Format, {1, 0}};
        ComPtr<ID3D12Resource> original;
        ComPtr<ID3D11Resource> texture;
        if (wrapped) {
          CHECK(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, IID_PPV_ARGS(&original)));
          D3D11_RESOURCE_FLAGS flags{D3D11_BIND_SHADER_RESOURCE};
          CHECK(interop->CreateWrappedResource(original.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&texture)));
        } else if (volume) {
          D3D11_TEXTURE3D_DESC desc11{UINT(desc.Width), desc.Height, desc.DepthOrArraySize, desc.MipLevels,
                                     desc.Format, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
          ComPtr<ID3D11Texture3D> created;
          CHECK(device->CreateTexture3D(&desc11, nullptr, &created));
          texture = created;
        } else {
          D3D11_TEXTURE1D_DESC desc11{UINT(desc.Width), desc.MipLevels, desc.DepthOrArraySize, desc.Format,
                                     D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
          ComPtr<ID3D11Texture1D> created;
          CHECK(device->CreateTexture1D(&desc11, nullptr, &created));
          texture = created;
        }
        const UINT count = volume ? 1 : desc.DepthOrArraySize;
        const UINT depth = volume ? desc.DepthOrArraySize : 1;
        const UINT row_pitch = UINT(desc.Width) * sizeof(Pixel), depth_pitch = row_pitch * desc.Height;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
        UINT64 bytes;
        device12->GetCopyableFootprints(&desc, 0, count, 0, layouts.data(), nullptr, nullptr, &bytes);
        auto staging = buffer(device12.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(staging != nullptr, "readback creation failed"))
          return verdict();
        for (UINT round : {0u, 1u}) {
          std::vector<std::vector<Pixel>> wanted(count);
          for (UINT sub = 0; sub < count; sub++) {
            auto &data = wanted[sub];
            for (UINT z = 0; z < depth; z++)
              for (UINT y = 0; y < desc.Height; y++)
                for (UINT x = 0; x < desc.Width; x++)
                  data.push_back(pixel(x, y, sub + z + round));
            context->UpdateSubresource(texture.Get(), sub, nullptr, data.data(), row_pitch, depth_pitch);
          }
          ComPtr<ID3D12Resource> underlying;
          hr = on12->UnwrapUnderlyingResource(texture.Get(), queues[1].Get(), IID_PPV_ARGS(&underlying));
          if (!expect(hr == S_OK && underlying, "texture unwrap failed: %#lx", hr))
            break;
          if (original) {
            ComPtr<IUnknown> a, b;
            CHECK(original.As(&a));
            CHECK(underlying.As(&b));
            expect(a == b, "unwrap changed the allocation identity");
          }
          auto got = underlying->GetDesc();
          expect(got.Dimension == desc.Dimension && got.Width == desc.Width && got.Height == desc.Height &&
                     got.DepthOrArraySize == desc.DepthOrArraySize, "unwrap changed the texture dimensions");
          ComPtr<ID3D12CommandAllocator> allocator;
          ComPtr<ID3D12GraphicsCommandList> list;
          CHECK(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
          CHECK(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&list)));
          transition(list.Get(), underlying.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
          for (UINT sub = 0; sub < count; sub++) {
            D3D12_TEXTURE_COPY_LOCATION from{underlying.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                             {.SubresourceIndex = sub}};
            D3D12_TEXTURE_COPY_LOCATION to{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                           {.PlacedFootprint = layouts[sub]}};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          }
          transition(list.Get(), underlying.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
          CHECK(submit(device12.Get(), queues[1].Get(), list.Get()));
          BYTE *mapped;
          CHECK(staging->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
          for (UINT sub = 0; sub < count; sub++)
            for (UINT z = 0; z < depth; z++)
              for (UINT y = 0; y < desc.Height; y++) {
                auto &layout = layouts[sub];
                auto offset = layout.Offset + (z * desc.Height + y) * layout.Footprint.RowPitch;
                expect(!memcmp(mapped + offset, wanted[sub].data() + (z * desc.Height + y) * desc.Width, row_pitch),
                       "round %u subresource %u depth %u row %u lost bytes", round, sub, z, y);
              }
          staging->Unmap(0, nullptr);
          CHECK(on12->ReturnUnderlyingResource(texture.Get(), 0, nullptr, nullptr));
        }
      }
    }
  }

  if (!strcmp(part, "teardown")) {
    // final device release drains submitted work, but cannot require an external producer to signal forever.
    ComPtr<ID3D11Device> owner;
    ComPtr<ID3D11DeviceContext> immediate;
    CHECK(D3D11On12CreateDevice(device12.Get(), 0, &level, 1, queue_list, 1, 0, &owner, &immediate, nullptr));
    ComPtr<ID3D11On12Device2> on12;
    ComPtr<ID3D11DeviceContext3> immediate3;
    CHECK(owner.As(&on12));
    CHECK(immediate.As(&immediate3));
    D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, texture_desc.Format, {1, 0}, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(owner->CreateTexture2D(&desc, nullptr, &texture));
    CHECK(owner->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
    ComPtr<ID3D12Resource> underlying;
    CHECK(on12->UnwrapUnderlyingResource(texture.Get(), queues[1].Get(), IID_PPV_ARGS(&underlying)));
    ComPtr<ID3D12Fence> held;
    CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
    UINT64 one = 1;
    ID3D12Fence *fences[] = {held.Get()};
    CHECK(on12->ReturnUnderlyingResource(texture.Get(), 1, &one, fences));
    const FLOAT color[] = {1, 0, 0, 1};
    immediate->ClearRenderTargetView(rtv.Get(), color);
    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE released = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!expect(done && released, "CreateEvent: %lu", GetLastError()))
      return verdict();
    immediate3->Flush1(D3D11_CONTEXT_TYPE_ALL, done);
    immediate->ClearState();
    immediate3.Reset();
    immediate.Reset();
    on12.Reset();
    rtv.Reset();
    texture.Reset();
    std::atomic<bool> fallback = false;
    std::thread producer([&] {
      if (WaitForSingleObject(released, 1000) == WAIT_TIMEOUT) {
        fallback = true;
        held->Signal(1);
      }
    });
    owner.Reset();
    expect(!fallback, "device release waited for an unsignaled external fence");
    expect(WaitForSingleObject(done, 0) == WAIT_TIMEOUT, "abandoned work reported GPU completion");
    SetEvent(released);
    producer.join();
    CHECK(held->Signal(1));
    CloseHandle(released);
    CloseHandle(done);
  }

  if (!strcmp(part, "longwait")) {
    // pending producers can outlive the native GPU timeout, so their waits must hold CPU submission.
    ComPtr<ID3D11On12Device2> on12;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    CHECK(device.As(&on12));
    CHECK(device.As(&device5));
    CHECK(context.As(&context4));
    D3D11_TEXTURE2D_DESC desc{UINT(texture_desc.Width), texture_desc.Height, 1, 1, texture_desc.Format,
                             {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    CHECK(draw(texture.Get(), 3));
    ComPtr<ID3D12Resource> underlying;
    CHECK(on12->UnwrapUnderlyingResource(texture.Get(), queues[1].Get(), IID_PPV_ARGS(&underlying)));
    ComPtr<ID3D12Fence> held;
    CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
    UINT64 one = 1;
    ID3D12Fence *fences[] = {held.Get()};
    CHECK(on12->ReturnUnderlyingResource(texture.Get(), 1, &one, fences));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    ComPtr<ID3D11Fence> copied;
    CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&copied)));
    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!expect(done != nullptr, "CreateEvent: %lu", GetLastError()))
      return verdict();
    CHECK(copied->SetEventOnCompletion(1, done));
    context->CopyResource(staging.Get(), texture.Get());
    CHECK(context4->Signal(copied.Get(), 1));
    context->Flush();
    DWORD early = WaitForSingleObject(done, 6500);
    CHECK(held->Signal(1));
    expect(early == WAIT_TIMEOUT, "long producer wait allowed the consumer to complete: %lu", early);
    expect(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0, "consumer did not recover after a long CPU wait");
    CloseHandle(done);
    CHECK(device->GetDeviceRemovedReason());
    CHECK(read11(staging.Get(), 3));
  }

  if (!strcmp(part, "ordering")) {
    // Return defers its waits until work uses that resource. a query ending before that work must remain drainable.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-returnunderlyingresource
    const char sample_hlsl[] =
        "Texture2D<float4> source : register(t0);"
        "float4 ps(float4 p : SV_Position) : SV_Target { return source.Load(int3(p.xy, 0)); }";
    auto sample_code = compile(sample_hlsl, "ps", "ps");
    if (!sample_code)
      return verdict();
    ComPtr<ID3D11PixelShader> sample;
    CHECK(device->CreatePixelShader(sample_code->GetBufferPointer(), sample_code->GetBufferSize(), nullptr, &sample));
    D3D11_TEXTURE2D_DESC desc{UINT(texture_desc.Width), texture_desc.Height, 1, 1, texture_desc.Format,
                             {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> source, target;
    CHECK(device->CreateTexture2D(&desc, nullptr, &source));
    CHECK(device->CreateTexture2D(&desc, nullptr, &target));
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    CHECK(device->CreateShaderResourceView(source.Get(), nullptr, &srv));
    CHECK(draw(source.Get(), 3));
    CHECK(draw(target.Get(), 0));
    ComPtr<ID3D11On12Device2> interop2;
    CHECK(device.As(&interop2));
    auto setup = [&](ID3D11DeviceContext *record) {
      D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
      record->RSSetViewports(1, &viewport);
      record->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      record->VSSetShader(vs.Get(), nullptr, 0);
      record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    };
    setup(context.Get());
    context->PSSetShader(sample.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, srv.GetAddressOf());
    context->Draw(3, 0);
    CHECK(read11(target.Get(), 3));
    context->ClearState();
    for (bool deferred : {false, true}) {
      for (auto type : {D3D11_QUERY_EVENT, D3D11_QUERY_TIMESTAMP, D3D11_QUERY_OCCLUSION}) {
        step("ordering: %s prefix query %u", deferred ? "deferred" : "immediate", type);
        D3D11_QUERY_DESC query_desc{type};
        ComPtr<ID3D11Query> query;
        CHECK(device->CreateQuery(&query_desc, &query));
        ComPtr<ID3D11DeviceContext> record = context;
        ComPtr<ID3D11CommandList> commands;
        if (deferred)
          CHECK(device->CreateDeferredContext(0, &record));
        auto workload = [&] {
          setup(record.Get());
          record->PSSetShader(ps.Get(), nullptr, 0);
          if (type == D3D11_QUERY_OCCLUSION)
            record->Begin(query.Get());
          record->Draw(3, 0);
          record->End(query.Get());
          // the dependent draw deliberately shares the same render target and pass as the unrelated prefix.
          record->PSSetShader(sample.Get(), nullptr, 0);
          record->PSSetShaderResources(0, 1, srv.GetAddressOf());
          record->Draw(3, 0);
        };
        if (deferred) {
          workload();
          CHECK(record->FinishCommandList(FALSE, &commands));
        }
        ComPtr<ID3D12Resource> underlying;
        CHECK(interop2->UnwrapUnderlyingResource(source.Get(), queues[1].Get(), IID_PPV_ARGS(&underlying)));
        ComPtr<ID3D12Fence> held;
        CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
        UINT64 one = 1;
        ID3D12Fence *fences[] = {held.Get()};
        CHECK(interop2->ReturnUnderlyingResource(source.Get(), 1, &one, fences));
        // always release the producer, including when a broken implementation waits in the API call itself.
        HANDLE release = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!expect(release != nullptr, "CreateEvent: %lu", GetLastError()))
          return verdict();
        HRESULT released = E_FAIL;
        std::atomic<bool> opened = false;
        std::thread producer([&] {
          WaitForSingleObject(release, 1000);
          opened = true;
          released = held->Signal(1);
        });
        if (deferred)
          context->ExecuteCommandList(commands.Get(), FALSE);
        else
          workload();
        context->Flush();
        HRESULT ready = S_FALSE;
        const auto deadline = GetTickCount64() + 500;
        while (ready == S_FALSE && GetTickCount64() < deadline) {
          ready = context->GetData(query.Get(), nullptr, 0, 0);
          if (ready == S_FALSE)
            Sleep(1);
        }
        expect(ready == S_OK && !opened, "prefix query %u waited for the later returned resource: %#lx", type, ready);
        SetEvent(release);
        producer.join();
        CloseHandle(release);
        CHECK(released);
        CHECK(read11(target.Get(), 3));
        context->ClearState();
      }
    }
  }

  if (!strcmp(part, "wrapped")) {
    // "Call this method prior to calling Flush" inserts barriers to OutState;
    // "If no resource list is provided, all wrapped resources are
    // transitioned." (ReleaseWrappedResources).
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device-releasewrappedresources
    // Acquire marks resources acquired in hazard tracking; creation starts
    // acquired (programming guide).
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device-acquirewrappedresources
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12
    // CreateWrappedResource overrides inferred flags and defines
    // InState/OutState for each D3D12 allocation.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device-createwrappedresource
    for (UINT width : {aligned_width - 1, aligned_width, aligned_width + 1}) {
      texture_desc.Width = width;
      const UINT word_count = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT / sizeof(UINT) + width - aligned_width;
      ComPtr<ID3D12Resource> texture;
      CHECK(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture)));
      auto data =
          buffer(device12.Get(), D3D12_HEAP_TYPE_DEFAULT, word_count * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
      auto staging =
          buffer(device12.Get(), D3D12_HEAP_TYPE_READBACK, word_count * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(data && staging, "buffer creation failed"))
        return verdict();
      ComPtr<ID3D11Texture2D> wrapped_texture;
      ComPtr<ID3D11Buffer> wrapped_buffer;
      D3D11_RESOURCE_FLAGS texture_flags{D3D11_BIND_RENDER_TARGET}, buffer_flags{};
      CHECK(interop->CreateWrappedResource(texture.Get(), &texture_flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE, IID_PPV_ARGS(&wrapped_texture)));
      CHECK(interop->CreateWrappedResource(data.Get(), &buffer_flags, D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE, IID_PPV_ARGS(&wrapped_buffer)));
      ID3D11Resource *resources[] = {wrapped_texture.Get(), wrapped_buffer.Get()};
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)));
      CHECK(list->Close());
      for (UINT phase : {0u, 1u, 2u}) {
        step("wrapped: width %u words %u phase %u, %s release", width, word_count, phase,
             phase == 1 ? "all" : "listed");
        if (phase)
          interop->AcquireWrappedResources(resources, ARRAYSIZE(resources));
        CHECK(draw(wrapped_texture.Get(), phase));
        std::vector<UINT> wanted(word_count);
        for (UINT i = 0; i < word_count; i++)
          wanted[i] = (phase + 1) * (i + 1);
        D3D11_BUFFER_DESC desc{UINT(wanted.size() * sizeof(UINT)), D3D11_USAGE_DEFAULT};
        D3D11_SUBRESOURCE_DATA initial{wanted.data()};
        ComPtr<ID3D11Buffer> source;
        CHECK(device->CreateBuffer(&desc, &initial, &source));
        context->CopyResource(wrapped_buffer.Get(), source.Get());
        interop->ReleaseWrappedResources(phase == 1 ? nullptr : resources, phase == 1 ? 0 : ARRAYSIZE(resources));
        context->Flush();
        // "the D3D11 immediate context must be flushed" before further D3D12
        // submissions (Direct3D 11 on 12, Background, above). no CPU/GPU wait
        // separates Flush and these copies on queue 0.
        CHECK(read12(texture.Get(), queues[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, phase));
        CHECK(forget(staging.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        list->CopyBufferRegion(staging.Get(), 0, data.Get(), 0, wanted.size() * sizeof(UINT));
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(submit(device12.Get(), queues[0].Get(), list.Get()));
        void *mapped;
        CHECK(staging->Map(0, nullptr, &mapped));
        expect(!memcmp(mapped, wanted.data(), wanted.size() * sizeof(UINT)), "wrapped buffer contents differ");
        staging->Unmap(0, nullptr);
      }
    }
  }

  if (!strcmp(part, "wait") || !strcmp(part, "unwrap")) {
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    CHECK(device.As(&device5));
    CHECK(context.As(&context4));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device12->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = buffer(device12.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(upload != nullptr, "upload buffer creation failed"))
      return verdict();
    BYTE *mapped;
    CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
    for (UINT y = 0; y < texture_desc.Height; y++)
      for (UINT x = 0; x < texture_desc.Width; x++) {
        auto value = pixel(x, y, 1);
        memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(Pixel), value.data(),
               sizeof(Pixel));
      }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION source{
        upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    const UINT64 value = UINT64(std::numeric_limits<UINT>::max()) + 1;
    const DWORD observation_ms = 100, completion_ms = 10000;
    const bool unwrap = !strcmp(part, "unwrap");
    ComPtr<ID3D11On12Device2> interop2;
    if (unwrap)
      CHECK(device.As(&interop2));
    for (bool native11 : {false, true}) {
      if (!unwrap && native11)
        continue;
      for (UINT count : {0u, 1u, 2u}) {
        if (!unwrap && count != 1)
          continue;
        for (UINT pending = 0; pending < (count ? count : 1); pending++) {
          step("%s: %s texture, %u return fences, pending index %u", part, native11 ? "D3D11-created" : "wrapped",
               count, pending);
          ComPtr<ID3D12Resource> texture;
          ComPtr<ID3D11Texture2D> texture11;
          if (native11) {
            D3D11_TEXTURE2D_DESC desc{
                UINT(texture_desc.Width), texture_desc.Height,     1, 1, texture_desc.Format, {1, 0},
                D3D11_USAGE_DEFAULT,      D3D11_BIND_RENDER_TARGET};
            CHECK(device->CreateTexture2D(&desc, nullptr, &texture11));
          } else {
            CHECK(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                    IID_PPV_ARGS(&texture)));
            D3D11_RESOURCE_FLAGS flags{D3D11_BIND_RENDER_TARGET};
            CHECK(interop->CreateWrappedResource(texture.Get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE, IID_PPV_ARGS(&texture11)));
          }
          CHECK(draw(texture11.Get(), 0));
          if (unwrap) {
            // "The resource is transitioned to D3D12_RESOURCE_STATE_COMMON";
            // unwrap "doesn't flush".
            // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-unwrapunderlyingresource
            ComPtr<ID3D12Resource> underlying;
            CHECK(interop2->UnwrapUnderlyingResource(texture11.Get(), queues[1].Get(), IID_PPV_ARGS(&underlying)));
            context->Flush();
            if (texture) {
              ComPtr<IUnknown> original, returned;
              CHECK(texture.As(&original));
              CHECK(underlying.As(&returned));
              expect(original == returned, "unwrap returned a different allocation");
            }
            texture = underlying;
            CHECK(read12(texture.Get(), queues[1].Get(), D3D12_RESOURCE_STATE_COMMON, 0));
          } else {
            ID3D11Resource *resources[] = {texture11.Get()};
            interop->ReleaseWrappedResources(resources, ARRAYSIZE(resources));
            context->Flush();
            CHECK(read12(texture.Get(), queues[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, 0));
          }
          ComPtr<ID3D12CommandAllocator> allocator;
          ComPtr<ID3D12GraphicsCommandList> list;
          CHECK(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
          CHECK(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&list)));
          const auto before = unwrap ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_SOURCE;
          const auto after = unwrap ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RENDER_TARGET;
          transition(list.Get(), texture.Get(), before, D3D12_RESOURCE_STATE_COPY_DEST);
          D3D12_TEXTURE_COPY_LOCATION target{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
          transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, after);
          CHECK(list->Close());
          D3D11_TEXTURE2D_DESC staging_desc{
              UINT(texture_desc.Width), texture_desc.Height, 1, 1, texture_desc.Format, {1, 0}, D3D11_USAGE_STAGING, 0,
              D3D11_CPU_ACCESS_READ};
          ComPtr<ID3D11Texture2D> staging;
          CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
          ComPtr<ID3D12Fence> fences[2], gate;
          ComPtr<ID3D11Fence> completed11;
          for (auto &fence : fences)
            CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
          CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
          CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed11)));
          HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
          if (!expect(done != nullptr, "CreateEvent: %lu", GetLastError()))
            return verdict();
          HANDLE submitted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
          if (!expect(submitted != nullptr, "CreateEvent: %lu", GetLastError())) {
            CloseHandle(done);
            return verdict();
          }
          CHECK(completed11->SetEventOnCompletion(value, done));
          ID3D12Fence *fence_list[] = {fences[0].Get(), fences[1].Get()};
          UINT64 values[] = {value, value + 1};
          if (count) {
            CHECK(queues[1]->Wait(gate.Get(), 1));
            if (count == ARRAYSIZE(fence_list))
              CHECK(fences[1 - pending]->Signal(values[1 - pending]));
            ID3D12CommandList *lists[] = {list.Get()};
            queues[1]->ExecuteCommandLists(ARRAYSIZE(lists), lists);
            CHECK(queues[1]->Signal(fences[pending].Get(), values[pending]));
          } else
            CHECK(execute(device12.Get(), queues[1].Get(), list.Get()));
          DWORD early = WAIT_TIMEOUT, queued = WAIT_OBJECT_0;
          HRESULT released = S_OK;
          std::thread release;
          if (count)
            release = std::thread([&] {
              // the producer remains drainable even if the translation layer
              // waits on the calling thread.
              queued = WaitForSingleObject(submitted, completion_ms);
              if (queued == WAIT_OBJECT_0)
                early = WaitForSingleObject(done, observation_ms);
              released = gate->Signal(1);
            });
          if (unwrap) {
            // "include any pending work against the resource"; completion returns
            // it to COMMON.
            // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-returnunderlyingresource
            hr = interop2->ReturnUnderlyingResource(texture11.Get(), count, count ? values : nullptr,
                                                    count ? fence_list : nullptr);
          } else {
            // Acquire observes the direct queue's work, including its pending fence wait.
            // https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12
            hr = queues[0]->Wait(fences[0].Get(), values[0]);
            ID3D11Resource *resources[] = {texture11.Get()};
            interop->AcquireWrappedResources(resources, ARRAYSIZE(resources));
          }
          // a staging copy is queued before the producer is released; dropping
          // Wait/Return's pending fence exposes phase 0 rather than the phase 1
          // upload. the completion event also detects an early consumer.
          HRESULT copied = S_OK;
          if (SUCCEEDED(hr)) {
            context->CopyResource(staging.Get(), texture11.Get());
            copied = context4->Signal(completed11.Get(), value);
            context->Flush();
          }
          BOOL notified = SetEvent(submitted);
          if (release.joinable())
            release.join();
          CHECK(hr);
          CHECK(copied);
          CHECK(released);
          expect(notified && queued == WAIT_OBJECT_0, "consumer submission did not finish: %lu", queued);
          expect(!count || early == WAIT_TIMEOUT, "consumer completed before the producer was released: %lu", early);
          if (!expect(WaitForSingleObject(done, completion_ms) == WAIT_OBJECT_0, "consumer fence did not complete"))
            ExitProcess(1);
          CloseHandle(done);
          CloseHandle(submitted);
          D3D11_MAPPED_SUBRESOURCE result{};
          CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &result));
          pixels(static_cast<const BYTE *>(result.pData), result.RowPitch, staging_desc.Width, staging_desc.Height, 1);
          context->Unmap(staging.Get(), 0);
          // returning a resource must restore ordinary D3D11 use, including
          // rendering, not only a staging copy.
          CHECK(draw(texture11.Get(), 2));
          CHECK(read11(texture11.Get(), 2));
          if (unwrap) {
            ComPtr<ID3D12Resource> again;
            CHECK(interop2->UnwrapUnderlyingResource(texture11.Get(), queues[1].Get(), IID_PPV_ARGS(&again)));
            context->Flush();
            CHECK(read12(again.Get(), queues[1].Get(), D3D12_RESOURCE_STATE_COMMON, 2));
            CHECK(interop2->ReturnUnderlyingResource(texture11.Get(), 0, nullptr, nullptr));
          }
        }
      }
    }
    if (unwrap) {
      // "no keyed mutex resources, no GDI-compatible resources, and no buffers"
      // (UnwrapUnderlyingResource, above).
      auto data = buffer(device.Get(), sizeof(UINT), 0);
      if (!expect(data != nullptr, "D3D11 buffer creation failed"))
        return verdict();
      ComPtr<ID3D12Resource> out;
      hr = interop2->UnwrapUnderlyingResource(data.Get(), queues[1].Get(), IID_PPV_ARGS(&out));
      expect(FAILED(hr) && !out, "buffer unwrap: %#lx", hr);
    }
  }

  if (!strcmp(part, "lifetime")) {
    const DWORD completion_ms = 10000;
    ComPtr<ID3D11On12Device2> interop2;
    CHECK(device.As(&interop2));
    std::vector<Pixel> initial_pixels(texture_desc.Width * texture_desc.Height);
    for (UINT y = 0; y < texture_desc.Height; y++)
      for (UINT x = 0; x < texture_desc.Width; x++)
        initial_pixels[y * texture_desc.Width + x] = pixel(x, y, 3);
    // initial data is the resource's content before any operation: an unwrapped texture that nothing has used since
    // its creation has it (D3D11_SUBRESOURCE_DATA; UnwrapUnderlyingResource "include any pending work")
    step("lifetime: unwrap of a texture that has only its initial data");
    D3D11_TEXTURE2D_DESC desc11{
        UINT(texture_desc.Width), texture_desc.Height, 1, 1, texture_desc.Format, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET};
    D3D11_SUBRESOURCE_DATA initial{initial_pixels.data(), UINT(texture_desc.Width * sizeof(Pixel))};
    ComPtr<ID3D11Texture2D> texture11;
    CHECK(device->CreateTexture2D(&desc11, &initial, &texture11));
    ComPtr<ID3D12Resource> texture;
    CHECK(interop2->UnwrapUnderlyingResource(texture11.Get(), queues[1].Get(), IID_PPV_ARGS(&texture)));
    CHECK(read12(texture.Get(), queues[1].Get(), D3D12_RESOURCE_STATE_COMMON, 3));

    // the returned fences are waited for "prior to the 11 device using the resource": queue 0 is not held before
    // then, so a producer on queue 1 that waits for queue 0 still runs, and a fence the application releases is kept
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-returnunderlyingresource
    step("lifetime: a returned fence holds later Direct3D 11 work, not queue 0, and outlives the application's");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device12->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = buffer(device12.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(upload != nullptr, "upload buffer creation failed"))
      return verdict();
    BYTE *mapped;
    CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
    for (UINT y = 0; y < texture_desc.Height; y++)
      for (UINT x = 0; x < texture_desc.Width; x++)
        memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(Pixel), pixel(x, y, 4).data(),
               sizeof(Pixel));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&list)));
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                       {.PlacedFootprint = footprint}};
    D3D12_TEXTURE_COPY_LOCATION target{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    CHECK(list->Close());
    ComPtr<ID3D12Fence> gate, held, produced, released;
    CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
    CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&produced)));
    CHECK(queues[1]->Wait(gate.Get(), 1));
    CHECK(queues[1]->Wait(held.Get(), 1));
    ID3D12CommandList *lists[] = {list.Get()};
    queues[1]->ExecuteCommandLists(1, lists);
    CHECK(queues[1]->Signal(produced.Get(), 1));
    UINT64 one = 1;
    ID3D12Fence *fences[] = {produced.Get()};
    CHECK(interop2->ReturnUnderlyingResource(texture11.Get(), 1, &one, fences));
    CHECK(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&released)));
    HANDLE opened = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CHECK(queues[0]->Signal(gate.Get(), 1));
    CHECK(queues[0]->Signal(released.Get(), 1));
    CHECK(released->SetEventOnCompletion(1, opened));
    if (!expect(WaitForSingleObject(opened, completion_ms) == WAIT_OBJECT_0,
                "queue 0 was held by a returned fence before Direct3D 11 used the resource"))
      ExitProcess(1);
    CloseHandle(opened);
    // returning one allocation must leave unrelated Direct3D 11 work drainable
    ComPtr<ID3D11Device5> unrelated_device;
    ComPtr<ID3D11DeviceContext4> unrelated_context;
    ComPtr<ID3D11Fence> unrelated_done;
    CHECK(device.As(&unrelated_device));
    CHECK(context.As(&unrelated_context));
    CHECK(unrelated_device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&unrelated_done)));
    HANDLE unrelated_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CHECK(unrelated_done->SetEventOnCompletion(1, unrelated_event));
    auto unrelated = buffer(device.Get(), sizeof(UINT), 0);
    UINT unrelated_value = 1;
    context->UpdateSubresource(unrelated.Get(), 0, nullptr, &unrelated_value, 0, 0);
    CHECK(unrelated_context->Signal(unrelated_done.Get(), 1));
    if (!expect(WaitForSingleObject(unrelated_event, 1000) == WAIT_OBJECT_0,
                "a returned resource held unrelated Direct3D 11 work")) {
      held->Signal(1);
      CloseHandle(unrelated_event);
      return verdict();
    }
    CloseHandle(unrelated_event);
    // the producer is held on the CPU until the Direct3D 11 copy, which must wait for it, is shown not to have run
    produced.Reset();
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> copied;
    CHECK(device.As(&device5));
    CHECK(context.As(&context4));
    CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&copied)));
    HANDLE copy_done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CHECK(copied->SetEventOnCompletion(1, copy_done));
    desc11.Usage = D3D11_USAGE_STAGING;
    desc11.BindFlags = 0;
    desc11.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging11;
    CHECK(device->CreateTexture2D(&desc11, nullptr, &staging11));
    context->CopyResource(staging11.Get(), texture11.Get());
    CHECK(context4->Signal(copied.Get(), 1));
    context->Flush();
    const DWORD observation_ms = 100;
    expect(WaitForSingleObject(copy_done, observation_ms) == WAIT_TIMEOUT,
           "Direct3D 11 used the returned resource before its released fence was reached");
    CHECK(held->Signal(1));
    if (!expect(WaitForSingleObject(copy_done, completion_ms) == WAIT_OBJECT_0, "the Direct3D 11 copy did not finish"))
      ExitProcess(1);
    CloseHandle(copy_done);
    D3D11_MAPPED_SUBRESOURCE result{};
    CHECK(context->Map(staging11.Get(), 0, D3D11_MAP_READ, 0, &result));
    pixels(static_cast<const BYTE *>(result.pData), result.RowPitch, desc11.Width, desc11.Height, 4);
    context->Unmap(staging11.Get(), 0);

    // releasing the devices finishes the work they were given (Flush: "submits ... to the GPU"): what a flush sent is
    // seen by the queue after the Direct3D 11 device is gone. each round races the release with the submission
    constexpr UINT rounds = 16;
    for (UINT round = 0; round < rounds; round++) {
      step("lifetime: a flush right before the device's release, round %u", round);
      ComPtr<ID3D12Resource> target12;
      CHECK(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target12)));
      {
        ComPtr<ID3D11Device> device_once;
        ComPtr<ID3D11DeviceContext> context_once;
        CHECK(D3D11On12CreateDevice(device12.Get(), 0, &level, 1, queue_list, 1, 0, &device_once, &context_once,
                                    nullptr));
        ComPtr<ID3D11On12Device> interop_once;
        CHECK(device_once.As(&interop_once));
        ComPtr<ID3D11Texture2D> wrapped;
        D3D11_RESOURCE_FLAGS flags{D3D11_BIND_RENDER_TARGET};
        CHECK(interop_once->CreateWrappedResource(target12.Get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                  D3D12_RESOURCE_STATE_COPY_SOURCE, IID_PPV_ARGS(&wrapped)));
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device_once->CreateRenderTargetView(wrapped.Get(), nullptr, &rtv));
        const float color[4] = {0, 1, 0, 1};
        context_once->ClearRenderTargetView(rtv.Get(), color);
        ID3D11Resource *resources[] = {wrapped.Get()};
        interop_once->ReleaseWrappedResources(resources, 1);
        context_once->Flush();
      }
      auto desc = target12->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
      UINT64 size;
      device12->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &size);
      auto staging = buffer(device12.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(staging != nullptr, "readback buffer creation failed"))
        return verdict();
      CHECK(forget(staging.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      D3D12_TEXTURE_COPY_LOCATION from{target12.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION to{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                     {.PlacedFootprint = layout}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      CHECK(submit(device12.Get(), queues[0].Get(), list.Get()));
      BYTE *read;
      CHECK(staging->Map(0, nullptr, reinterpret_cast<void **>(&read)));
      const Pixel green{0, std::numeric_limits<BYTE>::max(), 0, std::numeric_limits<BYTE>::max()};
      for (UINT y = 0; y < desc.Height; y++)
        for (UINT x = 0; x < desc.Width; x++)
          if (!expect(!memcmp(read + layout.Offset + y * layout.Footprint.RowPitch + x * sizeof(Pixel), green.data(),
                              sizeof(Pixel)),
                      "pixel %u,%u lost the clear a released device was given", x, y))
            y = desc.Height, x = desc.Width;
      staging->Unmap(0, nullptr);
    }
  }

  if (!strcmp(part, "swapchain")) {
    // "create D3D11 wrapped resources for each of them." the guide's
    // frame sequence is Acquire, render, Release, Flush, Present; OutState is
    // PRESENT (Example Usage).
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12
    // "For Direct3D 12 this is a pointer to a direct command queue"
    // (CreateSwapChainForHwnd, pDevice).
    // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
    // two buffers are the flip-model minimum (Direct3D 12 Swap Chains, Swap
    // effects).
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains
    const UINT buffer_count = 2;
    RECT area{0, 0, LONG(texture_desc.Width), LONG(texture_desc.Height)};
    if (!expect(!!AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE), "AdjustWindowRect: %lu", GetLastError()))
      return verdict();
    HWND window = CreateWindowA("static", "d3d11_on12", WS_OVERLAPPEDWINDOW, 0, 0, area.right - area.left,
                                area.bottom - area.top, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
      return verdict();
    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc{UINT(texture_desc.Width),        texture_desc.Height, texture_desc.Format, FALSE, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, buffer_count};
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;
    ComPtr<IDXGISwapChain3> chain;
    CHECK(factory->CreateSwapChainForHwnd(queues[0].Get(), window, &desc, nullptr, nullptr, &chain1));
    CHECK(chain1.As(&chain));
    ComPtr<ID3D11On12Device2> interop2;
    CHECK(device.As(&interop2));
    std::vector<ComPtr<ID3D12Resource>> back_buffers(buffer_count);
    std::vector<ComPtr<ID3D11Texture2D>> wrapped(buffer_count);
    D3D11_RESOURCE_FLAGS flags{D3D11_BIND_RENDER_TARGET};
    for (UINT i = 0; i < buffer_count; i++) {
      CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&back_buffers[i])));
      CHECK(interop->CreateWrappedResource(back_buffers[i].Get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped[i])));
    }
    for (UINT frame = 0; frame < 2 * buffer_count; frame++) {
      const UINT index = chain->GetCurrentBackBufferIndex();
      step("swapchain: frame %u back buffer %u", frame, index);
      if (!expect(index < buffer_count, "invalid current back buffer index"))
        return verdict();
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)));
      transition(list.Get(), back_buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device12.Get(), queues[0].Get(), list.Get()));
      ID3D11Resource *resources[] = {wrapped[index].Get()};
      interop->AcquireWrappedResources(resources, ARRAYSIZE(resources));
      CHECK(draw(wrapped[index].Get(), frame));
      interop->ReleaseWrappedResources(resources, ARRAYSIZE(resources));
      context->Flush();
      CHECK(read12(back_buffers[index].Get(), queues[0].Get(), D3D12_RESOURCE_STATE_PRESENT, frame));
      if (frame % 2) {
        // an unwrapped swap-chain buffer must be returned before Present (UnwrapUnderlyingResource, Remarks).
        // https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-id3d11on12device2-unwrapunderlyingresource
        ComPtr<ID3D12Resource> underlying;
        CHECK(interop2->UnwrapUnderlyingResource(wrapped[index].Get(), queues[1].Get(), IID_PPV_ARGS(&underlying)));
        context->Flush();
        CHECK(read12(underlying.Get(), queues[1].Get(), D3D12_RESOURCE_STATE_COMMON, frame));
        CHECK(interop2->ReturnUnderlyingResource(wrapped[index].Get(), 0, nullptr, nullptr));
      }
      CHECK(chain->Present(0, 0));
    }
    context->ClearState();
    wrapped.clear();
    context->Flush();
    back_buffers.clear();
    chain.Reset();
    chain1.Reset();
    DestroyWindow(window);
  }
  context->ClearState();
  context->Flush();
  return verdict();
}
