// the Direct3D 9 object: DXGI's adapters, which are this build's Metal devices,
// as D3D9's adapters. the caps of an adapter are the ones its D3D11 device has,
// so a device this build makes and a device a game asks about agree; the
// texture size is Metal's own limit as the D3D11 feature level over it carries
// it.
#include "d3d9_direct3d.hpp"
#include "d3d9_lights.hpp"
#include "d3d9_device.hpp"
#include <cwchar>

namespace mullion::d3d9 {

namespace {

// a Direct3D 9 identifier's strings are the narrow ones this header has (Wine's
// d3d9.h, as Windows' ANSI build)
void set_ansi(char *destination, size_t count, const WCHAR *value) {
  if (!WideCharToMultiByte(CP_ACP, 0, value ? value : L"", -1, destination, (int)count, nullptr, nullptr))
    destination[0] = 0;
}

// the largest texture a device of `level` makes: D3D11's required maximum for
// the feature level over the Metal device
UINT max_texture_size(D3D_FEATURE_LEVEL level) {
  return level >= D3D_FEATURE_LEVEL_11_0 ? 16384u : level >= D3D_FEATURE_LEVEL_10_0 ? 8192u : 4096u;
}

// what a device of this build can do: be a HAL, present to a window, move a
// frame's bytes between its back buffer and the CPU's memory, draw with the
// fixed-function pipeline's pre-transformed vertices under the render states
// and into the targets it has, sample one texture stage, and light a vertex with D3D9's eight lights and its material
// (it has no game's own shaders, no fog and no cube or volume texture: src/d3d9/README)
D3DCAPS9
caps_of(UINT adapter, D3D_FEATURE_LEVEL level) {
  D3DCAPS9 caps{};
  caps.DeviceType = D3DDEVTYPE_HAL;
  caps.AdapterOrdinal = adapter;
  caps.PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
  caps.MaxTextureWidth = max_texture_size(level);
  caps.MaxTextureHeight = caps.MaxTextureWidth;
  caps.MaxTextureRepeat = 65536;
  // any texture whose dimensions are inside the two maxima above is made, so the
  // ratio of one to the other is bounded by those maxima
  caps.MaxTextureAspectRatio = caps.MaxTextureWidth;
  caps.MaxAnisotropy = 16;
  caps.RasterCaps = D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_ZFOG;
  // a texture holds an alpha channel, and a texture of this build has mip levels: the ones the game authored, or the
  // ones GenerateMipSubLevels makes out of the level above when the format supports D3D11's generator
  // (d3d9_device_texture.cpp, d3d9_texture.cpp)
  caps.TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_CUBEMAP |
                     D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP;
  caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                           D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC |
                           D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR;
  caps.TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                            D3DPTADDRESSCAPS_MIRRORONCE | D3DPTADDRESSCAPS_INDEPENDENTUV;
  // the operations the one stage's pixel shader carries (d3d9_fixed_function_shaders.hpp)
  caps.TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 |
                       D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_SUBTRACT;
  caps.MaxStreams = 4; // SetStreamSource 0..3 (d3d9_device_draw.cpp)
  caps.StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                     D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
  caps.CubeTextureFilterCaps = caps.TextureFilterCaps;
  caps.VolumeTextureFilterCaps = caps.TextureFilterCaps;
  caps.MaxVolumeExtent = caps.MaxTextureWidth;
  caps.MaxVertexIndex = 0xFFFFFF;
  caps.MaxPrimitiveCount = 0x555555;
  // the fixed-function blender has two stages (the extended pixel shader blends stage 1 after stage 0); a game's own
  // shader samples eight textures, one an interface's stage with its own sampler
  caps.MaxTextureBlendStages = 2;
  caps.MaxSimultaneousTextures = 8;
  caps.MaxPointSize = 1.0f;
  // the eight lights D3D9's fixed-function path has, which is the index SetLight, GetLight, LightEnable and
  // GetLightEnable are refused past (d3d9_lights.hpp): the lit vertex shader computes ambient, directional, point (with
  // the documented attenuation and Range) and spot (with its cone) lights
  caps.MaxActiveLights = Lights::kMax;
  caps.NumSimultaneousRTs = 1;
  // the shader models this build's translator certifies: vs_3_0 and ps_3_0 (and every version below). a program of a
  // higher one is refused by CreateVertexShader/CreatePixelShader. the constants are D3D9's 256 float registers a
  // stage. if/else/endif, ifc, loop/rep and call are translated (d3d9_shader_translator.cpp)
  caps.VertexShaderVersion = D3DVS_VERSION(3, 0);
  caps.PixelShaderVersion = D3DPS_VERSION(3, 0);
  caps.MaxVertexShaderConst = 256;
  caps.VS20Caps.NumTemps = 12;
  caps.VS20Caps.DynamicFlowControlDepth = 24;
  caps.VS20Caps.StaticFlowControlDepth = 24;
  caps.VS20Caps.Caps = 0;
  caps.PS20Caps.NumTemps = 12;
  caps.PS20Caps.DynamicFlowControlDepth = 24;
  caps.PS20Caps.StaticFlowControlDepth = 24;
  caps.PS20Caps.Caps = 0;
  return caps;
}

} // namespace

class MTLD3D9Direct3D9 final : private Object9, public IDirect3D9 {
public:
  explicit MTLD3D9Direct3D9(ComPtr<IDXGIFactory2> factory) : factory_(std::move(factory)) {
    for (UINT index = 0;; index++) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory_->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
        break;
      adapters_.push_back(adapter);
      probes_.emplace_back(nullptr);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3D9)) {
      *object = static_cast<IDirect3D9 *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *) override { return E_NOTIMPL; }

  UINT STDMETHODCALLTYPE GetAdapterCount() override { return (UINT)adapters_.size(); }

  // the identity DXGI has of the adapter, which is the Metal device's: its
  // name, and the vendor and device ids the Metal bridge reports for it
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER9 *identifier) override {
    if (!identifier || adapter >= adapters_.size())
      return D3DERR_INVALIDCALL;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapters_[adapter]->GetDesc1(&description)))
      return D3DERR_INVALIDCALL;

    *identifier = {};
    set_ansi(identifier->Description, ARRAYSIZE(identifier->Description), description.Description);
    set_ansi(identifier->DeviceName, ARRAYSIZE(identifier->DeviceName), L"Mullion Metal Device");
    set_ansi(identifier->Driver, ARRAYSIZE(identifier->Driver), L"Mullion");
    identifier->DriverVersion.QuadPart = 0x000A0000; // 10.0: this build's own, not a driver's
    identifier->VendorId = description.VendorId;
    identifier->DeviceId = description.DeviceId;
    identifier->SubSysId = description.SubSysId;
    identifier->Revision = description.Revision;
    // a device's identity, from the one the Metal bridge reports: not Windows'
    // GUID for a real card, since there is no card, but the same every run of
    // this build on the same Metal device
    identifier->DeviceIdentifier.Data1 = description.VendorId;
    identifier->DeviceIdentifier.Data2 = (WORD)description.DeviceId;
    identifier->DeviceIdentifier.Data3 = (WORD)(description.DeviceId >> 16);
    identifier->WHQLLevel = 0;
    return S_OK;
  }

  // the modes of a display this build shows a window on: the desktop's, since
  // nothing changes a display mode yet
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override {
    if (adapter >= adapters_.size() || format != D3DFMT_X8R8G8B8)
      return 0;
    return 1;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode,
                                             D3DDISPLAYMODE *result) override {
    if (adapter >= adapters_.size() || format != D3DFMT_X8R8G8B8 || mode != 0 || !result)
      return D3DERR_INVALIDCALL;
    *result = desktop_mode();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *mode) override {
    if (adapter >= adapters_.size() || !mode)
      return D3DERR_INVALIDCALL;
    *mode = desktop_mode();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display_format,
                                            D3DFORMAT back_buffer_format, WINBOOL windowed) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    if (to_dxgi_format(display_format) == DXGI_FORMAT_UNKNOWN ||
        to_dxgi_format(back_buffer_format) == DXGI_FORMAT_UNKNOWN)
      return D3DERR_NOTAVAILABLE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT, DWORD usage,
                                              D3DRESOURCETYPE resource_type, D3DFORMAT format) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    if (resource_type != D3DRTYPE_SURFACE && resource_type != D3DRTYPE_TEXTURE &&
        resource_type != D3DRTYPE_CUBETEXTURE && resource_type != D3DRTYPE_VOLUMETEXTURE &&
        resource_type != D3DRTYPE_VOLUME)
      return D3DERR_NOTAVAILABLE;
    if (usage & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC | D3DUSAGE_QUERY_FILTER |
                         D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING))
      return D3DERR_NOTAVAILABLE;
    return to_dxgi_format(format) == DXGI_FORMAT_UNKNOWN ? D3DERR_NOTAVAILABLE : S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, WINBOOL,
                                                       D3DMULTISAMPLE_TYPE samples, DWORD *quality_levels) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    if (samples == D3DMULTISAMPLE_NONE) {
      if (quality_levels)
        *quality_levels = 1;
      return S_OK;
    }
    D3D_FEATURE_LEVEL level;
    if (FAILED(level_of(adapter, level)))
      return D3DERR_NOTAVAILABLE;
    const DXGI_FORMAT dxgi = to_dxgi_format(format) != DXGI_FORMAT_UNKNOWN ? to_dxgi_format(format)
                                                                          : to_dxgi_depth_format(format);
    if (dxgi == DXGI_FORMAT_UNKNOWN)
      return D3DERR_NOTAVAILABLE;
    UINT quality = 0;
    if (FAILED(probes_[adapter]->CheckMultisampleQualityLevels(dxgi, (UINT)samples, &quality)) || !quality)
      return D3DERR_NOTAVAILABLE;
    if (quality_levels)
      *quality_levels = quality;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT, D3DFORMAT render_target,
                                                   D3DFORMAT depth_stencil) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    if (to_dxgi_format(render_target) == DXGI_FORMAT_UNKNOWN ||
        to_dxgi_depth_format(depth_stencil) == DXGI_FORMAT_UNKNOWN)
      return D3DERR_NOTAVAILABLE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source,
                                                        D3DFORMAT target) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    if (to_dxgi_format(source) == DXGI_FORMAT_UNKNOWN || to_dxgi_format(source) != to_dxgi_format(target))
      return D3DERR_NOTAVAILABLE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) override {
    if (!caps || adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = level_of(adapter, level);
    if (FAILED(hr))
      return D3DERR_NOTAVAILABLE;
    *caps = caps_of(adapter, level);
    return S_OK;
  }

  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
    if (adapter >= adapters_.size())
      return nullptr;
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior_flags,
                                         D3DPRESENT_PARAMETERS *present, IDirect3DDevice9 **device) override {
    if (!present || !device || adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;

    D3D_FEATURE_LEVEL level;
    HRESULT hr = level_of(adapter, level);
    if (FAILED(hr))
      return D3DERR_NOTAVAILABLE;

    return create_device(static_cast<IDirect3D9 *>(this), factory_.Get(), adapters_[adapter].Get(), adapter,
                         caps_of(adapter, level), window, behavior_flags, *present, device);
  }

private:
  // the feature level this build's D3D11 device over the adapter has, which its
  // caps come from: asked once per adapter, since the device a probe is costs a
  // Metal device
  HRESULT level_of(UINT adapter, D3D_FEATURE_LEVEL &level) {
    if (adapter >= adapters_.size())
      return D3DERR_INVALIDCALL;
    if (!probes_[adapter]) {
      HRESULT hr = D3D11CreateDevice(adapters_[adapter].Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &probes_[adapter],
                                     nullptr, nullptr);
      if (FAILED(hr)) {
        ERR("D3D9: no D3D11 device over adapter ", adapter, ": 0x", std::hex, hr);
        return hr;
      }
    }
    level = probes_[adapter]->GetFeatureLevel();
    return S_OK;
  }

  D3DDISPLAYMODE desktop_mode() const {
    D3DDISPLAYMODE mode{};
    mode.Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
    mode.Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
    mode.RefreshRate = 60;
    mode.Format = D3DFMT_X8R8G8B8;
    return mode;
  }

  ComPtr<IDXGIFactory2> factory_;
  std::vector<ComPtr<IDXGIAdapter1>> adapters_;
  // the D3D11 device of each adapter's caps: one probe for the whole object,
  // not one per question
  std::vector<ComPtr<ID3D11Device>> probes_;
};

IDirect3D9 *create_direct3d() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    ERR("D3D9: no DXGI factory, so no adapters.");
    return nullptr;
  }
  ComPtr<IDXGIFactory2> factory2;
  if (FAILED(factory.As(&factory2))) {
    ERR("D3D9: DXGI has no factory 2, which a window's chain is made by.");
    return nullptr;
  }
  ComPtr<MTLD3D9Direct3D9> result;
  result.Attach(new MTLD3D9Direct3D9(factory2));
  if (!result->GetAdapterCount())
    ERR("D3D9: DXGI has no adapters, so this machine presents nothing.");
  return result.Detach();
}

} // namespace mullion::d3d9
