// contract: a D3DFVF_XYZ vertex carries no colour, so the fixed-function pipeline supplies D3D9's opaque white, and a
// draw that binds no texture still reads the white texel rather than whatever the previous draw left bound. Wine's
// visual.c test_viewport draws D3DFVF_XYZ with lighting off and expects white; before this test existed the
// position-only vertices were read at the diffuse layout's stride, so the pixel shader interpolated the next vertex's
// position bytes as the colour and drew a gradient.
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

static HWND
create_window() {
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_position"};
  RegisterClassA(&wc);
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("d3d9_position", "d3d9_position", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left,
                       rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  HWND window = create_window();
  ComPtr<IDirect3D9> d3d9;
  d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d9) {
    printf("failed: Direct3DCreate9 made no object\n");
    return 1;
  }
  D3DPRESENT_PARAMETERS present{};
  present.Windowed = TRUE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.BackBufferWidth = 64;
  present.BackBufferHeight = 64;
  present.BackBufferFormat = D3DFMT_A8R8G8B8;
  present.EnableAutoDepthStencil = TRUE;
  present.AutoDepthStencilFormat = D3DFMT_D24S8;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present,
                           device.GetAddressOf()));

  ComPtr<IDirect3DSurface9> backbuffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backbuffer.GetAddressOf()));
  auto read = [&](int x, int y) {
    D3DSURFACE_DESC desc{};
    backbuffer->GetDesc(&desc);
    ComPtr<IDirect3DSurface9> readback;
    ComPtr<IDirect3DDevice9> device_for_copy = device;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                   readback.GetAddressOf(), nullptr)) ||
        FAILED(device->GetRenderTargetData(backbuffer.Get(), readback.Get())))
      return 0xdeadbeefu;
    D3DLOCKED_RECT locked{};
    if (FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0xdeadbeefu;
    unsigned value = ((const unsigned *)locked.pBits)[y * locked.Pitch / 4 + x];
    readback->UnlockRect();
    return value;
  };

  static const struct {
    float x, y, z;
  } quad[] = {
      {-1.0f, -1.0f, 0.5f},
      {-1.0f,  1.0f, 0.5f},
      { 1.0f, -1.0f, 0.5f},
      { 1.0f,  1.0f, 0.5f},
  };
  D3DVIEWPORT9 vp{0, 0, 64, 64, 0.0f, 1.0f};

  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_ZENABLE, TRUE);
  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  device->SetFVF(D3DFVF_XYZ);
  CHECK(device->SetViewport(&vp));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0));
  CHECK(device->BeginScene());
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
  CHECK(device->EndScene());

  // every pixel of the full-target quad is D3D9's opaque white, not a gradient of the vertex positions
  const unsigned corners[4][2] = {{1, 1}, {62, 1}, {1, 62}, {62, 62}};
  for (const auto &corner : corners) {
    unsigned colour = read((int)corner[0], (int)corner[1]);
    char what[96];
    snprintf(what, sizeof(what), "position-only draw at (%u,%u) is %08x, not opaque white", corner[0], corner[1],
             colour);
    expect(colour == 0xffffffffu, what);
  }

  // a textured draw with no texture bound samples D3D9's white texel, so the stage's colour is the vertex's
  struct textured {
    float x, y, z, u, v;
  };
  static const textured texquad[] = {
      {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f},
      {-1.0f,  1.0f, 0.5f, 0.0f, 1.0f},
      { 1.0f, -1.0f, 0.5f, 1.0f, 0.0f},
      { 1.0f,  1.0f, 0.5f, 1.0f, 1.0f},
  };
  device->SetTexture(0, nullptr);
  CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0));
  CHECK(device->BeginScene());
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, texquad, sizeof(texquad[0])));
  CHECK(device->EndScene());
  expect(read(32, 32) == 0xffffffffu, "an unbound texture samples D3D9's white texel");

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
