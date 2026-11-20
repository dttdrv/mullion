// contract: the topologies, fill modes and counters Direct3D 9 accepts are drawn, not refused, and the caps that name
// how much a draw reaches are what the device then accepts.
// - D3DPT_TRIANGLEFAN has no Direct3D 11 primitive; the device expands it into the triangle list a fan is, for
//   DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP and DrawIndexedPrimitiveUP (Microsoft Learn,
//   D3DPRIMITIVETYPE). The fan drawn here is a square of four triangles around a centre vertex, and the four calls
//   cover the same pixels.
// - D3DFILL_POINT draws a primitive's vertices as single pixels: a solid triangle covers its interior, and the same
//   triangle point-filled does not, while its vertices are still marked (Microsoft Learn, D3DFILLMODE; D3DCAPS9
//   MaxPointSize is 1, and a point is one pixel).
// - NumSimultaneousRTs is four: a ps_3_0 program's oC0 and oC1 land in the first two targets SetRenderTarget set
//   (Microsoft Learn, D3DCAPS9::NumSimultaneousRTs and IDirect3DDevice9::SetRenderTarget).
// - an additional chain is the second one GetSwapChain indexes, with its own back buffer, display mode, raster status
//   and vertical blank (Microsoft Learn, IDirect3DDevice9::GetSwapChain and GetNumberOfSwapChains).
// - CheckResourceResidency answers for this device's resources, and refuses a set it cannot answer for (Microsoft
//   Learn, IDirect3DDevice9Ex::CheckResourceResidency).
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
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

const UINT kClient = 128;

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

const DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

D3DCOLOR
texel(const D3DLOCKED_RECT &locked, UINT x, UINT y) {
  const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
  return row[x] & 0x00FFFFFF;
}

// the vs_2_0 the shader draw uses: the position straight through
const DWORD kVertex20[] = {
    0xFFFE0200, // vs_2_0
    0x0200001F, 0x00000000, 0x900F0000, // dcl_position v0
    0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
    0x0000FFFF, // end
};

// ps_3_0 writing four targets: oC0 red, oC1 green, oC2 blue, oC3 yellow
const DWORD kPixel30FourTargets[] = {
    0xFFFF0300, // ps_3_0
    0x05000051, 0xA00F0000, 0x3F800000, 0x00000000, 0x00000000, 0x3F800000, // def c0, 1, 0, 0, 1
    0x05000051, 0xA00F0001, 0x00000000, 0x3F800000, 0x00000000, 0x3F800000, // def c1, 0, 1, 0, 1
    0x05000051, 0xA00F0002, 0x00000000, 0x00000000, 0x3F800000, 0x3F800000, // def c2, 0, 0, 1, 1
    0x05000051, 0xA00F0003, 0x3F800000, 0x3F800000, 0x00000000, 0x3F800000, // def c3, 1, 1, 0, 1
    0x02000001, 0x800F0800, 0xA0E40000, // mov oC0, c0
    0x02000001, 0x800F0801, 0xA0E40001, // mov oC1, c1
    0x02000001, 0x800F0802, 0xA0E40002, // mov oC2, c2
    0x02000001, 0x800F0803, 0xA0E40003, // mov oC3, c3
    0x0000FFFF, // end
};

} // namespace

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_topology"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_topology", "d3d9_topology", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, nullptr,
                              nullptr, window_class.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }
  // an additional chain is a chain of its own window: DXGI gives a window one flip-model chain
  HWND extra_window = CreateWindowA("d3d9_topology", "d3d9_topology extra", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                                    window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
                                    nullptr, nullptr, window_class.hInstance, nullptr);
  if (!extra_window) {
    printf("failed: no second window\n");
    return 1;
  }

  // WaitForVBlank and CheckResourceResidency are IDirect3DDevice9Ex methods, so this test creates the extended
  // device through Direct3DCreate9Ex. A regular Direct3DCreate9 device correctly rejects the Ex IID.
  ComPtr<IDirect3D9Ex> d3d9;
  if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9))) {
    printf("failed: Direct3DCreate9Ex made no object\n");
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
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  // WaitForVBlank and CheckResourceResidency are the extended device's
  ComPtr<IDirect3DDevice9Ex> device_ex;
  CHECK(device.As(&device_ex));

  const D3DCOLOR clear = D3DCOLOR_XRGB(16, 16, 48);
  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback,
                                            nullptr));

  std::vector<D3DCOLOR> pixels;
  auto read_back = [&](IDirect3DSurface9 *target) -> bool {
    if (FAILED(device->GetRenderTargetData(target, readback.Get())))
      return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return false;
    pixels.assign((size_t)kClient * kClient, 0);
    for (UINT y = 0; y < kClient; y++)
      for (UINT x = 0; x < kClient; x++)
        pixels[(size_t)y * kClient + x] = texel(locked, x, y);
    readback->UnlockRect();
    return true;
  };
  auto at = [&](UINT x, UINT y) { return pixels[(size_t)y * kClient + x]; };
  auto count_coloured = [&](D3DCOLOR colour) {
    return (UINT)std::count(pixels.begin(), pixels.end(), colour & 0x00FFFFFF);
  };

  // the fan: a centre vertex and the square's corners, the last one closing the square (four primitives, six vertices)
  const D3DCOLOR fan_colour = D3DCOLOR_XRGB(255, 0, 0);
  const Vertex fan[6] = {
      {64.0f, 64.0f, 0.0f, 1.0f, fan_colour}, {16.0f, 16.0f, 0.0f, 1.0f, fan_colour},
      {112.0f, 16.0f, 0.0f, 1.0f, fan_colour}, {112.0f, 112.0f, 0.0f, 1.0f, fan_colour},
      {16.0f, 112.0f, 0.0f, 1.0f, fan_colour}, {16.0f, 16.0f, 0.0f, 1.0f, fan_colour},
  };
  const WORD fan_indices[6] = {0, 1, 2, 3, 4, 5};
  // one pixel inside each of the four triangles, and the centre
  const UINT inside[5][2] = {{64, 20}, {108, 64}, {64, 108}, {20, 64}, {64, 64}};

  ComPtr<IDirect3DVertexBuffer9> vertex_buffer;
  CHECK(device->CreateVertexBuffer(sizeof(fan), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
                                   &vertex_buffer, nullptr));
  void *data = nullptr;
  CHECK(vertex_buffer->Lock(0, 0, &data, D3DLOCK_DISCARD));
  memcpy(data, fan, sizeof(fan));
  CHECK(vertex_buffer->Unlock());
  ComPtr<IDirect3DIndexBuffer9> index_buffer;
  CHECK(device->CreateIndexBuffer(sizeof(fan_indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                  &index_buffer, nullptr));
  void *index_data = nullptr;
  CHECK(index_buffer->Lock(0, 0, &index_data, 0));
  memcpy(index_data, fan_indices, sizeof(fan_indices));
  CHECK(index_buffer->Unlock());
  CHECK(device->SetFVF(kFVF));

  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));

  // the fan through each of the four draws that take a topology: every one covers the square's four triangles
  auto fan_frame = [&](const char *what, int path) {
    expect(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0)), "a fan frame did not clear");
    expect(SUCCEEDED(device->BeginScene()), "a fan frame did not begin");
    switch (path) {
    case 0:
      expect(SUCCEEDED(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(Vertex))), "a fan stream");
      expect(SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLEFAN, 0, 4)), "DrawPrimitive refused a fan");
      break;
    case 1:
      expect(SUCCEEDED(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(Vertex))), "a fan stream");
      expect(SUCCEEDED(device->SetIndices(index_buffer.Get())), "a fan index buffer");
      expect(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLEFAN, 0, 0, 6, 0, 4)),
             "DrawIndexedPrimitive refused a fan");
      break;
    case 2:
      expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 4, fan, sizeof(Vertex))),
             "DrawPrimitiveUP refused a fan");
      break;
    default:
      expect(SUCCEEDED(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLEFAN, 0, 6, 4, fan_indices, D3DFMT_INDEX16, fan,
                                                      sizeof(Vertex))),
             "DrawIndexedPrimitiveUP refused a fan");
      break;
    }
    expect(SUCCEEDED(device->EndScene()), "a fan frame did not end");
    const bool read = read_back(back_buffer.Get());
    expect(read, what);
    if (read) {
      bool covered = true;
      for (const auto &point : inside)
        covered &= at(point[0], point[1]) == (fan_colour & 0x00FFFFFF);
      if (!covered)
        printf("%s: inside reads %06lx %06lx %06lx %06lx %06lx, wanted %06lx\n", what, (unsigned long)at(64, 20),
               (unsigned long)at(108, 64), (unsigned long)at(64, 108), (unsigned long)at(20, 64),
               (unsigned long)at(64, 64), (unsigned long)(fan_colour & 0x00FFFFFF));
      expect(covered, what);
      expect(at(4, 4) == (clear & 0x00FFFFFF), "a fan draw wrote outside its triangles");
      printf("probe: %s covers %u pixels\n", what, count_coloured(fan_colour));
    }
  };
  fan_frame("DrawPrimitive fan", 0);
  fan_frame("DrawIndexedPrimitive fan", 1);
  fan_frame("DrawPrimitiveUP fan", 2);
  fan_frame("DrawIndexedPrimitiveUP fan", 3);

  // point fill: the same triangle solid covers its interior, and point-filled marks its vertices only
  const D3DCOLOR point_colour = D3DCOLOR_XRGB(0, 255, 0);
  const Vertex triangle[3] = {
      {16.0f, 16.0f, 0.0f, 1.0f, point_colour},
      {16.0f, 112.0f, 0.0f, 1.0f, point_colour},
      {112.0f, 16.0f, 0.0f, 1.0f, point_colour},
  };
  CHECK(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
  expect(read_back(back_buffer.Get()) && at(40, 40) == (point_colour & 0x00FFFFFF),
         "a solid triangle does not fill its interior");

  CHECK(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_POINT));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
  if (read_back(back_buffer.Get())) {
    const UINT marked = count_coloured(point_colour);
    printf("probe: point fill marks %u pixels, and the interior is %06lx\n", marked, (unsigned long)at(40, 40));
    expect(at(40, 40) == (clear & 0x00FFFFFF), "point fill filled the triangle's interior");
    expect(marked >= 1 && marked <= 12, "point fill did not mark the triangle's vertices");
  } else {
    expect(false, "point fill readback");
  }
  CHECK(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));

  // four targets: ps_3_0 writes oC0 through oC3 to the four slots SetRenderTarget set, read independently
  const UINT mrt_size = 64;
  ComPtr<IDirect3DSurface9> target0, target1, target2, target3;
  CHECK(device->CreateRenderTarget(mrt_size, mrt_size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target0,
                                   nullptr));
  CHECK(device->CreateRenderTarget(mrt_size, mrt_size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target1,
                                   nullptr));
  CHECK(device->CreateRenderTarget(mrt_size, mrt_size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target2,
                                   nullptr));
  CHECK(device->CreateRenderTarget(mrt_size, mrt_size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target3,
                                   nullptr));
  CHECK(device->SetRenderTarget(0, target0.Get()));
  CHECK(device->SetRenderTarget(1, target1.Get()));
  CHECK(device->SetRenderTarget(2, target2.Get()));
  CHECK(device->SetRenderTarget(3, target3.Get()));
  ComPtr<IDirect3DSurface9> second;
  expect(SUCCEEDED(device->GetRenderTarget(1, &second)) && second.Get() == target1.Get(),
         "the second render target is not the one SetRenderTarget set");

  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  ComPtr<IDirect3DVertexShader9> vertex_shader;
  ComPtr<IDirect3DPixelShader9> pixel_shader;
  CHECK(device->CreateVertexShader(kVertex20, &vertex_shader));
  CHECK(device->CreatePixelShader(kPixel30FourTargets, &pixel_shader));
  CHECK(device->SetVertexShader(vertex_shader.Get()));
  CHECK(device->SetPixelShader(pixel_shader.Get()));
  // a full-screen triangle in clip space
  const float big[3][3] = {{-1.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}, {3.0f, -1.0f, 0.0f}};
  ComPtr<IDirect3DVertexBuffer9> big_buffer;
  CHECK(device->CreateVertexBuffer(sizeof(big), 0, 0, D3DPOOL_DEFAULT, &big_buffer, nullptr));
  CHECK(big_buffer->Lock(0, 0, &data, 0));
  memcpy(data, big, sizeof(big));
  CHECK(big_buffer->Unlock());
  CHECK(device->SetStreamSource(0, big_buffer.Get(), 0, sizeof(big[0])));
  CHECK(device->BeginScene());
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
  CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  CHECK(device->EndScene());

  ComPtr<IDirect3DSurface9> mrt_readback;
  CHECK(device->CreateOffscreenPlainSurface(mrt_size, mrt_size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &mrt_readback,
                                            nullptr));
  auto target_texel = [&](IDirect3DSurface9 *target, UINT x, UINT y) -> D3DCOLOR {
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(target, mrt_readback.Get())) ||
        FAILED(mrt_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0xFFFFFFFF;
    const D3DCOLOR value = texel(locked, x, y);
    mrt_readback->UnlockRect();
    return value;
  };
  const D3DCOLOR first_colour = target_texel(target0.Get(), mrt_size / 2, mrt_size / 2);
  const D3DCOLOR second_colour = target_texel(target1.Get(), mrt_size / 2, mrt_size / 2);
  const D3DCOLOR third_colour = target_texel(target2.Get(), mrt_size / 2, mrt_size / 2);
  const D3DCOLOR fourth_colour = target_texel(target3.Get(), mrt_size / 2, mrt_size / 2);
  printf("probe: oC0..oC3 read %06lx %06lx %06lx %06lx\n", (unsigned long)first_colour,
         (unsigned long)second_colour, (unsigned long)third_colour, (unsigned long)fourth_colour);
  expect(first_colour == 0x00FF0000, "oC0 did not land in the first render target");
  expect(second_colour == 0x0000FF00, "oC1 did not land in the second render target");
  expect(third_colour == 0x000000FF, "oC2 did not land in the third render target");
  expect(fourth_colour == 0x00FFFF00, "oC3 did not land in the fourth render target");

  // per-target colour write masks: D3DRS_COLORWRITEENABLE is target 0's, and 1..3 are the other targets'. Two passes
  // with the same pixel shader but different masks prove the mask reaches each Metal attachment, including NONE, ALL,
  // one channel and two channels, and that a warm pipeline cache does not keep the first pass's mask
  {
    const DWORD all = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
                      D3DCOLORWRITEENABLE_ALPHA;
    const D3DRENDERSTATETYPE mask_states[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
                                               D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};
    IDirect3DSurface9 *mask_targets[4] = {target0.Get(), target1.Get(), target2.Get(), target3.Get()};
    const DWORD pass_a[4] = {D3DCOLORWRITEENABLE_RED, D3DCOLORWRITEENABLE_GREEN, D3DCOLORWRITEENABLE_BLUE, 0};
    const D3DCOLOR pass_a_expected[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000};
    const DWORD pass_b[4] = {D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, 0,
                             D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, all};
    const D3DCOLOR pass_b_expected[4] = {0x00FF0000, 0x00000000, 0x000000FF, 0x00FFFF00};

    // each target is cleared through slot 0, because D3D9's Clear names one target
    auto clear_targets = [&]() -> bool {
      for (UINT at = 0; at < 4; at++) {
        if (FAILED(device->SetRenderTarget(0, mask_targets[at])) ||
            FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0)))
          return false;
      }
      return SUCCEEDED(device->SetRenderTarget(0, target0.Get())) &&
             SUCCEEDED(device->SetRenderTarget(1, target1.Get())) &&
             SUCCEEDED(device->SetRenderTarget(2, target2.Get())) &&
             SUCCEEDED(device->SetRenderTarget(3, target3.Get()));
    };
    auto mask_pass = [&](const char *what, const DWORD masks[4], const D3DCOLOR expected[4]) -> bool {
      for (UINT at = 0; at < 4; at++) {
        if (FAILED(device->SetRenderState(mask_states[at], masks[at]))) {
          printf("%s: SetRenderState refused mask %lu for target %u\n", what, (unsigned long)masks[at], at);
          return false;
        }
        DWORD got = 0;
        if (FAILED(device->GetRenderState(mask_states[at], &got)) || got != masks[at]) {
          printf("%s: GetRenderState answered %lu for target %u's mask %lu\n", what, (unsigned long)got, at,
                 (unsigned long)masks[at]);
          return false;
        }
      }
      if (!clear_targets()) {
        printf("%s: the targets were not cleared\n", what);
        return false;
      }
      if (FAILED(device->BeginScene()) || FAILED(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)) ||
          FAILED(device->EndScene())) {
        printf("%s: the mask draw was refused\n", what);
        return false;
      }
      bool matched = true;
      D3DCOLOR read[4] = {};
      for (UINT at = 0; at < 4; at++) {
        read[at] = target_texel(mask_targets[at], mrt_size / 2, mrt_size / 2);
        if (read[at] != expected[at]) {
          printf("%s: target %u reads %06lx, wanted %06lx\n", what, at, (unsigned long)read[at],
                 (unsigned long)expected[at]);
          matched = false;
        }
      }
      printf("%s: masks read %06lx %06lx %06lx %06lx\n", what, (unsigned long)read[0], (unsigned long)read[1],
             (unsigned long)read[2], (unsigned long)read[3]);
      return matched;
    };

    expect(mask_pass("mask pass A", pass_a, pass_a_expected), "the first per-target mask set did not reach its targets");
    expect(mask_pass("mask pass B", pass_b, pass_b_expected), "a changed per-target mask set did not reach its targets");

    // the masks are render states a state block captures and restores, like D3DRS_COLORWRITEENABLE
    ComPtr<IDirect3DStateBlock9> mask_block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &mask_block));
    for (UINT at = 0; at < 4; at++)
      CHECK(device->SetRenderState(mask_states[at], 0));
    CHECK(mask_block->Apply());
    for (UINT at = 0; at < 4; at++) {
      DWORD got = 0;
      expect(SUCCEEDED(device->GetRenderState(mask_states[at], &got)) && got == pass_b[at],
             "a state block did not restore a per-target colour write mask");
    }
    for (UINT at = 0; at < 4; at++)
      CHECK(device->SetRenderState(mask_states[at], all));
  }

  // the slots are independent: clearing slot 1 answers D3DERR_NOTFOUND for it and leaves slots 2 and 3 bound, and a
  // slot can be set again while the one below it is empty
  CHECK(device->SetRenderTarget(1, nullptr));
  expect(device->GetRenderTarget(1, &second) == D3DERR_NOTFOUND, "a cleared render target was handed back");
  second.Reset();
  CHECK(device->GetRenderTarget(2, &second));
  expect(second.Get() == target2.Get(), "clearing slot 1 cleared slot 2");
  second.Reset();
  CHECK(device->SetRenderTarget(3, target3.Get()));
  CHECK(device->GetRenderTarget(3, &second));
  expect(second.Get() == target3.Get(), "slot 3 could not be set with slot 1 empty");
  second.Reset();
  CHECK(device->SetRenderTarget(2, nullptr));
  CHECK(device->SetRenderTarget(3, nullptr));
  CHECK(device->SetVertexShader(nullptr));
  CHECK(device->SetPixelShader(nullptr));
  CHECK(device->SetRenderTarget(0, back_buffer.Get()));
  CHECK(device->SetFVF(kFVF));

  // the additional chain: the second one the device reports, with a back buffer, a mode, a raster status and a blank
  D3DPRESENT_PARAMETERS extra = present;
  extra.BackBufferWidth = 64;
  extra.BackBufferHeight = 64;
  extra.hDeviceWindow = extra_window;
  ComPtr<IDirect3DSwapChain9> additional, indexed;
  CHECK(device->CreateAdditionalSwapChain(&extra, &additional));
  // the additional chain is not one of the device's implicit chains: GetNumberOfSwapChains counts only the implicit
  // one, GetSwapChain does not hand the additional chain back, and neither does the device's GetBackBuffer. Those are
  // the chain's own calls (Microsoft Learn, IDirect3DDevice9::GetSwapChain; Wine's device test asks for index 1 after
  // CreateAdditionalSwapChain and takes D3DERR_INVALIDCALL with a NULL result)
  expect(device->GetNumberOfSwapChains() == 1, "the device counted a chain that is not its implicit one");
  CHECK(device->GetSwapChain(0, &indexed));
  expect(indexed.Get() != nullptr, "GetSwapChain(0) handed back nothing");
  indexed.Reset();
  expect(device->GetSwapChain(1, &indexed) == D3DERR_INVALIDCALL, "an additional chain was handed back as implicit");
  expect(indexed.Get() == nullptr, "a failed GetSwapChain left a chain behind");
  ComPtr<IDirect3DSurface9> extra_back;
  expect(device->GetBackBuffer(1, 0, D3DBACKBUFFER_TYPE_MONO, &extra_back) == D3DERR_INVALIDCALL,
         "the device handed out an additional chain's back buffer");
  expect(extra_back.Get() == nullptr, "a failed GetBackBuffer left a surface behind");
  CHECK(additional->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &extra_back));
  expect(extra_back.Get() != nullptr, "the additional chain has no back buffer");
  extra_back.Reset();
  D3DDISPLAYMODE mode{};
  expect(SUCCEEDED(device->GetDisplayMode(1, &mode)) && mode.Width && mode.Height,
         "the additional chain has no display mode");
  // the scan line is the display's, so it is not bounded by the chain's 64 pixels
  D3DRASTER_STATUS raster{};
  expect(SUCCEEDED(device->GetRasterStatus(1, &raster)) && (raster.InVBlank == TRUE || raster.InVBlank == FALSE),
         "the additional chain has no raster status");
  expect(device->GetRasterStatus(2, &raster) == D3DERR_INVALIDCALL, "a raster status of a chain that is not there");
  expect(device_ex->WaitForVBlank(1) == S_OK, "WaitForVBlank refused the additional chain");
  expect(device_ex->WaitForVBlank(2) == D3DERR_INVALIDCALL, "WaitForVBlank took a chain that is not there");
  printf("probe: chains %u, mode %ux%u, scan line %u\n", device->GetNumberOfSwapChains(), mode.Width, mode.Height,
         raster.ScanLine);

  // residency: this device's resource is resident, and a set it cannot answer for is refused
  IDirect3DResource9 *resident = vertex_buffer.Get();
  expect(device_ex->CheckResourceResidency(&resident, 1) == S_OK, "a resource of this device is not resident");
  expect(device_ex->CheckResourceResidency(nullptr, 0) == S_OK, "an empty residency set is refused");
  expect(device_ex->CheckResourceResidency(nullptr, 1) == D3DERR_INVALIDCALL, "an empty entry was answered for");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(extra_window);
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
