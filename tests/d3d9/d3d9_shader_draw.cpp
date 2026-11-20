// contract: a game's own Direct3D 9 shaders draw, through the translator that turns their d3dbc programs into the DXBC
// this build's D3D11 device compiles, and the pixels are what the shaders say.
// - a vertex shader's inputs are the vertex declaration's elements (v0 the first, v1 the second), its oPos is the
//   position, oD0 the first colour; a pixel shader's v0 is that colour and oC0 the target. the declaration and the FVF
//   are exclusive: each clears the other.
// - the float and pixel constant tables reach the shader: a shader that multiplies by c0/c1 draws black when the table
//   is zero and the colour when it is one.
// - both a hand-written program (the tokens of this test, so the translator is proved without a compiler) and one
//   Microsoft's compiler makes of HLSL at test time (a real game's program) draw the same pixels.
// - a user clip plane under a vertex shader is in clip space (SetClipPlane): the plane x >= 0 clips the left half of
//   the triangle each program draws, and the whole triangle is drawn again once D3DRS_CLIPPLANEENABLE is zero.
// - a volume-sampling program is tests/d3d9/d3d9_volume.cpp; this file no longer refuses dcl_volume.
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
  expect(read_declaration.Get() != nullptr, "SetFVF did not create the declaration it implies");
  D3DVERTEXELEMENT9 implied[4]{};
  UINT implied_count = 0;
  expect(SUCCEEDED(read_declaration->GetDeclaration(implied, &implied_count)) && implied_count == 3,
         "SetFVF's implied declaration has the wrong element count");
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

  // ---- a game's vertex shader with the fixed-function pixel stage ------------------------------------------------
  // D3D9 draws a game's vertex shader with the fixed-function pixel stage when no pixel shader is bound (Wine visual.c
  // projected_textures_test's vs-only runs bind the test's vs_1_1 and leave the pixel stage to the texture stages). The
  // translated program is compiled against the fixed-function stage's COLOR0 input, and its own output signature has
  // to carry it.
  hr = draw_once(hand_vertex.Get(), nullptr);
  expect(SUCCEEDED(hr), "a game's vertex shader with no pixel shader was refused");
  expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "a game's vertex shader drew nothing with no pixel shader bound");


  // the constant tables reach the shaders: with c0 zero the position is zero (no triangle), and with c1 zero the colour
  // is black; both are set back to one after
  CHECK(device->SetPixelShaderConstantF(1, zero, 1));
  hr = draw_once(hand_vertex.Get(), hand_pixel.Get());
  expect(SUCCEEDED(hr), "the draw with a zero pixel constant was refused");
  expect(pixel(32, 32) == 0, "a zero c1 did not make the pixel black");
  CHECK(device->SetPixelShaderConstantF(1, identity, 1));

  // a user clip plane under a vertex shader is in clip space: x >= 0 keeps the triangle's right half. at row 40 the
  // triangle spans pixels 20..44, so 24 is clipped and 40 is not
  auto clip_check = [&](IDirect3DVertexShader9 *vertex_shader, IDirect3DPixelShader9 *pixel_shader,
                        const char *clipped, const char *kept) -> HRESULT {
    const float plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    CHECK(device->SetClipPlane(0, plane));
    CHECK(device->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0));
    HRESULT drawn = draw_once(vertex_shader, pixel_shader);
    CHECK(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
    expect(SUCCEEDED(drawn), "a shader draw with a clip plane was refused");
    expect(pixel(24, 40) == kClearPixel, clipped);
    expect(pixel(40, 40) == (kCyan & 0x00FFFFFF), kept);
    drawn = draw_once(vertex_shader, pixel_shader);
    expect(pixel(24, 40) == (kCyan & 0x00FFFFFF), "a disabled clip plane still clipped a shader draw");
    return drawn;
  };
  CHECK(clip_check(hand_vertex.Get(), hand_pixel.Get(), "the clip plane did not clip the hand-written shader's draw",
                   "the clip plane clipped the half of the hand-written shader's draw on its side"));

  // a pixel shader that reads a varying the vertex shader never writes still draws: D3D9 gives the uninitialized
  // varying its default (white for the diffuse, transparent black for the specular), and a partially written register
  // keeps that default in the components the program does not write (Wine visual.c test_uninitialized_varyings)
  {
    const DWORD vs_position_only[] = {
        0xFFFE0101,                         // vs_1_1
        0x0000001F, 0x80000000, 0x900F0000, // dcl_position v0
        0x00000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
        0x0000FFFF,
    };
    const DWORD ps_diffuse[] = {
        0xFFFF0101,                         // ps_1_1
        0x00000001, 0x800F0000, 0x90E40000, // mov r0, v0
        0x0000FFFF,
    };
    const DWORD ps_specular[] = {
        0xFFFF0101,                         // ps_1_1
        0x00000001, 0x800F0000, 0x90E40001, // mov r0, v1
        0x0000FFFF,
    };
    const DWORD vs_partial_diffuse[] = {
        0xFFFE0101,                                                             // vs_1_1
        0x0000001F, 0x80000000, 0x900F0000,                                     // dcl_position v0
        0x00000051, 0xA00F0000, 0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000, // def c0, 0.5, 0.5, 0.5, 0.5
        0x00000001, 0xC00F0000, 0x90E40000,                                     // mov oPos, v0
        0x00000001, 0xD0010000, 0xA0E40000,                                     // mov oD0.x, c0
        0x0000FFFF,
    };
    const DWORD vs2_partial_texcoord[] = {
        0xFFFE0200,                                                             // vs_2_0
        0x0200001F, 0x80000000, 0x900F0000,                                     // dcl_position v0
        0x05000051, 0xA00F0000, 0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000, // def c0, 0.5, 0.5, 0.5, 0.5
        0x02000001, 0xC00F0000, 0x90E40000,                                     // mov oPos, v0
        0x02000001, 0xE0010000, 0xA0E40000,                                     // mov oT0.x, c0
        0x0000FFFF,
    };
    const DWORD ps2_texcoord[] = {
        0xFFFF0200,                         // ps_2_0
        0x0200001F, 0x80000000, 0xB00F0000, // dcl t0
        0x02000001, 0x800F0800, 0xB0E40000, // mov oC0, t0
        0x0000FFFF,
    };
    const DWORD vs3_position_only[] = {
        0xFFFE0300,                         // vs_3_0
        0x0200001F, 0x80000000, 0x900F0000, // dcl_position v0
        0x0200001F, 0x80000000, 0xE00F0000, // dcl_position o0
        0x02000001, 0xE00F0000, 0x90E40000, // mov o0, v0
        0x0000FFFF,
    };
    const DWORD ps3_texcoord[] = {
        0xFFFF0300,                         // ps_3_0
        0x0200001F, 0x80000005, 0x900F0000, // dcl_texcoord0 v0
        0x02000001, 0x800F0800, 0x90E40000, // mov oC0, v0
        0x0000FFFF,
    };
    ComPtr<IDirect3DVertexShader9> position_only;
    ComPtr<IDirect3DVertexShader9> partial;
    ComPtr<IDirect3DVertexShader9> partial_texcoord;
    ComPtr<IDirect3DPixelShader9> diffuse;
    ComPtr<IDirect3DPixelShader9> specular;
    ComPtr<IDirect3DPixelShader9> texcoord;
    CHECK(device->CreateVertexShader(vs_position_only, &position_only));
    CHECK(device->CreateVertexShader(vs_partial_diffuse, &partial));
    CHECK(device->CreateVertexShader(vs2_partial_texcoord, &partial_texcoord));
    CHECK(device->CreatePixelShader(ps_diffuse, &diffuse));
    CHECK(device->CreatePixelShader(ps_specular, &specular));
    CHECK(device->CreatePixelShader(ps2_texcoord, &texcoord));
    ComPtr<IDirect3DVertexShader9> vs3;
    ComPtr<IDirect3DPixelShader9> ps3;
    CHECK(device->CreateVertexShader(vs3_position_only, &vs3));
    CHECK(device->CreatePixelShader(ps3_texcoord, &ps3));

    expect(SUCCEEDED(draw_once(position_only.Get(), diffuse.Get())),
           "the draw with an unwritten diffuse varying was refused");
    expect(pixel(32, 32) == 0x00FFFFFF, "an unwritten diffuse varying did not read white");
    expect(SUCCEEDED(draw_once(position_only.Get(), specular.Get())),
           "the draw with an unwritten specular varying was refused");
    expect(pixel(32, 32) == 0x00000000, "an unwritten specular varying did not read transparent black");
    expect(SUCCEEDED(draw_once(partial.Get(), diffuse.Get())),
           "the draw with a partially written diffuse varying was refused");
    expect(pixel(32, 32) == 0x007FFFFF, "a partially written diffuse varying lost its unwritten components");
    expect(SUCCEEDED(draw_once(partial_texcoord.Get(), texcoord.Get())),
           "the draw with a partially written texcoord varying was refused");
    expect(pixel(32, 32) == 0x007F0000, "a partially written texcoord varying did not read (0.5, 0, 0, 1)");
    expect(SUCCEEDED(draw_once(vs3.Get(), ps3.Get())), "the vs_3_0/ps_3_0 texcoord draw was refused");
    expect(pixel(32, 32) == 0x00000000, "a missing vs_3_0 texcoord varying did not read transparent black");
    // the same programs over the declaration SetFVF(D3DFVF_XYZ) implies, which is the format Wine's test draws
    {
      const float xyz[3][3] = {{-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}};
      CHECK(device->SetVertexDeclaration(nullptr));
      CHECK(device->SetFVF(D3DFVF_XYZ));
      CHECK(device->SetVertexShader(vs3.Get()));
      CHECK(device->SetPixelShader(ps3.Get()));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      CHECK(device->BeginScene());
      HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, xyz, sizeof(xyz[0]));
      CHECK(device->EndScene());
      back_buffer.Reset();
      CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
      expect(SUCCEEDED(drawn), "the vs_3_0 draw over an FVF declaration was refused");
      expect(pixel(32, 32) == 0x00000000, "the vs_3_0 missing texcoord over an FVF read white");
      printf("shader: vs_3_0 over FVF read %06lx at 32,32\n", (unsigned long)pixel(32, 32));
      CHECK(device->SetFVF(0));
      CHECK(device->SetVertexDeclaration(declaration.Get()));
    }
    // a game pixel shader with no vertex shader: the fixed-function VS writes TEXCOORD0, so a program that reads it
    // still draws (Wine visual.c:23011 cases 1 and 2)
    {
      const DWORD ps1_texcoord[] = {
          0xFFFF0101,                         // ps_1_1
          0x00000001, 0x800F0000, 0x90E40000, // mov r0, v0
          0x0000FFFF,
      };
      ComPtr<IDirect3DPixelShader9> texcoord1;
      CHECK(device->CreatePixelShader(ps1_texcoord, &texcoord1));
      const float xyz[3][3] = {{-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}};
      CHECK(device->SetVertexDeclaration(nullptr));
      CHECK(device->SetFVF(D3DFVF_XYZ));
      CHECK(device->SetVertexShader(nullptr));
      for (int which = 0; which < 2; which++) {
        CHECK(device->SetPixelShader(which ? texcoord1.Get() : texcoord.Get()));
        CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
        CHECK(device->BeginScene());
        HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, xyz, sizeof(xyz[0]));
        CHECK(device->EndScene());
        back_buffer.Reset();
        CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
        expect(SUCCEEDED(drawn), which ? "the ps_1_1 over the fixed-function VS was refused"
                                      : "the ps_2_0 over the fixed-function VS was refused");
        expect(pixel(32, 32) == 0x00000000, which ? "the ps_1_1 over the fixed-function VS did not read transparent black"
                                                  : "the ps_2_0 over the fixed-function VS did not read transparent black");
        printf("shader: mixed %s over the fixed-function VS read %06lx at 32,32\n", which ? "ps_1_1" : "ps_2_0",
               (unsigned long)pixel(32, 32));
      }
      CHECK(device->SetFVF(0));
      CHECK(device->SetVertexDeclaration(declaration.Get()));
      CHECK(device->SetVertexShader(nullptr));
      CHECK(device->SetPixelShader(nullptr));
    }
    printf("shader: uninitialized varyings read %06lx at 32,32\n", (unsigned long)pixel(32, 32));
  }

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
    CHECK(clip_check(vertex_shader.Get(), pixel_shader.Get(), "the clip plane did not clip Microsoft's shader's draw",
                     "the clip plane clipped the half of Microsoft's shader's draw on its side"));
  }

  // a vertex shader's texture fetch: tex2Dlod is texldl, which a Direct3D 11 vertex shader samples at an explicit lod
  // (Microsoft Learn, texldl - vs). the pixel shader only writes the colour the vertex shader sampled
  {
    const char *vertex_hlsl = "sampler2D tex : register(s0);\n"
                              "float4 uv : register(c0);\n"
                              "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                              "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                              "  VSOut o;\n"
                              "  o.position = position;\n"
                              "  o.colour = tex2Dlod(tex, uv);\n"
                              "  return o;\n"
                              "}\n";
    const char *pixel_hlsl = "float4 main(float4 colour : COLOR0) : COLOR { return colour; }\n";
    ComPtr<ID3DBlob> vertex_code = compile(vertex_hlsl, "vs_3_0");
    ComPtr<ID3DBlob> pixel_code = compile(pixel_hlsl, "ps_3_0");
    expect(vertex_code != nullptr && pixel_code != nullptr, "Microsoft's compiler made no vertex texture fetch");
    if (vertex_code && pixel_code) {
      ComPtr<IDirect3DVertexShader9> vertex_shader;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      ComPtr<IDirect3DTexture9> red;
      CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &vertex_shader));
      CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &pixel_shader));
      CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &red, nullptr));
      D3DLOCKED_RECT texels{};
      CHECK(red->LockRect(0, &texels, nullptr, 0));
      *static_cast<DWORD *>(texels.pBits) = 0xFFFF0000;
      CHECK(red->UnlockRect(0));
      // a vertex shader's sampler is D3DVERTEXTEXTURESAMPLER0 (257), not the pixel stage 0
      CHECK(device->SetTexture(D3DVERTEXTEXTURESAMPLER0, red.Get()));
      CHECK(device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
      CHECK(device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
      const float uv[4] = {0.5f, 0.5f, 0.0f, 0.0f};
      CHECK(device->SetVertexShaderConstantF(0, uv, 1));
      const HRESULT fetched = draw_once(vertex_shader.Get(), pixel_shader.Get());
      expect(SUCCEEDED(fetched), "a vertex shader texture fetch was refused");
      expect(pixel(32, 32) == 0x00FF0000, "a vertex shader tex2Dlod did not draw the texel");
      printf("shader: vertex texture fetch read %06lx at 32,32\n", (unsigned long)pixel(32, 32));
      CHECK(device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr));
    }
  }

  // each public vertex sampler 257–260 feeds its own vs_3_0 register, and the vertex and pixel sampler namespaces
  // sample at once: 257 red and pixel stage 0 green make yellow. a default (white) texture or the other namespace's
  // texture would make one of the channels miss
  {
    const D3DCOLOR colours[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00};
    const float uv[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    for (UINT at = 0; at < 4; at++) {
      char vertex_hlsl[512], what[96];
      snprintf(vertex_hlsl, sizeof(vertex_hlsl),
               "sampler2D tex : register(s%u);\n"
               "float4 uv : register(c0);\n"
               "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
               "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
               "  VSOut o;\n"
               "  o.position = position;\n"
               "  o.colour = tex2Dlod(tex, uv);\n"
               "  return o;\n"
               "}\n",
               at);
      const char *pixel_hlsl = "float4 main(float4 colour : COLOR0) : COLOR { return colour; }\n";
      ComPtr<ID3DBlob> vertex_code = compile(vertex_hlsl, "vs_3_0");
      ComPtr<ID3DBlob> pixel_code = compile(pixel_hlsl, "ps_3_0");
      snprintf(what, sizeof(what), "Microsoft's compiler made no vertex sampler %u shader", at);
      expect(vertex_code != nullptr && pixel_code != nullptr, what);
      if (!vertex_code || !pixel_code)
        continue;
      ComPtr<IDirect3DVertexShader9> vertex_shader;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      ComPtr<IDirect3DTexture9> texture;
      CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &vertex_shader));
      CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &pixel_shader));
      CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
      D3DLOCKED_RECT texels{};
      CHECK(texture->LockRect(0, &texels, nullptr, 0));
      *static_cast<DWORD *>(texels.pBits) = colours[at];
      CHECK(texture->UnlockRect(0));
      const DWORD stage = D3DVERTEXTEXTURESAMPLER0 + at;
      CHECK(device->SetTexture(stage, texture.Get()));
      CHECK(device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT));
      CHECK(device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
      CHECK(device->SetVertexShaderConstantF(0, uv, 1));
      CHECK(draw_once(vertex_shader.Get(), pixel_shader.Get()));
      snprintf(what, sizeof(what), "D3DVERTEXTEXTURESAMPLER%u did not draw its own texel", at);
      expect(pixel(32, 32) == (colours[at] & 0x00FFFFFF), what);
      CHECK(device->SetTexture(stage, nullptr));
    }

    const char *vertex_hlsl = "sampler2D tex : register(s0);\n"
                              "float4 uv : register(c0);\n"
                              "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                              "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                              "  VSOut o;\n"
                              "  o.position = position;\n"
                              "  o.colour = tex2Dlod(tex, uv);\n"
                              "  return o;\n"
                              "}\n";
    const char *pixel_hlsl = "sampler2D tex : register(s0);\n"
                             "float4 main(float4 colour : COLOR0) : COLOR {\n"
                             "  return float4(colour.r, tex2Dlod(tex, float4(0.5, 0.5, 0, 0)).g, 0, 1);\n"
                             "}\n";
    ComPtr<ID3DBlob> vertex_code = compile(vertex_hlsl, "vs_3_0");
    ComPtr<ID3DBlob> pixel_code = compile(pixel_hlsl, "ps_3_0");
    expect(vertex_code != nullptr && pixel_code != nullptr, "Microsoft's compiler made no both-namespace shader");
    if (vertex_code && pixel_code) {
      ComPtr<IDirect3DVertexShader9> vertex_shader;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      ComPtr<IDirect3DTexture9> vertex_texture, pixel_texture;
      CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &vertex_shader));
      CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &pixel_shader));
      CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vertex_texture, nullptr));
      CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pixel_texture, nullptr));
      D3DLOCKED_RECT texels{};
      CHECK(vertex_texture->LockRect(0, &texels, nullptr, 0));
      *static_cast<DWORD *>(texels.pBits) = 0xFFFF0000;
      CHECK(vertex_texture->UnlockRect(0));
      CHECK(pixel_texture->LockRect(0, &texels, nullptr, 0));
      *static_cast<DWORD *>(texels.pBits) = 0xFF00FF00;
      CHECK(pixel_texture->UnlockRect(0));
      CHECK(device->SetTexture(D3DVERTEXTEXTURESAMPLER0, vertex_texture.Get()));
      CHECK(device->SetTexture(0, pixel_texture.Get()));
      CHECK(device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
      CHECK(device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
      CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
      CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
      CHECK(device->SetVertexShaderConstantF(0, uv, 1));
      CHECK(draw_once(vertex_shader.Get(), pixel_shader.Get()));
      expect(pixel(32, 32) == 0x00FFFF00,
             "a pixel sampler and a vertex sampler did not sample their own textures at the same time");
      printf("shader: simultaneous 257 and pixel s0 read %06lx at 32,32\n", (unsigned long)pixel(32, 32));
      CHECK(device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr));
      CHECK(device->SetTexture(0, nullptr));
    }
  }

  // pixel sampler 15 is the last of the ps_3_0 sampler registers, and it samples its own stage rather than wrapping
  {
    const char *vertex_hlsl = "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                              "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                              "  VSOut o; o.position = position; o.colour = colour; return o;\n"
                              "}\n";
    const char *pixel_hlsl = "sampler2D tex : register(s15);\n"
                             "float4 main(float4 colour : COLOR0) : COLOR {\n"
                             "  return tex2Dlod(tex, float4(0.5, 0.5, 0, 0));\n"
                             "}\n";
    ComPtr<ID3DBlob> vertex_code = compile(vertex_hlsl, "vs_3_0");
    ComPtr<ID3DBlob> pixel_code = compile(pixel_hlsl, "ps_3_0");
    expect(vertex_code != nullptr && pixel_code != nullptr, "Microsoft's compiler made no pixel s15 shader");
    if (vertex_code && pixel_code) {
      ComPtr<IDirect3DVertexShader9> vertex_shader;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      ComPtr<IDirect3DTexture9> texture;
      CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &vertex_shader));
      CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &pixel_shader));
      CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
      D3DLOCKED_RECT texels{};
      CHECK(texture->LockRect(0, &texels, nullptr, 0));
      *static_cast<DWORD *>(texels.pBits) = 0xFFFF00FF;
      CHECK(texture->UnlockRect(0));
      CHECK(device->SetTexture(15, texture.Get()));
      CHECK(device->SetSamplerState(15, D3DSAMP_MINFILTER, D3DTEXF_POINT));
      CHECK(device->SetSamplerState(15, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
      CHECK(draw_once(vertex_shader.Get(), pixel_shader.Get()));
      expect(pixel(32, 32) == 0x00FF00FF, "a ps_3_0 program did not sample pixel sampler s15");
      printf("shader: pixel sampler s15 read %06lx at 32,32\n", (unsigned long)pixel(32, 32));
      CHECK(device->SetTexture(15, nullptr));
    }
  }

  // volume sampling is tests/d3d9/d3d9_volume.cpp; this translator now emits tex3D rather than refusing dcl_volume

  // ---- Wine visual.c test_uninitialized_varyings, on the device and in the order that test draws it -----------------
  {
    HWND second_window = CreateWindowA("d3d9_shader_draw", "second", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                       nullptr, window_class.hInstance, nullptr);
    D3DPRESENT_PARAMETERS second_present{};
    second_present.BackBufferWidth = 640;
    second_present.BackBufferHeight = 480;
    second_present.BackBufferFormat = D3DFMT_X8R8G8B8;
    second_present.BackBufferCount = 1;
    second_present.MultiSampleType = D3DMULTISAMPLE_NONE;
    second_present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    second_present.hDeviceWindow = second_window;
    second_present.Windowed = TRUE;
    second_present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    ComPtr<IDirect3DDevice9> other;
    expect(SUCCEEDED(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, second_window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                        &second_present, &other)),
           "the second device was refused");
    if (other) {
      D3DMATRIX unit = {};
      unit._11 = unit._22 = unit._33 = unit._44 = 1.0f;
      CHECK(other->SetTransform(D3DTS_WORLD, &unit));
      CHECK(other->SetTransform(D3DTS_VIEW, &unit));
      CHECK(other->SetTransform(D3DTS_PROJECTION, &unit));
      CHECK(other->SetRenderState(D3DRS_CLIPPING, FALSE));
      CHECK(other->SetRenderState(D3DRS_ZENABLE, FALSE));
      CHECK(other->SetRenderState(D3DRS_FOGENABLE, FALSE));
      CHECK(other->SetRenderState(D3DRS_STENCILENABLE, FALSE));
      CHECK(other->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
      CHECK(other->SetRenderState(D3DRS_LIGHTING, TRUE));
      CHECK(other->SetFVF(D3DFVF_XYZ));
      const DWORD vs1_code[] = {
          0xFFFE0101,                         // vs_1_1
          0x0000001F, 0x80000000, 0x900F0000, // dcl_position v0
          0x00000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
          0x0000FFFF,
      };
      const DWORD vs3_code[] = {
          0xFFFE0300,                         // vs_3_0
          0x0200001F, 0x80000000, 0x900F0000, // dcl_position v0
          0x0200001F, 0x80000000, 0xE00F0000, // dcl_position o0
          0x02000001, 0xE00F0000, 0x90E40000, // mov o0, v0
          0x0000FFFF,
      };
      const DWORD ps1_texcoord_code[] = {
          0xFFFF0101,                         // ps_1_1
          0x00000040, 0xB00F0000,             // texcoord t0
          0x00000001, 0x800F0000, 0xB0E40000, // mov r0, t0
          0x0000FFFF,
      };
      const DWORD ps2_texcoord_code[] = {
          0xFFFF0200,                         // ps_2_0
          0x0200001F, 0x80000000, 0xB00F0000, // dcl t0
          0x02000001, 0x800F0800, 0xB0E40000, // mov oC0, t0
          0x0000FFFF,
      };
      const DWORD ps3_specular_code[] = {
          0xFFFF0300,                         // ps_3_0
          0x0200001F, 0x8001000A, 0x900F0001, // dcl_color1 v1
          0x02000001, 0x800F0800, 0x90E40001, // mov oC0, v1
          0x0000FFFF,
      };
      const DWORD ps3_texcoord_code[] = {
          0xFFFF0300,                         // ps_3_0
          0x0200001F, 0x80000005, 0x900F0000, // dcl_texcoord0 v0
          0x02000001, 0x800F0800, 0x90E40000, // mov oC0, v0
          0x0000FFFF,
      };
      const struct {
        const DWORD *vs;
        const DWORD *ps;
        const char *name;
      } cases[] = {{vs1_code, nullptr, "0 vs1"},
                   {nullptr, ps1_texcoord_code, "1 ps1"},
                   {nullptr, ps2_texcoord_code, "2 ps2"},
                   {vs3_code, ps3_specular_code, "9 vs3/ps3 spec"},
                   {vs3_code, ps3_texcoord_code, "10 vs3/ps3 tex"}};
      const float quad[4][3] = {{-1.0f, -1.0f, 0.1f}, {-1.0f, 1.0f, 0.1f}, {1.0f, -1.0f, 0.1f}, {1.0f, 1.0f, 0.1f}};
      ComPtr<IDirect3DSurface9> other_back;
      ComPtr<IDirect3DSurface9> other_readback;
      CHECK(other->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &other_back));
      CHECK(other->CreateOffscreenPlainSurface(640, 480, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &other_readback,
                                               nullptr));
      for (const auto &one : cases) {
        ComPtr<IDirect3DVertexShader9> vs;
        ComPtr<IDirect3DPixelShader9> ps;
        if (one.vs)
          CHECK(other->CreateVertexShader(one.vs, &vs));
        if (one.ps)
          CHECK(other->CreatePixelShader(one.ps, &ps));
        CHECK(other->SetVertexShader(vs.Get()));
        CHECK(other->SetPixelShader(ps.Get()));
        CHECK(other->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 0.0f, 0));
        CHECK(other->BeginScene());
        HRESULT drawn = other->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        CHECK(other->EndScene());
        expect(SUCCEEDED(drawn), one.name);
        other_back.Reset();
        CHECK(other->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &other_back));
        D3DLOCKED_RECT locked{};
        D3DCOLOR at = 0;
        if (SUCCEEDED(other->GetRenderTargetData(other_back.Get(), other_readback.Get())) &&
            SUCCEEDED(other_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
          at = ((const DWORD *)((const char *)locked.pBits + 240 * locked.Pitch))[320];
          other_readback->UnlockRect();
        }
        printf("shader: wine case %s read %08lx\n", one.name, (unsigned long)at);
      }
      other.Reset();
    }
    if (second_window)
      DestroyWindow(second_window);
  }

  // ---- Wine visual.c projected_textures_test: the vs-only run's declaration and program ---------------------------
  // A four-component texture coordinate with the test's vs_1_1 (mov oPos, v0; mov oT0, v1) and no pixel shader: the
  // projected coordinate's q reaches the fixed-function stage, which is the run that draws the test's bottom-left quad.
  {
    static const DWORD kWineVertexShader[] = {
        0xfffe0101u, 0x0000001fu, 0x80000000u, 0x900f0000u, 0x0000001fu, 0x80000005u, 0x900f0001u,
        0x00000001u, 0xc00f0000u, 0x90e40000u, 0x00000001u, 0xe00f0000u, 0x90e40001u, 0x0000ffffu,
    };
    static const D3DVERTEXELEMENT9 kWideDeclaration[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const float kWideQuad[4][7] = {
        {-1.0f, -1.0f, 0.1f, 0.0f, 0.0f, 4.0f, 6.0f},
        {-1.0f, 0.0f, 0.1f, 0.0f, 4.0f, 4.0f, 6.0f},
        {0.0f, -1.0f, 0.1f, 4.0f, 0.0f, 4.0f, 6.0f},
        {0.0f, 0.0f, 0.1f, 4.0f, 4.0f, 4.0f, 6.0f},
    };
    ComPtr<IDirect3DVertexShader9> wine_vertex;
    ComPtr<IDirect3DVertexDeclaration9> wide_declaration;
    CHECK(device->CreateVertexShader(kWineVertexShader, &wine_vertex));
    CHECK(device->CreateVertexDeclaration(kWideDeclaration, &wide_declaration));
    CHECK(device->SetVertexDeclaration(wide_declaration.Get()));
    CHECK(device->SetVertexShader(wine_vertex.Get()));
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    const HRESULT wide_drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, kWideQuad, sizeof(kWideQuad[0]));
    CHECK(device->EndScene());
    expect(SUCCEEDED(wide_drawn), "a projected draw through a game's vs_1_1 and no pixel shader was refused");
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetVertexDeclaration(nullptr);
    device->SetVertexShader(nullptr);
  }

  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(nullptr));
  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
