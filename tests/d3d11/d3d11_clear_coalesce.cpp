// contract: redundant consecutive color/depth-stencil clears fold into one clear pass; cross-view
// UNORM/sRGB clears on the same subresource fold into a following render pass only when the RGB clear
// values are [0, 1] endpoints (bit-identical in linear and sRGB), and never corrupt non-endpoint linear
// clears; render followed by a color clear preserves the cleared result.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <utility>

static constexpr UINT kWidth = 8, kHeight = 4;
static const char kHlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps() : SV_Target { return float4(1, 0, 0, 1); }
)hlsl";

static bool
require(HRESULT hr, const char *what) {
  return expect(SUCCEEDED(hr), "%s failed: %#lx", what, hr);
}

static int
child(const ComPtr<ID3DBlob> &vs_code, const ComPtr<ID3DBlob> &ps_code) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (!require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context),
               "D3D11CreateDevice"))
    return verdict();
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  if (!require(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs),
               "CreateVertexShader") ||
      !require(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps),
               "CreatePixelShader"))
    return verdict();
  D3D11_RASTERIZER_DESC raster_desc{.FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE, .DepthClipEnable = TRUE};
  D3D11_RASTERIZER_DESC scissored_desc = raster_desc;
  scissored_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster, scissored;
  if (!require(device->CreateRasterizerState(&raster_desc, &raster), "CreateRasterizerState") ||
      !require(device->CreateRasterizerState(&scissored_desc, &scissored), "CreateRasterizerState(scissored)"))
    return verdict();
  context->RSSetState(raster.Get());
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D11_VIEWPORT viewport{.TopLeftX = 0, .TopLeftY = 0, .Width = kWidth, .Height = kHeight, .MinDepth = 0, .MaxDepth = 1};
  context->RSSetViewports(1, &viewport);

  D3D11_TEXTURE2D_DESC staging_desc{
      .Width = kWidth,
      .Height = kHeight,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
      .SampleDesc = {.Count = 1, .Quality = 0},
      .Usage = D3D11_USAGE_STAGING,
      .BindFlags = 0,
      .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
      .MiscFlags = 0,
  };
  ComPtr<ID3D11Texture2D> staging;
  if (!require(device->CreateTexture2D(&staging_desc, nullptr, &staging), "CreateTexture2D(staging)"))
    return verdict();

  D3D11_TEXTURE2D_DESC typeless_desc = staging_desc;
  typeless_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  typeless_desc.Usage = D3D11_USAGE_DEFAULT;
  typeless_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  typeless_desc.CPUAccessFlags = 0;
  ComPtr<ID3D11Texture2D> target;
  if (!require(device->CreateTexture2D(&typeless_desc, nullptr, &target), "CreateTexture2D(target)"))
    return verdict();

  D3D11_RENDER_TARGET_VIEW_DESC unorm_desc{.Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D};
  D3D11_RENDER_TARGET_VIEW_DESC srgb_desc{.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D};
  ComPtr<ID3D11RenderTargetView> unorm_rtv, srgb_rtv;
  if (!require(device->CreateRenderTargetView(target.Get(), &unorm_desc, &unorm_rtv), "CreateRTV(UNORM)") ||
      !require(device->CreateRenderTargetView(target.Get(), &srgb_desc, &srgb_rtv), "CreateRTV(sRGB)"))
    return verdict();

  D3D11_TEXTURE2D_DESC depth_desc = typeless_desc;
  depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> dsv;
  if (!require(device->CreateTexture2D(&depth_desc, nullptr, &depth), "CreateTexture2D(depth)") ||
      !require(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "CreateDSV"))
    return verdict();

  const auto check_all = [&](const std::array<BYTE, 4> &color) {
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "readback failed"))
      return;
    for (UINT y = 0; y < kHeight; y++)
      for (UINT x = 0; x < kWidth; x++)
        expect(!memcmp(static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch + x * color.size(), color.data(),
                       color.size()),
               "wrong pixel %u/%u", x, y);
    context->Unmap(staging.Get(), 0);
  };

  constexpr std::array<BYTE, 4> kRed{255, 0, 0, 255};
  constexpr std::array<BYTE, 4> kGreen{0, 255, 0, 255};
  constexpr std::array<float, 4> kRedClear{1.0f, 0.0f, 0.0f, 1.0f};
  constexpr std::array<float, 4> kGreenClear{0.0f, 1.0f, 0.0f, 1.0f};
  constexpr std::array<float, 4> kMidClear{0.5f, 0.25f, 0.75f, 1.0f};

  auto *unorm_ptr = unorm_rtv.Get();
  auto *srgb_ptr = srgb_rtv.Get();

  step("redundant consecutive color clears fold into one clear");
  context->ClearRenderTargetView(unorm_ptr, kRedClear.data());
  context->ClearRenderTargetView(unorm_ptr, kGreenClear.data());
  check_all(kGreen);

  step("redundant consecutive depth/stencil clears fold into one clear");
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  context->Flush();

  step("endpoint UNORM clear folds into sRGB render pass");
  context->ClearRenderTargetView(unorm_ptr, kGreenClear.data());
  context->OMSetRenderTargets(1, &srgb_ptr, nullptr);
  context->Draw(3, 0);
  check_all(kRed);

  step("non-endpoint UNORM clear does not fold into sRGB render pass");
  context->ClearRenderTargetView(unorm_ptr, kMidClear.data());
  context->RSSetState(scissored.Get());
  D3D11_RECT clip{.left = 0, .top = 0, .right = 1, .bottom = 1};
  context->RSSetScissorRects(1, &clip);
  context->OMSetRenderTargets(1, &srgb_ptr, nullptr);
  context->Draw(3, 0);
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "mid readback failed")) {
    const auto *px = static_cast<const BYTE *>(mapped.pData) + 4;
    expect(std::abs(int(px[0]) - 128) <= 1 && std::abs(int(px[1]) - 64) <= 1 && std::abs(int(px[2]) - 191) <= 1 &&
               px[3] == 255,
           "sRGB fold corrupted linear clear: got (%u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
    context->Unmap(staging.Get(), 0);
  }
  context->RSSetState(raster.Get());

  step("render followed by color clear preserves cleared target");
  context->OMSetRenderTargets(1, &unorm_ptr, nullptr);
  context->Draw(3, 0);
  context->ClearRenderTargetView(unorm_ptr, kGreenClear.data());
  check_all(kGreen);

  return verdict();
}

int
main(int argc, char ** /*argv*/) {
  auto vs = compile(kHlsl, "vs", "vs");
  auto ps = compile(kHlsl, "ps", "ps");
  if (!expect(vs && ps, "HLSL did not compile"))
    return verdict();
  if (argc > 1)
    return child(vs, ps);

  std::array<char, MAX_PATH> temporary{};
  std::array<char, MAX_PATH> directory{};
  std::array<char, MAX_PATH> exe{};
  GetTempPathA(static_cast<DWORD>(temporary.size()), temporary.data());
  GetModuleFileNameA(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  if (!expect(temporary_directory(temporary.data(), "clr", directory.data()), "temporary_directory failed"))
    return verdict();
  SetEnvironmentVariableA("MULLION_LOG_PATH", directory.data());
  SetEnvironmentVariableA("MULLION_LOG_LEVEL", "info");
  SetEnvironmentVariableA("MULLION_TRACE_ENCODERS", "1");
  SetEnvironmentVariableA("MULLION_DIAG", nullptr);

  std::string command = std::string("\"") + exe.data() + "\" child";
  STARTUPINFOA startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!expect(CreateProcessA(exe.data(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                             &process) != 0,
              "CreateProcessA failed"))
    return verdict();
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

  struct Pass {
    const char *kind;
    UINT index;
    UINT draws;
  };
  constexpr std::array<Pass, 7> kExpected{{
      {.kind = "clear", .index = 0, .draws = 0},
      {.kind = "clear", .index = 0, .draws = 0},
      {.kind = "render", .index = 0, .draws = 1},
      {.kind = "clear", .index = 0, .draws = 0},
      {.kind = "render", .index = 1, .draws = 1},
      {.kind = "render", .index = 0, .draws = 1},
      {.kind = "clear", .index = 1, .draws = 0},
  }};
  struct Record {
    unsigned long long seq = 0;
    UINT index = 0, width = 0, height = 0, draws = 0, geometry = 0, tessellation = 0;
    std::array<char, 16> kind{};
  };
  std::vector<Record> records;
  for (const auto &entry : std::filesystem::directory_iterator(directory.data())) {
    std::ifstream file(entry.path());
    for (std::string line; std::getline(file, line);) {
      const std::string prefix = "info:  encoder ";
      if (!line.starts_with(prefix))
        continue;
      auto &record = records.emplace_back();
      int end = 0;
      auto text = line.substr(prefix.size());
      expect(sscanf(text.c_str(), "%15s %llu.%u: %ux%u, %u draws (%u geometry, %u tessellation)%n", record.kind.data(),
                    &record.seq, &record.index, &record.width, &record.height, &record.draws, &record.geometry,
                    &record.tessellation, &end) == 8 &&
                 std::cmp_equal(end, text.size()),
             "invalid trace: %s", line.c_str());
    }
  }
  std::ranges::sort(records, [](const auto &a, const auto &b) {
    return std::tie(a.seq, a.index) < std::tie(b.seq, b.index);
  });
  expect(records.size() == kExpected.size(), "got %zu render traces, expected %zu", records.size(), kExpected.size());
  for (size_t i = 0; i < std::min(records.size(), kExpected.size()); i++) {
    const auto &rec = records[i];
    const auto &exp = kExpected[i];
    expect(!strcmp(rec.kind.data(), exp.kind) && rec.index == exp.index && rec.draws == exp.draws &&
               rec.width == kWidth && rec.height == kHeight,
           "wrong encoder at %zu: %s %llu.%u (%ux%u, %u draws)", i, rec.kind.data(), rec.seq, rec.index, rec.width,
           rec.height, rec.draws);
  }
  std::filesystem::remove_all(directory.data());
  return verdict();
}
