#pragma once
#include "mullion_buffer.hpp"
#include "mullion_format.hpp"
#include "mullion_counter.hpp"
#include "mullion_dynamic.hpp"
#include "mullion_staging.hpp"
#include "mullion_texture.hpp"
#include "mullion_keyed_mutex.hpp"
#include "d3d11_device_child.hpp"
#include "com/com_pointer.hpp"
#include "com/com_guid.hpp"
#include "d3d11_view.hpp"
#include "dxgi_resource.hpp"
#include "d3d11_surface.hpp"
#include "log/log.hpp"
#include "../d3d10/d3d10_buffer.hpp"
#include "../d3d10/d3d10_texture.hpp"
#include "../d3d10/d3d10_view.hpp"
#include <memory>
#include <type_traits>
#include <random>
#include <cstdio>

DEFINE_COM_INTERFACE("9a6f6549-d4b1-45ea-8794-8503d190d3d1",
                     IMTLMinLODClampable)
    : public IUnknown {
  virtual void SetMinLOD(float MinLOD) = 0;
  virtual float GetMinLOD() = 0;
};

namespace mullion {

template <typename RESOURCE_DESC>
void UpgradeResourceDescription(const RESOURCE_DESC *pSrc, RESOURCE_DESC &dst) {
  dst = *pSrc;
}
template <typename RESOURCE_DESC>
void DowngradeResourceDescription(const RESOURCE_DESC &src,
                                  RESOURCE_DESC *pDst) {
  *pDst = src;
}

template <typename RESOURCE_DESC_SRC, typename RESOURCE_DESC_DST>
void UpgradeResourceDescription(const RESOURCE_DESC_SRC *pSrc,
                                RESOURCE_DESC_DST &dst);
template <typename RESOURCE_DESC_SRC, typename RESOURCE_DESC_DST>
void DowngradeResourceDescription(const RESOURCE_DESC_SRC &src,
                                  RESOURCE_DESC_DST *pDst);

template <typename VIEW_DESC>
void UpgradeViewDescription(const VIEW_DESC *pSrc, VIEW_DESC &dst) {
  dst = *pSrc;
}
template <typename VIEW_DESC>
void DowngradeViewDescription(const VIEW_DESC &src, VIEW_DESC *pDst) {
  *pDst = src;
}

template <typename VIEW_DESC_SRC, typename VIEW_DESC_DST>
void UpgradeViewDescription(const VIEW_DESC_SRC *pSrc, VIEW_DESC_DST &dst);
template <typename VIEW_DESC_SRC, typename VIEW_DESC_DST>
void DowngradeViewDescription(const VIEW_DESC_SRC &src, VIEW_DESC_DST *pDst);

struct tag_buffer {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_BUFFER;
  using COM = ID3D11Buffer;
  using COM_IMPL = ID3D11Buffer;
  using DESC = D3D11_BUFFER_DESC;
  using DESC1 = D3D11_BUFFER_DESC;
  using D3D10 = ID3D10Buffer;
  using D3D10_IMPL = MTLD3D10Buffer;
  static constexpr std::string_view debug_name = "buffer";
};

struct tag_texture_1d {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_TEXTURE1D;
  using COM = ID3D11Texture1D;
  using COM_IMPL = ID3D11Texture1D;
  using DESC = D3D11_TEXTURE1D_DESC;
  using DESC1 = D3D11_TEXTURE1D_DESC;
  using D3D10 = ID3D10Texture1D;
  using D3D10_IMPL = MTLD3D10Texture1D;
  static constexpr std::string_view debug_name = "tex1d";
};

struct tag_texture_2d {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_TEXTURE2D;
  using COM = ID3D11Texture2D;
  using COM_IMPL = ID3D11Texture2D1;
  using DESC = D3D11_TEXTURE2D_DESC;
  using DESC1 = D3D11_TEXTURE2D_DESC1;
  using D3D10 = ID3D10Texture2D;
  using D3D10_IMPL = MTLD3D10Texture2D;
  static constexpr std::string_view debug_name = "tex2d";
};

struct tag_texture_3d {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_TEXTURE3D;
  using COM = ID3D11Texture3D;
  using COM_IMPL = ID3D11Texture3D1;
  using DESC = D3D11_TEXTURE3D_DESC;
  using DESC1 = D3D11_TEXTURE3D_DESC1;
  using D3D10 = ID3D10Texture3D;
  using D3D10_IMPL = MTLD3D10Texture3D;
  static constexpr std::string_view debug_name = "tex3d";
};

struct WrappedResourceState {
  Rc<Buffer> buffer;
  Rc<Texture> texture;
  struct Use {
    ResourceSubsetState subset;
    uint64_t read = 0, written = 0;
  };
  std::vector<Use> uses;
  uint64_t returned_seq = 0;
};

struct D3D11ResourceCommon : ID3D11Resource {
    /* ID3D11Buffer::GetDesc */
    /* ID3D11Texture1D::GetDesc */
    /* ID3D11Texture2D::GetDesc */
    /* ID3D11Texture3D::GetDesc */
  virtual void STDMETHODCALLTYPE GetDesc(void *pDesc) = 0;
    /* ID3D11Texture2D1::GetDesc */
    /* ID3D11Texture3D1::GetDesc */
  virtual void STDMETHODCALLTYPE GetDesc1(void *pDesc) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  virtual ULONG GetPrivateRefCount() = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc, ID3D11UnorderedAccessView1 **ppView) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc, ID3D11RenderTargetView1 **ppView) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppView) = 0;
  virtual HRESULT GetSharedHandle(HANDLE *pSharedHandle) = 0;
  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle) = 0;

  virtual Rc<StagingResource> staging(UINT Subresource) = 0;
  virtual Rc<DynamicBuffer> dynamicBuffer(UINT *pBufferLength, UINT *pBindFlags) = 0;
  virtual Rc<DynamicLinearTexture> dynamicLinearTexture(UINT *pBytesPerRow, UINT *pBytesPerImage) = 0;
  virtual Rc<DynamicBuffer> dynamicTexture(UINT Subresource, UINT *pBytesPerRow, UINT *pBytesPerImage) = 0;
  virtual bool CanMap(UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_DEVICE_CONTEXT_TYPE ContextType) = 0;

  Com<ID3D12Resource> wrapped_resource;
  std::shared_ptr<WrappedResourceState> wrapped_state;
  Rc<Buffer> buffer_{};
  Rc<Texture> texture_{};
  uint32_t bind_flags_{};
  DXGI_USAGE dxgi_usage_{};
  Rc<KeyedMutex> keyed_mutex_{};
  Rc<Counter> so_filled_{};

  const Rc<Buffer> &buffer() const {
    return buffer_;
  }
  const Rc<Texture> &texture() const {
    return texture_;
  }
  uint32_t bindFlags() const {
    return bind_flags_;
  }
  // how much of the buffer stream output has filled
  const Rc<Counter> &
  streamOutputFilled() {
    if (!so_filled_.ptr())
      so_filled_ = new Counter();
    return so_filled_;
  }
  bool
  hazardsFree() const {
    return (bind_flags_ & (D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET |
                           D3D11_BIND_DEPTH_STENCIL)) == 0;
  }
  const Rc<KeyedMutex> &keyedMutex() const {
    return keyed_mutex_;
  }
};

inline D3D11ResourceCommon* GetResourceCommon(ID3D11Resource *pResource) {
  return static_cast<D3D11ResourceCommon *>(pResource);
}

inline Rc<DynamicBuffer>
GetDynamicBuffer(ID3D11Resource *pResource, UINT *pBufferLength, UINT *pBindFlags) {
  return static_cast<D3D11ResourceCommon *>(pResource)->dynamicBuffer(pBufferLength, pBindFlags);
}
inline Rc<DynamicLinearTexture>
GetDynamicLinearTexture(ID3D11Resource *pResource, UINT *pBytesPerRow, UINT *pBytesPerImage) {
  return static_cast<D3D11ResourceCommon *>(pResource)->dynamicLinearTexture(pBytesPerRow, pBytesPerImage);
}
inline Rc<DynamicBuffer>
GetDynamicTexture(ID3D11Resource *pResource, UINT Subresource, UINT *pBufferLength, UINT *pBindFlags) {
  return static_cast<D3D11ResourceCommon *>(pResource)->dynamicTexture(Subresource, pBufferLength, pBindFlags);
}
inline Rc<StagingResource>
GetStagingResource(ID3D11Resource *pResource, UINT Subresource) {

  return static_cast<D3D11ResourceCommon *>(pResource)->staging(Subresource);
};
inline Rc<Texture>
GetTexture(ID3D11Resource *pResource, UINT plane = 0) {
  auto texture = static_cast<D3D11ResourceCommon *>(pResource)->texture();
  return texture ? texture->plane(plane) : nullptr;
}

template <typename TEXTURE_DESC>
void GetMipmapSize(const TEXTURE_DESC *pDesc, uint32_t level, uint32_t *pWidth,
                   uint32_t *pHeight, uint32_t *pDepth);

template <typename tag, typename... Base>
class TResourceBase : public MTLD3D11DeviceChild<D3D11ResourceCommon, Base...> {
public:
  TResourceBase(const tag::DESC1 &desc, MTLD3D11Device *device) :
      MTLD3D11DeviceChild<D3D11ResourceCommon, Base...>(device),
      desc(desc),
      dxgi_resource(this),
      keyed_mutex(this, device->GetImmediateContextPrivate()),
      d3d10(reinterpret_cast<tag::COM *>(this), device->GetImmediateContextPrivate()) {
    // D3D11ResourceCommonß::bind_flags_
    this->bind_flags_ = desc.BindFlags;
  }

  template <std::size_t n> HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    return E_NOINTERFACE;
  };

  template <std::size_t n, typename V, typename... Args>
  HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    if (riid == __uuidof(V)) {
      *ppvObject = ref_and_cast<V>(this);
      return S_OK;
    }
    return ResolveBase<n + 1, Args...>(riid, ppvObject);
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    auto hr = ResolveBase<0, Base...>(riid, ppvObject);
    if (SUCCEEDED(hr)) {
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Resource) ||
        riid == __uuidof(typename tag::COM) ||
        riid == __uuidof(typename tag::COM_IMPL)) {
      *ppvObject = ref_and_cast<D3D11ResourceCommon>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) ||
        riid == __uuidof(ID3D10Resource) ||
        riid == __uuidof(typename tag::D3D10)) {
      if (desc.MiscFlags & (D3D11_RESOURCE_MISC_RESOURCE_CLAMP | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
        return E_NOINTERFACE;
      *ppvObject = ref_and_cast<typename tag::D3D10>(&d3d10);
      return S_OK;
    }

    if (riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGIResource) || riid == __uuidof(IDXGIResource1)) {
      *ppvObject = ref(&dxgi_resource);
      return S_OK;
    }
  
    if (riid == __uuidof(IDXGIKeyedMutex)) {
      if (!(this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
        return E_NOINTERFACE;
      *ppvObject = ref(&keyed_mutex);
      return S_OK;
    }

    // a single-subresource 1D or 2D texture is its surface (Wine's test_texture1d_interfaces and IDXGISurface)
    if (riid == __uuidof(IDXGISurface) || riid == __uuidof(IDXGISurface1) || riid == __uuidof(IDXGISurface2)) {
      if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ||
                    tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        if (Subresources() == 1) {
          try {
            std::call_once(surfaces_once, [this] { surface.reset(Surface(0, true)); });
          } catch (const std::bad_alloc &) {
            return E_OUTOFMEMORY;
          }
          *ppvObject = ref(surface.get());
          return S_OK;
        }
      }
      return E_NOINTERFACE;
    }

    if (logQueryInterfaceError(__uuidof(typename tag::COM), riid)) {
      WARN("D3D11Resource(", tag::debug_name ,"): Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE GetDesc(void *pDesc) final {
    ::mullion::DowngradeResourceDescription(desc, (typename tag::DESC *)pDesc);
  }

  void STDMETHODCALLTYPE GetDesc1(void *pDesc) override { *(typename tag::DESC1 *)pDesc = desc; }

  void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) final {
    *pResourceDimension = tag::dimension;
  }

  bool
  CanMap(UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_DEVICE_CONTEXT_TYPE ContextType) final {
    UINT subresources = 1;
    if constexpr (tag::dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
      subresources = desc.MipLevels;
      if constexpr (tag::dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        subresources *= desc.ArraySize;
    }
    // D3D11.3 5.6.1.1 and D3D11_MAP/Map remarks: access and usage constrain the CPU's map, not internal updates.
    if (Subresource >= subresources || MapType < D3D11_MAP_READ || MapType > D3D11_MAP_WRITE_NO_OVERWRITE ||
        ((MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) && MapType >= D3D11_MAP_WRITE_DISCARD) ||
        (ContextType == D3D11_DEVICE_CONTEXT_DEFERRED && MapType < D3D11_MAP_WRITE_DISCARD))
      return false;
    if (desc.Usage == D3D11_USAGE_DYNAMIC) {
      if (MapType < D3D11_MAP_WRITE_DISCARD ||
          (MapType == D3D11_MAP_WRITE_NO_OVERWRITE && tag::dimension != D3D11_RESOURCE_DIMENSION_BUFFER))
        return false;
    } else if ((desc.Usage != D3D11_USAGE_STAGING && !this->wrapped_state) ||
               MapType > D3D11_MAP_READ_WRITE) {
      return false;
    }
    return ((MapType != D3D11_MAP_READ && MapType != D3D11_MAP_READ_WRITE) ||
            (desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) &&
           (MapType == D3D11_MAP_READ || (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE));
  }

  void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) final {}

  UINT STDMETHODCALLTYPE GetEvictionPriority() final { return DXGI_RESOURCE_PRIORITY_NORMAL; }

  virtual HRESULT GetDeviceInterface(REFIID riid, void **ppDevice) {
    Com<ID3D11Device> device;
    this->GetDevice(&device);
    return device->QueryInterface(riid, ppDevice);
  };

  // Wine's test_subresource_surface records Windows rejecting a whole volume, even its depth-one mips
  UINT
  Subresources() {
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
      return 1;
    else if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
      return desc.Depth == 1 ? desc.MipLevels : 0;
    else
      return desc.MipLevels * desc.ArraySize;
  }

  MTLD3D11Surface *
  Surface(UINT Index, bool Whole) {
    if (Index >= Subresources())
      return nullptr;
    DXGI_SURFACE_DESC surface_desc{0, 1, DXGI_FORMAT_UNKNOWN, {1, 0}};
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      surface_desc.Width = desc.ByteWidth;
    } else {
      UINT depth;
      GetMipmapSize(&desc, Index % desc.MipLevels, &surface_desc.Width, &surface_desc.Height, &depth);
      surface_desc.Format = desc.Format;
      if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        surface_desc.SampleDesc = desc.SampleDesc;
    }
    if (desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE)
      std::call_once(gdi_once, [this] { gdi = std::make_unique<MTLD3D11Surface::GDIState[]>(Subresources()); });
    return new MTLD3D11Surface(
        this, this->m_parent->GetImmediateContextPrivate(), Index, Whole, surface_desc, gdi ? &gdi[Index] : nullptr
    );
  }

  HRESULT
  CreateSubresourceSurface(UINT Index, IDXGISurface2 **ppSurface) {
    if (!ppSurface)
      return E_INVALIDARG;
    try {
      auto created = Surface(Index, false);
      if (!created)
        return E_INVALIDARG;
      *ppSurface = ref(created);
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  virtual HRESULT GetDXGIUsage(DXGI_USAGE *pUsage) {
    if (!pUsage) {
      return E_INVALIDARG;
    }
    *pUsage = this->dxgi_usage_ | (desc.BindFlags & D3D11_BIND_RENDER_TARGET ? DXGI_USAGE_RENDER_TARGET_OUTPUT : 0) |
              (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE ? DXGI_USAGE_SHADER_INPUT : 0) |
              (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS ? DXGI_USAGE_UNORDERED_ACCESS : 0);
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    return E_INVALIDARG;
  };
  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    return E_INVALIDARG;
  };
  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                         ID3D11RenderTargetView1 **ppView) override {
    return E_INVALIDARG;
  };
  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                         ID3D11DepthStencilView **ppView) override {
    return E_INVALIDARG;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    return E_INVALIDARG;
  }
  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    if (this->wrapped_resource && (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
      InitReturnPtr(pNTHandle);
      if (!pNTHandle)
        return E_INVALIDARG;
      Com<ID3D12Device> owner;
      HRESULT hr = this->wrapped_resource->GetDevice(IID_PPV_ARGS(&owner));
      return FAILED(hr) ? hr : owner->CreateSharedHandle(this->wrapped_resource.ptr(), Attributes, Access, pName, pNTHandle);
    }
    return E_INVALIDARG;
  }

protected:
  tag::DESC1 desc;
  MTLDXGIResource<TResourceBase<tag, Base...>> dxgi_resource;
  MTLDXGIKeyedMutex<TResourceBase<tag, Base...>, IMTLD3D11DeviceContext> keyed_mutex;
  tag::D3D10_IMPL d3d10;
  std::once_flag surfaces_once;
  std::unique_ptr<MTLD3D11Surface> surface;
  std::once_flag gdi_once;
  std::unique_ptr<MTLD3D11Surface::GDIState[]> gdi;
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11RenderTargetView>
struct tag_render_target_view {
  using COM = ID3D11RenderTargetView;
  using COM1 = ID3D11RenderTargetView1;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_RENDER_TARGET_VIEW_DESC;
  using DESC1 = D3D11_RENDER_TARGET_VIEW_DESC1;
  using D3D10 = ID3D10RenderTargetView;
  using D3D10_1 = ID3D10RenderTargetView;
  using D3D10_IMPL = MTLD3D10RenderTargetView;
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11DepthStencilView>
struct tag_depth_stencil_view {
  using COM = ID3D11DepthStencilView;
  using COM1 = ID3D11DepthStencilView;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_DEPTH_STENCIL_VIEW_DESC;
  using DESC1 = D3D11_DEPTH_STENCIL_VIEW_DESC;
  using D3D10 = ID3D10DepthStencilView;
  using D3D10_1 = ID3D10DepthStencilView;
  using D3D10_IMPL = MTLD3D10DepthStencilView;
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11ShaderResourceView>
struct tag_shader_resource_view {
  using COM = ID3D11ShaderResourceView;
  using COM1 = ID3D11ShaderResourceView1;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_SHADER_RESOURCE_VIEW_DESC;
  using DESC1 = D3D11_SHADER_RESOURCE_VIEW_DESC1;
  using D3D10 = ID3D10ShaderResourceView;
  using D3D10_1 = ID3D10ShaderResourceView1;
  using D3D10_IMPL = MTLD3D10ShaderResourceView;
};

class ImaginaryD3D10UnorderedAccessView {
public:
  ImaginaryD3D10UnorderedAccessView(IUnknown *d3d11) {}
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11UnorderedAccessView>
struct tag_unordered_access_view {
  using COM = ID3D11UnorderedAccessView;
  using COM1 = ID3D11UnorderedAccessView1;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_UNORDERED_ACCESS_VIEW_DESC;
  using DESC1 = D3D11_UNORDERED_ACCESS_VIEW_DESC1;
  using D3D10 = IUnknown;
  using D3D10_1 = IUnknown;
  using D3D10_IMPL = ImaginaryD3D10UnorderedAccessView;
};

template <typename tag, typename... Base>
class TResourceViewBase
    : public MTLD3D11DeviceChild<typename tag::COM_IMPL, Base...> {
public:
  TResourceViewBase(const tag::DESC1 *pDesc, tag::RESOURCE_IMPL *pResource,
                    MTLD3D11Device *device)
      : MTLD3D11DeviceChild<typename tag::COM_IMPL, Base...>(device),
        d3d10(static_cast<typename tag::COM_IMPL *>(this)) {
    if (pDesc) {
      desc = *pDesc;
    }
    this->resource_ = pResource;
    this->bind_flags_ = this->resource_->bind_flags_;
  }

  template <std::size_t n> HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    return E_NOINTERFACE;
  };

  template <std::size_t n, typename V, typename... Args>
  HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    if (riid == __uuidof(V)) {
      *ppvObject = ref_and_cast<V>(this);
      return S_OK;
    }
    return ResolveBase<n + 1, Args...>(riid, ppvObject);
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    auto hr = ResolveBase<0, Base...>(riid, ppvObject);
    if (SUCCEEDED(hr)) {
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11View) || riid == __uuidof(typename tag::COM) ||
        riid == __uuidof(typename tag::COM1)) {
      *ppvObject = ref_and_cast<typename tag::COM_IMPL>(this);
      return S_OK;
    }

    if constexpr (!std::is_same<typename tag::D3D10_IMPL, ImaginaryD3D10UnorderedAccessView>::value) {
      if (riid == __uuidof(ID3D10DeviceChild) || riid == __uuidof(ID3D10View) ||
          riid == __uuidof(typename tag::D3D10) ||
          riid == __uuidof(typename tag::D3D10_1)) {
        *ppvObject = ref_and_cast<typename tag::D3D10_IMPL>(&d3d10);
        return S_OK;
      }
    }

    if (logQueryInterfaceError(__uuidof(typename tag::COM), riid)) {
      WARN("D3D11View: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE GetDesc(tag::DESC *pDesc) final {
    DowngradeViewDescription(desc, pDesc);
  }

  void STDMETHODCALLTYPE GetDesc1(tag::DESC1 *pDesc) /* override / final */ { *pDesc = desc; }

  void STDMETHODCALLTYPE GetResource(tag::RESOURCE **ppResource) final {
    this->resource_->QueryInterface(IID_PPV_ARGS(ppResource));
  }

protected:
  tag::DESC1 desc;
  tag::D3D10_IMPL d3d10;
};

#pragma region Resource Factory

inline void MakeUniqueSharedName(char (&out)[54]) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t r1 = dis(gen);
    uint64_t r2 = dis(gen);

    std::snprintf(out, sizeof(out), "MULLION_shared_%016llx%016llx",
                  static_cast<unsigned long long>(r1),
                  static_cast<unsigned long long>(r2));
}

HRESULT
CreateStagingBuffer(MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc,
                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                    ID3D11Buffer **ppBuffer);

HRESULT
CreateStagingTexture1D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE1D_DESC *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture1D **ppTexture);

HRESULT
CreateStagingTexture2D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE2D_DESC1 *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture2D1 **ppTexture);

HRESULT
CreateStagingTexture3D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE3D_DESC1 *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture3D1 **ppTexture);

HRESULT CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture);

HRESULT CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE2D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D1 **ppTexture,
                              WMTTextureUsage usage = WMTTextureUsageUnknown);

HRESULT CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE3D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D1 **ppTexture);

HRESULT ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture);

// a texture over another API's Metal texture, of the description of its dimension's DESC1 (D3D11On12)
HRESULT WrapTexture(
    MTLD3D11Device *pDevice, D3D11_RESOURCE_DIMENSION Dimension, const void *pDesc1, WMT::Reference<WMT::Texture> &&Metal,
    const MullionMetalResource &Info, REFIID riid, void **ppTexture
);

// a buffer over another API's Metal buffer, at its GPU address and with its CPU mapping or none (D3D11On12)
HRESULT WrapBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, WMT::Reference<WMT::Buffer> &&Metal,
    const MullionMetalResource &Info, REFIID riid, void **ppBuffer
);

HRESULT ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture);

HRESULT ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
);

HRESULT
CreateBuffer(MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc,
                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                    ID3D11Buffer **ppBuffer);

HRESULT CreateDynamicLinearTexture1D(MTLD3D11Device *pDevice,
                               const D3D11_TEXTURE1D_DESC *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture1D **ppTexture);

HRESULT CreateDynamicLinearTexture2D(MTLD3D11Device *pDevice,
                               const D3D11_TEXTURE2D_DESC1 *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture2D1 **ppTexture);

HRESULT CreateDynamicTexture1D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture);

HRESULT CreateDynamicTexture2D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE2D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D1 **ppTexture);

HRESULT CreateDynamicTexture3D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE3D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D1 **ppTexture);
#pragma endregion

#pragma region Helper
template <typename RESOURCE_DESC, typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const RESOURCE_DESC *pResourceDesc,
                                             VIEW_DESC *pViewDescOut);

template <typename RESOURCE_DESC, typename VIEW_DESC>
HRESULT
ExtractEntireResourceViewDescription(MTLD3D11Device *pDevice, const RESOURCE_DESC *pResourceDesc,
                                    const VIEW_DESC *pViewDescIn, VIEW_DESC *pViewDescOut, DXGI_USAGE Usage) {
  if (pViewDescIn) {
    *pViewDescOut = *pViewDescIn;
    if constexpr (!std::is_same_v<RESOURCE_DESC, D3D11_BUFFER_DESC>) {
      if (pViewDescOut->Format == DXGI_FORMAT_UNKNOWN) {
        pViewDescOut->Format = pResourceDesc->Format;
      }
    }
  } else {
    *pViewDescOut = {};
    if (FAILED(ExtractEntireResourceViewDescription(pResourceDesc, pViewDescOut)))
      return E_INVALIDARG;
  }
  constexpr bool srv = std::is_same_v<VIEW_DESC, D3D11_SHADER_RESOURCE_VIEW_DESC1>;
  constexpr bool rtv = std::is_same_v<VIEW_DESC, D3D11_RENDER_TARGET_VIEW_DESC1>;
  constexpr bool dsv = std::is_same_v<VIEW_DESC, D3D11_DEPTH_STENCIL_VIEW_DESC>;
  constexpr UINT bind = srv   ? D3D11_BIND_SHADER_RESOURCE
                        : rtv ? D3D11_BIND_RENDER_TARGET
                        : dsv ? D3D11_BIND_DEPTH_STENCIL
                              : D3D11_BIND_UNORDERED_ACCESS;
  if (!(pResourceDesc->BindFlags & bind))
    return E_INVALIDARG;
  if constexpr (!std::is_same_v<RESOURCE_DESC, D3D11_BUFFER_DESC>) {
    constexpr UINT one = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE1D) : UINT(D3D11_SRV_DIMENSION_TEXTURE1D);
    constexpr UINT one_array =
        dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE1DARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE1DARRAY);
    constexpr UINT two = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2D) : UINT(D3D11_SRV_DIMENSION_TEXTURE2D);
    constexpr UINT ms_array =
        dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY);
    UINT dimension = pViewDescOut->ViewDimension;
    if constexpr (std::is_same_v<RESOURCE_DESC, D3D11_TEXTURE1D_DESC>) {
      if (dimension != one && dimension != one_array)
        return E_INVALIDARG;
    } else if constexpr (std::is_same_v<RESOURCE_DESC, D3D11_TEXTURE2D_DESC1>) {
      if (dimension < two || dimension > ms_array) {
        if constexpr (srv) {
          if ((dimension != D3D11_SRV_DIMENSION_TEXTURECUBE && dimension != D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) ||
              !(pResourceDesc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE))
            return E_INVALIDARG;
        } else {
          return E_INVALIDARG;
        }
      }
    } else {
      if (dsv || dimension != D3D11_SRV_DIMENSION_TEXTURE3D)
        return E_INVALIDARG;
    }
    // D3D11.3 5.1.6: typed resources keep their format; 5.2.1: views fully qualify typeless resources
    MTL_DXGI_FORMAT_DESC resource_format, view_format;
    if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), pResourceDesc->Format, resource_format)) ||
        FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), pViewDescOut->Format, view_format)) ||
        view_format.PixelFormat == WMTPixelFormatInvalid ||
        // a family's parent qualifies no view; the formats of a depth stencil texture's planes carry the name and do
        ((view_format.Flag & MTL_DXGI_FORMAT_TYPELESS) && DXGIFormatType(pViewDescOut->Format) == pViewDescOut->Format))
      return E_INVALIDARG;
    // and an unordered access view of a 32-bit typeless texture can be R32 of any type (D3D11.3 5.3.9.5, as D3D12's
    // CastsToR32UnorderedAccess)
    constexpr bool uav = !srv && !rtv && !dsv;
    const bool r32 = pViewDescOut->Format == DXGI_FORMAT_R32_UINT || pViewDescOut->Format == DXGI_FORMAT_R32_SINT ||
                     pViewDescOut->Format == DXGI_FORMAT_R32_FLOAT;
    if (!(resource_format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) && !(uav && r32 && CastsToR32UnorderedAccess(pResourceDesc->Format)) &&
        (DXGIFormatType(pResourceDesc->Format) != DXGIFormatType(pViewDescOut->Format) ||
         (pViewDescOut->Format != pResourceDesc->Format && !(resource_format.Flag & MTL_DXGI_FORMAT_TYPELESS) &&
          !((srv || rtv) && (Usage & DXGI_USAGE_BACK_BUFFER)))))
      return E_INVALIDARG;
    UINT support = 0;
    constexpr UINT required = srv   ? D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
                              : rtv ? D3D11_FORMAT_SUPPORT_RENDER_TARGET
                              : dsv ? D3D11_FORMAT_SUPPORT_DEPTH_STENCIL
                                    : D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (FAILED(pDevice->CheckFormatSupport(pViewDescOut->Format, &support)) || !(support & required))
      return E_INVALIDARG;
  }
  return S_OK;
}

template <typename TEXTURE_DESC>
HRESULT CreateMTLTextureDescriptor(MTLD3D11Device *pDevice,
                                   const TEXTURE_DESC *pDesc,
                                   TEXTURE_DESC *pOutDesc,
                                   WMTTextureInfo *pMtlDescOut);

template <typename VIEW_DESC>
HRESULT InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture *pTexture, VIEW_DESC &ViewDesc,
    TextureViewDescriptor &Descriptor
);

template <typename VIEW_DESC>
HRESULT InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture* pTexture,
    VIEW_DESC &ViewDesc, MTL_RENDER_PASS_ATTACHMENT_DESC &AttachmentDesc, TextureViewDescriptor &Descriptor
);

template <typename TEXTURE_DESC>
HRESULT GetLinearTextureLayout(MTLD3D11Device *pDevice,
                               const TEXTURE_DESC &Desc, uint32_t Level,
                               uint32_t &BytesPerRow, uint32_t &BytesPerImage,
                               uint32_t &BytesPerSlice, bool Aligned = true);

constexpr void
CalculateBufferViewOffsetAndSize(const D3D11_BUFFER_DESC &buffer_desc,
                                 uint32_t element_stride,
                                 uint32_t first_element, uint32_t num_elements,
                                 uint32_t &offset, uint32_t &size) {
  offset = first_element * element_stride;
  size = std::min(std::max(0u, buffer_desc.ByteWidth - offset),
                  element_stride * num_elements);
};
#pragma endregion

constexpr UINT kD3D11OutputBindFlags =
    D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL;

} // namespace mullion
