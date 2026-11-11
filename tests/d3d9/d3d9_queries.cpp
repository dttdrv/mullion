#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <initializer_list>

static int failures = 0;
static IDirect3DDevice9 *device;

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

template <typename T>
static HRESULT
wait(IDirect3DQuery9 *query, T &value, DWORD flags = D3DGETDATA_FLUSH) {
  HRESULT hr;
  DWORD start = GetTickCount();
  while ((hr = query->GetData(&value, sizeof(value), flags)) == S_FALSE && GetTickCount() - start < 5000)
    ;
  return hr;
}

struct ScreenVertex {
  float x, y, z, rhw;
  DWORD color;
};

static void
quad(float x0, float y0, float x1, float y1, float z) {
  ScreenVertex v[4] = {
      {x0, y0, z, 1, 0xFFFFFFFF},
      {x1, y0, z, 1, 0xFFFFFFFF},
      {x0, y1, z, 1, 0xFFFFFFFF},
      {x1, y1, z, 1, 0xFFFFFFFF},
  };
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ScreenVertex));
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_queries";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr, instance, nullptr);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = hwnd;
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

  check("occlusion queries are supported", device->CreateQuery(D3DQUERYTYPE_OCCLUSION, nullptr) == D3D_OK);
  check("pipeline timings are not", device->CreateQuery(D3DQUERYTYPE_PIPELINETIMINGS, nullptr) == D3DERR_NOTAVAILABLE);

  IDirect3DQuery9 *event = nullptr, *occlusion = nullptr, *ts0 = nullptr, *ts1 = nullptr, *disjoint = nullptr,
                  *freq = nullptr;
  device->CreateQuery(D3DQUERYTYPE_EVENT, &event);
  device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion);
  device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &ts0);
  device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &ts1);
  device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint);
  device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq);
  if (!event || !occlusion || !ts0 || !ts1 || !disjoint || !freq) {
    printf("FAIL: query creation\n");
    return 1;
  }
  check("data sizes", event->GetDataSize() == sizeof(BOOL) && occlusion->GetDataSize() == sizeof(DWORD) &&
                          ts0->GetDataSize() == sizeof(UINT64));

  device->BeginScene();
  device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

  disjoint->Issue(D3DISSUE_BEGIN);
  ts0->Issue(D3DISSUE_END);
  occlusion->Issue(D3DISSUE_BEGIN);
  DWORD samples = 0;
  check("GetData on a building query is not ready", occlusion->GetData(&samples, sizeof(samples), 0) == S_FALSE);
  quad(0, 0, 64, 64, 0.5f);
  occlusion->Issue(D3DISSUE_END);
  check("occlusion counts a full-screen quad", wait(occlusion, samples) == S_OK && samples == 64 * 64);

  occlusion->Issue(D3DISSUE_BEGIN);
  quad(0, 0, 32, 64, 0.25f);
  occlusion->Issue(D3DISSUE_END);
  check("occlusion counts half the screen", wait(occlusion, samples) == S_OK && samples == 32 * 64);

  occlusion->Issue(D3DISSUE_BEGIN);
  quad(0, 0, 64, 64, 0.75f);
  occlusion->Issue(D3DISSUE_END);
  check("occlusion counts nothing behind the depth buffer", wait(occlusion, samples) == S_OK && samples == 0);

  occlusion->Issue(D3DISSUE_BEGIN);
  occlusion->Issue(D3DISSUE_END);
  samples = 1234;
  check("empty occlusion query", wait(occlusion, samples) == S_OK && samples == 0);

  // Wine's test_occlusion_query: END on a fresh query, BEGIN twice, then a draw
  IDirect3DQuery9 *fresh = nullptr;
  device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &fresh);
  fresh->Issue(D3DISSUE_END);
  fresh->Issue(D3DISSUE_BEGIN);
  fresh->Issue(D3DISSUE_BEGIN);
  quad(0, 0, 64, 64, 0.2f);
  fresh->Issue(D3DISSUE_END);
  check("occlusion restarted by a second BEGIN", wait(fresh, samples) == S_OK && samples == 64 * 64);
  fresh->Release();

  // Wine's order: END on a fresh query and two BEGINs outside a scene, then a scene with the draw
  device->EndScene();
  IDirect3DQuery9 *outside = nullptr;
  device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &outside);
  outside->Issue(D3DISSUE_END);
  outside->Issue(D3DISSUE_BEGIN);
  outside->Issue(D3DISSUE_BEGIN);
  device->BeginScene();
  quad(0, 0, 64, 64, 0.15f);
  device->EndScene();
  outside->Issue(D3DISSUE_END);
  samples = 0;
  check("occlusion begun outside a scene", wait(outside, samples) == S_OK && samples == 64 * 64);
  outside->Release();

  // counts add up over many draws, across a flush, and past 32 bits (Wine's test_occlusion_query)
  {
    IDirect3DSurface9 *old_rt = nullptr, *big = nullptr;
    device->GetRenderTarget(0, &old_rt);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    for (UINT size : {1024u, 8192u}) {
      device->CreateRenderTarget(size, size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &big, nullptr);
      device->SetRenderTarget(0, big);
      UINT draws = size == 1024 ? 10 : 65;
      IDirect3DQuery9 *many = nullptr, *flush = nullptr;
      device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &many);
      device->CreateQuery(D3DQUERYTYPE_EVENT, &flush);
      many->Issue(D3DISSUE_BEGIN);
      device->BeginScene();
      for (UINT i = 0; i < draws; i++) {
        quad(0, 0, (float)size, (float)size, 0.5f);
        if (i == draws / 2) {
          flush->Issue(D3DISSUE_END);
          BOOL done = FALSE;
          wait(flush, done);
        }
      }
      device->EndScene();
      many->Issue(D3DISSUE_END);
      UINT64 count = 0;
      HRESULT hr = wait(many, count);
      char name[128];
      snprintf(name, sizeof(name), "occlusion over %u draws of %ux%u (got %llu)", draws, size, size,
               (unsigned long long)count);
      check(name, hr == S_OK && count == (UINT64)draws * size * size);
      many->Release();
      flush->Release();
      device->SetRenderTarget(0, old_rt);
      big->Release();
    }
    old_rt->Release();
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
  }
  device->BeginScene();

  quad(0, 0, 64, 64, 0.1f);
  ts1->Issue(D3DISSUE_END);
  disjoint->Issue(D3DISSUE_END);
  event->Issue(D3DISSUE_END);
  BOOL done = FALSE;
  check("event without flush completes", wait(event, done, 0) == S_OK && done);

  UINT64 t0 = 0, t1 = 0, frequency = 0;
  BOOL is_disjoint = TRUE;
  freq->Issue(D3DISSUE_END);
  check("timestamps", wait(ts0, t0) == S_OK && wait(ts1, t1) == S_OK && t0 && t1 >= t0);
  check("timestamp frequency", wait(freq, frequency) == S_OK && frequency == 1000000000);
  check("timestamps are not disjoint", wait(disjoint, is_disjoint) == S_OK && !is_disjoint);
  device->EndScene();

  event->Release();
  occlusion->Release();
  ts0->Release();
  ts1->Release();
  disjoint->Release();
  freq->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
