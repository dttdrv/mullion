// contract: a Direct3D 11 swap chain's buffers and what its window shows, as DXGI's documentation has them.
// - GetDesc says what the swap chain was created as, and its buffer 0 is a texture of that size and format.
// - ResizeBuffers takes a new buffer count and keeps it for 0, keeps the format for DXGI_FORMAT_UNKNOWN and takes the
//   window's client size for a size of 0; its flags are the swap chain's from then on. a format no swap chain has is
//   refused and leaves the swap chain as it was, and so is a change of DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, which
//   "can't be added or removed" by it (IDXGISwapChain::ResizeBuffers).
// - Present shows the back buffer (IDXGISwapChain::Present): the window's pixels are the colour the buffer was
//   cleared to, a new one each frame, also after the buffers were resized.
// the window's pixels are read through GDI, which has them when frames get to the window through GDI: the test asks
// for that (dxgi.presentThroughGDI), since nothing reads a Metal view's pixels back.
#include "d3d11_test.hpp"
#include <cmath>

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("%s\n", what);
  };
  SetEnvironmentVariableA("DXMT_CONFIG", "dxgi.presentThroughGDI=True");
  const UINT client = 192, buffers = 2;
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d11_swapchain"};
  RegisterClassA(&wc);
  RECT rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  auto window = CreateWindowA(
      "d3d11_swapchain", "d3d11_swapchain", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr
  );
  const UINT created_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  DXGI_SWAP_CHAIN_DESC created{{client, client, {60, 1}, format}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers,
                               window, TRUE, DXGI_SWAP_EFFECT_DISCARD, created_flags};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swapchain;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &created, &swapchain, &device,
      nullptr, &context
  ));

  // what the swap chain says its buffers are, and what its first buffer is
  auto is = [&](UINT width, UINT height, UINT count, DXGI_FORMAT buffer_format, UINT flags) {
    DXGI_SWAP_CHAIN_DESC desc{};
    D3D11_TEXTURE2D_DESC texture{};
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(swapchain->GetDesc(&desc)) || FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
      return false;
    buffer->GetDesc(&texture);
    return desc.BufferDesc.Width == width && desc.BufferDesc.Height == height && desc.BufferCount == count &&
           desc.BufferDesc.Format == buffer_format && desc.Flags == flags && desc.OutputWindow == window &&
           texture.Width == width && texture.Height == height && texture.Format == buffer_format;
  };
  // the back buffer cleared to a colour and presented: the window's pixels are that colour, within a step of the
  // buffer's rounding
  auto shows = [&](const char *what, const float colour[4]) -> HRESULT {
    HRESULT hr;
    {
      ComPtr<ID3D11Texture2D> buffer;
      ComPtr<ID3D11RenderTargetView> view;
      if (FAILED(hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
          FAILED(hr = device->CreateRenderTargetView(buffer.Get(), nullptr, &view)))
        return hr;
      context->ClearRenderTargetView(view.Get(), colour);
    }
    if (FAILED(hr = swapchain->Present(0, 0)))
      return hr;
    RECT area;
    GetClientRect(window, &area);
    const UINT across = 8, attempts = 250;
    unsigned differ = 0;
    for (UINT attempt = 0; attempt < attempts; attempt++) {
      MSG msg;
      while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
      Sleep(20);
      differ = 0;
      HDC dc = GetDC(window);
      for (UINT y = area.bottom / across / 2; y < (UINT)area.bottom; y += area.bottom / across)
        for (UINT x = area.right / across / 2; x < (UINT)area.right; x += area.right / across) {
          auto got = GetPixel(dc, x, y);
          const int channels[3] = {GetRValue(got), GetGValue(got), GetBValue(got)};
          bool same = true;
          for (int c = 0; c < 3; c++)
            same &= std::fabs(channels[c] - colour[c] * 255) <= 1;
          if (!same && differ++ < 2 && attempt == attempts - 1)
            printf("%s: the window's pixel %u,%u is %d %d %d, want %.1f %.1f %.1f\n", what, x, y, channels[0],
                   channels[1], channels[2], colour[0] * 255, colour[1] * 255, colour[2] * 255);
        }
      ReleaseDC(window, dc);
      if (!differ)
        break;
    }
    expect(!differ, what);
    return S_OK;
  };
  const float red[4] = {1, 0.25f, 0, 1}, green[4] = {0, 1, 0.25f, 1}, blue[4] = {0.25f, 0, 1, 1}, yellow[4] = {1, 0.75f, 0, 1};

  expect(is(client, client, buffers, format, created_flags), "a new swap chain is not what it was created as");
  CHECK(shows("the first frame is not in the window", red));
  CHECK(shows("the second frame is not in the window", green));

  const UINT wider = client + 64, taller = client + 32, more = buffers + 1;
  const DXGI_FORMAT other = DXGI_FORMAT_B8G8R8A8_UNORM;
  CHECK(swapchain->ResizeBuffers(more, wider, taller, other, 0));
  expect(is(wider, taller, more, other, 0), "resized buffers are not what was asked for");
  CHECK(shows("a frame of resized buffers is not in the window", blue));
  // what is kept
  RECT area;
  GetClientRect(window, &area);
  CHECK(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
  expect(is(area.right, area.bottom, more, other, 0),
         "a resize to nothing new does not keep the count and format and take the window's size");
  // a size of 0 is the window's in that direction alone ("If you specify zero, DXGI will use the width of the client
  // area of the target window", and so the height)
  CHECK(swapchain->ResizeBuffers(0, 0, taller, DXGI_FORMAT_UNKNOWN, 0));
  expect(is(area.right, taller, more, other, 0), "a resize to a height alone does not take the window's width with it");
  CHECK(swapchain->ResizeBuffers(0, wider, 0, DXGI_FORMAT_UNKNOWN, 0));
  expect(is(wider, area.bottom, more, other, 0), "a resize to a width alone does not take the window's height with it");
  CHECK(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
  // what is refused
  expect(FAILED(swapchain->ResizeBuffers(0, wider, taller, DXGI_FORMAT_D32_FLOAT, 0)) && is(area.right, area.bottom, more, other, 0),
         "a resize to a format no swap chain has is taken, or changes the swap chain");
  expect(swapchain->ResizeBuffers(0, wider, taller, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == DXGI_ERROR_INVALID_CALL &&
             is(area.right, area.bottom, more, other, 0),
         "a resize adds tearing to a swap chain created without it");
  CHECK(shows("a frame after refused resizes is not in the window", yellow));

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
