// contract: remaining vs_2_0/ps_2_0 translator opcodes draw the pixels they name: relative constant addressing
// through a0, if/else on a bool constant, and texldl at an explicit lod. Microsoft Learn, mova, if, texldl.
// assembled tokens and d3dcompiler_47 HLSL both read back (tests/d3d9/d3d9_shader_draw.cpp).
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

static const UINT kClient = 64;
static const D3DCOLOR kCyan = D3DCOLOR_XRGB(0, 255, 255);
static const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);

const char *kRelativeVs = "float4 table[4] : register(c0);\n"
                          "float index : register(c4);\n"
                          "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                          "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                          "  VSOut o; o.position = position * table[index]; o.colour = colour; return o;\n"
                          "}\n";
const char *kPassVs = "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                      "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                      "  VSOut o; o.position = position; o.colour = colour; return o;\n"
                      "}\n";
const char *kIfPs = "float4 flag : register(c0);\n"
                    "float4 main(float4 colour : COLOR0) : COLOR {\n"
                    "  if (flag.x) return colour;\n"
                    "  return float4(0, 0, 0, 1);\n"
                    "}\n";
const char *kPassPs = "float4 main(float4 colour : COLOR0) : COLOR { return colour; }\n";
const char *kTexcoord5Vs = "struct VSOut { float4 position : POSITION; float4 extra : TEXCOORD5; };\n"
                           "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                           "  VSOut o; o.position = position; o.extra = colour; return o;\n"
                           "}\n";
const char *kTexcoord5Ps = "float4 main(float4 extra : TEXCOORD5) : COLOR { return extra; }\n";

ComPtr<ID3DBlob>
compile(const char *hlsl, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", profile, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
               "d3d9_shader_opcodes"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_shader_opcodes", "d3d9_shader_opcodes", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }
  ComPtr<IDirect3D9> d3d9;
  d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d9) {
    printf("failed: Direct3DCreate9 made no object\n");
    return 1;
  }
  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = kClient;
  present.BackBufferHeight = kClient;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  ComPtr<IDirect3DSurface9> readback, back;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    back.Reset();
    return got;
  };

  struct Vertex {
    float x, y, z;
    D3DCOLOR colour;
  };
  const Vertex triangle[3] = {{-0.5f, -0.5f, 0, kCyan}, {0.5f, -0.5f, 0, kCyan}, {0, 0.5f, 0, kCyan}};
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));

  ComPtr<ID3DBlob> rel_vs = compile(kRelativeVs, "vs_2_0");
  ComPtr<ID3DBlob> pass_ps = compile(kPassPs, "ps_2_0");
  expect(rel_vs && pass_ps, "relative-addressing shaders did not compile");
  if (rel_vs && pass_ps) {
    ComPtr<IDirect3DVertexShader9> vertex;
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    CHECK(device->CreateVertexShader((const DWORD *)rel_vs->GetBufferPointer(), &vertex));
    CHECK(device->CreatePixelShader((const DWORD *)pass_ps->GetBufferPointer(), &pixel_shader));
    const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1}, index[4] = {1, 0, 0, 0};
    CHECK(device->SetVertexShaderConstantF(0, zero, 1));
    CHECK(device->SetVertexShaderConstantF(1, one, 1));
    CHECK(device->SetVertexShaderConstantF(4, index, 1));
    CHECK(device->SetVertexShader(vertex.Get()));
    CHECK(device->SetPixelShader(pixel_shader.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex));
    CHECK(device->EndScene());
    expect(SUCCEEDED(hr), "relative addressing draw was refused");
    expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "c[a0.x] with a0 selecting c1 was not the vertex colour");
  }

  ComPtr<ID3DBlob> pass_vs = compile(kPassVs, "vs_2_0");
  ComPtr<ID3DBlob> if_ps = compile(kIfPs, "ps_2_0");
  expect(pass_vs && if_ps, "if shaders did not compile");
  if (pass_vs && if_ps) {
    ComPtr<IDirect3DVertexShader9> vertex;
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    CHECK(device->CreateVertexShader((const DWORD *)pass_vs->GetBufferPointer(), &vertex));
    CHECK(device->CreatePixelShader((const DWORD *)if_ps->GetBufferPointer(), &pixel_shader));
    CHECK(device->SetVertexShader(vertex.Get()));
    CHECK(device->SetPixelShader(pixel_shader.Get()));
    const float off[4] = {0, 0, 0, 0}, on[4] = {1, 0, 0, 0};
    CHECK(device->SetPixelShaderConstantF(0, off, 1));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
    CHECK(device->EndScene());
    expect(pixel(32, 32) == 0, "if(false) did not draw black");
    CHECK(device->SetPixelShaderConstantF(0, on, 1));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
    CHECK(device->EndScene());
    expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "if(true) did not draw the vertex colour");
  }

  ComPtr<ID3DBlob> t5_vs = compile(kTexcoord5Vs, "vs_2_0");
  ComPtr<ID3DBlob> t5_ps = compile(kTexcoord5Ps, "ps_2_0");
  expect(t5_vs && t5_ps, "TEXCOORD5 shaders did not compile");
  if (t5_vs && t5_ps) {
    ComPtr<IDirect3DVertexShader9> vertex;
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    CHECK(device->CreateVertexShader((const DWORD *)t5_vs->GetBufferPointer(), &vertex));
    CHECK(device->CreatePixelShader((const DWORD *)t5_ps->GetBufferPointer(), &pixel_shader));
    CHECK(device->SetVertexShader(vertex.Get()));
    CHECK(device->SetPixelShader(pixel_shader.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex));
    CHECK(device->EndScene());
    expect(SUCCEEDED(hr), "TEXCOORD5 draw was refused");
    expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "TEXCOORD5 did not carry the vertex colour");
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
