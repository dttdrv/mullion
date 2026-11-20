// the frame the fixed-function tests read back, in one place: a 64 by 64 window and the device over it, the back buffer
// copied into a system-memory surface, and the pixels of it. tests/d3d9/d3d9_transform.cpp and tests/d3d9/d3d9_lighting.cpp
// both draw on such a frame and compare the pixels it comes back with, which is the whole of what they share: what each
// of them draws, the state it draws under and what a pixel should be are its own.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>

// a call a test cannot go on without: it says which and where, and ends the test as a failure. this is the frame's too
// because both tests that use it are written around a sequence of these
#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace d3d9_test {

// the frame's size in pixels, and the space the device's matrices take a vertex to: the NDC of a pixel is
// (px - viewport.x) * 2 / viewport.width - 1 for x, and the other way round for y, since Direct3D's viewport maps
// NDC y = +1 to the top of the target (D3DVIEWPORT9, "Viewports and Clipping")
inline constexpr UINT kSize = 64;
inline constexpr float kExtent = 64.0f;

inline float
ndc_x(float pixel) {
  return pixel * 2.0f / kExtent - 1.0f;
}

inline float
ndc_y(float pixel) {
  return 1.0f - pixel * 2.0f / kExtent;
}

// D3D9's matrices, which start at the identity
inline D3DMATRIX
identity() {
  D3DMATRIX m{};
  for (int at = 0; at < 4; at++)
    m.m[at][at] = 1.0f;
  return m;
}

class Frame {
public:
  // the class and the window named `name`, the Direct3D 9 object, the device over the window with a discard chain and
  // an automatic depth-stencil surface, and the system-memory surface the frame is read back into
  bool create(const char *name) {
    WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, name};
    RegisterClassA(&window_class);
    RECT window_rect{0, 0, (LONG)kSize, (LONG)kSize};
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
    window_ = CreateWindowA(
        name, name, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top, nullptr, nullptr, window_class.hInstance, nullptr
    );
    if (!window_) {
      printf("failed: no window\n");
      return false;
    }

    direct3d_.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    if (!direct3d_) {
      printf("failed: Direct3DCreate9 made no object\n");
      return false;
    }
    present_.BackBufferWidth = kSize;
    present_.BackBufferHeight = kSize;
    present_.BackBufferFormat = D3DFMT_X8R8G8B8;
    present_.BackBufferCount = 1;
    present_.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_.EnableAutoDepthStencil = TRUE;
    present_.AutoDepthStencilFormat = D3DFMT_D24S8;
    present_.hDeviceWindow = window_;
    present_.Windowed = TRUE;
    present_.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(direct3d_->CreateDevice(
            0, D3DDEVTYPE_HAL, window_, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present_, &device_
        )) ||
        !device_)
      return false;
    return SUCCEEDED(device_->CreateOffscreenPlainSurface(
        kSize, kSize, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback_, nullptr
    ));
  }

  // the device goes first, as the tests that owned it released it: the Direct3D object before the window, so nothing the
  // device made outlives the chain it presented to
  ~Frame() {
    device_.Reset();
    readback_.Reset();
    direct3d_.Reset();
    if (window_)
      DestroyWindow(window_);
  }

  IDirect3DDevice9 *device() {
    return device_.Get();
  }
  // the chain's parameters as they were made, for a device Reset (which takes them by pointer and zeroes the fields it
  // reports back, so it is given a copy)
  D3DPRESENT_PARAMETERS present() const {
    return present_;
  }

  // the back buffer's pixels, read before Present: a D3DSWAPEFFECT_DISCARD chain keeps nothing of what it presented,
  // so the pixels are the back buffer's only until then
  bool read() {
    Microsoft::WRL::ComPtr<IDirect3DSurface9> back_buffer;
    D3DLOCKED_RECT locked{};
    if (FAILED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) ||
        FAILED(device_->GetRenderTargetData(back_buffer.Get(), readback_.Get())) ||
        FAILED(readback_->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return false;
    for (UINT y = 0; y < kSize; y++)
      memcpy(&pixels_[(size_t)y * kSize], (const char *)locked.pBits + (size_t)y * locked.Pitch, kSize * 4);
    readback_->UnlockRect();
    return true;
  }
  // one pixel of the frame read last, without its alpha: an X8R8G8B8 target has no room for the alpha D3DCOLOR_XRGB
  // writes
  D3DCOLOR pixel(UINT x, UINT y) const {
    return pixels_[(size_t)y * kSize + x] & 0x00FFFFFF;
  }
  // every pixel of it, row by row, for a check that compares two frames
  const std::vector<D3DCOLOR> &pixels() const {
    return pixels_;
  }

private:
  HWND window_ = nullptr;
  D3DPRESENT_PARAMETERS present_{};
  Microsoft::WRL::ComPtr<IDirect3D9> direct3d_;
  Microsoft::WRL::ComPtr<IDirect3DDevice9> device_;
  Microsoft::WRL::ComPtr<IDirect3DSurface9> readback_;
  std::vector<D3DCOLOR> pixels_ = std::vector<D3DCOLOR>((size_t)kSize * kSize, 0);
};

} // namespace d3d9_test
