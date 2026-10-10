// contract: raw reads return zero for each component past the view, and structured indices cannot wrap into it.
// "Out of bounds addressing on u#/t# of any given 32-bit component returns 0 for that component." (D3D11.3 22.4.10,
// 22.4.12). the generated matrix also checks in-range SRV, UAV, coherent and threadgroup reads against input bytes.
#include "d3d11_test.hpp"
#include "../buffer_loads.hpp"

int
main() {
  using namespace buffer_loads;
  std::vector<Case> cases;
  std::vector<ComPtr<ID3DBlob>> shaders;
  for (UINT width = 1; width <= components; width++)
    for (UINT padding : {0u, components}) {
      cases.emplace_back(width + padding, sizeof(UINT) * CHAR_BIT, width, padding, D3D11_RAW_UAV_SRV_BYTE_ALIGNMENT);
      shaders.push_back(compile(hlsl, "cs", "cs", cases.back().defines()));
      if (!shaders.back()) {
        printf("failed: HLSL did not compile\n");
        return 1;
      }
    }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  for (size_t variant = 0; variant < cases.size(); variant++) {
    auto &test = cases[variant];
    step("seed %u, width %u, padding %u, first %u, count %u", test.seed, test.width, test.padding, test.first,
         test.count);
    ComPtr<ID3D11ComputeShader> shader;
    CHECK(device->CreateComputeShader(shaders[variant]->GetBufferPointer(), shaders[variant]->GetBufferSize(), nullptr,
                                      &shader));
    ComPtr<ID3D11Buffer> sources[6], addresses, result;
    ComPtr<ID3D11ShaderResourceView> srvs[3];
    ComPtr<ID3D11UnorderedAccessView> uavs[5];
    for (UINT kind = 0; kind < std::size(sources); kind++) {
      const bool structured = kind % 2;
      D3D11_BUFFER_DESC desc{UINT(test.data.size()),
                             D3D11_USAGE_DEFAULT,
                             kind < 2 ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_UNORDERED_ACCESS,
                             0,
                             structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                        : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                             structured ? test.stride : 0};
      D3D11_SUBRESOURCE_DATA data{test.data.data()};
      CHECK(device->CreateBuffer(&desc, &data, &sources[kind]));
      const UINT first = structured ? test.first : test.raw_first;
      const UINT count = structured ? test.count : test.raw_bytes / sizeof(UINT);
      const auto format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
      if (kind < 2) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{format, D3D11_SRV_DIMENSION_BUFFEREX};
        view.BufferEx = {first, count, structured ? 0u : D3D11_BUFFEREX_SRV_FLAG_RAW};
        CHECK(device->CreateShaderResourceView(sources[kind].Get(), &view, &srvs[kind]));
      } else {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{format, D3D11_UAV_DIMENSION_BUFFER};
        view.Buffer = {first, count, structured ? 0u : D3D11_BUFFER_UAV_FLAG_RAW};
        CHECK(device->CreateUnorderedAccessView(sources[kind].Get(), &view, &uavs[kind - 2]));
      }
    }
    D3D11_BUFFER_DESC addresses_desc{
        UINT(test.addresses.size() * sizeof(Address)), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,         sizeof(Address)};
    D3D11_SUBRESOURCE_DATA addresses_data{test.addresses.data()};
    CHECK(device->CreateBuffer(&addresses_desc, &addresses_data, &addresses));
    CHECK(device->CreateShaderResourceView(addresses.Get(), nullptr, &srvs[2]));
    D3D11_BUFFER_DESC result_desc{
        UINT(test.expected.size() * sizeof(Result)), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,       sizeof(Result)};
    CHECK(device->CreateBuffer(&result_desc, nullptr, &result));
    CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uavs[4]));
    ID3D11ShaderResourceView *reads[] = {srvs[0].Get(), srvs[1].Get(), srvs[2].Get()};
    ID3D11UnorderedAccessView *writes[] = {uavs[0].Get(), uavs[1].Get(), uavs[2].Get(), uavs[3].Get(), uavs[4].Get()};
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, std::size(reads), reads);
    context->CSSetUnorderedAccessViews(0, std::size(writes), writes, nullptr);
    context->Dispatch(1, 1, 1);
    auto got = read(device.Get(), context.Get(), result.Get());
    if (expect(got.size() * sizeof(UINT) == test.expected.size() * sizeof(Result), "readback size"))
      test.check(got.data());
  }
  return verdict();
}
