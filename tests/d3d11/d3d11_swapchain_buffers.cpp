// contract: sequential swap chains expose each buffer, with read-only buffers after 0, and Present rotates their
// contents through retained textures and views: "Present rotates the identities of the back buffers" (D3D11.3
// 6.3.9.2). "The swap chain's buffers with indexes greater than zero can only be read from" (IDXGISwapChain::GetBuffer,
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getbuffer).
// DXGI_USAGE defines BACK_BUFFER and READ_ONLY; Wine's test_swapchain_parameters witnesses DISCARD_ON_PRESENT and
// the inaccessible buffers of both discard effects. DXGI_SWAP_CHAIN_DESC1 and ResizeBuffers bound counts by
// DXGI_MAX_SWAP_CHAIN_BUFFERS, with at least two for flip effects. DXGI_PRESENT_TEST and DO_NOT_SEQUENCE leave the
// sequence alone (https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).
// each frame clears buffer 0 to a distinct byte value. copies and reused command lists both read retained objects,
// including commands queued before Present. untouched buffers have undefined contents and are not compared.
// counts come from the API limit; every failure names the effect, count, resize, frame, and buffer.
// RTV resources "must have been created with the D3D11_BIND_RENDER_TARGET flag" (CreateRenderTargetView).
// ResizeBuffers: "You must release all of its direct and indirect references on the back buffers in order for
// ResizeBuffers to succeed." Present: "a successful presentation unbinds back buffer 0", except DO_NOT_SEQUENCE.
// sources: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createrendertargetview
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
// presented pixels at the count boundaries use the existing GDI path. SRVs are created in different orders, so rotating
// allocations must preserve each buffer's view mapping; SRGB decoding and UNORM rounding use D3D11.3 3.2.3's
// named conversion and tolerance constants, rather than requiring an ideal result where the contract permits error.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <limits>

int
main() {
  const char hlsl[] = R"(
    float4 vs(uint id : SV_VertexID) : SV_Position {
      float2 vertices[3] = {float2(-1, -1), float2(-1, 3), float2(3, -1)};
      return float4(vertices[id], 0, 1);
    }
    Texture2D<float4> tex;
    float4 ps(float4 location : SV_Position) : SV_Target { return tex.Load(int3(location.xy, 0)); }
  )";
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!vs_code || !ps_code)
    return 1;
  SetEnvironmentVariableA("DXMT_CONFIG", "dxgi.presentThroughGDI=True");
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory> factory;
  CHECK(device.As(&dxgi));
  CHECK(dxgi->GetAdapter(&adapter));
  CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateDeferredContext(0, &deferred));
  HWND window = CreateWindowA("static", "swapchain_buffers", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64,
                              nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  // the control paints itself now, not over a presented frame when the queue is first pumped
  UpdateWindow(window);
  DXGI_SWAP_CHAIN_DESC desc{{3, 5, {}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0},
                           DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT, 1, window, TRUE};
  D3D11_TEXTURE2D_DESC read_desc{desc.BufferDesc.Width, desc.BufferDesc.Height, 1, 1, desc.BufferDesc.Format,
                               {1, 0}, D3D11_USAGE_STAGING, 0,
                               D3D11_CPU_ACCESS_READ};
  ComPtr<ID3D11Texture2D> readback, before, target;
  CHECK(device->CreateTexture2D(&read_desc, nullptr, &readback));
  CHECK(device->CreateTexture2D(&read_desc, nullptr, &before));
  read_desc.Usage = D3D11_USAGE_DEFAULT;
  read_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  read_desc.CPUAccessFlags = 0;
  CHECK(device->CreateTexture2D(&read_desc, nullptr, &target));
  ComPtr<ID3D11RenderTargetView> target_view;
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &target_view));
  ComPtr<IDXGIResource> ordinary;
  CHECK(target.As(&ordinary));
  DXGI_USAGE ordinary_usage = 0;
  CHECK(ordinary->GetUsage(&ordinary_usage));
  expect(ordinary_usage == DXGI_USAGE_RENDER_TARGET_OUTPUT, "ordinary texture has swap-chain usage %#x",
         ordinary_usage);
  auto bytes = [&](ID3D11Texture2D *texture, BYTE value, bool srgb = false) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(texture, 0, D3D11_MAP_READ, 0, &mapped);
    if (!expect(hr == S_OK, "Map: %#lx", hr))
      return;
    auto decode = [&](float c) {
      c /= std::numeric_limits<BYTE>::max();
      if (!srgb)
        return c;
      return c <= D3D11_SRGB_TO_FLOAT_THRESHOLD
                 ? c / D3D11_SRGB_TO_FLOAT_DENOMINATOR_1
                 : std::pow((c + D3D11_SRGB_TO_FLOAT_OFFSET) / D3D11_SRGB_TO_FLOAT_DENOMINATOR_2,
                            D3D11_SRGB_TO_FLOAT_EXPONENT);
    };
    // D3D11.3 3.2.3.6 defines D3D11_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP as 0.6f
    constexpr float unorm_tolerance = 0.6f;
    float minimum = srgb ? decode(value - D3D11_SRGB_TO_FLOAT_TOLERANCE_IN_ULP) * std::numeric_limits<BYTE>::max() -
                               unorm_tolerance : value;
    float maximum = srgb ? decode(value + D3D11_SRGB_TO_FLOAT_TOLERANCE_IN_ULP) * std::numeric_limits<BYTE>::max() +
                               unorm_tolerance : value;
    const BYTE wanted[] = {BYTE(std::round(decode(value) * std::numeric_limits<BYTE>::max())), 0, 0,
                           std::numeric_limits<BYTE>::max()};
    for (UINT y = 0; y < read_desc.Height; y++) {
      for (UINT x = 0; x < read_desc.Width; x++) {
        const BYTE *got = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(wanted);
        expect(got[0] >= minimum && got[0] <= maximum && !memcmp(got + 1, wanted + 1, sizeof(wanted) - 1),
               "pixel %u,%u: %u,%u,%u,%u, expected %u,0,0,%u", x, y, got[0], got[1], got[2], got[3], wanted[0],
               std::numeric_limits<BYTE>::max());
      }
    }
    context->Unmap(texture, 0);
  };
  auto shows = [&](BYTE value) {
    RECT area{};
    GetClientRect(window, &area);
    COLORREF got = CLR_INVALID;
    for (UINT attempt = 0; attempt < 250; attempt++) {
      MSG message;
      while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
        DispatchMessageA(&message);
      Sleep(20);
      HDC dc = GetDC(window);
      got = GetPixel(dc, area.right / 2, area.bottom / 2);
      ReleaseDC(window, dc);
      if (got == RGB(value, 0, 0))
        break;
    }
    expect(got == RGB(value, 0, 0), "window pixel %#lx, expected %#lx", got, RGB(value, 0, 0));
  };

  for (UINT effect = DXGI_SWAP_EFFECT_DISCARD; effect <= DXGI_SWAP_EFFECT_FLIP_DISCARD; effect++) {
    desc.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(effect);
    const bool sequential = effect == DXGI_SWAP_EFFECT_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    const bool flip = effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const bool defined = sequential || effect == DXGI_SWAP_EFFECT_DISCARD || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    for (UINT count = 0; count <= DXGI_MAX_SWAP_CHAIN_BUFFERS + 1; count++) {
      step("effect %u count %u: create", effect, count);
      desc.BufferCount = count;
      ComPtr<IDXGISwapChain> chain;
      HRESULT hr = factory->CreateSwapChain(device.Get(), &desc, &chain);
      bool valid = defined && count && count <= DXGI_MAX_SWAP_CHAIN_BUFFERS && (!flip || count >= 2);
      if (!expect(hr == (valid ? S_OK : DXGI_ERROR_INVALID_CALL), "CreateSwapChain: %#lx", hr) || !valid)
        continue;
      for (UINT resize = 0; resize < 2; resize++) {
        if (flip) {
          step("effect %u count %u resize %u: refuse one buffer", effect, count, resize);
          expect(chain->ResizeBuffers(1, 1, 1, DXGI_FORMAT_UNKNOWN, 0) == DXGI_ERROR_INVALID_CALL,
                 "flip ResizeBuffers(1) succeeded");
        }
        const UINT accessible = sequential ? count : 1;
        std::vector<ComPtr<ID3D11Texture2D>> textures(accessible);
        std::vector<ComPtr<ID3D11ShaderResourceView>> views(accessible);
        std::vector<ComPtr<ID3D11CommandList>> lists(accessible);
        ComPtr<ID3D11RenderTargetView> writable;
        bool complete = true;
        for (UINT i = 0; i <= count; i++) {
          step("effect %u count %u resize %u buffer %u: access", effect, count, resize, i);
          ComPtr<ID3D11Texture2D> texture;
          hr = chain->GetBuffer(i, IID_PPV_ARGS(&texture));
          bool available = i < accessible;
          if (!expect(hr == (available ? S_OK : DXGI_ERROR_INVALID_CALL), "GetBuffer: %#lx", hr))
            complete = false;
          if (!available || !texture)
            continue;
          textures[i] = texture;
          ComPtr<ID3D11Texture2D> again;
          CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&again)));
          expect(again == texture, "GetBuffer changes identity");
          for (UINT j = 0; j < i; j++)
            expect(texture != textures[j], "buffers %u and %u alias", i, j);
          ComPtr<IDXGIResource> resource;
          CHECK(texture.As(&resource));
          DXGI_USAGE usage = 0;
          CHECK(resource->GetUsage(&usage));
          const DXGI_USAGE wanted = desc.BufferUsage | DXGI_USAGE_BACK_BUFFER |
                                    (i ? DXGI_USAGE_READ_ONLY : 0) |
                                    (sequential ? 0 : DXGI_USAGE_DISCARD_ON_PRESENT);
          expect(usage == wanted, "usage %#x, expected %#x", usage, wanted);
          D3D11_TEXTURE2D_DESC texture_desc{};
          texture->GetDesc(&texture_desc);
          expect(texture_desc.Width == desc.BufferDesc.Width && texture_desc.Height == desc.BufferDesc.Height &&
                     texture_desc.BindFlags == (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE),
                 "texture description differs from its swap chain");
          D3D11_SHADER_RESOURCE_VIEW_DESC array_desc{};
          array_desc.Format = desc.BufferDesc.Format;
          array_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
          array_desc.Texture2DArray = {0, 1, 0, 1};
          D3D11_SHADER_RESOURCE_VIEW_DESC srgb_desc{};
          srgb_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
          srgb_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
          srgb_desc.Texture2D = {0, 1};
          ComPtr<ID3D11ShaderResourceView> array_view;
          if (i % 2)
            CHECK(device->CreateShaderResourceView(texture.Get(), &array_desc, &array_view));
          CHECK(device->CreateShaderResourceView(texture.Get(), &srgb_desc, &views[i]));
          if (!(i % 2))
            CHECK(device->CreateShaderResourceView(texture.Get(), &array_desc, &array_view));
          deferred->VSSetShader(vs.Get(), nullptr, 0);
          deferred->PSSetShader(ps.Get(), nullptr, 0);
          deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          D3D11_VIEWPORT viewport{0, 0, float(desc.BufferDesc.Width), float(desc.BufferDesc.Height), 0, 1};
          deferred->RSSetViewports(1, &viewport);
          deferred->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
          deferred->PSSetShaderResources(0, 1, views[i].GetAddressOf());
          deferred->Draw(3, 0);
          CHECK(deferred->FinishCommandList(FALSE, &lists[i]));
          ComPtr<ID3D11RenderTargetView> view;
          hr = device->CreateRenderTargetView(texture.Get(), nullptr, &view);
          expect(hr == (i ? E_INVALIDARG : S_OK), "CreateRenderTargetView: %#lx", hr);
          expect(device->CreateRenderTargetView(texture.Get(), nullptr, nullptr) == (i ? E_INVALIDARG : S_FALSE),
                 "CreateRenderTargetView validation differs");
          if (!i)
            writable = view;
        }
        if (!complete || !writable)
          break;
        std::vector<BYTE> contents(accessible, 0);
        for (UINT frame = 0; frame <= count; frame++) {
          BYTE value = static_cast<BYTE>((frame + 1) * (std::numeric_limits<BYTE>::max() /
                                                       (DXGI_MAX_SWAP_CHAIN_BUFFERS + 2)));
          step("effect %u count %u resize %u frame %u: clear", effect, count, resize, frame);
          const float colour[4] = {float(value) / std::numeric_limits<BYTE>::max(), 0, 0, 1};
          context->ClearRenderTargetView(writable.Get(), colour);
          contents[0] = value;
          context->ExecuteCommandList(lists[0].Get(), TRUE);
          context->CopyResource(before.Get(), target.Get());
          const UINT present_flags[] = {DXGI_PRESENT_TEST, DXGI_PRESENT_DO_NOT_SEQUENCE,
                                        DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE, 0};
          for (UINT flags : present_flags) {
            if (flags && frame != count)
              continue;
            step("effect %u count %u resize %u frame %u flags %#x: present", effect, count, resize, frame, flags);
            ID3D11RenderTargetView *bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            bound[0] = writable.Get();
            bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - 1] = target_view.Get();
            context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, nullptr);
            expect(chain->Present(5, flags) == DXGI_ERROR_INVALID_CALL, "invalid Present succeeded");
            CHECK(chain->Present(0, flags));
            ComPtr<ID3D11RenderTargetView> got[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            ID3D11RenderTargetView *raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw, nullptr);
            for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; slot++) {
              got[slot].Attach(raw[slot]);
              expect(got[slot].Get() == (!slot && flip && !flags ? nullptr : bound[slot]),
                     "Present RTV slot %u differs", slot);
            }
            context->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
            if (!(flags & DXGI_PRESENT_TEST) && frame == count &&
                (count == (flip ? 2u : 1u) || count == DXGI_MAX_SWAP_CHAIN_BUFFERS))
              shows(value);
            if (sequential && !flags)
              std::rotate(contents.begin(), contents.begin() + 1, contents.end());
            if (sequential || flags) {
              for (UINT i = 0; i < accessible; i++) {
                step("effect %u count %u resize %u frame %u flags %#x buffer %u: contents",
                     effect, count, resize, frame, flags, i);
                if (!contents[i])
                  continue;
                context->CopyResource(readback.Get(), textures[i].Get());
                bytes(readback.Get(), contents[i]);
                context->ExecuteCommandList(lists[i].Get(), TRUE);
                context->CopyResource(readback.Get(), target.Get());
                bytes(readback.Get(), contents[i], true);
              }
            }
          }
          bytes(before.Get(), value, true);
        }
        context->ClearState();
        textures.clear();
        views.clear();
        lists.clear();
        writable.Reset();
        step("effect %u count %u resize %u: resize", effect, count, resize);
        CHECK(chain->ResizeBuffers(0, desc.BufferDesc.Width, desc.BufferDesc.Height, DXGI_FORMAT_UNKNOWN, 0));
        DXGI_SWAP_CHAIN_DESC resized{};
        CHECK(chain->GetDesc(&resized));
        expect(resized.BufferCount == count, "ResizeBuffers(0) changed the count");
      }
    }
  }
  desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
  desc.BufferCount = 3;
  const UINT bindings = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_UNORDERED_ACCESS;
  for (UINT usage = 0; usage <= bindings; usage++) {
    if (usage & ~bindings)
      continue;
    step("usage %#x: create", usage);
    desc.BufferUsage = usage;
    ComPtr<IDXGISwapChain> chain;
    CHECK(factory->CreateSwapChain(device.Get(), &desc, &chain));
    for (UINT i = 0; i < desc.BufferCount; i++) {
      step("usage %#x buffer %u: usage and bindings", usage, i);
      ComPtr<ID3D11Texture2D> texture;
      CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&texture)));
      D3D11_TEXTURE2D_DESC texture_desc{};
      texture->GetDesc(&texture_desc);
      UINT bind_flags = (usage & DXGI_USAGE_RENDER_TARGET_OUTPUT ? D3D11_BIND_RENDER_TARGET : 0) |
                        (usage & DXGI_USAGE_SHADER_INPUT ? D3D11_BIND_SHADER_RESOURCE : 0) |
                        (usage & DXGI_USAGE_UNORDERED_ACCESS ? D3D11_BIND_UNORDERED_ACCESS : 0);
      expect(texture_desc.BindFlags == bind_flags, "bind flags %#x, expected %#x", texture_desc.BindFlags, bind_flags);
      ComPtr<IDXGIResource> resource;
      CHECK(texture.As(&resource));
      DXGI_USAGE got = 0;
      CHECK(resource->GetUsage(&got));
      DXGI_USAGE wanted = usage | DXGI_USAGE_BACK_BUFFER | (i ? DXGI_USAGE_READ_ONLY : 0);
      expect(got == wanted, "usage %#x, expected %#x", got, wanted);
      ComPtr<ID3D11RenderTargetView> rtv;
      HRESULT rtv_result = !i && (bind_flags & D3D11_BIND_RENDER_TARGET) ? S_OK : E_INVALIDARG;
      expect(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv) == rtv_result,
             "CreateRenderTargetView binding validation differs");
      expect(device->CreateRenderTargetView(texture.Get(), nullptr, nullptr) ==
                 (rtv_result == S_OK ? S_FALSE : E_INVALIDARG), "RTV null-output validation differs");
      {
        ComPtr<ID3D11UnorderedAccessView> view;
        bool writable = !i && (bind_flags & D3D11_BIND_UNORDERED_ACCESS);
        expect(device->CreateUnorderedAccessView(texture.Get(), nullptr, &view) == (writable ? S_OK : E_INVALIDARG),
               "CreateUnorderedAccessView binding validation differs");
        expect(device->CreateUnorderedAccessView(texture.Get(), nullptr, nullptr) == (writable ? S_FALSE : E_INVALIDARG),
               "CreateUnorderedAccessView validation differs");
      }
    }
    CHECK(chain->ResizeBuffers(DXGI_MAX_SWAP_CHAIN_BUFFERS, 1, 1, DXGI_FORMAT_UNKNOWN, 0));
    {
      ComPtr<ID3D11Texture2D> last;
      CHECK(chain->GetBuffer(DXGI_MAX_SWAP_CHAIN_BUFFERS - 1, IID_PPV_ARGS(&last)));
    }
    CHECK(chain->ResizeBuffers(1, 1, 1, DXGI_FORMAT_UNKNOWN, 0));
    ComPtr<ID3D11Texture2D> unavailable;
    expect(chain->GetBuffer(1, IID_PPV_ARGS(&unavailable)) == DXGI_ERROR_INVALID_CALL,
           "ResizeBuffers shrink left a buffer accessible");
  }
  desc.BufferCount = 2;
  for (UINT effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; effect <= DXGI_SWAP_EFFECT_FLIP_DISCARD; effect++) {
    desc.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(effect);
    for (UINT sample = 1; sample <= 2; sample++) {
      for (UINT quality = 0; quality <= 1; quality++) {
        step("effect %u samples %u quality %u: create", effect, sample, quality);
        desc.SampleDesc = {sample, quality};
        ComPtr<IDXGISwapChain> chain;
        expect(factory->CreateSwapChain(device.Get(), &desc, &chain) ==
                   (sample == 1 && !quality ? S_OK : DXGI_ERROR_INVALID_CALL),
               "flip sample validation differs");
      }
    }
  }
  desc.SampleDesc = {1, 0};
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_UNORDERED_ACCESS;
  desc.BufferCount = 3;
  for (UINT effect = DXGI_SWAP_EFFECT_DISCARD; effect <= DXGI_SWAP_EFFECT_FLIP_DISCARD; effect++) {
    if (effect != DXGI_SWAP_EFFECT_DISCARD && effect != DXGI_SWAP_EFFECT_SEQUENTIAL &&
        effect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && effect != DXGI_SWAP_EFFECT_FLIP_DISCARD)
      continue;
    desc.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(effect);
    ComPtr<IDXGISwapChain> chain;
    CHECK(factory->CreateSwapChain(device.Get(), &desc, &chain));
    UINT accessible = effect == DXGI_SWAP_EFFECT_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                          ? desc.BufferCount : 1;
    for (UINT i = 0; i < accessible; i++) {
      auto refuses = [&](const char *reference) {
        step("effect %u buffer %u: resize with %s", effect, i, reference);
        expect(chain->ResizeBuffers(0, desc.BufferDesc.Width + 1, desc.BufferDesc.Height + 1,
                                    DXGI_FORMAT_UNKNOWN, 0) == DXGI_ERROR_INVALID_CALL,
               "ResizeBuffers accepts a retained %s", reference);
        DXGI_SWAP_CHAIN_DESC got{};
        expect(SUCCEEDED(chain->GetDesc(&got)) && got.BufferDesc.Width == desc.BufferDesc.Width &&
                   got.BufferDesc.Height == desc.BufferDesc.Height,
               "refused resize changes the descriptor");
      };
      ComPtr<ID3D11Texture2D> texture;
      CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&texture)));
      refuses("texture");
      ComPtr<ID3D11Texture2D> same;
      CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&same)));
      expect(same == texture, "refused resize replaces the texture");
      same.Reset();
      ComPtr<ID3D11ShaderResourceView> view;
      CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
      texture.Reset();
      refuses("SRV");
      context->PSSetShaderResources(0, 1, view.GetAddressOf());
      deferred->PSSetShaderResources(0, 1, view.GetAddressOf());
      view.Reset();
      refuses("context binding");
      context->ClearState();
      ComPtr<ID3D11CommandList> list, nested;
      CHECK(deferred->FinishCommandList(FALSE, &list));
      refuses("command list");
      deferred->ExecuteCommandList(list.Get(), FALSE);
      CHECK(deferred->FinishCommandList(FALSE, &nested));
      list.Reset();
      refuses("nested command list");
      nested.Reset();
      step("effect %u buffer %u: resize after releasing references", effect, i);
      CHECK(chain->ResizeBuffers(0, desc.BufferDesc.Width, desc.BufferDesc.Height, DXGI_FORMAT_UNKNOWN, 0));
    }
    ComPtr<ID3D11Texture2D> texture, unrelated;
    CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&texture)));
    D3D11_TEXTURE2D_DESC uav_desc{};
    texture->GetDesc(&uav_desc);
    CHECK(device->CreateTexture2D(&uav_desc, nullptr, &unrelated));
    ComPtr<ID3D11UnorderedAccessView> uav, other;
    CHECK(device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav));
    CHECK(device->CreateUnorderedAccessView(unrelated.Get(), nullptr, &other));
    const UINT flags[] = {DXGI_PRESENT_TEST, DXGI_PRESENT_DO_NOT_SEQUENCE, 0};
    for (bool compute : {false, true}) {
      for (UINT flag : flags) {
        step("effect %u compute %u flags %#x: UAV output bindings", effect, compute, flag);
        ID3D11UnorderedAccessView *bound[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        bound[0] = uav.Get();
        bound[D3D11_PS_CS_UAV_REGISTER_COUNT - 1] = other.Get();
        if (compute)
          context->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, bound, nullptr);
        else
          context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
                                                           D3D11_PS_CS_UAV_REGISTER_COUNT, bound, nullptr);
        CHECK(chain->Present(0, flag));
        ID3D11UnorderedAccessView *got[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        if (compute)
          context->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, got);
        else
          context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
                                                           D3D11_PS_CS_UAV_REGISTER_COUNT, got);
        bool unbind = effect >= DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && !flag;
        for (UINT slot = 0; slot < D3D11_PS_CS_UAV_REGISTER_COUNT; slot++) {
          expect(got[slot] == (!slot && unbind ? nullptr : bound[slot]), "Present UAV slot %u differs", slot);
          if (got[slot])
            got[slot]->Release();
        }
        context->ClearState();
      }
    }
  }
  desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
  desc.Windowed = FALSE;
  {
    step("fullscreen: create and leave");
    ComPtr<IDXGISwapChain> chain;
    HRESULT hr = factory->CreateSwapChain(device.Get(), &desc, &chain);
    if (expect(hr == S_OK, "fullscreen CreateSwapChain: %#lx", hr)) {
      BOOL fullscreen = FALSE;
      CHECK(chain->GetFullscreenState(&fullscreen, nullptr));
      expect(fullscreen, "fullscreen creation leaves the chain windowed");
      CHECK(chain->SetFullscreenState(FALSE, nullptr));
    }
  }
  {
    step("fullscreen: refuse a destroyed window");
    HWND gone = CreateWindowA("static", "closed", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                              nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    expect(gone != nullptr, "CreateWindow: %lu", GetLastError());
    DestroyWindow(gone);
    desc.OutputWindow = gone;
    ComPtr<IDXGISwapChain> chain;
    HRESULT hr = factory->CreateSwapChain(device.Get(), &desc, &chain);
    expect(FAILED(hr) && !chain, "fullscreen creation with a destroyed window: %#lx", hr);
  }
  desc.OutputWindow = window;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
  HMODULE library = LoadLibraryA("d3d10_1.dll");
  auto create10 = library ? reinterpret_cast<decltype(&D3D10CreateDevice1)>(
                               GetProcAddress(library, "D3D10CreateDevice1")) : nullptr;
  if (!expect(create10 != nullptr, "D3D10CreateDevice1 unavailable"))
    return verdict();
  ComPtr<ID3D10Device1> device10;
  CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION,
                 &device10));
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
  desc.BufferCount = 3;
  {
    ComPtr<IDXGISwapChain> chain;
    CHECK(factory->CreateSwapChain(device10.Get(), &desc, &chain));
    std::vector<ComPtr<ID3D10Texture2D>> textures(desc.BufferCount);
    ComPtr<ID3D10RenderTargetView> writable;
    for (UINT i = 0; i < desc.BufferCount; i++) {
      step("D3D10 buffer %u: access", i);
      CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&textures[i])));
      ComPtr<ID3D10RenderTargetView> view;
      expect(device10->CreateRenderTargetView(textures[i].Get(), nullptr, &view) == (i ? E_INVALIDARG : S_OK),
             "D3D10 write access differs");
      if (!i)
        writable = view;
    }
    D3D10_TEXTURE2D_DESC staging{desc.BufferDesc.Width, desc.BufferDesc.Height, 1, 1, desc.BufferDesc.Format,
                               {1, 0}, D3D10_USAGE_STAGING, 0,
                               D3D10_CPU_ACCESS_READ};
    ComPtr<ID3D10Texture2D> read;
    CHECK(device10->CreateTexture2D(&staging, nullptr, &read));
    std::vector<BYTE> contents(desc.BufferCount, 0);
    for (UINT frame = 0; frame <= desc.BufferCount; frame++) {
      BYTE value = static_cast<BYTE>(frame + 1);
      const float colour[4] = {float(value) / std::numeric_limits<BYTE>::max(), 0, 0, 1};
      device10->ClearRenderTargetView(writable.Get(), colour);
      contents[0] = value;
      CHECK(chain->Present(0, 0));
      std::rotate(contents.begin(), contents.begin() + 1, contents.end());
      for (UINT i = 0; i < desc.BufferCount; i++) {
        if (!contents[i])
          continue;
        step("D3D10 frame %u buffer %u: contents", frame, i);
        device10->CopyResource(read.Get(), textures[i].Get());
        D3D10_MAPPED_TEXTURE2D mapped{};
        CHECK(read->Map(0, D3D10_MAP_READ, 0, &mapped));
        const BYTE wanted[] = {contents[i], 0, 0, std::numeric_limits<BYTE>::max()};
        for (UINT y = 0; y < staging.Height; y++) {
          for (UINT x = 0; x < staging.Width; x++) {
            const BYTE *got = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch + x * sizeof(wanted);
            expect(!memcmp(got, wanted, sizeof(wanted)), "D3D10 pixel %u,%u: %u,%u,%u,%u, expected %u,0,0,%u",
                   x, y, got[0], got[1], got[2], got[3], contents[i], std::numeric_limits<BYTE>::max());
          }
        }
        read->Unmap(0);
      }
    }
    device10->ClearState();
  }
  desc.BufferUsage = DXGI_USAGE_SHADER_INPUT;
  {
    step("D3D10: refuse an RTV without render-target binding");
    ComPtr<IDXGISwapChain> chain;
    CHECK(factory->CreateSwapChain(device10.Get(), &desc, &chain));
    ComPtr<ID3D10Texture2D> texture;
    CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&texture)));
    ComPtr<ID3D10RenderTargetView> view;
    expect(device10->CreateRenderTargetView(texture.Get(), nullptr, &view) == E_INVALIDARG,
           "D3D10 accepts an RTV without render-target binding");
    expect(device10->CreateRenderTargetView(texture.Get(), nullptr, nullptr) == E_INVALIDARG,
           "D3D10 null-output validation accepts missing binding");
  }
  device10.Reset();
  FreeLibrary(library);
  context->ClearState();
  DestroyWindow(window);
  return verdict();
}
