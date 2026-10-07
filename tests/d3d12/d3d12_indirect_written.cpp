// contract: ExecuteIndirect draws with what is in its argument and count buffers when it runs on the GPU, so a
// compute pass before it can write them. ExecuteIndirect is to "instruct the GPU to interpret the contents of the
// indirect argument buffer according to the format defined by a particular command signature", and with a count
// buffer "the actual number of operations to be performed are defined by the minimum of [MaxCommandCount], and a
// 32-bit unsigned integer contained in pCountBuffer" (Indirect Drawing; ID3D12GraphicsCommandList::ExecuteIndirect,
// Microsoft Learn). Microsoft's D3D12ExecuteIndirect sample culls so, and so do engines' GPU-driven passes (Unreal's
// instance culling and Nanite's raster binning write draw arguments and counts in compute passes of the frame).
// a compute shader writes COMMANDS commands and a count that is three fewer. command i draws VERTICES vertices from
// vertex or index i on, in up to ROWS instances (the number follows i and a SEED the dispatch is given, and some
// commands have no instances and some no vertices, as the commands of what a culling pass rejects do). vertex v
// is a point in column v of the target and index k is vertex COMMANDS - 1 - k (the vertex ID of a draw does not
// have its start, D3D11.3 8.16, so the column comes from a vertex buffer); an instance is a row. the draws add into
// a float target, so each pixel says how often it was drawn and by which draws, and every pixel is held to that.
// - one list: written, drawn, written again with another seed and drawn indexed. each draw has the arguments of the
//   dispatch before it;
// - two lists of one ExecuteCommandLists call: written in the first, drawn in the second;
// - two ExecuteCommandLists calls with no barrier and no wait between: the buffer is promoted from the common state
//   by its first use in a call and decays to it when the call's lists have run (implicit state transitions; inside
//   one call a buffer written as an unordered access view needs its barrier before it is an indirect argument);
// - dispatches: a dispatch writes the commands of an indirect dispatch (none to ROWS + 1 groups along x, one or two
//   along y and z) of a kernel of THREADS threads a group, in which every thread counts itself in at its place in
//   the grid; each place is held to the number of commands that reach it. a culling pass sizes the next so;
// - written by the pixel shader of a draw into the render targets that stay bound, drawn, written by a second such
//   draw and drawn indexed; and drawn, replaced by CopyBufferRegion and drawn again indexed. an indirect draw has
//   the arguments that what came before it in the list left, whatever kind of command wrote them and whatever pass
//   a translation has open across them;
// - one ExecuteIndirect a command, at the command's offset and with no count buffer, CALLS times over in one list:
//   how Unreal draws what its instance culling wrote, some thousand calls a pass. every command is drawn, the three
//   past the count too, CALLS times;
// - a direct draw after an indirect one with nothing set in between: it draws with the pipeline, the buffers and
//   the root arguments the list has, since ExecuteIndirect changes only what its signature names ("No command
//   signature state leaks back to the command list after the execution is complete", Indirect Drawing). an engine
//   that keeps track of what it has set (Unreal does) sets nothing twice;
// - commands that run past the views: what a command reads past its vertex or index buffer view is 0, as in a
//   direct draw with those arguments (D3D11.3 8.19.2: "the return is 0"). an index 0 is vertex 0 and a vertex of
//   zeros is in column 0, where the buffer behind the view has other vertices. last, and CASE=n (argv[2]) runs one
//   case: a GPU that faults takes the rest of the process's work with it.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint seed; uint indexed; uint vertices; };
RWStructuredBuffer<uint> commands : register(u0);
// a command is five words, indexed or not; the last is no argument of a draw that is not indexed
void command(uint i) {
  const uint turn = (i + seed) % (ROWS + 2);
  commands[5 * i] = turn > ROWS ? 0 : vertices;
  commands[5 * i + 1] = min(turn, ROWS);
  commands[5 * i + 2] = i;
  commands[5 * i + 3] = 0;
  commands[5 * i + 4] = indexed ? 0 : 0x55555555;
  if (i == 0)
    commands[5 * COMMANDS] = COMMANDS - 3;
}
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { command(id.x); }
// the same from a draw of a point a command along the first row, which adds nothing to the target
float4 ps_writes(float4 at : SV_Position) : SV_Target { command(at.x); return 0; }
// the commands of an indirect dispatch, and the kernel it runs
[numthreads(1, 1, 1)] void groups(uint3 id : SV_DispatchThreadID) {
  const uint i = id.x;
  commands[5 * i] = (i + seed) % (ROWS + 2);
  commands[5 * i + 1] = 1 + i % 2;
  commands[5 * i + 2] = 1 + (i + seed) / 2 % 2;
  if (i == 0)
    commands[5 * COMMANDS] = COMMANDS - 3;
}
RWStructuredBuffer<uint> tally : register(u1);
[numthreads(THREADS, 1, 1)] void counts(uint3 id : SV_DispatchThreadID) {
  InterlockedAdd(tally[id.x + GRID * (id.y + 2 * id.z)], 1);
}
float4 vs(float column : COLUMN, uint row : SV_InstanceID) : SV_Position {
  return float4((column + 0.5) * 2 / COMMANDS - 1, 1 - (row + 0.5) * 2 / ROWS, 0, 1);
}
// one for the draw, and the draw's number, which is where a dispatch has its seed
float4 ps() : SV_Target { return float4(1, seed, 0, 0); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT commands = 32, rows = 4, stride = 5 * sizeof(UINT), counted = commands - 3, over = 3, calls = 64;
  // a kernel of two threads a group, and a grid as wide as the most groups along x have threads
  const UINT threads = 2, grid = threads * (rows + 1), places = grid * 2 * 2;
  const std::vector<std::string> defines = {"COMMANDS=" + std::to_string(commands), "ROWS=" + std::to_string(rows),
                                            "THREADS=" + std::to_string(threads), "GRID=" + std::to_string(grid)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines), groups = compiler.compile(hlsl, "groups", "cs", defines),
       counts = compiler.compile(hlsl, "counts", "cs", defines), ps_writes = compiler.compile(hlsl, "ps_writes", "ps", defines);
  if (cs.empty() || vs.empty() || ps.empty() || groups.empty() || counts.empty() || ps_writes.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 3};
  params[2].Descriptor = {1, 0};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  const D3D12_INPUT_ELEMENT_DESC element{"COLUMN", 0, DXGI_FORMAT_R32_FLOAT};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  // what is drawn adds up
  desc.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                                     D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {&element, 1};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> draws, writes, drawn_writes, sizes, counted_in;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draws)));
  desc.PS = bytecode(ps_writes);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&drawn_writes)));
  auto kernel = [&](const std::string &code, ComPtr<ID3D12PipelineState> &pso) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(code)};
    return device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&pso));
  };
  CHECK(kernel(cs, writes));
  CHECK(kernel(groups, sizes));
  CHECK(kernel(counts, counted_in));
  // a draw, an indexed draw and a dispatch
  ComPtr<ID3D12CommandSignature> signatures[3];
  const D3D12_INDIRECT_ARGUMENT_DESC kinds[3] = {{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}, {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED}, {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
  for (UINT kind = 0; kind < std::size(kinds); kind++) {
    D3D12_COMMAND_SIGNATURE_DESC signature{stride, 1, &kinds[kind]};
    CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&signatures[kind])));
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, commands, rows, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  // the commands and, after them, their count; in the common state, which a buffer's first use promotes
  const UINT64 command_bytes = commands * stride + sizeof(UINT);
  auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, command_bytes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto staged = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, command_bytes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(UINT16), D3D12_RESOURCE_STATE_GENERIC_READ);
  float *vertex;
  UINT16 *index;
  CHECK(vertices->Map(0, nullptr, (void **)&vertex));
  CHECK(indices->Map(0, nullptr, (void **)&index));
  auto column_of = [=](UINT indexed, UINT element) { return indexed ? commands - 1 - element : element; };
  for (UINT i = 0; i < commands; i++)
    vertex[i] = i, index[i] = column_of(1, i);

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocators[2];
  ComPtr<ID3D12GraphicsCommandList> lists[2];
  for (UINT i = 0; i < 2; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  // a dispatch that writes commands and the draw of them: its number, how many elements the view has of the buffer
  // its commands count along, and how often each command is drawn by a call of its own (0: all by one call that has
  // the count buffer)
  struct Made {
    UINT seed, number, indexed, vertices, there, calls;
  };
  auto dispatch = [&](ID3D12GraphicsCommandList *list, const Made &made, ID3D12Resource *into) {
    const UINT constants[] = {made.seed, made.indexed, made.vertices};
    list->SetPipelineState(writes.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRoot32BitConstants(0, std::size(constants), constants, 0);
    list->SetComputeRootUnorderedAccessView(1, into->GetGPUVirtualAddress());
    list->Dispatch(commands, 1, 1);
  };
  auto points = [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline, UINT there) {
    const D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), UINT(there * sizeof(float)), sizeof(float)};
    list->SetPipelineState(pipeline);
    list->IASetVertexBuffers(0, 1, &vbv);
  };
  auto draw = [&](ID3D12GraphicsCommandList *list, const Made &made) {
    const D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), UINT(made.there * sizeof(UINT16)), DXGI_FORMAT_R16_UINT};
    points(list, draws.Get(), made.indexed ? commands : made.there);
    list->SetGraphicsRoot32BitConstants(0, 1, &made.number, 0);
    if (made.indexed)
      list->IASetIndexBuffer(&ibv);
    if (!made.calls)
      list->ExecuteIndirect(signatures[made.indexed].Get(), commands, written.Get(), 0, written.Get(), commands * stride);
    for (UINT call = 0; call < made.calls * commands; call++)
      list->ExecuteIndirect(signatures[made.indexed].Get(), 1, written.Get(), call % commands * stride, nullptr, 0);
  };
  // the commands written by a draw: a point a command, whose pixel shader stores it
  auto draw_writes = [&](ID3D12GraphicsCommandList *list, const Made &made) {
    const UINT constants[] = {made.seed, made.indexed, made.vertices};
    points(list, drawn_writes.Get(), commands);
    list->SetGraphicsRoot32BitConstants(0, std::size(constants), constants, 0);
    list->SetGraphicsRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
    list->DrawInstanced(commands, 1, 0, 0);
  };
  // a list with the target bound, once for all its draws; the first list of a case clears it
  auto begins = [&](UINT i) {
    const float nothing[4] = {};
    const D3D12_VIEWPORT viewport{0, 0, (float)commands, (float)rows, 0, 1};
    const D3D12_RECT scissor{0, 0, (LONG)commands, (LONG)rows};
    auto list = lists[i].Get();
    if (!expect(SUCCEEDED(allocators[i]->Reset()) && SUCCEEDED(list->Reset(allocators[i].Get(), nullptr)), "list %u could not be begun", i))
      return false;
    if (i == 0)
      list->ClearRenderTargetView(rtv, nothing, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    return true;
  };
  const auto common = D3D12_RESOURCE_STATE_COMMON, writing = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, reading = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
             copied = D3D12_RESOURCE_STATE_COPY_SOURCE;
  // commands written, by a dispatch or by a draw, and drawn, with the barriers a list needs around them
  auto pass = [&](ID3D12GraphicsCommandList *list, const Made &made, bool drawn = false) {
    transition(list, written.Get(), common, writing);
    drawn ? draw_writes(list, made) : dispatch(list, made, written.Get());
    transition(list, written.Get(), writing, reading);
    draw(list, made);
    transition(list, written.Get(), reading, common);
  };
  // the target read, and every pixel held to the draws that were made; `past` is how many vertices they are to
  // read past a view, and `direct` are columns that draws of these numbers filled besides
  struct Direct {
    UINT column, number;
  };
  auto holds = [&](std::vector<Made> all, bool past = false, std::vector<Direct> direct = {}) {
    std::vector<float> want(2 * commands * rows);
    UINT read_past = 0;
    for (auto [column, number] : direct)
      for (UINT row = 0; row < rows; row++)
        want[2 * (row * commands + column)]++, want[2 * (row * commands + column) + 1] += number;
    for (auto &made : all)
      for (UINT i = 0; i < (made.calls ? commands : counted); i++)
        for (UINT element = i; element < i + made.vertices; element++)
          for (UINT row = 0, turn = (i + made.seed) % (rows + 2), times = made.calls ? made.calls : 1; row < turn && turn <= rows; row++) {
            auto pixel = &want[2 * (row * commands + (element < made.there ? column_of(made.indexed, element) : 0))];
            pixel[0] += times, pixel[1] += times * made.number, read_past += element >= made.there;
          }
    expect(!read_past == !past, "the case has %u vertices past a view", read_past);
    if (FAILED(forget(readback.Get())) || FAILED(allocators[0]->Reset()) || FAILED(lists[0]->Reset(allocators[0].Get(), nullptr)))
      return (void)expect(false, "the read could not be begun");
    auto list = lists[0].Get();
    transition(list, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT ran = submit(device.Get(), queue.Get(), list);
    const char *out;
    if (!expect(ran == S_OK, "the device after the lists: %08lx", ran) || FAILED(readback->Map(0, nullptr, (void **)&out)))
      return;
    unsigned wrong = 0;
    for (UINT row = 0; row < rows; row++)
      for (UINT column = 0; column < commands; column++) {
        auto got = (const _Float16 *)(out + row * footprint.Footprint.RowPitch) + 4 * column;
        auto pixel = &want[2 * (row * commands + column)];
        if (((float)got[0] != pixel[0] || (float)got[1] != pixel[1]) && wrong++ < 4)
          expect(false, "column %u, row %u was drawn %g times by draws whose numbers add up to %g, want %g and %g", column, row,
                 (float)got[0], (float)got[1], pixel[0], pixel[1]);
      }
    expect(wrong <= 4, "and %u more pixels", wrong - 4);
    readback->Unmap(0, nullptr);
  };
  const UINT only = argc > 2 && !strncmp(argv[2], "CASE=", 5) ? strtoul(argv[2] + 5, nullptr, 0) : ~0u;
  UINT number = 0;
  auto chosen = [&](const char *what) {
    if (only != ~0u && only != number++)
      return false;
    step("%s", what);
    return true;
  };

  if (chosen("one list: commands written, drawn, written again and drawn indexed") && begins(0)) {
    const Made first{0, 1, 0, 1, commands}, second{1, 2, 1, 1, commands};
    pass(lists[0].Get(), first);
    pass(lists[0].Get(), second);
    if (expect(submit(device.Get(), queue.Get(), lists[0].Get()) == S_OK, "the list did not run"))
      holds({first, second});
  }
  if (chosen("two lists of one ExecuteCommandLists: commands written in the first, drawn indexed in the second") && begins(0) && begins(1)) {
    const Made made{2, 1, 1, 1, commands};
    transition(lists[0].Get(), written.Get(), common, writing);
    dispatch(lists[0].Get(), made, written.Get());
    transition(lists[1].Get(), written.Get(), writing, reading);
    draw(lists[1].Get(), made);
    transition(lists[1].Get(), written.Get(), reading, common);
    ID3D12CommandList *both[] = {lists[0].Get(), lists[1].Get()};
    if (expect(SUCCEEDED(lists[0]->Close()) && SUCCEEDED(lists[1]->Close()), "the lists did not close")) {
      queue->ExecuteCommandLists(2, both);
      holds({made});
    }
  }
  if (chosen("two ExecuteCommandLists with no barrier and no wait: commands written in the first, drawn in the second") && begins(0) && begins(1)) {
    const Made made{3, 1, 0, 1, commands};
    dispatch(lists[0].Get(), made, written.Get());
    draw(lists[1].Get(), made);
    ID3D12CommandList *first[] = {lists[0].Get()}, *second[] = {lists[1].Get()};
    if (expect(SUCCEEDED(lists[0]->Close()) && SUCCEEDED(lists[1]->Close()), "the lists did not close")) {
      queue->ExecuteCommandLists(1, first);
      queue->ExecuteCommandLists(1, second);
      holds({made});
    }
  }
  if (chosen("one list: the commands of an indirect dispatch written by a dispatch") && begins(0)) {
    const UINT seed = 1, tally_bytes = places * sizeof(UINT);
    auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, tally_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto tally = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, tally_bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto tallied = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, tally_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    void *nothing;
    const UINT *got;
    auto list = lists[0].Get();
    if (expect(SUCCEEDED(zeros->Map(0, nullptr, &nothing)), "no zeros")) {
      memset(nothing, 0, tally_bytes);
      list->CopyBufferRegion(tally.Get(), 0, zeros.Get(), 0, tally_bytes);
      transition(list, tally.Get(), D3D12_RESOURCE_STATE_COPY_DEST, writing);
      transition(list, written.Get(), common, writing);
      list->SetPipelineState(sizes.Get());
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRoot32BitConstants(0, 1, &seed, 0);
      list->SetComputeRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
      list->Dispatch(commands, 1, 1);
      transition(list, written.Get(), writing, reading);
      list->SetPipelineState(counted_in.Get());
      list->SetComputeRootUnorderedAccessView(2, tally->GetGPUVirtualAddress());
      list->ExecuteIndirect(signatures[2].Get(), commands, written.Get(), 0, written.Get(), commands * stride);
      transition(list, written.Get(), reading, common);
      transition(list, tally.Get(), writing, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(tallied.Get(), tally.Get());
      HRESULT ran = submit(device.Get(), queue.Get(), list);
      if (expect(ran == S_OK, "the device after the list: %08lx", ran) && SUCCEEDED(tallied->Map(0, nullptr, (void **)&got))) {
        std::vector<UINT> want(places);
        for (UINT i = 0; i < counted; i++)
          for (UINT z = 0; z < 1 + (i + seed) / 2 % 2; z++)
            for (UINT y = 0; y < 1 + i % 2; y++)
              for (UINT x = 0; x < threads * ((i + seed) % (rows + 2)); x++)
                want[x + grid * (y + 2 * z)]++;
        for (UINT place = 0; place < places; place++)
          expect(got[place] == want[place], "%u threads counted themselves in at x %u, y %u, z %u, want %u", got[place], place % grid,
                 place / grid % 2, place / grid / 2, want[place]);
        tallied->Unmap(0, nullptr);
      }
    }
  }
  if (chosen("one list, the target bound throughout: commands written by a draw, drawn, written by a draw again and drawn indexed") && begins(0)) {
    const Made first{4, 1, 0, 1, commands}, second{5, 2, 1, 1, commands};
    pass(lists[0].Get(), first, true);
    pass(lists[0].Get(), second, true);
    if (expect(submit(device.Get(), queue.Get(), lists[0].Get()) == S_OK, "the list did not run"))
      holds({first, second});
  }
  if (chosen("one list: commands drawn, replaced by a copy and drawn indexed") && begins(0)) {
    const Made first{6, 1, 0, 1, commands}, second{7, 2, 1, 1, commands};
    auto list = lists[0].Get();
    // the second commands wait in a buffer of their own, so that the copy is all that lies between the draws
    transition(list, staged.Get(), common, writing);
    dispatch(list, second, staged.Get());
    transition(list, staged.Get(), writing, copied);
    transition(list, written.Get(), common, writing);
    dispatch(list, first, written.Get());
    transition(list, written.Get(), writing, reading);
    draw(list, first);
    transition(list, written.Get(), reading, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(written.Get(), 0, staged.Get(), 0, command_bytes);
    transition(list, written.Get(), D3D12_RESOURCE_STATE_COPY_DEST, reading);
    draw(list, second);
    transition(list, written.Get(), reading, common);
    transition(list, staged.Get(), copied, common);
    if (expect(submit(device.Get(), queue.Get(), list) == S_OK, "the list did not run"))
      holds({first, second});
  }
  if (chosen("one list: commands written, each drawn by calls of its own, written again and drawn so indexed") && begins(0)) {
    const Made first{8, 1, 0, 1, commands, calls}, second{9, 2, 1, 1, commands, calls};
    pass(lists[0].Get(), first);
    pass(lists[0].Get(), second);
    if (expect(submit(device.Get(), queue.Get(), lists[0].Get()) == S_OK, "the list did not run"))
      holds({first, second});
  }
  if (chosen("one list: commands drawn and then a direct draw with nothing set in between, not indexed and indexed") && begins(0)) {
    const Made first{10, 1, 0, 1, commands}, second{11, 2, 1, 1, commands};
    // the vertex and the index the direct draws begin at: one point in every row
    const UINT vertex = 5, index_start = 9;
    auto list = lists[0].Get();
    pass(list, first);
    list->DrawInstanced(1, rows, vertex, 0);
    pass(list, second);
    list->DrawIndexedInstanced(1, rows, index_start, 0, 0);
    if (expect(submit(device.Get(), queue.Get(), list) == S_OK, "the list did not run"))
      holds({first, second}, false, {{column_of(0, vertex), first.number}, {column_of(1, index_start), second.number}});
  }
  for (UINT indexed = 0; indexed < 2; indexed++)
    if (chosen(indexed ? "commands whose indices run past the index buffer's view" : "commands whose vertices run past the vertex buffer's view") && begins(0)) {
      // the last commands that count read past a view of as many elements as there are commands that count
      const Made made{indexed, 1, indexed, over, counted};
      pass(lists[0].Get(), made);
      if (expect(submit(device.Get(), queue.Get(), lists[0].Get()) == S_OK, "the list did not run"))
        holds({made}, true);
    }
  return verdict();
}
