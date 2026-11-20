// contract: a triangle fan keeps its first vertex as the centre of every triangle, through every draw entry point and
// every index encoding. The fan drawn here has a nonidentity index list, a different colour at each corner, and a
// centre that is not vertex 0, so a conversion that advances the first index with the others draws different colours
// in the middle and a different area.
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dprimitivetype
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
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

// the ring: top-left green, top-right blue, bottom-right white, bottom-left yellow, top-left again
const D3DCOLOR kGreen = D3DCOLOR_XRGB(0, 255, 0);
const D3DCOLOR kBlue = D3DCOLOR_XRGB(0, 0, 255);
const D3DCOLOR kWhite = D3DCOLOR_XRGB(255, 255, 255);
const D3DCOLOR kYellow = D3DCOLOR_XRGB(255, 255, 0);
const D3DCOLOR kRed = D3DCOLOR_XRGB(255, 0, 0);

// vertex 5 is the fan's centre, the last vertex of the array, and the first index of every triangle is 5
const Vertex kVertices[6] = {
    {16.0f, 16.0f, 0.0f, 1.0f, kGreen},   {112.0f, 16.0f, 0.0f, 1.0f, kBlue},
    {112.0f, 112.0f, 0.0f, 1.0f, kWhite}, {16.0f, 112.0f, 0.0f, 1.0f, kYellow},
    {16.0f, 16.0f, 0.0f, 1.0f, kGreen},   {64.0f, 64.0f, 0.0f, 1.0f, kRed},
};
// (5,0,1), (5,1,2), (5,2,3), (5,3,4)
const WORD kIndices[6] = {5, 0, 1, 2, 3, 4};
// the nonindexed fan, whose first vertex is the centre
const Vertex kVerticesUp[6] = {
    {64.0f, 64.0f, 0.0f, 1.0f, kRed},     {16.0f, 16.0f, 0.0f, 1.0f, kGreen},
    {112.0f, 16.0f, 0.0f, 1.0f, kBlue},   {112.0f, 112.0f, 0.0f, 1.0f, kWhite},
    {16.0f, 112.0f, 0.0f, 1.0f, kYellow}, {16.0f, 16.0f, 0.0f, 1.0f, kGreen},
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
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_fan"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_fan", "d3d9_fan", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
  CHECK(device->SetFVF(kFVF));

  const D3DCOLOR clear = D3DCOLOR_XRGB(1, 2, 3);
  ComPtr<IDirect3DSurface9> readback, back_buffer;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));

  std::vector<D3DCOLOR> pixels;
  auto read_back = [&]() -> bool {
    if (FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())))
      return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return false;
    pixels.assign((size_t)kClient * kClient, 0);
    for (UINT y = 0; y < kClient; y++) {
      const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
      for (UINT x = 0; x < kClient; x++)
        pixels[(size_t)y * kClient + x] = row[x] & 0x00FFFFFF;
    }
    readback->UnlockRect();
    return true;
  };
  auto at = [&](UINT x, UINT y) { return pixels[(size_t)y * kClient + x]; };
  auto covered = [&]() {
    return (UINT)std::count_if(pixels.begin(), pixels.end(), [&](D3DCOLOR c) { return c != (clear & 0x00FFFFFF); });
  };

  // the centre is the fan's first vertex and reads its colour, not a blend of the vertices the sliding conversion
  // would have used there; each corner is dominated by its own vertex's colour
  auto frame = [&](const char *what, int path) -> int {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
    CHECK(device->BeginScene());
    switch (path) {
    case 0:
      CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 4, kVerticesUp, sizeof(Vertex)));
      break;
    case 1:
      CHECK(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLEFAN, 0, 6, 4, kIndices, D3DFMT_INDEX16, kVertices,
                                           sizeof(Vertex)));
      break;
    default:
      break;
    }
    CHECK(device->EndScene());
    if (!read_back()) {
      expect(false, what);
      return 0;
    }
    const D3DCOLOR centre = at(64, 64);
    printf("probe: %s centre %06lx area %u\n", what, (unsigned long)centre, covered());
    expect((centre & 0xFF0000) >= 0xF00000 && (centre & 0x00FF00) <= 0x0F0000 && (centre & 0x0000FF) <= 0x0F0000,
           "the fan's centre is not its first vertex's colour");
    // the quadrilateral between the ring's corners is what the fan covers
    expect(covered() >= 9000 && covered() <= 9400, "the fan did not cover the quadrilateral it spans");
    const D3DCOLOR tl = at(17, 17), tr = at(111, 17), br = at(111, 111), bl = at(17, 111);
    expect((tl & 0x0000FF00) >= 0x0000C000 && (tl & 0x00FF0000) <= 0x00300000, "the top-left corner is not green");
    expect((tr & 0x000000FF) >= 0xC0 && (tr & 0x00FF0000) <= 0x300000, "the top-right corner is not blue");
    expect((br & 0x00FF0000) >= 0xC00000 && (br & 0x000000FF) >= 0xC0 && (br & 0x0000FF00) >= 0x0000C000,
           "the bottom-right corner is not white");
    expect((bl & 0x00FF0000) >= 0xC00000 && (bl & 0x0000FF00) >= 0x0000C000 && (bl & 0x000000FF) <= 0x30,
           "the bottom-left corner is not yellow");
    return 0;
  };
  frame("DrawPrimitiveUP", 0);
  frame("DrawIndexedPrimitiveUP", 1);

  // the indexed forms: a 16-bit index buffer whose fan starts at index 2 and whose vertices start at vertex 3, so both
  // StartIndex and BaseVertexIndex are nonzero
  ComPtr<IDirect3DVertexBuffer9> vertex_buffer;
  CHECK(device->CreateVertexBuffer(sizeof(kVertices) + 3 * sizeof(Vertex), D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
                                   &vertex_buffer, nullptr));
  void *data = nullptr;
  CHECK(vertex_buffer->Lock(0, 0, &data, 0));
  memset(data, 0, 3 * sizeof(Vertex));
  memcpy((char *)data + 3 * sizeof(Vertex), kVertices, sizeof(kVertices));
  CHECK(vertex_buffer->Unlock());
  ComPtr<IDirect3DIndexBuffer9> index16;
  CHECK(device->CreateIndexBuffer(sizeof(WORD) * 8, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &index16,
                                  nullptr));
  CHECK(index16->Lock(0, 0, &data, 0));
  reinterpret_cast<WORD *>(data)[0] = 0xFFFF;
  reinterpret_cast<WORD *>(data)[1] = 0xFFFF;
  memcpy((char *)data + 2 * sizeof(WORD), kIndices, sizeof(kIndices));
  CHECK(index16->Unlock());
  CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(Vertex)));
  CHECK(device->SetIndices(index16.Get()));
  auto indexed_centre = [&](INT base_vertex, UINT start_index, UINT min_vertex, UINT vertex_count,
                            const char *what) -> int {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
    CHECK(device->BeginScene());
    const HRESULT hr =
        device->DrawIndexedPrimitive(D3DPT_TRIANGLEFAN, base_vertex, min_vertex, vertex_count, start_index, 4);
    CHECK(device->EndScene());
    expect(SUCCEEDED(hr), what);
    if (!SUCCEEDED(hr) || !read_back())
      return 0;
    const D3DCOLOR centre = at(64, 64);
    printf("probe: %s centre %06lx area %u\n", what, (unsigned long)centre, covered());
    expect((centre & 0xFF0000) >= 0xF00000 && (centre & 0x00FF00) <= 0x0F0000 && (centre & 0x0000FF) <= 0x0F0000,
           what);
    expect(covered() >= 9000 && covered() <= 9400, what);
    return 0;
  };
  indexed_centre(3, 2, 0, 6, "a fan with StartIndex and BaseVertexIndex is not the fan it indexes");

  // a 32-bit index buffer whose fan is past the 16-bit boundary: a reader that takes 16-bit indices from it finds
  // different vertices, and the centre is not red
  const UINT kFar = 65600;
  ComPtr<IDirect3DIndexBuffer9> index32;
  const UINT index32_bytes = (kFar + 8) * sizeof(DWORD);
  CHECK(device->CreateIndexBuffer(index32_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &index32,
                                  nullptr));
  CHECK(index32->Lock(0, 0, &data, 0));
  memset(data, 0, index32_bytes);
  DWORD *far_indices = reinterpret_cast<DWORD *>((char *)data + kFar * sizeof(DWORD));
  for (int i = 0; i < 6; i++)
    far_indices[i] = kIndices[i];
  CHECK(index32->Unlock());
  CHECK(device->SetIndices(index32.Get()));
  CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(Vertex)));
  indexed_centre(3, kFar, 0, 6, "a 32-bit fan past index 65535 is not read as 16-bit indices");

  // the same fan as vertices whose indices are past 65535: a 16-bit conversion cannot address them at all
  const UINT kHigh = 70000;
  const UINT high_vertices = kHigh + 6;
  std::vector<Vertex> high(high_vertices, Vertex{0.0f, 0.0f, 0.0f, 1.0f, 0});
  memcpy(&high[kHigh], kVertices, sizeof(kVertices));
  ComPtr<IDirect3DVertexBuffer9> high_buffer;
  CHECK(device->CreateVertexBuffer(high_vertices * sizeof(Vertex), D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
                                   &high_buffer, nullptr));
  CHECK(high_buffer->Lock(0, 0, &data, 0));
  memcpy(data, high.data(), high.size() * sizeof(Vertex));
  CHECK(high_buffer->Unlock());
  ComPtr<IDirect3DIndexBuffer9> high_indices;
  CHECK(device->CreateIndexBuffer(6 * sizeof(DWORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT,
                                  &high_indices, nullptr));
  CHECK(high_indices->Lock(0, 0, &data, 0));
  for (int i = 0; i < 6; i++)
    reinterpret_cast<DWORD *>(data)[i] = kHigh + kIndices[i];
  CHECK(high_indices->Unlock());
  CHECK(device->SetStreamSource(0, high_buffer.Get(), 0, sizeof(Vertex)));
  CHECK(device->SetIndices(high_indices.Get()));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  CHECK(device->BeginScene());
  const HRESULT high_hr = device->DrawIndexedPrimitive(D3DPT_TRIANGLEFAN, 0, kHigh, 6, 0, 4);
  CHECK(device->EndScene());
  expect(SUCCEEDED(high_hr), "a fan whose indices are past 65535 was refused");
  if (SUCCEEDED(high_hr) && read_back()) {
    const D3DCOLOR centre = at(64, 64);
    printf("probe: indices past 65535 centre %06lx area %u\n", (unsigned long)centre, covered());
    expect((centre & 0xFF0000) >= 0xF00000 && (centre & 0x00FF00) <= 0x0F0000 && (centre & 0x0000FF) <= 0x0F0000,
           "a fan whose indices are past 65535 lost its centre");
    expect(covered() >= 9000 && covered() <= 9400, "a fan whose indices are past 65535 covered the wrong area");
  }

  // winding: the fan's triangles face the same way, so the cull mode that hides one hides all of them
  CHECK(device->SetIndices(index16.Get()));
  CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(Vertex)));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  CHECK(device->BeginScene());
  CHECK(device->DrawIndexedPrimitive(D3DPT_TRIANGLEFAN, 3, 0, 6, 2, 4));
  CHECK(device->EndScene());
  if (read_back()) {
    printf("probe: culled area %u, centre %06lx\n", covered(), (unsigned long)at(64, 64));
    expect(at(64, 64) == (clear & 0x00FFFFFF), "a culled fan still drew its centre");
  }
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  CHECK(device->BeginScene());
  CHECK(device->DrawIndexedPrimitive(D3DPT_TRIANGLEFAN, 3, 0, 6, 2, 4));
  CHECK(device->EndScene());
  if (read_back()) {
    expect((at(64, 64) & 0xFF0000) >= 0xF00000, "the fan's winding is not the front face it draws");
  }
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  // the scratch buffer is reused and grown: a large fan after a small one, then the small one again
  const UINT kRing = 65;
  std::vector<Vertex> large(kRing + 1);
  large[0] = Vertex{64.0f, 64.0f, 0.0f, 1.0f, kRed};
  std::vector<WORD> large_indices(kRing + 1);
  large_indices[0] = (WORD)kRing;
  for (UINT i = 0; i < kRing; i++) {
    const float angle = 6.2831853f * (float)i / (float)kRing;
    large[i + 1] = Vertex{64.0f + 48.0f * cosf(angle), 64.0f + 48.0f * sinf(angle), 0.0f, 1.0f, kRed};
    large_indices[i + 1] = (WORD)(i + 1);
  }
  large[kRing] = large[0];
  ComPtr<IDirect3DVertexBuffer9> large_buffer;
  CHECK(device->CreateVertexBuffer((UINT)(large.size() * sizeof(Vertex)), D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
                                   &large_buffer, nullptr));
  CHECK(large_buffer->Lock(0, 0, &data, 0));
  memcpy(data, large.data(), large.size() * sizeof(Vertex));
  CHECK(large_buffer->Unlock());
  ComPtr<IDirect3DIndexBuffer9> large_indices_buffer;
  CHECK(device->CreateIndexBuffer((UINT)(large_indices.size() * sizeof(WORD)), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                  D3DPOOL_DEFAULT, &large_indices_buffer, nullptr));
  CHECK(large_indices_buffer->Lock(0, 0, &data, 0));
  memcpy(data, large_indices.data(), large_indices.size() * sizeof(WORD));
  CHECK(large_indices_buffer->Unlock());
  auto large_frame = [&]() -> int {
    CHECK(device->SetStreamSource(0, large_buffer.Get(), 0, sizeof(Vertex)));
    CHECK(device->SetIndices(large_indices_buffer.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawIndexedPrimitive(D3DPT_TRIANGLEFAN, 0, 0, kRing + 1, 0, kRing - 2));
    CHECK(device->EndScene());
    return read_back() && (at(64, 64) & 0xFF0000) >= 0xF00000 && at(64, 20) != (clear & 0x00FFFFFF) ? 0 : 1;
  };
  expect(large_frame() == 0, "a large fan did not draw");
  CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(Vertex)));
  CHECK(device->SetIndices(index16.Get()));
  indexed_centre(3, 2, 0, 6, "a fan after a larger fan did not reuse the scratch buffer");
  expect(large_frame() == 0, "the large fan did not draw after the scratch buffer was reused");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
