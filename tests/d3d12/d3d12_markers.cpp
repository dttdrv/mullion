// contract: WriteBufferImmediate writes each 32-bit value to its address, MARKER_IN once the work before has begun,
// MARKER_OUT once it has completed, DEFAULT in order with the list's other work (Microsoft Learn,
// D3D12_WRITEBUFFERIMMEDIATE_MODE); what the GPU reads after the list's next barrier, or a later list reads, holds
// every value written before, the last written to an address. the writes here mix the modes on shared addresses
// between dispatches: a marker then a DEFAULT value and the other way round, markers to one address in a row, values
// read back after a barrier in the same list and by a second list with markers left at the end of the first. word w
// of the marker buffer ends with the last value written to it; the dispatches' own buffer ends with theirs.
#include "d3d12_test.hpp"
#include <map>
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer Dispatch : register(b0) { uint round; };
RWByteAddressBuffer words : register(u0);
[numthreads(64, 1, 1)] void cs(uint id : SV_DispatchThreadID) { words.Store(id * 4, round * 1000 + id); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!code.empty(), "the shader does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList2> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 1};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {2, parameters});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  constexpr UINT words = 64;
  auto marked = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, words * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  auto worked = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, words * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 3 * words * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(rs && marked && worked && readback, "the root signature or buffers are refused"))
    return verdict();

  std::map<UINT, UINT> want;
  auto write = [&](UINT word, UINT value, D3D12_WRITEBUFFERIMMEDIATE_MODE mode) {
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter{marked->GetGPUVirtualAddress() + word * 4, value};
    list->WriteBufferImmediate(1, &parameter, &mode);
    want[word] = value;
  };
  auto dispatch = [&](UINT pass) {
    list->SetComputeRootSignature(rs.Get());
    list->SetPipelineState(pso.Get());
    list->SetComputeRoot32BitConstants(0, 1, &pass, 0);
    list->SetComputeRootUnorderedAccessView(1, worked->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
  };
  constexpr auto in = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN, out = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
                 plain = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;

  // the first list: markers and values between dispatches, read back after a barrier
  dispatch(1);
  write(0, 100, in);
  write(1, 101, out);
  write(2, 102, in), write(2, 202, plain); // a marker, then a DEFAULT value
  write(3, 103, plain), write(3, 203, out); // a DEFAULT value, then a marker
  dispatch(2);
  for (UINT i = 0; i < 8; i++)
    write(4, 400 + i, i & 1 ? out : in); // markers in a row
  // several in one call, in the call's order
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER several[3] = {{marked->GetGPUVirtualAddress() + 5 * 4, 105},
                                                     {marked->GetGPUVirtualAddress() + 5 * 4, 205},
                                                     {marked->GetGPUVirtualAddress() + 6 * 4, 106}};
  D3D12_WRITEBUFFERIMMEDIATE_MODE modes[3] = {out, plain, in};
  list->WriteBufferImmediate(3, several, modes);
  want[5] = 205, want[6] = 106;
  dispatch(3);
  auto first = want;
  transition(list.Get(), marked.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, marked.Get(), 0, words * 4);
  transition(list.Get(), marked.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  // markers left at the end of the list, which the second list reads
  write(7, 107, out);
  write(0, 300, in);
  transition(list.Get(), worked.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 2 * words * 4, worked.Get(), 0, words * 4);
  transition(list.Get(), worked.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  write(8, 108, out);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  transition(list.Get(), marked.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), words * 4, marked.Get(), 0, words * 4);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  auto check = [&](const char *when, const UINT *values, std::map<UINT, UINT> &expected) {
    for (UINT w = 0; w < words; w++) {
      auto found = expected.find(w);
      UINT value = found == expected.end() ? 0 : found->second;
      if (values[w] != value && !wrong++)
        printf("%s: word %u holds %u, want %u\n", when, w, values[w], value);
    }
  };
  check("after the barrier", got, first);
  check("in the next list", got + words, want);
  std::map<UINT, UINT> dispatched;
  for (UINT w = 0; w < words; w++)
    dispatched[w] = 3000 + w;
  check("the dispatches' buffer", got + 2 * words, dispatched);
  readback->Unmap(0, nullptr);
  expect(!wrong, "%u wrong words", wrong);
  return verdict();
}
