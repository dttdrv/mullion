// contract: a linear constant-buffer load returns the scalar at its byte offset, or zero outside its view.
// "Out of bounds access to ConstantBuffers returns 0 in all components." (D3D11.3 7.5).
// "Read alignment is a constant value identifying what the byte offset alignment is." (DXIL.rst, CBufferLoad).
// "Root constants are an array of DWORD values" with "no extra padding or alignment" (DXR, Shader table memory
// initialization). root CBVs and root constants have no view size; a table views less than the allocated buffer.
// scalar loads use the compiler's legacy layout, including the view ends and a high unsigned offset, with opcode
// 59 as a control. a ray generation record puts a uint64 after one DWORD, and after two as an aligned control.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <regex>

// DXC ignores -no-legacy-cbuf-layout. assemble equivalent scalar loads at the legacy layout's byte offsets
// (DXIL.rst, CBufferLoadLegacy and CBufferLoad), preserving the compiler's signatures and resource metadata.
static std::string
scalar_loads(const Compiler &compiler, const std::string &code) {
  auto disassemble = [&](const std::string &shader) {
    DxcBuffer source{shader.data(), shader.size(), 0};
    ComPtr<IDxcResult> result;
    ComPtr<IDxcBlobUtf8> text;
    if (FAILED(compiler.dxc->Disassemble(&source, IID_PPV_ARGS(&result))) ||
        FAILED(result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&text), nullptr)) || !text)
      return std::string{};
    return std::string(text->GetStringPointer(), text->GetStringLength());
  };
  auto ir = disassemble(code);
  std::regex instruction(
      R"((%[\w.$-]+) = call %dx.types.CBufRet\.(i[0-9]+)(?:\.[0-9]+)? )"
      R"(@dx.op.cbufferLoadLegacy\.\2)"
      R"(\(i32 59, (%dx.types.Handle(?:\.[0-9]+)? [%\w.$-]+), i32 ([%\w.$-]+)\)[^\r\n]*|)"
      R"((%[\w.$-]+) = extractvalue %dx.types.CBufRet\.(i[0-9]+)(?:\.[0-9]+)? (%[\w.$-]+), ([0-9]+))");
  std::map<std::string, std::string> handles;
  std::map<std::string, std::string> types;
  std::string scalar;
  size_t at = 0;
  for (auto it = std::sregex_iterator(ir.begin(), ir.end(), instruction); it != std::sregex_iterator(); ++it) {
    auto &m = *it;
    scalar.append(ir, at, m.position() - at);
    if (m[1].matched) {
      handles[m[1]] = m[3];
      scalar += m[1].str() + " = mul i32 " + m[4].str() + ", " +
                std::to_string(D3D12_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS * sizeof(uint32_t));
    } else {
      const auto type = m[6].str();
      const UINT bytes = std::stoul(type.substr(1)) / 8;
      auto handle = handles.find(m[7]);
      if (!expect(handle != handles.end(), "unrecognized cbuffer extraction"))
        return {};
      types[type] = handle->second.substr(0, handle->second.find(' '));
      auto offset = "%scalar.offset." + m[5].str().substr(1);
      scalar += offset + " = add i32 " + m[7].str() + ", " + std::to_string(std::stoul(m[8]) * bytes) + "\n  " +
                m[5].str() + " = call " + type + " @dx.op.cbufferLoad." + type + "(i32 58, " + handle->second +
                ", i32 " + offset + ", i32 " + std::to_string(bytes) + ")";
    }
    at = m.position() + m.length();
  }
  scalar.append(ir, at, std::string::npos);
  if (!expect(!types.empty(), "no scalar cbuffer loads generated"))
    return {};
  for (auto &[type, handle] : types)
    scalar += "\ndeclare " + type + " @dx.op.cbufferLoad." + type + "(i32, " + handle + ", i32, i32)\n";
  auto create = (DxcCreateInstanceProc)GetProcAddress(GetModuleHandleA("dxcompiler.dll"), "DxcCreateInstance");
  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcAssembler> assembler;
  ComPtr<IDxcBlobEncoding> source, errors;
  ComPtr<IDxcOperationResult> assembled;
  ComPtr<IDxcBlob> object;
  HRESULT status = E_FAIL;
  if (!expect(create && SUCCEEDED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) &&
                  SUCCEEDED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) &&
                  SUCCEEDED(utils->CreateBlob(scalar.data(), scalar.size(), DXC_CP_UTF8, &source)) &&
                  SUCCEEDED(assembler->AssembleToContainer(source.Get(), &assembled)),
              "scalar DXIL assembly did not start"))
    return {};
  assembled->GetStatus(&status);
  if (FAILED(status)) {
    if (SUCCEEDED(assembled->GetErrorBuffer(&errors)) && errors)
      printf("scalar DXIL: %.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
    expect(false, "scalar DXIL did not assemble");
    return {};
  }
  if (!expect(SUCCEEDED(assembled->GetResult(&object)) && object, "scalar DXIL has no container"))
    return {};
  std::string out((const char *)object->GetBufferPointer(), object->GetBufferSize());
  auto emitted = disassemble(out);
  if (!expect(emitted.find("call %dx.types.CBufRet.") == std::string::npos,
              "legacy cbuffer call left in scalar shader"))
    return {};
  for (auto &[type, handle] : types)
    if (!expect(emitted.find("@dx.op.cbufferLoad." + type + "(i32 58,") != std::string::npos,
                "assembled shader has no opcode 58 for %s", type.c_str()))
      return {};
  dxbc::sign(out);
  return kept(out, "scalar_loads");
}

static const char hlsl[] = R"hlsl(
#if BITS == 16
#define T uint16_t
#define V uint16_t4
struct R { V lo; V hi; };
#elif BITS == 64
#define T uint64_t
#define V uint64_t4
#define R uint64_t2
#else
#define T uint
#define V uint4
#define R uint4
#endif
cbuffer Root : register(b0) { R root_values[ROWS]; };
cbuffer Table : register(b1) { R table_values[ROWS]; };
cbuffer Immediate : register(b2) { V immediate_values; };
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1, 1, 1)]
void cs(uint t : SV_DispatchThreadID) {
  uint row = t / COMPONENTS, component = t % COMPONENTS;
  uint index = t == VALUES - 1 ? 0xffffffff / ROW_BYTES : row;
  uint table_component = t == VALUES - 1 ? COMPONENTS - 1 : component;
#if BITS == 16
  T a = component < CONSTANTS ? root_values[row].lo[component] : root_values[row].hi[component - CONSTANTS];
  T b = table_component < CONSTANTS ? table_values[index].lo[table_component]
                                  : table_values[index].hi[table_component - CONSTANTS];
#else
  T a = root_values[row][component], b = table_values[index][table_component];
#endif
  T c = immediate_values[t % CONSTANTS];
#if BITS == 64
  result[t] = uint4(uint(a), uint(a >> 32), uint(b), uint(b >> 32));
  result[VALUES + t] = uint4(uint(c), uint(c >> 32), 0, 0);
#else
  result[t] = uint4(uint(a), 0, uint(b), 0);
  result[VALUES + t] = uint4(uint(c), 0, 0, 0);
#endif
}
)hlsl";

static const char ray_hlsl[] = R"hlsl(
cbuffer Prefix : register(b3) { uint prefix; };
cbuffer Wide : register(b4) { uint64_t wide; };
RWStructuredBuffer<uint4> result : register(u0);
[shader("raygeneration")]
void local_root() { result[0] = uint4(prefix, uint(wide), uint(wide >> 32), 0); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT view_bytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
             constants = D3D12_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS,
             root_words = constants * sizeof(uint64_t) / sizeof(uint32_t);
  struct Case {
    UINT bytes, stride, values;
    bool linear;
    std::string code;
  };
  std::vector<Case> cases;
  const UINT row_bytes = D3D12_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS * sizeof(uint32_t);
  if (compiler.dxc)
    for (UINT bytes = sizeof(uint16_t); bytes <= sizeof(uint64_t); bytes *= 2)
      cases.push_back({bytes, bytes, 2 * view_bytes / bytes + 1, true});
  cases.push_back({sizeof(uint32_t), sizeof(uint32_t), 2 * view_bytes / sizeof(uint32_t) + 1, false});
  for (auto &c : cases) {
    step("compile %u-bit %s layout", c.bytes * 8, c.linear ? "linear" : "legacy");
    std::vector<std::wstring> flags;
    if (c.bytes == sizeof(uint16_t))
      flags.push_back(L"-enable-16bit-types");
    c.code = compiler.compile(hlsl, "cs", compiler.dxc ? "cs_6_2" : "cs",
                              {"BITS=" + std::to_string(c.bytes * 8), "VALUES=" + std::to_string(c.values),
                               "ROWS=" + std::to_string((c.values * c.bytes + row_bytes - 1) / row_bytes),
                               "COMPONENTS=" + std::to_string(row_bytes / c.bytes),
                               "ROW_BYTES=" + std::to_string(row_bytes), "CONSTANTS=" + std::to_string(constants)},
                              flags);
    if (!expect(!c.code.empty(), "HLSL did not compile"))
      return verdict();
    if (c.linear) {
      c.code = scalar_loads(compiler, c.code);
      if (c.code.empty())
        return verdict();
    }
  }
  std::string ray;
  if (compiler.dxc) {
    ray = compiler.compile(ray_hlsl, "", "lib_6_3");
    if (!expect(!ray.empty(), "ray HLSL did not compile"))
      return verdict();
    ray = scalar_loads(compiler, ray);
    if (ray.empty())
      return verdict();
  }

  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1};
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_CBV},
                                   {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                   {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                   {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].DescriptorTable = {1, &range};
  params[3].Constants = {2, 0, root_words};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(!!rs, "root signature"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  const uint32_t seed = 0x735a29b1;
  for (auto &c : cases) {
    const UINT data_bytes = c.values * c.stride, table_at = (data_bytes + view_bytes - 1) / view_bytes * view_bytes;
    std::vector<uint8_t> input(table_at + data_bytes);
    std::mt19937 random(seed);
    std::generate(input.begin(), input.end(), [&] { return uint8_t(random()); });
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
    const UINT64 output_bytes = 2 * c.values * 4 * sizeof(uint32_t);
    auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && output && readback, "buffers"))
      return verdict();
    void *mapped;
    CHECK(upload->Map(0, nullptr, &mapped));
    memcpy(mapped, input.data(), input.size());
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(c.code)};
    step("seed=%#x bits=%u linear=%u create compute pipeline", seed, c.bytes * 8, c.linear);
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (!expect(hr == S_OK, "CreateComputePipelineState returned %#lx", hr))
      continue;
    for (UINT size : {0u, view_bytes, 2 * view_bytes}) {
      step("seed=%#x bits=%u linear=%u view=%u read every scalar", seed, c.bytes * 8, c.linear, size);
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{size ? upload->GetGPUVirtualAddress() + table_at : 0, size};
      device->CreateConstantBufferView(size ? &cbv : nullptr, heap->GetCPUDescriptorHandleForHeapStart());
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      ID3D12DescriptorHeap *heaps[] = {heap.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootConstantBufferView(0, upload->GetGPUVirtualAddress());
      list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
      list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
      list->SetComputeRoot32BitConstants(3, root_words, input.data(), 0);
      list->Dispatch(c.values, 1, 1);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      uint32_t *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < c.values; i++) {
        uint64_t root = 0, table = 0, immediate = 0;
        memcpy(&root, input.data() + i * c.stride, c.bytes);
        if (i != c.values - 1 && i * c.stride + c.bytes <= size)
          memcpy(&table, input.data() + table_at + i * c.stride, c.bytes);
        memcpy(&immediate, input.data() + (i % constants) * c.bytes, c.bytes);
        const uint32_t want[] = {uint32_t(root), uint32_t(root >> 32), uint32_t(table), uint32_t(table >> 32)};
        for (UINT j = 0; j < std::size(want); j++)
          expect(got[4 * i + j] == want[j], "scalar=%u component=%u got=%#x want=%#x", i, j, got[4 * i + j], want[j]);
        const uint32_t want_immediate[] = {uint32_t(immediate), uint32_t(immediate >> 32), 0, 0};
        for (UINT j = 0; j < std::size(want_immediate); j++)
          expect(got[4 * (c.values + i) + j] == want_immediate[j], "root constant scalar=%u component=%u", i, j);
      }
      readback->Unmap(0, nullptr);
    }
  }
  if (compiler.dxc) {
    D3D12_ROOT_PARAMETER global_param{D3D12_ROOT_PARAMETER_TYPE_UAV}, local_params[2]{};
    local_params[0].ParameterType = local_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    local_params[1].Constants = {4, 0, sizeof(uint64_t) / sizeof(uint32_t)};
    auto global = root_signature(device.Get(), {1, &global_param});
    const UINT64 record_bytes = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    auto record = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, record_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(uint32_t) * constants,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback =
        buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(uint32_t) * constants, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(global && record && output && readback, "local root buffers and global signature"))
      return verdict();
    for (UINT prefix_words : {1u, 2u}) {
      step("seed=%#x local root prefix=%u DWORDs", seed, prefix_words);
      local_params[0].Constants = {3, 0, prefix_words};
      auto local = root_signature(device.Get(), {UINT(std::size(local_params)), local_params, 0, nullptr,
                                                 D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE});
      if (!expect(!!local, "local root signature"))
        return verdict();
      D3D12_DXIL_LIBRARY_DESC library{bytecode(ray)};
      D3D12_GLOBAL_ROOT_SIGNATURE global_desc{global.Get()};
      D3D12_LOCAL_ROOT_SIGNATURE local_desc{local.Get()};
      D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
      D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
      D3D12_STATE_SUBOBJECT subobjects[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                                            {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_desc},
                                            {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local_desc},
                                            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
                                            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}};
      D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(std::size(subobjects)),
                                   subobjects};
      ComPtr<ID3D12StateObject> state;
      CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&state)));
      ComPtr<ID3D12StateObjectProperties> properties;
      CHECK(state.As(&properties));
      auto identifier = properties->GetShaderIdentifier(L"local_root");
      if (!expect(!!identifier, "local root shader identifier"))
        return verdict();
      std::mt19937 random(seed);
      const uint32_t prefix = random(), low = random(), high = random(), want[] = {prefix, low, high, 0};
      uint8_t *mapped;
      CHECK(record->Map(0, nullptr, (void **)&mapped));
      memset(mapped, 0, record_bytes);
      memcpy(mapped, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      auto arguments = mapped + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
      memcpy(arguments, &prefix, sizeof(prefix));
      memcpy(arguments + prefix_words * sizeof(uint32_t), &low, sizeof(low));
      memcpy(arguments + prefix_words * sizeof(uint32_t) + sizeof(low), &high, sizeof(high));
      record->Unmap(0, nullptr);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->SetComputeRootSignature(global.Get());
      list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      list->SetPipelineState1(state.Get());
      D3D12_DISPATCH_RAYS_DESC dispatch{};
      dispatch.RayGenerationShaderRecord = {record->GetGPUVirtualAddress(), record_bytes};
      dispatch.Width = dispatch.Height = dispatch.Depth = 1;
      list->DispatchRays(&dispatch);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      uint32_t *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < std::size(want); i++)
        expect(got[i] == want[i], "local root component=%u got=%#x want=%#x", i, got[i], want[i]);
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
