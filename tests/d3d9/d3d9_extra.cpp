// contract: additional windowed DISCARD swap chains, four vertex streams, fog state stored without being advertised,
// and mixed FF+programmable draws. Microsoft Learn, IDirect3DDevice9::CreateAdditionalSwapChain, SetStreamSource,
// D3DRS_FOGENABLE. a fullscreen Reset is accepted. MaxTextureBlendStages is two once extra FF bytecode exists.
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

const char *kVertexHlsl = "struct VSOut { float4 position : POSITION; float4 colour : COLOR0; };\n"
                          "VSOut main(float4 position : POSITION, float4 colour : COLOR0) {\n"
                          "  VSOut o; o.position = position; o.colour = colour; return o;\n"
                          "}\n";

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

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_extra"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_extra", "d3d9_extra", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64, nullptr,
                              nullptr, wc.hInstance, nullptr);
  HWND extra_window = CreateWindowA("d3d9_extra", "d3d9_extra2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 0, 64, 64,
                                    nullptr, nullptr, wc.hInstance, nullptr);
  if (!window || !extra_window) {
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
  present.BackBufferWidth = 64;
  present.BackBufferHeight = 64;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  D3DCAPS9 caps{};
  CHECK(device->GetDeviceCaps(&caps));
  expect(caps.MaxStreams == 4, "MaxStreams is not 4");
  expect((caps.RasterCaps & D3DPRASTERCAPS_FOGTABLE) != 0, "RasterCaps does not advertise the linear table fog it applies");
  expect(!(caps.RasterCaps & (D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGRANGE)),
         "RasterCaps advertised vertex or range fog this build does not apply");
  expect(caps.MaxTextureBlendStages == 2, "MaxTextureBlendStages is not the two fixed-function stages");
  CHECK(device->SetRenderState(D3DRS_FOGENABLE, TRUE));
  CHECK(device->SetRenderState(D3DRS_FOGCOLOR, D3DCOLOR_XRGB(255, 255, 255)));
  DWORD fog = 0;
  CHECK(device->GetRenderState(D3DRS_FOGENABLE, &fog));
  expect(fog == TRUE, "FOGENABLE was not stored");

  D3DPRESENT_PARAMETERS extra_present = present;
  extra_present.hDeviceWindow = extra_window;
  ComPtr<IDirect3DSwapChain9> extra;
  CHECK(device->CreateAdditionalSwapChain(&extra_present, &extra));
  ComPtr<IDirect3DSurface9> extra_back;
  CHECK(extra->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &extra_back));
  CHECK(device->ColorFill(extra_back.Get(), nullptr, D3DCOLOR_XRGB(0, 255, 0)));
  expect(SUCCEEDED(extra->Present(nullptr, nullptr, nullptr, nullptr, 0)), "the additional chain did not present");

  D3DPRESENT_PARAMETERS fullscreen = present;
  fullscreen.Windowed = FALSE;
  CHECK(device->Reset(&fullscreen));
  CHECK(device->Reset(&present));

  ComPtr<IDirect3DVertexBuffer9> positions, colours;
  CHECK(device->CreateVertexBuffer(3 * 12, 0, 0, D3DPOOL_MANAGED, &positions, nullptr));
  CHECK(device->CreateVertexBuffer(3 * 4, 0, 0, D3DPOOL_MANAGED, &colours, nullptr));
  void *bytes = nullptr;
  CHECK(positions->Lock(0, 0, &bytes, 0));
  const float pos[9] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
  memcpy(bytes, pos, sizeof(pos));
  CHECK(positions->Unlock());
  CHECK(colours->Lock(0, 0, &bytes, 0));
  const D3DCOLOR cyan[3] = {D3DCOLOR_XRGB(0, 255, 255), D3DCOLOR_XRGB(0, 255, 255), D3DCOLOR_XRGB(0, 255, 255)};
  memcpy(bytes, cyan, sizeof(cyan));
  CHECK(colours->Unlock());
  CHECK(device->SetStreamSource(0, positions.Get(), 0, 12));
  CHECK(device->SetStreamSource(1, colours.Get(), 0, 4));
  IDirect3DVertexBuffer9 *read_colours = nullptr;
  UINT offset = 0, stride = 0;
  CHECK(device->GetStreamSource(1, &read_colours, &offset, &stride));
  expect(read_colours == colours.Get() && stride == 4, "GetStreamSource(1) is not the colour stream");
  if (read_colours)
    read_colours->Release();

  ComPtr<ID3DBlob> vs = compile(kVertexHlsl, "vs_2_0");
  expect(vs != nullptr, "Microsoft's compiler made no vertex shader");
  if (vs) {
    ComPtr<IDirect3DVertexShader9> vertex;
    CHECK(device->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &vertex));
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    CHECK(device->CreateVertexDeclaration(elements, &declaration));
    CHECK(device->SetVertexDeclaration(declaration.Get()));
    CHECK(device->SetVertexShader(vertex.Get()));
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
    CHECK(device->BeginScene());
    expect(SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)),
           "a programmable VS with a fixed-function PS was refused");
    CHECK(device->EndScene());
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
