// contract: the caps say what this device can be asked for, and a game that reads them gets it.
// - VertexShaderVersion and PixelShaderVersion name the shader models the translator certifies (vs_2_0 and ps_2_0), and
//   MaxVertexShaderConst the 256 float registers a stage has; the flow-control caps are 0 because the translator reads no
//   flow control.
// - a program of the version the caps name is created and drawn, and a program of a version above them is refused by
//   CreateVertexShader/CreatePixelShader: a version reported but refused here would be worse than a lower cap, because a
//   game plans its shaders from these numbers.
// - MaxSimultaneousTextures is the sixteen stages a ps_3_0 program samples (s0 through s15) and MaxTextureBlendStages
//   the two stages of the fixed-function blender: the caps are the counts the tests certify, and the two are separate
//   counts.
// - NumSimultaneousRTs is the four render targets a ps_3_0 program writes oC0 through oC3 to, and MaxStreams the
//   sixteen vertex streams vs_3_0 reads (Microsoft Learn, D3DCAPS9).
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcaps9
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace {

const UINT kClient = 32;

struct Vertex {
  float x, y, z;
};

// the vs_2_0 the caps certify: the position straight through
const DWORD kVertex20[] = {
    0xFFFE0200, // vs_2_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x0000FFFF, // end
};

// the ps_2_0 the caps certify: green, from a constant the program defines
const DWORD kPixel20[] = {
    0xFFFF0200, // ps_2_0
    0x05000051, 0xA00F0000, 0x00000000, 0x3F800000, 0x00000000, 0x3F800000, // def c0, 0, 1, 0, 1
    0x02000001, 0x800F0800, 0xA0E40000, // mov oC0, c0
    0x0000FFFF, // end
};

// the same two programs one version up (a 3.0 version token and a 3.0 instruction shape)
const DWORD kVertex30[] = {
    0xFFFE0300, // vs_3_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x0000FFFF, // end
};
const DWORD kPixel30[] = {
    0xFFFF0300, // ps_3_0
    0x05000051, 0xA00F0000, 0x00000000, 0x3F800000, 0x00000000, 0x3F800000, // def c0, 0, 1, 0, 1
    0x02000001, 0x800F0800, 0xA0E40000, // mov oC0, c0
    0x0000FFFF, // end
};

// ps_1_1, whose colour is r0. the length field is shader model 2's, and a real ps_1_1 token leaves it zero; both are read
const DWORD kPixel11[] = {
    0xFFFF0101, // ps_1_1
    0x02000001, 0x800F0000, 0xA0E40000, // mov r0, c0
    0x0000FFFF, // end
};
// the length-zero encoding fxc emits, and `tex t0` which samples stage 0 at interpolator 0 (Microsoft Learn, tex - ps)
const DWORD kPixel11Tex[] = {
    0xFFFF0101, // ps_1_1
    0x00000042, 0xB00F0000, // tex t0
    0x00000001, 0x800F0000, 0xB0E40000, // mov r0, t0
    0x0000FFFF, // end
};
const DWORD kTexVertex[] = {
    0xFFFE0200, // vs_2_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x0200001F, 0x00000005, 0x900F0001, // dcl_texcoord0 v1
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x02000001, 0xE00F0000, 0x90E40001, // mov oT0, v1
    0x0000FFFF, // end
};
// ps_1_1 bump and matrix texture ops: with a zero bump matrix (the D3DTSS_BUMPENVMAT* defaults) and no preceding
// texm3x*pad, each samples stage 0 at interpolator 0, then mov r0, t0 writes the colour (Microsoft Learn, texbem,
// texm3x2tex, texm3x3tex, texdp3tex).
const DWORD kPixel11Texbem[] = {
    0xFFFF0101, // ps_1_1
    0x00000043, 0xB00F0000, // texbem t0
    0x00000001, 0x800F0000, 0xB0E40000, // mov r0, t0
    0x0000FFFF,
};
const DWORD kPixel11Texm3x2[] = {
    0xFFFF0101, // ps_1_1
    0x00000048, 0xB00F0000, // texm3x2tex t0
    0x00000001, 0x800F0000, 0xB0E40000, // mov r0, t0
    0x0000FFFF,
};
const DWORD kPixel11Texm3x3[] = {
    0xFFFF0101, // ps_1_1
    0x0000004A, 0xB00F0000, // texm3x3tex t0
    0x00000001, 0x800F0000, 0xB0E40000, // mov r0, t0
    0x0000FFFF,
};
const DWORD kPixel11Texdp3[] = {
    0xFFFF0101, // ps_1_1
    0x00000053, 0xB00F0000, // texdp3tex t0
    0x00000001, 0x800F0000, 0xB0E40000, // mov r0, t0
    0x0000FFFF,
};

struct TexVertex {
  float x, y, z;
  float u, v;
};

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
                         "d3d9_caps"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_caps", "d3d9_caps", WS_OVERLAPPEDWINDOW, 0, 0, window_rect.right - window_rect.left,
                              window_rect.bottom - window_rect.top, nullptr, nullptr, window_class.hInstance, nullptr);
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

  // the adapter's caps and the device's are the same numbers: a game may read either before it makes its device
  D3DCAPS9 adapter_caps{};
  CHECK(d3d9->GetDeviceCaps(0, D3DDEVTYPE_HAL, &adapter_caps));
  auto check_caps = [&](const D3DCAPS9 &caps, const char *which) {
    printf("probe: %s vs=%04lx ps=%04lx consts=%lu stages=%lu blend=%lu temps=%lu\n", which,
           (unsigned long)caps.VertexShaderVersion, (unsigned long)caps.PixelShaderVersion,
           (unsigned long)caps.MaxVertexShaderConst, (unsigned long)caps.MaxSimultaneousTextures,
           (unsigned long)caps.MaxTextureBlendStages, (unsigned long)caps.PS20Caps.NumTemps);
    expect(caps.VertexShaderVersion == D3DVS_VERSION(3, 0), "VertexShaderVersion is not the vs_3_0 the translator has");
    expect(caps.PixelShaderVersion == D3DPS_VERSION(3, 0), "PixelShaderVersion is not the ps_3_0 the translator has");
    expect(caps.MaxVertexShaderConst == 256, "MaxVertexShaderConst is not the 256 float registers a stage has");
    expect(caps.VS20Caps.NumTemps == 12 && caps.PS20Caps.NumTemps == 12,
           "the shader model 2 temp count is not the twelve both models have");
    expect(caps.VS20Caps.DynamicFlowControlDepth == 24 && caps.PS20Caps.DynamicFlowControlDepth == 24 &&
               caps.VS20Caps.StaticFlowControlDepth == 24 && caps.PS20Caps.StaticFlowControlDepth == 24,
           "the flow control depth is not the if/ifc the translator reads");
    expect(caps.MaxSimultaneousTextures == 16, "MaxSimultaneousTextures is not the sixteen samplers ps_3_0 reads");
    expect(caps.MaxTextureBlendStages == 8, "MaxTextureBlendStages is not the eight fixed-function stages");
    expect(caps.NumSimultaneousRTs == 4, "NumSimultaneousRTs is not the four targets ps_3_0 writes");
    expect((caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) != 0,
           "independent per-target write masks are implemented but not advertised");
    expect(caps.MaxStreams == 16, "MaxStreams is not the sixteen vertex streams vs_3_0 reads");
    expect(caps.MaxVertexShader30InstructionSlots >= 512 && caps.MaxPixelShader30InstructionSlots >= 512,
           "the shader model 3 instruction slots are below the model's minimum");
    expect(caps.MaxVShaderInstructionsExecuted >= 65535 && caps.MaxPShaderInstructionsExecuted >= 65535,
           "the executed-instruction counts are below the model's minimum");
    expect(caps.MaxStreamStride >= 255, "MaxStreamStride is below what a stream may be set with");
    expect(caps.PixelShader1xMaxValue >= 8.0f, "PixelShader1xMaxValue is below ps_1_x's range");
  };
  check_caps(adapter_caps, "adapter");

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
  D3DCAPS9 caps{};
  CHECK(device->GetDeviceCaps(&caps));
  check_caps(caps, "device");

  // a program of the version the caps name is made and drawn: green across the frame
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  ComPtr<IDirect3DVertexShader9> vertex_shader;
  ComPtr<IDirect3DPixelShader9> pixel_shader;
  CHECK(device->CreateVertexShader(kVertex20, &vertex_shader));
  CHECK(device->CreatePixelShader(kPixel20, &pixel_shader));
  CHECK(device->SetVertexShader(vertex_shader.Get()));
  CHECK(device->SetPixelShader(pixel_shader.Get()));
  const Vertex vertices[4] = {{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
  CHECK(device->BeginScene());
  const HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
  CHECK(device->EndScene());
  expect(SUCCEEDED(drawn), "a draw of the shader model the caps name was refused");

  ComPtr<IDirect3DSurface9> readback, back_buffer;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  D3DLOCKED_RECT locked{};
  CHECK(device->GetRenderTargetData(back_buffer.Get(), readback.Get()));
  CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
  const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)(kClient / 2) * locked.Pitch);
  const DWORD middle = row[kClient / 2] & 0x00FFFFFF;
  readback->UnlockRect();
  printf("probe: vs_2_0/ps_2_0 %06lx\n", (unsigned long)middle);
  expect(middle == 0x0000FF00, "the vs_2_0/ps_2_0 pair the caps name did not draw its colour");

  ComPtr<IDirect3DVertexShader9> vertex30;
  ComPtr<IDirect3DPixelShader9> pixel30;
  expect(SUCCEEDED(device->CreateVertexShader(kVertex30, &vertex30)),
         "a vs_3_0 program the caps name was refused");
  expect(SUCCEEDED(device->CreatePixelShader(kPixel30, &pixel30)),
         "a ps_3_0 program the caps name was refused");

  // ps_1_1: r0 is the colour, and c0 is the constant the draw sets. the pixel is that constant
  ComPtr<IDirect3DPixelShader9> pixel11;
  expect(SUCCEEDED(device->CreatePixelShader(kPixel11, &pixel11)), "a ps_1_1 program was refused");
  if (pixel11) {
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    CHECK(device->SetPixelShader(pixel11.Get()));
    CHECK(device->SetPixelShaderConstantF(0, green, 1));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    CHECK(device->BeginScene());
    const HRESULT ps11 = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    CHECK(device->EndScene());
    expect(SUCCEEDED(ps11), "a ps_1_1 draw was refused");
    CHECK(device->GetRenderTargetData(back_buffer.Get(), readback.Get()));
    CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    const DWORD *ps11_row = (const DWORD *)((const char *)locked.pBits + (size_t)(kClient / 2) * locked.Pitch);
    const DWORD ps11_middle = ps11_row[kClient / 2] & 0x00FFFFFF;
    readback->UnlockRect();
    printf("probe: ps_1_1 %06lx\n", (unsigned long)ps11_middle);
    expect(ps11_middle == 0x0000FF00, "ps_1_1 mov r0, c0 did not draw c0");
  }

  // and the length-zero `tex t0; mov r0, t0` a compiler emits: the texel is the pixel
  ComPtr<IDirect3DPixelShader9> pixel11_tex;
  ComPtr<IDirect3DVertexShader9> tex_vertex;
  expect(SUCCEEDED(device->CreatePixelShader(kPixel11Tex, &pixel11_tex)), "a ps_1_1 tex program was refused");
  expect(SUCCEEDED(device->CreateVertexShader(kTexVertex, &tex_vertex)), "the vs_2_0 a ps_1_1 tex draws with was refused");
  if (pixel11_tex && tex_vertex) {
    ComPtr<IDirect3DTexture9> red;
    CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &red, nullptr));
    D3DLOCKED_RECT texels{};
    CHECK(red->LockRect(0, &texels, nullptr, 0));
    *static_cast<DWORD *>(texels.pBits) = 0xFFFF0000;
    CHECK(red->UnlockRect(0));
    const D3DVERTEXELEMENT9 tex_elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    ComPtr<IDirect3DVertexDeclaration9> tex_declaration;
    CHECK(device->CreateVertexDeclaration(tex_elements, &tex_declaration));
    CHECK(device->SetVertexDeclaration(tex_declaration.Get()));
    CHECK(device->SetVertexShader(tex_vertex.Get()));
    CHECK(device->SetPixelShader(pixel11_tex.Get()));
    CHECK(device->SetTexture(0, red.Get()));
    CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    const TexVertex quad[4] = {{-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}, {-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}};
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    CHECK(device->BeginScene());
    const HRESULT tex_drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexVertex));
    CHECK(device->EndScene());
    expect(SUCCEEDED(tex_drawn), "a ps_1_1 tex draw was refused");
    CHECK(device->GetRenderTargetData(back_buffer.Get(), readback.Get()));
    CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    const DWORD *tex_row = (const DWORD *)((const char *)locked.pBits + (size_t)(kClient / 2) * locked.Pitch);
    const DWORD tex_middle = tex_row[kClient / 2] & 0x00FFFFFF;
    readback->UnlockRect();
    printf("probe: ps_1_1 tex %06lx\n", (unsigned long)tex_middle);
    expect(tex_middle == 0x00FF0000, "ps_1_1 tex t0 did not draw the texture");
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->SetVertexDeclaration(declaration.Get()));
    CHECK(device->SetVertexShader(vertex_shader.Get()));
  }

  // texbem / texm3x2tex / texm3x3tex / texdp3tex with a zero bump matrix and no pads sample like tex and colour the
  // frame from the bound texture
  if (tex_vertex) {
    ComPtr<IDirect3DTexture9> red_bump;
    CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &red_bump, nullptr));
    D3DLOCKED_RECT bump_texels{};
    CHECK(red_bump->LockRect(0, &bump_texels, nullptr, 0));
    *static_cast<DWORD *>(bump_texels.pBits) = 0xFFFF0000;
    CHECK(red_bump->UnlockRect(0));
    const D3DVERTEXELEMENT9 bump_elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    ComPtr<IDirect3DVertexDeclaration9> bump_declaration;
    CHECK(device->CreateVertexDeclaration(bump_elements, &bump_declaration));
    CHECK(device->SetVertexDeclaration(bump_declaration.Get()));
    CHECK(device->SetVertexShader(tex_vertex.Get()));
    CHECK(device->SetTexture(0, red_bump.Get()));
    CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    const TexVertex bump_quad[4] = {{-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}, {-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}};
    auto draw_bump = [&](const DWORD *code, const char *name) -> int {
      ComPtr<IDirect3DPixelShader9> bump;
      expect(SUCCEEDED(device->CreatePixelShader(code, &bump)), name);
      if (!bump)
        return 0;
      CHECK(device->SetPixelShader(bump.Get()));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
      CHECK(device->BeginScene());
      const HRESULT drew = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bump_quad, sizeof(TexVertex));
      CHECK(device->EndScene());
      expect(SUCCEEDED(drew), name);
      CHECK(device->GetRenderTargetData(back_buffer.Get(), readback.Get()));
      CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
      const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)(kClient / 2) * locked.Pitch);
      const DWORD middle = row[kClient / 2] & 0x00FFFFFF;
      readback->UnlockRect();
      printf("probe: ps_1_1 %s %06lx\n", name, (unsigned long)middle);
      expect(middle == 0x00FF0000, name);
      return 0;
    };
    if (draw_bump(kPixel11Texbem, "texbem"))
      return 1;
    if (draw_bump(kPixel11Texm3x2, "texm3x2tex"))
      return 1;
    if (draw_bump(kPixel11Texm3x3, "texm3x3tex"))
      return 1;
    if (draw_bump(kPixel11Texdp3, "texdp3tex"))
      return 1;
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->SetVertexDeclaration(declaration.Get()));
    CHECK(device->SetVertexShader(vertex_shader.Get()));
  }

  // the caps' pixel sampler count is the one the device enforces, and the four vertex samplers follow it:
  // D3DVERTEXTEXTURESAMPLER0..3 are API stages 16..19, and stage 20 is the first the device refuses
  for (DWORD stage = 0; stage < caps.MaxSimultaneousTextures; stage++)
    expect(SUCCEEDED(device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT)),
           "a sampler of a stage the caps count was refused");
  for (DWORD stage = D3DVERTEXTEXTURESAMPLER0; stage <= D3DVERTEXTEXTURESAMPLER3; stage++)
    expect(SUCCEEDED(device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT)),
           "a vertex sampler D3D9 requires was refused");
  expect(device->SetSamplerState(D3DVERTEXTEXTURESAMPLER3 + 1, D3DSAMP_MINFILTER, D3DTEXF_POINT) ==
             D3DERR_INVALIDCALL,
         "a sampler past the pixel and vertex stages was accepted");
  expect(device->SetSamplerState(16, D3DSAMP_MINFILTER, D3DTEXF_POINT) == D3DERR_INVALIDCALL,
         "an internal vertex sampler slot was accepted as a public sampler");
  expect(SUCCEEDED(device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr)),
         "the first vertex texture stage was refused");
  expect(device->SetTexture(D3DVERTEXTEXTURESAMPLER3 + 1, nullptr) == D3DERR_INVALIDCALL,
         "a texture stage past the pixel and vertex stages was accepted");
  expect(device->SetTexture(D3DDMAPSAMPLER, nullptr) == D3DERR_INVALIDCALL,
         "the displacement sampler was accepted without displacement mapping");
  expect(SUCCEEDED(device->SetTextureStageState(caps.MaxTextureBlendStages - 1, D3DTSS_COLOROP, D3DTOP_MODULATE)),
         "the last fixed-function stage the caps count was refused");
  expect(device->SetTextureStageState(caps.MaxTextureBlendStages, D3DTSS_COLOROP, D3DTOP_MODULATE) == D3DERR_INVALIDCALL,
         "a texture stage past MaxTextureBlendStages was accepted");

  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(nullptr));
  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
