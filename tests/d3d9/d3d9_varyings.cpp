// contract: a pixel shader may read any usage the vertex declaration carries, including over a transformed
// (POSITIONT) vertex: ps_3_0's dcl_* names the usage and the draw delivers the interpolator that usage declared
// ("Declaration Types and Usage Flags", Microsoft Learn; Wine visual.c pretransformed_varying_test, whose twelve
// cases carry Windows-observed colours). The draws must be accepted, not hang the device.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <vector>
#include <cstdio>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

#define REQUIRE(x)                                                                                                     \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    wrong++;                                                                                                           \
    return;                                                                                                            \
  }

static const UINT kClient = 64;

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_varyings"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_varyings", "d3d9_varyings", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient, nullptr,
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
  present.EnableAutoDepthStencil = TRUE;
  present.AutoDepthStencilFormat = D3DFMT_D16;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));

  // the monster vertex declaration: every usage FLOAT4 but the colours, as Wine's pretransformed_varying_test
  // declares it
  static const D3DVERTEXELEMENT9 elements[] = {
      {0,   0, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT,    0},
      {0,  16, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT,  0},
      {0,  32, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
      {0,  48, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,       0},
      {0,  64, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_FOG,          0},
      {0,  80, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     0},
      {0,  96, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,      0},
      {0, 112, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL,     0},
      {0, 128, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_DEPTH,        0},
      {0, 144, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,       0},
      {0, 148, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,       1},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));

  // one vertex of Wine's quad: the usage's value is what the colour check reads back
  struct Vertex {
    float position[4];
    float blend_weight[4];
    float blend_indices[4];
    float normal[4];
    float fog[4];
    float texcoord[4];
    float tangent[4];
    float binormal[4];
    float depth[4];
    D3DCOLOR diffuse;
    D3DCOLOR specular;
  };
  static_assert(sizeof(Vertex) == 152, "the declaration's stride");
  const Vertex vertices[4] = {
      {{ 16.0f,  16.0f, 0.1f, 1.0f}, {0.1f, 0.1f, 0.1f, 0.1f}, {0.2f, 0.2f, 0.2f, 0.2f}, {0.3f, 0.3f, 0.3f, 0.3f},
       {0.4f, 0.4f, 0.4f, 0.4f}, {0.5f, 0.55f, 0.55f, 0.55f}, {0.6f, 0.6f, 0.6f, 0.7f}, {0.7f, 0.7f, 0.7f, 0.6f},
       {0.8f, 0.8f, 0.8f, 0.8f}, 0xe6e6e6e6, 0x224488ff},
      {{48.0f,  16.0f, 0.1f, 1.0f}, {0.1f, 0.1f, 0.1f, 0.1f}, {0.2f, 0.2f, 0.2f, 0.2f}, {0.3f, 0.3f, 0.3f, 0.3f},
       {0.4f, 0.4f, 0.4f, 0.4f}, {0.5f, 0.55f, 0.55f, 0.55f}, {0.6f, 0.6f, 0.6f, 0.7f}, {0.7f, 0.7f, 0.7f, 0.6f},
       {0.8f, 0.8f, 0.8f, 0.8f}, 0xe6e6e6e6, 0x224488ff},
      {{16.0f,  48.0f, 0.1f, 1.0f}, {0.1f, 0.1f, 0.1f, 0.1f}, {0.2f, 0.2f, 0.2f, 0.2f}, {0.3f, 0.3f, 0.3f, 0.3f},
       {0.4f, 0.4f, 0.4f, 0.4f}, {0.5f, 0.55f, 0.55f, 0.55f}, {0.6f, 0.6f, 0.6f, 0.7f}, {0.7f, 0.7f, 0.7f, 0.6f},
       {0.8f, 0.8f, 0.8f, 0.8f}, 0xe6e6e6e6, 0x224488ff},
      {{48.0f,  48.0f, 0.1f, 1.0f}, {0.1f, 0.1f, 0.1f, 0.1f}, {0.2f, 0.2f, 0.2f, 0.2f}, {0.3f, 0.3f, 0.3f, 0.3f},
       {0.4f, 0.4f, 0.4f, 0.4f}, {0.5f, 0.55f, 0.55f, 0.55f}, {0.6f, 0.6f, 0.6f, 0.7f}, {0.7f, 0.7f, 0.7f, 0.6f},
       {0.8f, 0.8f, 0.8f, 0.8f}, 0xe6e6e6e6, 0x224488ff},
  };

  ComPtr<IDirect3DSurface9> readback, back;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      back.Reset();
      return 0;
    }
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    back.Reset();
    return got;
  };

  // each case: a ps_3_0 that reads one usage and writes it to oC0; the colour is the usage's value, as Wine's
  // pretransformed_varying_test records from Windows (todo cases included: their colours are Windows observations)
  struct Case {
    const char *name;
    unsigned usage;  // the dcl's usage bits (0x8000 | usage<<16 | index<<8)
    D3DCOLOR expected;
    bool alpha_write;  // mov oC0, v0.w instead of the whole register
    D3DCOLOR expected_alpha;
  };
  const Case cases[] = {
      {"grey",        0,          0x00ffffff, false, 0},
      {"blendweight",  0x80000001, 0x00191919, false, 0},
      {"blendindices", 0x80000002, 0x00333333, false, 0},
      {"normal",       0x80000003, 0x004c4c4c, false, 0},
      {"texcoord0",    0x80000005, 0x00808c8c, false, 0},
      {"tangent",      0x80000006, 0x00999999, false, 0},
      {"binormal",     0x80000007, 0x00b2b2b2, false, 0},
      {"color",        0x8000000a, 0x00e6e6e6, false, 0},
      {"fog",          0x8000000b, 0x00666666, false, 0},
      {"depth",        0x8000000c, 0x00cccccc, false, 0},
      {"specular",     0x8001000a, 0x004488ff, false, 0},
      {"texcoord1",    0x80010005, 0x00000000, false, 0},
      {"texcoord1 w",  0x80010005, 0x00000000, true,  0x00000000},
  };

  for (const Case &c : cases) {
    // ps_3_0: dcl_<usage>, v0 / def c0, 1, 1, 1, 1 / mov oC0, v0 (or .w) or c0. the oC0 destination carries
    // COLOROUT's register type (8), whose bit sits past the three at 28: 0x800f0800 whole, 0x800f8800 for .w
    std::vector<DWORD> code = {0xffff0300u};
    if (c.usage)
      code.insert(code.end(), {0x0200001fu, c.usage, 0x900f0000u});  // dcl, the semantic token, v0
    code.insert(code.end(),
                {0x05000051u, 0xa00f0000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,  // def c0, 1,1,1,1
                 0x02000001u, (c.alpha_write ? 0x800f8800u : 0x800f0800u),
                 c.usage ? 0x90e40000u : 0xa0e40000u,  // mov oC0, v0 (or .w), or the constant c0
                 0x0000ffffu});
    ComPtr<IDirect3DPixelShader9> shader;
    HRESULT hr = device->CreatePixelShader(code.data(), &shader);
    checks++;
    if (FAILED(hr) && wrong++ < 16) {
      printf("wrong: %s: CreatePixelShader %08lx\n", c.name, (unsigned long)hr);
      continue;
    }
    CHECK(device->SetPixelShader(shader.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1.0f, 0));
    CHECK(device->BeginScene());
    hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    checks++;
    if (FAILED(hr) && wrong++ < 16)
      printf("wrong: %s: DrawPrimitiveUP %08lx (the draw must be accepted, not hang the device)\n", c.name,
             (unsigned long)hr);
    CHECK(device->EndScene());
    const D3DCOLOR outside = pixel(2, 2);
    const D3DCOLOR got = pixel(16, 16);
    checks++;
    if (outside != 0x00ffffff && wrong++ < 16)
      printf("wrong: %s: the pixel outside the quad is %06x, not the clear's ffffff\n", c.name, outside);
    const D3DCOLOR want = (c.alpha_write ? c.expected_alpha : c.expected) & 0x00FFFFFF;
    printf("%-14s hr=%08lx got=%06lx want=%06lx\n", c.name, (unsigned long)hr, (unsigned long)got,
           (unsigned long)want);
    // Wine's own conformance harness compares a colour by a tolerance (visual.c color_match, 13 a channel at most
    // away), which the interpolation's last bit at a half-way value the Windows observation records (texcoord0's
    // 0.5) sits under
    auto within = [](D3DCOLOR a, D3DCOLOR b) {
      for (UINT channel = 0; channel < 3; channel++)
        if (abs((int)((a >> (channel * 8)) & 0xff) - (int)((b >> (channel * 8)) & 0xff)) > 13)
          return false;
      return true;
    };
    expect(within(got, want), c.name);
  }

  printf("%s: %d wrong of %d checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
