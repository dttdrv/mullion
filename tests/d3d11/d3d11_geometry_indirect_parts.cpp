// contract: indirect draws render InstanceCount, "How many Instances to render." (D3D11.3 functional spec, 8.7,
// 8.8). "DrawAuto will draw from the Buffer offset associate with slot zero to the BufferFilledSize associated
// with the Buffer." (8.9). every point streams its vertex and instance IDs in order, including parts after the
// sixteenth; an unwritten sentinel detects excess output. a small mesh budget forces both part origins to advance.
// a draw with no known bound may be omitted once; after completion, its rendered pixels check the grown part count.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { uint2 value : VALUE; };
V vs(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  V v; v.value = uint2(id, instance); return v;
}
struct P { float4 pos : SV_Position; nointerpolation uint2 value : VALUE; };
[maxvertexcount(1)] [instance(GS_INSTANCES)]
void gs(point V v[1], uint geometry_instance : SV_GSInstanceID, inout PointStream<P> stream) {
  if (geometry_instance)
    return;
  P p; p.value = v[0].value;
  p.pos = float4((p.value.x + 0.5) / WIDTH * 2 - 1, 1 - (p.value.y + 0.5) / HEIGHT * 2, 0.5, 1);
  stream.Append(p);
}
uint2 ps(P p) : SV_Target { return p.value + 1; }
)hlsl";

int
main() {
  const UINT width = 64, height = 3;
  const UINT geometry_instances = 32; // D3D11.3 22.3.7: D3D11_GS_MAX_INSTANCE_COUNT
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height),
                                           "GS_INSTANCES=" + std::to_string(geometry_instances)};
  auto vs = compile(hlsl, "vs", "vs", defines), gs = compile(hlsl, "gs", "gs", defines),
       ps = compile(hlsl, "ps", "ps", defines);
  if (!expect(vs && gs && ps, "HLSL did not compile"))
    return verdict();
  CHECK(SetEnvironmentVariableA("MULLION_MAX_MESH_THREADGROUPS",
                                std::to_string(geometry_instances * 1024).c_str()) ? S_OK : E_FAIL);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11GeometryShader> geometry, fill, raster;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  const D3D11_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, 2, 0};
  const UINT stride = 2 * sizeof(UINT);
  CHECK(device->CreateGeometryShaderWithStreamOutput(gs->GetBufferPointer(), gs->GetBufferSize(), &entry, 1, &stride,
                                                    1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &geometry));
  CHECK(device->CreateGeometryShaderWithStreamOutput(vs->GetBufferPointer(), vs->GetBufferSize(), &entry, 1, &stride,
                                                    1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &fill));
  CHECK(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &raster));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  D3D11_TEXTURE2D_DESC texture_desc{width, height, 1, 1, DXGI_FORMAT_R32G32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, staging;
  ComPtr<ID3D11RenderTargetView> view;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &view));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &staging));
  const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
  context->GSSetShader(raster.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  // neither the argument buffer nor any vertex buffer bounds an indirect draw's instance count.
  const UINT words[] = {width, height, 0, 0};
  D3D11_BUFFER_DESC args_desc{sizeof(words), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  D3D11_SUBRESOURCE_DATA args_data{words};
  ComPtr<ID3D11Buffer> args;
  CHECK(device->CreateBuffer(&args_desc, &args_data, &args));
  for (UINT attempt = 0; attempt < 2; attempt++) {
    step("rasterized indirect draw, attempt %u", attempt);
    const float clear[4] = {};
    context->ClearRenderTargetView(view.Get(), clear);
    context->DrawInstancedIndirect(args.Get(), 0);
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < height; y++) {
        auto row = reinterpret_cast<const UINT *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < width; x++)
          if (!expect(row[2 * x] == x + 1 && row[2 * x + 1] == y + 1, "pixel %u,%u: (%u,%u)", x, y,
                      row[2 * x], row[2 * x + 1]))
            break;
      }
    context->Unmap(staging.Get(), 0);
  }
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->PSSetShader(nullptr, nullptr, 0);
  const UINT sentinel = 0xa5a5a5a5;
  for (UINT mode : {0u, 1u, 2u}) {
    const UINT points = mode == 2 ? 2049 : width;
    for (UINT instances : mode == 2 ? std::vector<UINT>{1} : std::vector<UINT>{0, 1, height})
      for (UINT count : {0u, 1u, points - 1, points, points + 1, 2 * points + 1}) {
        step("%s: %u points, %u instances", mode == 0 ? "indirect" : mode == 1 ? "indexed indirect" : "DrawAuto",
             count, instances);
        auto output = buffer(device.Get(), stride * (count * instances + 1), D3D11_BIND_STREAM_OUTPUT, sentinel);
        CHECK(output ? S_OK : E_FAIL);
        const UINT zero = 0;
        if (mode == 2) {
          auto source = buffer(device.Get(), stride * (count + 1), D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER);
          CHECK(source ? S_OK : E_FAIL);
          ID3D11Buffer *target = source.Get();
          context->GSSetShader(fill.Get(), nullptr, 0);
          context->SOSetTargets(1, &target, &zero);
          context->Draw(count, 0);
          context->SOSetTargets(0, nullptr, nullptr);
          context->IASetVertexBuffers(0, 1, &target, &stride, &zero);
          target = output.Get();
          context->GSSetShader(geometry.Get(), nullptr, 0);
          context->SOSetTargets(1, &target, &zero);
          context->DrawAuto();
          target = nullptr;
          context->IASetVertexBuffers(0, 1, &target, &stride, &zero);
        } else {
          // arguments updated after creation must be read at the draw.
          const UINT words[] = {count, instances, 0, 0, 0};
          D3D11_BUFFER_DESC args_desc{sizeof(words), D3D11_USAGE_DEFAULT, 0, 0,
                                     D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
          ComPtr<ID3D11Buffer> arguments;
          CHECK(device->CreateBuffer(&args_desc, nullptr, &arguments));
          context->UpdateSubresource(arguments.Get(), 0, nullptr, words, 0, 0);
          context->GSSetShader(geometry.Get(), nullptr, 0);
          ID3D11Buffer *target = output.Get();
          context->SOSetTargets(1, &target, &zero);
          if (mode == 0) {
            context->DrawInstancedIndirect(arguments.Get(), 0);
          } else {
            std::vector<UINT> indices(count);
            for (UINT i = 0; i < count; i++)
              indices[i] = i;
            D3D11_BUFFER_DESC index_desc{UINT(sizeof(UINT) * (count ? count : 1)), D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_INDEX_BUFFER};
            D3D11_SUBRESOURCE_DATA index_data{count ? indices.data() : &zero};
            ComPtr<ID3D11Buffer> index;
            CHECK(device->CreateBuffer(&index_desc, &index_data, &index));
            context->IASetIndexBuffer(index.Get(), DXGI_FORMAT_R32_UINT, 0);
            context->DrawIndexedInstancedIndirect(arguments.Get(), 0);
          }
        }
        context->SOSetTargets(0, nullptr, nullptr);
        auto got = read(device.Get(), context.Get(), output.Get());
        if (!expect(got.size() == 2 * (count * instances + 1), "readback has %zu words", got.size()))
          continue;
        for (UINT i = 0; i < count * instances; i++)
          if (!expect(got[2 * i] == i % count && got[2 * i + 1] == i / count,
                      "vertex %u: (%u, %u), want (%u, %u)", i, got[2 * i], got[2 * i + 1], i % count, i / count))
            break;
        expect(got[got.size() - 2] == sentinel && got.back() == sentinel, "wrote past the draw");
      }
  }
  return verdict();
}
