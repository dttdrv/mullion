// contract: an indexed draw without an index buffer reads every index as zero
// and draws vertex BaseVertexLocation. "If a Vertex Buffer or Index Buffer is
// read by the Input Assembler, but the slot being read has no Buffer bound, the
// result of the read is 0 for all expected components." (D3D11.3 functional
// specification, 8.20).
#define INITGUID
#include "d3d11_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include <algorithm>
#include <limits>

static const char hlsl[] = R"hlsl(
struct V {
  float4 pos : SV_Position;
  nointerpolation float value : VALUE;
#if CULL
  float cull : SV_CullDistance;
#endif
};
V vs(float x : POSITION, float2 data : INSTANCE, uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  V v;
  v.pos = float4(x, data.x, 0, 1);
  v.value = (vertex + 1) * (instance + 1) + data.y;
#if CULL
  v.cull = 1;
#endif
  return v;
}
float ps(V v) : SV_Target { return v.value; }
float ps_primitive(uint primitive : SV_PrimitiveID) : SV_Target { return primitive + 1; }
)hlsl";

int
main() {
  step("create the device and shaders");
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  auto cull_code = compile(hlsl, "vs", "vs", {"CULL=1"});
  auto primitive_code = compile(hlsl, "ps_primitive", "ps");
  if (!expect(vs_code && ps_code && cull_code && primitive_code, "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11VertexShader> vs, vs_cull;
  ComPtr<ID3D11PixelShader> ps, ps_primitive;
  ComPtr<ID3D11InputLayout> layout;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreateVertexShader(cull_code->GetBufferPointer(), cull_code->GetBufferSize(), nullptr, &vs_cull));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreatePixelShader(
      primitive_code->GetBufferPointer(), primitive_code->GetBufferSize(), nullptr, &ps_primitive
  ));
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"INSTANCE", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}
  };
  CHECK(device->CreateInputLayout(
      elements, std::size(elements), vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout
  ));
  const UINT width = 3, instance_count = 3, first_instance = 2, height = first_instance + instance_count;
  float positions[width], instances[height][2];
  for (UINT x = 0; x < width; x++)
    positions[x] = (x + 0.5f) / width * 2 - 1;
  for (UINT y = 0; y < height; y++) {
    instances[y][0] = 1 - (y + 0.5f) / height * 2;
    instances[y][1] = float(y + 1);
  }
  ComPtr<ID3D11Buffer> vertices, instance_data;
  D3D11_BUFFER_DESC vertex_desc{sizeof(positions), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA vertex_initial{positions}, instance_initial{instances};
  CHECK(device->CreateBuffer(&vertex_desc, &vertex_initial, &vertices));
  vertex_desc.ByteWidth = sizeof(instances);
  CHECK(device->CreateBuffer(&vertex_desc, &instance_initial, &instance_data));

  const UINT start_index = 4, offset_indices = 3;
  std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> draws = {
      {0, 1, 0, 0, 0}, {1, 1, 0, 0, 0}, {3, instance_count, 0, 0, first_instance}, {1, 0, 0, 0, 0}
  };
  UINT small_count = 0, point_group = 0;
  for (auto code : {vs_code.Get(), cull_code.Get()}) {
    ComPtr<ID3D11ShaderReflection> reflection;
    CHECK(D3DReflect(
        code->GetBufferPointer(), code->GetBufferSize(), IID_ID3D11ShaderReflection,
        reinterpret_cast<void **>(reflection.GetAddressOf())
    ));
    D3D11_SHADER_DESC shader_desc{};
    CHECK(reflection->GetDesc(&shader_desc));
    UINT registers = 0;
    for (UINT i = 0; i < shader_desc.OutputParameters; i++) {
      D3D11_SIGNATURE_PARAMETER_DESC output{};
      CHECK(reflection->GetOutputParameterDesc(i, &output));
      registers = std::max(registers, output.Register + 1);
    }
    const UINT group = SM50GeometryWarp(1, false, registers).vertices;
    if (!expect(group > 1, "no room for point primitives"))
      return verdict();
    if (code == vs_code.Get())
      point_group = group;
    for (UINT count : {group - 1, group, group + 1, 2 * group + 1})
      draws.push_back({count, instance_count, start_index, 0, first_instance});
    small_count = std::max(small_count, 2 * group + 1);
  }
  auto large_draws = draws;
  const UINT index_range = UINT(std::numeric_limits<UINT16>::max()) + 1;
  for (UINT count = 2 * point_group; count <= index_range; count *= 2)
    for (UINT boundary : {count - 1, count, count + 1})
      large_draws.push_back({boundary, instance_count, start_index, 0, first_instance});
  const UINT indices = std::max(small_count, index_range + 1) + start_index + offset_indices;
  std::vector<UINT16> words16(indices, 1);
  std::vector<UINT> words32(indices, 1);
  ComPtr<ID3D11Buffer> ib16, ib32, arguments;
  D3D11_BUFFER_DESC index_desc{
      UINT(words16.size() * sizeof(words16[0])), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER
  };
  D3D11_SUBRESOURCE_DATA index_initial{words16.data()};
  CHECK(device->CreateBuffer(&index_desc, &index_initial, &ib16));
  index_desc.ByteWidth = words32.size() * sizeof(words32[0]);
  index_initial.pSysMem = words32.data();
  CHECK(device->CreateBuffer(&index_desc, &index_initial, &ib32));
  D3D11_BUFFER_DESC argument_desc{
      2 * sizeof(draws[0]), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
  };
  CHECK(device->CreateBuffer(&argument_desc, nullptr, &arguments));

  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  D3D11_TEXTURE2D_DESC texture_desc{
      width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET
  };
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &staging));
  D3D11_BLEND_DESC blend_desc{};
  blend_desc.RenderTarget[0] = {
      TRUE,
      D3D11_BLEND_ONE,
      D3D11_BLEND_ONE,
      D3D11_BLEND_OP_ADD,
      D3D11_BLEND_ONE,
      D3D11_BLEND_ONE,
      D3D11_BLEND_OP_ADD,
      D3D11_COLOR_WRITE_ENABLE_ALL
  };
  ComPtr<ID3D11BlendState> blend;
  CHECK(device->CreateBlendState(&blend_desc, &blend));

  const char *calls[] = {"DrawIndexed", "DrawIndexedInstanced", "DrawIndexedInstancedIndirect"};
  struct Binding {
    ID3D11Buffer *buffer;
    DXGI_FORMAT format;
    const char *name;
  };
  const Binding bindings[] = {
      {nullptr, DXGI_FORMAT_UNKNOWN, "never bound"},
      {ib16.Get(), DXGI_FORMAT_R16_UINT, "bound 16-bit"},
      {nullptr, DXGI_FORMAT_R16_UINT, "unbound 16-bit"},
      {ib32.Get(), DXGI_FORMAT_R32_UINT, "bound 32-bit"},
      {nullptr, DXGI_FORMAT_R32_UINT, "unbound 32-bit"}
  };
  for (auto context : {immediate.Get(), deferred.Get()}) {
    context->IASetInputLayout(layout.Get());
    ID3D11Buffer *buffers[] = {vertices.Get(), instance_data.Get()};
    const UINT strides[] = {sizeof(positions[0]), sizeof(instances[0])}, offsets[] = {0, 0};
    context->IASetVertexBuffers(0, std::size(buffers), buffers, strides, offsets);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->OMSetBlendState(blend.Get(), nullptr, ~0u);
    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    // "PrimitiveID starts at 0 for the first primitive generated by a Draw*() call, and increments for each
    // subsequent primitive." it "resets to its starting value whenever a new instance begins" (D3D11.3 8.17).
    for (UINT mode : {0, 1, 2}) {
      if (mode)
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
      context->VSSetShader(mode == 2 ? vs_cull.Get() : vs.Get(), nullptr, 0);
      context->PSSetShader(mode == 1 ? ps_primitive.Get() : ps.Get(), nullptr, 0);
      for (auto &binding : bindings)
        for (INT base : {0, 1})
          for (UINT call = 0; call < std::size(calls); call++)
            for (auto draw : mode ? draws : large_draws) {
              draw.BaseVertexLocation = base;
              if (!call) {
                draw.InstanceCount = 1;
                draw.StartInstanceLocation = 0;
              }
              const UINT index_size = binding.format == DXGI_FORMAT_R32_UINT ? sizeof(UINT) : sizeof(UINT16);
              if (binding.format != DXGI_FORMAT_UNKNOWN)
                context->IASetIndexBuffer(
                    binding.buffer, binding.format, draw.StartIndexLocation ? offset_indices * index_size : 0
                );
              for (auto topology :
                   {D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
                    D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP}) {
                if ((binding.buffer || draw.IndexCountPerInstance > small_count) &&
                    topology != D3D11_PRIMITIVE_TOPOLOGY_POINTLIST)
                  continue;
                step(
                    "%s, mode %u, %s, %s, topology %u, count %u, base %d, start %u, "
                    "instances %u from %u",
                    context == immediate.Get() ? "immediate" : "deferred", mode, binding.name, calls[call], topology,
                    draw.IndexCountPerInstance, base, draw.StartIndexLocation, draw.InstanceCount,
                    draw.StartInstanceLocation
                );
                const float clear[4] = {};
                context->ClearRenderTargetView(rtv.Get(), clear);
                context->IASetPrimitiveTopology(topology);
                if (call == 0)
                  context->DrawIndexed(draw.IndexCountPerInstance, draw.StartIndexLocation, base);
                else if (call == 1)
                  context->DrawIndexedInstanced(
                      draw.IndexCountPerInstance, draw.InstanceCount, draw.StartIndexLocation, base,
                      draw.StartInstanceLocation
                  );
                else {
                  const D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS records[] = {{}, draw};
                  context->UpdateSubresource(arguments.Get(), 0, nullptr, records, 0, 0);
                  context->DrawIndexedInstancedIndirect(arguments.Get(), sizeof(records[0]));
                }
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
                if (mode != 1)
                  context->Draw(1, base);
                context->CopyResource(staging.Get(), target.Get());
                if (context == deferred.Get()) {
                  ComPtr<ID3D11CommandList> list;
                  CHECK(context->FinishCommandList(TRUE, &list));
                  immediate->ExecuteCommandList(list.Get(), TRUE);
                }
                D3D11_MAPPED_SUBRESOURCE mapped;
                CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                for (UINT y = 0; y < height; y++)
                  for (UINT x = 0; x < width; x++) {
                    const UINT index = binding.buffer ? 1 : 0, instance = y - draw.StartInstanceLocation;
                    float want =
                        topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST && x == UINT(base) + index &&
                                y >= draw.StartInstanceLocation && instance < draw.InstanceCount
                            ? mode == 1 ? draw.IndexCountPerInstance * (draw.IndexCountPerInstance + 1) / 2
                                        : draw.IndexCountPerInstance * ((index + 1) * (instance + 1) + instances[y][1])
                            : 0;
                    if (mode != 1 && x == UINT(base) && y == 0)
                      want += 1 + instances[0][1];
                    const float got = ((const float *)((const char *)mapped.pData + y * mapped.RowPitch))[x];
                    expect(got == want, "pixel %u,%u is %g, want %g", x, y, got, want);
                  }
                immediate->Unmap(staging.Get(), 0);
              }
            }
    }
  }
  return verdict();
}
