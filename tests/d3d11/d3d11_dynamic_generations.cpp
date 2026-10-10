// contract: DISCARD preserves data for earlier draws; NO_OVERWRITE appends to the current version without changing
// referenced data, for dynamic VBs, CBs and buffer SRVs on immediate and deferred contexts. Microsoft Learn,
// "How to: Use dynamic resources", "Using dynamic buffers": "This allows the GPU to continue using the old data
// while the app places data in the new buffer."
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/how-to--use-dynamic-resources
// D3D11.3 5.6.1.1: "Use of this flag indicates the application will not modify any data referred to by a previous
// Draw or Resource update, etc."; 5.3.4.3.1: "Map() allows NO_OVERWRITE for Constant Buffers."; 5.6.1.2: "Map() allows
// NO_OVERWRITE for Buffers with DYNAMIC usage and the SHADER_RESOURCE (shader input) bind flag."
// "Before the first call with NO_OVERWRITE on a deferred context, a DISCARD must be done on the same context"
// (5.3.4.3.1, 5.6.1.2). CB/SRV NO_OVERWRITE support is queried as required by Microsoft Learn,
// ID3D11DeviceContext::Map:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
// a recorded map belongs to its list: "After generation, a Command List can be used multiple times; but cannot be
// altered by the application explicitly" (6.3.1); execution "inherits and can change the global state of objects,
// such as texture data, constant buffer data, and query data" (6.3.5).
// each case changes only one buffer. the shader copies its VB/CB/SRV values into separate integer channels; expected
// pixels come from the supplied data. bindings stay set through all maps. row 0 is drawn before and after an append,
// then the middle and last rows; row 1 remains unused for an immediate append after list execution. sizes double
// across the allocator's minimum, then straddle DXMT_PAGE_SIZE (meson.build). page / buffer size + 2 discards exceed
// its suballocation count even without the allocation minimum (dxmt_buffer.cpp); no reads or flushes separate them.
// a deferred list is recorded before an immediate overwrite, then replayed after readback and immediate reuse.
// discarded contents, write-only mapped contents, pointer identity and the time work takes are never asserted.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>

using Row = std::array<UINT, 4>;

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint4 rows[CONSTANTS]; };
Buffer<uint4> source : register(t0);
struct V { float4 position : SV_Position; nointerpolation uint4 value : VALUE; };
V vs(uint4 data : DATA) {
  uint4 cb = rows[data.w], srv = source.Load(data.w);
  V v;
  v.position = float4(0, 0, 0, 1);
  v.value = uint4(data.x, cb.y, srv.z, data.y ^ cb.z ^ srv.x);
  return v;
}
uint4 ps(V v) : SV_Target { return v.value; }
)hlsl";

int
main() {
  step("compile every shader before creating a device");
  auto vs_code = compile(hlsl, "vs", "vs", {"CONSTANTS=" + std::to_string(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)});
  auto ps_code = compile(hlsl, "ps", "ps", {"CONSTANTS=" + std::to_string(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)});
  if (!vs_code || !ps_code)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
  if (!expect(options.MapNoOverwriteOnDynamicConstantBuffer && options.MapNoOverwriteOnDynamicBufferSRV,
              "D3D11.1 CB and buffer SRV NO_OVERWRITE support"))
    return verdict();
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> layout;
  const D3D11_INPUT_ELEMENT_DESC element{"DATA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0,
                                         D3D11_INPUT_PER_VERTEX_DATA, 0};
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout));

  std::vector<UINT> sizes;
  for (UINT bytes = 4 * sizeof(Row); bytes < DXMT_PAGE_SIZE; bytes *= 2)
    sizes.push_back(bytes);
  sizes.insert(sizes.end(), {DXMT_PAGE_SIZE - sizeof(Row), DXMT_PAGE_SIZE, DXMT_PAGE_SIZE + sizeof(Row)});
  const UINT binds[] = {D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_CONSTANT_BUFFER, D3D11_BIND_SHADER_RESOURCE};
  const char *names[] = {"VB", "CB", "SRV"};
  for (UINT bytes : sizes)
    for (UINT active = 0; active < std::size(binds); active++)
      for (bool recorded : {false, true}) {
        step("%s %s, %u bytes: initial-data control", recorded ? "deferred" : "immediate", names[active], bytes);
        const UINT rows = bytes / sizeof(Row), generations = DXMT_PAGE_SIZE / bytes + 2;
        const UINT probes[] = {0, rows / 2, rows - 1}, aftermath[] = {rows - 1, 1};
        auto data = [&](UINT version) {
          std::vector<Row> out(rows);
          for (UINT r = 0; r < rows; r++) {
            for (UINT c = 0; c < out[r].size() - 1; c++)
              out[r][c] = 1 + (version * rows + r) * out[r].size() + c;
            out[r].back() = r;
          }
          return out;
        };
        std::vector<Row> contents[] = {data(0), data(0), data(0)};
        ComPtr<ID3D11Buffer> inputs[std::size(binds)];
        for (UINT kind = 0; kind < std::size(binds); kind++) {
          D3D11_BUFFER_DESC desc{bytes, kind == active ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE, binds[kind],
                                  kind == active ? D3D11_CPU_ACCESS_WRITE : 0u};
          D3D11_SUBRESOURCE_DATA initial{contents[kind].data()};
          CHECK(device->CreateBuffer(&desc, &initial, &inputs[kind]));
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32G32B32A32_UINT, D3D11_SRV_DIMENSION_BUFFER};
        view.Buffer.NumElements = rows;
        ComPtr<ID3D11ShaderResourceView> srv;
        CHECK(device->CreateShaderResourceView(inputs[2].Get(), &view, &srv));
        const UINT controls = std::size(probes), after = recorded ? std::size(aftermath) : 0;
        const UINT width = controls + generations * (1 + std::size(probes)) + after + 1;
        D3D11_TEXTURE2D_DESC desc{width, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET};
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device->CreateTexture2D(&desc, nullptr, &target));
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
        immediate->ClearState();
        deferred->ClearState();
        const float clear[4] = {};
        immediate->ClearRenderTargetView(rtv.Get(), clear);
        auto bind = [&](ID3D11DeviceContext *context) {
          const UINT stride = sizeof(Row), offset = 0;
          context->IASetInputLayout(layout.Get());
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
          context->IASetVertexBuffers(0, 1, inputs[0].GetAddressOf(), &stride, &offset);
          context->VSSetConstantBuffers(0, 1, inputs[1].GetAddressOf());
          context->VSSetShaderResources(0, 1, srv.GetAddressOf());
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->PSSetShader(ps.Get(), nullptr, 0);
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        };
        std::vector<Row> want(width);
        UINT pixel = 0;
        auto draw = [&](ID3D11DeviceContext *context, UINT row) {
          D3D11_VIEWPORT viewport{float(pixel), 0, 1, 1, 0, 1};
          context->RSSetViewports(1, &viewport);
          context->Draw(1, row);
          const auto &vb = contents[0][row], &cb = contents[1][row], &sr = contents[2][row];
          want[pixel++] = {vb[0], cb[1], sr[2], vb[1] ^ cb[2] ^ sr[0]};
        };
        auto write = [&](ID3D11DeviceContext *context, D3D11_MAP mode, UINT first, UINT count) -> int {
          D3D11_MAPPED_SUBRESOURCE mapped{};
          auto hr = context->Map(inputs[active].Get(), 0, mode, 0, &mapped);
          if (!expect(hr == S_OK, "Map(%u) returned %#lx", mode, hr))
            return 1;
          if (!expect(mapped.pData != nullptr, "successful Map returned no memory")) {
            context->Unmap(inputs[active].Get(), 0);
            return 1;
          }
          memcpy(static_cast<Row *>(mapped.pData) + first, contents[active].data() + first, count * sizeof(Row));
          context->Unmap(inputs[active].Get(), 0);
          return 0;
        };
        auto verify = [&](const std::vector<Row> &expected) -> int {
          immediate->CopyResource(staging.Get(), target.Get());
          D3D11_MAPPED_SUBRESOURCE mapped{};
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          auto got = static_cast<const Row *>(mapped.pData);
          auto mismatch = std::mismatch(expected.begin(), expected.end(), got);
          if (mismatch.first != expected.end()) {
            UINT at = mismatch.first - expected.begin();
            for (UINT c = 0; c < mismatch.first->size(); c++)
              expect((*mismatch.second)[c] == (*mismatch.first)[c], "pixel %u channel %u: %#x, want %#x", at, c,
                     (*mismatch.second)[c], (*mismatch.first)[c]);
          }
          immediate->Unmap(staging.Get(), 0);
          return 0;
        };
        bind(immediate.Get());
        for (UINT row : probes)
          draw(immediate.Get(), row);
        auto context = recorded ? deferred.Get() : immediate.Get();
        if (recorded)
          bind(context);
        for (UINT generation = 0; generation < generations; generation++) {
          step("%s %s, %u bytes: generation %u DISCARD", recorded ? "deferred" : "immediate", names[active], bytes,
               generation);
          contents[active] = data(2 * generation + 1);
          if (write(context, D3D11_MAP_WRITE_DISCARD, 0, rows))
            return 1;
          draw(context, 0);
          step("%s %s, %u bytes: generation %u nonoverlapping append", recorded ? "deferred" : "immediate",
               names[active], bytes, generation);
          auto appended = data(2 * generation + 2);
          std::copy(appended.begin() + 2, appended.end(), contents[active].begin() + 2);
          if (write(context, D3D11_MAP_WRITE_NO_OVERWRITE, 2, rows - 2))
            return 1;
          for (UINT row : probes)
            draw(context, row);
        }
        ComPtr<ID3D11CommandList> list;
        if (recorded) {
          CHECK(deferred->FinishCommandList(FALSE, &list));
          step("deferred %s, %u bytes: immediate overwrite before execution", names[active], bytes);
          auto latest = contents[active];
          contents[active] = data(2 * generations + 1);
          if (write(immediate.Get(), D3D11_MAP_WRITE_DISCARD, 0, rows))
            return 1;
          immediate->ExecuteCommandList(list.Get(), TRUE);
          contents[active] = std::move(latest);
          step("deferred %s, %u bytes: immediate append uses the list's latest generation", names[active], bytes);
          contents[active][1] = data(2 * generations + 2)[1];
          if (write(immediate.Get(), D3D11_MAP_WRITE_NO_OVERWRITE, 1, 1))
            return 1;
          for (UINT row : aftermath)
            draw(immediate.Get(), row);
        }
        step("%s %s, %u bytes: every pending version and the untouched pixel", recorded ? "deferred" : "immediate",
             names[active], bytes);
        if (verify(want))
          return 1;
        if (recorded) {
          step("deferred %s, %u bytes: immediate reuse while the completed list remains alive", names[active], bytes);
          contents[active] = data(2 * generations + 3);
          // readback retires the displaced allocation so replay can expose premature recycling
          if (write(immediate.Get(), D3D11_MAP_WRITE_DISCARD, 0, rows) || verify(want))
            return 1;
          for (UINT generation = 1; generation < generations; generation++) {
            contents[active] = data(2 * generations + 3 + generation);
            if (write(immediate.Get(), D3D11_MAP_WRITE_DISCARD, 0, rows))
              return 1;
          }
          immediate->ClearRenderTargetView(rtv.Get(), clear);
          immediate->ExecuteCommandList(list.Get(), TRUE);
          std::fill(want.begin(), want.begin() + controls, Row{});
          std::fill(want.end() - after - 1, want.end(), Row{});
          step("deferred %s, %u bytes: replay preserves recorded versions after immediate reuse", names[active], bytes);
          if (verify(want))
            return 1;
        }
      }
  return verdict();
}
