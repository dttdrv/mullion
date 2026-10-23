// contract: min/max return the non-NaN operand and saturation clamps to [0, 1], including infinities.
// "If one source operand is NaN, then the other source operand is returned" (D3D11.3 22.10.10, 22.10.11;
// DXIL.rst FMax, FMin); "sat(NaN) returns 0" (22.19.1; DXIL.rst Saturate). two NaNs give any NaN, and either
// zero sign is allowed when comparing zeros (3.1.3.2). the oracle applies those rules per component.
// Metal's fast forms give these results on the GPU the test runs on, though MSL 6.3/8.1 leave NaN/INF undefined.
// runtime buffer operands cover every pair of edge values, in every width and stage, with and without precise.
// stored library call instructions require each operation to use its fast form; declarations do not count.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

static const char hlsl[] = R"hlsl(
ByteAddressBuffer src : register(t0);
RWStructuredBuffer<uint4> dst : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 evaluate(uint index, uint operation) {
  uint4 ax = src.Load4(index * 2 * VECTOR_BYTES), bx = src.Load4((index * 2 + 1) * VECTOR_BYTES);
  PRECISE TYPE a = (TYPE)asfloat(ax), b = (TYPE)asfloat(bx);
  PRECISE TYPE value;
  if (operation == 0) value = min(a, b);
  else if (operation == 1) value = max(a, b);
  else if (operation == 2) value = saturate(a);
  else value = saturate(b);
  uint4 result = 0;
  result.COMPONENTS = asuint(value);
  return result;
}
uint4 ps(float4 position : SV_Position) : SV_Target {
  return evaluate((uint)position.x, (uint)position.y);
}
[numthreads(GROUP, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  if (id.x >= COUNT) return;
  for (uint op = 0; op < OPERATIONS; op++) dst[id.x * OPERATIONS + op] = evaluate(id.x, op);
}
)hlsl";

static std::string
converted_calls(const std::string &code) {
  auto unix_name = reinterpret_cast<char *(__cdecl *)(const WCHAR *)>(
      GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")
  );
  auto spawn = reinterpret_cast<LONG(WINAPI *)(char *const *, int)>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_unix_spawnvp")
  );
  WCHAR module[MAX_PATH], exe[MAX_PATH + 4], temporary[MAX_PATH], name[MAX_PATH];
  if (!expect(unix_name && spawn && GetModuleFileNameW(nullptr, module, std::size(module)) &&
                  GetTempPathW(std::size(temporary), temporary) && GetTempFileNameW(temporary, L"mm", 0, name),
              "converter paths or process launch unavailable"))
    return {};
  // a test run from a Unix path outside every drive reports its module as unix\... (the
  // \\?\unix device without its prefix): put the prefix back so the path names a real file
  if (module[0] == L'\\' || module[1] == L':') {
    lstrcpynW(exe, module, std::size(exe));
  } else {
    lstrcpyW(exe, L"\\\\?\\");
    lstrcpynW(exe + 4, module, std::size(exe) - 4);
  }
  std::filesystem::path source(name), output(source.wstring() + L".ll");
  auto tool = std::filesystem::path(exe).parent_path().parent_path().parent_path() / "src" / "airconv" / "darwin" /
              "airconv";
  {
    std::ofstream file(source, std::ios::binary);
    file.write(code.data(), code.size());
    if (!expect(bool(file), "pipeline library was not written")) {
      DeleteFileW(source.c_str());
      return {};
    }
  }
  char *paths[] = {unix_name(tool.c_str()), unix_name(source.c_str()), unix_name(output.c_str())};
  std::string calls;
  if (expect(paths[0] && paths[1] && paths[2], "no Unix converter paths")) {
    const char *args[] = {paths[0], "-S", paths[1], "-o", paths[2], nullptr};
    if (expect(spawn(const_cast<char *const *>(args), TRUE) == 0, "library disassembly failed: %ls", tool.c_str())) {
      std::ifstream file(output);
      calls.assign(std::istreambuf_iterator<char>(file), {});
      expect(!calls.empty(), "no converted instructions");
    }
  }
  for (auto path : paths)
    if (path)
      HeapFree(GetProcessHeap(), 0, path);
  DeleteFileW(source.c_str());
  DeleteFileW(output.c_str());
  return calls;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  using Vector = std::array<UINT, 4>;
  constexpr UINT operations = 4, group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  const float inf = std::numeric_limits<float>::infinity();
  const float edges[] = {
      0.0f,
      -0.0f,
      -2.0f,
      -0.5f,
      0.5f,
      1.0f,
      2.0f,
      -inf,
      inf,
      std::numeric_limits<float>::max(),
      -std::numeric_limits<float>::max(),
      std::numeric_limits<float>::quiet_NaN(),
      -std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::signaling_NaN(),
      std::nextafter(0.5f, 1.0f),
      std::nextafter(1.0f, 2.0f)
  };
  const UINT count = std::max<UINT>(std::size(edges) * std::size(edges), 2 * group + 1);
  std::vector<Vector> input(count * 2);
  for (UINT i = 0; i < count; i++)
    for (UINT lane = 0; lane < input[0].size(); lane++) {
      UINT pair = i + lane;
      input[2 * i][lane] = std::bit_cast<UINT>(edges[pair / std::size(edges) % std::size(edges)]);
      input[2 * i + 1][lane] = std::bit_cast<UINT>(edges[pair % std::size(edges)]);
    }
  struct Shader {
    UINT width;
    bool precise;
    std::string ps, cs;
  };
  std::vector<Shader> shaders;
  for (UINT width = 1; width <= input[0].size(); width++)
    for (bool precise : {false, true}) {
      auto suffix = width == 1 ? "" : std::to_string(width);
      std::vector<std::string> defines = {
          "TYPE=float" + suffix,
          "COMPONENTS=" + std::string("xyzw").substr(0, width),
          "PRECISE=" + std::string(precise ? "precise" : ""),
          "GROUP=" + std::to_string(group),
          "COUNT=" + std::to_string(count),
          "OPERATIONS=" + std::to_string(operations),
          "VECTOR_BYTES=" + std::to_string(sizeof(Vector))
      };
      step("compile width=%u precise=%d", width, precise);
      shaders.push_back(
          {width, precise, compiler.compile(hlsl, "ps", "ps", defines), compiler.compile(hlsl, "cs", "cs", defines)}
      );
      if (!expect(!shaders.back().ps.empty() && !shaders.back().cs.empty(), "HLSL did not compile"))
        return 1;
    }
  auto vs = compiler.compile(
      hlsl, "vs", "vs",
      {"TYPE=float", "COMPONENTS=x", "PRECISE=", "GROUP=1", "COUNT=1",
       "OPERATIONS=" + std::to_string(operations), "VECTOR_BYTES=" + std::to_string(sizeof(Vector))}
  );
  if (!expect(!vs.empty(), "vertex shader did not compile"))
    return 1;

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12Device1> device1;
  CHECK(device.As(&device1));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(rs != nullptr, "root signature"))
    return 1;
  auto source =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input.size() * sizeof(Vector), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * operations * sizeof(Vector), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  void *mapped;
  CHECK(source->Map(0, nullptr, &mapped));
  memcpy(mapped, input.data(), input.size() * sizeof(Vector));
  source->Unmap(0, nullptr);
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      count,
      operations,
      1,
      1,
      DXGI_FORMAT_R32G32B32A32_UINT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (auto &shader : shaders)
    for (bool pixel : {false, true}) {
      step("%s width=%u precise=%d", pixel ? "pixel" : "kernel", shader.width, shader.precise);
      ComPtr<ID3D12PipelineState> pipeline;
      if (pixel) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rs.Get();
        pso.VS = bytecode(vs);
        pso.PS = bytecode(shader.ps);
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.SampleDesc.Count = 1;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = desc.Format;
        CHECK(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)));
      } else {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{rs.Get(), bytecode(shader.cs)};
        CHECK(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)));
      }
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipeline.Get()));
      if (pixel) {
        const float clear[4] = {};
        D3D12_VIEWPORT viewport{0, 0, float(count), float(operations), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(count), LONG(operations)};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetGraphicsRootShaderResourceView(0, source->GetGPUVirtualAddress());
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION to{
            readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}
        };
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      } else {
        list->SetComputeRootSignature(rs.Get());
        list->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        list->Dispatch((count + group - 1) / group, 1, 1);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, count * operations * sizeof(Vector));
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      unsigned wrong = 0;
      for (UINT i = 0; i < count; i++)
        for (UINT op = 0; op < operations; op++) {
          Vector got;
          memcpy(
              got.data(),
              (char *)mapped + (pixel ? op * footprint.Footprint.RowPitch + i * sizeof(Vector)
                                      : (i * operations + op) * sizeof(Vector)),
              sizeof(got)
          );
          for (UINT lane = 0; lane < got.size(); lane++) {
            float a = std::bit_cast<float>(input[2 * i][lane]), b = std::bit_cast<float>(input[2 * i + 1][lane]);
            auto minimum = [](float x, float y) { return std::isnan(x) ? y : std::isnan(y) ? x : x < y ? x : y; };
            auto maximum = [](float x, float y) { return std::isnan(x) ? y : std::isnan(y) ? x : x >= y ? x : y; };
            float want = op == 0   ? minimum(a, b)
                         : op == 1 ? maximum(a, b)
                                   : minimum(1.0f, maximum(0.0f, op == 2 ? a : b));
            if (lane >= shader.width)
              want = 0;
            float value = std::bit_cast<float>(got[lane]);
            bool zeros = lane < shader.width && (op < 2 ? a == 0 && b == 0 : (op == 2 ? a : b) == 0);
            bool same = std::isnan(want) ? std::isnan(value)
                        : zeros        ? value == 0
                                         : got[lane] == std::bit_cast<UINT>(want);
            if (!same && wrong++ < 4)
              expect(
                  false, "readback pair=%u op=%u lane=%u inputs=%08x,%08x got=%08x want=%08x", i, op, lane,
                  input[2 * i][lane], input[2 * i + 1][lane], got[lane], std::bit_cast<UINT>(want)
              );
          }
        }
      readback->Unmap(0, nullptr);
      if (wrong > 4)
        expect(false, "%u more wrong components", wrong - 4);
      printf("readback: %u mismatched components\n", wrong);

      ComPtr<ID3D12PipelineLibrary> library;
      CHECK(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)));
      CHECK(library->StorePipeline(L"tested", pipeline.Get()));
      std::string stored(library->GetSerializedSize(), '\0');
      CHECK(library->Serialize(stored.data(), stored.size()));
      std::array<UINT, 3> fast{}, precise{};
      auto assembly = converted_calls(stored);
      std::istringstream instructions(assembly);
      bool calls_ok = true;
      const char *names[] = {"fmin", "fmax", "saturate"};
      for (std::string line; std::getline(instructions, line);)
        if (line.find(" call ") != line.npos)
          for (UINT op = 0; op < fast.size(); op++) {
            for (bool is_fast : {false, true}) {
              auto target = std::string("@air.") + (is_fast ? "fast_" : "") + names[op] + ".";
              auto at = line.find(target);
              if (at == line.npos)
                continue;
              auto suffix = line.substr(at + target.size(), line.find('(', at) - at - target.size());
              UINT lanes = suffix == "f32" ? 1 : 0;
              for (UINT width = 2; width <= input[0].size(); width++)
                if (suffix == "v" + std::to_string(width) + "f32")
                  lanes = width;
              calls_ok &= expect(lanes != 0, "unexpected call width: %s", line.c_str());
              (is_fast ? fast : precise)[op] += lanes;
              printf("call: %s\n", line.c_str());
            }
          }
      for (UINT op = 0; op < fast.size(); op++) {
        printf("calls: %s fast_lanes=%u precise_lanes=%u library_bytes=%zu\n", names[op], fast[op], precise[op],
               stored.size());
        calls_ok &= expect(fast[op] >= shader.width && precise[op] == 0,
                           "%s width=%u: fast_lanes=%u precise_lanes=%u; expected fast calls only", names[op],
                           shader.width, fast[op], precise[op]);
      }
      if (!calls_ok)
        printf("stored library assembly:\n%s\n", assembly.c_str());
    }
  return verdict();
}
