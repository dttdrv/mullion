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

D3DDISPLAYMODE g_exclusive_mode{};
UINT g_exclusive_holders = 0;

// a Direct3D 9 identifier's strings are the narrow ones this header has (Wine's
// d3d9.h, as Windows' ANSI build)
void set_ansi(char *destination, size_t count, const WCHAR *value) {
  if (!WideCharToMultiByte(CP_ACP, 0, value ? value : L"", -1, destination, (int)count, nullptr, nullptr))
    destination[0] = 0;
}

// whether a focus-window handle is a window of this session: CreateDevice of a handle that is not a window, and
// of one that was destroyed, is refused (Microsoft Learn, IDirect3D9::CreateDevice; tests/d3d9/d3d9_hwnd.cpp).
// user32 is a module of the process D3D9 runs in front of, so IsWindow is resolved from it where it is loaded,
// and a device of a window is taken when it is not
bool is_window_resolvable(HWND window) {
  static const auto is_window = []() -> WINBOOL(WINAPI *)(HWND) {
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<WINBOOL(WINAPI *)(HWND)>(::GetProcAddress(user32, "IsWindow")) : nullptr;
  }();
  return !is_window || is_window(window) != FALSE;
}

// the largest texture a device of `level` makes: D3D11's required maximum for
// the feature level over the Metal device
UINT max_texture_size(D3D_FEATURE_LEVEL level) {
  return level >= D3D_FEATURE_LEVEL_11_0 ? 16384u : level >= D3D_FEATURE_LEVEL_10_0 ? 8192u : 4096u;
}

// what a device of this build can do, which is the caps below and src/d3d9/README:
// a HAL over this build's D3D11 device, fixed function (two blend stages, eight
// lights, six clip planes, linear table fog) and a game's own shaders through
// vs_3_0 and ps_3_0, 2D, cube and volume textures.
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
                     D3DPTEXTURECAPS_MIPCUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP;
  caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                           D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC |
                           D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR;
  caps.TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                            D3DPTADDRESSCAPS_MIRRORONCE | D3DPTADDRESSCAPS_INDEPENDENTUV;
  // the operations the one stage's pixel shader carries (d3d9_fixed_function_shaders.hpp)
  caps.TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 |
                       D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_SUBTRACT;
  // vs_3_0 reads sixteen vertex streams (Microsoft Learn, D3DCAPS9: MaxStreams is 16 for a device with vs_3_0), and
  // SetStreamSource takes 0..15 (d3d9_device_draw.cpp)
  caps.MaxStreams = 16;
  // the stride a stream may be set with, and the shader-model 3 counts: the translator reads a program of any length
  // D3D9's format allows, and a program executes at least the 65535 instructions the model requires. The instruction
  // slots are advertised at the model's 512 minimum, which the translator exceeds, rather than at a number it cannot
  // honour
  caps.MaxStreamStride = 255;
  caps.MaxVShaderInstructionsExecuted = 65535;
  caps.MaxPShaderInstructionsExecuted = 65535;
  caps.MaxVertexShader30InstructionSlots = 512;
  caps.MaxPixelShader30InstructionSlots = 512;
  caps.PixelShader1xMaxValue = 8.0f;
  caps.StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                     D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
  caps.CubeTextureFilterCaps = caps.TextureFilterCaps;
  caps.VolumeTextureFilterCaps = caps.TextureFilterCaps;
  caps.MaxVolumeExtent = caps.MaxTextureWidth;
  caps.MaxVertexIndex = 0xFFFFFF;
  caps.MaxPrimitiveCount = 0x555555;
  // the fixed-function blender has two stages (the extended pixel shader blends stage 1 after stage 0). MaxSimultaneous-
  // Textures is the programmable stages' count instead: ps_3_0 samples sixteen (s0-s15), which the caps must not
  // understate, so the fixed-function blend count and the shader sampler count are two caps with two values
  caps.MaxTextureBlendStages = 8;
  caps.MaxSimultaneousTextures = 16;
  caps.MaxPointSize = 1.0f;
  // the eight lights D3D9's fixed-function path has, which is the index SetLight, GetLight, LightEnable and
  // GetLightEnable are refused past (d3d9_lights.hpp): the lit vertex shader computes ambient, directional, point (with
  // the documented attenuation and Range) and spot (with its cone) lights
  caps.MaxActiveLights = Lights::kMax;
  // the six user clip planes SetClipPlane takes and D3DRS_CLIPPLANEENABLE enables: each one is a clip distance the
  // vertex shader writes, the fixed-function path's and a game's own alike (d3d9_device_state.hpp)
  caps.MaxUserClipPlanes = 6;
  // ps_3_0 writes oC0 through oC3, and a device with ps_3_0 has four simultaneous render targets (Microsoft Learn,
  // D3DCAPS9: NumSimultaneousRTs)
  caps.NumSimultaneousRTs = 4;
  // D3DRS_COLORWRITEENABLE and 1..3 mask each target on its own (Microsoft Learn, D3DPMISCCAPS_INDEPENDENTWRITEMASKS)
  caps.PrimitiveMiscCaps |= D3DPMISCCAPS_INDEPENDENTWRITEMASKS;
  // the shader models this build's translator certifies: vs_3_0 and ps_3_0 (and every version below). a program of a
  // higher one is refused by CreateVertexShader/CreatePixelShader. the constants are D3D9's 256 float registers a
  // stage. if/else/endif, ifc, loop/rep and call are translated (d3d9_shader_translator.cpp)
  caps.VertexShaderVersion = D3DVS_VERSION(3, 0);
  caps.PixelShaderVersion = D3DPS_VERSION(3, 0);
  caps.MaxVertexShaderConst = 256;
  // four world matrices, non-indexed. MaxVertexBlendMatrixIndex of 0 is the cap's way of saying indexed blending is
  // not offered (Microsoft Learn, D3DCAPS9)
  caps.MaxVertexBlendMatrices = 4;
  caps.MaxVertexBlendMatrixIndex = 0;
  caps.VS20Caps.NumTemps = 12;
  caps.VS20Caps.DynamicFlowControlDepth = 24;
  caps.VS20Caps.StaticFlowControlDepth = 24;
  caps.VS20Caps.Caps = 0;
  caps.PS20Caps.NumTemps = 12;
  caps.PS20Caps.DynamicFlowControlDepth = 24;
  caps.PS20Caps.StaticFlowControlDepth = 24;
  caps.PS20Caps.Caps = 0;
  // StretchRect from and to a texture level is what D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES promises (Microsoft
  // Learn, D3DDEVCAPS2), and device_transfer.cpp implements it: a texture level is a source, and a render-target
  // texture level is a destination, which is the same copy a render target surface's is
  caps.DevCaps2 |= D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;
  return caps;
}

} // namespace

class MTLD3D9Direct3D9 final : private Object9, public IDirect3D9Ex {
public:
  MTLD3D9Direct3D9(ComPtr<IDXGIFactory2> factory, bool extended) : factory_(std::move(factory)), extended_(extended) {
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
    // every object is an extended one underneath: Direct3DCreate9 hands back the IDirect3D9 of it, and
    // Direct3DCreate9Ex asks for the IDirect3D9Ex of the same
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3D9)) {
      *object = static_cast<IDirect3D9 *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IDirect3D9Ex)) {
      // Direct3DCreate9's object is only IDirect3D9; Direct3DCreate9Ex's is the same implementation with this gate open.
      if (!extended_)
        return E_NOINTERFACE;
      *object = static_cast<IDirect3D9Ex *>(this);
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

  // Metal cannot ChangeDisplaySettings; advertising extra modes makes Wine tests call ChangeDisplaySettings and
  // cascade (DISP_CHANGE_BADMODE + focus/message failures). Desktop mode only.
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
    // Always the desktop. Exclusive back-buffer size is reported by IDirect3DDevice9::GetDisplayMode. Sticky
    // exclusive here disagrees with GetMonitorInfo on macOS (panel mode never changes).
    *mode = desktop_mode();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display_format,
                                            D3DFORMAT back_buffer_format, WINBOOL windowed) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    // the display formats a window of this build is shown on, and A2R10G10B10, which is full screen only
    const bool display_known = display_format == D3DFMT_X8R8G8B8 || display_format == D3DFMT_R5G6B5 ||
                               display_format == D3DFMT_X1R5G5B5 || display_format == D3DFMT_A2R10G10B10;
    if (!display_known)
      return D3DERR_NOTAVAILABLE;
    // a windowed chain's D3DFMT_UNKNOWN means the display's format; a full-screen one names its back buffer
    if (!windowed && back_buffer_format == D3DFMT_UNKNOWN)
      return D3DERR_NOTAVAILABLE;
    const D3DFORMAT back_buffer = back_buffer_format == D3DFMT_UNKNOWN ? display_format : back_buffer_format;
    if (to_dxgi_format(back_buffer) == DXGI_FORMAT_UNKNOWN)
      return D3DERR_NOTAVAILABLE;
    if (windowed) {
      if (display_format == D3DFMT_A2R10G10B10)
        return D3DERR_NOTAVAILABLE;
      // a window presents the display's format; its back buffer may be one the display converts from
      if (FAILED(CheckDeviceFormat(
              adapter, type, display_format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, back_buffer
          )))
        return D3DERR_NOTAVAILABLE;
      return CheckDeviceFormatConversion(adapter, type, back_buffer, display_format);
    }
    // nothing changes a display mode here: a full-screen device needs a mode of its display format, and this build has
    // modes for the desktop's X8R8G8B8 only
    if (!GetAdapterModeCount(adapter, display_format))
      return D3DERR_NOTAVAILABLE;
    if (FAILED(CheckDeviceFormat(adapter, type, display_format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, back_buffer)))
      return D3DERR_NOTAVAILABLE;
    // a full-screen back buffer is the display's own format, or differs from it only in the alpha the display ignores
    if (back_buffer == display_format)
      return S_OK;
    if ((display_format == D3DFMT_X8R8G8B8 && back_buffer == D3DFMT_A8R8G8B8) ||
        (display_format == D3DFMT_X1R5G5B5 && back_buffer == D3DFMT_A1R5G5B5))
      return S_OK;
    return D3DERR_NOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage,
                                              D3DRESOURCETYPE resource_type, D3DFORMAT format) override {
    if (adapter >= adapters_.size())
      return D3DERR_INVALIDCALL;
    // REF/SW/NULLREF are accepted for validation; only HAL is available for real work. Unknown adapter format is
    // always INVALIDCALL (Wine device.c test_check_device_format D3DFMT_UNKNOWN).
    if (type != D3DDEVTYPE_HAL && type != D3DDEVTYPE_REF && type != D3DDEVTYPE_SW && type != D3DDEVTYPE_NULLREF)
      return D3DERR_INVALIDCALL;
    if (adapter_format == D3DFMT_UNKNOWN)
      return D3DERR_INVALIDCALL;
    if (type != D3DDEVTYPE_HAL)
      return D3DERR_NOTAVAILABLE;
    // Adapter format must be a display-mode format; A8R8G8B8 is not (Wine device.c CheckDeviceFormat A8R8G8B8→X8).
    const bool adapter_display = adapter_format == D3DFMT_X8R8G8B8 || adapter_format == D3DFMT_R5G6B5 ||
                                 adapter_format == D3DFMT_X1R5G5B5 || adapter_format == D3DFMT_A2R10G10B10;
    if (!adapter_display)
      return D3DERR_NOTAVAILABLE;
    if (resource_type != D3DRTYPE_SURFACE && resource_type != D3DRTYPE_TEXTURE &&
        resource_type != D3DRTYPE_CUBETEXTURE && resource_type != D3DRTYPE_VOLUMETEXTURE &&
        resource_type != D3DRTYPE_VOLUME)
      return D3DERR_NOTAVAILABLE;
    // QUERY_SRGBWRITE on a plain surface (no RENDERTARGET) is unavailable (Wine device.c).
    if ((usage & D3DUSAGE_QUERY_SRGBWRITE) && resource_type == D3DRTYPE_SURFACE && !(usage & D3DUSAGE_RENDERTARGET))
      return D3DERR_NOTAVAILABLE;
    if (usage & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_QUERY_FILTER |
                         D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING |
                         D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_QUERY_WRAPANDMIP))
      return D3DERR_NOTAVAILABLE;
    // DYNAMIC and AUTOGENMIPMAP are mutually exclusive (Wine device.c CheckDeviceFormat).
    if ((usage & D3DUSAGE_DYNAMIC) && (usage & D3DUSAGE_AUTOGENMIPMAP))
      return D3DERR_NOTAVAILABLE;
    if (to_dxgi_format(format) != DXGI_FORMAT_UNKNOWN)
      return S_OK;
    // Depth formats as 2D textures (Wine test_resource_access depth_2d). Cube/plain depth create paths are not
    // wired yet; advertising them makes Wine expect creates this build refuses. Classic D32 is not offered.
    if (format == D3DFMT_D32)
      return D3DERR_NOTAVAILABLE;
    if (to_dxgi_depth_format(format) != DXGI_FORMAT_UNKNOWN && resource_type == D3DRTYPE_TEXTURE) {
      // Depth textures do not autogenerate mips (Wine device.c D3DFMT_D16 + AUTOGENMIPMAP → D3DOK_NOAUTOGEN).
      if (usage & D3DUSAGE_AUTOGENMIPMAP)
        return D3DOK_NOAUTOGEN;
      return S_OK;
    }
    return D3DERR_NOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, WINBOOL,
                                                       D3DMULTISAMPLE_TYPE samples, DWORD *quality_levels) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    // D3DFMT_UNKNOWN is refused; other unknown ordinals succeed for NONE (Wine device.c test_check_device_multisample_type).
    if (format == D3DFMT_UNKNOWN)
      return D3DERR_INVALIDCALL;
    // Sample counts above D3DMULTISAMPLE_16_SAMPLES are INVALIDCALL, not NOTAVAILABLE.
    if ((UINT)samples > 16)
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
    if (FAILED(probes_[adapter]->CheckMultisampleQualityLevels(dxgi, (UINT)samples, &quality)) || !quality) {
      // Unsupported sample counts still report one quality level (Wine device.c MULTISAMPLE_15_SAMPLES).
      if (quality_levels)
        *quality_levels = 1;
      return D3DERR_NOTAVAILABLE;
    }
    if (quality_levels)
      *quality_levels = quality;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT, D3DFORMAT render_target,
                                                   D3DFORMAT depth_stencil) override {
    if (adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    // Classic D3DFMT_D32 is not a supported depth-stencil on this path (Wine device.c expects NOTAVAILABLE).
    if (depth_stencil == D3DFMT_D32)
      return D3DERR_NOTAVAILABLE;
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
    // a NULL focus window still makes a device (Wine d3d9ex.c test_desktop_window asks for one): create_device
    // presents through a private window of its own in that case
    if (window && !is_window_resolvable(window))
      return D3DERR_INVALIDCALL;

    D3D_FEATURE_LEVEL level;
    HRESULT hr = level_of(adapter, level);
    if (FAILED(hr))
      return D3DERR_NOTAVAILABLE;

    return create_device(static_cast<IDirect3D9 *>(this), factory_.Get(), adapters_[adapter].Get(), adapter,
                         caps_of(adapter, level), window, behavior_flags, *present, device, extended_);
  }

  // IDirect3D9Ex: the modes are the desktop's, as the non-Ex calls above have them, in the Ex structure
  UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER *filter) override {
    if (adapter >= adapters_.size() || (filter && filter->Format != D3DFMT_X8R8G8B8))
      return 0;
    return 1;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER *filter, UINT mode,
                                               D3DDISPLAYMODEEX *result) override {
    if (adapter >= adapters_.size() || (filter && filter->Format != D3DFMT_X8R8G8B8) || mode != 0 || !result)
      return D3DERR_INVALIDCALL;
    *result = desktop_mode_ex();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX *mode,
                                                    D3DDISPLAYROTATION *rotation) override {
    if (adapter >= adapters_.size() || !mode)
      return D3DERR_INVALIDCALL;
    *mode = desktop_mode_ex();
    if (rotation)
      *rotation = D3DDISPLAYROTATION_IDENTITY;
    return S_OK;
  }

  // the device is the one CreateDevice makes, asked for again as IDirect3DDevice9Ex; the display mode is the
  // desktop's, which a window's chain presents into
  HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior_flags,
                                           D3DPRESENT_PARAMETERS *present, D3DDISPLAYMODEEX *,
                                           IDirect3DDevice9Ex **device) override {
    if (!device)
      return D3DERR_INVALIDCALL;
    // a NULL focus window still makes a device (d3d9ex.c test_desktop_window's last case)
    if (window && !is_window_resolvable(window))
      return D3DERR_INVALIDCALL;
    *device = nullptr;

    if (!present || adapter >= adapters_.size() || type != D3DDEVTYPE_HAL)
      return D3DERR_INVALIDCALL;
    D3D_FEATURE_LEVEL level;
    if (HRESULT hr = level_of(adapter, level); FAILED(hr))
      return D3DERR_NOTAVAILABLE;
    ComPtr<IDirect3DDevice9> created;
    if (HRESULT hr = create_device(static_cast<IDirect3D9 *>(this), factory_.Get(), adapters_[adapter].Get(), adapter,
                                   caps_of(adapter, level), window, behavior_flags, *present, &created, true);
        FAILED(hr))
      return hr;
    return created->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(device));
  }

  HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT adapter, LUID *luid) override {
    if (!luid || adapter >= adapters_.size())
      return D3DERR_INVALIDCALL;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapters_[adapter]->GetDesc1(&description)))
      return D3DERR_INVALIDCALL;
    *luid = description.AdapterLuid;
    return S_OK;
  }

private:
  // the feature level this build's D3D11 device over the adapter has, which its
  // caps come from: asked once per adapter, since the device a probe is costs a
  // Metal device
  HRESULT level_of(UINT adapter, D3D_FEATURE_LEVEL &level) {
    if (adapter >= adapters_.size())
      return D3DERR_INVALIDCALL;
    if (!probes_[adapter]) {
      // this build's own layer, not the application: the probe is how Direct3D 9 finds the device it draws over,
      // and the internal bit says that the router has no decision to make here (d3d11.cpp)
      HRESULT hr = D3D11CoreCreateDevice(factory_.Get(), adapters_[adapter].Get(),
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                             0x40000000 /* MULLION_INTERNAL_DEVICE */,
                                         nullptr, 0, &probes_[adapter]);
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

  D3DDISPLAYMODEEX desktop_mode_ex() const {
    const D3DDISPLAYMODE mode = desktop_mode();
    D3DDISPLAYMODEEX result{};
    result.Size = sizeof(D3DDISPLAYMODEEX);
    result.Width = mode.Width;
    result.Height = mode.Height;
    result.RefreshRate = mode.RefreshRate;
    result.Format = mode.Format;
    result.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    return result;
  }

  ComPtr<IDXGIFactory2> factory_;
  // whether Direct3DCreate9Ex made the object: every object answers IDirect3D9Ex, so it is the maker's, not a gate
  [[maybe_unused]] bool extended_ = false;
  std::vector<ComPtr<IDXGIAdapter1>> adapters_;
  // the D3D11 device of each adapter's caps: one probe for the whole object,
  // not one per question
  std::vector<ComPtr<ID3D11Device>> probes_;
};

void d3d9_enter_exclusive_mode(IDirect3D9 *, UINT width, UINT height) {
  g_exclusive_mode.Width = width;
  g_exclusive_mode.Height = height;
  g_exclusive_mode.RefreshRate = 60;
  g_exclusive_mode.Format = D3DFMT_X8R8G8B8;
  g_exclusive_holders++;
}

void d3d9_leave_exclusive_mode(IDirect3D9 *) {
  if (g_exclusive_holders)
    g_exclusive_holders--;
  if (!g_exclusive_holders)
    g_exclusive_mode = {};
}

IDirect3D9 *create_direct3d(bool extended) {
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
  result.Attach(new MTLD3D9Direct3D9(factory2, extended));
  if (!result->GetAdapterCount())
    ERR("D3D9: DXGI has no adapters, so this machine presents nothing.");
  return result.Detach();
}

} // namespace mullion::d3d9
