// contract: the caps say what this device can be asked for, and a game that reads them gets it.
// - VertexShaderVersion and PixelShaderVersion name the shader models the translator certifies (vs_2_0 and ps_2_0), and
//   MaxVertexShaderConst the 256 float registers a stage has; the flow-control caps are 0 because the translator reads no
//   flow control.
// - a program of the version the caps name is created and drawn, and a program of a version above them is refused by
//   CreateVertexShader/CreatePixelShader: a version reported but refused here would be worse than a lower cap, because a
//   game plans its shaders from these numbers.
// - MaxSimultaneousTextures is the eight stages a pixel shader samples and MaxTextureBlendStages the two stages of the
//   fixed-function blender: the caps are the counts the tests certify.
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

// the oldest pixel shader there is, which this build does not translate yet: it is refused where it is made, with the
// reason the log carries
const DWORD kPixel11[] = {
    0xFFFF0101, // ps_1_1
    0x02000001, 0x800F0000, 0xA0E40000, // mov r0, c0
    0x0000FFFF, // end
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
    expect(caps.MaxSimultaneousTextures == 8, "MaxSimultaneousTextures is not the eight stages a draw samples");
    expect(caps.MaxTextureBlendStages == 2, "MaxTextureBlendStages is not the two fixed-function stages");
    expect(caps.NumSimultaneousRTs == 1, "NumSimultaneousRTs is not the one target a draw writes");
    expect(caps.MaxStreams == 4, "MaxStreams is not the four vertex streams a draw reads");
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

  // and the 1.x pixel programs, which the translator does not read yet, are refused where they are made rather than at
  // a draw: a device whose caps say ps_2_0 does not make a ps_1_1 program this build cannot draw
  ComPtr<IDirect3DPixelShader9> pixel11;
  expect(device->CreatePixelShader(kPixel11, &pixel11) == D3DERR_INVALIDCALL,
         "a ps_1_1 program was made, though the translator does not read its registers");

  // the caps' texture counts are the ones the device enforces: eight stages are set, and a ninth is refused
  for (DWORD stage = 0; stage < caps.MaxSimultaneousTextures; stage++)
    expect(SUCCEEDED(device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT)),
           "a sampler of a stage the caps count was refused");
  expect(device->SetSamplerState(caps.MaxSimultaneousTextures, D3DSAMP_MINFILTER, D3DTEXF_POINT) == D3DERR_INVALIDCALL,
         "a sampler past MaxSimultaneousTextures was accepted");
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
