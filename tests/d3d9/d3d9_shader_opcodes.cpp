// contract: remaining vs_2_0/ps_2_0 translator opcodes draw the pixels they name: relative constant addressing
// through a0, if/else on a bool constant, and texldl at an explicit lod. Microsoft Learn, mova, if, texldl.
// assembled tokens and d3dcompiler_47 HLSL both read back (tests/d3d9/d3d9_shader_draw.cpp). the shader-model 1-3
// opcodes that have no HLSL spelling (sgn, lit, defi, defb, setp, breakp) are encoded as d3dbc tokens and drawn too,
// and a program naming an opcode the translator does not read is refused by CreatePixelShader.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>

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
const char *kBumpVs = "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; float2 uv0 : TEXCOORD0; "
                      "float2 uv1 : TEXCOORD1; };\n"
                      "VSOut main(float4 position : POSITION, float4 colour : COLOR0, float2 uv0 : TEXCOORD0, "
                      "float2 uv1 : TEXCOORD1) {\n"
                      "  VSOut o; o.position = position; o.colour = colour; o.uv0 = uv0; o.uv1 = uv1; return o;\n"
                      "}\n";

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
  struct BumpVertex {
    float x, y, z;
    D3DCOLOR colour;
    float u0, v0, u1, v1;
  };
  const BumpVertex bump_triangle[3] = {{-0.5f, -0.5f, 0, kCyan, 0.25f, 0.5f, 0.5f, 0.5f},
                                       {0.5f, -0.5f, 0, kCyan, 0.25f, 0.5f, 0.5f, 0.5f},
                                       {0, 0.5f, 0, kCyan, 0.25f, 0.5f, 0.5f, 0.5f}};
  const D3DVERTEXELEMENT9 bump_elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> bump_declaration;
  CHECK(device->CreateVertexDeclaration(bump_elements, &bump_declaration));

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

  // the opcodes no HLSL spelling reaches, encoded as d3dbc tokens: a register is
  // 0x80000000 | type << 28 | mask-or-swizzle << 16 | number, and an instruction token is
  // (length << 24) | comparison << 16 | opcode
  auto reg = [](DWORD type, DWORD number, DWORD field) {
    // the type's low three bits sit at 28 and the bits types 8 through 19 need at 11 (D3DSP_REGTYPE_MASK2), which is
    // what makes oC0 r0 and b0 a texture register when read the first way
    return 0x80000000u | ((type & 7) << D3DSP_REGTYPE_SHIFT) | ((type >> 3) << 11) | (field << 16) | number;
  };
  auto dreg = [&](DWORD type, DWORD number, DWORD mask = 0xF) { return reg(type, number, mask); };
  auto sreg = [&](DWORD type, DWORD number, DWORD swizzle = 0xE4) { return reg(type, number, swizzle); };
  auto ins = [](DWORD opcode, DWORD length, DWORD comparison = 0) {
    return (length << 24) | (comparison << 16) | opcode;
  };
  auto close_to = [](D3DCOLOR got, int r, int g, int b, int tolerance = 2) {
    auto close = [&](int one, int other) { return (one > other ? one - other : other - one) <= tolerance; };
    const bool ok = close((int)((got >> 16) & 0xFF), r) && close((int)((got >> 8) & 0xFF), g) &&
                    close((int)(got & 0xFF), b);
    if (!ok)
      printf("  colour: got %06x, wanted %02x%02x%02x\n", (unsigned)got, r, g, b);
    return ok;
  };
  auto draw = [&](IDirect3DVertexShader9 *vertex, IDirect3DPixelShader9 *pixel_shader) {
    if (FAILED(device->SetVertexShader(vertex)) || FAILED(device->SetPixelShader(pixel_shader)) ||
        FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0)) || FAILED(device->BeginScene()))
      return E_FAIL;
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex));
    if (FAILED(device->EndScene()))
      return E_FAIL;
    return hr;
  };

  // vs_2_0: lit c0 = (1, max(0.5, 0), pow(0.25, 2), 1) = (1, 0.5, 0.0625, 1) and sgn c1 = (-1, 0, 1, 0), summed to
  // (0, 0.5, 1.0625, 1): green 128 and blue 255 distinguish lit's 1 in x from a copy of src.x, its power from src.y and
  // sgn's negative from its zero
  const DWORD lit_vs_tokens[] = {
      D3DVS_VERSION(2, 0),
      ins(D3DSIO_DCL, 2), 0x00000000, dreg(D3DSPR_INPUT, 0),
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 0), 0x3F000000, 0x3E800000, 0x00000000, 0x40000000,
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 1), 0xC0000000, 0x00000000, 0x40400000, 0x00000000,
      ins(D3DSIO_LIT, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 0),
      ins(D3DSIO_SGN, 2), dreg(D3DSPR_TEMP, 1), sreg(D3DSPR_CONST, 1),
      ins(D3DSIO_ADD, 3), dreg(D3DSPR_TEMP, 2), sreg(D3DSPR_TEMP, 0), sreg(D3DSPR_TEMP, 1),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_ATTROUT, 0), sreg(D3DSPR_TEMP, 2),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_RASTOUT, 0), sreg(D3DSPR_INPUT, 0),
      ins(D3DSIO_END, 0),
  };
  // a non-positive src.x zeroes dst.z even with a non-zero exponent
  const DWORD lit_zero_vs_tokens[] = {
      D3DVS_VERSION(2, 0),
      ins(D3DSIO_DCL, 2), 0x00000000, dreg(D3DSPR_INPUT, 0),
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 0), 0xBF800000, 0x40800000, 0x00000000, 0x40000000,
      ins(D3DSIO_LIT, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 0),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_ATTROUT, 0), sreg(D3DSPR_TEMP, 0),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_RASTOUT, 0), sreg(D3DSPR_INPUT, 0),
      ins(D3DSIO_END, 0),
  };
  // ps_3_0: defb b0 gates a setp_gt p0 that a breakp inside a defi-counted loop reads. with c0.x above c1.x the loop
  // breaks on its first pass and r0 stays green; with it below, p0 is false, the loop runs and r0 turns red. a
  // translator that dropped defi, defb, setp or breakp lands on one of those two pixels, whichever way it guessed
  const DWORD setp_ps_tokens[] = {
      D3DPS_VERSION(3, 0),
      ins(D3DSIO_DEFI, 5), dreg(D3DSPR_CONSTINT, 0), 4, 0, 0, 0,
      ins(D3DSIO_DEFB, 2), dreg(D3DSPR_CONSTBOOL, 0), 1,
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 1), 0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000,
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 2), 0x00000000, 0x3F800000, 0x00000000, 0x3F800000,
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 3), 0x3F800000, 0x00000000, 0x00000000, 0x3F800000,
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 2),
      ins(D3DSIO_IF, 1), sreg(D3DSPR_CONSTBOOL, 0),
      ins(D3DSIO_SETP, 3, D3DSPC_GT), dreg(D3DSPR_PREDICATE, 0), sreg(D3DSPR_CONST, 0, 0x0000),
      sreg(D3DSPR_CONST, 1, 0x0000),
      ins(D3DSIO_LOOP, 2), sreg(D3DSPR_LOOP, 0), sreg(D3DSPR_CONSTINT, 0),
      ins(D3DSIO_BREAKP, 1), sreg(D3DSPR_PREDICATE, 0, 0x0000),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 3),
      ins(D3DSIO_ENDLOOP, 0),
      ins(D3DSIO_ELSE, 0),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 2),
      ins(D3DSIO_ENDIF, 0),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_COLOROUT, 0), sreg(D3DSPR_TEMP, 0),
      ins(D3DSIO_END, 0),
  };

  ComPtr<IDirect3DVertexShader9> lit_vertex, lit_zero_vertex;
  ComPtr<IDirect3DPixelShader9> setp_shader;
  expect(SUCCEEDED(device->CreateVertexShader(lit_vs_tokens, &lit_vertex)), "a vs_2_0 lit/sgn was refused");
  expect(SUCCEEDED(device->CreateVertexShader(lit_zero_vs_tokens, &lit_zero_vertex)), "a vs_2_0 lit was refused");
  expect(SUCCEEDED(device->CreatePixelShader(setp_ps_tokens, &setp_shader)), "a ps_3_0 setp/breakp was refused");
  if (lit_vertex && lit_zero_vertex && setp_shader && pass_ps && pass_vs) {
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    CHECK(device->CreatePixelShader((const DWORD *)pass_ps->GetBufferPointer(), &pixel_shader));
    expect(SUCCEEDED(draw(lit_vertex.Get(), pixel_shader.Get())), "lit/sgn draw was refused");
    expect(close_to(pixel(32, 32), 0, 128, 255),
           "lit/sgn was not (1, 0.5, 0.0625, 1) + (-1, 0, 1, 0) clamped by the target");
    expect(SUCCEEDED(draw(lit_zero_vertex.Get(), pixel_shader.Get())), "lit with a negative x draw was refused");
    expect(close_to(pixel(32, 32), 255, 0, 0), "lit with a negative x did not zero dst.z");

    ComPtr<IDirect3DVertexShader9> vertex;
    CHECK(device->CreateVertexShader((const DWORD *)pass_vs->GetBufferPointer(), &vertex));
    const float above[4] = {0.75f, 0.75f, 0.75f, 1}, below[4] = {0.25f, 0.25f, 0.25f, 1};
    CHECK(device->SetPixelShaderConstantF(0, above, 1));
    expect(SUCCEEDED(draw(vertex.Get(), setp_shader.Get())), "setp/breakp draw was refused");
    expect(close_to(pixel(32, 32), 0, 255, 0), "a true setp did not break the loop");
    CHECK(device->SetPixelShaderConstantF(0, below, 1));
    expect(SUCCEEDED(draw(vertex.Get(), setp_shader.Get())), "false setp draw was refused");
    expect(close_to(pixel(32, 32), 255, 0, 0), "a false setp broke the loop (or defi's count did not run it)");
  }

  // ps_1_2 bem: dst.xy = src0.xy + the stage's bump-environment matrix times src1.xy, with the stage the destination's
  // register number. t0 = (0.25, 0.5) and t1 = (0.5, 0.5) with m00, m01, m10, m11 = (0.5, 0.25, 0.5, 0.5) give
  // x = 0.25 + 0.5*0.5 + 0.5*0.5 = 0.75 and y = 0.5 + 0.25*0.5 + 0.5*0.5 = 0.875, so red 191 and green 223 tell a
  // matrix that was not read (64, 128) from a transposed one (159, 223)
  {
    const DWORD bem_ps_tokens[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_BEM, dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_TEXTURE, 0), sreg(D3DSPR_TEXTURE, 1),
        D3DSIO_END,
    };
    ComPtr<ID3DBlob> bump_vs = compile(kBumpVs, "vs_2_0");
    expect(bump_vs != nullptr, "the bump-environment vertex shader did not compile");
    if (bump_vs) {
      ComPtr<IDirect3DVertexShader9> vertex;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      CHECK(device->CreateVertexShader((const DWORD *)bump_vs->GetBufferPointer(), &vertex));
      CHECK(device->CreatePixelShader(bem_ps_tokens, &pixel_shader));
      const float bits[4] = {0.5f, 0.25f, 0.5f, 0.5f};
      DWORD raw[4];
      memcpy(raw, bits, sizeof(raw));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT00, raw[0]));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT01, raw[1]));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT10, raw[2]));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT11, raw[3]));
      CHECK(device->SetVertexDeclaration(bump_declaration.Get()));
      CHECK(device->SetVertexShader(vertex.Get()));
      CHECK(device->SetPixelShader(pixel_shader.Get()));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      CHECK(device->BeginScene());
      HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, bump_triangle, sizeof(BumpVertex));
      CHECK(device->EndScene());
      expect(SUCCEEDED(hr), "bem draw was refused");
      expect(close_to(pixel(32, 32), 191, 223, 0, 3), "bem was not src0 + the bump matrix times src1");
      // the matrix is part of the variant key: zeroing it has to translate a new program, not draw the old immediates
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT00, 0));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT01, 0));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT10, 0));
      CHECK(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT11, 0));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      CHECK(device->BeginScene());
      hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, bump_triangle, sizeof(BumpVertex));
      CHECK(device->EndScene());
      expect(SUCCEEDED(hr), "the second bem draw was refused");
      expect(close_to(pixel(32, 32), 64, 128, 0, 3),
             "a changed bump matrix reused the previous program (the variant is not keyed by it)");
      CHECK(device->SetVertexDeclaration(declaration.Get()));
    }
  }

  // ps_2_0 ifc_gt: c0.x above c1.x takes the first branch, below takes the else. the comparison shares setp's
  // encoding, and an ifc_gt drawn as "src0 <= src1" swaps the two colours
  const DWORD ifc_ps_tokens[] = {
      D3DPS_VERSION(2, 0),
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 2), 0x00000000, 0x3F800000, 0x00000000, 0x3F800000,
      ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 3), 0x3F800000, 0x00000000, 0x00000000, 0x3F800000,
      ins(D3DSIO_IFC, 2, D3DSPC_GT), sreg(D3DSPR_CONST, 0, 0x0000), sreg(D3DSPR_CONST, 1, 0x0000),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 2),
      ins(D3DSIO_ELSE, 0),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 3),
      ins(D3DSIO_ENDIF, 0),
      ins(D3DSIO_MOV, 2), dreg(D3DSPR_COLOROUT, 0), sreg(D3DSPR_TEMP, 0),
      ins(D3DSIO_END, 0),
  };
  ComPtr<IDirect3DPixelShader9> ifc_shader;
  expect(SUCCEEDED(device->CreatePixelShader(ifc_ps_tokens, &ifc_shader)), "a ps_2_0 ifc_gt was refused");
  if (ifc_shader && pass_vs) {
    ComPtr<IDirect3DVertexShader9> vertex;
    CHECK(device->CreateVertexShader((const DWORD *)pass_vs->GetBufferPointer(), &vertex));
    const float above[4] = {0.75f, 0, 0, 0}, below[4] = {0.25f, 0, 0, 0}, half[4] = {0.5f, 0, 0, 0};
    CHECK(device->SetPixelShaderConstantF(0, above, 1));
    CHECK(device->SetPixelShaderConstantF(1, half, 1));
    expect(SUCCEEDED(draw(vertex.Get(), ifc_shader.Get())), "ifc_gt draw was refused");
    expect(close_to(pixel(32, 32), 0, 255, 0), "ifc_gt with src0 above src1 did not take the first branch");
    CHECK(device->SetPixelShaderConstantF(0, below, 1));
    expect(SUCCEEDED(draw(vertex.Get(), ifc_shader.Get())), "ifc_gt false draw was refused");
    expect(close_to(pixel(32, 32), 255, 0, 0), "ifc_gt with src0 below src1 did not take the else branch");
  }

  // boundary maths: rcp(0) and rsq(0) are +inf, which the 8-bit target stores as 255 where a guard that answered 0 would
  // show black; pow(0.5, 2) is 0.25 (64) and 0.5*0.5 is 0.25 rounded to 64, where truncation would show 63
  {
    const DWORD def_c0[] = {ins(D3DSIO_DEF, 5), dreg(D3DSPR_CONST, 0), 0x00000000, 0x3F000000, 0x40000000, 0x40400000};
    auto draw_math = [&](const char *what, DWORD opcode, DWORD swizzle_x, DWORD swizzle_y, bool unary, int expected) {
      const DWORD program[] = {
          D3DVS_VERSION(2, 0),
          ins(D3DSIO_DCL, 2), 0x00000000, dreg(D3DSPR_INPUT, 0),
          def_c0[0], def_c0[1], def_c0[2], def_c0[3], def_c0[4], def_c0[5],
          ins(opcode, unary ? 2 : 3), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_CONST, 0, swizzle_x),
      };
      const DWORD tail[] = {
          ins(D3DSIO_MOV, 2), dreg(D3DSPR_ATTROUT, 0), sreg(D3DSPR_TEMP, 0),
          ins(D3DSIO_MOV, 2), dreg(D3DSPR_RASTOUT, 0), sreg(D3DSPR_INPUT, 0),
          ins(D3DSIO_END, 0),
      };
      std::vector<DWORD> tokens(program, program + sizeof(program) / sizeof(program[0]));
      if (!unary)
        tokens.push_back(sreg(D3DSPR_CONST, 0, swizzle_y));
      tokens.insert(tokens.end(), tail, tail + sizeof(tail) / sizeof(tail[0]));
      ComPtr<IDirect3DVertexShader9> vertex;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      if (!pass_ps || FAILED(device->CreateVertexShader(tokens.data(), &vertex)) ||
          FAILED(device->CreatePixelShader((const DWORD *)pass_ps->GetBufferPointer(), &pixel_shader))) {
        expect(false, what);
        return;
      }
      expect(SUCCEEDED(draw(vertex.Get(), pixel_shader.Get())), what);
      expect(close_to(pixel(32, 32), expected, expected, expected, 1), what);
    };
    draw_math("rcp(0) did not write +inf's saturated 255", D3DSIO_RCP, 0x0000, 0, true, 255);
    draw_math("rsq(0) did not write +inf's saturated 255", D3DSIO_RSQ, 0x0000, 0, true, 255);
    draw_math("pow(0.5, 2) was not 0.25", D3DSIO_POW, 0x55, 0xAA, false, 64);
    draw_math("0.5*0.5 was not 0.25 rounded to 64", D3DSIO_MUL, 0x55, 0x55, false, 64);
  }

  // a program naming an opcode the translator does not read is refused, not drawn as something else
  {
    const DWORD unknown_ps[] = {D3DPS_VERSION(2, 0), ins(0x7FFF, 1), dreg(D3DSPR_TEMP, 0), ins(D3DSIO_END, 0)};
    ComPtr<IDirect3DPixelShader9> rejected;
    expect(FAILED(device->CreatePixelShader(unknown_ps, &rejected)), "an unknown opcode was accepted");
  }

  // a vs_3_0 samples through four vertex texture samplers (s0-s3): a dcl_2d s4 names a vertex sampler D3D9 does not
  // have, and CreateVertexShader refuses it rather than drawing from a stage the device never binds
  {
    const DWORD vertex_sampler4[] = {
        D3DVS_VERSION(3, 0),
        ins(D3DSIO_DCL, 2), 0x00000000, dreg(D3DSPR_INPUT, 0),
        ins(D3DSIO_DCL, 2), 0x00000005, dreg(D3DSPR_INPUT, 1),
        ins(D3DSIO_DCL, 2), 0x00000000, dreg(D3DSPR_OUTPUT, 0),
        ins(D3DSIO_DCL, 2), 0x00000005, dreg(D3DSPR_OUTPUT, 1),
        ins(D3DSIO_DCL, 2), 0x90000000, dreg(D3DSPR_SAMPLER, 4),
        ins(D3DSIO_TEXLDL, 3), dreg(D3DSPR_TEMP, 0), sreg(D3DSPR_INPUT, 1), sreg(D3DSPR_SAMPLER, 4),
        ins(D3DSIO_MOV, 2), dreg(D3DSPR_OUTPUT, 0), sreg(D3DSPR_INPUT, 0),
        ins(D3DSIO_MOV, 2), dreg(D3DSPR_OUTPUT, 1), sreg(D3DSPR_TEMP, 0),
        ins(D3DSIO_END, 0),
    };
    ComPtr<IDirect3DVertexShader9> refused;
    expect(FAILED(device->CreateVertexShader(vertex_sampler4, &refused)),
           "a vs_3_0 dcl_2d s4 was accepted (a vertex shader has four texture samplers)");
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
