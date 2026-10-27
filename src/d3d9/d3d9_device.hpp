// the Direct3D 9 device this build makes: its interfaces and everything a frame reads. the device's source is split by
// what a frame needs -- d3d9_device.cpp has the object, its chain, its back buffer and the surfaces a frame is read
// back into, d3d9_device_targets.cpp the render states, depth-stencil surfaces, render targets and viewport a frame is
// drawn with and into, d3d9_device_draw.cpp the geometry a draw reads, and d3d9_device_texture.cpp the textures a draw
// samples with the stage that samples them. nothing else of this build sees the class, only the interfaces it has.
#pragma once
#include "d3d9_buffer.hpp"
#include "d3d9_common.hpp"
#include "d3d9_fixed_function.hpp"
#include "d3d9_state.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_texture.hpp"

namespace mullion::d3d9 {

class MTLD3D9Device final : private Object9, public IDirect3DDevice9, public IMTLD3D9PresentSource {
public:
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
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IMTLD3D9PresentSource
  HRESULT present(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty, DWORD flags) override;
  HRESULT back_buffer(UINT index, IDirect3DSurface9 **surface) override;
  HRESULT display_mode(D3DDISPLAYMODE *mode) override;

  // the device's own
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return S_OK; }

  // not measured yet: nothing has asked this build how much memory the Metal
  // device's textures take
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return 0; }

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

  // D3D9 has no state to lose yet: the frames of this milestone are what the
  // device clears to
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *) override {
    ERR_ONCE("D3D9: Reset is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }

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
                                                      IDirect3DSwapChain9 **pSwapChain) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(WINBOOL bEnableDialogs) override { return D3DERR_INVALIDCALL; }
  void STDMETHODCALLTYPE SetGammaRamp(UINT swapchain_idx, DWORD flags, const D3DGAMMARAMP *ramp) override {}
  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) override {}
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                          D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                D3DFORMAT Format, D3DPOOL Pool,
                                                IDirect3DVolumeTexture9 **ppVolumeTexture,
                                                HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                              IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                               IDirect3DVertexBuffer9 **vertex_buffer,
                                               HANDLE *shared_handle) override;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                              IDirect3DIndexBuffer9 **index_buffer, HANDLE *shared_handle) override;
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *src_surface, const RECT *src_rect,
                                          IDirect3DSurface9 *dst_surface, const POINT *dst_point) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                                          IDirect3DBaseTexture9 *pDestinationTexture) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *src_surface, const RECT *src_rect,
                                        IDirect3DSurface9 *dst_surface, const RECT *dst_rect,
                                        D3DTEXTUREFILTERTYPE filter) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override {
    return D3DERR_INVALIDCALL;
  }
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
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *viewport) override;
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *viewport) override;
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *material) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9 *light) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, WINBOOL Enable) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, WINBOOL *pEnable) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) override;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *clip_status) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *rect) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override { return D3DERR_INVALIDCALL; }
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
                                                   D3DFORMAT index_format, const void *data, UINT stride) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                                            IDirect3DVertexBuffer9 *pDestBuffer,
                                            IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements,
                                                    IDirect3DVertexDeclaration9 **declaration) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *fvf) override;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *byte_code, IDirect3DVertexShader9 **shader) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT reg_idx, const float *data, UINT count) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float *pConstantData,
                                                     UINT Vector4fCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT reg_idx, const int *data, UINT count) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int *pConstantData,
                                                     UINT Vector4iCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT reg_idx, const WINBOOL *data, UINT count) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, WINBOOL *pConstantData,
                                                     UINT BoolCount) override {
    return D3DERR_INVALIDCALL;
  }
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
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, IDirect3DPixelShader9 **shader) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT reg_idx, const float *data, UINT count) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float *pConstantData,
                                                    UINT Vector4fCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT reg_idx, const int *data, UINT count) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int *pConstantData,
                                                    UINT Vector4iCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT reg_idx, const WINBOOL *data, UINT count) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, WINBOOL *pConstantData,
                                                    UINT BoolCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segment_count,
                                          const D3DRECTPATCH_INFO *patch_info) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segment_count,
                                         const D3DTRIPATCH_INFO *patch_info) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override {
    return D3DERR_INVALIDCALL;
  }

private:
  // the back buffer of the frame being drawn: taken from the chain once, and the same buffer from then on. a flip
  // model chain turns its buffers over at every Present, but it does so by giving the buffers' storage to the next
  // buffer's object, so this device's view and surface of buffer 0 address whatever the next frame is drawn into (the
  // frames this milestone's tests draw, present and draw again check that, and a Present that took the buffer again
  // changed nothing they could see)
  HRESULT ensure_back_buffer();
  // the frame's target: the back buffer until SetRenderTarget names another one
  HRESULT ensure_render_target();
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

  ComPtr<ID3D11Device> d3d11_device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapchain_;
  ComPtr<MTLD3D9SwapChain> swap_chain_;

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
  RenderState render_state_;

  // the fixed-function path, and the state a draw reads: the FVF, one stream source and the indices (what this
  // milestone draws with; SetStreamSourceFreq is 1, no instancing)
  FixedFunctionPipeline fixed_function_;
  DWORD fvf_ = 0;
  ComPtr<IDirect3DVertexBuffer9> stream_source_;
  ComPtr<IMTLD3D9Buffer> stream_buffer_;
  UINT stream_offset_ = 0;
  UINT stream_stride_ = 0;
  ComPtr<IDirect3DIndexBuffer9> indices_;
  ComPtr<IMTLD3D9Buffer> index_buffer_;
  ComPtr<ID3D11Buffer> up_scratch_;
  UINT up_scratch_size_ = 0;

  // the texture stage 0 samples, the sampler it samples through and the operation the stage performs
  // (d3d9_device_texture.cpp)
  ComPtr<IDirect3DBaseTexture9> texture_;
  ComPtr<IMTLD3D9Texture> texture_source_;
  SamplerState9 sampler_state_;
  TextureStageState9 texture_stage_;

  // what every draw needs: the chain's back buffer as it is now, and the fixed-function shaders over it
  HRESULT prepare_draw();
  // the vertices or indices a primitive count is made of, with whether this milestone draws the topology at all
  static bool topology_count(D3DPRIMITIVETYPE type, UINT primitive_count, UINT &count);
  static D3D11_PRIMITIVE_TOPOLOGY topology_of(D3DPRIMITIVETYPE type);
  HRESULT draw_vertices(
      D3DPRIMITIVETYPE type, UINT primitive_count, UINT count, UINT start_vertex, ID3D11Buffer *buffer, UINT stride,
      UINT offset
  );
};

// makes a device over `adapter` for `window`, with the implicit swap chain
// CreateDevice's present parameters ask for
HRESULT create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
                      const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
                      IDirect3DDevice9 **device);

} // namespace mullion::d3d9
