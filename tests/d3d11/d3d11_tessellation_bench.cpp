// a benchmark: the GPU time of tessellated draws, for work on the mesh pipeline that tessellates them. each frame is 8
// draws of 128 patches in 4 instances over a 1024x1024 target, a domain shader that displaces its points with a few
// sines and passes a normal and coordinates on; its GPU time is the time between two timestamp queries around it,
// the median of `frames` frames less the median of as many frames without the draws, for each domain, partitioning
// and factor; a frame of a low factor repeats its draws, and its time is divided by the repeats.
//   d3d11_tessellation_bench [frames] [filter] [plain], filter a substring of a configuration's name ("quad integer
//   64"), plain a domain shader that only interpolates, culled every triangle culled, capture no frames
//   before those measured, for a GPU capture of the first command buffers (any together: "plain,culled")
#include "d3d11_test.hpp"
#include <algorithm>
#include <cstdlib>

static const char hlsl[] = R"(
cbuffer Factors : register(b0) { float factor; float patches_x; float time; float pad; };
struct VSOut { float3 p : POS; };
VSOut vs(float3 p : POSITION) { VSOut o; o.p = p; return o; }
struct HSConst {
#if QUAD
  float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor;
#else
  float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor;
#endif
};
HSConst constants(InputPatch<VSOut, CONTROL_POINTS> patch, uint id : SV_PrimitiveID) {
  HSConst c;
#if QUAD
  c.edges[0] = c.edges[1] = c.edges[2] = c.edges[3] = factor;
  c.inside[0] = c.inside[1] = factor;
#else
  c.edges[0] = c.edges[1] = c.edges[2] = factor;
  c.inside = factor;
#endif
  return c;
}
[domain(DOMAIN)]
[partitioning(PARTITIONING)]
[outputtopology("triangle_cw")]
[outputcontrolpoints(CONTROL_POINTS)]
[patchconstantfunc("constants")]
[maxtessfactor(MAX_FACTOR)]
VSOut hs(InputPatch<VSOut, CONTROL_POINTS> patch, uint i : SV_OutputControlPointID) { return patch[i]; }
struct DSOut { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD; };
[domain(DOMAIN)]
#if QUAD
DSOut ds(HSConst c, float2 at : SV_DomainLocation, const OutputPatch<VSOut, CONTROL_POINTS> patch) {
  float3 p = lerp(lerp(patch[0].p, patch[1].p, at.x), lerp(patch[3].p, patch[2].p, at.x), at.y);
#else
DSOut ds(HSConst c, float3 at : SV_DomainLocation, const OutputPatch<VSOut, CONTROL_POINTS> patch) {
  float3 p = at.x * patch[0].p + at.y * patch[1].p + at.z * patch[2].p;
#endif
  DSOut o;
#if PLAIN
  o.position = float4(p.xy * 2 - 1, p.z, 1);
  o.normal = float3(0, 0, 1);
#else
  float h = sin(p.x * 31 + time) * cos(p.y * 17) * 0.01;
  o.position = float4(p.xy * 2 - 1, p.z + h, 1);
  o.normal = normalize(float3(cos(p.x * 31) * 0.3, sin(p.y * 17) * 0.2, 1));
#endif
  o.uv = p.xy;
  return o;
}
float4 ps(DSOut i) : SV_Target { return float4(i.normal * 0.5 + 0.5, i.uv.x); }
)";

int
main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 15;
  const char *filter = argc > 2 && *argv[2] ? argv[2] : nullptr;
  bool plain = argc > 3 && strstr(argv[3], "plain"), culled = argc > 3 && strstr(argv[3], "culled"),
       capture = argc > 3 && strstr(argv[3], "capture");
  // declared=F: the hull shader declares F its largest factor, 64 otherwise
  const char *declares = argc > 3 ? strstr(argv[3], "declared=") : nullptr;
  int declared = declares ? atoi(declares + 9) : 0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  const UINT size = 1024, columns = 16, rows = 8, patches = columns * rows, instances = 4, draws = 8;
  D3D11_TEXTURE2D_DESC target_desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  ComPtr<ID3D11RasterizerState> raster;
  // culled: every triangle culled as it is set up, which leaves the stages before the rasterizer (the patches face away:
  // the domain's clockwise triangles are drawn with y up)
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, culled ? D3D11_CULL_BACK : D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));

  // each patch a tile of the target: a quad's corners, a triangle's three of them
  std::vector<float> quad_points, tri_points;
  for (UINT y = 0; y < rows; y++)
    for (UINT x = 0; x < columns; x++) {
      float x0 = float(x) / columns, x1 = float(x + 1) / columns, y0 = float(y) / rows, y1 = float(y + 1) / rows;
      float corners[4][3] = {{x0, y0, 0.5f}, {x1, y0, 0.5f}, {x1, y1, 0.5f}, {x0, y1, 0.5f}};
      for (auto &c : corners)
        quad_points.insert(quad_points.end(), c, c + 3);
      for (int c : {0, 1, 2})
        tri_points.insert(tri_points.end(), corners[c], corners[c] + 3);
    }
  auto vertex_buffer = [&](std::vector<float> &points) {
    D3D11_BUFFER_DESC desc{UINT(points.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
    D3D11_SUBRESOURCE_DATA data{points.data()};
    ComPtr<ID3D11Buffer> out;
    device->CreateBuffer(&desc, &data, &out);
    return out;
  };
  auto quad_vb = vertex_buffer(quad_points), tri_vb = vertex_buffer(tri_points);
  ComPtr<ID3D11Buffer> constants;
  D3D11_BUFFER_DESC cb_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  CHECK(device->CreateBuffer(&cb_desc, nullptr, &constants));

  D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT}, stamp_desc{D3D11_QUERY_TIMESTAMP};
  printf("%-28s %10s %10s %10s %10s\n", "configuration", "draws ms", "min ms", "max ms", "repeat");
  double total = 0;
  for (bool quad : {false, true})
    for (const char *partitioning : {"integer", "fractional_odd"})
      for (float factor : {4.0f, 8.0f, 16.0f, 32.0f, 64.0f}) {
        char name[64];
        snprintf(name, sizeof(name), "%s %s %g", quad ? "quad" : "tri", partitioning, factor);
        if (filter && !strstr(name, filter))
          continue;
        std::vector<std::string> defines = {quad ? "QUAD=1" : "QUAD=0", quad ? "DOMAIN=\"quad\"" : "DOMAIN=\"tri\"",
                                            std::string("PARTITIONING=\"") + partitioning + "\"",
                                            quad ? "CONTROL_POINTS=4" : "CONTROL_POINTS=3", plain ? "PLAIN=1" : "PLAIN=0",
                                            "MAX_FACTOR=" + std::to_string(declared ? declared : 64)};
        auto vs_code = compile(hlsl, "vs", "vs", defines), hs_code = compile(hlsl, "hs", "hs", defines),
             ds_code = compile(hlsl, "ds", "ds", defines), ps_code = compile(hlsl, "ps", "ps", defines);
        if (!vs_code || !hs_code || !ds_code || !ps_code)
          return 1;
        // dump: the configuration's shaders, as files named after it and the stage, for the converter's command line
        if (argc > 3 && strstr(argv[3], "dump"))
          for (auto [code, stage] : {std::pair{vs_code.Get(), "vs"}, {hs_code.Get(), "hs"}, {ds_code.Get(), "ds"}}) {
            std::string file = std::string(name) + "." + stage + ".dxbc";
            std::replace(file.begin(), file.end(), ' ', '_');
            if (FILE *out = fopen(file.c_str(), "wb")) {
              fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), out);
              fclose(out);
            }
          }
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> ds;
        ComPtr<ID3D11PixelShader> ps;
        CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
        CHECK(device->CreateHullShader(hs_code->GetBufferPointer(), hs_code->GetBufferSize(), nullptr, &hs));
        CHECK(device->CreateDomainShader(ds_code->GetBufferPointer(), ds_code->GetBufferSize(), nullptr, &ds));
        CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
        D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT};
        ComPtr<ID3D11InputLayout> layout;
        CHECK(device->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout));
        float cb[4] = {factor, float(columns), 0, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
        UINT stride = 12, offset = 0;
        auto vb = quad ? quad_vb : tri_vb;
        UINT control_points = quad ? 4 : 3;
        context->IASetPrimitiveTopology(quad ? D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                             : D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        context->IASetInputLayout(layout.Get());
        context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->HSSetShader(hs.Get(), nullptr, 0);
        context->HSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->DSSetShader(ds.Get(), nullptr, 0);
        context->DSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->RSSetState(raster.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        // frames of no draws, then of the draws, each between two timestamps, all recorded before any is read so
        // that the GPU runs them back to back at its working clock: what the timestamps of a frame hold beside its
        // draws the empty frames measure. the first `warm` frames are not counted
        auto measure = [&](UINT count) -> std::vector<double> {
          const int warm = capture ? 0 : 8;
          std::vector<ComPtr<ID3D11Query>> disjoints(warm + frames), begins(warm + frames), ends(warm + frames);
          float clear[4] = {0, 0, 0, 0};
          for (int frame = 0; frame < warm + frames; frame++) {
            if (FAILED(device->CreateQuery(&disjoint_desc, &disjoints[frame])) ||
                FAILED(device->CreateQuery(&stamp_desc, &begins[frame])) ||
                FAILED(device->CreateQuery(&stamp_desc, &ends[frame])))
              return {};
            context->ClearRenderTargetView(rtv.Get(), clear);
            context->Begin(disjoints[frame].Get());
            context->End(begins[frame].Get());
            for (UINT d = 0; d < count; d++)
              context->DrawInstanced(patches * control_points, instances, 0, 0);
            context->End(ends[frame].Get());
            context->End(disjoints[frame].Get());
          }
          context->Flush();
          std::vector<double> times;
          for (int frame = warm; frame < warm + frames; frame++) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
            UINT64 t0, t1;
            while (context->GetData(disjoints[frame].Get(), &dj, sizeof(dj), 0) == S_FALSE)
              Sleep(0);
            while (context->GetData(begins[frame].Get(), &t0, sizeof(t0), 0) == S_FALSE)
              Sleep(0);
            while (context->GetData(ends[frame].Get(), &t1, sizeof(t1), 0) == S_FALSE)
              Sleep(0);
            if (!dj.Disjoint)
              times.push_back(double(t1 - t0) * 1000.0 / double(dj.Frequency));
          }
          std::sort(times.begin(), times.end());
          return times;
        };
        // the GPU's clock falls while the CPU makes the pipelines, and stays low under short frames: a frame has as
        // many times the draws as take it about 3 ms (`repeat`), and 300 ms of such frames, with one waited for while
        // the next runs, raise the clock first. the time is a frame's over `repeat`
        UINT repeat = 1;
        if (!capture) {
          auto one = measure(draws);
          double each = one.empty() ? 1 : one[one.size() / 2];
          repeat = UINT(std::clamp(3.0 / std::max(each, 1e-3), 1.0, 64.0));
          D3D11_QUERY_DESC event_desc{D3D11_QUERY_EVENT};
          ComPtr<ID3D11Query> before;
          for (DWORD start = GetTickCount(); GetTickCount() - start < 300;) {
            ComPtr<ID3D11Query> done;
            CHECK(device->CreateQuery(&event_desc, &done));
            for (UINT d = 0; d < draws * repeat; d++)
              context->DrawInstanced(patches * control_points, instances, 0, 0);
            context->End(done.Get());
            context->Flush();
            BOOL finished;
            while (before && context->GetData(before.Get(), &finished, sizeof(finished), 0) == S_FALSE)
              Sleep(0);
            before = done;
          }
        }
        auto empty = measure(0);
        auto times = measure(draws * repeat);
        if (times.empty() || empty.empty()) {
          printf("%-28s disjoint\n", name);
          continue;
        }
        double median = (times[times.size() / 2] - empty[empty.size() / 2]) / repeat;
        total += median;
        printf("%-28s %10.3f %10.3f %10.3f %10u\n", name, median, times.front() / repeat, times.back() / repeat,
               repeat);
        fflush(stdout);
      }
  printf("total of the draws' medians: %.3f ms\n", total);
  return 0;
}
