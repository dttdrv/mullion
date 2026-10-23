// contract: MULLION_TRACE_ENCODERS logs each created render encoder's command-buffer ordinal, size and draws,
// including clears, resolves and presents; folded records and signals consume no encoder ordinal.
// "A command encoder defines the actions of a single pass" (Apple, Command encoder factory methods,
// https://developer.apple.com/documentation/metal/command-encoder-factory-methods). labels identify
// "The name of the encoder that generates the error information." (Apple, MTLCommandBufferEncoderInfo.label,
// https://developer.apple.com/documentation/metal/mtlcommandbufferencoderinfo/label).
// "The full extent of the resource view is always cleared.  Viewport and scissor are not applied."
// (D3D11.3 functional specification, 5.2.3.1). every pixel must retain the shader or clear value with tracing off/on.
// the sequential swap effect makes "DXGI persist the contents of the back buffer after you call" Present1
// (Microsoft, DXGI_SWAP_EFFECT_SEQUENTIAL,
// https://learn.microsoft.com/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect).
#include "d3d11_test.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <tuple>

static constexpr UINT width = 8, height = 4;
static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps() : SV_Target { return float4(1, 0, 0, 1); }
RWBuffer<uint> result : register(u0);
RWTexture2D<float4> image : register(u1);
[numthreads(1, 1, 1)] void cs() { result[0] = 1; image[uint2(0, 0)] = float4(1, 0, 0, 1); }
struct Vertex { float4 position : SV_Position; };
[maxvertexcount(3)] void gs(triangle Vertex vertices[3], inout TriangleStream<Vertex> stream) {
  for (uint i = 0; i < 3; i++) stream.Append(vertices[i]);
}
struct Factors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<Vertex, 3> vertices) {
  Factors value;
  value.edge[0] = value.edge[1] = value.edge[2] = value.inside = 1;
  return value;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("factors")]
Vertex hs(InputPatch<Vertex, 3> vertices, uint id : SV_OutputControlPointID) { return vertices[id]; }
[domain("tri")]
Vertex ds(Factors factor, const OutputPatch<Vertex, 3> vertices, float3 location : SV_DomainLocation) {
  Vertex value;
  value.position = vertices[0].position * location.x + vertices[1].position * location.y +
                   vertices[2].position * location.z;
  return value;
}
)hlsl";

static int
child(ComPtr<ID3DBlob> vs_code, ComPtr<ID3DBlob> ps_code, ComPtr<ID3DBlob> cs_code,
      ComPtr<ID3DBlob> gs_code, ComPtr<ID3DBlob> hs_code, ComPtr<ID3DBlob> ds_code) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ComputeShader> cs;
  ComPtr<ID3D11GeometryShader> gs;
  ComPtr<ID3D11HullShader> hs;
  ComPtr<ID3D11DomainShader> ds;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
  CHECK(device->CreateGeometryShader(gs_code->GetBufferPointer(), gs_code->GetBufferSize(), nullptr, &gs));
  CHECK(device->CreateHullShader(hs_code->GetBufferPointer(), hs_code->GetBufferSize(), nullptr, &hs));
  CHECK(device->CreateDomainShader(ds_code->GetBufferPointer(), ds_code->GetBufferSize(), nullptr, &ds));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  context->RSSetState(raster.Get());
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
  context->RSSetViewports(1, &viewport);
  D3D11_TEXTURE2D_DESC desc{
      width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> view;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &view));
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  auto pixels = [&](ID3D11Texture2D *texture, const BYTE (&color)[4]) {
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "readback failed"))
      return;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++)
        expect(!memcmp((const BYTE *)mapped.pData + y * mapped.RowPitch + x * sizeof(color), color, sizeof(color)),
               "wrong pixel %u/%u", x, y);
    context->Unmap(staging.Get(), 0);
  };
  const BYTE red[] = {255, 0, 0, 255}, green[] = {0, 255, 0, 255};
  const float clear[] = {0, 1, 0, 1};
  ComPtr<ID3D11Query> event;
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT};
  CHECK(device->CreateQuery(&query_desc, &event));
  auto output = buffer(device.Get(), sizeof(UINT), D3D11_BIND_UNORDERED_ACCESS);
  CHECK(output ? S_OK : E_FAIL);
  ComPtr<ID3D11UnorderedAccessView> unordered, target_uav;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER};
  uav_desc.Buffer.NumElements = 1;
  CHECK(device->CreateUnorderedAccessView(output.Get(), &uav_desc, &unordered));
  CHECK(device->CreateUnorderedAccessView(target.Get(), nullptr, &target_uav));
  auto rtv = view.Get();
  context->OMSetRenderTargets(1, &rtv, nullptr);
  const UINT indices[] = {0, 1, 2}, arguments[] = {std::size(indices), 1, 0, 0, 0};
  D3D11_BUFFER_DESC index_desc{sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA index_data{indices};
  ComPtr<ID3D11Buffer> index_buffer, indirect;
  CHECK(device->CreateBuffer(&index_desc, &index_data, &index_buffer));
  context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
  D3D11_BUFFER_DESC argument_desc{sizeof(arguments), D3D11_USAGE_DEFAULT};
  argument_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  D3D11_SUBRESOURCE_DATA argument_data{arguments};
  CHECK(device->CreateBuffer(&argument_desc, &argument_data, &indirect));

  step("a folded clear, compute and blit encoders between draws of the same target");
  context->ClearRenderTargetView(rtv, clear);
  context->Draw(0, 0);
  context->Draw(std::size(indices), 0);
  context->DrawIndexed(std::size(indices), 0, 0);
  context->DrawInstancedIndirect(indirect.Get(), 0);
  context->DrawIndexedInstancedIndirect(indirect.Get(), 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  ID3D11UnorderedAccessView *uavs[] = {unordered.Get(), target_uav.Get()};
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(0, std::size(uavs), uavs, nullptr);
  context->Dispatch(1, 1, 1);
  ID3D11UnorderedAccessView *unbound[std::size(uavs)]{};
  context->CSSetUnorderedAccessViews(0, std::size(unbound), unbound, nullptr);
  context->OMSetRenderTargets(1, &rtv, nullptr);
  context->Draw(3, 0);
  context->CopyResource(staging.Get(), target.Get());
  context->Draw(3, 0);
  pixels(target.Get(), red);
  expect(read(device.Get(), context.Get(), output.Get()) == std::vector<uint32_t>{1}, "wrong compute bytes");

  for (bool tessellation : {false, true}) {
    step("four %s draws: direct, indexed and their indirect forms", tessellation ? "tessellation" : "geometry");
    context->ClearRenderTargetView(rtv, clear);
    context->GSSetShader(tessellation ? nullptr : gs.Get(), nullptr, 0);
    context->HSSetShader(tessellation ? hs.Get() : nullptr, nullptr, 0);
    context->DSSetShader(tessellation ? ds.Get() : nullptr, nullptr, 0);
    context->IASetPrimitiveTopology(tessellation ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(std::size(indices), 0);
    context->DrawIndexed(std::size(indices), 0, 0);
    context->DrawInstancedIndirect(indirect.Get(), 0);
    context->DrawIndexedInstancedIndirect(indirect.Get(), 0);
    pixels(target.Get(), red);
  }

  step("a standalone color clear");
  context->ClearRenderTargetView(rtv, clear);
  pixels(target.Get(), green);

  step("a standalone depth/stencil clear");
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  desc.CPUAccessFlags = 0;
  desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> dsv;
  CHECK(device->CreateTexture2D(&desc, nullptr, &depth));
  CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
  context->Flush();

  step("a multisample clear followed by a standalone resolve");
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  desc.SampleDesc.Count = 2;
  UINT quality = 0;
  CHECK(device->CheckMultisampleQualityLevels(desc.Format, desc.SampleDesc.Count, &quality));
  CHECK(quality ? S_OK : E_FAIL);
  ComPtr<ID3D11Texture2D> multisampled;
  ComPtr<ID3D11RenderTargetView> ms_view;
  CHECK(device->CreateTexture2D(&desc, nullptr, &multisampled));
  CHECK(device->CreateRenderTargetView(multisampled.Get(), nullptr, &ms_view));
  const float red_clear[] = {1, 0, 0, 1};
  context->ClearRenderTargetView(rtv, red_clear);
  context->ClearRenderTargetView(ms_view.Get(), clear);
  context->ResolveSubresource(target.Get(), 0, multisampled.Get(), 0, desc.Format);
  pixels(target.Get(), green);

  step("present a cleared back buffer");
  auto window = CreateWindowA("STATIC", "trace_encoders", WS_POPUP, 0, 0, width, height, nullptr, nullptr,
                              GetModuleHandleA(nullptr), nullptr);
  CHECK(window ? S_OK : E_FAIL);
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory> factory;
  ComPtr<IDXGISwapChain> chain;
  CHECK(device.As(&dxgi));
  CHECK(dxgi->GetAdapter(&adapter));
  CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC swap{{width, height, {}, desc.Format}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, window,
                            TRUE, DXGI_SWAP_EFFECT_SEQUENTIAL};
  CHECK(factory->CreateSwapChain(device.Get(), &swap, &chain));
  ComPtr<ID3D11Texture2D> backbuffer;
  ComPtr<ID3D11RenderTargetView> back_view;
  CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));
  CHECK(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &back_view));
  context->ClearRenderTargetView(back_view.Get(), clear);
  context->End(event.Get());
  CHECK(chain->Present(0, 0));
  pixels(backbuffer.Get(), green);
  chain.Reset();
  DestroyWindow(window);
  return verdict();
}

int
main(int argc, char **argv) {
  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps"), cs = compile(hlsl, "cs", "cs");
  auto gs = compile(hlsl, "gs", "gs"), hs = compile(hlsl, "hs", "hs"), ds = compile(hlsl, "ds", "ds");
  if (!expect(vs && ps && cs && gs && hs && ds, "HLSL did not compile"))
    return verdict();
  if (argc > 1 && !strcmp(argv[1], "shaders"))
    return verdict();
  if (argc > 1)
    return child(vs, ps, cs, gs, hs, ds);

  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  modulePath(exe, sizeof(exe));
  struct Setting { bool trace, errors; };
  const Setting settings[] = {{false, false}, {false, true}, {true, false}, {true, true}};
  for (auto setting : settings) {
    step("trace=%u, errors=%u", setting.trace, setting.errors);
    CHECK(GetTempFileNameA(temporary, "enc", 0, directory) ? S_OK : E_FAIL);
    DeleteFileA(directory);
    CHECK(CreateDirectoryA(directory, nullptr) ? S_OK : E_FAIL);
    SetEnvironmentVariableA("MULLION_LOG_PATH", directory);
    SetEnvironmentVariableA("MULLION_LOG_LEVEL", "info");
    SetEnvironmentVariableA("MULLION_TRACE_ENCODERS", setting.trace ? "1" : nullptr);
    SetEnvironmentVariableA("MULLION_D3D12_GPU_ERRORS", setting.errors ? "1" : nullptr);
    SetEnvironmentVariableA("MULLION_DIAG", nullptr);
    std::string command = std::string("\"") + exe + "\" child";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)
              ? S_OK : E_FAIL);
    auto waited = WaitForSingleObject(process.hProcess, 60000);
    if (waited != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 60000);
    }
    expect(waited == WAIT_OBJECT_0, "child timed out");
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(status == 0, "child failed with %lu", status);
    struct Pass { const char *kind; UINT index, draws, geometry = 0, tessellation = 0; };
    const Pass expected[] = {{"render", 0, 4}, {"render", 2, 1}, {"render", 0, 1},
                             {"render", 0, 4, 4}, {"render", 0, 4, 0, 4},
                             {"clear", 0, 0}, {"clear", 0, 0}, {"clear", 0, 0}, {"clear", 1, 0}, {"resolve", 2, 0},
                             {"clear", 0, 0}, {"present", 1, 1}};
    size_t count = 0;
    unsigned long long draw_chunk = 0;
    struct Record {
      unsigned long long seq;
      UINT index, width, height, draws, geometry, tessellation;
      char kind[16];
    };
    std::vector<Record> records;
    for (auto &entry : std::filesystem::directory_iterator(directory)) {
      std::ifstream file(entry.path());
      for (std::string line; std::getline(file, line);) {
        const std::string prefix = "info:  encoder ";
        if (!line.starts_with(prefix))
          continue;
        auto &record = records.emplace_back();
        int end = 0;
        auto text = line.substr(prefix.size());
        expect(sscanf(text.c_str(), "%15s %llu.%u: %ux%u, %u draws (%u geometry, %u tessellation)%n", record.kind,
                      &record.seq, &record.index, &record.width, &record.height, &record.draws, &record.geometry,
                      &record.tessellation, &end) == 8 && end == int(text.size()),
               "invalid trace: %s", line.c_str());
      }
    }
    std::sort(records.begin(), records.end(), [](auto &a, auto &b) {
      return std::tie(a.seq, a.index) < std::tie(b.seq, b.index);
    });
    for (auto &record : records) {
      if (expect(setting.trace && count < std::size(expected), "unexpected encoder %s", record.kind)) {
        auto pass = expected[count];
        expect(!strcmp(record.kind, pass.kind) && record.index == pass.index && record.draws == pass.draws &&
                   record.geometry == pass.geometry && record.tessellation == pass.tessellation,
               "wrong encoder at %zu: %s %llu.%u with %u draws", count, record.kind, record.seq, record.index,
               record.draws);
        expect(record.width == width && record.height == height, "wrong size: %ux%u", record.width, record.height);
        expect(record.seq != 0, "invalid command buffer sequence");
        if (!count)
          draw_chunk = record.seq;
        if (count < 2)
          expect(record.seq == draw_chunk, "draws unexpectedly split into different command buffers");
        if (count == 2)
          expect(record.seq > draw_chunk, "draw after staging readback did not start a new command buffer");
      }
      count++;
    }
    expect(count == (setting.trace ? std::size(expected) : 0), "got %zu render traces", count);
    std::filesystem::remove_all(directory);
  }
  return verdict();
}
