// contract: a pipeline is made only where the root signature covers each range its shaders declare, as Direct3D 12
// covers one. an array of a set size lies inside one range of the signature: two ranges side by side do not cover
// it. an unbounded array starts inside its range, which may end before the array does, and a root descriptor at
// its first register covers it. a static sampler is one register: an array of several over the static samplers at
// its registers is not covered, an unbounded one from the first is. vkd3d-proton's test_descriptor_range_validation
// and test_static_sampler_dynamic_index expect each of these of Windows.
#include "d3d12_test.hpp"
#include <limits>

static const char ranges_hlsl[] = R"hlsl(
RWStructuredBuffer<uint> sized[2] : register(u1, space0);
RWStructuredBuffer<uint> unsized[] : register(u1, space1);
[numthreads(1, 1, 1)] void cs(uint3 id : SV_GroupID) {
  sized[id.x][0] = 1;
  unsized[id.x][0] = 2;
}
)hlsl";

static const char samplers_hlsl[] = R"hlsl(
Texture2D<float> tex : register(t0);
SamplerState samplers[COUNT] : register(s0, space100);
RWStructuredBuffer<float> result : register(u0);
[numthreads(1, 1, 1)] void cs(uint3 id : SV_GroupID) {
  result[id.x] = tex.SampleLevel(samplers[id.x], float2(0.5, 0.5), 0);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto shader = [&](const auto &hlsl, const std::string &count) -> std::string {
    if (compiler.dxc)
      return compiler.compile(hlsl, "cs", "cs", {"COUNT=" + count});
    const std::string source = "#define COUNT " + count + "\n" + hlsl;
    ComPtr<ID3DBlob> code, errors;
    // D3DCOMPILE Constants: "Directs the compiler to enable unbounded descriptor tables."
    // https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/d3dcompile-constants
    HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "cs", "cs_5_1",
                            D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code, &errors);
    if (FAILED(hr) && errors)
      printf("cs: %.*s\n", int(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer()));
    return FAILED(hr) ? std::string()
                      : kept(std::string(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize()),
                             "cs");
  };
  auto ranges_cs = shader(ranges_hlsl, ""), four_cs = shader(samplers_hlsl, "4"),
       unsized_cs = shader(samplers_hlsl, "");
  if (ranges_cs.empty() || four_cs.empty() || unsized_cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto made = [&](ID3D12RootSignature *rs, const std::string &code, bool want, const char *what) {
    step("%s", what);
    if (!expect(!!rs, "%s: no root signature", what))
      return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs, bytecode(code)};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(want ? hr == S_OK && pso : FAILED(hr) && !pso, "%s: CreateComputePipelineState returned %08lx, want it %s",
           what, hr, want ? "made" : "refused");
  };

  // the table's ranges: two of space 0 for the sized array at u1 and u2, one of space 1 for the unbounded one from
  // u1; and a root descriptor of a space no shader names, until the last case
  const struct {
    UINT first, second;      // the first registers of the two ranges of space 0, two descriptors each
    UINT third, third_count; // the range of space 1
    UINT third_space, root_register, root_space;
    bool want;
    const char *what;
  } cases[] = {
      {0, 2, 0, 2, 1, 0, 10, false, "a sized array over the end of one range and the start of the next"},
      {1, 3, 0, 2, 1, 0, 10, true, "a sized array inside one range"},
      {1, 3, 0, 1, 1, 0, 10, false, "an unbounded array that starts past its range"},
      {1, 3, 1, 1, 1, 0, 10, true, "an unbounded array that starts at a range of one descriptor"},
      {1, 3, 0, 1, 100, 1, 1, true, "an unbounded array at a root descriptor"},
  };
  for (const auto &c : cases) {
    D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, c.first, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, c.second, 0, 2},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, c.third_count, c.third, c.third_space, 4},
    };
    D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
    params[0].DescriptorTable = {UINT(std::size(ranges)), ranges};
    params[1].Descriptor = {c.root_register, c.root_space};
    auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
    made(rs.Get(), ranges_cs, c.want, c.what);
  }

  D3D12_STATIC_SAMPLER_DESC samplers[4]{};
  for (UINT i = 0; i < std::size(samplers); i++) {
    samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
    samplers[i].RegisterSpace = 100;
  }
  D3D12_DESCRIPTOR_RANGE texture{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &texture};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params, UINT(std::size(samplers)), samplers});
  made(rs.Get(), four_cs, false, "a sampler array of four over four static samplers");
  made(rs.Get(), unsized_cs, true, "an unbounded sampler array from the first static sampler");
  return verdict();
}
