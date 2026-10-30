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
#include <vector>
#include "d3d9_surface.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_texture.hpp"

namespace mullion::d3d9 {

class MTLD3D9Device final : private Object9,
                          public IDirect3DDevice9,
                          public IMTLD3D9PresentSource,
                          public IMTLD3D9DeviceLifetime,
                          public IMTLD3D9DeviceResources {
public:
  // the texture stages, and so the samplers, a draw samples: D3D9's eight (the caps' MaxSimultaneousTextures)
  static constexpr UINT kTextureStages = 8;
  // the fixed-function blender's stages (the caps' MaxTextureBlendStages): stage 0 and one extra that the extended
  // pixel shader blends after it
  static constexpr UINT kMaxBlendStages = 2;
  // the vertex streams a shader declaration may read (the caps' MaxStreams)
  static constexpr UINT kMaxStreams = 4;

  MTLD3D9Device(IDirect3D9 *direct3d, ComPtr<IDXGIAdapter1> adapter, UINT adapter_ordinal, const D3DCAPS9 &caps,
                HWND window, DWORD behavior_flags, const D3DPRESENT_PARAMETERS &present)
      : direct3d_(direct3d), adapter_(std::move(adapter)), adapter_ordinal_(adapter_ordinal), caps_(caps),
        window_(window), behavior_flags_(behavior_flags), present_(present) {}

  ~MTLD3D9Device() override { invalidate_back_buffer(); }

  HRESULT init(IDXGIFactory2 *factory);

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3DDevice9)) {
      *object = static_cast<IDirect3DDevice9 *>(this);
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

  // IMTLD3D9PresentSource
  HRESULT present(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty, DWORD flags) override;
  HRESULT back_buffer(UINT index, IDirect3DSurface9 **surface) override;
  HRESULT display_mode(D3DDISPLAYMODE *mode) override;

  // the device's own
  //
  // D3D9's answer for a device whose frames the GPU failed: lost, and it stays lost until the application resets it.
  // A lost device is one this build can reset (Reset makes a new Direct3D 11 device), so the level is the resettable
  // one (D3DERR_DEVICENOTRESET); the calls that draw answer D3DERR_DEVICELOST until it is reset (Microsoft Learn,
  // IDirect3DDevice9::TestCooperativeLevel)
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
    return FAILED(lost_reason()) ? D3DERR_DEVICENOTRESET : S_OK;
  }

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
    if (swap_chain != 0)
      return D3DERR_INVALIDCALL;
    return display_mode(mode);
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

  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT swap_chain, IDirect3DSwapChain9 **result) override {
    if (swap_chain != 0 || !result)
      return D3DERR_INVALIDCALL;
    *result = swap_chain_.Get();
    (*result)->AddRef();
    return S_OK;
  }

  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }

  // the device's windowed chain, sized and formatted again: the state is lost and the back buffer is the new size, so
  // a game that changes resolution or alt-tabs draws its next frame at the new size. A device whose frames failed is
  // made again here, which is the road back from D3DERR_DEVICENOTRESET: the new chain is over a new Direct3D 11 device,
  // the default pool is gone with the old one, and the managed pool's contents are put onto the new one. a fullscreen
  // chain is not implemented (src/d3d9/README).
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *present_parameters) override;

  HRESULT STDMETHODCALLTYPE Present(const RECT *source, const RECT *destination, HWND window,
                                    const RGNDATA *dirty) override {
    return present(source, destination, window, dirty, 0);
  }

  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT swap_chain, UINT back_buffer, D3DBACKBUFFER_TYPE type,
                                          IDirect3DSurface9 **result) override {
    if (swap_chain != 0 || type != D3DBACKBUFFER_TYPE_MONO)
      return D3DERR_INVALIDCALL;
    return this->back_buffer(back_buffer, result);
  }

  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swap_chain, D3DRASTER_STATUS *status) override {
    if (swap_chain != 0 || !status)
      return D3DERR_INVALIDCALL;
    status->InVBlank = FALSE;
    status->ScanLine = 0;
    return S_OK;
  }

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

  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT swap_chain, IDirect3DSurface9 *) override {
    ERR_ONCE("D3D9: GetFrontBufferData is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }

  // the argument checks of a surface read back into or out of: ours, and of the
  // pool the copy needs
  HRESULT source_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out);
  HRESULT staging_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out);

  // what this milestone does not have yet: resources, shaders, queries, state
  // that reaches a draw. the calls are refused (or, for state that nothing
  // reads yet, kept by nobody) rather than pretended (src/d3d9/README)
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot,
                                                IDirect3DSurface9 *pCursorBitmap) override {
    return D3DERR_INVALIDCALL;
  }
  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override {}
  WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL bShow) override { return FALSE; }
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
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override {
    ERR_ONCE("D3D9: a clip plane is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override { return D3DERR_INVALIDCALL; }
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
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *rect) override;
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override;
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(WINBOOL bSoftware) override { return D3DERR_INVALIDCALL; }
  WINBOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return FALSE; }
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
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                                            IDirect3DVertexBuffer9 *pDestBuffer,
                                            IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) override {
    ERR_ONCE("D3D9: ProcessVertices is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements,
                                                    IDirect3DVertexDeclaration9 **declaration) override;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override;
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *fvf) override;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *byte_code, IDirect3DVertexShader9 **shader) override;
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *shader) override;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **shader) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const int *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, int *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const WINBOOL *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, WINBOOL *data, UINT count) override;
  HRESULT STDMETHODCALLTYPE SetStreamSource(
      UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride
  ) override;
  HRESULT STDMETHODCALLTYPE GetStreamSource(
      UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride
  ) override;
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT setting) override;
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT *setting) override;
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *indices) override;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **indices) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, IDirect3DPixelShader9 **shader) override;
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
  // D3DERR_DEVICELOST while the D3D11 device under this one is removed, S_OK otherwise: the calls that draw or present
  // answer it, and TestCooperativeLevel is it
  HRESULT lost_reason() const;
  // a surface of this device's, of the kind a target or a depth-stencil surface has to be; reached through
  // QueryInterface, so a surface of someone else's is refused rather than misread
  HRESULT device_surface(IDirect3DSurface9 *surface, bool depth_stencil, IMTLD3D9Surface **out);
  void invalidate_back_buffer() {
    back_buffer_surface_.Reset();
    render_target_.Reset();
    back_buffer_texture_.Reset();
  }

  ComPtr<IDirect3D9> direct3d_;
  ComPtr<IDXGIAdapter1> adapter_;
  UINT adapter_ordinal_ = 0;
  D3DCAPS9 caps_{};
  HWND window_ = nullptr;
  DWORD behavior_flags_ = 0;
  D3DPRESENT_PARAMETERS present_{};
  UINT width_ = 0, height_ = 0;

  // the factory the device makes its chain through: a Reset that recreates the device makes a new chain, since a DXGI
  // chain belongs to the device that made it
  ComPtr<IDXGIFactory2> factory_;
  ComPtr<ID3D11Device> d3d11_device_;
  ComPtr<ID3D11DeviceContext> context_;
  // the same context, as the one that clears a part of a view (ID3D11DeviceContext1::ClearView): D3D11's own
  // ClearRenderTargetView and ClearDepthStencilView clear the whole of it
  ComPtr<ID3D11DeviceContext1> context1_;
  ComPtr<IDXGISwapChain1> swapchain_;
  ComPtr<MTLD3D9SwapChain> swap_chain_;
  // the resources a Reset rebuilds on the device it makes (the managed pool): each registers itself when it is made and
  // unregisters itself when it is destroyed (d3d9_texture.cpp)
  std::vector<IMTLD3D9ResetTarget *> reset_targets_;

  ComPtr<ID3D11Texture2D> back_buffer_texture_;
  ComPtr<ID3D11RenderTargetView> render_target_;
  ComPtr<MTLD3D9Surface> back_buffer_surface_;
  bool in_scene_ = false;

  // the surface the frame is drawn into (the back buffer until SetRenderTarget names another one), the depth-stencil
  // surface a draw tests against, and the viewport a draw is placed and clipped by
  ComPtr<IDirect3DSurface9> current_target_;
  ComPtr<ID3D11RenderTargetView> current_target_view_;
  UINT target_width_ = 0, target_height_ = 0;
  ComPtr<MTLD3D9Surface> depth_stencil_;
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
  ComPtr<IDirect3DVertexDeclaration9> declaration_;
  ComPtr<IMTLD3D9VertexDeclaration> declaration_inputs_;
  struct StreamSlot {
    ComPtr<IDirect3DVertexBuffer9> source;
    ComPtr<IMTLD3D9Buffer> buffer;
    UINT offset = 0;
    UINT stride = 0;
  };
  StreamSlot streams_[kMaxStreams];
  void bind_streams();
  void clear_stream(UINT stream);
  ComPtr<IDirect3DIndexBuffer9> indices_;
  ComPtr<IMTLD3D9Buffer> index_buffer_;
  ComPtr<ID3D11Buffer> up_scratch_;
  UINT up_scratch_size_ = 0;
  // the indices of a DrawIndexedPrimitiveUP, staged the same way as its vertices
  ComPtr<ID3D11Buffer> up_index_scratch_;
  UINT up_index_scratch_size_ = 0;

  // the texture stages a draw samples, each with the texture SetTexture set and the sampler that stage's sampler states
  // make (the caps' MaxSimultaneousTextures), and the one fixed-function stage whose operations the textured path
  // performs (MaxTextureBlendStages), which is stage 0's (d3d9_device_texture.cpp)
  ComPtr<IDirect3DBaseTexture9> textures_[kTextureStages];
  ComPtr<IMTLD3D9Texture> texture_sources_[kTextureStages];
  SamplerState9 sampler_states_[kTextureStages];
  TextureStageState9 texture_stages_[kMaxBlendStages];
  UINT stream_freq_[kMaxStreams] = {1, 1, 1, 1};
  // what a stage with no texture samples: D3D9's default texture, one opaque white texel, which a shader's texld reads
  // like any other texture (made the first time a sampling draw needs it)
  ComPtr<ID3D11Texture2D> default_texture_;
  ComPtr<ID3D11ShaderResourceView> default_texture_view_;
  HRESULT ensure_default_texture();

  // the shaders a game made and set, and the constants it set for them: D3D9's tables are 256 float4s, 16 int4s and
  // 16 bools for a vertex shader, and the same for a pixel shader (IDirect3DDevice9::Set*ShaderConstant*)
  ComPtr<IDirect3DVertexShader9> vertex_shader_;
  ComPtr<IDirect3DPixelShader9> pixel_shader_;
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
  DWORD uploaded_alpha_ref_ = ~0u;

  // the D3D11 constant buffers the tables are read from, three a stage, and the one a pixel program with D3D9's alpha
  // test reads D3DRS_ALPHAREF from (register b3)
  ComPtr<ID3D11Buffer> vs_constants_f_, vs_constants_i_, vs_constants_b_;
  ComPtr<ID3D11Buffer> ps_constants_f_, ps_constants_i_, ps_constants_b_;
  ComPtr<ID3D11Buffer> ps_alpha_constants_;

  // the states a state block records: every call a block of that kind carries adds its state while the device is in
  // BeginStateBlock's recording mode (d3d9_state_block.cpp)
  void record(StateKey key);
  bool recording_ = false;
  std::vector<StateKey> recorded_keys_;

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
  HRESULT ensure_shader_constants();
  HRESULT upload_shader_constants();
  // the viewport every draw is placed in and cut by (d3d9_device_shader.cpp)
  void bind_viewport_state();
  // the vertices or indices a primitive count is made of, with whether this milestone draws the topology at all
  static bool topology_count(D3DPRIMITIVETYPE type, UINT primitive_count, UINT &count);
  // whether a primitive count is within D3DCAPS9::MaxPrimitiveCount
  bool primitive_count_fits(UINT primitive_count) const;
  // the state a _UP draw leaves behind: D3D9's DrawPrimitiveUP and DrawIndexedPrimitiveUP clear stream 0, and the
  // indexed one clears the index buffer too (d3d9_device_draw.cpp)
  void unbind_stream_source();
  void unbind_indices();
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
};

// makes a device over `adapter` for `window`, with the implicit swap chain
// CreateDevice's present parameters ask for
HRESULT create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
                      const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
                      IDirect3DDevice9 **device);

} // namespace mullion::d3d9
