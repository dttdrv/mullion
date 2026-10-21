// a Direct3D 9 device: a D3D11 device of this build's over the adapter the
// Metal device is, the implicit swap chain a window's frames are shown
// through, and the fixed-function path's pre-transformed geometry: vertex and
// index buffers, one stream source, the draws that read them, and the render
// state, depth-stencil surfaces and render targets they are drawn with and into.
// textures, the world transform and a game's own shaders come next
// (src/d3d9/README).
#include "d3d9_device.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_fixed_function.hpp"
#include "d3d9_state.hpp"
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

  // the fixed-function path a device draws through, made once with the device: a device without it can clear and
  // present, but not draw, and this milestone's device draws
  hr = fixed_function_.initialize(d3d11_device_.Get());
  if (FAILED(hr))
    return D3DERR_NOTAVAILABLE;

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

  // the viewport of a device that has drawn nothing yet is its chain's whole back buffer, as D3D9's is
  viewport_ = D3DVIEWPORT9{0, 0, width_, height_, 0.0f, 1.0f};

  // a chain made with an automatic depth-stencil surface gets one, so a game that asked for depth has it from its
  // first frame rather than drawing every pixel in the order it drew them
  if (present_.EnableAutoDepthStencil) {
    ComPtr<IDirect3DSurface9> auto_depth;
    HRESULT depth_hr = CreateDepthStencilSurface(width_, height_, present_.AutoDepthStencilFormat,
                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &auto_depth, nullptr);
    if (SUCCEEDED(depth_hr))
      depth_hr = SetDepthStencilSurface(auto_depth.Get());
    if (FAILED(depth_hr)) {
      ERR("D3D9: no automatic depth-stencil surface of format {}: {}", (uint32_t)present_.AutoDepthStencilFormat,
          (uint32_t)depth_hr);
      return D3DERR_INVALIDCALL;
    }
  }
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

// what a frame is drawn into: the surface SetRenderTarget named, or the back buffer when it named none. a target of
// the app's is a surface of this device's, so it outlives the reference the app holds.
HRESULT
MTLD3D9Device::ensure_render_target() {
  if (current_target_view_)
    return S_OK;
  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  current_target_ = back_buffer_surface_;
  current_target_view_ = render_target_;
  target_width_ = width_;
  target_height_ = height_;
  return S_OK;
}

HRESULT
MTLD3D9Device::device_surface(IDirect3DSurface9 *surface, bool depth_stencil, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      ours->depth_stencil() != depth_stencil)
    return D3DERR_INVALIDCALL;
  *out = ours.Detach();
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
  // the next frame is drawn into the buffer this frame's view addresses: the chain turns its buffers over by giving
  // this buffer the next buffer's storage, so this device keeps its view and surface of buffer 0 across a Present
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
MTLD3D9Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float depth, DWORD stencil) {
  if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return S_OK;

  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  if (count && rects) {
    ERR_ONCE("D3D9: clearing part of a surface is not implemented yet; the whole of it is cleared.");
  }
  if (flags & D3DCLEAR_TARGET) {
    const float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
    context_->ClearRenderTargetView(current_target_view_.Get(), rgba);
  }
  // a device made without a depth-stencil surface has nothing to clear of one, as D3D9's has not
  UINT surfaces = 0;
  if ((flags & D3DCLEAR_ZBUFFER) && depth_stencil_view_)
    surfaces |= D3D11_CLEAR_DEPTH;
  if ((flags & D3DCLEAR_STENCIL) && depth_stencil_view_)
    surfaces |= D3D11_CLEAR_STENCIL;
  if (surfaces)
    context_->ClearDepthStencilView(depth_stencil_view_.Get(), surfaces, depth, (UINT8)stencil);
  return S_OK;
}

HRESULT
MTLD3D9Device::source_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      ours->lockable() || ours->depth_stencil())
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
MTLD3D9Device::GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) {
  if (index != 0 || !surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  *surface = current_target_.Get();
  (*surface)->AddRef();
  return S_OK;
}

// a surface SetRenderTarget or SetDepthStencilSurface can take: a texture this device draws into, or tests against
HRESULT
MTLD3D9Device::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                  DWORD, WINBOOL lockable, IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle || !width || !height)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (multisample != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;
  if (lockable) {
    ERR_ONCE("D3D9: a render target whose bytes the CPU locks is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, false, context_));
  *surface = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                         DWORD, WINBOOL, IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle || !width || !height)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (multisample != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;
  const DXGI_FORMAT dxgi_format = to_dxgi_depth_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN) {
    ERR_ONCE("D3D9: this build has no depth-stencil surface of that format.");
    return D3DERR_INVALIDCALL;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, false, context_, true));
  *surface = result.Detach();
  return S_OK;
}

// a target of a size the depth-stencil surface does not have is refused: the two are one target's, and D3D9 refuses
// a pair whose sizes differ rather than drawing with half of one
HRESULT
MTLD3D9Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) {
  if (index != 0)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, false, &ours)))
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc, depth_desc;
  ours->texture()->GetDesc(&desc);
  if (depth_stencil_) {
    depth_stencil_->texture()->GetDesc(&depth_desc);
    if (depth_desc.Width != desc.Width || depth_desc.Height != desc.Height) {
      ERR_ONCE("D3D9: a render target of another size than the depth-stencil surface is not taken.");
      return D3DERR_INVALIDCALL;
    }
  }

  ComPtr<ID3D11RenderTargetView> view;
  if (FAILED(d3d11_device_->CreateRenderTargetView(ours->texture(), nullptr, &view)))
    return D3DERR_INVALIDCALL;
  current_target_ = surface;
  current_target_view_ = view;
  target_width_ = desc.Width;
  target_height_ = desc.Height;
  // a target brings its own viewport: D3D9 places it on the target SetRenderTarget set (Wine's
  // device_reset_viewport_state, dlls/d3d9/device.c), so a game that switches targets and draws without another
  // SetViewport draws over the new target rather than over the old one's place in it
  viewport_ = D3DVIEWPORT9{0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
  return S_OK;
}

HRESULT
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *surface) {
  if (!surface) {
    depth_stencil_.Reset();
    depth_stencil_view_.Reset();
    return S_OK;
  }
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, true, &ours)))
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;

  D3D11_TEXTURE2D_DESC desc;
  ours->texture()->GetDesc(&desc);
  if (desc.Width != target_width_ || desc.Height != target_height_) {
    ERR_ONCE("D3D9: a depth-stencil surface of another size than the render target is not taken.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11DepthStencilView> view;
  if (FAILED(d3d11_device_->CreateDepthStencilView(ours->texture(), nullptr, &view)))
    return D3DERR_INVALIDCALL;
  depth_stencil_.Attach(static_cast<MTLD3D9Surface *>(ours.Detach()));
  depth_stencil_view_ = view;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **surface) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = depth_stencil_.Get();
  if (*surface)
    (*surface)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetViewport(const D3DVIEWPORT9 *viewport) {
  if (!viewport)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  // a viewport is a rectangle of the target, and its depths are the device's 0 to 1
  if (!viewport->Width || !viewport->Height || viewport->X + viewport->Width > target_width_ ||
      viewport->Y + viewport->Height > target_height_ || viewport->MinZ < 0.0f || viewport->MaxZ > 1.0f ||
      viewport->MinZ > viewport->MaxZ) {
    ERR_ONCE("D3D9: that viewport is not one of the target's; the device keeps the one it had.");
    return D3DERR_INVALIDCALL;
  }
  viewport_ = *viewport;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetViewport(D3DVIEWPORT9 *viewport) {
  if (!viewport)
    return D3DERR_INVALIDCALL;
  *viewport = viewport_;
  return S_OK;
}

HRESULT
MTLD3D9Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if (!RenderState::supported(state)) {
    ERR_ONCE("D3D9: render state {} is not honored yet; the call is refused rather than ignored.", (int)state);
    return D3DERR_INVALIDCALL;
  }
  if (!RenderState::usable_value(state, value)) {
    ERR_ONCE("D3D9: render state {} cannot take {}; the device keeps the value it had.", (int)state, value);
    return D3DERR_INVALIDCALL;
  }
  return render_state_.set(state, value);
}

HRESULT
MTLD3D9Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) {
  if (!value || !RenderState::supported(state))
    return D3DERR_INVALIDCALL;
  return render_state_.get(state, value);
}

// the vertices (or indices, which are one per vertex of a primitive) a primitive count is made of, for the topologies
// D3D11 has a primitive for. D3DPT_TRIANGLEFAN has none: this milestone refuses it rather than draws it wrong.
bool
MTLD3D9Device::topology_count(D3DPRIMITIVETYPE type, UINT primitive_count, UINT &count) {
  switch (type) {
  case D3DPT_POINTLIST:
    count = primitive_count;
    return true;
  case D3DPT_LINELIST:
    count = primitive_count * 2;
    return true;
  case D3DPT_LINESTRIP:
    count = primitive_count + 1;
    return true;
  case D3DPT_TRIANGLELIST:
    count = primitive_count * 3;
    return true;
  case D3DPT_TRIANGLESTRIP:
    count = primitive_count + 2;
    return true;
  default:
    return false;
  }
}

D3D11_PRIMITIVE_TOPOLOGY
MTLD3D9Device::topology_of(D3DPRIMITIVETYPE type) {
  switch (type) {
  case D3DPT_POINTLIST:
    return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
  case D3DPT_LINELIST:
    return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
  case D3DPT_LINESTRIP:
    return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
  case D3DPT_TRIANGLELIST:
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  case D3DPT_TRIANGLESTRIP:
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  default:
    return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  }
}

// a draw's target: the surface SetRenderTarget named (the back buffer until it names another), the depth-stencil
// surface a draw tests against, the render state, and the viewport and shaders the fixed-function path binds
HRESULT
MTLD3D9Device::prepare_draw() {
  if (!FixedFunctionPipeline::supports(fvf_))
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  ID3D11RenderTargetView *target = current_target_view_.Get();
  context_->OMSetRenderTargets(1, &target, depth_stencil_view_.Get());
  hr = render_state_.apply(d3d11_device_.Get(), context_.Get());
  if (FAILED(hr))
    return hr;
  return fixed_function_.bind(context_.Get(), viewport_, render_state_.flat_shaded());
}

HRESULT
MTLD3D9Device::CreateVertexBuffer(
    UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **vertex_buffer, HANDLE *shared_handle
) {
  if (!vertex_buffer || shared_handle || !length)
    return D3DERR_INVALIDCALL;
  *vertex_buffer = nullptr;
  // a managed buffer is the same buffer here: its bytes are the CPU's when it is locked and the device's otherwise,
  // which is what every buffer of this milestone is
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING | D3DUSAGE_NPATCHES))
    return D3DERR_INVALIDCALL;

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = (length + 15u) & ~15u; // a D3D11 buffer is mapped in whole units of 16 bytes; D3D9's length stays
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Buffer> buffer;
  if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &buffer))) {
    ERR("D3D9: no vertex buffer of %u bytes", length);
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9VertexBuffer> result;
  result.Attach(new MTLD3D9VertexBuffer(
      static_cast<IDirect3DDevice9 *>(this), buffer, context_, length, usage, pool, fvf
  ));
  *vertex_buffer = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateIndexBuffer(
    UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9 **index_buffer,
    HANDLE *shared_handle
) {
  if (!index_buffer || shared_handle || !length)
    return D3DERR_INVALIDCALL;
  *index_buffer = nullptr;
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING))
    return D3DERR_INVALIDCALL;
  const UINT index_size = format == D3DFMT_INDEX32 ? 4 : 2;
  if (length % index_size)
    return D3DERR_INVALIDCALL;

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = (length + 15u) & ~15u; // as a vertex buffer's: whole 16-byte units for the map
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Buffer> buffer;
  if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &buffer)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9IndexBuffer> result;
  result.Attach(new MTLD3D9IndexBuffer(
      static_cast<IDirect3DDevice9 *>(this), buffer, context_, length, usage, pool, format
  ));
  *index_buffer = result.Detach();
  return S_OK;
}

// the one vertex format this milestone transforms; a failed SetFVF leaves the device's last format, as D3D9's does
HRESULT
MTLD3D9Device::SetFVF(DWORD fvf) {
  if (!fvf) {
    fvf_ = 0;
    return S_OK;
  }
  if (!FixedFunctionPipeline::supports(fvf)) {
    ERR_ONCE("D3D9: only D3DFVF_XYZRHW | D3DFVF_DIFFUSE is transformed yet.");
    return D3DERR_INVALIDCALL;
  }
  fvf_ = fvf;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetFVF(DWORD *fvf) {
  if (!fvf)
    return D3DERR_INVALIDCALL;
  *fvf = fvf_;
  return S_OK;
}

HRESULT
MTLD3D9Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) {
  if (stream != 0)
    return D3DERR_INVALIDCALL;
  if (!buffer) {
    if (offset || stride)
      return D3DERR_INVALIDCALL;
    stream_source_.Reset();
    stream_buffer_.Reset();
    stream_offset_ = 0;
    stream_stride_ = 0;
    return S_OK;
  }

  ComPtr<IMTLD3D9Buffer> ours;
  if (FAILED(buffer->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(ours.GetAddressOf()))) || !stride ||
      offset > ours->size())
    return D3DERR_INVALIDCALL;
  stream_source_ = buffer;
  stream_buffer_ = ours;
  stream_offset_ = offset;
  stream_stride_ = stride;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) {
  if (stream != 0 || !buffer || !offset || !stride)
    return D3DERR_INVALIDCALL;
  *buffer = stream_source_.Get();
  if (*buffer)
    (*buffer)->AddRef();
  *offset = stream_offset_;
  *stride = stream_stride_;
  return S_OK;
}

// instancing is not drawn yet: the setting of a stream that does not instance is the one thing this accepts
HRESULT
MTLD3D9Device::SetStreamSourceFreq(UINT stream, UINT setting) {
  return stream == 0 && setting == 1 ? S_OK : D3DERR_INVALIDCALL;
}

HRESULT
MTLD3D9Device::GetStreamSourceFreq(UINT stream, UINT *setting) {
  if (stream != 0 || !setting)
    return D3DERR_INVALIDCALL;
  *setting = 1;
  return S_OK;
}

HRESULT
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *indices) {
  if (!indices) {
    indices_.Reset();
    index_buffer_.Reset();
    return S_OK;
  }
  ComPtr<IMTLD3D9Buffer> ours;
  if (FAILED(indices->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(ours.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  indices_ = indices;
  index_buffer_ = ours;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **indices) {
  if (!indices)
    return D3DERR_INVALIDCALL;
  *indices = indices_.Get();
  if (*indices)
    (*indices)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count) {
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!stream_buffer_ || stream_stride_ != kPreTransformedStride)
    return D3DERR_INVALIDCALL;
  // the vertices the draw reads are the stream's, from the offset it was set with
  const uint64_t vertices = (uint64_t)start_vertex + count;
  if (stream_offset_ + vertices * stream_stride_ > stream_buffer_->size())
    return D3DERR_INVALIDCALL;

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = stream_buffer_->buffer();
  context_->IASetVertexBuffers(0, 1, &buffer, &stream_stride_, &stream_offset_);
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->Draw(count, start_vertex);
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE type, INT base_vertex, UINT min_vertex, UINT vertex_count, UINT start_index, UINT primitive_count
) {
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!stream_buffer_ || stream_stride_ != kPreTransformedStride || !index_buffer_ || !vertex_count ||
      min_vertex >= vertex_count)
    return D3DERR_INVALIDCALL;

  const UINT index_size = index_buffer_->format() == D3DFMT_INDEX32 ? 4 : 2;
  if ((uint64_t)(start_index + count) * index_size > index_buffer_->size())
    return D3DERR_INVALIDCALL;
  const int64_t first_vertex = (int64_t)base_vertex + min_vertex;
  const uint64_t stream_vertices = (stream_buffer_->size() - stream_offset_) / stream_stride_;
  if (first_vertex < 0 || (uint64_t)first_vertex + vertex_count > stream_vertices)
    return D3DERR_INVALIDCALL;

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = stream_buffer_->buffer();
  context_->IASetVertexBuffers(0, 1, &buffer, &stream_stride_, &stream_offset_);
  context_->IASetIndexBuffer(
      index_buffer_->buffer(),
      index_buffer_->format() == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0
  );
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->DrawIndexed(count, start_index, base_vertex);
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertices, UINT stride
) {
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!FixedFunctionPipeline::supports(fvf_) || !vertices || stride != kPreTransformedStride)
    return D3DERR_INVALIDCALL;

  const uint64_t bytes = (uint64_t)count * stride;
  if (bytes > UINT_MAX)
    return D3DERR_INVALIDCALL;
  if (!up_scratch_ || up_scratch_size_ < bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(bytes, 1024);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    up_scratch_ = scratch;
    up_scratch_size_ = desc.ByteWidth;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(up_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  memcpy(mapped.pData, vertices, (size_t)bytes);
  context_->Unmap(up_scratch_.Get(), 0);

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = up_scratch_.Get();
  UINT offset = 0;
  context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->Draw(count, 0);
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
