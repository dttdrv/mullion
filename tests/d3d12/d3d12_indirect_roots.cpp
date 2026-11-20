// contract: indirect root CBV/SRV/UAV arguments supply each command's GPU addresses while other bindings are inherited.
// "All of the arguments for 1 draw/dispatch call within an indirect argument buffer are tightly packed."
// (DirectX-Specs, Indirect Drawing, Command signature Creation). ByteStride may include padding; the count buffer
// clamps execution to MaxCommandCount. each command writes independently derived CBV/SRV bytes through its UAV.
// all seven nonempty descriptor combinations run on direct/compute queues with FXC5.0/5.1 or DXC6.0 shaders.
// https://microsoft.github.io/DirectX-Specs/d3d/IndirectDrawing.html
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>

static const char hlsl[] = R"hlsl(
cbuffer source_cbv : register(b0) { uint cbv_value; };
StructuredBuffer<uint> source_srv : register(t0);
RWStructuredBuffer<uint4> destination : register(u0);
cbuffer parameters : register(b1) { uint slot, marker; };
[numthreads(1,1,1)] void cs() {
  destination[slot] = uint4(cbv_value, source_srv[0], source_srv[1], marker);
}
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  std::vector<std::string> profiles{compiler.dxc ? "cs_6_0" : "cs_5_0"}, shaders;
  if (!compiler.dxc)
    profiles.push_back("cs_5_1");
  for (const auto &profile : profiles) {
    shaders.push_back(compiler.compile(hlsl, "cs", profile.c_str()));
    if (!expect(!shaders.back().empty(), "%s indirect roots shader did not compile", profile.c_str()))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[]{{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[3].Constants = {1, 0, 2};
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(root != nullptr, "indirect roots signature"))
    return verdict();
  D3D12_INDIRECT_ARGUMENT_DESC roots[]{{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW},
                                      {D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW},
                                      {D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW}};
  roots[0].ConstantBufferView.RootParameterIndex = 0;
  roots[1].ShaderResourceView.RootParameterIndex = 1;
  roots[2].UnorderedAccessView.RootParameterIndex = 2;
  const UINT commands = 33, slots = commands + 2, cbv_stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  using Pixel = std::array<UINT, 4>;
  const UINT64 row_bytes = slots * sizeof(Pixel), output_bytes = slots * row_bytes, srv_stride = 2 * sizeof(UINT);
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT64 offsets[]{0, system.dwPageSize - sizeof(UINT), system.dwPageSize + sizeof(UINT)};
  const UINT maximum = std::numeric_limits<UINT>::max();
  auto cbv_value = [](UINT id) { return ~id ^ (id << 16); };
  auto srv_value = [](UINT id, UINT field) { return (id + 1) * (field + 1) ^ (id << (field + 1)); };
  auto cbv = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, slots * cbv_stride, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto srv = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, slots * srv_stride, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto zero = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, output_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, offsets[2] + sizeof(UINT),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(cbv && srv && zero && output && readback && counts, "indirect roots buffers"))
    return verdict();
  UINT8 *mapped;
  CHECK(cbv->Map(0, nullptr, (void **)&mapped));
  memset(mapped, 0, slots * cbv_stride);
  for (UINT i = 0; i < slots; i++) {
    UINT value = cbv_value(i);
    memcpy(mapped + i * cbv_stride, &value, sizeof(value));
  }
  cbv->Unmap(0, nullptr);
  CHECK(srv->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < slots; i++)
    for (UINT field = 0; field < 2; field++) {
      UINT value = srv_value(i, field);
      memcpy(mapped + i * srv_stride + field * sizeof(value), &value, sizeof(value));
    }
  srv->Unmap(0, nullptr);
  CHECK(zero->Map(0, nullptr, (void **)&mapped));
  memset(mapped, 0xff, output_bytes);
  zero->Unmap(0, nullptr);
  struct Count { UINT maximum, value; bool counted; };
  std::vector<Count> cases;
  for (UINT count : {0u, 1u, commands - 2, commands - 1, commands})
    cases.push_back({count, 0, false});
  for (UINT count : {0u, 1u, commands - 2, commands - 1, commands, commands + 1, maximum})
    cases.push_back({commands, count, true});
  cases.push_back({0, maximum, true});
  cases.push_back({1, commands, true});
  for (UINT profile = 0; profile < shaders.size(); profile++) {
    ComPtr<ID3D12PipelineState> pipeline;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(shaders[profile])};
    CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
    for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE}) {
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      D3D12_COMMAND_QUEUE_DESC q{type};
      CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
      CHECK(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      CHECK(list->Close());
      for (UINT mask = 1; mask < (1u << std::size(roots)); mask++)
        for (UINT padding : {0u, UINT(sizeof(D3D12_DISPATCH_ARGUMENTS))}) {
          std::vector<D3D12_INDIRECT_ARGUMENT_DESC> operations;
          for (UINT i = 0; i < std::size(roots); i++)
            if (mask & (1u << i))
              operations.push_back(roots[i]);
          D3D12_INDIRECT_ARGUMENT_DESC constant{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT};
          constant.Constant = {3, 0, 2};
          operations.push_back(constant);
          operations.push_back({D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH});
          const UINT packed = std::popcount(mask) * sizeof(D3D12_GPU_VIRTUAL_ADDRESS) +
                              2 * sizeof(UINT) + sizeof(D3D12_DISPATCH_ARGUMENTS), stride = packed + padding;
          ComPtr<ID3D12CommandSignature> signature;
          D3D12_COMMAND_SIGNATURE_DESC sd{stride, UINT(operations.size()), operations.data()};
          CHECK(device->CreateCommandSignature(&sd, root.Get(), IID_PPV_ARGS(&signature)));
          for (UINT64 offset : offsets) {
            auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, offset + commands * stride,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!expect(arguments != nullptr, "indirect argument buffer"))
              return verdict();
            CHECK(arguments->Map(0, nullptr, (void **)&mapped));
            memset(mapped, 0xff, offset + commands * stride);
            for (UINT command = 0; command < commands; command++) {
              UINT8 *at = mapped + offset + command * stride;
              auto append = [&](auto value) { memcpy(at, &value, sizeof(value)); at += sizeof(value); };
              D3D12_GPU_VIRTUAL_ADDRESS addresses[]{cbv->GetGPUVirtualAddress() + (command + 1) * cbv_stride,
                  srv->GetGPUVirtualAddress() + (command + 1) * srv_stride,
                  output->GetGPUVirtualAddress() + (command + 1) * row_bytes};
              for (UINT i = 0; i < std::size(roots); i++)
                if (mask & (1u << i))
                  append(addresses[i]);
              append(UINT(mask & (1u << 2) ? 1 : command + 1));
              append(command + 1);
              append(D3D12_DISPATCH_ARGUMENTS{1, 1, 1});
            }
            arguments->Unmap(0, nullptr);
            for (const auto &c : cases) {
              step("%s queue %u roots %#x stride %u offset %llu max %u count %u/%u", profiles[profile].c_str(),
                   type, mask, stride, offset, c.maximum, c.counted, c.value);
              CHECK(counts->Map(0, nullptr, (void **)&mapped));
              memset(mapped, 0xff, offsets[2] + sizeof(UINT));
              memcpy(mapped + offset, &c.value, sizeof(c.value));
              counts->Unmap(0, nullptr);
              CHECK(forget(readback.Get()));
              CHECK(allocator->Reset());
              CHECK(list->Reset(allocator.Get(), pipeline.Get()));
              list->CopyResource(output.Get(), zero.Get());
              transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
              list->SetComputeRootSignature(root.Get());
              list->SetComputeRootConstantBufferView(0, cbv->GetGPUVirtualAddress());
              list->SetComputeRootShaderResourceView(1, srv->GetGPUVirtualAddress());
              list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
              UINT control[]{slots - 1, maximum};
              list->SetComputeRoot32BitConstants(3, std::size(control), control, 0);
              list->ExecuteIndirect(signature.Get(), c.maximum, arguments.Get(), offset, c.counted ? counts.Get() : nullptr,
                                      c.counted ? offset : 0);
              transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
              list->CopyResource(readback.Get(), output.Get());
              CHECK(submit(device.Get(), queue.Get(), list.Get()));
              std::vector<Pixel> expected(slots * slots, Pixel{maximum, maximum, maximum, maximum});
              const UINT executed = c.counted ? std::min(c.maximum, c.value) : c.maximum;
              for (UINT command = 0; command < executed; command++) {
                UINT row = mask & (1u << 2) ? command + 1 : 0, column = mask & (1u << 2) ? 1 : command + 1;
                UINT cbv_id = mask & 1 ? command + 1 : 0, srv_id = mask & 2 ? command + 1 : 0;
                expected[row * slots + column] = {cbv_value(cbv_id), srv_value(srv_id, 0), srv_value(srv_id, 1), command + 1};
              }
              const Pixel *got;
              CHECK(readback->Map(0, nullptr, (void **)&got));
              bool same = true;
              for (UINT i = 0; i < expected.size(); i++)
                if (got[i] != expected[i]) {
                  expect(false, "row %u column %u: %08x %08x %08x %08x, want %08x %08x %08x %08x", i / slots,
                         i % slots, got[i][0], got[i][1], got[i][2], got[i][3], expected[i][0], expected[i][1],
                         expected[i][2], expected[i][3]);
                  same = false;
                  break;
                }
              readback->Unmap(0, nullptr);
              if (!same)
                return verdict();
            }
          }
        }
    }
  }
  return verdict();
}
