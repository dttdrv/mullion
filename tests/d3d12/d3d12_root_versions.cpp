// contract: root signatures serialize at their requested version and convert without changing shared fields.
// "Serializes a root signature of any version" (Microsoft Learn, D3D12SerializeVersionedRootSignature), and
// "all versions the interface has been asked to convert to are available until the deserializer is destroyed"
// (GetRootSignatureDescAtVersion). DirectX-Headers, d3d12.h, defines version 1.2 as 3, with the 1.1 parameters
// and D3D12_STATIC_SAMPLER_DESC1, which appends Flags; d3dx12_root_signature.h initializes added flags to NONE
// when upgrading and copies the common sampler fields when downgrading. multiple samplers distinguish their
// strides; the serialized sampler bytes are checked independently of the deserializer.
// sampler ranges cannot have DATA flags (Microsoft Learn, Root Signature Version 1.1: "For samplers,
// only DESCRIPTORS_VOLATILE is supported."). malformed internal arrays are invalid even in a correctly hashed
// container (CreateRootSignature: "E_INVALIDARG if the blob that pBlobWithRootSignature points to is invalid").
// device creation stays within the queried version support: "the highest version, up to the input version
// specified, actually available" (Microsoft Learn, D3D12_FEATURE_DATA_ROOT_SIGNATURE).
// a library's root signature subobject is also a root signature by its name
// (ID3D12Device14::CreateRootSignatureFromSubobjectInLibrary, d3d12.idl of microsoft/DirectX-Headers): the one made
// of the global subobject serves the pipeline the library's own global root signature serves, and a name the library
// lacks makes none.
#include "d3d12_test.hpp"
#include "d3d12_device14.hpp"
#include <array>
#include <random>

// the installed headers stop at 1.1; these are the 1.2 ABI declarations from DirectX-Headers, d3d12.h
static constexpr auto version12 = D3D_ROOT_SIGNATURE_VERSION(3);
static constexpr UINT uint_border = 1;
static constexpr UINT black_uint = 3, white_uint = 4;
struct Sampler1 {
  D3D12_STATIC_SAMPLER_DESC desc;
  UINT flags;
};
struct Desc2 {
  UINT NumParameters;
  const D3D12_ROOT_PARAMETER1 *pParameters;
  UINT NumStaticSamplers;
  const Sampler1 *pStaticSamplers;
  D3D12_ROOT_SIGNATURE_FLAGS Flags;
};
struct Version2 {
  D3D_ROOT_SIGNATURE_VERSION Version;
  Desc2 desc;
};
static_assert(sizeof(Version2) == sizeof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC));
static_assert(offsetof(Sampler1, flags) == sizeof(D3D12_STATIC_SAMPLER_DESC));

#ifndef __ID3D12DeviceConfiguration1_INTERFACE_DEFINED__
MIDL_INTERFACE("ed342442-6343-4e16-bb82-a3a577874e56")
ID3D12DeviceConfiguration1 : public ID3D12DeviceConfiguration {
  virtual HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *Library, SIZE_T Size, LPCWSTR Name, REFIID riid, void **Deserializer) = 0;
};
__CRT_UUID_DECL(ID3D12DeviceConfiguration1, 0xed342442, 0x6343, 0x4e16, 0xbb, 0x82, 0xa3, 0xa5, 0x77, 0x87, 0x4e, 0x56)
#endif

static const char compute_hlsl[] = R"hlsl(
RWByteAddressBuffer output_bytes : register(u0);
[numthreads(1, 1, 1)] void cs() { output_bytes.Store(0, 1); }
)hlsl";
static const char library_hlsl[] = R"hlsl(
RWByteAddressBuffer output_bytes : register(u0);
LocalRootSignature lrs = { "RootConstants(num32BitConstants=1, b1)" };
#if MIXED
GlobalRootSignature grs = { "UAV(u1)" };
[RootSignature("UAV(u0)")]
[shader("compute")]
[numthreads(1, 1, 1)] void generate() { output_bytes.Store(0, 1); }
#else
GlobalRootSignature grs = { "UAV(u0)" };
#if MULTIPLE
GlobalRootSignature grs2 = { "UAV(u1)" };
#endif
[shader("raygeneration")] void generate() { output_bytes.Store(0, 1); }
#endif
)hlsl";

template <typename T>
static void
check_desc(const T &got, const D3D12_ROOT_PARAMETER1 *parameters, UINT count, const std::vector<Sampler1> &samplers,
           D3D_ROOT_SIGNATURE_VERSION input, D3D_ROOT_SIGNATURE_VERSION output, D3D12_ROOT_SIGNATURE_FLAGS flags) {
  expect(got.NumParameters == count && got.NumStaticSamplers == samplers.size() && got.Flags == flags,
         "description counts or flags changed");
  if (got.NumParameters != count || got.NumStaticSamplers != samplers.size())
    return;
  for (UINT i = 0; i < count; i++) {
    const auto &p = got.pParameters[i];
    const auto &want = parameters[i];
    expect(p.ParameterType == want.ParameterType && p.ShaderVisibility == want.ShaderVisibility,
           "parameter %u changed type or visibility", i);
    switch (want.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      expect(p.DescriptorTable.NumDescriptorRanges == want.DescriptorTable.NumDescriptorRanges,
             "table %u changed its range count", i);
      if (p.DescriptorTable.NumDescriptorRanges != want.DescriptorTable.NumDescriptorRanges)
        break;
      for (UINT j = 0; j < want.DescriptorTable.NumDescriptorRanges; j++) {
        const auto &r = p.DescriptorTable.pDescriptorRanges[j];
        const auto &w = want.DescriptorTable.pDescriptorRanges[j];
        expect(r.RangeType == w.RangeType && r.NumDescriptors == w.NumDescriptors &&
                   r.BaseShaderRegister == w.BaseShaderRegister && r.RegisterSpace == w.RegisterSpace &&
                   r.OffsetInDescriptorsFromTableStart == w.OffsetInDescriptorsFromTableStart,
               "range %u changed", j);
        if constexpr (std::is_same_v<T, D3D12_ROOT_SIGNATURE_DESC1> || std::is_same_v<T, Desc2>)
          expect(r.Flags == (input == D3D_ROOT_SIGNATURE_VERSION_1_0
                                 ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                       (w.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                                            ? D3D12_DESCRIPTOR_RANGE_FLAG_NONE
                                            : D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE)
                                 : w.Flags),
                 "range %u changed flags", j);
      }
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      expect(!memcmp(&p.Constants, &want.Constants, sizeof(want.Constants)), "constants %u changed", i);
      break;
    default:
      expect(p.Descriptor.ShaderRegister == want.Descriptor.ShaderRegister &&
                 p.Descriptor.RegisterSpace == want.Descriptor.RegisterSpace,
             "descriptor %u changed", i);
      if constexpr (std::is_same_v<T, D3D12_ROOT_SIGNATURE_DESC1> || std::is_same_v<T, Desc2>)
        expect(p.Descriptor.Flags == (input == D3D_ROOT_SIGNATURE_VERSION_1_0 ? D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE
                                                                              : want.Descriptor.Flags),
               "descriptor %u changed flags", i);
      break;
    }
  }
  for (UINT i = 0; i < samplers.size(); i++) {
    expect(!memcmp(&got.pStaticSamplers[i], &samplers[i].desc, sizeof(D3D12_STATIC_SAMPLER_DESC)),
           "sampler %u changed shared fields", i);
    if constexpr (std::is_same_v<T, Desc2>)
      expect(got.pStaticSamplers[i].flags == (input == version12 ? samplers[i].flags : 0),
             "sampler %u changed flags at version %u", i, output);
  }
}

int
main(int argc, char **argv) {
  const UINT seed = argc > 2 ? strtoul(argv[2], nullptr, 0) : 1;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(compute_hlsl, "cs", "cs");
  std::array<std::string, 3> libraries;
  if (compiler.dxc)
    for (UINT i = 0; i < libraries.size(); i++) {
      libraries[i] = compiler.compile(library_hlsl, "", "lib_6_3",
                                      {"MULTIPLE=" + std::to_string(i == 1), "MIXED=" + std::to_string(i == 2)});
      if (!expect(!libraries[i].empty(), "library variant %u did not compile", i))
        return verdict();
    }
  if (!expect(!cs.empty(), "compute shader did not compile"))
    return verdict();
  std::minstd_rand random(seed);
  std::array<D3D12_ROOT_PARAMETER1, D3D12_ROOT_PARAMETER_TYPE_UAV + 2> parameters{};
  std::array<D3D12_ROOT_PARAMETER, parameters.size()> old_parameters{};
  D3D12_DESCRIPTOR_RANGE1 ranges[] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, 1}};
  D3D12_DESCRIPTOR_RANGE1 sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0};
  D3D12_DESCRIPTOR_RANGE old_sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0};
  D3D12_DESCRIPTOR_RANGE old_ranges[std::size(ranges)]{};
  for (UINT i = 0; i < std::size(ranges); i++)
    old_ranges[i] = {ranges[i].RangeType, ranges[i].NumDescriptors, ranges[i].BaseShaderRegister,
                     ranges[i].RegisterSpace, ranges[i].OffsetInDescriptorsFromTableStart};
  for (UINT i = 0; i < parameters.size(); i++) {
    auto &p = parameters[i];
    auto &old = old_parameters[i];
    old.ParameterType = p.ParameterType = D3D12_ROOT_PARAMETER_TYPE(i % (D3D12_ROOT_PARAMETER_TYPE_UAV + 1));
    old.ShaderVisibility = p.ShaderVisibility = D3D12_SHADER_VISIBILITY(i % (D3D12_SHADER_VISIBILITY_PIXEL + 1));
    if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      p.DescriptorTable = i ? D3D12_ROOT_DESCRIPTOR_TABLE1{1, &sampler_range}
                            : D3D12_ROOT_DESCRIPTOR_TABLE1{UINT(std::size(ranges)), ranges};
      old.DescriptorTable = i ? D3D12_ROOT_DESCRIPTOR_TABLE{1, &old_sampler_range}
                              : D3D12_ROOT_DESCRIPTOR_TABLE{UINT(std::size(old_ranges)), old_ranges};
    } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
      old.Constants = p.Constants = {i, i + 1, i + 1};
    else {
      p.Descriptor = {i, i + 1, i % 2 ? D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC : D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
      old.Descriptor = {i, i + 1};
    }
  }
  struct Serialized {
    UINT version;
    std::string context, bytes;
  };
  std::vector<Serialized> controls;
  std::array<std::string, UINT(version12)> malformed_bases;
  for (auto flags : {D3D12_ROOT_SIGNATURE_FLAG_NONE, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT})
    for (UINT v = D3D_ROOT_SIGNATURE_VERSION_1_0; v <= UINT(version12); v++)
      for (UINT count = 0; count <= parameters.size(); count++)
        for (UINT sampler_count : {0u, 1u, 2u, UINT(D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT + 1)}) {
          step("seed %u, version %u, parameters %u, samplers %u, flags %u", seed, v, count, sampler_count, flags);
          std::vector<Sampler1> samplers(sampler_count);
          std::vector<D3D12_STATIC_SAMPLER_DESC> old_samplers(sampler_count);
          for (UINT i = 0; i < sampler_count; i++) {
            auto &s = samplers[i];
            s.desc = {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                      D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
            s.desc.ShaderRegister = i;
            // not space 0, where the tables' sampler range has s0: two bindings a stage sees cannot name one
            // register (DirectXShaderCompiler, DxilRootSignatureValidator.cpp, AddRegisterRange)
            s.desc.RegisterSpace = 1 + random() % (D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT - 1);
            s.desc.MipLODBias = float(random() % D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT);
            s.desc.MaxAnisotropy = 1;
            s.desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            s.desc.MaxLOD = D3D12_FLOAT32_MAX;
            s.desc.BorderColor = D3D12_STATIC_BORDER_COLOR(random() % (D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE + 1));
            s.flags = v == UINT(version12) && i % 2 ? uint_border : 0;
            if (s.flags) {
              UINT color = random() % 2 ? black_uint : white_uint;
              memcpy(&s.desc.BorderColor, &color, sizeof(color));
            }
            old_samplers[i] = s.desc;
          }
          auto input = D3D_ROOT_SIGNATURE_VERSION(v);
          D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{input};
          if (input == D3D_ROOT_SIGNATURE_VERSION_1_0)
            desc.Desc_1_0 = {count, old_parameters.data(), sampler_count, old_samplers.data(), flags};
          else
            desc.Desc_1_1 = {count, parameters.data(), sampler_count, old_samplers.data(), flags};
          Version2 desc2{version12, {count, parameters.data(), sampler_count, samplers.data(), flags}};
          ComPtr<ID3DBlob> blob, errors;
          HRESULT hr = D3D12SerializeVersionedRootSignature(
              input == version12 ? reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&desc2) : &desc, &blob,
              &errors);
          if (!expect(hr == S_OK && blob && !errors, "serialize: %08lx", hr))
            continue;
          // DXC, DxilRootSignatureSerializer.cpp: static samplers are the final array, with the version's stride
          const size_t stride = input == version12 ? sizeof(Sampler1) : sizeof(D3D12_STATIC_SAMPLER_DESC);
          if (!expect(blob->GetBufferSize() >= sampler_count * stride, "sampler array exceeds the serialized size"))
            continue;
          const char *tail = (const char *)blob->GetBufferPointer() + blob->GetBufferSize() - sampler_count * stride;
          for (UINT i = 0; i < sampler_count; i++)
            expect(!memcmp(tail + i * stride, &samplers[i], stride), "serialized sampler %u lost bytes", i);
          std::string bytes((const char *)blob->GetBufferPointer(), blob->GetBufferSize());
          controls.push_back({v, trace::doing, bytes});
          if (count == parameters.size() && sampler_count == 2)
            malformed_bases[v - 1] = bytes;
          ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
          ComPtr<ID3D12RootSignatureDeserializer> old_reader;
          if (!expect(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                                    IID_PPV_ARGS(&reader)) == S_OK &&
                          D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                               IID_PPV_ARGS(&old_reader)) == S_OK,
                      "deserialize failed"))
            continue;
          memset(blob->GetBufferPointer(), 0xff, blob->GetBufferSize());
          blob.Reset();
          const auto *unconverted = reader->GetUnconvertedRootSignatureDesc();
          expect(unconverted->Version == input, "unconverted version changed");
          const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *converted[UINT(version12)]{};
          for (UINT out = D3D_ROOT_SIGNATURE_VERSION_1_0; out <= UINT(version12); out++) {
            hr = reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION(out), &converted[out - 1]);
            if (!expect(hr == S_OK && converted[out - 1] &&
                            converted[out - 1]->Version == D3D_ROOT_SIGNATURE_VERSION(out),
                        "convert to version %u: %08lx", out, hr))
              converted[out - 1] = nullptr;
            if (out == v)
              expect(converted[out - 1] == unconverted, "unconverted pointer changed");
          }
          for (UINT out = D3D_ROOT_SIGNATURE_VERSION_1_0; out <= UINT(version12); out++) {
            const auto *p = converted[out - 1];
            if (!p)
              continue;
            if (out == D3D_ROOT_SIGNATURE_VERSION_1_0)
              check_desc(p->Desc_1_0, parameters.data(), count, samplers, input, D3D_ROOT_SIGNATURE_VERSION(out),
                         flags);
            else if (out == D3D_ROOT_SIGNATURE_VERSION_1_1)
              check_desc(p->Desc_1_1, parameters.data(), count, samplers, input, D3D_ROOT_SIGNATURE_VERSION(out),
                         flags);
            else
              check_desc(reinterpret_cast<const Version2 *>(p)->desc, parameters.data(), count, samplers, input,
                         version12, flags);
          }
          check_desc(*old_reader->GetRootSignatureDesc(), parameters.data(), count, samplers, input,
                     D3D_ROOT_SIGNATURE_VERSION_1_0, flags);
          const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *invalid = nullptr;
          expect(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION(0), &invalid) == E_INVALIDARG,
                 "invalid version accepted");
        }
  // DXC's DxilContainerRootSignatureDesc and DxilContainerRootParameter are DWORDs, their offsets in RTS0.
  const auto get = [](const std::string &bytes, size_t at) {
    UINT value;
    memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
  };
  for (auto &malformed_base : malformed_bases) {
    if (!expect(!malformed_base.empty(), "no serialized control for malformed cases"))
      continue;
    const size_t part = get(malformed_base, 8 * sizeof(UINT));
    const size_t data = part + 2 * sizeof(UINT), size = get(malformed_base, part + sizeof(UINT));
    auto refuse = [&](std::string bytes, const char *kind, size_t at, UINT value) {
      step("seed %u, version %u, malformed %s, byte %zu, value %u", seed, get(malformed_base, data), kind, at, value);
      dxbc::sign(bytes);
      ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
      ComPtr<ID3D12RootSignatureDeserializer> old_reader;
      expect(FAILED(D3D12CreateVersionedRootSignatureDeserializer(bytes.data(), bytes.size(), IID_PPV_ARGS(&reader))) &&
                 !reader,
             "malformed versioned signature accepted");
      expect(FAILED(D3D12CreateRootSignatureDeserializer(bytes.data(), bytes.size(), IID_PPV_ARGS(&old_reader))) &&
                 !old_reader,
             "malformed signature accepted");
    };
    for (size_t cut = 0; cut < size; cut++) {
      std::string bytes = malformed_base.substr(0, data + cut);
      UINT total = bytes.size(), length = cut;
      memcpy(bytes.data() + 6 * sizeof(UINT), &total, sizeof(total));
      memcpy(bytes.data() + part + sizeof(UINT), &length, sizeof(length));
      refuse(bytes, "truncated payload", cut, get(malformed_base, data));
    }
    std::string header = malformed_base.substr(0, data + 6 * sizeof(UINT));
    UINT total = header.size(), length = 6 * sizeof(UINT), zero = 0, one = 1;
    memcpy(header.data() + 6 * sizeof(UINT), &total, sizeof(total));
    memcpy(header.data() + part + sizeof(UINT), &length, sizeof(length));
    memcpy(header.data() + data + sizeof(UINT), &zero, sizeof(zero));
    memcpy(header.data() + data + 3 * sizeof(UINT), &one, sizeof(one));
    memcpy(header.data() + data + 4 * sizeof(UINT), &length, sizeof(length));
    refuse(header, "sampler beyond header", length, get(header, data));
    std::vector<size_t> fields = {sizeof(UINT), 2 * sizeof(UINT), 3 * sizeof(UINT), 4 * sizeof(UINT)};
    const size_t parameters_at = get(malformed_base, data + 2 * sizeof(UINT));
    for (UINT i = 0; i < get(malformed_base, data + sizeof(UINT)); i++) {
      const size_t parameter = parameters_at + i * 3 * sizeof(UINT);
      fields.push_back(parameter + 2 * sizeof(UINT));
      if (get(malformed_base, data + parameter) == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const size_t table = get(malformed_base, data + parameter + 2 * sizeof(UINT));
        fields.push_back(table);
        fields.push_back(table + sizeof(UINT));
      }
    }
    for (size_t at : fields)
      for (UINT value : {UINT(size), UINT(size + 1), ~0u}) {
        std::string bytes = malformed_base;
        memcpy(bytes.data() + data + at, &value, sizeof(value));
        refuse(bytes, "count or offset", at, value);
      }
  }
  step("seed %u, device configuration", seed);
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12DeviceConfiguration1> config, second;
  CHECK(device.As(&config));
  CHECK(device.As(&second));
  expect(config == second, "device configuration identity changed");
  expect(config->AddRef() == 4 && config->Release() == 3, "configuration reference count is not the device's");
  second.Reset();
  auto configuration = config->GetDesc();
  expect(configuration.SDKVersion >= D3D12_SDK_VERSION && configuration.Flags == D3D12_DEVICE_FLAG_NONE &&
             configuration.NumEnabledExperimentalFeatures == 0 && configuration.GpuBasedValidationFlags == 0,
         "configuration description differs");
  CHECK(config->GetEnabledExperimentalFeatures(nullptr, 0));
  Version2 config_desc{version12,
                       {1, &parameters[D3D12_ROOT_PARAMETER_TYPE_CBV], 0, nullptr,
                        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}};
  ComPtr<ID3DBlob> config_blob;
  CHECK(config->SerializeVersionedRootSignature(
      reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&config_desc), &config_blob, nullptr));
  ComPtr<ID3D12VersionedRootSignatureDeserializer> config_reader;
  CHECK(config->CreateVersionedRootSignatureDeserializer(config_blob->GetBufferPointer(), config_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&config_reader)));
  check_desc(reinterpret_cast<const Version2 *>(config_reader->GetUnconvertedRootSignatureDesc())->desc,
             config_desc.desc.pParameters, 1, {}, version12, version12, config_desc.desc.Flags);
  for (UINT i = 0; compiler.dxc && i < libraries.size(); i++) {
    step("seed %u, library variant %u", seed, i);
    ComPtr<ID3D12RootSignature> signature;
    HRESULT hr = device->CreateRootSignature(0, libraries[i].data(), libraries[i].size(), IID_PPV_ARGS(&signature));
    if (i == 1)
      expect(hr == E_INVALIDARG && !signature, "ambiguous global signature accepted: %08lx", hr);
    else if (expect(hr == S_OK && signature, "library signature refused: %08lx", hr)) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{signature.Get(), bytecode(cs)};
      ComPtr<ID3D12PipelineState> pipeline;
      hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
      expect(hr == (i == 2 ? E_INVALIDARG : S_OK), "library signature pipeline: %08lx", hr);
    }
    for (auto name : {L"grs", L"lrs", L"missing"}) {
      ComPtr<ID3D12Device14> device14;
      ComPtr<ID3D12RootSignature> named;
      if (expect(SUCCEEDED(device.As(&device14)), "no ID3D12Device14")) {
        hr = device14->CreateRootSignatureFromSubobjectInLibrary(
            0, libraries[i].data(), libraries[i].size(), name, IID_PPV_ARGS(&named)
        );
        if (!wcscmp(name, L"missing"))
          expect(FAILED(hr) && !named, "a root signature of a missing subobject: %08lx", hr);
        else if (
            expect(hr == S_OK && named, "no root signature of a named subobject: %08lx", hr) && !wcscmp(name, L"grs")
        ) {
          D3D12_COMPUTE_PIPELINE_STATE_DESC desc{named.Get(), bytecode(cs)};
          ComPtr<ID3D12PipelineState> pipeline;
          hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
          expect(hr == (i == 2 ? E_INVALIDARG : S_OK), "named global signature pipeline: %08lx", hr);
        }
      }
      ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
      hr = config->CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
          libraries[i].data(), libraries[i].size(), name, IID_PPV_ARGS(&reader));
      if (!wcscmp(name, L"missing"))
        expect(FAILED(hr) && !reader, "missing subobject accepted");
      else if (expect(hr == S_OK && reader, "named subobject refused: %08lx", hr)) {
        const auto *got = reader->GetUnconvertedRootSignatureDesc();
        const auto &desc = got->Desc_1_1;
        bool local = !wcscmp(name, L"lrs");
        if (expect(got->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && desc.NumParameters == 1,
                   "named subobject description differs")) {
          expect(bool(desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) == local, "local flag differs");
          const auto &p = desc.pParameters[0];
          expect(p.ParameterType == (local ? D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS : D3D12_ROOT_PARAMETER_TYPE_UAV),
                 "subobject type differs");
          if (local)
            expect(p.Constants.Num32BitValues == 1 && p.Constants.ShaderRegister == 1, "local constant differs");
          else
            expect(p.Descriptor.ShaderRegister == UINT(i == 2), "global UAV differs");
        }
      }
    }
  }
  D3D12_FEATURE_DATA_ROOT_SIGNATURE support{version12};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &support, sizeof(support)));
  for (auto &control : controls) {
    if (control.version > UINT(support.HighestVersion))
      continue;
    step("device creation: %s", control.context.c_str());
    ComPtr<ID3D12RootSignature> signature;
    HRESULT hr = device->CreateRootSignature(0, control.bytes.data(), control.bytes.size(), IID_PPV_ARGS(&signature));
    expect(hr == S_OK && signature, "supported signature refused: %08lx", hr);
  }
  if (support.HighestVersion < version12) {
    step("seed %u, device version %u, unsupported version %u", seed, support.HighestVersion, version12);
    Version2 empty{version12, {}};
    ComPtr<ID3DBlob> blob;
    CHECK(D3D12SerializeVersionedRootSignature(reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&empty),
                                               &blob, nullptr));
    ComPtr<ID3D12RootSignature> signature;
    expect(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)) ==
               E_INVALIDARG,
           "unsupported root signature version accepted");
  }
  return verdict();
}
