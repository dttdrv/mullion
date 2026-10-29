// contract: the pixels a program does not write.
// - texkill discards the whole pixel when any component of its register is negative, wherever the instruction sits in
//   the program (D3D9 kills the pixel, it does not order the writes around it).
// - D3DRS_ALPHATESTENABLE is D3D9's alpha test on the alpha a pixel shader writes: D3DRS_ALPHAFUNC compares it with
//   D3DRS_ALPHAREF and a pixel that fails is not written, and the three states are the draw's (they are the program's
//   only by being translated into it, so the same program is drawn with the test and without it).
// - a fixed-function draw under the alpha test is refused rather than drawn without it: the fixed-function pixel
//   shaders this build carries have no test of their own.
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texkill---ps
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drs-alphatestenable
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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

namespace {

const UINT kClient = 64;
const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 0);
const DWORD kClearPixel = kClear & 0x00FFFFFF;
// the colour the textured program draws (white) and the one the hand-written killing programs draw (the c0 the test
// defines), which the frame says apart from the clear
const DWORD kWhite = 0x00FFFFFF;
const DWORD kYellow = 0x00FFFF00;

struct Vertex {
  float x, y, z;
  float u, v;
};

// the hand-written vs_2_0 of the texture test: the position, and the texture coordinate in register the pixel shader
// reads
const DWORD kVertexShader[] = {
    0xFFFE0200, // vs_2_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x0200001F, 0x00000005, 0x900F0001, // dcl_texcoord0 v1
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x02000001, 0xE00F0000, 0x90E40001, // mov oT0, v1
    0x0000FFFF, // end
};

// dcl t0; def c0, 1, 1, 0, 1; mov oC0, c0; texkill t0 -- the shape Microsoft's compiler makes of `clip(uv.x); return
// float4(1, 1, 0, 1);` at ps_2_0 (the kill after the write, because D3D9's texkill is not ordered against the writes)
const DWORD kKillAfterWrite[] = {
    0xFFFF0200, // ps_2_0
    0x05000051, 0xA00F0000, 0x3F800000, 0x3F800000, 0x00000000, 0x3F800000, // def c0, 1, 1, 0, 1
    0x0200001F, 0x80000000, 0xB0030000, // dcl t0
    0x02000001, 0x800F0800, 0xA0E40000, // mov oC0, c0
    0x01000041, 0xB00F0000, // texkill t0
    0x0000FFFF, // end
};

// the same program with the kill first: D3D9 kills the pixel either way, so the frame is the same
const DWORD kKillBeforeWrite[] = {
    0xFFFF0200, // ps_2_0
    0x05000051, 0xA00F0000, 0x3F800000, 0x3F800000, 0x00000000, 0x3F800000, // def c0, 1, 1, 0, 1
    0x0200001F, 0x80000000, 0xB0030000, // dcl t0
    0x01000041, 0xB00F0000, // texkill t0
    0x02000001, 0x800F0800, 0xA0E40000, // mov oC0, c0
    0x0000FFFF, // end
};

// dcl t0; dcl_2d s0; texld r0, t0, s0; mov oC0, r0 -- the texel, whose alpha the test reads
const DWORD kSampleShader[] = {
    0xFFFF0200, // ps_2_0
    0x0200001F, 0x80000000, 0xB0030000, // dcl t0
    0x0200001F, 0x90000000, 0xA00F0800, // dcl_2d s0
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800, // texld r0, t0, s0
    0x02000001, 0x800F0800, 0x80E40000, // mov oC0, r0
    0x0000FFFF, // end
};

const char *kKillHlsl = "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
                        "  clip(uv.x);\n"
                        "  return float4(1, 1, 1, 1);\n"
                        "}\n";

ComPtr<ID3DBlob>
compile(const char *hlsl, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", profile, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

void
quad(Vertex out[4], float u0, float v0, float u1, float v1) {
  out[0] = {-1.0f, -1.0f, 0.0f, u0, v0};
  out[1] = {1.0f, -1.0f, 0.0f, u1, v0};
  out[2] = {-1.0f, 1.0f, 0.0f, u0, v1};
  out[3] = {1.0f, 1.0f, 0.0f, u1, v1};
}

} // namespace

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_discard"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_discard", "d3d9_discard", WS_OVERLAPPEDWINDOW, 0, 0,
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

  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  ComPtr<IDirect3DVertexShader9> vertex_shader;
  CHECK(device->CreateVertexShader(kVertexShader, &vertex_shader));
  CHECK(device->SetVertexShader(vertex_shader.Get()));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  ComPtr<IDirect3DSurface9> back_buffer;
  auto pixel = [&](UINT x, UINT y) -> DWORD {
    if (!back_buffer)
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const DWORD got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };
  auto draw_quad = [&](float u0, float v0, float u1, float v1) -> HRESULT {
    Vertex vertices[4];
    quad(vertices, u0, v0, u1, v1);
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    CHECK(device->EndScene());
    back_buffer.Reset();
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    return drawn;
  };

  // ---- texkill: a coordinate that is negative on the left half kills those pixels --------------------------------
  {
    ComPtr<IDirect3DPixelShader9> kill_after, kill_before;
    CHECK(device->CreatePixelShader(kKillAfterWrite, &kill_after));
    CHECK(device->CreatePixelShader(kKillBeforeWrite, &kill_before));
    for (int order = 0; order < 2; order++) {
      CHECK(device->SetPixelShader(order ? kill_before.Get() : kill_after.Get()));
      // the quad names u from -1 to 1, so the frame's left half is the negative one
      HRESULT hr = draw_quad(-1.0f, 0.0f, 1.0f, 1.0f);
      expect(SUCCEEDED(hr), "a draw with a texkill program was refused");
      printf("probe: texkill %d 8,30=%06lx 56,30=%06lx\n", order, (unsigned long)pixel(8, 30),
             (unsigned long)pixel(56, 30));
      expect(pixel(8, 30) == kClearPixel, "a pixel whose coordinate is negative was not killed");
      expect(pixel(56, 30) == kYellow, "a pixel whose coordinate is positive was killed");
      // the boundary is where u changes sign, at the middle of the frame
      expect(pixel(31, 30) == kClearPixel, "the negative side of the boundary was not killed");
      expect(pixel(32, 30) == kYellow, "the positive side of the boundary was killed");
    }
    // the same program Microsoft's compiler makes of clip() draws the same pixels
    ComPtr<ID3DBlob> compiled = compile(kKillHlsl, "ps_2_0");
    expect(compiled != nullptr, "Microsoft's compiler made no ps_2_0 program of clip()");
    if (compiled) {
      ComPtr<IDirect3DPixelShader9> clip_shader;
      CHECK(device->CreatePixelShader((const DWORD *)compiled->GetBufferPointer(), &clip_shader));
      CHECK(device->SetPixelShader(clip_shader.Get()));
      HRESULT hr = draw_quad(-1.0f, 0.0f, 1.0f, 1.0f);
      expect(SUCCEEDED(hr), "a draw with the compiled clip() program was refused");
      printf("probe: clip 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
      expect(pixel(8, 30) == kClearPixel, "the compiled clip() program did not kill the negative side");
      expect(pixel(56, 30) == kWhite, "the compiled clip() program killed the positive side");
    }
    // and a program that does not kill draws the whole quad, which says the kills above were the program's
    CHECK(device->SetPixelShader(kill_after.Get()));
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
    HRESULT hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw of the texkill program with non-negative coordinates was refused");
    expect(pixel(8, 30) == kYellow && pixel(56, 30) == kYellow,
           "the texkill program did not draw a quad whose coordinates are all positive");
  }

  // ---- D3DRS_ALPHATESTENABLE: the alpha the program writes, compared with D3DRS_ALPHAREF ---------------------------
  {
    // a texture whose left half is transparent white and right half opaque white: the frame's alpha comes from it
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    {
      D3DLOCKED_RECT locked{};
      CHECK(texture->LockRect(0, &locked, nullptr, 0));
      for (UINT y = 0; y < 4; y++) {
        DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
        row[0] = row[1] = D3DCOLOR_ARGB(0, 255, 255, 255);
        row[2] = row[3] = D3DCOLOR_ARGB(255, 255, 255, 255);
      }
      CHECK(texture->UnlockRect(0));
    }
    CHECK(device->SetTexture(0, texture.Get()));
    CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));

    ComPtr<IDirect3DPixelShader9> sample;
    CHECK(device->CreatePixelShader(kSampleShader, &sample));
    CHECK(device->SetPixelShader(sample.Get()));

    // without the test, both halves are drawn: the program is the same one every case below uses
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
    HRESULT hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw with the alpha test off was refused");
    printf("probe: no test 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
    expect(pixel(8, 30) == kWhite && pixel(56, 30) == kWhite,
           "a draw with the alpha test off did not draw the transparent pixels");

    // GREATER 128: the transparent half fails and is not written, the opaque half passes
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER));
    CHECK(device->SetRenderState(D3DRS_ALPHAREF, 128));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw under the alpha test was refused");
    printf("probe: greater 128 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
    expect(pixel(8, 30) == kClearPixel, "a pixel whose alpha failed the test was written");
    expect(pixel(56, 30) == kWhite, "a pixel whose alpha passed the test was not written");

    // LESS 128: the other way round, which says the comparison and not the draw order decided it
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESS));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw under LESS was refused");
    printf("probe: less 128 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
    expect(pixel(8, 30) == kWhite, "a pixel whose alpha is below the reference failed LESS");
    expect(pixel(56, 30) == kClearPixel, "a pixel whose alpha is above the reference passed LESS");

    // the reference is the draw's state: at 255 a 255 alpha no longer passes GREATER (the comparison is strict), and at
    // 0 a 0 alpha no longer passes it either
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER));
    CHECK(device->SetRenderState(D3DRS_ALPHAREF, 255));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw under GREATER 255 was refused");
    printf("probe: greater 255 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
    expect(pixel(8, 30) == kClearPixel && pixel(56, 30) == kClearPixel,
           "GREATER 255 passed an alpha of 255, which is not greater than 255");

    // ALWAYS with the test enabled draws everything, and NEVER draws nothing
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS));
    CHECK(device->SetRenderState(D3DRS_ALPHAREF, 200));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw under ALWAYS was refused");
    expect(pixel(8, 30) == kWhite && pixel(56, 30) == kWhite, "ALWAYS did not pass every pixel");
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_NEVER));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw under NEVER was refused");
    expect(pixel(8, 30) == kClearPixel && pixel(56, 30) == kClearPixel, "NEVER passed a pixel");

    // EQUAL at an alpha the texture holds: the opaque half passes, the transparent half does not
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_EQUAL));
    CHECK(device->SetRenderState(D3DRS_ALPHAREF, 255));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw under EQUAL was refused");
    printf("probe: equal 255 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
    expect(pixel(56, 30) == kWhite, "EQUAL 255 failed an alpha of 255");
    expect(pixel(8, 30) == kClearPixel, "EQUAL 255 passed an alpha of 0");
  }

  // ---- a fixed-function draw under the alpha test is refused, not drawn without it ----------------------------------
  {
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetVertexShader(nullptr));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER));
    struct FixedVertex {
      float x, y, z, rhw;
      D3DCOLOR colour;
      float u, v;
    };
    const FixedVertex vertices[4] = {
        {0.0f, 0.0f, 0.0f, 1.0f, D3DCOLOR_ARGB(0, 255, 255, 255), 0.0f, 0.0f},
        {(float)kClient, 0.0f, 0.0f, 1.0f, D3DCOLOR_ARGB(0, 255, 255, 255), 1.0f, 0.0f},
        {0.0f, (float)kClient, 0.0f, 1.0f, D3DCOLOR_ARGB(0, 255, 255, 255), 0.0f, 1.0f},
        {(float)kClient, (float)kClient, 0.0f, 1.0f, D3DCOLOR_ARGB(0, 255, 255, 255), 1.0f, 1.0f},
    };
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    const HRESULT refused = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(FixedVertex));
    CHECK(device->EndScene());
    back_buffer.Reset();
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    expect(refused == D3DERR_INVALIDCALL, "a fixed-function draw under the alpha test was drawn anyway");
    expect(pixel(32, 32) == kClearPixel, "a refused fixed-function draw wrote pixels");
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
  }

  // ---- and the state block carries the alpha test, as the pixel state it is ----------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL));
    CHECK(device->SetRenderState(D3DRS_ALPHAREF, 7));
    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_PIXELSTATE, &block));
    CHECK(device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_NEVER));
    CHECK(device->SetRenderState(D3DRS_ALPHAREF, 64));
    CHECK(block->Apply());
    DWORD enabled = 0, func = 0, reference = 0;
    CHECK(device->GetRenderState(D3DRS_ALPHATESTENABLE, &enabled));
    CHECK(device->GetRenderState(D3DRS_ALPHAFUNC, &func));
    CHECK(device->GetRenderState(D3DRS_ALPHAREF, &reference));
    expect(enabled == FALSE, "a state block did not restore D3DRS_ALPHATESTENABLE");
    expect(func == D3DCMP_LESSEQUAL, "a state block did not restore D3DRS_ALPHAFUNC");
    expect(reference == 7, "a state block did not restore D3DRS_ALPHAREF");
  }

  CHECK(device->SetTexture(0, nullptr));
  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
