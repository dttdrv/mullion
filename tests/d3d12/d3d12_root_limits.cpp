// contract: the root budget counts DWORDs, not parameters. "The maximum size of a root signature is 64 DWORDs."
// tables and constants cost one DWORD, root descriptors two (Microsoft Learn, Root Signature Limits).
// legal unique registers below, at and above that budget distinguish
// parameter count from cost; no shader or GPU execution is needed to validate creation.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits
// root signature 1.1 permits at most one DATA flag; samplers permit no DATA flags, and
// DESCRIPTORS_VOLATILE cannot accompany DATA_STATIC (Root Signature Version 1.1, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1
// static-bounds descriptors "have all of the above static properties" (Resource Binding), so cannot be volatile.
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html
// LLVM's RootSignature-DescriptorTable-AllValidFlagCombinations.ll includes a static-bounds sampler range.
// https://github.com/llvm/llvm-project/pull/142492
// local signatures have "4064 bytes max local root signature footprint"
// (DXR, Local root signatures vs global root signatures).
// https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#local-root-signatures-vs-global-root-signatures
// two bindings that one stage sees cannot name the same register of a space: "Shader register range ... overlaps
// with another shader register range" (DirectXShaderCompiler, lib/DxilRootSignature/DxilRootSignatureValidator.cpp).
#include "d3d12_test.hpp"
#include <bit>
#include <limits>
#include <span>

int main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  for (auto type : {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, D3D12_ROOT_PARAMETER_TYPE_CBV,
                    D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_UAV,
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE})
    for (UINT boundary : {0u, 1u, 2u}) {
      const bool descriptor = type == D3D12_ROOT_PARAMETER_TYPE_CBV || type == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                              type == D3D12_ROOT_PARAMETER_TYPE_UAV;
      const UINT count = D3D12_MAX_ROOT_COST / (descriptor ? 2 : 1) + boundary - 1;
      const UINT cost_per_parameter = type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 1 :
                                      type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? count : 2;
      const UINT parameters = type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? 1 :
                              count;
      const UINT cost = parameters * cost_per_parameter;
      step("parameter type %u, %u parameters costing %u DWORDs", type, parameters, cost);
      std::vector<D3D12_ROOT_PARAMETER> params(parameters);
      std::vector<D3D12_DESCRIPTOR_RANGE> ranges(parameters);
      for (UINT i = 0; i < parameters; i++) {
        auto &p = params[i];
        p.ParameterType = type;
        if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
          p.Constants = {i, 0, count};
        else if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
          ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
          p.DescriptorTable = {1, &ranges[i]};
        } else
          p.Descriptor = {i, 0};
      }
      D3D12_ROOT_SIGNATURE_DESC desc{parameters, params.data()};
      ComPtr<ID3DBlob> blob, errors;
      HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
      if (FAILED(hr) && errors)
        printf("serializer: %.*s\n", int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
      if (cost <= D3D12_MAX_ROOT_COST && !expect(hr == S_OK && blob, "valid serialization: %08lx", hr))
        continue;
      if (FAILED(hr)) {
        expect(cost > D3D12_MAX_ROOT_COST, "legal cost rejected");
        continue;
      }
      ComPtr<ID3D12RootSignature> root;
      hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
      expect(cost <= D3D12_MAX_ROOT_COST ? hr == S_OK && root : hr == E_INVALIDARG && !root,
             "cost %u root creation: %08lx", cost, hr);
    }
  for (auto type : {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                    D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER}) {
    step("version 1.0 table range type %u", type);
    D3D12_DESCRIPTOR_RANGE range{type, 1};
    D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
    parameter.DescriptorTable = {1, &range};
    auto root = root_signature(device.Get(), {1, &parameter});
    expect(bool(root), "legal version 1.0 table was rejected");
  }
  constexpr UINT mixed_overhead = sizeof(D3D12_GPU_VIRTUAL_ADDRESS) / sizeof(UINT) + 1;
  for (auto type : {D3D12_ROOT_PARAMETER_TYPE_CBV, D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_UAV})
    for (UINT constants : {D3D12_MAX_ROOT_COST - mixed_overhead - 1, D3D12_MAX_ROOT_COST - mixed_overhead,
                           D3D12_MAX_ROOT_COST - mixed_overhead + 1,
                           std::numeric_limits<UINT>::max()}) {
      step("mixed root descriptor type %u, %u constants", type, constants);
      D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
      D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {type},
                                      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
      params[0].Constants.Num32BitValues = constants;
      params[1].Descriptor.ShaderRegister = 1;
      params[2].DescriptorTable = {1, &range};
      D3D12_ROOT_SIGNATURE_DESC desc{UINT(std::size(params)), params};
      ComPtr<ID3DBlob> blob, errors;
      HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
      const UINT64 cost = UINT64(constants) + mixed_overhead;
      if (FAILED(hr) && errors)
        printf("serializer: %.*s\n", int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
      if (FAILED(hr)) {
        expect(cost > D3D12_MAX_ROOT_COST, "legal mixed root cost rejected");
        continue;
      }
      ComPtr<ID3D12RootSignature> root;
      hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
      expect(cost <= D3D12_MAX_ROOT_COST ? hr == S_OK && root : hr == E_INVALIDARG && !root,
             "mixed root cost %llu returned %08lx", cost, hr);
    }
  D3D12_FEATURE_DATA_ROOT_SIGNATURE support{D3D_ROOT_SIGNATURE_VERSION_1_1};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &support, sizeof(support)));
  if (support.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1)
    return verdict();
  const D3D12_DESCRIPTOR_RANGE_FLAGS data[] = {D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
                                              D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
                                              D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC};
  const D3D12_ROOT_DESCRIPTOR_FLAGS root_data[] = {D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
                                                  D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
                                                  D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
  auto create = [&](std::span<const D3D12_ROOT_PARAMETER1> parameters, bool valid,
                    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{D3D_ROOT_SIGNATURE_VERSION_1_1};
    desc.Desc_1_1 = {UINT(parameters.size()), parameters.data(), 0, nullptr, flags};
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &errors);
    if (FAILED(hr)) {
      if (valid && errors)
        printf("serializer: %.*s\n", int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
      expect(!valid, "valid flags failed serialization: %08lx", hr);
      return;
    }
    ComPtr<ID3D12RootSignature> root;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    expect(valid ? hr == S_OK && root : hr == E_INVALIDARG && !root,
           "root flag validation: %08lx, expected %s", hr, valid ? "valid" : "invalid");
  };
  for (UINT mask = 0; mask < (1u << std::size(data)); mask++) {
    UINT flags = 0;
    for (UINT i = 0; i < std::size(data); i++)
      if (mask & (1u << i))
        flags |= data[i];
    for (auto type : {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                      D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER})
      for (auto descriptors : {D3D12_DESCRIPTOR_RANGE_FLAG_NONE, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
                               D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS,
                               D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                   D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS)}) {
        const bool is_volatile = descriptors & D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        const bool keeping_bounds = descriptors & D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS;
        bool valid = std::popcount(mask) <= 1 &&
                     !(is_volatile && keeping_bounds) && !(is_volatile && (flags & D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC));
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
          valid &= !mask;
        step("range type %u, descriptor flags %#x, data flags %#x", type, descriptors, flags);
        D3D12_DESCRIPTOR_RANGE1 range{type, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAGS(flags | descriptors)};
        D3D12_ROOT_PARAMETER1 parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
        parameter.DescriptorTable = {1, &range};
        create({&parameter, 1}, valid);
      }
    for (auto type : {D3D12_ROOT_PARAMETER_TYPE_CBV, D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_UAV}) {
      step("root descriptor type %u data flags %#x", type, flags);
      D3D12_ROOT_PARAMETER1 parameter{type};
      for (UINT i = 0; i < std::size(root_data); i++)
        if (mask & (1u << i))
          parameter.Descriptor.Flags |= root_data[i];
      create({&parameter, 1}, std::popcount(mask) <= 1);
    }
  }
  step("unknown root descriptor and range flags");
  D3D12_ROOT_PARAMETER1 parameter{D3D12_ROOT_PARAMETER_TYPE_CBV};
  parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
  create({&parameter, 1}, false);
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  range.Flags = D3D12_DESCRIPTOR_RANGE_FLAGS(std::bit_ceil(
      UINT(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS) + 1));
  parameter = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  parameter.DescriptorTable = {1, &range};
  create({&parameter, 1}, false);
  step("global and local root constant budgets");
  parameter = {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants.Num32BitValues = D3D12_MAX_ROOT_COST + 1;
  create({&parameter, 1}, false);
  // DXR, Local root signatures vs global root signatures: local signatures use the shader-record byte limit.
  // https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#local-root-signatures-vs-global-root-signatures
  constexpr auto local = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
  constexpr UINT local_limit = D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE - D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
  constexpr UINT local_constants = local_limit / sizeof(UINT);
  for (UINT count : {local_constants - 1, local_constants, local_constants + 1}) {
    step("local constants %u", count);
    parameter.Constants.Num32BitValues = count;
    create({&parameter, 1}, count <= local_constants, local);
  }
  for (auto type : {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_ROOT_PARAMETER_TYPE_CBV,
                    D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_UAV}) {
    const UINT local_views = local_limit / (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE
                                              ? sizeof(D3D12_GPU_DESCRIPTOR_HANDLE)
                                              : sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
    for (UINT count : {local_views - 1, local_views, local_views + 1}) {
      step("local parameter type %u count %u", type, count);
      std::vector<D3D12_ROOT_PARAMETER1> parameters(count);
      std::vector<D3D12_DESCRIPTOR_RANGE1> ranges(count);
      for (UINT i = 0; i < count; i++) {
        parameters[i].ParameterType = type;
        if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
          ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i};
          parameters[i].DescriptorTable = {1, &ranges[i]};
        } else
          parameters[i].Descriptor.ShaderRegister = i;
      }
      create(parameters, count <= local_views, local);
    }
  }
  // the descriptor after one DWORD starts at the next descriptor-sized boundary
  // (DXR, Shader table memory initialization).
  constexpr UINT alignment = sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
  constexpr UINT prefix = (sizeof(UINT) + alignment - 1) / alignment * alignment;
  constexpr UINT tail = (local_limit - prefix - sizeof(D3D12_GPU_VIRTUAL_ADDRESS)) / sizeof(UINT);
  D3D12_ROOT_PARAMETER1 mixed[] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                  {D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  mixed[0].Constants.Num32BitValues = 1;
  mixed[1].Descriptor.ShaderRegister = 1;
  mixed[2].Constants.ShaderRegister = 2;
  for (UINT count : {tail - 1, tail, tail + 1}) {
    step("local mixed alignment, tail constants %u", count);
    mixed[2].Constants.Num32BitValues = count;
    create(mixed, count <= tail, local);
  }
  auto constants = [](UINT reg, D3D12_SHADER_VISIBILITY stages) {
    D3D12_ROOT_PARAMETER1 p{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
    p.Constants = {reg, 0, 1};
    p.ShaderVisibility = stages;
    return p;
  };
  auto descriptor = [](D3D12_ROOT_PARAMETER_TYPE type, UINT reg, UINT space, D3D12_SHADER_VISIBILITY stages) {
    D3D12_ROOT_PARAMETER1 p{type};
    p.Descriptor = {reg, space};
    p.ShaderVisibility = stages;
    return p;
  };
  auto table = [](std::span<const D3D12_DESCRIPTOR_RANGE1> ranges, D3D12_SHADER_VISIBILITY stages) {
    D3D12_ROOT_PARAMETER1 p{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
    p.DescriptorTable = {UINT(ranges.size()), ranges.data()};
    p.ShaderVisibility = stages;
    return p;
  };
  const auto all = D3D12_SHADER_VISIBILITY_ALL, vertex = D3D12_SHADER_VISIBILITY_VERTEX,
             pixel = D3D12_SHADER_VISIBILITY_PIXEL;
  const auto cbv = D3D12_ROOT_PARAMETER_TYPE_CBV, srv = D3D12_ROOT_PARAMETER_TYPE_SRV,
             uav = D3D12_ROOT_PARAMETER_TYPE_UAV;
  const D3D12_DESCRIPTOR_RANGE1 four[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0}},
                                unbounded[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0}},
                                twice[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0,
                                            D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 2}};
  // two bindings a stage sees cannot name one register of one space: a visibility meets its own and ALL, constants
  // and constant buffers share the b registers, and an unbounded range has every register from its first
  // (Microsoft's verifier: DirectXShaderCompiler, DxilRootSignatureValidator.cpp, AddRegisterRange)
  const struct {
    D3D12_ROOT_PARAMETER1 first, second;
    bool valid;
    const char *what;
  } pairs[] = {
      {constants(0, all), constants(0, vertex), false, "b0 of every stage and of the vertex stage"},
      {constants(0, pixel), constants(0, vertex), true, "b0 of the pixel stage and of the vertex stage"},
      {constants(0, pixel), constants(0, pixel), false, "b0 of the pixel stage twice"},
      {constants(0, all), descriptor(cbv, 0, 0, all), false, "constants and a constant buffer at b0"},
      {descriptor(cbv, 0, 0, all), descriptor(srv, 0, 0, all), true, "b0 and t0"},
      {descriptor(uav, 0, 0, all), descriptor(uav, 0, 1, all), true, "u0 of two spaces"},
      {descriptor(uav, 0, 0, all), descriptor(uav, 0, 0, pixel), false, "u0 of every stage and of the pixel stage"},
      {table(four, all), descriptor(srv, 3, 0, all), false, "t3 after a range of t0 to t3"},
      {table(four, all), descriptor(srv, 4, 0, all), true, "t4 after a range of t0 to t3"},
      {table(unbounded, all), descriptor(srv, 100000, 0, vertex), false, "a register in an unbounded range"},
      {table(unbounded, all), descriptor(srv, 0, 1, all), true, "another space than an unbounded range's"},
      {table(twice, all), constants(0, all), false, "two ranges of one table that share t1"},
  };
  for (auto &pair : pairs) {
    step("two bindings: %s", pair.what);
    const D3D12_ROOT_PARAMETER1 both[] = {pair.first, pair.second};
    create(both, pair.valid);
  }
  // a static sampler has an s register as a sampler range has
  const D3D12_DESCRIPTOR_RANGE1 sampler_range[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0}};
  for (UINT reg : {0u, 1u})
    for (UINT space : {0u, 1u}) {
      step("a static sampler at s%u of space %u beside a sampler range of s0 of space 0", reg, space);
      const D3D12_ROOT_PARAMETER1 parameters[] = {table(sampler_range, pixel)};
      D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
      sampler.MaxAnisotropy = 1, sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      sampler.MaxLOD = D3D12_FLOAT32_MAX;
      sampler.ShaderRegister = reg, sampler.RegisterSpace = space;
      D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{D3D_ROOT_SIGNATURE_VERSION_1_1};
      desc.Desc_1_1 = {1, parameters, 1, &sampler};
      ComPtr<ID3DBlob> blob;
      ComPtr<ID3D12RootSignature> root;
      const bool valid = reg || space;
      HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr);
      if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
      expect(valid ? hr == S_OK && root : FAILED(hr) && !root, "returned %08lx, expected %s", hr,
             valid ? "valid" : "invalid");
    }
  return verdict();
}
