// contract: a Direct3D 9 shader is made from the d3dbc program it is given, and the device hands it back as it was
// given, whatever the stage. Microsoft Learn, IDirect3DDevice9::CreateVertexShader: "Creates a vertex shader";
// IDirect3DDevice9::CreatePixelShader: "Creates a pixel shader"; IDirect3DVertexShader9::GetFunction: "Gets a pointer
// to the shader function"; IDirect3DDevice9::SetVertexShader: "Sets the current vertex shader"; Set*ShaderConstantF:
// "Sets a floating-point vertex shader constant". A program of the other stage, one whose version this build does not
// read, one with no END, and a null pointer are D3DERR_INVALIDCALL, and a constant outside its table is too. A draw
// with a game's shader is refused, with the code D3D9 names for it, until the translator that turns d3dbc into the
// DXBC this build's D3D11 device compiles lands -- and clearing the shader draws the fixed-function frame again.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-createvertexshader
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-createpixelshader
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dvertexshader9-getfunction
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setvertexshaderconstantf
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

// the d3dbc program Microsoft's compiler makes of an HLSL shader of `profile` ("vs_1_1", "ps_2_0", ...)
ComPtr<ID3DBlob>
compile(const char *hlsl, const char *entry, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, profile, 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", entry, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

const char *kVertexShader = "float4 main(float4 position : POSITION) : POSITION { return position; }";
const char *kPixelShader = "float4 main(float4 colour : COLOR0) : COLOR { return colour; }";

} // namespace

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  const UINT client = 64;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_shader"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_shader", "d3d9_shader", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  present.BackBufferWidth = client;
  present.BackBufferHeight = client;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.MultiSampleType = D3DMULTISAMPLE_NONE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  ComPtr<ID3DBlob> vertex_code = compile(kVertexShader, "main", "vs_1_1");
  ComPtr<ID3DBlob> pixel_code = compile(kPixelShader, "main", "ps_2_0");
  if (!vertex_code || !pixel_code) {
    printf("failed: Microsoft's compiler made no shader\n");
    return 1;
  }

  // a program of another stage, one with no END, and no program at all are refused, and no shader is handed back
  ComPtr<IDirect3DVertexShader9> vertex_shader;
  CHECK(device->CreateVertexShader((const DWORD *)vertex_code->GetBufferPointer(), &vertex_shader));
  expect(vertex_shader != nullptr, "a vs_1_1 program made no vertex shader");
  ComPtr<IDirect3DVertexShader9> refused;
  expect(device->CreateVertexShader((const DWORD *)pixel_code->GetBufferPointer(), &refused) == D3DERR_INVALIDCALL,
         "a pixel program was made into a vertex shader");
  expect(refused == nullptr, "a refused vertex shader was handed back");
  expect(device->CreateVertexShader(nullptr, &refused) == D3DERR_INVALIDCALL, "null bytecode was made a shader");
  std::vector<DWORD> wrong_version(8, 0);
  wrong_version[0] = 0xFFFE0400;
  wrong_version[7] = 0x0000FFFF;
  expect(device->CreateVertexShader(wrong_version.data(), &refused) == D3DERR_INVALIDCALL,
         "a 4.0 program was made a vertex shader");

  // the shader hands back the program it was made from, and knows its device
  UINT size = 0;
  CHECK(vertex_shader->GetFunction(nullptr, &size));
  expect(size == vertex_code->GetBufferSize(), "GetFunction's size is not the program's");
  std::vector<BYTE> copy(size);
  CHECK(vertex_shader->GetFunction(copy.data(), &size));
  expect(!memcmp(copy.data(), vertex_code->GetBufferPointer(), vertex_code->GetBufferSize()),
         "GetFunction's bytes are not the program's");
  ComPtr<IDirect3DDevice9> owner;
  CHECK(vertex_shader->GetDevice(&owner));
  expect(owner.Get() == device.Get(), "GetDevice is not the device that made the shader");

  ComPtr<IDirect3DPixelShader9> pixel_shader;
  CHECK(device->CreatePixelShader((const DWORD *)pixel_code->GetBufferPointer(), &pixel_shader));
  ComPtr<IDirect3DPixelShader9> refused_pixel;
  expect(device->CreatePixelShader((const DWORD *)vertex_code->GetBufferPointer(), &refused_pixel) ==
             D3DERR_INVALIDCALL,
         "a vertex program was made into a pixel shader");

  // what is set is what is read back, and the null shader is the fixed-function one
  CHECK(device->SetVertexShader(vertex_shader.Get()));
  ComPtr<IDirect3DVertexShader9> read;
  CHECK(device->GetVertexShader(&read));
  expect(read.Get() == vertex_shader.Get(), "the vertex shader read back is not the one set");
  CHECK(device->SetPixelShader(pixel_shader.Get()));
  ComPtr<IDirect3DPixelShader9> read_pixel;
  CHECK(device->GetPixelShader(&read_pixel));
  expect(read_pixel.Get() == pixel_shader.Get(), "the pixel shader read back is not the one set");
  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(nullptr));
  CHECK(device->GetVertexShader(&read));
  CHECK(device->GetPixelShader(&read_pixel));
  expect(read == nullptr && read_pixel == nullptr, "the null shader did not clear the stage");

  // the constant tables: what is set is what is read, and a register outside the table is refused
  const float constants[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  float read_back[8] = {};
  CHECK(device->SetVertexShaderConstantF(3, constants, 2));
  CHECK(device->GetVertexShaderConstantF(3, read_back, 2));
  expect(!memcmp(read_back, constants, sizeof(constants)), "the vertex constants read back are not the ones set");
  expect(device->SetVertexShaderConstantF(255, constants, 2) == D3DERR_INVALIDCALL,
         "a vertex constant past the table was accepted");
  const int ints[4] = {1, -2, 3, -4};
  int read_ints[4] = {};
  CHECK(device->SetVertexShaderConstantI(15, ints, 1));
  CHECK(device->GetVertexShaderConstantI(15, read_ints, 1));
  expect(!memcmp(read_ints, ints, sizeof(ints)), "the integer constants read back are not the ones set");
  expect(device->SetVertexShaderConstantI(16, ints, 1) == D3DERR_INVALIDCALL,
         "an integer constant past the table was accepted");
  const WINBOOL bools[2] = {TRUE, FALSE};
  WINBOOL read_bools[2] = {};
  CHECK(device->SetVertexShaderConstantB(14, bools, 2));
  CHECK(device->GetVertexShaderConstantB(14, read_bools, 2));
  expect(!memcmp(read_bools, bools, sizeof(bools)), "the boolean constants read back are not the ones set");
  expect(device->SetPixelShaderConstantF(0, constants, 1) == S_OK, "a pixel constant was refused");
  expect(device->SetPixelShaderConstantF(256, constants, 1) == D3DERR_INVALIDCALL,
         "a pixel constant past the table was accepted");

  // a draw with a game's shader is refused until the translator lands, and the fixed-function frame draws again
  // once the shader is cleared
  struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR colour;
  };
  const Vertex vertices[3] = {{4.0f, 4.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 255)},
                              {60.0f, 4.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 255)},
                              {4.0f, 60.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 255)}};
  ComPtr<IDirect3DVertexBuffer9> buffer;
  CHECK(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                   D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &buffer, nullptr));
  void *data = nullptr;
  CHECK(buffer->Lock(0, 0, &data, D3DLOCK_DISCARD));
  memcpy(data, vertices, sizeof(vertices));
  CHECK(buffer->Unlock());
  CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
  CHECK(device->SetStreamSource(0, buffer.Get(), 0, sizeof(Vertex)));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 20, 30), 1.0f, 0));
  CHECK(device->BeginScene());
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == S_OK, "the fixed-function draw was refused");
  CHECK(device->SetVertexShader(vertex_shader.Get()));
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == D3DERR_INVALIDCALL,
         "a draw with a game's shader was not refused");
  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(pixel_shader.Get()));
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == D3DERR_INVALIDCALL,
         "a draw with a game's pixel shader was not refused");
  CHECK(device->SetPixelShader(nullptr));
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == S_OK,
         "the fixed-function draw after clearing the shaders was refused");
  CHECK(device->EndScene());

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
