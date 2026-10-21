// a Direct3D 9 device: a D3D11 device of this build's over the adapter the
// Metal device is, and the implicit swap chain a window's frames are shown
// through. this milestone has the device, its back buffer and Present; draws,
// resources and state that reaches a shader come next (src/d3d9/README).
#include "d3d9_device.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_swapchain.hpp"
#include <algorithm>

namespace mullion::d3d9 {

namespace {

float channel(D3DCOLOR colour, unsigned shift) { return (float)((colour >> shift) & 0xFF) / 255.0f; }

} // namespace

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

  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) override {
    if (index != 0)
      return D3DERR_INVALIDCALL;
    return back_buffer(0, surface);
  }

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
                                          D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
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
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                               IDirect3DVertexBuffer9 **ppVertexBuffer,
                                               HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                              IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                               D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                               WINBOOL Lockable, IDirect3DSurface9 **ppSurface,
                                               HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                                      WINBOOL Discard, IDirect3DSurface9 **ppSurface,
                                                      HANDLE *pSharedHandle) override {
    return D3DERR_INVALIDCALL;
  }
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
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *viewport) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *material) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9 *light) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, WINBOOL Enable) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, WINBOOL *pEnable) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *clip_status) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override {
    return D3DERR_INVALIDCALL;
  }
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
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                          UINT PrimitiveCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex,
                                                 UINT NumVertices, UINT startIndex, UINT primCount) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data,
                                            UINT stride) override {
    return D3DERR_INVALIDCALL;
  }
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
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) override { return D3DERR_INVALIDCALL; }
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
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes,
                                            UINT Stride) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
                                            UINT *OffsetInBytes, UINT *pStride) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Divider) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT *Divider) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override { return D3DERR_INVALIDCALL; }
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
  // the back buffer of the frame being drawn: a flip model chain turns its
  // buffers over at every Present, so this is asked of the chain again after
  // one, and the buffer taken before it is not the next frame's
  HRESULT ensure_back_buffer();
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
};

HRESULT
MTLD3D9Device::init(IDXGIFactory2 *factory) {
  const DXGI_FORMAT format = to_dxgi_format(present_.BackBufferFormat);
  if (format == DXGI_FORMAT_UNKNOWN || !window_ || !present_.Windowed)
    return D3DERR_INVALIDCALL;
  if (present_.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;

  HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, &context_);
  if (FAILED(hr)) {
    ERR("D3D9: no D3D11 device over the adapter: 0x%08x", hr);
    return D3DERR_NOTAVAILABLE;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = present_.BackBufferWidth;
  desc.Height = present_.BackBufferHeight;
  desc.Format = format;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  // a flip model chain shows the buffer drawn into last and turns over to
  // another one, which is what a D3D9 D3DSWAPEFFECT_DISCARD chain does, and it
  // has at least two buffers to turn between
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = std::max<UINT>(present_.BackBufferCount, 2);
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = 0;

  hr = factory->CreateSwapChainForHwnd(d3d11_device_.Get(), window_, &desc, nullptr, nullptr, &swapchain_);
  if (FAILED(hr)) {
    ERR("D3D9: no swap chain for the window: 0x%08x", hr);
    return D3DERR_NOTAVAILABLE;
  }

  DXGI_SWAP_CHAIN_DESC1 created{};
  swapchain_->GetDesc1(&created);
  width_ = created.Width;
  height_ = created.Height;
  present_.BackBufferWidth = created.Width;
  present_.BackBufferHeight = created.Height;
  present_.BackBufferCount = created.BufferCount;
  present_.SwapEffect = D3DSWAPEFFECT_DISCARD;

  swap_chain_.Attach(new MTLD3D9SwapChain(static_cast<IDirect3DDevice9 *>(this),
                                          static_cast<IMTLD3D9PresentSource *>(this), present_));
  return S_OK;
}

HRESULT
MTLD3D9Device::ensure_back_buffer() {
  if (back_buffer_texture_)
    return S_OK;

  ComPtr<ID3D11Texture2D> buffer;
  HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
  if (FAILED(hr)) {
    ERR("D3D9: the swap chain has no buffer 0: 0x%08x", hr);
    return D3DERR_INVALIDCALL;
  }
  ComPtr<ID3D11RenderTargetView> view;
  hr = d3d11_device_->CreateRenderTargetView(buffer.Get(), nullptr, &view);
  if (FAILED(hr)) {
    ERR("D3D9: the back buffer takes no render target view: 0x%08x", hr);
    return D3DERR_INVALIDCALL;
  }

  back_buffer_texture_ = buffer;
  render_target_ = view;
  back_buffer_surface_.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), back_buffer_texture_,
                                                 present_.BackBufferFormat, false, context_));
  return S_OK;
}

HRESULT
MTLD3D9Device::present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD) {
  if (!swapchain_)
    return D3DERR_INVALIDCALL;

  // what was cleared and drawn into goes to the window before the chain turns
  // over
  context_->Flush();
  HRESULT hr = swapchain_->Present(to_sync_interval(present_.PresentationInterval), 0);
  if (FAILED(hr)) {
    ERR("D3D9: Present failed: 0x%08x", hr);
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
  }
  // a flip model chain shows the buffer just presented and the next frame is
  // drawn into another one: the surface and view of the frame before are not
  // the next frame's, so they are taken again (DXGI's flip model says a buffer
  // must be released before it can be presented again, and a chain that reused
  // its view would present nothing)
  invalidate_back_buffer();
  return S_OK;
}

HRESULT
MTLD3D9Device::back_buffer(UINT index, IDirect3DSurface9 **surface) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (index != 0)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  *surface = back_buffer_surface_.Get();
  (*surface)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::display_mode(D3DDISPLAYMODE *mode) {
  if (!mode)
    return D3DERR_INVALIDCALL;
  return direct3d_->GetAdapterDisplayMode(adapter_ordinal_, mode);
}

HRESULT
MTLD3D9Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float, DWORD) {
  // a depth or stencil buffer is a resource this milestone has none of: a clear
  // of one has nothing to clear
  if (!(flags & D3DCLEAR_TARGET))
    return S_OK;

  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  if (count && rects) {
    ERR_ONCE("D3D9: clearing part of the back buffer is not implemented yet; "
             "the whole of it is cleared.");
  }
  const float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
  context_->ClearRenderTargetView(render_target_.Get(), rgba);
  return S_OK;
}

HRESULT
MTLD3D9Device::source_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      ours->lockable())
    return D3DERR_INVALIDCALL;
  *out = ours.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::staging_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      !ours->lockable())
    return D3DERR_INVALIDCALL;
  *out = ours.Detach();
  return S_OK;
}

// a frame's bytes read back: the copy of a render target into a surface whose
// bytes are the CPU's, which the surface then locks (GetRenderTargetData)
HRESULT
MTLD3D9Device::GetRenderTargetData(IDirect3DSurface9 *render_target, IDirect3DSurface9 *destination) {
  ComPtr<IMTLD3D9Surface> source, target;
  if (FAILED(source_surface(render_target, &source)) || FAILED(staging_surface(destination, &target)))
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC source_desc, target_desc;
  source->texture()->GetDesc(&source_desc);
  target->texture()->GetDesc(&target_desc);
  if (source_desc.Width != target_desc.Width || source_desc.Height != target_desc.Height ||
      source_desc.Format != target_desc.Format)
    return D3DERR_INVALIDCALL;

  context_->CopyResource(target->texture(), source->texture());
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                           IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  // a default pool surface is a texture a device may draw into, which this
  // milestone has none of: the surfaces a frame is read back into are the CPU's
  if (pool != D3DPOOL_SYSTEMMEM)
    return D3DERR_INVALIDCALL;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !width || !height)
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, true, context_));
  *surface = result.Detach();
  return S_OK;
}

HRESULT
create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
              const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
              IDirect3DDevice9 **device) {
  if (!device || !factory || !adapter)
    return D3DERR_INVALIDCALL;
  *device = nullptr;

  ComPtr<MTLD3D9Device> result;
  result.Attach(new MTLD3D9Device(direct3d, ComPtr<IDXGIAdapter1>(adapter), adapter_ordinal, caps, window,
                                  behavior_flags, present));
  HRESULT hr = result->init(factory);
  if (FAILED(hr))
    return hr;

  *device = result.Detach();
  return S_OK;
}

} // namespace mullion::d3d9
