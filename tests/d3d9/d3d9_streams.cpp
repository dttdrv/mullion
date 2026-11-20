// contract: the stream frequencies D3D9 accepts, and the instanced draws they make.
// - SetStreamSourceFreq takes a plain frequency (one or more) or one flag with a count: D3DSTREAMSOURCE_INDEXEDDATA on
//   any stream, D3DSTREAMSOURCE_INSTANCEDATA on any stream but 0. A zero count is accepted with a flag, both flags
//   together are refused, a zero frequency is refused, and a refused call leaves the stream's frequency as it was.
// - DrawIndexedPrimitive draws the number of instances the D3DSTREAMSOURCE_INDEXEDDATA stream names, reading a stream
//   marked D3DSTREAMSOURCE_INSTANCEDATA at that instance's own vertices. DrawPrimitive draws one instance whatever the
//   frequencies say (Microsoft Learn, "Instancing"; Wine visual.c stream_test draws both and reads the pixels back).
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

// the vs_1_1 Wine's stream_test uses: the vertex position plus the instance's own position, and the vertex colour
const DWORD kInstanceVertexShader[] = {
    0xfffe0101,                                     // vs_1_1
    0x0000001f, 0x80000000, 0x900f0000,             // dcl_position v0
    0x0000001f, 0x8000000a, 0x900f0001,             // dcl_color0 v1
    0x0000001f, 0x80000005, 0x900f0002,             // dcl_texcoord v2
    0x00000001, 0x800f0000, 0x90e40000,             // mov r0, v0
    0x00000002, 0xc00f0000, 0x80e40000, 0x90e40002, // add oPos, r0, v2
    0x00000001, 0xd00f0000, 0x90e40001,             // mov oD0, v1
    0x0000ffff,                                     // end
};

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
                         "d3d9_streams"};
  RegisterClassA(&window_class);
  const UINT kWidth = 640;
  const UINT kHeight = 480;
  RECT window_rect{0, 0, (LONG)kWidth, (LONG)kHeight};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_streams", "d3d9_streams", WS_OVERLAPPEDWINDOW, 0, 0,
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
  present.BackBufferWidth = kWidth;
  present.BackBufferHeight = kHeight;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  // Wine visual.c stream_test's device: an automatic depth-stencil and the depth test it turns on
  present.EnableAutoDepthStencil = TRUE;
  present.AutoDepthStencilFormat = D3DFMT_D24S8;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  // ---- the accept/refuse table -----------------------------------------------------------------------------------
  UINT value = 0xdeadbeef;
  expect(SUCCEEDED(device->GetStreamSourceFreq(0, &value)) && value == 1, "stream 0's frequency does not start at 1");
  expect(SUCCEEDED(device->GetStreamSourceFreq(1, &value)) && value == 1, "stream 1's frequency does not start at 1");
  expect(device->SetStreamSourceFreq(1, 1) == D3D_OK, "a plain frequency of 1 was refused");
  expect(device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INSTANCEDATA | 1) == D3DERR_INVALIDCALL,
         "INSTANCEDATA on stream 0 was accepted");
  expect(device->SetStreamSourceFreq(1, 0) == D3DERR_INVALIDCALL, "a zero frequency was accepted");
  expect(SUCCEEDED(device->GetStreamSourceFreq(1, &value)) && value == 1, "a refused call changed the frequency");
  expect(device->SetStreamSourceFreq(1, 2) == D3D_OK, "a plain frequency of 2 was refused");
  expect(SUCCEEDED(device->GetStreamSourceFreq(1, &value)) && value == 2, "the frequency read back is not 2");
  expect(device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INDEXEDDATA) == D3D_OK,
         "INDEXEDDATA with a zero count was refused");
  expect(SUCCEEDED(device->GetStreamSourceFreq(1, &value)) && value == D3DSTREAMSOURCE_INDEXEDDATA,
         "the flagged zero frequency read back wrong");
  expect(device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA) == D3D_OK,
         "INSTANCEDATA with a zero count was refused");
  expect(device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | D3DSTREAMSOURCE_INDEXEDDATA) ==
             D3DERR_INVALIDCALL,
         "both flags together were accepted");
  expect(SUCCEEDED(device->GetStreamSourceFreq(1, &value)) && value == D3DSTREAMSOURCE_INSTANCEDATA,
         "a refused call changed a flagged frequency");
  expect(device->SetStreamSourceFreq(16, 1) == D3DERR_INVALIDCALL, "a stream past MaxStreams was accepted");
  CHECK(device->SetStreamSourceFreq(0, 1));
  CHECK(device->SetStreamSourceFreq(1, 1));
  CHECK(device->SetStreamSourceFreq(2, 1));

  // ---- an indexed instanced draw ----------------------------------------------------------------------------------
  // four vertices of a small quad at the origin, one colour, and two instance positions that move it apart
  // Wine visual.c stream_test's vertices: a ±0.5 quad and ±0.6 instance offsets, each operand's missing w filled
  // with 1, so the sum has w 2 and the perspective divide halves what the quad covers
  const float quad[4][3] = {{-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}};
  const float colours[4][4] = {{1.0f, 0.0f, 0.0f, 1.0f},
                               {1.0f, 0.0f, 0.0f, 1.0f},
                               {1.0f, 0.0f, 0.0f, 1.0f},
                               {1.0f, 0.0f, 0.0f, 1.0f}};
  const float instances[4][3] = {{-0.6f, -0.6f, 0.0f}, {0.6f, -0.6f, 0.0f}, {0.6f, 0.6f, 0.0f}, {-0.6f, 0.6f, 0.0f}};
  const short indices[6] = {0, 1, 2, 2, 1, 3};

  const D3DVERTEXELEMENT9 declaration[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      {2, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> layout;
  ComPtr<IDirect3DVertexShader9> shader;
  CHECK(device->CreateVertexDeclaration(declaration, &layout));
  CHECK(device->CreateVertexShader(kInstanceVertexShader, &shader));

  auto make_buffer = [&](const void *data, UINT bytes) -> ComPtr<IDirect3DVertexBuffer9> {
    ComPtr<IDirect3DVertexBuffer9> buffer;
    if (FAILED(device->CreateVertexBuffer(bytes, 0, 0, D3DPOOL_MANAGED, &buffer, nullptr)))
      return nullptr;
    void *mapped = nullptr;
    if (FAILED(buffer->Lock(0, bytes, &mapped, 0)))
      return nullptr;
    memcpy(mapped, data, bytes);
    buffer->Unlock();
    return buffer;
  };
  ComPtr<IDirect3DVertexBuffer9> positions = make_buffer(quad, sizeof(quad));
  ComPtr<IDirect3DVertexBuffer9> colour_buffer = make_buffer(colours, sizeof(colours));
  ComPtr<IDirect3DVertexBuffer9> instance_buffer = make_buffer(instances, sizeof(instances));
  ComPtr<IDirect3DIndexBuffer9> index_buffer;
  // Wine visual.c stream_test's index buffer is a default-pool one it locks and fills
  CHECK(device->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &index_buffer, nullptr));
  {
    void *mapped = nullptr;
    CHECK(index_buffer->Lock(0, sizeof(indices), &mapped, 0));
    memcpy(mapped, indices, sizeof(indices));
    index_buffer->Unlock();
  }

  CHECK(device->SetVertexDeclaration(layout.Get()));
  CHECK(device->SetVertexShader(shader.Get()));
  CHECK(device->SetPixelShader(nullptr));
  CHECK(device->SetIndices(index_buffer.Get()));
  CHECK(device->SetStreamSource(0, positions.Get(), 0, sizeof(quad[0])));
  CHECK(device->SetStreamSource(1, colour_buffer.Get(), 0, sizeof(colours[0])));
  CHECK(device->SetStreamSource(2, instance_buffer.Get(), 0, sizeof(instances[0])));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  ComPtr<IDirect3DSurface9> back_buffer;
  auto pixel = [&](UINT x, UINT y) -> DWORD {
    if (!back_buffer)
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD got = ((const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch))[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };
  auto frame = [&](bool indexed, UINT instances_count) -> HRESULT {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
    CHECK(device->BeginScene());
    HRESULT drawn = indexed ? device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2)
                            : device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    CHECK(device->EndScene());
    back_buffer.Reset();
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    return drawn;
  };

  const DWORD red = D3DCOLOR_XRGB(255, 0, 0) & 0x00FFFFFF;
  const DWORD clear = D3DCOLOR_XRGB(0, 0, 255) & 0x00FFFFFF;

  // one instance: the quad is at the left, the right is untouched
  CHECK(device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1));
  CHECK(device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INDEXEDDATA | 1));
  CHECK(device->SetStreamSourceFreq(2, D3DSTREAMSOURCE_INSTANCEDATA | 1));
  expect(SUCCEEDED(frame(true, 1)), "an indexed draw with one instance was refused");
  expect(pixel(160, 360) == red, "one instance's quad was not drawn where its instance position puts it");
  expect(pixel(480, 360) == clear, "one instance's draw covered the second instance's place");

  // the draw after a Present is still the frame's: Wine visual.c stream_test presents the non-indexed draw and then
  // draws the indexed one into the same back buffer
  CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
  expect(SUCCEEDED(frame(true, 1)), "an indexed draw after Present was refused");
  expect(pixel(160, 360) == red, "an indexed draw after Present did not draw");
  expect(pixel(480, 360) == clear, "an indexed draw after Present covered another instance's place");

  // Wine's case 0: four instances, one in each quadrant
  CHECK(device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 4));
  CHECK(device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INDEXEDDATA | 4));
  expect(SUCCEEDED(frame(true, 4)), "an indexed draw with four instances was refused");
  expect(pixel(160, 360) == red, "the first instance's quad was not drawn");
  expect(pixel(480, 360) == red, "the second instance's quad was not drawn");
  expect(pixel(480, 120) == red, "the third instance's quad was not drawn");
  expect(pixel(160, 120) == red, "the fourth instance's quad was not drawn");

  // a non-indexed draw is one instance whatever the frequency says: only the first quad
  CHECK(device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2));
  expect(SUCCEEDED(frame(false, 1)), "a non-indexed draw under a frequency was refused");
  expect(pixel(160, 360) == red, "a non-indexed draw did not draw the first instance");
  expect(pixel(480, 360) == clear, "a non-indexed draw drew more than one instance");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
