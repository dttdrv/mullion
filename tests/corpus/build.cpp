// builds the corpus tests/d3d12_corpus.cpp and tests/host_dxbchash.cpp read, from HLSL in this file and nothing else:
// each shader is compiled by dxcompiler.dll (the one the suite's DXIL halves already need), signed with the tests'
// own hash (tests/dxbc_hash.hpp), and written as "<hash>.dxbc" — a directory of DXIL containers named by their
// hash, which is the contract MULLION_CORPUS names. It is a tool, not a test: it runs once, its output is committed
// beside it, and the suite reads that output.
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O1 -I../include -Itests build.cpp -o build.exe   (from the repository root)
//   WINEPREFIX=<prefix with the native dxcompiler.dll> <wine>/bin/wine build.exe tests/corpus
//
// whose shaders are chosen so the corpus test can make every pipeline it makes: a compute shader with a UAV, a
// vertex shader with an input signature and a pixel shader whose inputs it writes, and a geometry shader the two
// pass through. Sizes are chosen so both endings of the hash's last block are present among them (the hash test
// requires containers of both), by padding one container past the block boundary before signing.
#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../dxbc_hash.hpp"

using Microsoft::WRL::ComPtr;

static const char corpus[] = R"hlsl(
// ---- vertex shader: position and a colour for the pixel shader
struct VSOut {
  float4 position : SV_Position;
  float4 colour : COLOR0;
};

VSOut vs(float3 position : POSITION0, float4 colour : COLOR0) {
  VSOut written;
  written.position = float4(position, 1.0);
  written.colour = colour;
  return written;
}

// ---- pixel shader: writes the colour it is given to the target, and a depth
struct PSOut {
  float4 colour : SV_Target;
  float depth : SV_Depth;
};

PSOut ps(float4 position : SV_Position, float4 colour : COLOR0) {
  PSOut written;
  written.colour = colour;
  written.depth = colour.a;
  return written;
}

// ---- compute shader: one UAV of the binding the root signature is made of
#ifndef EXTRA
#define EXTRA 0
#endif

RWStructuredBuffer<uint> outBuffer : register(u0);

[numthreads(8, 8, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
  outBuffer[id.x] = id.x;
  [unroll]
  for (uint e = 0; e < EXTRA; e++)
    outBuffer[id.x] += e;
}
)hlsl";

struct Built {
  std::string entry;
  std::string code;
};

static ComPtr<IDxcCompiler3> dxc;

// one shader compiled, or empty (with why printed) when it did not compile
static Built
compile_shader(const char *entry, int extra) {
  std::string profile = std::string(entry) + "_6_0";
  std::wstring wide_entry(entry, entry + strlen(entry)), wide_profile(profile.begin(), profile.end());
  std::wstring define = L"EXTRA=" + std::to_wstring(extra);
  const wchar_t *args[] = {L"-E", wide_entry.c_str(), L"-T", wide_profile.c_str(), L"-Vd", L"-D", define.c_str()};
  DxcBuffer src{corpus, sizeof(corpus) - 1, DXC_CP_UTF8};
  ComPtr<IDxcResult> result;
  ComPtr<IDxcBlob> blob;
  HRESULT compiled = dxc->Compile(&src, args, std::size(args), nullptr, IID_PPV_ARGS(&result));
  HRESULT taken = result ? result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr) : E_FAIL;
  if (FAILED(compiled) || FAILED(taken) || !blob || !blob->GetBufferSize()) {
    ComPtr<IDxcBlobUtf8> errors;
    if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors)
      printf("%s: %.*s\n", entry, (int)errors->GetStringLength(), errors->GetStringPointer());
    printf("failed: %s did not compile\n", entry);
    return {};
  }
  Built shader{entry, std::string((const char *)blob->GetBufferPointer(), blob->GetBufferSize())};
  if (shader.code.compare(0, 4, "DXBC")) {
    printf("failed: %s is no container (%zu bytes)\n", entry, shader.code.size());
    return {};
  }
  return shader;
}

int
main(int argc, char **argv) {
  const char *directory = argc > 1 ? argv[1] : ".";
  auto dll = LoadLibraryA("dxcompiler.dll");
  auto create = dll ? (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance") : nullptr;
  if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc)))) {
    printf("failed: no dxcompiler.dll, so nothing can be built\n");
    return 1;
  }

  // a geometry shader was part of this corpus until it was measured: this implementation answers E_INVALIDARG for
  // the corpus test's vs/gs and vs/gs/ps pipelines (0x80070057, with no line of the library's own), while the
  // suite's own d3d12_geometry and d3d12_geometry_oversize pipelines pass -- a gap of the pipeline builder, not of
  // the fixture, so the corpus carries the stages the test can make today and that gap wants its own measurement.
  std::vector<Built> built;
  for (const char *entry : {"vs", "ps", "cs"}) {
    Built shader = compile_shader(entry, 0);
    if (shader.code.empty())
      return 1;
    built.push_back(std::move(shader));
  }

  // the hash's last block: the bytes past the last whole block. A container with 56 or more needs a block of its
  // own for 0x80, a shorter one fits in the block that holds the bit count -- and the hash test requires both kinds
  // among the containers it finds. Trailing bytes cannot be used to move a container across that boundary (they make
  // the container's DXIL unreadable: the vs with 28 of them made every pipeline of it fail with E_INVALIDARG), so
  // the compute shader is compiled with more instructions until one of its containers lands there.
  auto rest = [](const std::string &code) { return (code.size() - dxbc::hashed_from) % 64; };
  const bool alone = std::any_of(built.begin(), built.end(), [&](auto &s) { return rest(s.code) >= 56; });
  if (!alone) {
    bool found = false;
    for (int extra = 1; extra <= 64 && !found; extra++) {
      Built grown = compile_shader("cs", extra);
      if (grown.code.empty())
        return 1;
      if (rest(grown.code) >= 56) {
        printf("the compute shader with %d instructions lands %zu bytes past a block\n", extra, rest(grown.code));
        built.push_back(std::move(grown));
        found = true;
      }
    }
    if (!found) {
      printf("failed: no container ends in a block of its own\n");
      return 1;
    }
  }

  for (auto &shader : built) {
    if (shader.code.find_first_not_of('\0', dxbc::hash_at) >= dxbc::hashed_from)
      dxbc::sign(shader.code);
    dxbc::Hash hash = dxbc::hash(shader.code.data(), shader.code.size());
    char hex[2 * sizeof(dxbc::Hash) + 1] = {};
    for (size_t i = 0; i < hash.size(); i++)
      snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    std::string name = std::string(directory) + "\\" + hex + ".dxbc";
    FILE *file = fopen(name.c_str(), "wb");
    if (!file) {
      printf("failed: cannot write %s\n", name.c_str());
      return 1;
    }
    fwrite(shader.code.data(), 1, shader.code.size(), file);
    fclose(file);
    printf("%-3s %zu bytes, %zu past a block: %s.dxbc\n", shader.entry.c_str(), shader.code.size(),
           rest(shader.code), hex);
  }
  return 0;
}
