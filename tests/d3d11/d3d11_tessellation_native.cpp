// contract: a tessellated draw that Metal's tessellator tessellates as Direct3D does (a triangle or quad domain to
// triangles, integer or pow2 partitioning, no geometry shader: D3D11.3 11.7) draws the same through it as through
// the mesh emulation, which d3d11.nativeTessellation = False keeps for every draw:
// - a patch's triangles cover it once, without a gap or a pixel twice (11.7.6), and are clockwise or
//   counter-clockwise as the hull shader says, where u goes right and v down: with back faces culled, clockwise ones
//   leave 1 over the patch and counter-clockwise ones nothing;
// - the vertex shader reads its control points from vertex buffers, per vertex and per instance from
//   StartInstanceLocation (8.4), through 16- and 32-bit indices from StartIndexLocation and BaseVertexLocation (8.3),
//   and each stage reads its own constant buffers and resources (5.3, 7.5): a vertex's value is linear in its
//   position, so every pixel over a patch has its center's, and the domain shader's patch value, from a buffer at
//   its SV_PrimitiveID, the patch's place in its instance (11.7.9.1), which the pixel shader gets too;
// - an indirect draw in the same pass (8.5.1), which the mesh emulation draws, and fractional partitioning between.
// each run is a process of its own, one with each setting; the encoder trace (MULLION_TRACE_ENCODERS) shows the
// hull kernels the native draws run before their passes, and none with the setting off. each run then times a
// large draw with timestamp queries.
#include "d3d11_test.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

static const char hlsl[] = R"hlsl(
cbuffer VertexConstants : register(b0) { float4 vs_add; };
struct VSI { float2 pos : POSITION; float2 offset : OFFSET; };
struct CP { float2 pos : POS; float attr : ATTR; };
CP vs(VSI i) {
  CP o;
  o.pos = i.pos + i.offset;
  o.attr = 0.01 * o.pos.x + 0.02 * o.pos.y + vs_add.x;
  return o;
}
cbuffer HullConstants : register(b1) { float4 hs_factors; };
struct PC {
  float edges[EDGES] : SV_TessFactor;
  float inside[INSIDES] : SV_InsideTessFactor;
};
PC pc(InputPatch<CP, CORNERS> ip) {
  PC o;
  [unroll] for (int i = 0; i < EDGES; i++)
    o.edges[i] = hs_factors.x + i;
  [unroll] for (int j = 0; j < INSIDES; j++)
    o.inside[j] = hs_factors.y + j;
  return o;
}
[domain(DOMAIN)] [partitioning(PARTITIONING)] [outputtopology(TOPOLOGY)] [outputcontrolpoints(CORNERS)]
[patchconstantfunc("pc")] [maxtessfactor(64)]
CP hs(InputPatch<CP, CORNERS> ip, uint id : SV_OutputControlPointID) { return ip[id]; }
cbuffer DomainConstants : register(b2) { float4 ds_scale; };
Buffer<float> patch_values : register(t3);
struct DSO {
  float4 pos : SV_Position;
  float attr : ATTR;
  nointerpolation float value : VALUE;
};
[domain(DOMAIN)]
DSO ds(PC pc, LOCATION l : SV_DomainLocation, const OutputPatch<CP, CORNERS> p, uint patch : SV_PrimitiveID) {
  DSO o;
#if QUAD
  float2 pos = lerp(lerp(p[0].pos, p[1].pos, l.x), lerp(p[3].pos, p[2].pos, l.x), l.y);
  o.attr = lerp(lerp(p[0].attr, p[1].attr, l.x), lerp(p[3].attr, p[2].attr, l.x), l.y);
#else
  float2 pos = p[0].pos * l.x + p[1].pos * l.y + p[2].pos * l.z;
  o.attr = p[0].attr * l.x + p[1].attr * l.y + p[2].attr * l.z;
#endif
  o.value = ds_scale.x * patch_values[patch];
  o.pos = float4(pos.x / WIDTH * 2 - 1, 1 - pos.y / HEIGHT * 2, 0, 1);
  return o;
}
float4 ps(DSO i, uint patch : SV_PrimitiveID) : SV_Target { return float4(1, i.attr, i.value, patch + 1); }
)hlsl";

static constexpr UINT width = 256, height = 128, cell = 16;
static constexpr float vs_add = 0.5f, ds_scale = 2;
// a patch value is 10 + its place
static float
patch_value(UINT patch) {
  return ds_scale * (10 + patch);
}

struct Domain {
  const char *name;
  bool quad;
};
static const Domain domains[] = {{"tri", false}, {"quad", true}};
static const char *const partitionings[] = {"integer", "pow2", "fractional_odd"};

// what a cell of the target holds after a draw: covered by a patch (its place, -1 for none)
struct Cell {
  int patch = -1;
};

// a patch's corners in pixels, at a cell's top left: a quad's are its corners, clockwise from the top left (u right,
// v down); a triangle's are u's, v's and w's, the cell's top right, bottom left and top left
static void
corners(bool quad, float x, float y, float (*out)[2]) {
  if (quad) {
    const float quad_corners[4][2] = {{x, y}, {x + cell, y}, {x + cell, y + cell}, {x, y + cell}};
    memcpy(out, quad_corners, sizeof(quad_corners));
  } else {
    const float tri_corners[3][2] = {{x + cell, y}, {x, y + cell}, {x, y}};
    memcpy(out, tri_corners, sizeof(tri_corners));
  }
}

static int
child(bool timing) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  D3D11_TEXTURE2D_DESC texture_desc{width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &staging));
  // a pixel adds every triangle over it
  D3D11_BLEND_DESC blend_desc{};
  blend_desc.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
  ComPtr<ID3D11BlendState> blend;
  CHECK(device->CreateBlendState(&blend_desc, &blend));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_BACK};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  context->RSSetState(raster.Get());
  context->RSSetViewports(1, &viewport);
  context->OMSetBlendState(blend.Get(), nullptr, ~0u);
  auto view = rtv.Get();
  context->OMSetRenderTargets(1, &view, nullptr);

  auto constants = [&](float x) {
    const float values[4] = {x, x, x, x};
    D3D11_BUFFER_DESC desc{sizeof(values), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    D3D11_SUBRESOURCE_DATA data{values};
    ComPtr<ID3D11Buffer> out;
    device->CreateBuffer(&desc, &data, &out);
    return out;
  };
  auto vs_constants = constants(vs_add), ds_constants = constants(ds_scale);
  D3D11_BUFFER_DESC hs_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> hs_constants;
  CHECK(device->CreateBuffer(&hs_desc, nullptr, &hs_constants));
  CHECK(vs_constants && ds_constants ? S_OK : E_FAIL);
  context->VSSetConstantBuffers(0, 1, vs_constants.GetAddressOf());
  context->HSSetConstantBuffers(1, 1, hs_constants.GetAddressOf());
  context->DSSetConstantBuffers(2, 1, ds_constants.GetAddressOf());
  const UINT most_patches = 256;
  std::vector<float> values(most_patches);
  for (UINT i = 0; i < most_patches; i++)
    values[i] = 10.0f + i;
  D3D11_BUFFER_DESC values_desc{(UINT)(values.size() * sizeof(float)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA values_data{values.data()};
  ComPtr<ID3D11Buffer> values_buffer;
  CHECK(device->CreateBuffer(&values_desc, &values_data, &values_buffer));
  D3D11_SHADER_RESOURCE_VIEW_DESC values_view_desc{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_BUFFER};
  values_view_desc.Buffer = {0, most_patches};
  ComPtr<ID3D11ShaderResourceView> values_view;
  CHECK(device->CreateShaderResourceView(values_buffer.Get(), &values_view_desc, &values_view));
  context->DSSetShaderResources(3, 1, values_view.GetAddressOf());

  D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  // one pipeline of shaders for each domain, partitioning and winding
  struct Shaders {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
  };
  auto make = [&](const Domain &domain, const char *partitioning, const char *topology, Shaders &out) {
    std::vector<std::string> d = {
        domain.quad ? "QUAD=1" : "QUAD=0",
        domain.quad ? "CORNERS=4" : "CORNERS=3",
        domain.quad ? "EDGES=4" : "EDGES=3",
        domain.quad ? "INSIDES=2" : "INSIDES=1",
        std::string("DOMAIN=\"") + domain.name + "\"",
        std::string("PARTITIONING=\"") + partitioning + "\"",
        std::string("TOPOLOGY=\"") + topology + "\"",
        domain.quad ? "LOCATION=float2" : "LOCATION=float3",
        "WIDTH=" + std::to_string(width),
        "HEIGHT=" + std::to_string(height),
    };
    auto vs = compile(hlsl, "vs", "vs", d), hs = compile(hlsl, "hs", "hs", d), ds = compile(hlsl, "ds", "ds", d),
         ps = compile(hlsl, "ps", "ps", d);
    return vs && hs && ds && ps &&
           SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &out.vs)) &&
           SUCCEEDED(device->CreateHullShader(hs->GetBufferPointer(), hs->GetBufferSize(), nullptr, &out.hs)) &&
           SUCCEEDED(device->CreateDomainShader(ds->GetBufferPointer(), ds->GetBufferSize(), nullptr, &out.ds)) &&
           SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &out.ps)) &&
           SUCCEEDED(device->CreateInputLayout(elements, std::size(elements), vs->GetBufferPointer(),
                                               vs->GetBufferSize(), &out.layout));
  };
  auto bind = [&](const Shaders &shaders, bool quad) {
    context->IASetInputLayout(shaders.layout.Get());
    context->IASetPrimitiveTopology(quad ? D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                         : D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    context->VSSetShader(shaders.vs.Get(), nullptr, 0);
    context->HSSetShader(shaders.hs.Get(), nullptr, 0);
    context->DSSetShader(shaders.ds.Get(), nullptr, 0);
    context->PSSetShader(shaders.ps.Get(), nullptr, 0);
  };
  auto vertex_buffer = [&](const std::vector<float> &data) {
    D3D11_BUFFER_DESC desc{(UINT)(data.size() * sizeof(float)), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
    D3D11_SUBRESOURCE_DATA init{data.data()};
    ComPtr<ID3D11Buffer> out;
    device->CreateBuffer(&desc, &init, &out);
    return out;
  };
  auto set_vertices = [&](ID3D11Buffer *vertices, ID3D11Buffer *instances) {
    ID3D11Buffer *buffers[] = {vertices, instances};
    UINT strides[] = {8, 8}, offsets[] = {0, 0};
    context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
  };
  auto factors = [&](float edge, float inside) {
    const float hs_values[4] = {edge, inside, 0, 0};
    context->UpdateSubresource(hs_constants.Get(), 0, nullptr, hs_values, 0, 0);
  };

  // every cell's expectation, and what the target holds, checked pixel by pixel: over a patch, its pixels have their
  // centers' value, the patch's value and place; elsewhere nothing. a triangle's pixels by its long edge are left out
  std::vector<Cell> cells((width / cell) * (height / cell));
  auto check = [&](const char *name, bool quad, bool drawn) {
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "%s: no readback", name))
      return;
    unsigned wrong = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        auto got = (const float *)((const char *)mapped.pData + y * mapped.RowPitch) + 4 * x;
        auto &c = cells[(y / cell) * (width / cell) + x / cell];
        float cx = x % cell + 0.5f, cy = y % cell + 0.5f;
        if (!quad && std::abs(cx + cy - cell) < 1)
          continue;
        bool over = drawn && c.patch >= 0 && (quad || cx + cy < cell);
        float want[4] = {0, 0, 0, 0};
        if (over) {
          want[0] = 1;
          want[1] = 0.01f * (x + 0.5f) + 0.02f * (y + 0.5f) + vs_add;
          want[2] = patch_value(c.patch);
          want[3] = float(c.patch + 1);
        }
        bool match = got[0] == want[0] && std::abs(got[1] - want[1]) <= 1e-3f && got[2] == want[2] && got[3] == want[3];
        if (!match && wrong++ < 3)
          printf("%s: pixel %u,%u has %g %g %g %g, want %g %g %g %g\n", name, x, y, got[0], got[1], got[2], got[3],
                 want[0], want[1], want[2], want[3]);
      }
    context->Unmap(staging.Get(), 0);
    expect(!wrong, "%s: %u wrong pixels", name, wrong);
  };

  // each configuration in one pass: a direct draw of two patches, an instanced draw of two through 16-bit indices,
  // an indirect draw of one, and an instanced draw through 32-bit indices
  // instance i is on row i
  const float instance_offsets[] = {0, 0, 0, float(cell), 0, float(2 * cell), 0, float(3 * cell), 0, float(4 * cell)};
  auto instances = vertex_buffer(std::vector<float>(std::begin(instance_offsets), std::end(instance_offsets)));
  CHECK(instances ? S_OK : E_FAIL);
  unsigned frames = 0;
  for (auto &domain : domains)
    for (auto partitioning : partitionings)
      for (bool clockwise : {true, false}) {
        std::string name = std::string(domain.name) + ", " + partitioning + (clockwise ? ", clockwise" : ", counter-clockwise");
        step("%s", name.c_str());
        Shaders shaders;
        if (!expect(make(domain, partitioning, clockwise ? "triangle_cw" : "triangle_ccw", shaders), "%s: no shaders",
                    name.c_str()))
          continue;
        bind(shaders, domain.quad);
        // factors between whole ones: integer partitioning rounds them up, pow2 to a power of two
        factors(3.5f, 5.25f);
        UINT n = domain.quad ? 4 : 3;
        std::fill(cells.begin(), cells.end(), Cell{});
        // the vertices, a patch for each cell of the first row; the index buffers take them in rows of their own
        std::vector<float> vertices;
        for (UINT column = 0; column < width / cell; column++) {
          float pts[4][2];
          corners(domain.quad, float(column * cell), 0, pts);
          for (UINT k = 0; k < n; k++)
            vertices.insert(vertices.end(), {pts[k][0], pts[k][1]});
        }
        auto vb = vertex_buffer(vertices);
        if (!expect(bool(vb), "%s: no vertex buffer", name.c_str()))
          continue;
        set_vertices(vb.Get(), instances.Get());
        const float clear[4] = {};
        context->ClearRenderTargetView(rtv.Get(), clear);
        // direct: columns 0 and 1 of row 0, as patches 0 and 1
        context->Draw(2 * n, 0);
        cells[0].patch = 0;
        cells[1].patch = 1;
        // 16-bit indices: the patches of columns 3 and 4, from index 2 and base vertex 3n - 1, in instances 1 and 2,
        // whose offsets put them on rows 1 and 2
        std::vector<uint16_t> indices16 = {7, 7};
        for (UINT k = 0; k < 2 * n; k++)
          indices16.push_back(uint16_t(1 + k));
        D3D11_BUFFER_DESC index_desc{(UINT)(indices16.size() * 2), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
        D3D11_SUBRESOURCE_DATA index_data{indices16.data()};
        ComPtr<ID3D11Buffer> ib16, ib32;
        CHECK(device->CreateBuffer(&index_desc, &index_data, &ib16));
        context->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->DrawIndexedInstanced(2 * n, 2, 2, 3 * n - 1, 1);
        for (UINT row : {1u, 2u}) {
          cells[row * (width / cell) + 3].patch = 0;
          cells[row * (width / cell) + 4].patch = 1;
        }
        // indirect, which the mesh emulation draws: column 6 of row 0, as patch 0
        const D3D11_DRAW_INSTANCED_INDIRECT_ARGS indirect_args{n, 1, 6 * n, 0};
        D3D11_BUFFER_DESC arguments_desc{sizeof(indirect_args), D3D11_USAGE_DEFAULT, 0, 0,
                                         D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
        D3D11_SUBRESOURCE_DATA arguments_data{&indirect_args};
        ComPtr<ID3D11Buffer> arguments;
        CHECK(device->CreateBuffer(&arguments_desc, &arguments_data, &arguments));
        context->DrawInstancedIndirect(arguments.Get(), 0);
        cells[6].patch = 0;
        // 32-bit indices: the patches of columns 8, 9 and 10 in instance 4, on row 4
        std::vector<uint32_t> indices32;
        for (UINT k = 0; k < 3 * n; k++)
          indices32.push_back(8 * n + k);
        index_desc.ByteWidth = UINT(indices32.size() * 4);
        index_data.pSysMem = indices32.data();
        CHECK(device->CreateBuffer(&index_desc, &index_data, &ib32));
        context->IASetIndexBuffer(ib32.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->DrawIndexedInstanced(3 * n, 1, 0, 0, 4);
        for (UINT k = 0; k < 3; k++)
          cells[4 * (width / cell) + 8 + k].patch = int(k);
        check(name.c_str(), domain.quad, clockwise);
        frames++;
      }

  // a large draw: every cell a quad patch of the factor, timed on the GPU
  for (float factor : {8.0f, 64.0f}) {
    if (!timing)
      break;
    step("timing, factor %g", factor);
    Shaders shaders;
    CHECK(make(domains[1], "integer", "triangle_cw", shaders) ? S_OK : E_FAIL);
    bind(shaders, true);
    factors(factor, factor);
    std::vector<float> vertices;
    for (UINT row = 0; row < height / cell; row++)
      for (UINT column = 0; column < width / cell; column++) {
        float pts[4][2];
        corners(true, float(column * cell), float(row * cell), pts);
        for (auto &p : pts)
          vertices.insert(vertices.end(), {p[0], p[1]});
      }
    auto vb = vertex_buffer(vertices);
    CHECK(vb ? S_OK : E_FAIL);
    const float no_offsets[8] = {};
    auto zero_instances = vertex_buffer(std::vector<float>(std::begin(no_offsets), std::end(no_offsets)));
    set_vertices(vb.Get(), zero_instances.Get());
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT}, stamp_desc{D3D11_QUERY_TIMESTAMP};
    const UINT patches = (width / cell) * (height / cell), repeats = 8, samples = 15;
    std::vector<double> times;
    for (UINT sample = 0; sample < samples + 3; sample++) {
      ComPtr<ID3D11Query> disjoint, start, end;
      CHECK(device->CreateQuery(&disjoint_desc, &disjoint));
      CHECK(device->CreateQuery(&stamp_desc, &start));
      CHECK(device->CreateQuery(&stamp_desc, &end));
      const float clear[4] = {};
      context->ClearRenderTargetView(rtv.Get(), clear);
      context->Begin(disjoint.Get());
      context->End(start.Get());
      for (UINT r = 0; r < repeats; r++)
        context->DrawInstanced(4 * patches, 4, 0, 0);
      context->End(end.Get());
      context->End(disjoint.Get());
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
      UINT64 t0 = 0, t1 = 0;
      while (context->GetData(disjoint.Get(), &frequency, sizeof(frequency), 0) == S_FALSE)
        Sleep(0);
      while (context->GetData(start.Get(), &t0, sizeof(t0), 0) == S_FALSE)
        Sleep(0);
      while (context->GetData(end.Get(), &t1, sizeof(t1), 0) == S_FALSE)
        Sleep(0);
      // the first runs warm the pipelines and clocks
      if (sample >= 3 && !frequency.Disjoint && frequency.Frequency)
        times.push_back(double(t1 - t0) * 1000.0 / double(frequency.Frequency));
    }
    std::sort(times.begin(), times.end());
    if (expect(!times.empty(), "no timestamps"))
      printf(
          "timing: %u draws of %u quad patches of factor %g x 4 instances: median %.3f ms, min %.3f, max %.3f (%zu "
          "samples)\n",
          repeats, patches, factor, times[times.size() / 2], times.front(), times.back(), times.size()
      );
  }
  printf("drew %u passes\n", frames);
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 1)
    return child(!strcmp(argv[1], "timed"));

  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  char config[4096] = {};
  GetEnvironmentVariableA("MULLION_CONFIG", config, sizeof(config));
  const std::string inherited = config;
  // the native draws of a run: a direct and two indexed draws in each of the 8 passes of an integer or pow2
  // partitioning (the timed run is a process of its own, without the trace)
  const unsigned native_draws = 2 * 2 * 2 * 3;
  for (bool native : {true, false}) {
    step("a run with d3d11.nativeTessellation = %s", native ? "True" : "False");
    CHECK(temporary_directory(temporary, "tes", directory) ? S_OK : E_FAIL);
    std::string setting = inherited + (inherited.empty() ? "" : ";") +
                          (native ? "d3d11.nativeTessellation=True" : "d3d11.nativeTessellation=False");
    SetEnvironmentVariableA("MULLION_CONFIG", setting.c_str());
    SetEnvironmentVariableA("MULLION_LOG_PATH", directory);
    SetEnvironmentVariableA("MULLION_LOG_LEVEL", "info");
    SetEnvironmentVariableA("MULLION_TRACE_ENCODERS", "1");
    for (bool timed : {false, true}) {
      // the trace of the timed run is its own
      if (timed) {
        SetEnvironmentVariableA("MULLION_TRACE_ENCODERS", nullptr);
        SetEnvironmentVariableA("MULLION_LOG_LEVEL", nullptr);
      }
      std::string command = std::string("\"") + exe + "\" " + (timed ? "timed" : "child");
      STARTUPINFOA startup{sizeof(startup)};
      PROCESS_INFORMATION process{};
      CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)
                ? S_OK : E_FAIL);
      auto waited = WaitForSingleObject(process.hProcess, 300000);
      if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 60000);
      }
      expect(waited == WAIT_OBJECT_0, "child timed out");
      DWORD status = 1;
      GetExitCodeProcess(process.hProcess, &status);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      expect(status == 0, "%s run failed with %lu", timed ? "the timed" : "the", status);
      if (timed)
        continue;
      // the hull kernels' encoders and the draws they ran
      unsigned hull_passes = 0, hull_draws = 0;
      for (auto &entry : std::filesystem::directory_iterator(directory)) {
        std::ifstream file(entry.path());
        for (std::string line; std::getline(file, line);) {
          const std::string prefix = "info:  encoder hull ";
          if (!line.starts_with(prefix))
            continue;
          unsigned long long seq;
          unsigned index, w, h, draws, geometry, tessellation;
          if (expect(sscanf(line.c_str() + prefix.size(), "%llu.%u: %ux%u, %u draws (%u geometry, %u tessellation)", &seq,
                            &index, &w, &h, &draws, &geometry, &tessellation) == 7,
                     "invalid trace: %s", line.c_str())) {
            hull_passes++;
            hull_draws += draws;
          }
        }
      }
      if (native)
        expect(hull_draws == native_draws, "%u draws ran hull kernels, want %u", hull_draws, native_draws);
      else
        expect(!hull_passes, "%u passes ran hull kernels with the setting off", hull_passes);
      printf("%s: %u hull kernels in %u passes\n", native ? "native" : "mesh", hull_draws, hull_passes);
    }
    std::filesystem::remove_all(directory);
  }
  SetEnvironmentVariableA("MULLION_CONFIG", inherited.empty() ? nullptr : inherited.c_str());
  return verdict();
}
