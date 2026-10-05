// shared by the D3D12 tests. every test runs one contract through one shader front end, named by argv[1]:
// "dxil" compiles its HLSL with DXC (dxcompiler.dll, SM 6.0), "dxbc" with Microsoft's d3dcompiler_47 (SM 5.1; Wine's
// lacks SM 5 methods), so the tests carry no binary shaders and both of Mullion's shader front ends meet the same
// expectations.
// a test exits 0 on pass, 1 on failure and 77 when its compiler is missing (skip).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

struct Compiler {
  ComPtr<IDxcCompiler3> dxc; // null for DXBC

  // `stage` is "vs", "ps" or "cs", or a whole profile ("cs_6_6") for HLSL only DXC compiles, which then fails for
  // DXBC; `defines` ("NAME=VALUE") pass test constants into the HLSL, so both sides use one value. `flags` go to DXC
  template <size_t N>
  std::string compile(
      const char (&hlsl)[N], const char *entry, const char *stage, std::vector<std::string> defines = {},
      std::vector<std::wstring> flags = {}
  ) const {
    std::vector<std::string> names, values;
    for (auto &d : defines) {
      names.push_back(d.substr(0, d.find('=')));
      values.push_back(d.substr(d.find('=') + 1));
    }
    std::string out, profile = stage;
    if (profile.find('_') == std::string::npos)
      profile += dxc ? "_6_0" : "_5_1";
    else if (!dxc)
      return out;
    if (!dxc) {
      std::vector<D3D_SHADER_MACRO> macros;
      for (size_t i = 0; i < defines.size(); i++)
        macros.push_back({names[i].c_str(), values[i].c_str()});
      macros.push_back({});
      ComPtr<ID3DBlob> code, errors;
      if (SUCCEEDED(
              D3DCompile(hlsl, N - 1, nullptr, macros.data(), nullptr, entry, profile.c_str(), 0, 0, &code, &errors)
          ))
        out.assign((const char *)code->GetBufferPointer(), code->GetBufferSize());
      else if (errors)
        printf("%s: %.*s\n", entry, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
      return out;
    }
    auto wide = [](const std::string &s) { return std::wstring(s.begin(), s.end()); };
    std::vector<std::wstring> args = {L"-E", wide(entry), L"-T", wide(profile)};
    args.insert(args.end(), flags.begin(), flags.end());
    for (auto &d : defines)
      args.insert(args.end(), {L"-D", wide(d)});
    std::vector<const wchar_t *> argv;
    for (auto &a : args)
      argv.push_back(a.c_str());
    DxcBuffer src{hlsl, N - 1, DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    ComPtr<IDxcBlob> blob;
    if (SUCCEEDED(dxc->Compile(&src, argv.data(), argv.size(), nullptr, IID_PPV_ARGS(&result))) &&
        SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr)) && blob)
      out.assign((const char *)blob->GetBufferPointer(), blob->GetBufferSize());
    ComPtr<IDxcBlobUtf8> errors;
    if (out.empty() && result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors)
      printf("%s: %.*s\n", entry, (int)errors->GetStringLength(), errors->GetStringPointer());
    return out;
  }
};

// the front end named by argv[1], or false when its compiler is missing
inline bool
front_end(int argc, char **argv, Compiler &compiler) {
  if (argc > 1 && !strcmp(argv[1], "dxbc"))
    return true;
  auto dll = LoadLibraryA("dxcompiler.dll");
  auto create = dll ? (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance") : nullptr;
  return create && SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler.dxc)));
}

inline D3D12_SHADER_BYTECODE
bytecode(const std::string &code) {
  return {code.data(), code.size()};
}

inline ComPtr<ID3D12RootSignature>
root_signature(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC &desc) {
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3D12RootSignature> rs;
  if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
  return rs;
}

inline ComPtr<ID3D12Resource>
buffer(
    ID3D12Device *device, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
) {
  D3D12_HEAP_PROPERTIES props{heap};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0,    size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR,  flags};
  ComPtr<ID3D12Resource> res;
  device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
  return res;
}

inline void
transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
  barrier.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
  list->ResourceBarrier(1, &barrier);
}

// runs a closed list and waits for the GPU
inline HRESULT
execute(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list) {
  ComPtr<ID3D12Fence> fence;
  HRESULT hr;
  if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    return hr;
  ID3D12CommandList *lists[] = {list};
  queue->ExecuteCommandLists(1, lists);
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr = queue->Signal(fence.Get(), 1)) || FAILED(hr = fence->SetEventOnCompletion(1, event)))
    return hr;
  WaitForSingleObject(event, INFINITE);
  CloseHandle(event);
  return S_OK;
}

// closes the list, runs it and waits for the GPU
inline HRESULT
submit(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list) {
  HRESULT hr = list->Close();
  return FAILED(hr) ? hr : execute(device, queue, list);
}
