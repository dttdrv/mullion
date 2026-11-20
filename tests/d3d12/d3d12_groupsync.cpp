// contract: the coherent group sync correction changes only group syncs in shaders whose loops access
// globallycoherent memory. HLSL RWStructuredBuffer: "causes memory barriers and syncs to flush data across the
// entire GPU such that other groups can see writes"
// (https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-rwstructuredbuffer).
// independent groups and memory barriers without sync keep identical compiled bytes with the correction on or
// off (SM50_SHADER_FLAG_COHERENT_GROUP_SYNC). a device barrier already has the required scope. both Metal versions
// are checked: 3.1 lowers to a barrier, 3.2 to a fence and a barrier. persistent checks the communicating groups'
// written nodes and leaves; this checks selection without using timing to detect an unnecessary device fence.
#include "d3d12_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include <optional>

static const char hlsl[] = R"hlsl(
#if COHERENT
globallycoherent
#endif
RWStructuredBuffer<uint> memory : register(u0);
RWStructuredBuffer<uint> results : register(u1);
[numthreads(GROUP, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
  uint value;
  InterlockedAdd(memory[id.x], 0, value);
  [loop] for (uint turn = 0; turn < ROUNDS; turn++) {
#if READ_LOOP
    InterlockedAdd(memory[id.x], 1, value);
#else
    InterlockedAdd(results[id.x], value + turn);
#endif
#if SYNC == 1
    GroupMemoryBarrier();
#elif SYNC == 2
    GroupMemoryBarrierWithGroupSync();
#elif SYNC == 3
    DeviceMemoryBarrier();
#elif SYNC == 4
    DeviceMemoryBarrierWithGroupSync();
#elif SYNC == 5
    AllMemoryBarrierWithGroupSync();
#endif
  }
  results[id.x] = value;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  enum { NoBarrier, GroupMemory, GroupSync, DeviceMemory, DeviceSync, AllSync, Barriers };
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  std::string shaders[2][2][Barriers];
  for (UINT coherent = 0; coherent < 2; coherent++)
    for (UINT reads = 0; reads < 2; reads++)
      for (UINT sync = 0; sync < Barriers; sync++) {
        shaders[coherent][reads][sync] = compiler.compile(
            hlsl, "cs", "cs", {"COHERENT=" + std::to_string(coherent), "READ_LOOP=" + std::to_string(reads),
                                 "SYNC=" + std::to_string(sync), "GROUP=" + std::to_string(group),
                                 "ROUNDS=" + std::to_string(group + 1)}
        );
        if (!expect(!shaders[coherent][reads][sync].empty(), "HLSL did not compile"))
          return verdict();
      }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto bridge = GetModuleHandleA("winemetal.dll");
  CHECK(bridge ? S_OK : E_FAIL);
  auto initialize = reinterpret_cast<decltype(&SM50Initialize)>(GetProcAddress(bridge, "SM50Initialize"));
  auto compile = reinterpret_cast<decltype(&SM50Compile)>(GetProcAddress(bridge, "SM50Compile"));
  auto get_bytes =
      reinterpret_cast<decltype(&SM50GetCompiledBitcode)>(GetProcAddress(bridge, "SM50GetCompiledBitcode"));
  auto destroy = reinterpret_cast<decltype(&SM50Destroy)>(GetProcAddress(bridge, "SM50Destroy"));
  auto free_bitcode = reinterpret_cast<decltype(&SM50DestroyBitcode)>(GetProcAddress(bridge, "SM50DestroyBitcode"));
  auto get_error = reinterpret_cast<decltype(&SM50GetErrorMessage)>(GetProcAddress(bridge, "SM50GetErrorMessage"));
  auto free_error = reinterpret_cast<decltype(&SM50FreeError)>(GetProcAddress(bridge, "SM50FreeError"));
  CHECK(initialize && compile && get_bytes && destroy && free_bitcode && get_error && free_error ? S_OK : E_FAIL);
  // an unavailable correction must still reach the positive case's byte comparison
  constexpr auto correction = []<typename Flag>() {
    if constexpr (requires { Flag::SM50_SHADER_FLAG_COHERENT_GROUP_SYNC; })
      return Flag::SM50_SHADER_FLAG_COHERENT_GROUP_SYNC;
    else
      return Flag{};
  }.operator()<SM50_SHADER_FLAG>();
  // the group sync a DXBC shader carries (sync (sm5 - asm)), "" for none: fxc may give it device scope already
  // (_uglobal), which leaves nothing to promote, or leave it out
  auto disassembled_sync = [&](const std::string &code) -> std::optional<std::string> {
    ComPtr<ID3DBlob> text;
    if (!expect(SUCCEEDED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text)), "fxc disassembles its shader"))
      return std::nullopt;
    std::string_view all((const char *)text->GetBufferPointer(), text->GetBufferSize());
    auto at = all.find("sync");
    return at == all.npos ? std::string{} : std::string(all.substr(at, all.find('\n', at) - at));
  };
  for (UINT coherent = 0; coherent < 2; coherent++)
    for (UINT reads = 0; reads < 2; reads++)
      for (UINT sync = 0; sync < Barriers; sync++)
        for (auto version : {SM50_SHADER_METAL_310, SM50_SHADER_METAL_320}) {
          step("coherent %u, loop read %u, barrier %u, Metal %u", coherent, reads, sync, (UINT)version);
          sm50_shader_t shader{};
          sm50_error_t error{};
          MTL_SHADER_REFLECTION reflection{};
          const auto &code = shaders[coherent][reads][sync];
          auto status = initialize(code.data(), code.size(), &shader, &reflection, &error);
          if (error) {
            char message[sizeof(trace::doing)];
            get_error(error, message, sizeof(message));
            printf("compiler: %s\n", message);
            free_error(error);
          }
          if (!expect(status == 0 && shader, "shader initialized: %d", status)) {
            if (shader)
              destroy(shader);
            return verdict();
          }
          expect(bool(reflection.GroupsWorkTogether) == bool(coherent && reads), "wrong group communication selection");
          std::string compiled[3];
          for (UINT run = 0; run < std::size(compiled); run++) {
            SM50_SHADER_COMMON_DATA common{
                nullptr, SM50_SHADER_COMMON, version, run == 2 ? correction : SM50_SHADER_FLAG{}
            };
            sm50_bitcode_t bitcode{};
            error = {};
            status = compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, "cs_main", &bitcode, &error);
            if (error) {
              char message[sizeof(trace::doing)];
              get_error(error, message, sizeof(message));
              printf("compiler: %s\n", message);
              free_error(error);
            }
            if (expect(status == 0 && bitcode, "shader converted: %d", status)) {
              SM50_COMPILED_BITCODE bytes{};
              get_bytes(bitcode, &bytes);
              if (expect(bytes.Data && bytes.Size, "compiled bytes returned"))
                compiled[run].assign((const char *)bytes.Data, bytes.Size);
            }
            if (bitcode)
              free_bitcode(bitcode);
          }
          destroy(shader);
          expect(compiled[0] == compiled[1], "identical conversions produced different bytes");
          auto instruction = compiler.dxc ? std::optional<std::string>("DXIL") : disassembled_sync(code);
          if (!instruction)
            return verdict();
          const bool promoted = coherent && reads && sync == GroupSync && !instruction->empty() &&
                                instruction->find("_uglobal") == std::string::npos;
          expect((compiled[0] != compiled[2]) == promoted, "device fence selection disagrees with shader and barrier (%s)",
                 instruction->c_str());
        }
  return verdict();
}
