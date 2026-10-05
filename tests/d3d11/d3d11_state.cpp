// contract: a context's state is carried by the objects Direct3D 11 carries it with.
// - ID3D11DeviceContext1::SwapDeviceContextState makes a state object's state the context's and gives back the
//   object that holds the state the context had: a new object holds the default state, a draw uses the state
//   that is current, and swapping the first object back brings back what was bound.
// - a deferred context executes command lists while it records: the list runs where it was executed, from the
//   default state; with RestoreContextState the deferred context then has the state it had, and without it the
//   default state. the list a deferred context finishes carries the lists it executed.
// draws write their value where their scissor is, so a target's pixels say which state drew.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps() : SV_Target { return value; }
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps");
  if (!vs || !ps) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext1> context;
  CHECK(device.As(&device1));
  CHECK(immediate.As(&context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  // the columns the draws below write, each with the value after it
  enum { First, Second, FirstAgain, SecondAgain, Inner, Outer, Columns };
  const UINT values[Columns] = {11, 22, 33, 44, 55, 66};
  // the column whose constants each column is drawn with: a state that came back draws with its own
  const UINT drawn_with[Columns] = {First, Second, First, Second, Inner, Outer};
  D3D11_TEXTURE2D_DESC target_desc{Columns, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> view;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &view));
  ComPtr<ID3D11Buffer> constants[Columns];
  for (UINT i = 0; i < Columns; i++)
    constants[i] = buffer(device.Get(), 16, D3D11_BIND_CONSTANT_BUFFER, values[i]);
  ComPtr<ID3D11RasterizerState> scissored;
  D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster.ScissorEnable = raster.DepthClipEnable = TRUE;
  CHECK(device->CreateRasterizerState(&raster, &scissored));
  const D3D11_VIEWPORT viewport{0, 0, (float)Columns, 1, 0, 1};
  // everything a draw of a column's value needs
  auto bind = [&](ID3D11DeviceContext *to, UINT column) {
    const D3D11_RECT rect{(LONG)column, 0, (LONG)column + 1, 1};
    to->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    to->VSSetShader(vertex.Get(), nullptr, 0);
    to->PSSetShader(pixel.Get(), nullptr, 0);
    to->PSSetConstantBuffers(0, 1, constants[column].GetAddressOf());
    to->RSSetState(scissored.Get());
    to->RSSetViewports(1, &viewport);
    to->RSSetScissorRects(1, &rect);
    to->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
  };
  // the column whose constants a context has bound with the pixel shader, or Columns when its state is the default one
  auto bound = [&](ID3D11DeviceContext *of) {
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11Buffer> constant;
    of->PSGetShader(&shader, nullptr, nullptr);
    of->PSGetConstantBuffers(0, 1, &constant);
    if (!shader && !constant)
      return (UINT)Columns;
    auto found = std::find(std::begin(constants), std::end(constants), constant);
    return shader == pixel && found != std::end(constants) ? UINT(found - constants) : ~0u;
  };
  unsigned failures = 0;
  auto expect = [&](bool ok, const char *what, long long got, long long want) {
    if (!ok && failures++ < 16)
      printf("%s: %lld, want %lld\n", what, got, want);
  };
  const float zero[4] = {};
  immediate->ClearRenderTargetView(view.Get(), zero);

  // state objects
  ComPtr<ID3DDeviceContextState> other, first, again;
  D3D_FEATURE_LEVEL chosen;
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &other));
  bind(immediate.Get(), First);
  immediate->Draw(3, 0);
  context->SwapDeviceContextState(other.Get(), &first);
  expect(first && first != other, "the object given for the first state", !!first, 1);
  expect(bound(immediate.Get()) == Columns, "the state of a new object", bound(immediate.Get()), Columns);
  bind(immediate.Get(), Second);
  immediate->Draw(3, 0);
  context->SwapDeviceContextState(first.Get(), &again);
  expect(again == other, "the object given for the second state", again == other, 1);
  expect(bound(immediate.Get()) == First, "the first state, swapped back", bound(immediate.Get()), First);
  // only the scissor changes: the rest of a draw, its constants too, is the state that came back
  const D3D11_RECT first_again{FirstAgain, 0, FirstAgain + 1, 1}, second_again{SecondAgain, 0, SecondAgain + 1, 1};
  immediate->RSSetScissorRects(1, &first_again);
  immediate->Draw(3, 0);
  context->SwapDeviceContextState(other.Get(), nullptr);
  expect(bound(immediate.Get()) == Second, "the second state, swapped back", bound(immediate.Get()), Second);
  immediate->RSSetScissorRects(1, &second_again);
  immediate->Draw(3, 0);
  // the same object again changes nothing
  context->SwapDeviceContextState(other.Get(), &again);
  expect(again == other && bound(immediate.Get()) == Second, "a swap with the current object", bound(immediate.Get()), Second);

  // lists executed by a deferred context
  ComPtr<ID3D11DeviceContext> inner_context, outer_context;
  CHECK(device->CreateDeferredContext(0, &inner_context));
  CHECK(device->CreateDeferredContext(0, &outer_context));
  ComPtr<ID3D11CommandList> inner, outer;
  bind(inner_context.Get(), Inner);
  inner_context->Draw(3, 0);
  CHECK(inner_context->FinishCommandList(FALSE, &inner));
  bind(outer_context.Get(), Outer);
  outer_context->ExecuteCommandList(inner.Get(), TRUE);
  expect(bound(outer_context.Get()) == Outer, "a deferred context's state after a list it restores from", bound(outer_context.Get()), Outer);
  // drawn with the state that was restored, after the list
  outer_context->Draw(3, 0);
  outer_context->ExecuteCommandList(inner.Get(), FALSE);
  expect(bound(outer_context.Get()) == Columns, "a deferred context's state after a list it does not restore from",
         bound(outer_context.Get()), Columns);
  CHECK(outer_context->FinishCommandList(FALSE, &outer));
  // the immediate context still has its own state while the lists are recorded, and loses it to an executed list
  immediate->ExecuteCommandList(outer.Get(), FALSE);
  expect(bound(immediate.Get()) == Columns, "the immediate context's state after a list", bound(immediate.Get()), Columns);

  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));
  immediate->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  const char *const names[Columns] = {"the first state's draw", "the second state's draw", "the first state's draw after it came back",
                                      "the second state's draw after it came back", "the executed list's draw",
                                      "the deferred context's draw after the list"};
  for (UINT i = 0; i < Columns; i++)
    expect(((const UINT *)mapped.pData)[i] == values[drawn_with[i]], names[i], ((const UINT *)mapped.pData)[i], values[drawn_with[i]]);
  printf("%s: %u wrong answers\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
