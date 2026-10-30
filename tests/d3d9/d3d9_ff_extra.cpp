// contract: extra fixed-function stages, linear table fog, FVF-fed programmable draws, and SetStreamSourceFreq
// instancing. Microsoft Learn, Texture Blending, D3DRS_FOGTABLEMODE, SetFVF, SetStreamSourceFreq.
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
static const D3DCOLOR kGreen = D3DCOLOR_XRGB(0, 255, 0);
static const D3DCOLOR kWhite = D3DCOLOR_XRGB(255, 255, 255);
static const D3DCOLOR kCyan = D3DCOLOR_XRGB(0, 255, 255);

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  float u, v;
};

const char *kPassVs = "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                      "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                      "  VSOut o; o.position = position; o.colour = colour; return o;\n"
                      "}\n";
const char *kPassPs = "float4 main(float4 colour : COLOR0) : COLOR { return colour; }\n";
const char *kRepPs = "int4 count : register(i0);\n"
                     "float4 addend : register(c1);\n"
                     "float4 main(float4 colour : COLOR0) : COLOR {\n"
                     "  float4 value = 0;\n"
                     "  [loop] for (int i = 0; i < count.x; i++) value += addend;\n"
                     "  return value;\n"
                     "}\n";
const char *kSm3Vs = "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                     "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                     "  VSOut o; o.position = position; o.colour = colour; return o;\n"
                     "}\n";
const char *kSm3Ps = "float4 main(float4 colour : COLOR0) : COLOR { return colour; }\n";

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

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_ff_extra"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_ff_extra", "d3d9_ff_extra", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient, nullptr,
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
  CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));

  const Vertex quad[4] = {
      {0, 0, 1, 1, kCyan, 0, 0},
      {(float)kClient, 0, 1, 1, kCyan, 1, 0},
      {0, (float)kClient, 1, 1, kCyan, 0, 1},
      {(float)kClient, (float)kClient, 1, 1, kCyan, 1, 1},
  };

  auto pixel = [&](UINT x, UINT y) -> DWORD {
    ComPtr<IDirect3DSurface9> back, staging;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
      return 0;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging,
                                                    nullptr)))
      return 0;
    if (FAILED(device->GetRenderTargetData(back.Get(), staging.Get())))
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD value = *(DWORD *)((char *)locked.pBits + y * locked.Pitch + x * 4) & 0x00FFFFFF;
    staging->UnlockRect();
    return value;
  };

  auto draw_quad = [&]() -> HRESULT {
    CHECK(device->BeginScene());
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    CHECK(device->EndScene());
    return hr;
  };

  auto make_solid = [&](D3DCOLOR colour, IDirect3DTexture9 **out) -> HRESULT {
    HRESULT hr = device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, nullptr);
    if (FAILED(hr))
      return hr;
    D3DLOCKED_RECT locked{};
    hr = (*out)->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr))
      return hr;
    *(DWORD *)locked.pBits = colour;
    return (*out)->UnlockRect(0);
  };

  // ---- two blend stages: red ADD green is yellow ----------------------------------------------------------------
  {
    ComPtr<IDirect3DTexture9> red, green;
    CHECK(make_solid(kRed | 0xFF000000, &red));
    CHECK(make_solid(kGreen | 0xFF000000, &green));
    CHECK(device->SetTexture(0, red.Get()));
    CHECK(device->SetTexture(1, green.Get()));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    expect(SUCCEEDED(draw_quad()), "a two-stage fixed-function draw was refused");
    printf("probe: two-stage 32,32=%06lx\n", (unsigned long)pixel(32, 32));
    expect(pixel(32, 32) == 0x00FFFF00, "stage 1 ADD of red and green was not yellow");
    CHECK(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
    CHECK(device->SetTexture(1, nullptr));
  }

  // ---- linear table fog: a far pixel is the fog colour ----------------------------------------------------------
  {
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    struct ColourVertex {
      float x, y, z, rhw;
      D3DCOLOR colour;
    };
    const ColourVertex fog_quad[4] = {
        {0, 0, 1, 1, kCyan},
        {(float)kClient, 0, 1, 1, kCyan},
        {0, (float)kClient, 1, 1, kCyan},
        {(float)kClient, (float)kClient, 1, 1, kCyan},
    };
    CHECK(device->SetRenderState(D3DRS_FOGENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR));
    CHECK(device->SetRenderState(D3DRS_FOGCOLOR, kWhite));
    const float start = 0.0f, end = 0.5f;
    CHECK(device->SetRenderState(D3DRS_FOGSTART, *(const DWORD *)&start));
    CHECK(device->SetRenderState(D3DRS_FOGEND, *(const DWORD *)&end));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fog_quad, sizeof(ColourVertex))),
           "a fogged fixed-function draw was refused");
    CHECK(device->EndScene());
    printf("probe: fog 32,32=%06lx\n", (unsigned long)pixel(32, 32));
    expect(pixel(32, 32) == (kWhite & 0x00FFFFFF), "linear table fog did not replace a far pixel with FOGCOLOR");
    CHECK(device->SetRenderState(D3DRS_FOGENABLE, FALSE));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
  }

  struct ClipVertex {
    float x, y, z;
    D3DCOLOR colour;
  };
  const ClipVertex clip_quad[4] = {
      {-1.0f, -1.0f, 0.0f, kCyan},
      {1.0f, -1.0f, 0.0f, kCyan},
      {-1.0f, 1.0f, 0.0f, kCyan},
      {1.0f, 1.0f, 0.0f, kCyan},
  };
  auto draw_clip = [&]() -> HRESULT {
    CHECK(device->BeginScene());
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, clip_quad, sizeof(ClipVertex));
    CHECK(device->EndScene());
    return hr;
  };

  // ---- FVF-fed programmable draw --------------------------------------------------------------------------------
  {
    ComPtr<ID3DBlob> vs = compile(kPassVs, "vs_2_0");
    ComPtr<ID3DBlob> ps = compile(kPassPs, "ps_2_0");
    expect(vs && ps, "Microsoft's compiler made no FVF-fed programs");
    if (vs && ps) {
      ComPtr<IDirect3DVertexShader9> vertex;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      CHECK(device->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &vertex));
      CHECK(device->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &pixel_shader));
      CHECK(device->SetVertexDeclaration(nullptr));
      CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
      CHECK(device->SetVertexShader(vertex.Get()));
      CHECK(device->SetPixelShader(pixel_shader.Get()));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      expect(SUCCEEDED(draw_clip()), "an FVF-fed programmable draw was refused");
      expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "an FVF-fed programmable draw did not write the vertex colour");
      CHECK(device->SetVertexShader(nullptr));
      CHECK(device->SetPixelShader(nullptr));
    }
  }

  // ---- vs_3_0/ps_3_0 passthrough --------------------------------------------------------------------------------
  {
    ComPtr<ID3DBlob> vs = compile(kSm3Vs, "vs_3_0");
    ComPtr<ID3DBlob> ps = compile(kSm3Ps, "ps_3_0");
    expect(vs && ps, "Microsoft's compiler made no shader model 3 programs");
    if (vs && ps) {
      ComPtr<IDirect3DVertexShader9> vertex;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      CHECK(device->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &vertex));
      CHECK(device->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &pixel_shader));
      CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
      CHECK(device->SetVertexShader(vertex.Get()));
      CHECK(device->SetPixelShader(pixel_shader.Get()));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      expect(SUCCEEDED(draw_clip()), "a vs_3_0/ps_3_0 draw was refused");
      expect(pixel(32, 32) == (kCyan & 0x00FFFFFF), "vs_3_0/ps_3_0 did not write the vertex colour");
      CHECK(device->SetVertexShader(nullptr));
      CHECK(device->SetPixelShader(nullptr));
    }
  }

  // ---- REP: add c1 four times -----------------------------------------------------------------------------------
  {
    ComPtr<ID3DBlob> vs = compile(kPassVs, "vs_2_0");
    ComPtr<ID3DBlob> ps = compile(kRepPs, "ps_3_0");
    expect(vs && ps, "Microsoft's compiler made no REP program");
    if (vs && ps) {
      ComPtr<IDirect3DVertexShader9> vertex;
      ComPtr<IDirect3DPixelShader9> pixel_shader;
      CHECK(device->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &vertex));
      CHECK(device->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &pixel_shader));
      CHECK(device->SetVertexShader(vertex.Get()));
      CHECK(device->SetPixelShader(pixel_shader.Get()));
      const int count[4] = {4, 0, 0, 0};
      const float addend[4] = {0.0f, 0.25f, 0.0f, 1.0f};
      CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
      CHECK(device->SetPixelShaderConstantI(0, count, 1));
      CHECK(device->SetPixelShaderConstantF(1, addend, 1));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      expect(SUCCEEDED(draw_clip()), "a REP pixel shader draw was refused");
      printf("probe: rep 32,32=%06lx\n", (unsigned long)pixel(32, 32));
      expect(pixel(32, 32) == 0x0000FF00, "REP did not add its constant four times");
      CHECK(device->SetVertexShader(nullptr));
      CHECK(device->SetPixelShader(nullptr));
    }
  }

  // ---- instancing: INDEXEDDATA | 2 draws the same triangle twice, additive blend doubles the colour --------------
  {
    CHECK(device->SetVertexShader(nullptr));
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE));
    CHECK(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE));
    struct ColourVertex {
      float x, y, z, rhw;
      D3DCOLOR colour;
    };
    const ColourVertex dim[4] = {
        {0, 0, 0, 1, D3DCOLOR_XRGB(64, 0, 0)},
        {(float)kClient, 0, 0, 1, D3DCOLOR_XRGB(64, 0, 0)},
        {0, (float)kClient, 0, 1, D3DCOLOR_XRGB(64, 0, 0)},
        {(float)kClient, (float)kClient, 0, 1, D3DCOLOR_XRGB(64, 0, 0)},
    };
    ComPtr<IDirect3DVertexBuffer9> buffer;
    CHECK(device->CreateVertexBuffer(sizeof(dim), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &buffer, nullptr));
    void *bytes = nullptr;
    CHECK(buffer->Lock(0, 0, &bytes, 0));
    memcpy(bytes, dim, sizeof(dim));
    CHECK(buffer->Unlock());
    CHECK(device->SetStreamSource(0, buffer.Get(), 0, sizeof(ColourVertex)));
    CHECK(device->SetStreamSourceFreq(0, 1));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    CHECK(device->EndScene());
    const DWORD once = pixel(32, 32);
    CHECK(device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    CHECK(device->EndScene());
    const DWORD twice = pixel(32, 32);
    printf("probe: instances once=%06lx twice=%06lx\n", (unsigned long)once, (unsigned long)twice);
    expect(((twice >> 16) & 0xFF) >= 2 * ((once >> 16) & 0xFF) && ((once >> 16) & 0xFF) >= 32,
           "INDEXEDDATA | 2 did not draw two additive instances");
    CHECK(device->SetStreamSourceFreq(0, 1));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
