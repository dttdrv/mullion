// the Direct3D 9 device this build makes: its interfaces and everything a frame reads. the device's source is split by
// what a frame needs -- d3d9_device.cpp has the object, its chain, its back buffer and the surfaces a frame is read
// back into, d3d9_device_targets.cpp the render targets, depth-stencil surfaces, viewport and scissor rectangle and the
// render-state and transform entry points, d3d9_device_draw.cpp the draws and what they are made of, and
// d3d9_device_texture.cpp the textures a draw samples with the stage that samples them. The state those draws read is
// not a member of this class any more but one object of its own (d3d9_device_state.hpp), and the direct3d 11 objects a
// fixed-function draw binds are the pipeline's (d3d9_fixed_function.hpp). nothing else of this build sees the class, only
// the interfaces it has.
#pragma once
#include "d3d9_buffer.hpp"
#include "d3d9_common.hpp"
#include "d3d9_device_state.hpp"
#include "d3d9_fixed_function.hpp"
#include "d3d9_query.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_lights.hpp"
#include "d3d9_state.hpp"
#include "d3d9_state_block.hpp"
#include "d3d9_vertex_declaration.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include "d3d9_surface.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_texture.hpp"

namespace mullion::d3d9 {

// the order two vertex declarations compare in, so the device can keep one declaration-shaped passthrough per
// declaration (D3DVERTEXELEMENT9 has no operator<)
struct DeclarationElementsLess {
  bool operator()(const std::vector<D3DVERTEXELEMENT9> &a, const std::vector<D3DVERTEXELEMENT9> &b) const {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(), [](const D3DVERTEXELEMENT9 &one, const D3DVERTEXELEMENT9 &other) {
          if (one.Stream != other.Stream)
            return one.Stream < other.Stream;
          if (one.Offset != other.Offset)
            return one.Offset < other.Offset;
          if (one.Type != other.Type)
            return one.Type < other.Type;
          if (one.Method != other.Method)
            return one.Method < other.Method;
          if (one.Usage != other.Usage)
            return one.Usage < other.Usage;
          return one.UsageIndex < other.UsageIndex;
        });
  }
};

class MTLD3D9Device final : private Object9,
                          public IDirect3DDevice9Ex,
                          public IMTLD3D9PresentSource,
                          public IMTLD3D9DeviceLifetime,
                          public IMTLD3D9DeviceResources {
public:
  // the texture stages, and so the samplers, a draw samples: ps_3_0's sixteen (s0 through s15), which is the caps'
  // MaxSimultaneousTextures. The fixed-function blender's stage count is a separate cap (MaxTextureBlendStages): a
  // ps_3_0 program samples textures its fixed-function predecessor never had
  static constexpr UINT kTextureStages = 16;
  // the vertex texture samplers: the API's D3DVERTEXTEXTURESAMPLER0..3 are 257..260 (d3d9types.h), and sample
  // through the vertex shader's s0..s3. They are a namespace of their own: a vertex shader's s0 is not the pixel
  // shader's s0. Internally they occupy compact slots 16..19, and every API entry point translates the public id
  static constexpr UINT kVertexTextureStages = 4;
  static constexpr UINT kTotalTextureStages = kTextureStages + kVertexTextureStages;
  // the fixed-function blender's stages (the caps' MaxTextureBlendStages): D3D9 advertises eight and Wine's device
  // test writes texture-stage state on stages 0..7; the Metal combiner path still honours the operations it supports
  static constexpr UINT kMaxBlendStages = 8;
  // the vertex streams a shader declaration may read (the caps' MaxStreams): vs_3_0 reads sixteen
  static constexpr UINT kMaxStreams = 16;
  // the render targets a draw writes (the caps' NumSimultaneousRTs): ps_3_0 writes oC0 through oC3
  static constexpr UINT kMaxRenderTargets = 4;

  MTLD3D9Device(IDirect3D9 *direct3d, ComPtr<IDXGIAdapter1> adapter, UINT adapter_ordinal, const D3DCAPS9 &caps,
                HWND window, DWORD behavior_flags, const D3DPRESENT_PARAMETERS &present, bool extended = false)
      : direct3d_(direct3d), extended_(extended), adapter_(std::move(adapter)), adapter_ordinal_(adapter_ordinal),
        caps_(caps), window_(window), behavior_flags_(behavior_flags), present_(present) {}

  ~MTLD3D9Device() override {
    invalidate_back_buffer();
    if (owns_swapchain_window_ && swapchain_window_ && swapchain_window_ != window_) {
      DestroyWindow(swapchain_window_);
      swapchain_window_ = nullptr;
      owns_swapchain_window_ = false;
    }
  }

  HRESULT init(IDXGIFactory2 *factory);
  // Focus HWND created for D3D9Ex NULL-window CreateDeviceEx: owned and destroyed with the device.
  void adopt_swapchain_window(HWND window) {
    swapchain_window_ = window;
    owns_swapchain_window_ = true;
    if (!window_)
      window_ = window;
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3DDevice9)) {
      *object = static_cast<IDirect3DDevice9 *>(this);
    } else if (riid == __uuidof(IDirect3DDevice9Ex) && extended_) {
      *object = static_cast<IDirect3DDevice9Ex *>(this);
    } else if (riid == IID_IMTLD3D9PresentSource) {
      *object = static_cast<IMTLD3D9PresentSource *>(this);
    } else if (riid == IID_IMTLD3D9DeviceLifetime) {
      // a child's reference, not the application's: it keeps the object alive but does not make the device usable again
      child_refs_++;
      *object = static_cast<IMTLD3D9DeviceLifetime *>(this);
      return S_OK;
    } else if (riid == IID_IMTLD3D9DeviceResources) {
      // a registered resource's reference, which is a child's as well (DeviceResourceRef releases it through
      // release_device_ref)
      child_refs_++;
      *object = static_cast<IMTLD3D9DeviceResources *>(this);
      return S_OK;
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++app_refs_; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG remaining = app_refs_.load();
    while (remaining) {
      if (app_refs_.compare_exchange_weak(remaining, remaining - 1))
        break;
    }
    if (!remaining)
      return 0;
    remaining--;
    if (!remaining) {
      if (!torn_down_.exchange(true)) {
        // a temporary child reference keeps the object alive while its children let go of theirs
        child_refs_++;
        release_device_children();
        if (--child_refs_ == 0)
          delete this;
      } else if (!child_refs_.load()) {
        delete this;
      }
    }
    return remaining;
  }

  // IMTLD3D9DeviceLifetime (and IMTLD3D9DeviceResources): the private reference QueryInterface added
  void release_device_ref() override {
    if (--child_refs_ == 0 && !app_refs_.load())
      delete this;
  }

  // IMTLD3D9DeviceResources: the resources a Reset rebuilds on the device it makes, which is the managed pool
  void track_reset_target(IMTLD3D9ResetTarget *target) override {
    reset_targets_.push_back(target);
  }
  void untrack_reset_target(IMTLD3D9ResetTarget *target) override {
    reset_targets_.erase(std::remove(reset_targets_.begin(), reset_targets_.end(), target), reset_targets_.end());
  }
  void unbind_buffer(IMTLD3D9Buffer *buffer) override {
    if (!buffer)
      return;
    for (UINT stream = 0; stream < kMaxStreams; stream++) {
      if (streams_[stream].buffer == buffer)
        release_stream(stream);
    }
    if (index_buffer_ == buffer)
      release_indices();
  }
  void unbind_surface(IMTLD3D9Surface *surface) override;
  void unbind_texture(IMTLD3D9Texture *texture) override;

  // IMTLD3D9PresentSource
  HRESULT present(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty, DWORD flags) override;
  HRESULT back_buffer(UINT index, IDirect3DSurface9 **surface) override;
  HRESULT display_mode(D3DDISPLAYMODE *mode) override;
  // the chain `index` names: 0 is the device's own and the additional ones follow it (d3d9_device.cpp)
  IDirect3DSwapChain9 *swap_chain_at(UINT index);
  HRESULT display_mode_at(UINT swap_chain, D3DDISPLAYMODE *mode);
  HRESULT front_buffer_data(IDirect3DSurface9 *destination) override;

  // the device's own
  //
  // D3D9's answer for a device whose frames the GPU failed: lost, and it stays lost until the application resets it.
  // A lost device is one this build can reset (Reset makes a new Direct3D 11 device), so the level is the resettable
  // one (D3DERR_DEVICENOTRESET); the calls that draw answer D3DERR_DEVICELOST until it is reset (Microsoft Learn,
  // IDirect3DDevice9::TestCooperativeLevel)
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;

  // what the Metal device has left of the working set it recommends: the textures this device allocates live there, so
  // it is the answer D3D9's own drivers give for the same question
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;

  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **direct3d) override {
    if (!direct3d)
      return D3DERR_INVALIDCALL;
    *direct3d = direct3d_.Get();
    if (*direct3d)
      (*direct3d)->AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *caps) override {
    if (!caps)
      return D3DERR_INVALIDCALL;
    *caps = caps_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT swap_chain, D3DDISPLAYMODE *mode) override {
    return display_mode_at(swap_chain, mode);
  }

  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *parameters) override {
    if (!parameters)
      return D3DERR_INVALIDCALL;
    parameters->AdapterOrdinal = adapter_ordinal_;
    parameters->DeviceType = D3DDEVTYPE_HAL;
    parameters->hFocusWindow = window_;
    parameters->BehaviorFlags = behavior_flags_;
    return S_OK;
  }

  // the implicit chain is 0 and the ones CreateAdditionalSwapChain made follow it, in the order they were made
  // (Microsoft Learn, IDirect3DDevice9::GetSwapChain)
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT swap_chain, IDirect3DSwapChain9 **result) override {
    if (!result)
      return D3DERR_INVALIDCALL;
    *result = nullptr;
    // only the device's own chain: D3D9's GetSwapChain does not return a chain made with CreateAdditionalSwapChain,
    // even though that chain belongs to the device (Wine's device test asks for index 1 after making chains and takes
    // D3DERR_INVALIDCALL with a NULL result)
    if (swap_chain != 0 || !swap_chain_)
      return D3DERR_INVALIDCALL;
    *result = static_cast<IDirect3DSwapChain9 *>(swap_chain_);
    (*result)->AddRef();
    return S_OK;
  }

  // the implicit chains are the device's: a chain made with CreateAdditionalSwapChain is not one of them, so it is not
  // counted here (Microsoft Learn, IDirect3DDevice9::GetNumberOfSwapChains)
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return swap_chain_ ? 1 : 0; }

  // the device's windowed chain, sized and formatted again: the state is lost and the back buffer is the new size, so
  // a game that changes resolution or alt-tabs draws its next frame at the new size. A device whose frames failed is
  // made again here, which is the road back from D3DERR_DEVICENOTRESET: the new chain is over a new Direct3D 11 device,
  // the default pool is gone with the old one, and the managed pool's contents are put onto the new one. Windowed FALSE
  // takes the DXGI chain fullscreen and TRUE brings it back (src/d3d9/README).
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *present_parameters) override;

  HRESULT STDMETHODCALLTYPE Present(const RECT *source, const RECT *destination, HWND window,
                                    const RGNDATA *dirty) override {
    return present(source, destination, window, dirty, 0);
  }

  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT swap_chain, UINT back_buffer, D3DBACKBUFFER_TYPE,
                                          IDirect3DSurface9 **result) override {
    // the type is ignored (Wine's device test asks for LEFT and RIGHT and takes the same surface), and a failed call
    // leaves the caller's pointer NULL, as D3D9's does
    if (!result)
      return D3DERR_INVALIDCALL;
    *result = nullptr;
    // only the implicit chain's buffers: an additional chain's are the chain's to hand out, not the device's
    if (swap_chain != 0)
      return D3DERR_INVALIDCALL;
    return this->back_buffer(back_buffer, result);
  }

  // the display's scan line and blank: counted from when the last refresh was due, since no Mac API reports the
  // raster's position (d3d9_device.cpp)
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swap_chain, D3DRASTER_STATUS *status) override;

  // a scene is the device's frames until EndScene: nothing is drawn between
  // them yet, so the device only notes them
  HRESULT STDMETHODCALLTYPE BeginScene() override {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
    if (in_scene_)
      return D3DERR_INVALIDCALL;
    in_scene_ = true;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EndScene() override {
    if (!in_scene_)
      return D3DERR_INVALIDCALL;
    in_scene_ = false;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float depth,
                                  DWORD stencil) override;

  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) override;

  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *render_target,
                                                IDirect3DSurface9 *destination) override;

  // a surface whose bytes are the CPU's, which a frame is read back into:
  // D3DPOOL_SYSTEMMEM and nothing else yet
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                        IDirect3DSurface9 **surface, HANDLE *shared_handle) override;

  // the frame the chain showed last, copied into a system-memory surface (d3d9_device.cpp)
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT swap_chain, IDirect3DSurface9 *destination) override {
    IDirect3DSwapChain9 *chain = swap_chain_at(swap_chain);
    if (!chain)
      return D3DERR_INVALIDCALL;
    return chain->GetFrontBufferData(destination);
  }

  // the argument checks of a surface read back into or out of: ours, and of the
  // pool the copy needs
  HRESULT source_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out);
  HRESULT staging_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out);

  // managed resources stay in Metal memory for the life of the device
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot,
                                                IDirect3DSurface9 *cursor_bitmap) override;
  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override;
  WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL bShow) override;
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                                                      IDirect3DSwapChain9 **pSwapChain) override;
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(WINBOOL bEnableDialogs) override { return D3DERR_INVALIDCALL; }
  void STDMETHODCALLTYPE SetGammaRamp(UINT swapchain_idx, DWORD flags, const D3DGAMMARAMP *ramp) override {}
  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) override {}
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                          D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                D3DFORMAT Format, D3DPOOL Pool,
                                                IDirect3DVolumeTexture9 **ppVolumeTexture,
                                                HANDLE *pSharedHandle) override;
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                              IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override;
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                               IDirect3DVertexBuffer9 **vertex_buffer,
                                               HANDLE *shared_handle) override;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                              IDirect3DIndexBuffer9 **index_buffer, HANDLE *shared_handle) override;
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *src_surface, const RECT *src_rect,
                                          IDirect3DSurface9 *dst_surface, const POINT *dst_point) override;
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                                          IDirect3DBaseTexture9 *pDestinationTexture) override;
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *src_surface, const RECT *src_rect,
                                        IDirect3DSurface9 *dst_surface, const RECT *dst_rect,
                                        D3DTEXTUREFILTERTYPE filter) override;
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override;
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                               D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                               WINBOOL Lockable, IDirect3DSurface9 **ppSurface,
                                               HANDLE *pSharedHandle) override;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                                      WINBOOL Discard, IDirect3DSurface9 **ppSurface,
                                                      HANDLE *pSharedHandle) override;
  HRESULT create_depth_stencil_surface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                       DWORD quality, WINBOOL discard, DWORD extra_usage,
                                       IDirect3DSurface9 **surface, HANDLE *shared_handle, bool implicit = false);
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD render_target, IDirect3DSurface9 *surface) override;
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *surface) override;
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **surface) override;
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override;
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix) override;
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override;
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *viewport) override;
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *viewport) override;
  // the lights and the material are the device state's (d3d9_device_state.hpp): these calls are the interface over it
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *material) override {
    const HRESULT hr = state_.lights().set_material(material);
    if (SUCCEEDED(hr) && recording_)
      record(StateKey{StateKey::Material, 0, 0});
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override {
    return state_.lights().get_material(pMaterial);
  }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9 *light) override {
    const HRESULT hr = state_.lights().set_light(index, light);
    if (SUCCEEDED(hr) && recording_)
      record(StateKey{StateKey::Light, index, 0});
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *light) override {
    return state_.lights().get_light(Index, light);
  }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, WINBOOL Enable) override {
    const HRESULT hr = state_.lights().enable_light(Index, Enable);
    if (SUCCEEDED(hr) && recording_)
      record(StateKey{StateKey::LightEnable, Index, 0});
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, WINBOOL *pEnable) override {
    return state_.lights().get_light_enable(Index, pEnable);
  }
  // the six user clip planes the caps report (MaxUserClipPlanes): a draw is cut by the ones D3DRS_CLIPPLANEENABLE
  // names, in world space for the fixed-function path and in clip space for a vertex shader's (d3d9_device_state.hpp)
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override {
    if (!plane)
      return D3DERR_INVALIDCALL;
    // Wine d3d9 device.c clamps an index past the device's MaxUserClipPlanes to the last plane instead of
    // refusing it: every SetClipPlane lands on the last slot the caps offer (winetest test_user_clip_planes
    // reads the last-set plane back through higher indices)
    state_.set_clip_plane(index < DeviceState::kClipPlanes ? index : DeviceState::kClipPlanes - 1, plane);
    if (recording_)
      record(StateKey{StateKey::ClipPlane, index < DeviceState::kClipPlanes ? index : DeviceState::kClipPlanes - 1, 0});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float *plane) override {
    if (!plane)
      return D3DERR_INVALIDCALL;
    // GetClipPlane never refuses: Wine clamps to the stateblock's clip-plane count and copies the stored vec4
    // (zero-filled until a SetClipPlane names it) — winetest test_user_clip_planes reads D3D_OK and zero planes
    // for every index under 2 * D3DMAXUSERCLIPPLANES
    state_.clip_plane(index < DeviceState::kClipPlanes ? index : DeviceState::kClipPlanes - 1, plane);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) override;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override;
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override;
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *clip_status) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override;
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override;
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override;
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override;
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override;
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *rect) override;
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override;
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(WINBOOL bSoftware) override {
    // MIXED devices accept the switch (Wine device.c CREATE_DEVICE_MIXED_ONLY); HW/SW-only refuse it.
    if (!(behavior_flags_ & D3DCREATE_MIXED_VERTEXPROCESSING))
      return D3DERR_INVALIDCALL;
    software_vertex_processing_ = bSoftware ? TRUE : FALSE;
    return D3D_OK;
  }
  WINBOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return software_vertex_processing_; }
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return D3DERR_INVALIDCALL; }
  float STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
      D3DPRIMITIVETYPE type, INT base_vertex, UINT min_vertex, UINT vertex_count, UINT start_index,
      UINT primitive_count
  ) override;
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
      D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertices, UINT stride
  ) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx,
                                                   UINT vertex_count, UINT primitive_count, const void *index_data,
                                                   D3DFORMAT index_format, const void *data, UINT stride) override;
  // the fixed-function transform and lighting done on the CPU into a buffer of screen-space vertices: stream 0's
  // vertices in the FVF the device has, transformed, divided and mapped through the viewport as a draw would, and lit
  // as a draw would light them (d3d9_device_draw.cpp)
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT source_start, UINT destination_index, UINT vertex_count,
                                            IDirect3DVertexBuffer9 *destination,
                                            IDirect3DVertexDeclaration9 *destination_declaration,
                                            DWORD flags) override;
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements,
                                                    IDirect3DVertexDeclaration9 **declaration) override;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override;
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *fvf) override;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *byte_code, IDirect3DVertexShader9 **shader) override;
  // `counted_device` is false for a shader the device itself keeps, so the object cannot hold a reference cycle on it
  HRESULT create_vertex_shader(const DWORD *byte_code, IDirect3DVertexShader9 **shader, bool counted_device);
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *shader) override;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **shader) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const int *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, int *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const WINBOOL *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, WINBOOL *data, UINT count) override;  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) override;
  // a state block restores a cleared stream with the stride it recorded (zero included), while the API call keeps the
  // stride when the call names none, so a block cannot restore through SetStreamSource alone
  HRESULT restore_stream_source(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride);
  HRESULT STDMETHODCALLTYPE GetStreamSource(
      UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride
  ) override;
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT setting) override;
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT *setting) override;
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *indices) override;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **indices) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, IDirect3DPixelShader9 **shader) override;
  // `counted_device` is false for a shader the device itself keeps, so the object cannot hold a reference cycle on it
  HRESULT create_pixel_shader(const DWORD *byte_code, IDirect3DPixelShader9 **shader, bool counted_device);
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *shader) override;
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **shader) override;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const int *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, int *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const WINBOOL *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, WINBOOL *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segment_count,
                                          const D3DRECTPATCH_INFO *patch_info) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segment_count,
                                         const D3DTRIPATCH_INFO *patch_info) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override;

  // IDirect3DDevice9Ex: the same device through the extended interface. the calls that have a non-Ex form are that
  // form with the extra argument dropped; the display mode is the one the chain reports, in the Ex structure
  HRESULT STDMETHODCALLTYPE PresentEx(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty,
                                      DWORD flags) override;
  HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND destination_window) override;

  HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS *present_parameters, D3DDISPLAYMODEEX *) override {
    return Reset(present_parameters);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT width, UINT height, D3DFORMAT format,
                                                 D3DMULTISAMPLE_TYPE multisample, DWORD multisample_quality,
                                                 WINBOOL lockable, IDirect3DSurface9 **surface, HANDLE *shared_handle,
                                                 DWORD usage) override {
    // Ex usage must be 0 for a colour RT; any usage (or a depth format via CreateRenderTarget) is INVALIDCALL
    // (Wine d3d9ex.c test_resource_access SURFACE_RT_EX).
    if (usage)
      return D3DERR_INVALIDCALL;
    return CreateRenderTarget(width, height, format, multisample, multisample_quality, lockable, surface,
                              shared_handle);
  }

  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                          IDirect3DSurface9 **surface, HANDLE *shared_handle,
                                                          DWORD) override {
    return CreateOffscreenPlainSurface(width, height, format, pool, surface, shared_handle);
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT width, UINT height, D3DFORMAT format,
                                                        D3DMULTISAMPLE_TYPE multisample, DWORD multisample_quality,
                                                        WINBOOL discard, IDirect3DSurface9 **surface,
                                                        HANDLE *shared_handle, DWORD usage) override {
    // The Ex call's usage is not the depth-stencil bit: that bit is implied. Requesting it explicitly is the one
    // documented invalid usage (Wine's d3d9ex conformance test covers exactly that distinction).
    if (usage & D3DUSAGE_DEPTHSTENCIL)
      return D3DERR_INVALIDCALL;
    if (usage & ~(DWORD)D3DUSAGE_RESTRICTED_CONTENT)
      return D3DERR_INVALIDCALL;
    return create_depth_stencil_surface(width, height, format, multisample, multisample_quality, discard, usage, surface,
                                        shared_handle);
  }

  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT swap_chain, D3DDISPLAYMODEEX *mode,
                                             D3DDISPLAYROTATION *rotation) override {
    if (swap_chain != 0 || !mode || mode->Size != sizeof(D3DDISPLAYMODEEX))
      return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE current{};
    if (HRESULT hr = display_mode(&current); FAILED(hr))
      return hr;
    mode->Size = sizeof(D3DDISPLAYMODEEX);
    mode->Width = current.Width;
    mode->Height = current.Height;
    mode->RefreshRate = current.RefreshRate;
    mode->Format = current.Format;
    mode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    if (rotation)
      *rotation = D3DDISPLAYROTATION_IDENTITY;
    return S_OK;
  }

  // the frames a game may queue ahead: D3D9Ex accepts 0 to 30, and 0 is the default of 3 (Wine d3d9ex.c
  // test_frame_latency: 30 succeeds and 31 is D3DERR_INVALIDCALL)
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    if (max_latency > 30)
      return D3DERR_INVALIDCALL;
    max_frame_latency_ = max_latency ? max_latency : 3;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    if (!max_latency)
      return D3DERR_INVALIDCALL;
    *max_latency = max_frame_latency_;
    return S_OK;
  }

  // the GPU thread's priority is kept as D3D9Ex's range of -7 to 7 and read back; Metal schedules the work itself
  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    if (!priority)
      return D3DERR_INVALIDCALL;
    *priority = gpu_thread_priority_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    if (priority < -7 || priority > 7)
      return D3DERR_INVALIDCALL;
    gpu_thread_priority_ = priority;
    return S_OK;
  }

  // waits for the display's next vertical blank, through the output's own WaitForVBlank where DXGI has one
  // (Microsoft Learn, IDirect3DDevice9Ex::WaitForVBlank / IDXGIOutput::WaitForVBlank)
  HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT swap_chain) override;

  // every resource lives in the Metal device's memory, which the GPU reads without a residency transition: a resource
  // this device made is resident, and the call checks its arguments rather than assuming them (Microsoft Learn,
  // IDirect3DDevice9Ex::CheckResourceResidency)
  HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9 **resources, UINT32 count) override;

  // a device whose frames the GPU failed is lost (lost_reason), and any other is fine

  HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT, UINT, float *, float *) override {
    ERR_ONCE("D3D9: SetConvolutionMonoKernel is not provided by this build.");
    return D3DERR_INVALIDCALL;
  }

  HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9 *, IDirect3DSurface9 *, IDirect3DVertexBuffer9 *, UINT,
                                         IDirect3DVertexBuffer9 *, D3DCOMPOSERECTSOP, INT, INT) override {
    ERR_ONCE("D3D9: ComposeRects is not provided by this build.");
    return D3DERR_INVALIDCALL;
  }

  // whether the device is recording a state block (BeginStateBlock until EndStateBlock), which a block's Capture and
  // Apply refuse, as D3D9's do
  bool state_block_recording() const {
    return recording_;
  }

private:
  // Let go of the children the device owned, so their private references can run down; the object stays alive while
  // any child still holds one, and that child can still name a valid device. The D3D11 device, context and DXGI chain
  // stay: they hold no D3D9 reference, and a retained swap chain's calls reach the device through them.
  void release_device_children();
  std::atomic<ULONG> app_refs_{1};
  std::atomic<ULONG> child_refs_{0};
  std::atomic<bool> torn_down_{false};

  // the state a Reset loses: the bindings, the shaders set, the textures, the constants, the render states and the
  // texture stages go back to the defaults a device is made with. the shader objects an application made survive, as
  // D3D9's do; they are only unbound here.
  void reset_state();
  // the chain the device presents through, over the device it has: a Reset that recreates the device makes a new one,
  // and the chain object an application may still hold answers the same device's frames through it
  HRESULT create_chain(IDXGIFactory2 *factory);
  // the device once its chain is the present parameters': the state is lost, the viewport and the scissor rectangle are
  // the whole of the new frame, a chain with an automatic depth-stencil surface gets one of the new size, and the
  // caller's parameters report the fields D3D9 says a reset leaves to be queried again
  HRESULT finish_reset(D3DPRESENT_PARAMETERS *parameters);
  // a lost device's Reset: the failed Direct3D 11 device and everything the device held on it are released, a new one
  // is made over the same adapter with the same chain, and the managed pool's contents are put back on it. The caller
  // has already checked the present parameters and set present_
  HRESULT recreate_after_loss();
  // the back buffer of the frame being drawn: taken from the chain once, and the same buffer from then on. a flip
  // model chain turns its buffers over at every Present, but it does so by giving the buffers' storage to the next
  // buffer's object, so this device's view and surface of buffer 0 address whatever the next frame is drawn into (the
  // frames this milestone's tests draw, present and draw again check that, and a Present that took the buffer again
  // changed nothing they could see)
  HRESULT ensure_back_buffer();
  // the frame's target: the back buffer until SetRenderTarget names another one
  HRESULT ensure_render_target();
  // the depth-stencil view when it can be bound with the current target set. D3D9 keeps the surface bound across a
  // target of another size and clips it; D3D11 needs the same size and sample count, so while they differ the view is
  // left out of a draw or clear and comes back when a matching target is set again -- the binding itself is not dropped
  // (Wine visual.c test_mipmap_autogen sets a 1024x1024 target, then the back buffer, and clears depth)
  ID3D11DepthStencilView *bound_depth_stencil() const;
  // let go of the private reference on a slot's target, if this device holds one
  void release_render_target(UINT index);
  // let go of the bound depth-stencil surface: an application-created one through its private reference, the automatic
  // one (implicit, and the device's) without one
  void clear_depth_stencil();
  // destroy the automatic depth-stencil surface with the device or the chain that made it
  void destroy_depth_stencil();
  // D3DERR_DEVICELOST while the D3D11 device under this one is removed, S_OK otherwise: the calls that draw or present
  // answer it, and TestCooperativeLevel is it
  HRESULT lost_reason() const;
  // a surface of this device's, of the kind a target or a depth-stencil surface has to be; reached through
  // QueryInterface, so a surface of someone else's is refused rather than misread
  HRESULT device_surface(IDirect3DSurface9 *surface, bool depth_stencil, IMTLD3D9Surface **out);
  // the render target view of the level and slice a surface is, and the scaled copy StretchRect draws into it
  // (d3d9_device_transfer.cpp)
  HRESULT surface_render_view(IMTLD3D9Surface *surface, ID3D11RenderTargetView **view);
  HRESULT stretch_blit(IMTLD3D9Surface *source, const D3D11_BOX &source_box, IMTLD3D9Surface *target,
                       const RECT &destination, bool linear);
  HRESULT ensure_blit_objects();
  void release_blit_objects();
  ID3D11Texture2D *blit_scratch(UINT width, UINT height, DXGI_FORMAT format);
  void invalidate_back_buffer();
  // whether the back buffer is a multisampled texture of the device's own that Present resolves into the DXGI chain's
  // buffer (a flip model chain cannot be multisampled), or the chain's own buffer
  bool multisampled_chain() const { return present_.MultiSampleType != D3DMULTISAMPLE_NONE; }

  ComPtr<IDirect3D9> direct3d_;
  // Direct3DCreate9Ex made this device: only then does QueryInterface answer IDirect3DDevice9Ex
  bool extended_ = false;
  ComPtr<IDXGIAdapter1> adapter_;
  UINT adapter_ordinal_ = 0;
  D3DCAPS9 caps_{};
  // CreateDevice's hFocusWindow: GetCreationParameters reports it and it must not receive device-window style
  // changes when hDeviceWindow is a different HWND (Wine device.c test_window_styles).
  HWND window_ = nullptr;
  // DXGI allows one swap chain per HWND; D3D9 allows multiple devices on one focus window. When CreateSwapChainForHwnd
  // refuses the presentation HWND, presentation is bound to this privately owned proxy of the same client size.
  HWND swapchain_window_ = nullptr;
  bool owns_swapchain_window_ = false;
  // The HWND the implicit chain presents through: present_.hDeviceWindow when set, otherwise the focus window.
  HWND present_hwnd() const {
    if (present_.hDeviceWindow && IsWindow(present_.hDeviceWindow))
      return present_.hDeviceWindow;
    return window_;
  }
  DWORD behavior_flags_ = 0;
  WINBOOL software_vertex_processing_ = FALSE;
  // Fullscreen focus-loss: DEVICELOST while another window is foreground; DEVICENOTRESET once focus returns until Reset.
  enum class Cooperative { Ok, Lost, NotReset };
  Cooperative cooperative_ = Cooperative::Ok;
  bool exclusive_reported_ = false;
  bool presented_once_ = false;
  // Fullscreen window management (Wine device.c test_window_style / wndproc).
  bool styles_saved_ = false;
  bool focus_subclassed_ = false;
  bool fullscreen_restore_used_ = false;
  LONG saved_device_style_ = 0;
  LONG saved_device_exstyle_ = 0;
  RECT saved_device_rect_{};
  WNDPROC focus_wndproc_ = nullptr;
  void apply_fullscreen_window_state(bool fullscreen);
  void save_device_window_styles_if_needed();
  RECT fullscreen_monitor_rect() const;
  WNDPROC focus_wndproc_for_forward() const { return focus_wndproc_; }

public:
  void notify_focus_lost();
  void notify_focus_gained();
  void restore_fullscreen_window_if_needed();
  void note_gpu_draw() { d3d9_note_gpu_draw(); }
  bool exclusive_occluded() const;

private:
  D3DPRESENT_PARAMETERS present_{};
  UINT width_ = 0, height_ = 0;
  // 256 D3D9 palettes of 256 entries (Wine device.c SetPaletteEntries).
  PALETTEENTRY palettes_[256][256]{};
  UINT current_palette_ = 0;

  // the factory the device makes its chain through: a Reset that recreates the device makes a new chain, since a DXGI
  // chain belongs to the device that made it
  ComPtr<IDXGIFactory2> factory_;
  ComPtr<ID3D11Device> d3d11_device_;
  ComPtr<ID3D11DeviceContext> context_;
  // the same context, as the one that clears a part of a view (ID3D11DeviceContext1::ClearView): D3D11's own
  // ClearRenderTargetView and ClearDepthStencilView clear the whole of it
  ComPtr<ID3D11DeviceContext1> context1_;
  ComPtr<IDXGISwapChain1> swapchain_;
  // the implicit chain is the device's own, made at reference count 0: the device holds it without a counted reference
  // and frees it with destroy_implicit (Object9)
  MTLD3D9SwapChain *swap_chain_ = nullptr;
  // the chains CreateAdditionalSwapChain made, held without a public reference so the application's count alone owns
  // them; a Reset clears the list after the application has released the default pool
  std::vector<MTLD3D9SwapChain *> additional_chains_;
  // the resources a Reset rebuilds on the device it makes (the managed pool): each registers itself when it is made and
  // unregisters itself when it is destroyed (d3d9_texture.cpp)
  std::vector<IMTLD3D9ResetTarget *> reset_targets_;

  ComPtr<ID3D11Texture2D> back_buffer_texture_;
  ComPtr<ID3D11RenderTargetView> render_target_;
  MTLD3D9Surface *back_buffer_surface_ = nullptr;
  // GetBackBuffer(1..N-1) for multi-buffer chains (Wine visual.c test_flip); index 0 is back_buffer_surface_.
  std::vector<MTLD3D9Surface *> extra_back_buffers_;
  // the frame the chain showed last: GetFrontBufferData's source, and what a D3DSWAPEFFECT_COPY chain's back buffer is
  // put back from after a Present
  FrontBuffer9 front_;
  // SetCursorProperties builds an HCURSOR from the surface bits (Wine device.c GetCursorInfo).
  bool cursor_set_ = false;
  HCURSOR cursor_handle_ = nullptr;
  UINT cursor_hotspot_x_ = 0, cursor_hotspot_y_ = 0;
  bool cursor_visible_ = false;
  int cursor_x_ = 0, cursor_y_ = 0;
  bool in_scene_ = false;
  // what SetMaximumFrameLatency and SetGPUThreadPriority were given: D3D9Ex's defaults until they are called
  UINT max_frame_latency_ = 3;
  INT gpu_thread_priority_ = 0;
  // DEFAULT-pool texture bytes this device has created: GetAvailableTextureMem subtracts them so the figure
  // drops across CreateTexture (Wine device.c test_get_available_texture_mem). DXGI CurrentUsage alone often stays flat.
  uint64_t tracked_texture_bytes_ = 0;

  // the surfaces the frame is drawn into, from 0 to the highest SetRenderTarget set: slot 0 is the back buffer until
  // SetRenderTarget names another one, and is the one the viewport, the scissor rectangle and the depth-stencil pair are
  // sized by. A pixel shader writes oC0 to the first and oC3 to the last (d3d9_device_targets.cpp)
  IDirect3DSurface9 *render_targets_[kMaxRenderTargets]{};
  // the private reference this device holds on a target the application made; the implicit back buffer is the device's
  // own and has none, so binding it is not counted in the slot
  IMTLD3D9Surface *render_target_objects_[kMaxRenderTargets]{};
  bool render_target_held_[kMaxRenderTargets]{};
  ComPtr<ID3D11RenderTargetView> render_target_views_[kMaxRenderTargets];
  UINT render_target_count_ = 0;
  UINT target_width_ = 0, target_height_ = 0;
  // the sample count of the render target set's slot 0, which every other target and the depth surface must match
  UINT target_samples_ = 1;
  MTLD3D9Surface *depth_stencil_ = nullptr;
  // whether this device holds a private reference on the bound depth-stencil surface (an application-created one);
  // the automatic surface is implicit and the device owns it without a counted reference
  bool depth_stencil_held_ = false;
  ComPtr<ID3D11DepthStencilView> depth_stencil_view_;
  D3DVIEWPORT9 viewport_{};
  // the application's scissor rectangle, which D3DRS_SCISSORTESTENABLE clips a draw to: the intersection of it and the
  // viewport is the draw's clip rectangle (IDirect3DDevice9::SetScissorRect)
  RECT scissor_rect_{};
  // the clip rectangle a draw runs under: the viewport, narrowed by the scissor rectangle while the scissor test is on
  RECT clip_rect() const;
  // the state a draw is made from: the render states, the world, view and projection transforms with their composition,
  // and the eight lights with their enables and the material. One object, one owner (d3d9_device_state.hpp); a Reset
  // is its reset, and a state block captures and restores it through the interface above
  DeviceState state_;

  // the fixed-function path, and the state a draw reads: the FVF or the declaration, the streams a declaration names
  // and the indices (SetStreamSourceFreq is 1, no instancing)
  FixedFunctionPipeline fixed_function_;
  DWORD fvf_ = 0;
  // Neither pointer is a counted reference: SetVertexDeclaration must not change the application's refcount (Wine's
  // device test checks it), and GetVertexDeclaration is what AddRefs. SetFVF's declarations are the device's cache.
  IDirect3DVertexDeclaration9 *declaration_ = nullptr;
  IMTLD3D9VertexDeclaration *declaration_inputs_ = nullptr;
  // SetFVF's declarations, one per format for the device's lifetime, as D3D9 keeps them: the application's counts on
  // them are its own (they can fall to zero while the device still holds the format), and the cache frees them.
  std::map<DWORD, MTLD3D9VertexDeclaration *> fvf_declarations_;
  struct StreamSlot {
    // borrowed from the application and held through a private reference on the buffer: SetStreamSource does not change
    // the count the application sees, and the device can still draw from a buffer it released
    IDirect3DVertexBuffer9 *source = nullptr;
    IMTLD3D9Buffer *buffer = nullptr;
    UINT offset = 0;
    UINT stride = 0;
  };
  StreamSlot streams_[kMaxStreams];
  void bind_streams();
  void release_stream(UINT stream);
  void clear_stream(UINT stream);
  IDirect3DIndexBuffer9 *indices_ = nullptr;
  IMTLD3D9Buffer *index_buffer_ = nullptr;
  void release_indices();
  ComPtr<ID3D11Buffer> up_scratch_;
  UINT up_scratch_size_ = 0;
  // the UP draw just staged its vertices into the canonical two-UV layout (a multi-coordinate-set FVF or declaration)
  bool uv_pair_ = false;
  // the indices of a DrawIndexedPrimitiveUP, staged the same way as its vertices
  ComPtr<ID3D11Buffer> up_index_scratch_;
  UINT up_index_scratch_size_ = 0;
  // the triangle list a fan draw is expanded into: 32-bit indices, grown as the largest fan drawn needs
  ComPtr<ID3D11Buffer> fan_index_scratch_;
  UINT fan_index_scratch_size_ = 0;
  // the vertices a fixed-function draw whose declaration or format the pipeline's static layouts cannot read gathered
  // into the format it draws (bind_gathered_declaration_stream)
  ComPtr<ID3D11Buffer> decl_scratch_;
  UINT decl_scratch_size_ = 0;

  // the texture stages a draw samples, each with the texture SetTexture set and the sampler that stage's sampler states
  // make (the caps' MaxSimultaneousTextures), and the one fixed-function stage whose operations the textured path
  // performs (MaxTextureBlendStages), which is stage 0's (d3d9_device_texture.cpp)
  // SetTexture holds a private reference: the application's public count is unchanged (Wine's test_refcount)
  IDirect3DBaseTexture9 *textures_[kTotalTextureStages]{};
  IMTLD3D9Texture *texture_sources_[kTotalTextureStages]{};
  SamplerState9 sampler_states_[kTotalTextureStages];
  TextureStageState9 texture_stages_[kMaxBlendStages];
  UINT stream_freq_[kMaxStreams] = {1, 1, 1, 1};
  // what a stage with no texture samples: D3D9's default texture, one opaque white texel, which a shader's texld reads
  // like any other texture (made the first time a sampling draw needs it)
  ComPtr<ID3D11Texture2D> default_texture_;
  ComPtr<ID3D11ShaderResourceView> default_texture_view_;
  HRESULT ensure_default_texture();

  // the shaders a game made and set, and the constants it set for them: D3D9's tables are 256 float4s, 16 int4s and
  // 16 bools for a vertex shader, and the same for a pixel shader (IDirect3DDevice9::Set*ShaderConstant*)
  // D3D9 SetVertexShader/SetPixelShader do not AddRef publicly (Wine device.c); they hold a private reference so a
  // bound shader survives a public Release (Wine visual.c alphatest_test).
  IDirect3DVertexShader9 *vertex_shader_ = nullptr;
  IDirect3DPixelShader9 *pixel_shader_ = nullptr;
  void clear_bound_shaders() {
    if (IMTLD3D9VertexShader *previous = lookup_vertex_shader(vertex_shader_))
      previous->release_private_ref();
    vertex_shader_ = nullptr;
    if (IMTLD3D9PixelShader *previous = lookup_pixel_shader(pixel_shader_))
      previous->release_private_ref();
    pixel_shader_ = nullptr;
  }
  std::array<float, 256 * 4> vertex_constants_f_{};
  std::array<int32_t, 16 * 4> vertex_constants_i_{};
  std::array<WINBOOL, 16> vertex_constants_b_{};
  std::array<float, 256 * 4> pixel_constants_f_{};
  std::array<int32_t, 16 * 4> pixel_constants_i_{};
  std::array<WINBOOL, 16> pixel_constants_b_{};
  // a programmable draw used to upload every table every time; only a table a Set*ShaderConstant* (or Reset) changed
  // is mapped again
  bool vs_f_dirty_ = true, vs_i_dirty_ = true, vs_b_dirty_ = true;
  bool ps_f_dirty_ = true, ps_i_dirty_ = true, ps_b_dirty_ = true;
  bool ps_alpha_dirty_ = true;
  bool ps_fog_dirty_ = true;
  DWORD uploaded_alpha_ref_ = ~0u;

  // the D3D11 constant buffers the tables are read from, three a stage, and the one a pixel program with D3D9's alpha
  // test and fog reads (register b3: c0 = ALPHAREF, c1 = fog start/end/density, c2 = fog colour)
  ComPtr<ID3D11Buffer> vs_constants_f_, vs_constants_i_, vs_constants_b_;
  ComPtr<ID3D11Buffer> ps_constants_f_, ps_constants_i_, ps_constants_b_;
  ComPtr<ID3D11Buffer> ps_alpha_constants_;
  // the viewport a transformed (POSITIONT) vertex shader maps its screen-space position through (d3d9_shader_translator.cpp)
  ComPtr<ID3D11Buffer> vs_viewport_;
  // the user clip planes, in clip space, a vertex program translated with them reads (register b3)
  ComPtr<ID3D11Buffer> vs_clip_planes_;

  // the states a state block records: every call a block of that kind carries adds its state while the device is in
  // BeginStateBlock's recording mode (d3d9_state_block.cpp)
  void record(StateKey key);
  bool recording_ = false;
  std::vector<StateKey> recorded_keys_;
  // the state the device had when BeginStateBlock was called: every recorded setter captures its new value and then
  // restores this snapshot, because recording a block is not applying it to the device (Microsoft Learn, BeginStateBlock)
  ComPtr<MTLD3D9StateBlock> recording_snapshot_;
  std::vector<StateValue> recorded_values_;

  // FVF for fixed-function draws: SetFVF's value, or the matching FVF of SetVertexDeclaration when GetFVF is 0
  DWORD fixed_function_fvf() const;
  // what every draw needs: the chain's back buffer as it is now, and the pipeline that draws it
  HRESULT prepare_draw();
  // the fixed-function draw over it: what the draw is, from the device state, handed to the pipeline that binds it
  // (d3d9_fixed_function.hpp); the draw path issues it and validates it, and nothing here touches a D3D11 object of the
  // pipeline's
  HRESULT prepare_fixed_function_draw();
  // the draw a game's own shaders make: the vertex declaration's layout, the translated programs, the constant tables
  // a draw uploads, and the texture stage 0 samples when the pixel shader samples it. a vertex shader with no pixel
  // shader bound draws through the fixed-function pixel shader (d3d9_device_shader.cpp)
  HRESULT prepare_shader_draw();
  // ps_1_1 `mov r0, v0` for a foggy VS that writes oFog while no pixel shader is bound
  HRESULT ensure_fog_passthrough_ps();
  // vs_1_1 position+texcoord passthrough when FF cannot express the active declaration (FLOAT3/4 texcoords)
  HRESULT ensure_pos_tex_passthrough_vs();
  // a vs_3_0 passthrough whose dcls match the declaration it is drawn over: one element per declaration input,
  // POSITION as the position, each colour and texture coordinate and any other usage the pixel stage reads as the
  // output its usage names (Wine visual.c pretransformed_varying_test draws a transformed monster declaration under
  // game pixel shaders that read blendweight, normal, fog, depth, tangent and binormal)
  HRESULT ensure_declaration_passthrough_vs(IMTLD3D9VertexDeclaration &declaration,
                                            IDirect3DVertexShader9 **shader);
  HRESULT ensure_shader_constants();
  HRESULT upload_shader_constants();
  // the viewport every draw is placed in and cut by (d3d9_device_shader.cpp)
  void bind_viewport_state();
  ComPtr<IDirect3DPixelShader9> fog_passthrough_ps_;
  ComPtr<IDirect3DVertexShader9> pos_tex_passthrough_vs_;
  // a declaration-shaped passthrough is one per declaration: each is kept until the device ends
  std::map<std::vector<D3DVERTEXELEMENT9>, ComPtr<IDirect3DVertexShader9>, DeclarationElementsLess>
      declaration_passthroughs_;
  // the vertices or indices a primitive count is made of, with whether this milestone draws the topology at all
  static bool topology_count(D3DPRIMITIVETYPE type, UINT primitive_count, UINT &count);
  // the triangle list a D3DPT_TRIANGLEFAN draw is expanded into, in fan_index_scratch_ (D3D11 has no fan topology)
  HRESULT bind_fan_triangle_list(const void *indices, UINT index_size, UINT start_index, UINT primitive_count);
  // whether a primitive count is within D3DCAPS9::MaxPrimitiveCount
  bool primitive_count_fits(UINT primitive_count) const;
  // the state a _UP draw leaves behind: D3D9's DrawPrimitiveUP and DrawIndexedPrimitiveUP clear stream 0, and the
  // indexed one clears the index buffer too (d3d9_device_draw.cpp)
  void unbind_stream_source();
  void unbind_indices();
  // D3DFVF_XYZ has no DIFFUSE: expand stream 0 into XYZ|DIFFUSE (opaque white) so the transformed FF layout binds
  HRESULT bind_position_fvf_stream(UINT start_vertex, UINT vertex_count);
  // the format the fixed-function pipeline draws for `fvf`: a format without a diffuse colour becomes its diffuse
  // counterpart, whose layout supplies opaque white
  DWORD packed_fixed_function_fvf(DWORD fvf) const;
  // gather `vertex_count` vertices of a declaration into the canonical layout of `packed_fvf`, from the streams its
  // elements name (a declaration split over streams, or laid out with its own offsets)
  HRESULT bind_gathered_declaration_stream(
      const std::vector<D3DVERTEXELEMENT9> &elements, UINT vertex_count, DWORD packed_fvf, UINT instance = 0
  );
  // the same gathering for the vertices a _UP draw carries on the CPU
  HRESULT pack_up_vertices(
      const void *vertices, UINT stride, UINT vertex_count, DWORD declared_fvf, DWORD packed_fvf, void *destination
  );
  // the multi-coordinate-set gather: position, colour and the two sets the stages' D3DTSS_TEXCOORDINDEX name, into the
  // canonical position, colour, TEXCOORD0 float2, TEXCOORD1 float2 layout the two-UV shaders read
  HRESULT pack_uv_pair_vertices(
      const void *vertices, UINT stride, UINT vertex_count, DWORD declared_fvf, UINT uv0, UINT uv1, void *destination
  );
  static D3D11_PRIMITIVE_TOPOLOGY topology_of(D3DPRIMITIVETYPE type);
  HRESULT draw_vertices(
      D3DPRIMITIVETYPE type, UINT primitive_count, UINT count, UINT start_vertex, ID3D11Buffer *buffer, UINT stride,
      UINT offset
  );
  // a rectangle's stencil: D3D11's ClearView writes depth of a DSV, not stencil, so a partial stencil clear is a
  // scissored draw that always replaces stencil (Microsoft Learn, IDirect3DDevice9::Clear, D3DCLEAR_STENCIL)
  HRESULT clear_stencil_rects(const D3D11_RECT *rects, UINT count, UINT8 stencil);
  HRESULT ensure_stencil_fill_target();
  ComPtr<IDirect3DSurface9> stencil_fill_target_;
  UINT stencil_fill_width_ = 0, stencil_fill_height_ = 0;

  // the objects StretchRect's scaled copy draws with, and the single-sample texture it samples when the source cannot
  // be sampled as it is
  ComPtr<ID3D11VertexShader> blit_vertex_shader_;
  ComPtr<ID3D11PixelShader> blit_pixel_shader_;
  ComPtr<ID3D11SamplerState> blit_point_sampler_, blit_linear_sampler_;
  ComPtr<ID3D11Buffer> blit_constants_;
  ComPtr<ID3D11BlendState> blit_blend_;
  ComPtr<ID3D11DepthStencilState> blit_depth_;
  ComPtr<ID3D11RasterizerState> blit_raster_;
  ComPtr<ID3D11Texture2D> blit_scratch_;
};

// makes a device over `adapter` for `window`, with the implicit swap chain
// CreateDevice's present parameters ask for
HRESULT create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
                      const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
                      IDirect3DDevice9 **device, bool extended = false);

} // namespace mullion::d3d9
