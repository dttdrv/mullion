// contract: the device calls a title makes around and between its draws.
// - DrawIndexedPrimitiveUP reads the vertex and index arrays it is given, draws the primitives the indices name, and
//   accepts both D3DFMT_INDEX16 and D3DFMT_INDEX32 (and refuses a primitive with no index array, or a format that is
//   neither).
// - a D3DQUERYTYPE_EVENT query is signalled once the GPU has finished everything issued before it: GetData says
//   S_FALSE while it has not and S_OK with a nonzero flag once it has, an unissued query has nothing to wait for, and
//   D3DISSUE_BEGIN is refused (an event query ends where it is issued).
// - GetData(NULL, 0, flags) polls the status (Microsoft Learn: pData may be NULL when dwSize is 0), and a query that
//   has signalled stays readable until it is issued again; it does not lose its result after one read.
// - a state block carries the state its kind names, and Apply puts it back: CreateStateBlock captures as it makes the
//   block, Capture re-reads the states the block carries, BeginStateBlock/EndStateBlock record only the states the
//   calls between them set, and a draw after Apply is drawn with the restored state.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-drawindexedprimitiveup
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dquery9-getdata
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dstateblock9-capture
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace {

const UINT kClient = 64;
const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 0);

// a vertex the fixed-function path transforms: a screen-space position and a colour
struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

// the four corners of a quad over the whole frame, in the winding a triangle strip reads
void
quad(Vertex out[4], D3DCOLOR colour) {
  out[0] = {0.0f, 0.0f, 0.0f, 1.0f, colour};
  out[1] = {(float)kClient, 0.0f, 0.0f, 1.0f, colour};
  out[2] = {0.0f, (float)kClient, 0.0f, 1.0f, colour};
  out[3] = {(float)kClient, (float)kClient, 0.0f, 1.0f, colour};
}

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

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_device_calls"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_device_calls", "d3d9_device_calls", WS_OVERLAPPEDWINDOW, 0, 0,
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

  CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

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
  // ---- DrawIndexedPrimitiveUP: the arrays the call is given, through both index formats ----------------------------
  {
    Vertex vertices[4];
    quad(vertices, D3DCOLOR_XRGB(255, 255, 255));
    // the bindings a DrawIndexedPrimitiveUP must clear: an ordinary stream and index buffer are set before it, so
    // the check after the call can tell a cleared binding from one that was never set
    ComPtr<IDirect3DVertexBuffer9> bound_vertices;
    CHECK(device->CreateVertexBuffer(sizeof(vertices), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT,
                                     &bound_vertices, nullptr));
    void *bound_vertex_data = nullptr;
    CHECK(bound_vertices->Lock(0, 0, &bound_vertex_data, 0));
    memcpy(bound_vertex_data, vertices, sizeof(vertices));
    CHECK(bound_vertices->Unlock());
    CHECK(device->SetStreamSource(0, bound_vertices.Get(), 0, sizeof(Vertex)));
    const WORD bound_indices[6] = {0, 1, 2, 2, 1, 3};
    ComPtr<IDirect3DIndexBuffer9> bound_index_buffer;
    CHECK(device->CreateIndexBuffer(sizeof(bound_indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &bound_index_buffer,
                                    nullptr));
    void *bound_index_data = nullptr;
    CHECK(bound_index_buffer->Lock(0, 0, &bound_index_data, 0));
    memcpy(bound_index_data, bound_indices, sizeof(bound_indices));
    CHECK(bound_index_buffer->Unlock());
    CHECK(device->SetIndices(bound_index_buffer.Get()));
    const WORD indices16[6] = {0, 1, 2, 2, 1, 3};
    const DWORD indices32[6] = {0, 1, 2, 2, 1, 3};
    for (int format = 0; format < 2; format++) {
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      CHECK(device->BeginScene());
      const HRESULT hr = device->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 0, 4, 2, format ? (const void *)indices32 : (const void *)indices16,
          format ? D3DFMT_INDEX32 : D3DFMT_INDEX16, vertices, sizeof(Vertex)
      );
      CHECK(device->EndScene());
      expect(SUCCEEDED(hr), "a DrawIndexedPrimitiveUP was refused");
      back_buffer.Reset();
      CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
      printf("probe: indexed %d 32,32=%06lx 2,2=%06lx\n", format, (unsigned long)pixel(32, 32),
             (unsigned long)pixel(2, 2));
      expect(pixel(32, 32) == 0x00FFFFFF, format ? "a 32-bit indexed quad did not draw its vertices"
                                                 : "a 16-bit indexed quad did not draw its vertices");
      expect(pixel(2, 2) == 0x00FFFFFF, "the indexed quad did not cover the frame");
    }
    // Following any DrawIndexedPrimitiveUP call, stream 0 and the index buffer are NULL (Microsoft Learn,
    // IDirect3DDevice9::DrawIndexedPrimitiveUP), so an ordinary draw has neither until they are set again.
    {
      ComPtr<IDirect3DVertexBuffer9> stream;
      ComPtr<IDirect3DIndexBuffer9> indices_now;
      UINT offset = 1, stride = 1;
      expect(SUCCEEDED(device->GetStreamSource(0, &stream, &offset, &stride)) && !stream && offset == 0 && stride == 0,
             "DrawIndexedPrimitiveUP did not clear stream 0");
      expect(SUCCEEDED(device->GetIndices(&indices_now)) && !indices_now,
             "DrawIndexedPrimitiveUP did not clear the index buffer");
      expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2) == D3DERR_INVALIDCALL,
             "a draw after DrawIndexedPrimitiveUP used the buffers it should have cleared");
    }
    // the vertices are the array's from vertex 0 whatever MinVertexIndex says, which is what the indices address
    Vertex offset_vertices[6] = {};
    for (int at = 0; at < 4; at++)
      offset_vertices[at + 2] = vertices[at];
    const WORD offset_indices[6] = {2, 3, 4, 4, 3, 5};
    const HRESULT hr = [&]() -> HRESULT {
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      CHECK(device->BeginScene());
      const HRESULT drawn = device->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 2, 4, 2, offset_indices, D3DFMT_INDEX16, offset_vertices, sizeof(Vertex)
      );
      CHECK(device->EndScene());
      back_buffer.Reset();
      CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
      return drawn;
    }();
    expect(SUCCEEDED(hr), "an indexed draw with MinVertexIndex != 0 was refused");
    expect(pixel(32, 32) == 0x00FFFFFF, "the indices did not address the vertices of the array");

    // and what it refuses: no index array, and a format that is not an index's
    expect(device->DrawIndexedPrimitiveUP(
               D3DPT_TRIANGLELIST, 0, 4, 2, nullptr, D3DFMT_INDEX16, vertices, sizeof(Vertex)
           ) == D3DERR_INVALIDCALL,
           "a DrawIndexedPrimitiveUP with no index array was accepted");
    expect(device->DrawIndexedPrimitiveUP(
               D3DPT_TRIANGLELIST, 0, 4, 2, indices16, D3DFMT_A8R8G8B8, vertices, sizeof(Vertex)
           ) == D3DERR_INVALIDCALL,
           "a DrawIndexedPrimitiveUP whose index format is not an index's was accepted");
    // the fixed-function vertex format is still the one the draw needs
    expect(device->DrawIndexedPrimitiveUP(
               D3DPT_TRIANGLELIST, 0, 4, 2, indices16, D3DFMT_INDEX16, vertices, sizeof(Vertex) + 4
           ) == D3DERR_INVALIDCALL,
           "a DrawIndexedPrimitiveUP with a stride the FVF is not was accepted");
  }

  // ---- D3DQUERYTYPE_EVENT: the GPU has finished what was issued before the query ----------------------------------
  {
    ComPtr<IDirect3DQuery9> query;
    CHECK(device->CreateQuery(D3DQUERYTYPE_EVENT, &query));
    expect(query->GetType() == D3DQUERYTYPE_EVENT, "an event query is not of its type");
    expect(query->GetDataSize() == sizeof(WINBOOL), "an event query's data is not the flag D3D9's is");

    // nothing is issued yet, so there is nothing to wait for
    WINBOOL signalled = FALSE;
    expect(query->GetData(&signalled, sizeof(signalled), 0) == D3DERR_INVALIDCALL,
           "an unissued event query answered GetData");
    expect(query->Issue(D3DISSUE_BEGIN) == D3DERR_INVALIDCALL, "an event query accepted D3DISSUE_BEGIN");
    expect(query->Issue(D3DISSUE_END | D3DISSUE_BEGIN) == D3DERR_INVALIDCALL,
           "an event query accepted both issue flags at once");

    // a draw, then the query: the flag is S_OK once the frame's work is done, which a title waits on
    Vertex vertices[4];
    quad(vertices, D3DCOLOR_XRGB(1, 2, 3));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
    CHECK(device->EndScene());
    CHECK(query->Issue(D3DISSUE_END));
    // a status poll with no data and no size is a successful call whether or not the GPU has finished (Microsoft
    // Learn: pData may be NULL when dwSize is 0); the earlier read above expected it to fail, which it must not
    HRESULT polled = query->GetData(nullptr, 0, 0);
    expect(polled == S_OK || polled == S_FALSE, "a null-data status poll was not a query status");
    HRESULT got = S_FALSE;
    for (int attempt = 0; attempt < 500 && got == S_FALSE; attempt++) {
      got = query->GetData(&signalled, sizeof(signalled), D3DGETDATA_FLUSH);
      if (got == S_FALSE)
        Sleep(10);
    }
    expect(got == S_OK, "an issued event query never signalled");
    if (got == S_OK)
      expect(signalled != FALSE, "an event query signalled with a zero flag");
    // a signalled query stays signalled until it is issued again: every read answers S_OK, with or without FLUSH,
    // with or without a data pointer
    expect(query->GetData(nullptr, 0, 0) == S_OK, "a signalled event query lost its result on a null-data poll");
    expect(query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK,
           "a signalled event query lost its result on a null-data FLUSH poll");
    expect(query->GetData(&signalled, 0, 0) == S_OK,
           "a signalled event query refused a zero-size read with a data pointer");
    signalled = FALSE;
    expect(query->GetData(&signalled, sizeof(signalled), 0) == S_OK && signalled != FALSE,
           "a signalled event query refused a repeated read");
    // and what GetData refuses: a data pointer with a size, a size that is not the query's, and an unknown flag
    expect(query->GetData(nullptr, sizeof(signalled), 0) == D3DERR_INVALIDCALL,
           "an event query took a null data pointer with a non-zero size");
    expect(query->GetData(&signalled, sizeof(signalled) - 1, 0) == D3DERR_INVALIDCALL,
           "an event query answered GetData with a size that is not its data's");
    expect(query->GetData(nullptr, 0, 0x80000000u) == D3DERR_INVALIDCALL,
           "an event query took an unknown GetData flag");
    // reissuing re-arms it, and the new result is read the same way
    CHECK(query->Issue(D3DISSUE_END));
    got = S_FALSE;
    for (int attempt = 0; attempt < 500 && got == S_FALSE; attempt++) {
      got = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
      if (got == S_FALSE)
        Sleep(10);
    }
    expect(got == S_OK, "a re-issued event query never signalled");
    expect(query->GetData(&signalled, sizeof(signalled), D3DGETDATA_FLUSH) == S_OK,
           "a re-issued event query refused its result");

    // the query types that need a pass of their own are refused rather than answered with a number nothing measured
    ComPtr<IDirect3DQuery9> occlusion;
    expect(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion) == D3DERR_INVALIDCALL,
           "an occlusion query was made");
    expect(!occlusion, "a refused query still handed an object back");
  }

  // ---- state blocks: what a block carries, and Apply putting it back ------------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));

    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    CHECK(device->SetTexture(0, texture.Get()));

    // a block of all device state, captured as it is made
    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &block));
    {
      DWORD colour_write = 0, alpha_blend = 0, cull = 0, mag = 0, colour_op = 0;
      CHECK(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colour_write));
      CHECK(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend));
      CHECK(device->GetRenderState(D3DRS_CULLMODE, &cull));
      CHECK(device->GetSamplerState(0, D3DSAMP_MAGFILTER, &mag));
      CHECK(device->GetTextureStageState(0, D3DTSS_COLOROP, &colour_op));
      expect(colour_write == 0xF && alpha_blend == FALSE && cull == D3DCULL_NONE && mag == D3DTEXF_LINEAR &&
                 colour_op == D3DTOP_MODULATE,
             "the device does not hold the state the block is about to capture");
    }

    // every state the block carries is changed, and the block puts them back
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    ComPtr<IDirect3DBaseTexture9> gone;
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->GetTexture(0, &gone));
    expect(!gone, "the texture stage was not emptied for the test");
    CHECK(block->Apply());

    {
      DWORD colour_write = 0, alpha_blend = 0, cull = 0, mag = 0, colour_op = 0;
      CHECK(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colour_write));
      CHECK(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend));
      CHECK(device->GetRenderState(D3DRS_CULLMODE, &cull));
      CHECK(device->GetSamplerState(0, D3DSAMP_MAGFILTER, &mag));
      CHECK(device->GetTextureStageState(0, D3DTSS_COLOROP, &colour_op));
      expect(colour_write == 0xF, "Apply did not restore the colour write mask");
      expect(alpha_blend == FALSE, "Apply did not restore alpha blending");
      expect(cull == D3DCULL_NONE, "Apply did not restore the cull mode");
      expect(mag == D3DTEXF_LINEAR, "Apply did not restore the sampler's filter");
      expect(colour_op == D3DTOP_MODULATE, "Apply did not restore the texture stage's operation");
      ComPtr<IDirect3DBaseTexture9> restored;
      CHECK(device->GetTexture(0, &restored));
      expect(restored.Get() == (IDirect3DBaseTexture9 *)texture.Get(), "Apply did not restore the stage's texture");
    }

    // and a draw after Apply is drawn with what the block put back: the colour write mask the frame's pixels say
    {
      Vertex vertices[4];
      quad(vertices, D3DCOLOR_XRGB(255, 255, 255));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
      CHECK(device->BeginScene());
      CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
      CHECK(device->EndScene());
      back_buffer.Reset();
      CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
      printf("probe: restored colour write 32,32=%06lx\n", (unsigned long)pixel(32, 32));
      expect(pixel(32, 32) == 0x00FFFFFF, "a draw after Apply did not write every channel the block restored");
    }

    // Capture re-reads the states the block carries, and does not capture the device's others
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN));
    CHECK(block->Capture());
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
    CHECK(block->Apply());
    {
      DWORD colour_write = 0;
      CHECK(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colour_write));
      expect(colour_write == D3DCOLORWRITEENABLE_GREEN, "Capture did not take the value the device had");
    }
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));

    // a pixel-state block carries the pixel pipeline's states and not the vertex pipeline's
    ComPtr<IDirect3DStateBlock9> pixel_block;
    CHECK(device->CreateStateBlock(D3DSBT_PIXELSTATE, &pixel_block));
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_BLUE));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW));
    CHECK(pixel_block->Apply());
    {
      DWORD colour_write = 0, cull = 0;
      CHECK(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colour_write));
      CHECK(device->GetRenderState(D3DRS_CULLMODE, &cull));
      expect(colour_write == 0xF, "a pixel-state block did not restore the colour write mask");
      expect(cull == D3DCULL_CCW, "a pixel-state block restored the cull mode, which is vertex state");
    }
    // ... and a vertex-state block the vertex pipeline's, not the pixel pipeline's
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW));
    ComPtr<IDirect3DStateBlock9> vertex_block;
    CHECK(device->CreateStateBlock(D3DSBT_VERTEXSTATE, &vertex_block));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED));
    CHECK(vertex_block->Apply());
    {
      DWORD colour_write = 0, cull = 0;
      CHECK(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colour_write));
      CHECK(device->GetRenderState(D3DRS_CULLMODE, &cull));
      expect(cull == D3DCULL_CCW, "a vertex-state block did not restore the cull mode");
      expect(colour_write == D3DCOLORWRITEENABLE_RED,
             "a vertex-state block restored the colour write mask, which is pixel state");
    }
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

    // a block made by recording carries what the calls between Begin and End set, and nothing else
    ComPtr<IDirect3DStateBlock9> recorded;
    CHECK(device->BeginStateBlock());
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    const float constants[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    CHECK(device->SetPixelShaderConstantF(3, constants, 2));
    // a capture of a block while the device records one is refused, as D3D9 refuses it
    expect(block->Capture() == D3DERR_INVALIDCALL, "a state block was captured while the device was recording");
    expect(block->Apply() == D3DERR_INVALIDCALL, "a state block was applied while the device was recording");
    CHECK(device->EndStateBlock(&recorded));
    expect(recorded != nullptr, "EndStateBlock made no block");
    IDirect3DStateBlock9 *without_begin = nullptr;
    expect(device->EndStateBlock(&without_begin) == D3DERR_INVALIDCALL,
           "EndStateBlock without BeginStateBlock succeeded");
    expect(!without_begin, "a refused EndStateBlock still handed a block back");

    // the recorded states change, and the block puts its own back while leaving the others as they are
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
    CHECK(device->SetPixelShaderConstantF(3, constants, 2)); // the same values: the block carries them either way
    const float changed[8] = {8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f};
    CHECK(device->SetPixelShaderConstantF(3, changed, 2));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE)); // not recorded, so not restored
    CHECK(recorded->Apply());
    {
      DWORD colour_write = 0, mag = 0, z_enable = 0;
      float got[8] = {};
      CHECK(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colour_write));
      CHECK(device->GetSamplerState(0, D3DSAMP_MAGFILTER, &mag));
      CHECK(device->GetRenderState(D3DRS_ZENABLE, &z_enable));
      CHECK(device->GetPixelShaderConstantF(3, got, 2));
      expect(colour_write == (D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE),
             "a recorded block did not restore the render state it recorded");
      expect(mag == D3DTEXF_POINT, "a recorded block did not restore the sampler state it recorded");
      expect(memcmp(got, constants, sizeof(constants)) == 0,
             "a recorded block did not restore the shader constants it recorded");
      expect(z_enable == TRUE, "a recorded block restored a state it did not record");
    }
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));

    // an FVF, the stream source and the viewport are all-device state: the block carries and restores them
    ComPtr<IDirect3DVertexBuffer9> buffer;
    CHECK(device->CreateVertexBuffer(sizeof(Vertex), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &buffer,
                                     nullptr));
    CHECK(device->SetStreamSource(0, buffer.Get(), 0, sizeof(Vertex)));
    D3DVIEWPORT9 small{4, 4, 32, 32, 0.0f, 1.0f};
    CHECK(device->SetViewport(&small));
    ComPtr<IDirect3DStateBlock9> resource_block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &resource_block));
    CHECK(device->SetStreamSource(0, nullptr, 0, 0));
    D3DVIEWPORT9 full{0, 0, kClient, kClient, 0.0f, 1.0f};
    CHECK(device->SetViewport(&full));
    CHECK(device->SetFVF(0));
    CHECK(resource_block->Apply());
    {
      ComPtr<IDirect3DVertexBuffer9> stream;
      UINT offset = 1, stride = 0;
      CHECK(device->GetStreamSource(0, &stream, &offset, &stride));
      expect(stream.Get() == buffer.Get() && offset == 0 && stride == sizeof(Vertex),
             "Apply did not restore the stream source");
      D3DVIEWPORT9 viewport{};
      CHECK(device->GetViewport(&viewport));
      expect(viewport.X == 4 && viewport.Y == 4 && viewport.Width == 32,
             "Apply did not restore the viewport");
      DWORD fvf = 0;
      CHECK(device->GetFVF(&fvf));
      expect(fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "Apply did not restore the FVF");
    }
    CHECK(device->SetViewport(&full));

    // a state block kind D3D9 does not have is refused
    ComPtr<IDirect3DStateBlock9> refused;
    expect(device->CreateStateBlock((D3DSTATEBLOCKTYPE)7, &refused) == D3DERR_INVALIDCALL,
           "a state block of an unknown kind was made");
  }

  CHECK(device->SetTexture(0, nullptr));
  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
