// contract: how a draw numbers and fetches its vertices, here through a pipeline that streams them out (which runs as
// a geometry pipeline, whose first stage is built for one way of numbering).
// - "For Draw() and DrawInstanced(), VertexID starts at 0, and it increments for every vertex"; "for DrawIndexed() ...
//   VertexID represents the index value" (D3D11.3 8.16). the vertex's data is the one at StartVertexLocation plus that
//   count, or at BaseVertexLocation plus the index (8.3, 8.5), which is read from the index buffer at its offset in
//   its format, 16 or 32 bits.
// - an index of all ones is the cut: "the strip defined up to the previous index is to be completed, and the next
//   index is a new strip" (8.12); what is left incomplete is discarded (8.13). the draws of lists here start after
//   the cut.
// - draws of each kind follow each other with nothing else set between them, and each is numbered its own way.
// - "any calculated address that would fall out of bounds for a Buffer being accessed results in ... 0 in all
//   non-missing components of the format ... and the default for missing components" (8.19.2), also for an element
//   that is only partly in the buffer and for a buffer bound at an offset; a slot with no buffer reads as 0 in every
//   component: "defaults are not applied to missing channels for this case" (8.20). elements are aligned, as they
//   must be (4.4.6).
// every vertex is streamed out as its VertexID and its data word.
#include "d3d11_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
struct V { uint id : ID; uint data : DATA; };
V vs(uint data : DATA, uint id : SV_VertexID) {
  V o;
  o.id = id;
  o.data = data;
  return o;
}
// the element's first component, and its last, which the format does not have, as W_MARK
V vs_w(uint4 data : DATA, uint id : SV_VertexID) {
  V o;
  o.id = id;
  o.data = data.x + data.w * W_MARK;
  return o;
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  auto vs_code = compile(hlsl, "vs", "vs", {"W_MARK=0"});
  if (!vs_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11GeometryShader> stream;
  ComPtr<ID3D11InputLayout> layout;
  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "ID", 0, 0, 1, 0}, {0, "DATA", 0, 0, 1, 0}};
  const D3D11_INPUT_ELEMENT_DESC elements[] = {{"DATA", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  const UINT record = 2, stride = record * sizeof(UINT);
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_code->GetBufferPointer(), vs_code->GetBufferSize(), entries, 2, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream
  ));
  CHECK(device->CreateInputLayout(elements, 1, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout));

  // the vertices' data: each its place, marked
  const UINT vertices = 16, mark = 0x1000;
  std::vector<UINT> data(vertices);
  for (UINT i = 0; i < vertices; i++)
    data[i] = mark + i;
  D3D11_BUFFER_DESC vb_desc{vertices * sizeof(UINT), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA vb_data{data.data()};
  ComPtr<ID3D11Buffer> vb;
  CHECK(device->CreateBuffer(&vb_desc, &vb_data, &vb));
  // indices in both formats after a first one that the buffer's offset leaves out. the 32-bit ones have an index no
  // 16 bits hold, which a base below zero brings back to a vertex
  const UINT16 cut16 = 0xffff, indices16[] = {9, 5, 1, 7, cut16, 3, 2, 6, 8};
  const UINT cut32 = 0xffffffff, wide = 0x10000, indices32[] = {9, wide + 4, wide + 2, cut32, wide + 6, wide + 1, wide + 3, wide + 5};
  const INT base16 = 3, base32 = -INT(wide);
  auto index_buffer = [&](const void *indices, UINT bytes) {
    D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
    D3D11_SUBRESOURCE_DATA initial{indices};
    ComPtr<ID3D11Buffer> buffer;
    device->CreateBuffer(&desc, &initial, &buffer);
    return buffer;
  };
  auto ib16 = index_buffer(indices16, sizeof(indices16)), ib32 = index_buffer(indices32, sizeof(indices32));
  if (!ib16 || !ib32) {
    printf("failed: no index buffers\n");
    return 1;
  }
  const UINT capacity = 64, sentinel = 0xa5a5a5a5u;
  D3D11_BUFFER_DESC so_desc{capacity * stride, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
  ComPtr<ID3D11Buffer> so;
  CHECK(device->CreateBuffer(&so_desc, nullptr, &so));

  UINT vb_stride = sizeof(UINT), vb_offset = 0;
  context->IASetInputLayout(layout.Get());
  context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &vb_stride, &vb_offset);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->GSSetShader(stream.Get(), nullptr, 0);

  // what a draw's vertices are: their numbers and data, in the order they are streamed out
  using Records = std::vector<UINT>;
  // of indices: the primitives of `corners` vertices each (a strip's share all but one with the one before), cut where
  // an index is all ones
  auto indexed = [&](const std::vector<UINT> &indices, UINT cut, INT base, UINT corners, bool strip) {
    Records out;
    std::vector<UINT> run;
    for (UINT index : indices) {
      if (index == cut) {
        run.clear();
        continue;
      }
      run.push_back(index);
      if (run.size() == corners) {
        for (UINT i : run)
          out.insert(out.end(), {i, data[i + base]});
        run.erase(run.begin(), strip ? run.begin() + 1 : run.end());
      }
    }
    return out;
  };
  unsigned wrong = 0, draws = 0;
  auto streamed = [&](const char *what, const Records &want) -> HRESULT {
    context->SOSetTargets(0, nullptr, nullptr);
    auto got = read(device.Get(), context.Get(), so.Get());
    draws++;
    bool same = got.size() >= want.size() + 1 && std::equal(want.begin(), want.end(), got.begin()) && got[want.size()] == sentinel;
    if (!same && wrong++ < 8) {
      printf("%s: streamed", what);
      for (size_t i = 0; i < want.size() + 2 && i < got.size(); i++)
        printf(" %x", got[i]);
      printf(", want");
      for (UINT w : want)
        printf(" %x", w);
      printf(" and no more\n");
    }
    return S_OK;
  };
  // the stream output buffer, every word of it the sentinel, bound from its start
  auto begin = [&](D3D11_PRIMITIVE_TOPOLOGY topology) {
    std::vector<UINT> fill(capacity * record, sentinel);
    context->UpdateSubresource(so.Get(), 0, nullptr, fill.data(), 0, 0);
    UINT offset = 0;
    context->SOSetTargets(1, so.GetAddressOf(), &offset);
    context->IASetPrimitiveTopology(topology);
  };
  std::vector<UINT> list16(std::begin(indices16) + 1, std::end(indices16)), list32(std::begin(indices32) + 1, std::end(indices32));
  // where each format's indices go on after the cut, and how many there are
  const UINT after16 = std::find(list16.begin(), list16.end(), cut16) - list16.begin() + 1, rest16 = list16.size() - after16;
  const UINT after32 = std::find(list32.begin(), list32.end(), cut32) - list32.begin() + 1, rest32 = list32.size() - after32;
  auto rest = [](const std::vector<UINT> &indices, UINT after) { return std::vector<UINT>(indices.begin() + after, indices.end()); };
  const UINT skipped = 2, counted = 5;
  Records counted_records;
  for (UINT i = 0; i < counted; i++)
    counted_records.insert(counted_records.end(), {i, data[skipped + i]});

  // twice over: every kind of draw after every other, with only the index buffer set between them
  for (int round = 0; round < 2; round++) {
    begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->Draw(counted, skipped);
    CHECK(streamed("points, not indexed", counted_records));

    begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, sizeof(UINT16));
    context->DrawIndexed(rest16, after16, base16);
    CHECK(streamed("points, 16-bit indices", indexed(rest(list16, after16), cut16, base16, 1, false)));

    begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->IASetIndexBuffer(ib32.Get(), DXGI_FORMAT_R32_UINT, sizeof(UINT));
    context->DrawIndexed(rest32, after32, base32);
    CHECK(streamed("points, 32-bit indices", indexed(rest(list32, after32), cut32, base32, 1, false)));

    begin(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    context->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, sizeof(UINT16));
    context->DrawIndexed(list16.size(), 0, base16);
    CHECK(streamed("a line strip, 16-bit indices", indexed(list16, cut16, base16, 2, true)));

    begin(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    context->IASetIndexBuffer(ib32.Get(), DXGI_FORMAT_R32_UINT, sizeof(UINT));
    context->DrawIndexed(list32.size(), 0, base32);
    CHECK(streamed("a line strip, 32-bit indices", indexed(list32, cut32, base32, 2, true)));

    begin(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    context->DrawIndexed(rest32, after32, base32);
    CHECK(streamed("a line list, 32-bit indices", indexed(rest(list32, after32), cut32, base32, 2, false)));

    // every kind into one buffer, one draw after another with only the index buffer set between them
    begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    Records all = counted_records;
    context->Draw(counted, skipped);
    for (auto &kind : {indexed(rest(list16, after16), cut16, base16, 1, false),
                       indexed(rest(list32, after32), cut32, base32, 1, false), counted_records})
      all.insert(all.end(), kind.begin(), kind.end());
    context->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, sizeof(UINT16));
    context->DrawIndexed(rest16, after16, base16);
    context->IASetIndexBuffer(ib32.Get(), DXGI_FORMAT_R32_UINT, sizeof(UINT));
    context->DrawIndexed(rest32, after32, base32);
    context->Draw(counted, skipped);
    CHECK(streamed("points of every kind, one after another", all));
  }
  // past the buffer's end
  {
    const UINT w_mark = 0x100000;
    auto code = compile(hlsl, "vs_w", "vs", {"W_MARK=" + std::to_string(w_mark)});
    if (!code) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    ComPtr<ID3D11VertexShader> vs_w;
    ComPtr<ID3D11GeometryShader> stream_w;
    ComPtr<ID3D11InputLayout> layout_w;
    CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs_w));
    CHECK(device->CreateGeometryShaderWithStreamOutput(
        code->GetBufferPointer(), code->GetBufferSize(), entries, 2, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream_w
    ));
    CHECK(device->CreateInputLayout(elements, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout_w));
    context->IASetInputLayout(layout_w.Get());
    context->VSSetShader(vs_w.Get(), nullptr, 0);
    context->GSSetShader(stream_w.Get(), nullptr, 0);
    const UINT past = 3, half = sizeof(UINT) / 2;
    // a buffer that ends in the middle of its last element (elements stay aligned: D3D11.3 4.4.6)
    D3D11_BUFFER_DESC cut_desc{vb_desc.ByteWidth - half, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
    ComPtr<ID3D11Buffer> cut;
    CHECK(device->CreateBuffer(&cut_desc, &vb_data, &cut));
    // what the vertices of a draw are with a buffer of `bytes` bound at an offset: an element that is all in the
    // buffer is its bytes, with the default in the last component
    auto drawn = [&](UINT bytes, UINT offset, UINT first, UINT count) {
      Records out;
      for (UINT i = 0; i < count; i++) {
        UINT at = offset + (first + i) * sizeof(UINT);
        out.insert(out.end(), {i, (at + sizeof(UINT) <= bytes ? data[at / sizeof(UINT)] : 0) + w_mark});
      }
      return out;
    };
    struct {
      ID3D11Buffer *buffer;
      UINT bytes, offset;
    } bound[] = {{vb.Get(), vb_desc.ByteWidth, 0},
                 {vb.Get(), vb_desc.ByteWidth, UINT((vertices - 2) * sizeof(UINT))},
                 {cut.Get(), cut_desc.ByteWidth, 0}};
    for (auto &b : bound) {
      begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
      context->IASetVertexBuffers(0, 1, &b.buffer, &vb_stride, &b.offset);
      // the last elements that are in the buffer, whole or not, then some past it
      UINT elements = (b.bytes - b.offset + sizeof(UINT) - 1) / sizeof(UINT), first = elements > past ? elements - past : 0;
      context->Draw(elements - first + past, first);
      CHECK(streamed("vertices past the buffer's end", drawn(b.bytes, b.offset, first, elements - first + past)));
    }
    begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &vb_stride, &vb_offset);
    context->IASetIndexBuffer(ib32.Get(), DXGI_FORMAT_R32_UINT, sizeof(UINT));
    // no base brings the wide indices back into the buffer
    Records wide_records;
    for (UINT index : rest(list32, after32))
      wide_records.insert(wide_records.end(), {index, w_mark});
    context->DrawIndexed(rest32, after32, 0);
    CHECK(streamed("indices past the buffer's end", wide_records));
    begin(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11Buffer *none = nullptr;
    context->IASetVertexBuffers(0, 1, &none, &vb_stride, &vb_offset);
    Records unbound;
    for (UINT i = 0; i < past; i++)
      unbound.insert(unbound.end(), {i, 0u});
    context->Draw(past, 0);
    CHECK(streamed("vertices of no buffer", unbound));
  }
  printf("%s: %u wrong of %u draws\n", wrong ? "failed" : "passed", wrong, draws);
  return wrong != 0;
}
