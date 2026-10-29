// contract: a game's pixel shader samples the texture its stage holds, and the pixels a draw makes are the texture's.
// - SetTexture(0) is the texture texld's sampler s0 reads, and the sampler states the device holds are the sampler it
//   samples through: the point filter a texel's own colour, the linear one the blend of the texels around a coordinate.
// - the address modes are the sample's at the edge of the texture: a wrapped coordinate repeats it and a clamped one
//   holds its edge.
// - the components a texture is stored in are the ones a shader reads (A8R8G8B8), and a compressed one (DXT1) is
//   uploaded as the blocks it is stored as and sampled as the colours they make.
// - a stage with no texture samples D3D9's default texture, one opaque white texel.
// - both a hand-written ps_2_0 (the tokens of this test, so the translator is proved without a compiler) and one
//   Microsoft's compiler makes of HLSL at test time draw the same pixels.
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texld---ps-2-0
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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

namespace {

const UINT kClient = 64;
// what a frame is cleared to: a colour no texel has, so a pixel of it says the draw left that pixel alone
const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 0);
const DWORD kClearPixel = kClear & 0x00FFFFFF;

// the colours the test's texture is made of, one a column: a texel that is its own makes a filter or an address mode
// the one that sampled it
const D3DCOLOR kColumns[4] = {D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0), D3DCOLOR_XRGB(0, 0, 255),
                              D3DCOLOR_XRGB(255, 255, 255)};
// the D3DCOLOR values those columns are, as an A8R8G8B8 surface's bytes read as a word (the red in the high byte of the
// three, as D3DCOLOR_XRGB is)
const DWORD kColumnPixels[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};

// a vertex the declaration reads: a float3 position and the texture coordinate the pixel shader samples at
struct Vertex {
  float x, y, z;
  float u, v;
};

// the hand-written vs_2_0: dcl_position v0, dcl_texcoord v1, mov oPos, v0, mov oT0, v1 (the position, and the texture
// coordinate in the register the pixel shader reads)
const DWORD kHandVertexShader[] = {
    0xFFFE0200, // vs_2_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x0200001F, 0x00000005, 0x900F0001, // dcl_texcoord0 v1
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x02000001, 0xE00F0000, 0x90E40001, // mov oT0, v1
    0x0000FFFF, // end
};

// the hand-written ps_2_0: dcl t0, dcl_2d s0, texld r0, t0, s0, mov oC0, r0 (the texel the stage's texture holds at the
// coordinate, written to the first target). the tokens are Microsoft's compiler's own for the program below
const DWORD kHandPixelShader[] = {
    0xFFFF0200, // ps_2_0
    0x0200001F, 0x80000000, 0xB0030000, // dcl t0
    0x0200001F, 0x90000000, 0xA00F0800, // dcl_2d s0
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800, // texld r0, t0, s0
    0x02000001, 0x800F0800, 0x80E40000, // mov oC0, r0
    0x0000FFFF, // end
};

const char *kVertexHlsl = "struct VSOut { float4 position : POSITION; float2 uv : TEXCOORD0; };\n"
                          "VSOut main(float4 position : POSITION, float2 uv : TEXCOORD0) {\n"
                          "  VSOut o;\n"
                          "  o.position = position;\n"
                          "  o.uv = uv;\n"
                          "  return o;\n"
                          "}\n";
const char *kPixelHlsl = "sampler2D s0 : register(s0);\n"
                         "float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(s0, uv); }\n";

ComPtr<ID3DBlob>
compile(const char *hlsl, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", profile, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

// the four vertices of a quad over the whole of clip space, with the texture coordinates they carry
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
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_texture_draw"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_texture_draw", "d3d9_texture_draw", WS_OVERLAPPEDWINDOW, 0, 0,
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

  // the declaration the vertex shader reads: the position, then the texture coordinate
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  // the quad is wound whichever way the two triangles of a strip are; D3D9's default cull mode culls one of them
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  ComPtr<IDirect3DVertexShader9> vertex_shader;
  ComPtr<IDirect3DPixelShader9> pixel_shader;
  CHECK(device->CreateVertexShader(kHandVertexShader, &vertex_shader));
  CHECK(device->CreatePixelShader(kHandPixelShader, &pixel_shader));
  CHECK(device->SetVertexShader(vertex_shader.Get()));
  CHECK(device->SetPixelShader(pixel_shader.Get()));

  // a frame's pixels: the back buffer, read through a system-memory surface as the triangle test reads its frames
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

  // ---- a texture whose columns are four colours, painted through a managed lock ----------------------------------
  ComPtr<IDirect3DTexture9> texture;
  CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
  {
    D3DLOCKED_RECT locked{};
    CHECK(texture->LockRect(0, &locked, nullptr, 0));
    for (UINT y = 0; y < 4; y++) {
      DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
      for (UINT x = 0; x < 4; x++)
        row[x] = kColumns[x];
    }
    CHECK(texture->UnlockRect(0));
  }
  CHECK(device->SetTexture(0, texture.Get()));

  // the sampler D3D9 starts with: point filtering, so every pixel is one texel's colour
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
  HRESULT hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
  expect(SUCCEEDED(hr), "a draw sampling the stage's texture was refused");
  printf("probe: 8,30=%06lx 24,30=%06lx 40,30=%06lx 56,30=%06lx 15,30=%06lx\n", (unsigned long)pixel(8, 30),
         (unsigned long)pixel(24, 30), (unsigned long)pixel(40, 30), (unsigned long)pixel(56, 30),
         (unsigned long)pixel(15, 30));
  // a 4-texel texture across 64 pixels: the four quadrants of a column are that column's colour
  for (UINT column = 0; column < 4; column++)
    expect(pixel(8 + column * 16, 30) == kColumnPixels[column], "a point-sampled column is not its texel's colour");
  expect(pixel(8, 2) == kColumnPixels[0] && pixel(56, 61) == kColumnPixels[3],
         "the texture's columns do not span the quad's height");
  expect(pixel(15, 30) == kColumnPixels[0], "a point-sampled pixel beside a texel's centre took another texel");
  expect(pixel(32, 32) != kClearPixel, "the quad did not cover the frame");

  // the linear filter: a pixel between two texel centres is the blend of them, and the same pixel is one texel's
  // colour under the point filter (checked above), which is what tells the two filters apart
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
  hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
  expect(SUCCEEDED(hr), "a draw through a linear sampler was refused");
  printf("probe: linear 15,30=%06lx 8,30=%06lx\n", (unsigned long)pixel(15, 30), (unsigned long)pixel(8, 30));
  {
    // x = 15 is u = 15.5/64 = 0.242, between the centres of texels 0 (0.125) and 1 (0.375): red and green both
    const DWORD blended = pixel(15, 30);
    const DWORD red = (blended >> 16) & 0xFF, green = (blended >> 8) & 0xFF, blue = blended & 0xFF;
    expect(red > 40 && green > 40 && blue < 40, "a linearly sampled pixel is not the blend of its texels");
    // and a pixel on a texel's centre is still that texel's colour, near enough
    const DWORD centred = pixel(8, 30);
    expect(((centred >> 16) & 0xFF) > 200 && ((centred >> 8) & 0xFF) < 40,
           "a linearly sampled texel's centre is not its colour");
  }
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));

  // the address modes, at the edge of the texture: the quad names coordinates from -1 to 1, so its left quarter is
  // outside the texture and repeats it or holds its edge, depending on the mode
  hr = draw_quad(-1.0f, 0.0f, 1.0f, 1.0f);
  expect(SUCCEEDED(hr), "a draw whose coordinates leave the texture was refused");
  // x = 27 is u = -1 + 2 * 27.5/64 = -0.141: clamped it is the first column, wrapped it is 0.859 of the way across the
  // texture, which is the fourth
  printf("probe: clamp 27,30=%06lx\n", (unsigned long)pixel(27, 30));
  expect(pixel(27, 30) == kColumnPixels[0], "a clamped coordinate outside the texture did not hold the edge texel");
  // a coordinate inside the texture is the same under both, which says the difference above is the mode's
  expect(pixel(36, 30) == kColumnPixels[0], "a coordinate inside the texture under clamp took another texel");
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP));
  hr = draw_quad(-1.0f, 0.0f, 1.0f, 1.0f);
  expect(SUCCEEDED(hr), "a draw through a wrapping sampler was refused");
  printf("probe: wrap 27,30=%06lx 36,30=%06lx\n", (unsigned long)pixel(27, 30), (unsigned long)pixel(36, 30));
  expect(pixel(27, 30) == kColumnPixels[3], "a wrapped coordinate outside the texture did not repeat it");
  expect(pixel(36, 30) == kColumnPixels[0], "a wrapped coordinate inside the texture took another texel");

  // ---- Microsoft's compiler's programs draw the same pixels -------------------------------------------------------
  ComPtr<ID3DBlob> vertex_code = compile(kVertexHlsl, "vs_2_0");
  ComPtr<ID3DBlob> pixel_code = compile(kPixelHlsl, "ps_2_0");
  expect(vertex_code != nullptr && pixel_code != nullptr, "Microsoft's compiler made no shader model 2 programs");
  if (vertex_code && pixel_code) {
    ComPtr<IDirect3DVertexShader9> compiled_vertex;
    ComPtr<IDirect3DPixelShader9> compiled_pixel;
    CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &compiled_vertex));
    CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &compiled_pixel));
    CHECK(device->SetVertexShader(compiled_vertex.Get()));
    CHECK(device->SetPixelShader(compiled_pixel.Get()));
    CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw with Microsoft's sampling ps_2_0 was refused");
    printf("probe: compiled 8,30=%06lx 24,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(24, 30));
    for (UINT column = 0; column < 4; column++)
      expect(pixel(8 + column * 16, 30) == kColumnPixels[column],
             "Microsoft's sampling program did not draw its texel's colour");
    CHECK(device->SetVertexShader(vertex_shader.Get()));
    CHECK(device->SetPixelShader(pixel_shader.Get()));
  }

  // ---- a compressed texture: DXT1's blocks are uploaded as they are and sampled as the colours they make ----------
  ComPtr<IDirect3DTexture9> compressed;
  hr = device->CreateTexture(4, 4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &compressed, nullptr);
  expect(SUCCEEDED(hr), "a DXT1 texture was refused");
  if (SUCCEEDED(hr)) {
    // one block: colour0 red, colour1 green, and every texel's two bits saying colour0
    const BYTE block[8] = {0x00, 0xF8, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00};
    D3DLOCKED_RECT locked{};
    CHECK(compressed->LockRect(0, &locked, nullptr, 0));
    expect(locked.Pitch == 8, "a DXT1 level's pitch is not its one block's eight bytes");
    memcpy(locked.pBits, block, sizeof(block));
    CHECK(compressed->UnlockRect(0));
    CHECK(device->SetTexture(0, compressed.Get()));
    hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
    expect(SUCCEEDED(hr), "a draw sampling a DXT1 texture was refused");
    printf("probe: dxt1 8,30=%06lx 56,30=%06lx\n", (unsigned long)pixel(8, 30), (unsigned long)pixel(56, 30));
    expect(pixel(8, 30) == kColumnPixels[0] && pixel(56, 30) == kColumnPixels[0],
           "a DXT1 block's colour0 is not what the draw sampled");
    // and a compressed texture of a size that is not whole blocks is refused, as D3D9 refuses it
    ComPtr<IDirect3DTexture9> refused;
    expect(device->CreateTexture(6, 6, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &refused, nullptr) == D3DERR_INVALIDCALL,
           "a DXT1 texture of a size that is not whole blocks was accepted");
  }

  // ---- a stage with no texture samples the default texture: one opaque white texel ---------------------------------
  CHECK(device->SetTexture(0, nullptr));
  hr = draw_quad(0.0f, 0.0f, 1.0f, 1.0f);
  expect(SUCCEEDED(hr), "a draw that samples an empty stage was refused");
  printf("probe: default 32,32=%06lx\n", (unsigned long)pixel(32, 32));
  expect(pixel(32, 32) == kColumnPixels[3], "a stage with no texture did not sample the default white texel");

  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(nullptr));
  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
