// contract: what a shader reads of a constant buffer slot (D3D11.3 7.5, 5.3.4.3).
// - "Out of bounds access to ConstantBuffers returns 0 in all components. Out of bounds behavior is always with
//   respect to the size of the buffer bound at that slot", whether the index is literal or computed, and
//   "fetching from a ConstantBuffer slot with no Buffer present always returns 0".
// - a buffer bound with a range of constants (*SetConstantBuffers1) "will appear as if it starts at the specified
//   FirstConstant offset ... and has a size defined by NumConstants"; where the range "falls off the underlying
//   resource, accesses to those addresses count as out of bounds reads". binding the buffer again without a range
//   shows all of it.
// - a buffer "larger than 4096 elements in size" bound without a range "appears to the shader as if it is only 4096
//   elements in size" (5.3.4.3.2).
// the buffer's rows are numbered, and a compute shader copies some rows out, each by a computed index and one more by
// a literal one.
#include "d3d11_test.hpp"
#include <d3d11_1.h>

static const char hlsl[] = R"hlsl(
cbuffer Rows : register(b0) { uint4 rows[DECLARED]; };
cbuffer Zero : register(b1) { uint zero; };
RWStructuredBuffer<uint4> copied : register(u0);
[numthreads(1, 1, 1)] void cs() {
  for (uint i = 0; i < COPIED; i++)
    copied[i] = rows[i + zero];
  copied[COPIED] = rows[LITERAL];
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11DeviceContext1> context1;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  CHECK(context.As(&context1));
  // a range is a multiple of 16 constants; the shader declares three of them, and the buffer has four
  const UINT range = 16, declared = 3 * range, literal = 2 * range + 5, rows = 4 * range, mark = 0x100;
  // and a shader that declares all a shader may, for a buffer of twice as many
  const UINT most = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, large = 2 * most;
  ComPtr<ID3D11ComputeShader> cs, wide;
  for (auto [shader, size] : {std::pair{std::addressof(cs), declared}, std::pair{std::addressof(wide), most}}) {
    auto code = compile(hlsl, "cs", "cs", {"DECLARED=" + std::to_string(size), "COPIED=" + std::to_string(declared),
                                           "LITERAL=" + std::to_string(literal)});
    if (!code) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader->GetAddressOf()));
  }
  // row n holds mark + n in every component
  std::vector<UINT> words(large * 4);
  for (UINT i = 0; i < words.size(); i++)
    words[i] = mark + i / 4;
  auto constants = [&](UINT count) {
    D3D11_BUFFER_DESC desc{count * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    D3D11_SUBRESOURCE_DATA data{words.data()};
    ComPtr<ID3D11Buffer> out;
    device->CreateBuffer(&desc, &data, &out);
    return out;
  };
  const UINT few = 2;
  auto whole = constants(rows), small = constants(few), huge = constants(large);
  auto zero = buffer(device.Get(), 16, D3D11_BIND_CONSTANT_BUFFER);
  D3D11_BUFFER_DESC out_desc{(declared + 1) * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
  ComPtr<ID3D11Buffer> out;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateBuffer(&out_desc, nullptr, &out));
  CHECK(device->CreateUnorderedAccessView(out.Get(), nullptr, &uav));
  if (!whole || !small || !huge || !zero) {
    printf("failed: no constant buffers\n");
    return 1;
  }
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetConstantBuffers(1, 1, zero.GetAddressOf());
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

  unsigned wrong = 0, cases = 0;
  // the shader's rows are the buffer's from `first`, as many as `count` and the buffer have; it copies those from
  // `base`
  auto reads = [&](const char *what, UINT buffer_rows, UINT first, UINT count, UINT base = 0) {
    const UINT computed[4] = {base};
    context->UpdateSubresource(zero.Get(), 0, nullptr, computed, 0, 0);
    context->Dispatch(1, 1, 1);
    auto got = read(device.Get(), context.Get(), out.Get());
    cases++;
    unsigned differ = 0;
    for (UINT i = 0; i <= declared; i++) {
      UINT row = i < declared ? base + i : literal, want = row < count && first + row < buffer_rows ? mark + first + row : 0;
      for (UINT c = 0; c < 4; c++)
        if (got[i * 4 + c] != want && differ++ < 2)
          printf("%s: row %u%s is %#x, want %#x\n", what, row, i < declared ? "" : " by a literal index", got[i * 4 + c], want);
    }
    wrong += differ != 0;
  };
  context->CSSetConstantBuffers(0, 1, whole.GetAddressOf());
  reads("a buffer larger than the shader declares", rows, 0, rows);
  context->CSSetConstantBuffers(0, 1, small.GetAddressOf());
  reads("a buffer smaller than the shader declares", few, 0, few);
  // ranges: inside the buffer, off its end, then none again
  const UINT first[] = {range, 3 * range}, count[] = {range, 2 * range};
  for (UINT i = 0; i < std::size(first); i++) {
    context1->CSSetConstantBuffers1(0, 1, whole.GetAddressOf(), &first[i], &count[i]);
    reads(i ? "a range that falls off the buffer" : "a range of the buffer", rows, first[i], count[i]);
  }
  context->CSSetConstantBuffers(0, 1, whole.GetAddressOf());
  reads("the buffer after a range of it", rows, 0, rows);
  // the rows around the last a shader is shown
  context->CSSetShader(wide.Get(), nullptr, 0);
  context->CSSetConstantBuffers(0, 1, huge.GetAddressOf());
  reads("a buffer of more rows than a shader is shown", large, 0, most, most - declared / 2);
  context1->CSSetConstantBuffers1(0, 1, huge.GetAddressOf(), &most, &most);
  reads("the later half of that buffer", large, most, most, most - declared / 2);
  context->CSSetShader(cs.Get(), nullptr, 0);
  ID3D11Buffer *none = nullptr;
  context->CSSetConstantBuffers(0, 1, &none);
  reads("no buffer", 0, 0, 0);

  printf("%s: %u wrong of %u bindings\n", wrong ? "failed" : "passed", wrong, cases);
  return wrong != 0;
}
