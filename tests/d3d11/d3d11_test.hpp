// shared by the D3D11 tests. HLSL is compiled by Microsoft's d3dcompiler_47 (Wine's lacks SM 5 methods), so the tests
// carry no binary shaders. a test exits 0 on pass and 1 on failure.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
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

// `stage` is "vs", "gs" or "ps"; `defines` ("NAME=VALUE") pass test constants into the HLSL, so both sides use one value
template <size_t N>
ComPtr<ID3DBlob>
compile(const char (&hlsl)[N], const char *entry, const char *stage, const std::vector<std::string> &defines = {}) {
  std::vector<std::string> names, values;
  std::vector<D3D_SHADER_MACRO> macros;
  for (auto &d : defines) {
    names.push_back(d.substr(0, d.find('=')));
    values.push_back(d.substr(d.find('=') + 1));
  }
  for (size_t i = 0; i < defines.size(); i++)
    macros.push_back({names[i].c_str(), values[i].c_str()});
  macros.push_back({});
  ComPtr<ID3DBlob> code, errors;
  auto profile = std::string(stage) + "_5_0";
  if (FAILED(D3DCompile(hlsl, N - 1, nullptr, macros.data(), nullptr, entry, profile.c_str(), 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", entry, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

// a buffer of `bytes`, every 32-bit word of it `fill`
inline ComPtr<ID3D11Buffer>
buffer(ID3D11Device *device, UINT bytes, UINT bind, uint32_t fill = 0) {
  std::vector<uint32_t> words((bytes + 3) / 4, fill);
  D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DEFAULT, bind};
  D3D11_SUBRESOURCE_DATA data{words.data()};
  ComPtr<ID3D11Buffer> out;
  device->CreateBuffer(&desc, &data, &out);
  return out;
}

// the contents of a buffer, read back through a staging copy
inline std::vector<uint32_t>
read(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Buffer *source) {
  D3D11_BUFFER_DESC desc;
  source->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  std::vector<uint32_t> words(desc.ByteWidth / 4);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(device->CreateBuffer(&desc, nullptr, &staging)))
    return {};
  context->CopyResource(staging.Get(), source);
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return {};
  memcpy(words.data(), mapped.pData, words.size() * 4);
  context->Unmap(staging.Get(), 0);
  return words;
}
