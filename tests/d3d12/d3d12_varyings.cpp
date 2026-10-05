// contract: a vertex shader's outputs reach the pixel shader by register and component, whatever elements either
// declares there (D3D11.3 4.4: signatures; 16.4: one interpolation mode per register):
// - as many scalar elements as the registers hold, each with its own value;
// - floats and integers of either sign side by side, their bits unchanged;
// - an element the pixel shader declares narrower than the vertex shader writes it;
// - two elements of one register, each evaluated at its own offset (EvaluateAttributeSnapped: 1/16 pixel steps from
//   the pixel center, D3D11.3 22.3.25);
// - clip and cull distances, read as inputs (15.4.3).
// every case draws one triangle over the target and is scissored to a column of its own.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>

static const char hlsl[] = R"hlsl(
// the whole target, and each pixel's own position as an attribute would have it
float4 corner(uint id) { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
float2 pixels(float4 pos) { return (pos.xy * float2(0.5, -0.5) + 0.5) * SIZE; }

struct Many { float4 pos : SV_Position; MANY_FIELDS };
Many vs_many(uint id : SV_VertexID) {
  Many v;
  v.pos = corner(id);
  MANY_SET
  return v;
}
float4 ps_many(Many v) : SV_Target { return float4(MANY_SUM, 0, 0, 0); }

struct Kinds {
  float4 pos : SV_Position;
  nointerpolation float f : F;
  nointerpolation uint u : U;
  nointerpolation int i : I;
  nointerpolation float g : G;
};
Kinds vs_kinds(uint id : SV_VertexID) {
  Kinds v;
  v.pos = corner(id);
  v.f = F_VALUE;
  v.u = U_VALUE;
  v.i = I_VALUE;
  v.g = G_VALUE;
  return v;
}
float4 ps_kinds(Kinds v) : SV_Target { return float4(v.f, v.u == U_VALUE, v.i == I_VALUE, v.g); }

struct Wide { float4 pos : SV_Position; float3 before : BEFORE; float4 wide : WIDE; };
Wide vs_wide(uint id : SV_VertexID) {
  Wide v;
  v.pos = corner(id);
  v.before = float3(7, 8, 9);
  v.wide = float4(1, 2, 3, 4);
  return v;
}
float4 ps_narrow(float4 pos : SV_Position, float3 before : BEFORE, float2 wide : WIDE) : SV_Target {
  return float4(wide, before.xz);
}

struct Pair { float4 pos : SV_Position; float2 a : A; float2 b : B; };
Pair vs_pair(uint id : SV_VertexID) {
  Pair v;
  v.pos = corner(id);
  v.a = pixels(v.pos);
  v.b = 2 * pixels(v.pos) + 1;
  return v;
}
float4 ps_pair(Pair v) : SV_Target {
  return float4(EvaluateAttributeSnapped(v.a, int2(A_OFFSET)), EvaluateAttributeSnapped(v.b, int2(B_OFFSET)));
}

// a register of clip distances and one of cull distances
struct Distances { float4 pos : SV_Position; float4 clip : SV_ClipDistance; float2 cull : SV_CullDistance; };
Distances vs_distances(uint id : SV_VertexID) {
  Distances v;
  v.pos = corner(id);
  v.clip = float4(3, 4, 7, 8);
  v.cull = float2(5, 6);
  return v;
}
float4 ps_distances(Distances v) : SV_Target { return float4(v.clip.xw, v.cull); }
)hlsl";

struct Pixel {
  float v[4];
};
// what the cases' expectations are made of
struct Context {
  float many_sum, f, g;
  const int *a, *b;
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // 31 registers of four scalars beside the position: Metal passes 124 components to a pixel shader, Direct3D 128
  const UINT size = 20, many = 124, column = 4;
  const int a_offset[2] = {4, -8}, b_offset[2] = {-8, 4};
  const float f_value = 1.5f, g_value = -2.25f;
  std::string fields, set, sum = "0";
  auto attribute = [](UINT n) { return (n * 7) % 13; };
  UINT many_sum = 0;
  for (UINT n = 0; n < many; n++) {
    auto name = "a" + std::to_string(n);
    fields += "float " + name + " : A" + std::to_string(n) + "; ";
    set += "v." + name + " = " + std::to_string(attribute(n)) + "; ";
    sum += " + v." + name + " * " + std::to_string(n + 1);
    many_sum += attribute(n) * (n + 1);
  }
  auto pair = [](const int *v) { return std::to_string(v[0]) + ", " + std::to_string(v[1]); };
  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size), "MANY_FIELDS=" + fields, "MANY_SET=" + set,    "MANY_SUM=" + sum,
      "F_VALUE=" + std::to_string(f_value), "G_VALUE=" + std::to_string(g_value),
      // an unsigned value that is a NaN's bits as a float, and a negative integer
      "U_VALUE=0xffc00001", "I_VALUE=-7", "A_OFFSET=" + pair(a_offset),              "B_OFFSET=" + pair(b_offset),
  };
  const Context context{(float)many_sum, f_value, g_value, a_offset, b_offset};
  // a value that is the same at every vertex arrives within a few units of its last place; one interpolated across
  // `size` pixels keeps this much less
  const float constant = 8 * FLT_EPSILON, interpolated = 64 * FLT_EPSILON * size;
  struct Case {
    const char *name, *vs, *ps;
    float tolerance;
    Pixel (*want)(float x, float y, const Context &c);
    ComPtr<ID3D12PipelineState> pso;
  } cases[] = {
      {"many", "vs_many", "ps_many", constant, [](float, float, const Context &c) { return Pixel{{c.many_sum, 0, 0, 0}}; }},
      {"kinds", "vs_kinds", "ps_kinds", constant, [](float, float, const Context &c) { return Pixel{{c.f, 1, 1, c.g}}; }},
      {"narrow", "vs_wide", "ps_narrow", constant, [](float, float, const Context &) { return Pixel{{1, 2, 7, 9}}; }},
      // x and y are the pixel's center, and an offset is in sixteenths of a pixel
      {"pair", "vs_pair", "ps_pair", interpolated,
       [](float x, float y, const Context &c) {
         return Pixel{{x + c.a[0] / 16.f, y + c.a[1] / 16.f, 2 * (x + c.b[0] / 16.f) + 1, 2 * (y + c.b[1] / 16.f) + 1}};
       }},
      {"distances", "vs_distances", "ps_distances", constant, [](float, float, const Context &) { return Pixel{{3, 8, 5, 6}}; }},
  };
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  for (auto &c : cases) {
    auto vs = compiler.compile(hlsl, c.vs, "vs", defines), ps = compiler.compile(hlsl, c.ps, "ps", defines);
    if (vs.empty() || ps.empty()) {
      printf("failed: the %s case does not compile\n", c.name);
      return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs);
    desc.PS = bytecode(ps);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&c.pso)))) {
      printf("failed: no pipeline for the %s case\n", c.name);
      return 1;
    }
  }

  const UINT64 row = (size * sizeof(Pixel) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                     ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
  commands->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  commands->RSSetViewports(1, &viewport);
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT c = 0; c < std::size(cases); c++) {
    D3D12_RECT scissor{(LONG)(c * column), 0, (LONG)((c + 1) * column), (LONG)size};
    commands->RSSetScissorRects(1, &scissor);
    commands->SetPipelineState(cases[c].pso.Get());
    commands->DrawInstanced(3, 1, 0, 0);
  }
  transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, size, size, 1, (UINT)row}};
  commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), commands.Get()));
  uint8_t *pixels;
  CHECK(readback->Map(0, nullptr, (void **)&pixels));

  unsigned failures = 0;
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < column * std::size(cases); x++) {
      auto &c = cases[x / column];
      auto &got = reinterpret_cast<const Pixel *>(pixels + row * y)[x];
      auto want = c.want(x + 0.5f, y + 0.5f, context);
      bool wrong = false;
      for (UINT i = 0; i < 4; i++)
        wrong |= !(std::fabs(got.v[i] - want.v[i]) <= c.tolerance * std::max(1.f, std::fabs(want.v[i])));
      if (wrong && failures++ < 12)
        printf(
            "%s at %u,%u: %g %g %g %g, want %g %g %g %g\n", c.name, x, y, got.v[0], got.v[1], got.v[2], got.v[3],
            want.v[0], want.v[1], want.v[2], want.v[3]
        );
    }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %zu cases, %u scalars in the largest\n", std::size(cases), many);
  return 0;
}
