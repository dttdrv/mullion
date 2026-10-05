// contract: while a predicate is set, the operations that honor predication (D3D11.3 20.2: Draw*, Dispatch*,
// ClearRenderTargetView, ClearDepthStencilView, ClearUnorderedAccessView, CopySubresourceRegion, CopyResource,
// CopyStructureCount, UpdateSubresource, GenerateMips, ResolveSubresource) are not performed if the predicate's data
// equals the value given to SetPredication, and are performed otherwise. "state modification operations ... Map ...
// are not affected by the predication". the predicates are an occlusion predicate whose draw was seen (TRUE, 20.4.8),
// one whose draw was not (FALSE), and a stream output overflow predicate whose buffer overflowed (TRUE, 20.4.10).
// each case leaves its own value in its own place; the expectations compare the predicate's data with the value. the
// cases run on the immediate context, and from a command list of a deferred context, whose list also holds
// predicates of its own, of both kinds and both answers: their data exists only once the list runs, and a stream
// output overflow predicate is no hint that may go unanswered (20.4.10).
#include "d3d11_test.hpp"
#include <bit>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; uint column; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps() : SV_Target { return value; }
RWBuffer<uint> o : register(u0);
[numthreads(1, 1, 1)] void cs() { o[column] = value; }
)hlsl";

int
main() {
  enum Kind {
    Draw,
    DrawIndexed,
    DrawInstanced,
    DrawIndexedInstanced,
    DrawInstancedIndirect,
    DrawIndexedInstancedIndirect,
    DrawAuto,
    Dispatch,
    DispatchIndirect,
    ClearTarget,
    ClearDepth,
    ClearUnorderedUint,
    ClearUnorderedFloat,
    CopyRegion,
    CopyAll,
    CopyCount,
    Update,
    Mips,
    Resolve,
    Kinds
  };
  const char *const names[Kinds] = {
      "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced", "DrawInstancedIndirect",
      "DrawIndexedInstancedIndirect", "DrawAuto", "Dispatch", "DispatchIndirect", "ClearRenderTargetView", "ClearDepthStencilView",
      "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat", "CopySubresourceRegion", "CopyResource",
      "CopyStructureCount", "UpdateSubresource", "GenerateMips", "ResolveSubresource"
  };
  enum { Seen, Unseen, Overflowed, Predicates };
  const BOOL data[Predicates] = {TRUE, FALSE, TRUE};
  const char *const predicate_names[Predicates] = {"occlusion seen", "occlusion unseen", "stream output overflowed"};
  // a case is a kind's `c % combos`-th: a predicate and the value it is set with
  const UINT combos = Predicates * 2, cases = Kinds * combos;
  // the cell a draw writes after a skipped region, by the scissor set inside it; then the deferred list's own
  // predicates: occlusion seen and unseen, stream output overflowed and not
  enum { OwnSeen, OwnUnseen, OwnOverflowed, OwnHeld, OwnPredicates };
  const BOOL own_data[OwnPredicates] = {TRUE, FALSE, TRUE, FALSE};
  const char *const own_names[OwnPredicates] = {"seen", "unseen", "overflowed", "not overflowed"};
  const UINT state_cell = cases, own_cells = cases, cells = cases + OwnPredicates;
  // what a whole-resource copy, a structure count, a cleared depth and the depth before hold, and a white pixel
  const UINT marker = 0xc0ffee, count = 7, white = 0xffffffff;
  const float depth_cleared = 0.5f, depth_before = 1;

  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps"), cs = compile(hlsl, "cs", "cs");
  if (!vs || !ps || !cs) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  ComPtr<ID3D11ComputeShader> compute;
  ComPtr<ID3D11GeometryShader> streamed;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  CHECK(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &compute));
  // the triangle's positions streamed out, into a buffer that holds one vertex of the three
  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "SV_Position", 0, 0, 4, 0}};
  const UINT stride = 4 * sizeof(float);
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs->GetBufferPointer(), vs->GetBufferSize(), entries, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &streamed
  ));
  auto too_small = buffer(device.Get(), stride, D3D11_BIND_STREAM_OUTPUT);
  // and into one that holds the triangle, for DrawAuto to draw as many vertices
  auto triangle = buffer(device.Get(), 3 * stride, D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER);

  auto texture = [&](UINT width, UINT array, UINT mips, DXGI_FORMAT format, UINT samples, UINT bind, UINT misc = 0) {
    D3D11_TEXTURE2D_DESC desc{width, 1, mips, array, format, {samples, 0}, D3D11_USAGE_DEFAULT, bind, 0, misc};
    ComPtr<ID3D11Texture2D> out;
    device->CreateTexture2D(&desc, nullptr, &out);
    return out;
  };
  const auto color = DXGI_FORMAT_R8G8B8A8_UNORM;
  // what the cases write: the draws' columns of the target, the dispatches' elements, a slice for each target and
  // depth clear, an element for each unordered clear, copy, count and update, and a texture for each mip chain and
  // resolve; and what they take
  auto target = texture(cells, 1, 1, DXGI_FORMAT_R32_UINT, 1, D3D11_BIND_RENDER_TARGET);
  auto cleared = texture(1, combos, 1, DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_RENDER_TARGET);
  auto depth = texture(1, combos, 1, DXGI_FORMAT_D32_FLOAT, 1, D3D11_BIND_DEPTH_STENCIL);
  auto multisampled = texture(1, 1, 1, color, 4, D3D11_BIND_RENDER_TARGET);
  auto scratch = texture(1, 1, 1, DXGI_FORMAT_R32_UINT, 1, D3D11_BIND_RENDER_TARGET);
  ComPtr<ID3D11Texture2D> chains[combos], resolved[combos];
  ComPtr<ID3D11ShaderResourceView> chain_views[combos];
  ComPtr<ID3D11RenderTargetView> target_view, scratch_view, multisampled_view, cleared_views[combos];
  ComPtr<ID3D11DepthStencilView> depth_views[combos];
  if (!target || !cleared || !depth || !multisampled || !scratch) {
    printf("failed: textures\n");
    return 1;
  }
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &target_view));
  CHECK(device->CreateRenderTargetView(scratch.Get(), nullptr, &scratch_view));
  CHECK(device->CreateRenderTargetView(multisampled.Get(), nullptr, &multisampled_view));
  for (UINT i = 0; i < combos; i++) {
    chains[i] = texture(2, 1, 2, color, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_GENERATE_MIPS);
    resolved[i] = texture(1, 1, 1, color, 1, 0);
    CHECK(device->CreateShaderResourceView(chains[i].Get(), nullptr, &chain_views[i]));
    D3D11_RENDER_TARGET_VIEW_DESC slice{DXGI_FORMAT_R32_FLOAT, D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
    slice.Texture2DArray = {0, i, 1};
    CHECK(device->CreateRenderTargetView(cleared.Get(), &slice, &cleared_views[i]));
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_slice{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2DARRAY};
    depth_slice.Texture2DArray = {0, i, 1};
    CHECK(device->CreateDepthStencilView(depth.Get(), &depth_slice, &depth_views[i]));
  }
  // buffers of 32-bit elements: the dispatches', the unordered clears', the copies', the counts' and the updates'
  auto elements = [&](UINT n, UINT bind) { return buffer(device.Get(), n * 4, bind); };
  auto out = elements(cells, D3D11_BIND_UNORDERED_ACCESS), unordered = elements(cases, D3D11_BIND_UNORDERED_ACCESS),
       unordered_float = elements(cases, D3D11_BIND_UNORDERED_ACCESS), copied = elements(cases, 0),
       counts = elements(cases, 0), updated = elements(cases, 0), marked = buffer(device.Get(), 4, 0, marker);
  ComPtr<ID3D11Buffer> own[combos];
  for (auto &b : own)
    b = buffer(device.Get(), 4, 0);
  std::vector<uint32_t> values(cases), zeros(cells);
  for (UINT c = 0; c < cases; c++)
    values[c] = c + 1;
  D3D11_BUFFER_DESC values_desc{cases * 4, D3D11_USAGE_DEFAULT, 0};
  D3D11_SUBRESOURCE_DATA values_data{values.data()};
  ComPtr<ID3D11Buffer> values_buffer;
  CHECK(device->CreateBuffer(&values_desc, &values_data, &values_buffer));
  ComPtr<ID3D11UnorderedAccessView> out_view, unordered_views[2][Predicates * 2], counter_view;
  D3D11_UNORDERED_ACCESS_VIEW_DESC whole{DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER};
  whole.Buffer = {0, cells};
  CHECK(device->CreateUnorderedAccessView(out.Get(), &whole, &out_view));
  // a counted structured buffer, whose count is set when it is bound
  D3D11_BUFFER_DESC counted_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
  ComPtr<ID3D11Buffer> counted;
  CHECK(device->CreateBuffer(&counted_desc, nullptr, &counted));
  D3D11_UNORDERED_ACCESS_VIEW_DESC counter{DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
  counter.Buffer = {0, 4, D3D11_BUFFER_UAV_FLAG_COUNTER};
  CHECK(device->CreateUnorderedAccessView(counted.Get(), &counter, &counter_view));

  // a triangle's indices, and the arguments of an indirect draw, indexed draw and dispatch
  const uint16_t indices[] = {0, 1, 2};
  const UINT draw_args[] = {3, 1, 0, 0}, indexed_args[] = {3, 1, 0, 0, 0}, dispatch_args[] = {1, 1, 1};
  auto with = [&](const void *bytes, UINT size, UINT bind, UINT misc) {
    D3D11_BUFFER_DESC desc{(size + 15) & ~15u, D3D11_USAGE_DEFAULT, bind, 0, misc};
    std::vector<uint8_t> padded(desc.ByteWidth);
    memcpy(padded.data(), bytes, size);
    D3D11_SUBRESOURCE_DATA init{padded.data()};
    ComPtr<ID3D11Buffer> made;
    device->CreateBuffer(&desc, &init, &made);
    return made;
  };
  auto index_buffer = with(indices, sizeof(indices), D3D11_BIND_INDEX_BUFFER, 0),
       draw_buffer = with(draw_args, sizeof(draw_args), 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS),
       indexed_buffer = with(indexed_args, sizeof(indexed_args), 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS),
       dispatch_buffer = with(dispatch_args, sizeof(dispatch_args), 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS);
  D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constants));

  // the predicates, and the two the deferred list makes for itself
  ComPtr<ID3D11Predicate> predicates[Predicates], own_predicates[OwnPredicates];
  const D3D11_QUERY_DESC occlusion{D3D11_QUERY_OCCLUSION_PREDICATE}, overflow{D3D11_QUERY_SO_OVERFLOW_PREDICATE};
  CHECK(device->CreatePredicate(&occlusion, &predicates[Seen]));
  CHECK(device->CreatePredicate(&occlusion, &predicates[Unseen]));
  CHECK(device->CreatePredicate(&overflow, &predicates[Overflowed]));
  for (UINT i = 0; i < OwnPredicates; i++)
    CHECK(device->CreatePredicate(i < OwnOverflowed ? &occlusion : &overflow, &own_predicates[i]));
  // room for the triangle the list streams out under the predicate that does not overflow
  auto roomy = buffer(device.Get(), 3 * stride, D3D11_BIND_STREAM_OUTPUT);

  const D3D11_VIEWPORT viewport{0, 0, (float)cells, 1, 0, 1};
  const D3D11_RECT nothing{};
  // the pipeline every draw uses, into `to`; a map of the constants, which predication leaves alone
  auto pipeline = [&](ID3D11DeviceContext *context, ID3D11RenderTargetView *to) {
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->CSSetShader(compute.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &to, nullptr);
    context->CSSetUnorderedAccessViews(0, 1, out_view.GetAddressOf(), nullptr);
  };
  auto set = [&](ID3D11DeviceContext *context, UINT value, UINT column) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return;
    const UINT words[4] = {value, column};
    memcpy(mapped.pData, words, sizeof(words));
    context->Unmap(constants.Get(), 0);
  };
  // an occlusion predicate's draw: over the scratch pixel, or with nothing in the scissor
  auto occlude = [&](ID3D11DeviceContext *context, ID3D11Predicate *predicate, bool seen) {
    const D3D11_RECT pixel{0, 0, 1, 1};
    pipeline(context, scratch_view.Get());
    context->RSSetScissorRects(1, seen ? &pixel : &nothing);
    context->Begin(predicate);
    context->Draw(3, 0);
    context->End(predicate);
  };
  ComPtr<ID3D11RasterizerState> scissored;
  D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster.ScissorEnable = raster.DepthClipEnable = TRUE;
  CHECK(device->CreateRasterizerState(&raster, &scissored));
  immediate->RSSetState(scissored.Get());
  occlude(immediate.Get(), predicates[Seen].Get(), true);
  occlude(immediate.Get(), predicates[Unseen].Get(), false);
  const UINT offset = 0;
  immediate->GSSetShader(streamed.Get(), nullptr, 0);
  immediate->SOSetTargets(1, too_small.GetAddressOf(), &offset);
  immediate->Begin(predicates[Overflowed].Get());
  immediate->Draw(3, 0);
  immediate->End(predicates[Overflowed].Get());
  immediate->SOSetTargets(1, triangle.GetAddressOf(), &offset);
  immediate->Draw(3, 0);
  immediate->SOSetTargets(0, nullptr, nullptr);
  // the multisampled pixel is white, and the counted buffer holds `count`
  const float ones[4] = {1, 1, 1, 1}, none[4] = {};
  immediate->ClearRenderTargetView(multisampled_view.Get(), ones);
  immediate->CSSetUnorderedAccessViews(1, 1, counter_view.GetAddressOf(), &count);
  ID3D11UnorderedAccessView *const unbound = nullptr;
  immediate->CSSetUnorderedAccessViews(1, 1, &unbound, nullptr);

  // nothing written, with no predicate set
  auto reset = [&] {
    immediate->SetPredication(nullptr, FALSE);
    immediate->ClearRenderTargetView(target_view.Get(), none);
    for (auto *b : {out.Get(), unordered.Get(), unordered_float.Get(), copied.Get(), counts.Get(), updated.Get()})
      immediate->UpdateSubresource(b, 0, nullptr, zeros.data(), 0, 0);
    for (UINT i = 0; i < combos; i++) {
      const uint32_t texels[2] = {white, white}, black = 0;
      immediate->ClearRenderTargetView(cleared_views[i].Get(), none);
      immediate->ClearDepthStencilView(depth_views[i].Get(), D3D11_CLEAR_DEPTH, depth_before, 0);
      immediate->UpdateSubresource(own[i].Get(), 0, nullptr, zeros.data(), 0, 0);
      immediate->UpdateSubresource(chains[i].Get(), 0, nullptr, texels, 8, 8);
      immediate->UpdateSubresource(chains[i].Get(), 1, nullptr, &black, 4, 4);
      immediate->UpdateSubresource(resolved[i].Get(), 0, nullptr, &black, 4, 4);
    }
  };
  // unordered views of one element each, made once
  for (UINT kind = 0; kind < 2; kind++)
    for (UINT i = 0; i < combos; i++) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC one{kind ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER};
      one.Buffer = {(kind ? ClearUnorderedFloat : ClearUnorderedUint) * combos + i, 1};
      CHECK(device->CreateUnorderedAccessView(kind ? unordered_float.Get() : unordered.Get(), &one, &unordered_views[kind][i]));
    }

  auto record = [&](ID3D11DeviceContext *context) {
    context->RSSetState(scissored.Get());
    pipeline(context, target_view.Get());
    for (UINT c = 0; c < cases; c++) {
      auto kind = Kind(c / combos);
      const UINT mine = c % combos;
      const float value[4] = {float(c + 1)};
      const UINT uint_value[4] = {c + 1};
      const D3D11_RECT column{(LONG)c, 0, (LONG)c + 1, 1};
      const D3D11_BOX element{c * 4, 0, 0, c * 4 + 4, 1, 1};
      context->SetPredication(predicates[mine / 2].Get(), mine % 2);
      set(context, c + 1, c);
      context->RSSetScissorRects(1, &column);
      switch (kind) {
      case Draw:
        context->Draw(3, 0);
        break;
      case DrawIndexed:
        context->DrawIndexed(3, 0, 0);
        break;
      case DrawInstanced:
        context->DrawInstanced(3, 1, 0, 0);
        break;
      case DrawIndexedInstanced:
        context->DrawIndexedInstanced(3, 1, 0, 0, 0);
        break;
      case DrawInstancedIndirect:
        context->DrawInstancedIndirect(draw_buffer.Get(), 0);
        break;
      case DrawIndexedInstancedIndirect:
        context->DrawIndexedInstancedIndirect(indexed_buffer.Get(), 0);
        break;
      case DrawAuto:
        context->IASetVertexBuffers(0, 1, triangle.GetAddressOf(), &stride, &offset);
        context->DrawAuto();
        break;
      case Dispatch:
        context->Dispatch(1, 1, 1);
        break;
      case DispatchIndirect:
        context->DispatchIndirect(dispatch_buffer.Get(), 0);
        break;
      case ClearTarget:
        context->ClearRenderTargetView(cleared_views[mine].Get(), value);
        break;
      case ClearDepth:
        context->ClearDepthStencilView(depth_views[mine].Get(), D3D11_CLEAR_DEPTH, depth_cleared, 0);
        break;
      case ClearUnorderedUint:
        context->ClearUnorderedAccessViewUint(unordered_views[0][mine].Get(), uint_value);
        break;
      case ClearUnorderedFloat:
        context->ClearUnorderedAccessViewFloat(unordered_views[1][mine].Get(), value);
        break;
      case CopyRegion:
        context->CopySubresourceRegion(copied.Get(), 0, c * 4, 0, 0, values_buffer.Get(), 0, &element);
        break;
      case CopyAll:
        context->CopyResource(own[mine].Get(), marked.Get());
        break;
      case CopyCount:
        context->CopyStructureCount(counts.Get(), c * 4, counter_view.Get());
        break;
      case Update:
        context->UpdateSubresource(updated.Get(), 0, &element, uint_value, 0, 0);
        break;
      case Mips:
        context->GenerateMips(chain_views[mine].Get());
        break;
      case Resolve:
        context->ResolveSubresource(resolved[mine].Get(), 0, multisampled.Get(), 0, color);
        break;
      default:
        break;
      }
    }
    // a draw that is skipped, and after it one that the scissor set meanwhile places
    const D3D11_RECT placed{(LONG)state_cell, 0, (LONG)state_cell + 1, 1};
    context->SetPredication(predicates[Seen].Get(), data[Seen]);
    context->RSSetScissorRects(1, &placed);
    set(context, marker, 0);
    context->Draw(3, 0);
    context->SetPredication(nullptr, FALSE);
    context->Draw(3, 0);
  };
  // the values the cases left, in the order of the checks below
  struct Left {
    std::vector<uint32_t> target, out, unordered, unordered_float, copied, counts, updated, cleared, depth, own, mips, resolved;
  };
  auto first_texel = [&](ID3D11Texture2D *of, UINT subresource, uint32_t &texel) {
    D3D11_TEXTURE2D_DESC desc;
    of->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr))
      return hr;
    immediate->CopyResource(staging.Get(), of);
    if (SUCCEEDED(hr = immediate->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapped))) {
      memcpy(&texel, mapped.pData, 4);
      immediate->Unmap(staging.Get(), subresource);
    }
    return hr;
  };
  auto left = [&](Left &l) {
    auto words = [&](ID3D11Buffer *of) { return read(device.Get(), immediate.Get(), of); };
    l = {{}, words(out.Get()), words(unordered.Get()), words(unordered_float.Get()), words(copied.Get()),
         words(counts.Get()), words(updated.Get())};
    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    immediate->CopyResource(staging.Get(), target.Get());
    CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    l.target.assign((const uint32_t *)mapped.pData, (const uint32_t *)mapped.pData + cells);
    immediate->Unmap(staging.Get(), 0);
    for (UINT i = 0; i < combos; i++) {
      uint32_t texel;
      CHECK(first_texel(cleared.Get(), i, texel));
      l.cleared.push_back(texel);
      CHECK(first_texel(depth.Get(), i, texel));
      l.depth.push_back(texel);
      CHECK(first_texel(chains[i].Get(), 1, texel));
      l.mips.push_back(texel);
      CHECK(first_texel(resolved[i].Get(), 0, texel));
      l.resolved.push_back(texel);
      l.own.push_back(words(own[i].Get())[0]);
    }
    return 0;
  };

  unsigned failures = 0;
  auto check = [&](const char *context_name, const Left &l) {
    auto real = [](uint32_t bits) { return std::bit_cast<float>(bits); };
    for (UINT c = 0; c < cases; c++) {
      auto kind = Kind(c / combos);
      const UINT mine = c % combos, predicate = mine / 2;
      bool skip = data[predicate] == BOOL(mine % 2);
      // what the case left, and what a run and a skip leave
      double value, ran = c + 1, skipped = 0;
      switch (kind) {
      case Dispatch:
      case DispatchIndirect:
        value = l.out[c];
        break;
      case ClearTarget:
        value = real(l.cleared[mine]);
        break;
      case ClearDepth:
        value = real(l.depth[mine]);
        ran = depth_cleared;
        skipped = depth_before;
        break;
      case ClearUnorderedUint:
        value = l.unordered[c];
        break;
      case ClearUnorderedFloat:
        value = real(l.unordered_float[c]);
        break;
      case CopyRegion:
        value = l.copied[c];
        break;
      case CopyAll:
        value = l.own[mine];
        ran = marker;
        break;
      case CopyCount:
        value = l.counts[c];
        ran = count;
        break;
      case Update:
        value = l.updated[c];
        break;
      case Mips:
        value = l.mips[mine];
        ran = white;
        break;
      case Resolve:
        value = l.resolved[mine];
        ran = white;
        break;
      default:
        value = l.target[c];
        break;
      }
      double want = skip ? skipped : ran;
      if (value != want && failures++ < 16)
        printf("%s context, %s set with %u, %s (case %u): %g, want %g\n", context_name, predicate_names[predicate],
               mine % 2, names[kind], c, value, want);
    }
    if (l.target[state_cell] != marker && failures++ < 16)
      printf("%s context: the draw after a skipped region left %u where its scissor is, want %u\n", context_name,
             l.target[state_cell], marker);
  };

  Left l;
  reset();
  record(immediate.Get());
  if (left(l))
    return 1;
  check("immediate", l);

  reset();
  // the list's own predicates: what the dispatches under them write shows which were skipped
  const BOOL own_value = TRUE;
  deferred->RSSetState(scissored.Get());
  for (UINT i = 0; i < OwnOverflowed; i++)
    occlude(deferred.Get(), own_predicates[i].Get(), own_data[i]);
  deferred->GSSetShader(streamed.Get(), nullptr, 0);
  for (UINT i = OwnOverflowed; i < OwnPredicates; i++) {
    deferred->SOSetTargets(1, (own_data[i] ? too_small : roomy).GetAddressOf(), &offset);
    deferred->Begin(own_predicates[i].Get());
    deferred->Draw(3, 0);
    deferred->End(own_predicates[i].Get());
  }
  deferred->SOSetTargets(0, nullptr, nullptr);
  pipeline(deferred.Get(), target_view.Get());
  for (UINT i = 0; i < OwnPredicates; i++) {
    deferred->SetPredication(own_predicates[i].Get(), own_value);
    set(deferred.Get(), marker, own_cells + i);
    deferred->Dispatch(1, 1, 1);
  }
  deferred->SetPredication(nullptr, FALSE);
  record(deferred.Get());
  ComPtr<ID3D11CommandList> list;
  CHECK(deferred->FinishCommandList(FALSE, &list));
  immediate->ExecuteCommandList(list.Get(), FALSE);
  if (left(l))
    return 1;
  check("deferred", l);
  for (UINT i = 0; i < OwnPredicates; i++) {
    UINT want = own_data[i] == own_value ? 0 : marker;
    if (l.out[own_cells + i] != want && failures++ < 16)
      printf("deferred context, its own %s predicate: %u, want %u\n", own_names[i], l.out[own_cells + i], want);
  }
  printf("%s: %u wrong of %u cases on two contexts\n", failures ? "failed" : "passed", failures, cases);
  return failures != 0;
}
