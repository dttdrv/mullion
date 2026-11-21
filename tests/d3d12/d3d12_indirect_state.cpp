// contract: ExecuteIndirect applies the packed CBV, SRV, UAV and partial root-constant arguments to each command.
// afterwards "all bindings are reset to known values": changed constants become zero; other root arguments keep
// their values. count-buffer execution is min(count, MaxCommandCount), including zero.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
// each command writes its three root-view reads and constants into its own record. a following direct dispatch
// reads only constants and the unchanged output UAV: dereferencing a reset null root view is not defined.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptors-overview#null-descriptors
// IndirectDrawing, Validation: "All resource bindings are compatible with the root signature" and
// "Num32BitValuesToSet must be greater than 0"; root ranges, ordering and duplicate bindings are validated too.
// https://microsoft.github.io/DirectX-Specs/d3d/IndirectDrawing.html
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <limits>

static const char hlsl[] = R"hlsl(
cbuffer Source : register(b0) { uint cb; };
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer writable : register(u0);
cbuffer Constants : register(b1) { uint keep_first, slot, value, keep_last; };
RWStructuredBuffer<uint4> destination : register(u1);
[numthreads(1, 1, 1)] void cs() {
  destination[slot] = uint4(cb, source.Load(0), writable.Load(0), keep_first + value + keep_last);
}
[numthreads(1, 1, 1)] void cs_after() {
  destination[slot] = uint4(keep_first, slot, value, keep_last);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile the shader before creating a device");
  auto code = compiler.compile(hlsl, "cs", "cs"), after_code = compiler.compile(hlsl, "cs_after", "cs");
  if (!expect(!code.empty() && !after_code.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  enum Parameter { Cbv, Srv, Uav, Constants, Output, Table, ParameterCount };
  D3D12_ROOT_PARAMETER parameters[ParameterCount] = {
      {D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  parameters[Constants].Constants = {1, 0, 4};
  parameters[Output].Descriptor.ShaderRegister = 1;
  const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2, 0, 0};
  parameters[Table].DescriptorTable = {1, &range};
  auto root = root_signature(device.Get(), {ParameterCount, parameters});
  if (!expect(bool(root), "root signature creation failed"))
    return verdict();
  ComPtr<ID3D12PipelineState> pso, after;
  const D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{root.Get(), bytecode(code)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  const D3D12_COMPUTE_PIPELINE_STATE_DESC after_desc{root.Get(), bytecode(after_code)};
  CHECK(device->CreateComputePipelineState(&after_desc, IID_PPV_ARGS(&after)));
  D3D12_INDIRECT_ARGUMENT_DESC arguments[] = {
      {D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW}, {D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW},
      {D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW}, {D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT},
      {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
  arguments[0].ConstantBufferView.RootParameterIndex = Cbv;
  arguments[1].ShaderResourceView.RootParameterIndex = Srv;
  arguments[2].UnorderedAccessView.RootParameterIndex = Uav;
  const UINT first_changed = 1, changed_count = 2;
  arguments[3].Constant = {Constants, first_changed, changed_count};
  const UINT commands = 3, word = sizeof(UINT), alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  const UINT payload = 3 * sizeof(D3D12_GPU_VIRTUAL_ADDRESS) + changed_count * word + sizeof(D3D12_DISPATCH_ARGUMENTS);
  const UINT stride = payload + word, argument_offset = word, count_offset = word;
  auto validate = [&](const char *name, std::vector<D3D12_INDIRECT_ARGUMENT_DESC> entries, bool rooted, bool valid) {
    UINT size = 0;
    for (const auto &entry : entries)
      switch (entry.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: size += entry.Constant.Num32BitValuesToSet * word; break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: size += sizeof(D3D12_DISPATCH_ARGUMENTS); break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: size += sizeof(D3D12_DRAW_ARGUMENTS); break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: size += sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: size += sizeof(D3D12_VERTEX_BUFFER_VIEW); break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: size += sizeof(D3D12_INDEX_BUFFER_VIEW); break;
      default: size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS); break;
      }
    step("command signature %s: %s", name, valid ? "valid" : "invalid");
    const D3D12_COMMAND_SIGNATURE_DESC desc{size, UINT(entries.size()), entries.data()};
    ComPtr<ID3D12CommandSignature> made;
    const HRESULT hr = device->CreateCommandSignature(&desc, rooted ? root.Get() : nullptr, IID_PPV_ARGS(&made));
    expect(valid ? hr == S_OK : FAILED(hr), "CreateCommandSignature returned %#lx", hr);
    expect(bool(made) == valid, "wrong command signature out parameter");
  };
  std::vector<UINT> parameter_indices;
  for (UINT parameter = 0; parameter <= ParameterCount; parameter++) parameter_indices.push_back(parameter);
  parameter_indices.push_back(std::numeric_limits<UINT>::max());
  for (UINT kind = 0; kind < 4; kind++)
    for (UINT parameter : parameter_indices) {
      auto entry = arguments[kind];
      entry.ConstantBufferView.RootParameterIndex = parameter;
      const auto type = kind == 0 ? D3D12_ROOT_PARAMETER_TYPE_CBV : kind == 1 ? D3D12_ROOT_PARAMETER_TYPE_SRV
                        : kind == 2 ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      step("argument type %u parameter %u", entry.Type, parameter);
      validate("root parameter type and range", {entry, arguments[4]}, true,
               parameter < ParameterCount && parameters[parameter].ParameterType == type);
    }
  const UINT constants_count = parameters[Constants].Constants.Num32BitValues;
  for (UINT offset : {0u, constants_count - 1, constants_count, std::numeric_limits<UINT>::max()})
    for (UINT count : {0u, 1u, constants_count, constants_count + 1}) {
      auto entry = arguments[3];
      entry.Constant = {Constants, offset, count};
      step("constant offset %u count %u", offset, count);
      validate("constant range", {entry, arguments[4]}, true, count && uint64_t(offset) + count <= constants_count);
    }
  validate("descending root parameters", {arguments[1], arguments[0], arguments[4]}, true, false);
  validate("duplicate root view", {arguments[0], arguments[0], arguments[4]}, true, false);
  auto overlap = arguments[3];
  overlap.Constant.DestOffsetIn32BitValues++;
  validate("overlapping root constants", {arguments[3], overlap, arguments[4]}, true, false);
  auto first_constant = arguments[3], second_constant = arguments[3];
  first_constant.Constant = {Constants, 0, 1};
  second_constant.Constant = {Constants, constants_count - 1, 1};
  validate("descending root constants", {second_constant, first_constant, arguments[4]}, true, false);
  validate("nonoverlapping root constants", {first_constant, second_constant, arguments[4]}, true, true);
  const D3D12_INDIRECT_ARGUMENT_DESC draw{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  const D3D12_INDIRECT_ARGUMENT_DESC index_view{D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW},
      indexed_draw{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  validate("index buffer on dispatch", {index_view, arguments[4]}, false, false);
  validate("index buffer on sequential draw", {index_view, draw}, false, false);
  validate("duplicate index buffer", {index_view, index_view, indexed_draw}, false, false);
  for (UINT slot : {0u, UINT(D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1),
                     UINT(D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)}) {
    D3D12_INDIRECT_ARGUMENT_DESC view{D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW};
    view.VertexBuffer.Slot = slot;
    step("vertex buffer slot %u", slot);
    validate("vertex buffer range", {view, draw}, false, slot < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
    if (slot < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
      validate("duplicate vertex buffer", {view, view, draw}, false, false);
      validate("vertex buffer on dispatch", {view, arguments[4]}, false, false);
    }
  }
  ComPtr<ID3D12CommandSignature> signature;
  const D3D12_COMMAND_SIGNATURE_DESC signature_desc{stride, UINT(std::size(arguments)), arguments};
  CHECK(device->CreateCommandSignature(&signature_desc, root.Get(), IID_PPV_ARGS(&signature)));
  using Record = std::array<UINT, 4>;
  const UINT output_records = commands + 2, output_bytes = output_records * sizeof(Record), sentinel = 0xa5a5a5a5;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (commands + 1) * alignment + output_bytes,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, (commands + 1) * alignment,
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto records = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, argument_offset + commands * stride,
                        D3D12_RESOURCE_STATE_GENERIC_READ);
  auto counter = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, count_offset + word, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && source && output && records && counter && readback, "buffer creation failed"))
    return verdict();
  const UINT zeros_at = (commands + 1) * alignment, cb_offset = 0, srv_offset = word, uav_offset = 2 * word;
  auto held = [](UINT command, UINT field) { return (command + 1) * 13 + field + 1; };
  unsigned char *bytes;
  CHECK(upload->Map(0, nullptr, (void **)&bytes));
  memset(bytes, 0, zeros_at);
  for (UINT command = 0; command <= commands; command++)
    for (UINT field = 0; field < 3; field++) {
      const UINT value = held(command, field);
      memcpy(bytes + command * alignment + field * word, &value, word);
    }
  std::vector<UINT> initial(output_bytes / word, sentinel);
  memcpy(bytes + zeros_at, initial.data(), output_bytes);
  upload->Unmap(0, nullptr);
  CHECK(records->Map(0, nullptr, (void **)&bytes));
  memset(bytes, 0, argument_offset + commands * stride);
  for (UINT command = 0; command < commands; command++) {
    unsigned char *at = bytes + argument_offset + command * stride;
    const UINT offsets[] = {cb_offset, srv_offset, uav_offset};
    for (UINT offset : offsets) {
      const D3D12_GPU_VIRTUAL_ADDRESS address = (offset == uav_offset ? source : upload)->GetGPUVirtualAddress()
                                               + command * alignment + offset;
      memcpy(at, &address, sizeof(address));
      at += sizeof(address);
    }
    const UINT constants[] = {command + 1, held(command, 3)};
    memcpy(at, constants, sizeof(constants));
    const D3D12_DISPATCH_ARGUMENTS dispatch{1, 1, 1};
    memcpy(at + sizeof(constants), &dispatch, sizeof(dispatch));
  }
  records->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, zeros_at);
  transition(list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT keep_first = 17, keep_last = 23;
  for (bool counted : {false, true})
    for (UINT maximum : {0u, 1u, commands})
      for (UINT requested : {0u, 1u, commands - 1, commands, commands + 1, std::numeric_limits<UINT>::max()}) {
        if (!counted && requested)
          continue;
        step("%s count %u, maximum %u: indirect bindings and following direct dispatch", counted ? "buffer" : "CPU",
             requested, maximum);
        CHECK(counter->Map(0, nullptr, (void **)&bytes));
        memcpy(bytes + count_offset, &requested, word);
        counter->Unmap(0, nullptr);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        list->CopyBufferRegion(output.Get(), 0, upload.Get(), zeros_at, output_bytes);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootConstantBufferView(Cbv, upload->GetGPUVirtualAddress() + commands * alignment + cb_offset);
        list->SetComputeRootShaderResourceView(Srv, upload->GetGPUVirtualAddress() + commands * alignment + srv_offset);
        list->SetComputeRootUnorderedAccessView(Uav, source->GetGPUVirtualAddress() + commands * alignment + uav_offset);
        list->SetComputeRootUnorderedAccessView(Output, output->GetGPUVirtualAddress());
        const UINT constants[] = {keep_first, commands + 1, held(commands, 3), keep_last};
        list->SetComputeRoot32BitConstants(Constants, std::size(constants), constants, 0);
        list->ExecuteIndirect(signature.Get(), maximum, records.Get(), argument_offset, counted ? counter.Get() : nullptr,
                              count_offset);
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        barrier.UAV.pResource = output.Get();
        list->ResourceBarrier(1, &barrier);
        list->SetPipelineState(after.Get());
        list->Dispatch(1, 1, 1);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, output_bytes);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        std::vector<Record> want(output_records, {sentinel, sentinel, sentinel, sentinel});
        const UINT executed = counted ? std::min(maximum, requested) : maximum;
        for (UINT command = 0; command < executed; command++)
          want[command + 1] = {held(command, 0), held(command, 1), held(command, 2), keep_first + held(command, 3) + keep_last};
        want[0] = {keep_first, 0, 0, keep_last};
        const Record *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        for (UINT record = 0; record < want.size(); record++)
          for (UINT component = 0; component < want[record].size(); component++)
            expect(got[record][component] == want[record][component], "record %u component %u is %#x, want %#x", record,
                   component, got[record][component], want[record][component]);
        readback->Unmap(0, nullptr);
      }
  return verdict();
}
