// contract: a game's own Direct3D 9 shaders draw, through the translator that turns their d3dbc programs into the DXBC
// this build's D3D11 device compiles, and the pixels are what the shaders say.
// - a vertex shader's inputs are the vertex declaration's elements (v0 the first, v1 the second), its oPos is the
//   position, oD0 the first colour; a pixel shader's v0 is that colour and oC0 the target. the declaration and the FVF
//   are exclusive: each clears the other.
// - the float and pixel constant tables reach the shader: a shader that multiplies by c0/c1 draws black when the table
//   is zero and the colour when it is one.
// - both a hand-written program (the tokens of this test, so the translator is proved without a compiler) and one
//   Microsoft's compiler makes of HLSL at test time (a real game's program) draw the same pixels.
// - a program that samples a texture this build does not have (a cube, a volume: tests/d3d9/d3d9_texture_draw.cpp draws
//   the 2D ones) refuses the draw with D3DERR_INVALIDCALL rather than drawing something else.
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/dx9-graphics-reference-asm-vs-instructions
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/dx9-graphics-reference-asm-ps-instructions
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace {

const UINT kClient = 64;
const D3DCOLOR kCyan = D3DCOLOR_XRGB(0, 255, 255);
// what a frame is cleared to: a colour neither shader writes, so a pixel of it says the draw left that pixel alone
const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);
const DWORD kClearPixel = kClear & 0x00FFFFFF;

// a vertex the declaration reads: a float3 position and a D3DCOLOR
struct Vertex {
  float x, y, z;
  D3DCOLOR colour;
};

// the hand-written vs_2_0 program: dcl_position v0, dcl_color v1, mov oPos, v0, mov oD0, v1. a declaration is its usage
// and then the register it fills; oPos is the position output (rasterizer output 0) and oD0 the first colour (attribute
// output 0), each written whole
const DWORD kHandVertexShader[] = {
    0xFFFE0200, // vs_2_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x0200001F, 0x0000000A, 0x900F0001, // dcl_color v1
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x02000001, 0xD00F0000, 0x90E40001, // mov oD0, v1
    0x0000FFFF, // end
};

// the hand-written ps_2_0 program: dcl v0 (the colour the vertex shader interpolates), mul oC0, v0, c1 (its first
// target, the colour times the pixel constant the test writes): the constant is what makes the draw's colour the
// table's, so a zero c1 is a black pixel and a c1 of ones is the vertex colour
const DWORD kHandPixelShader[] = {
    0xFFFF0200, // ps_2_0
    0x0200001F, 0x0000000A, 0x900F0000, // dcl v0
    0x03000005, 0x800F0800, 0x90E40000, 0xA0E40001, // mul oC0, v0, c1
    0x0000FFFF, // end
};

// the same two stages, in HLSL, for Microsoft's compiler to make a real program of
// a game's vertex shader: the position scales by c0 and the colour passes through to the pixel shader, which is what a
// program that draws something of its own does
const char *kVertexHlsl = "uniform float4 factor : register(c0);\n"
                          "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                          "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                          "  VSOut o;\n"
                          "  o.position = position * factor;\n"
                          "  o.colour = colour;\n"
                          "  return o;\n"
                          "}\n";
const char *kPixelHlsl = "uniform float4 tint : register(c1);\n"
                         "float4 main(float4 colour : COLOR0) : COLOR {\n"
                         "  return colour * tint;\n"
                         "}\n";
// a pixel shader whose sampler is a cube texture's, which this build does not sample (it reads 2D ones)
const char *kCubeSamplerHlsl = "samplerCUBE s0 : register(s0);\n"
                               "float4 main(float3 uv : TEXCOORD0) : COLOR { return texCUBE(s0, uv); }\n";

ComPtr<ID3DBlob>
compile(const char *hlsl, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", profile, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

} // namespace

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_shader_draw"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_shader_draw", "d3d9_shader_draw", WS_OVERLAPPEDWINDOW, 0, 0,
                              window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, nullptr,
                              nullptr, window_class.hInstance, nullptr);
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
  present.MultiSampleType = D3DMULTISAMPLE_NONE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  // the same programs as shader model 5, which is what the translator writes: a container of the format's own, for
  // comparing this build's writer against the compiler that makes them
  if (const char *dump = getenv("MULLION_DXBC_DUMP")) {
    const struct {
      const char *hlsl;
      const char *profile;
      const char *name;
    } references[] = {{kVertexHlsl, "vs_5_0", "ref_vs.bin"},
                       {kVertexHlsl, "vs_2_0", "ref_vs2.bin"},
                       {kPixelHlsl, "ps_2_0", "ref_ps2.bin"}};
    for (const auto &reference : references) {
      ComPtr<ID3DBlob> code = compile(reference.hlsl, reference.profile);
      if (!code)
        continue;
      const std::string path = std::string(dump) + "\\" + reference.name;
      if (FILE *file = fopen(path.c_str(), "wb")) {
        fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), file);
        fclose(file);
      }
    }
  }

  // the declaration: a float3 position and a D3DCOLOR, which is v0 and v1
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  ComPtr<IDirect3DVertexDeclaration9> read_declaration;
  CHECK(device->GetVertexDeclaration(&read_declaration));
  expect(read_declaration.Get() == declaration.Get(), "GetVertexDeclaration is not the declaration that was set");
  // a declaration and an FVF are exclusive
  DWORD fvf = 1;
  CHECK(device->GetFVF(&fvf));
  expect(fvf == 0, "SetVertexDeclaration did not clear the FVF");
  CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
  CHECK(device->GetVertexDeclaration(&read_declaration));
  expect(read_declaration.Get() == nullptr, "SetFVF did not clear the declaration");
  CHECK(device->SetVertexDeclaration(declaration.Get()));

  // the triangles are wound counter-clockwise in clip space, which D3D9's default cull mode culls
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));

  // the frame's back buffer, taken once a draw has ended, as the triangle test takes its frames'
  ComPtr<IDirect3DSurface9> back_buffer;
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    if (!back_buffer)
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };

  // a triangle in clip space whose colour is the vertex colour, and the constants the shaders multiply by
  const Vertex triangle[3] = {{-0.5f, -0.5f, 0.0f, kCyan}, {0.5f, -0.5f, 0.0f, kCyan}, {0.0f, 0.5f, 0.0f, kCyan}};
  const float identity[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  // a frame: the shaders draw the triangle, whose pixels are read from the back buffer before it is presented (as the
  // triangle test reads its frames)
  auto draw_once = [&](IDirect3DVertexShader9 *vertex_shader, IDirect3DPixelShader9 *pixel_shader) -> HRESULT {
    CHECK(device->SetVertexShader(vertex_shader));
    CHECK(device->SetPixelShader(pixel_shader));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex));
    for (int again = 0; again < 3 && SUCCEEDED(drawn); again++)
      drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex));
    CHECK(device->EndScene());
    back_buffer.Reset();
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    return drawn;
  };

  // ---- the hand-written programs ----------------------------------------------------------------------------------
  ComPtr<IDirect3DVertexShader9> hand_vertex;
  ComPtr<IDirect3DPixelShader9> hand_pixel;
  CHECK(device->CreateVertexShader(kHandVertexShader, &hand_vertex));
  CHECK(device->CreatePixelShader(kHandPixelShader, &hand_pixel));
  CHECK(device->SetVertexShaderConstantF(0, identity, 1));
  CHECK(device->SetPixelShaderConstantF(1, identity, 1));
  HRESULT hr = draw_once(hand_vertex.Get(), hand_pixel.Get());
  expect(SUCCEEDED(hr), "a draw with hand-written shader model 2 programs was refused");
  printf("probe: 32,32=%06lx 2,2=%06lx\n", (unsigned long)pixel(32, 32), (unsigned long)pixel(2, 2));
  expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "the hand-written shaders did not draw their vertex colour");
  expect(pixel(2, 2) == kClearPixel, "the hand-written shaders drew outside their triangle");
  printf("shader: hand-written vs_2_0/ps_2_0 read %06lx at 32,32\n", (unsigned long)pixel(32, 32));

  // the constant tables reach the shaders: with c0 zero the position is zero (no triangle), and with c1 zero the colour
  // is black; both are set back to one after
  CHECK(device->SetPixelShaderConstantF(1, zero, 1));
  hr = draw_once(hand_vertex.Get(), hand_pixel.Get());
  expect(SUCCEEDED(hr), "the draw with a zero pixel constant was refused");
  expect(pixel(32, 32) == 0, "a zero c1 did not make the pixel black");
  CHECK(device->SetPixelShaderConstantF(1, identity, 1));

  // ---- Microsoft's compiler's programs ----------------------------------------------------------------------------
  ComPtr<ID3DBlob> vertex_code = compile(kVertexHlsl, "vs_2_0");
  ComPtr<ID3DBlob> pixel_code = compile(kPixelHlsl, "ps_2_0");
  expect(vertex_code != nullptr && pixel_code != nullptr, "Microsoft's compiler made no shader model 2 programs");
  if (vertex_code && pixel_code) {
    ComPtr<IDirect3DVertexShader9> vertex_shader;
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &vertex_shader));
    CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &pixel_shader));
    CHECK(device->SetVertexShaderConstantF(0, identity, 1));
    CHECK(device->SetPixelShaderConstantF(1, identity, 1));
    hr = draw_once(vertex_shader.Get(), pixel_shader.Get());
    expect(SUCCEEDED(hr), "a draw with Microsoft's vs_2_0/ps_2_0 was refused");
    expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "Microsoft's shaders did not draw their vertex colour");
    expect(pixel(2, 2) == kClearPixel, "Microsoft's shaders drew outside their triangle");
    printf("shader: Microsoft's vs_2_0/ps_2_0 read %06lx at 32,32\n", (unsigned long)pixel(32, 32));

    // the vertex constant table: with c0 zero the triangle collapses to a point (nothing is drawn)
    CHECK(device->SetVertexShaderConstantF(0, zero, 1));
    hr = draw_once(vertex_shader.Get(), pixel_shader.Get());
    expect(SUCCEEDED(hr), "the draw with a zero vertex constant was refused");
    expect(pixel(32, 32) == kClearPixel, "a zero c0 did not collapse the triangle");
    CHECK(device->SetVertexShaderConstantF(0, identity, 1));
  }

  // ---- what the translator refuses ---------------------------------------------------------------------------------
  ComPtr<ID3DBlob> cube_sampler_code = compile(kCubeSamplerHlsl, "ps_2_0");
  if (cube_sampler_code) {
    ComPtr<IDirect3DPixelShader9> cube_sampler_shader;
    CHECK(device->CreatePixelShader((const DWORD *)cube_sampler_code->GetBufferPointer(), &cube_sampler_shader));
    expect(draw_once(hand_vertex.Get(), cube_sampler_shader.Get()) == D3DERR_INVALIDCALL,
           "a pixel shader that samples a cube texture did not refuse the draw");
    printf("shader: a ps_2_0 that samples a cube texture refused the draw, as it should\n");
  }

  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(nullptr));
  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
