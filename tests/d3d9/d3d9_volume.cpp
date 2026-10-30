// contract: CreateVolumeTexture makes a TEXTURE3D (Microsoft Learn, IDirect3DDevice9::CreateVolumeTexture).
// LockBox writes a voxel, SetTexture samples it from a programmable ps_2_0 tex3D, GetVolumeLevel is the same lock,
// and UpdateTexture copies a managed volume into a default-pool one.
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
static const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);
static const D3DCOLOR kRed = D3DCOLOR_XRGB(255, 0, 0);

const char *kVertexHlsl = "struct VSOut { float4 position : POSITION; float3 uvw : TEXCOORD0; };\n"
                          "VSOut main(float4 position : POSITION, float3 uvw : TEXCOORD0) {\n"
                          "  VSOut o; o.position = position; o.uvw = uvw; return o;\n"
                          "}\n";
const char *kPixelHlsl = "sampler3D s0 : register(s0);\n"
                         "float4 main(float3 uvw : TEXCOORD0) : COLOR { return tex3D(s0, uvw); }\n";

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

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_volume"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_volume", "d3d9_volume", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient, nullptr,
                              nullptr, wc.hInstance, nullptr);
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
  expect(SUCCEEDED(d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_VOLUMETEXTURE,
                                           D3DFMT_A8R8G8B8)),
         "CheckDeviceFormat refused a volume of A8R8G8B8");
  D3DCAPS9 caps{};
  CHECK(d3d9->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps));
  expect((caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) != 0, "TextureCaps does not advertise volume maps");
  expect(caps.MaxVolumeExtent >= 4, "MaxVolumeExtent is below a 4^3 volume");

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

  ComPtr<IDirect3DVolumeTexture9> volume;
  CHECK(device->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr));
  D3DLOCKED_BOX locked{};
  CHECK(volume->LockBox(0, &locked, nullptr, 0));
  for (UINT z = 0; z < 4; z++) {
    for (UINT y = 0; y < 4; y++) {
      DWORD *row = (DWORD *)((char *)locked.pBits + z * locked.SlicePitch + y * locked.RowPitch);
      for (UINT x = 0; x < 4; x++)
        row[x] = (x == 1 && y == 1 && z == 1) ? kRed : D3DCOLOR_XRGB(0, 255, 0);
    }
  }
  CHECK(volume->UnlockBox(0));

  ComPtr<IDirect3DVolume9> level;
  CHECK(volume->GetVolumeLevel(0, &level));
  expect(level != nullptr, "GetVolumeLevel of mip 0 made no volume");
  D3DVOLUME_DESC desc{};
  CHECK(level->GetDesc(&desc));
  expect(desc.Width == 4 && desc.Height == 4 && desc.Depth == 4, "the volume's size is not 4x4x4");

  ComPtr<IDirect3DVolumeTexture9> destination;
  CHECK(device->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &destination, nullptr));
  CHECK(device->UpdateTexture(volume.Get(), destination.Get()));

  ComPtr<ID3DBlob> vs = compile(kVertexHlsl, "vs_2_0");
  ComPtr<ID3DBlob> ps = compile(kPixelHlsl, "ps_2_0");
  expect(vs && ps, "Microsoft's compiler made no volume shaders");
  if (!vs || !ps)
    return 1;
  ComPtr<IDirect3DVertexShader9> vertex;
  ComPtr<IDirect3DPixelShader9> pixel;
  CHECK(device->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &vertex));
  CHECK(device->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &pixel));
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  CHECK(device->SetVertexShader(vertex.Get()));
  CHECK(device->SetPixelShader(pixel.Get()));
  CHECK(device->SetTexture(0, destination.Get()));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  struct Vertex {
    float x, y, z, u, v, w;
  };
  const float texel = (1.0f + 0.5f) / 4.0f;
  const Vertex triangle[3] = {
      {-0.5f, -0.5f, 0, texel, texel, texel},
      {0.5f, -0.5f, 0, texel, texel, texel},
      {0.0f, 0.5f, 0, texel, texel, texel},
  };
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  CHECK(device->BeginScene());
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
  CHECK(device->EndScene());

  ComPtr<IDirect3DSurface9> back, readback;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  CHECK(device->GetRenderTargetData(back.Get(), readback.Get()));
  D3DLOCKED_RECT locked_rect{};
  CHECK(readback->LockRect(&locked_rect, nullptr, D3DLOCK_READONLY));
  const DWORD *row = (const DWORD *)((const char *)locked_rect.pBits + 32 * locked_rect.Pitch);
  const D3DCOLOR got = row[32] & 0x00FFFFFF;
  readback->UnlockRect();
  expect(got == (kRed & 0x00FFFFFF), "sampling voxel (1,1,1) of the volume was not red");
  printf("%s: %u wrong of %u checks (pixel %06lx)\n", wrong ? "failed" : "passed", wrong, checks, (unsigned long)got);
  return wrong ? 1 : 0;
}
