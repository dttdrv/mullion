#pragma once

#include "com/com_pointer.hpp"
#include "d3d11_1.h"
#include "dxgi1_2.h"

namespace dxmt {

// a 2D texture's subresource as a DXGI surface. it is a part of its texture, which makes one when it is first asked
// for, owns it, and is what its references count. a texture of one subresource is its surface, and the surface the
// texture, to QueryInterface (IDXGISurface: "the runtime automatically creates an IDXGISurface interface when it
// creates a Direct3D resource object that represents a surface"); a subresource of several is only a surface
// (IDXGIResource1::CreateSubresourceSurface)
class MTLD3D11Surface final : public IDXGISurface2 {
public:
  MTLD3D11Surface(ID3D11Texture2D *texture, ID3D11DeviceContext *context, UINT subresource, bool whole) :
      texture_(texture),
      context_(context),
      subresource_(subresource),
      whole_(whole) {}

  ~MTLD3D11Surface() {
    if (dc_) {
      DeleteObject(SelectObject(dc_, old_));
      DeleteDC(dc_);
    }
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    if (whole_)
      return texture_->QueryInterface(riid, ppvObject);
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid != __uuidof(IUnknown) && riid != __uuidof(IDXGIObject) && riid != __uuidof(IDXGIDeviceSubObject) &&
        riid != __uuidof(IDXGISurface) && riid != __uuidof(IDXGISurface1) && riid != __uuidof(IDXGISurface2))
      return E_NOINTERFACE;
    *ppvObject = ref(this);
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE
  AddRef() final {
    return texture_->AddRef();
  }

  ULONG STDMETHODCALLTYPE
  Release() final {
    return texture_->Release();
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT data_size, const void *data) final {
    return texture_->SetPrivateData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *object) final {
    return texture_->SetPrivateDataInterface(guid, object);
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *data_size, void *data) final {
    return texture_->GetPrivateData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    return GetDevice(riid, parent);
  }

  HRESULT STDMETHODCALLTYPE
  GetDevice(REFIID riid, void **ppDevice) final {
    Com<ID3D11Device> device;
    texture_->GetDevice(&device);
    return device->QueryInterface(riid, ppDevice);
  }

  HRESULT STDMETHODCALLTYPE
  GetDesc(DXGI_SURFACE_DESC *pDesc) final {
    if (!pDesc)
      return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc;
    texture_->GetDesc(&desc);
    *pDesc = {desc.Width, desc.Height, desc.Format, desc.SampleDesc};
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  Map(DXGI_MAPPED_RECT *pLockedRect, UINT MapFlags) final {
    if (!pLockedRect)
      return E_INVALIDARG;
    // DXGI_MAP_READ and DXGI_MAP_WRITE are D3D11_MAP_READ and D3D11_MAP_WRITE's bits; both are D3D11_MAP_READ_WRITE
    D3D11_MAP type = MapFlags & DXGI_MAP_DISCARD ? D3D11_MAP_WRITE_DISCARD
                                                  : D3D11_MAP(MapFlags & (DXGI_MAP_READ | DXGI_MAP_WRITE));
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(texture_, subresource_, type, 0, &mapped);
    if (SUCCEEDED(hr))
      *pLockedRect = {INT(mapped.RowPitch), static_cast<BYTE *>(mapped.pData)};
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  Unmap() final {
    context_->Unmap(texture_, subresource_);
    return S_OK;
  }

  // GDI draws to a bitmap of the surface's texels, which go back to the surface when the DC is released
  // (IDXGISurface1::GetDC: a surface made GDI compatible, of a format GDI has, with no DC outstanding; the output
  // merger no longer renders to it)
  HRESULT STDMETHODCALLTYPE
  GetDC(BOOL Discard, HDC *phdc) final {
    if (!phdc)
      return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc;
    texture_->GetDesc(&desc);
    if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) || dc_)
      return DXGI_ERROR_INVALID_CALL;
    // top-down, as a texture's rows are
    BITMAPINFO info{{sizeof(BITMAPINFOHEADER), LONG(desc.Width), -LONG(desc.Height), 1, WORD(8 * texel), BI_RGB}};
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
    if (!bitmap) {
      DeleteDC(dc);
      return E_OUTOFMEMORY;
    }
    if (!Discard) {
      Com<ID3D11Device> device;
      texture_->GetDevice(&device);
      Com<ID3D11Texture2D> staging;
      D3D11_TEXTURE2D_DESC read{desc.Width, desc.Height, 1, 1, desc.Format, desc.SampleDesc, D3D11_USAGE_STAGING, 0,
                                D3D11_CPU_ACCESS_READ};
      D3D11_MAPPED_SUBRESOURCE mapped;
      HRESULT hr = device->CreateTexture2D(&read, nullptr, &staging);
      if (SUCCEEDED(hr)) {
        context_->CopySubresourceRegion(staging.ptr(), 0, 0, 0, 0, texture_, subresource_, nullptr);
        hr = context_->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &mapped);
      }
      if (FAILED(hr)) {
        DeleteObject(bitmap);
        DeleteDC(dc);
        return hr;
      }
      for (UINT y = 0; y < desc.Height; y++)
        memcpy(static_cast<char *>(bits_) + size_t(y) * desc.Width * texel,
               static_cast<char *>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * texel);
      context_->Unmap(staging.ptr(), 0);
    }
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *depth;
    context_->OMGetRenderTargets(std::size(targets), targets, &depth);
    bool bound = false;
    for (auto &target : targets) {
      Com<ID3D11Resource> resource;
      if (target)
        target->GetResource(&resource);
      if (target && resource.ptr() == texture_) {
        target->Release();
        target = nullptr;
        bound = true;
      }
    }
    if (bound)
      context_->OMSetRenderTargetsAndUnorderedAccessViews(
          std::size(targets), targets, depth, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr
      );
    for (auto target : targets)
      if (target)
        target->Release();
    if (depth)
      depth->Release();
    old_ = SelectObject(dc, bitmap);
    dc_ = dc;
    *phdc = dc;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  ReleaseDC(RECT *pDirtyRect) final {
    if (!dc_)
      return DXGI_ERROR_INVALID_CALL;
    GdiFlush();
    D3D11_TEXTURE2D_DESC desc;
    texture_->GetDesc(&desc);
    RECT all{0, 0, LONG(desc.Width), LONG(desc.Height)}, dirty;
    // what GDI drew, or of it what the application says
    if (IntersectRect(&dirty, &all, pDirtyRect ? pDirtyRect : &all)) {
      D3D11_BOX box{UINT(dirty.left), UINT(dirty.top), 0, UINT(dirty.right), UINT(dirty.bottom), 1};
      context_->UpdateSubresource(
          texture_, subresource_, &box,
          static_cast<char *>(bits_) + (size_t(dirty.top) * desc.Width + dirty.left) * texel, desc.Width * texel, 0
      );
    }
    DeleteObject(SelectObject(dc_, old_));
    DeleteDC(dc_);
    dc_ = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetResource(REFIID riid, void **ppParentResource, UINT *pSubresourceIndex) final {
    if (!ppParentResource || !pSubresourceIndex)
      return E_INVALIDARG;
    *pSubresourceIndex = subresource_;
    return texture_->QueryInterface(riid, ppParentResource);
  }

private:
  // the bytes of a texel of the formats GDI has, DXGI_FORMAT_B8G8R8A8_UNORM and its sRGB form, which are a 32-bit
  // bitmap's
  static constexpr UINT texel = 4;

  ID3D11Texture2D *texture_; // no reference: the texture owns the surface
  ID3D11DeviceContext *context_;
  UINT subresource_;
  bool whole_;
  HDC dc_ = nullptr;
  HGDIOBJ old_ = nullptr;
  void *bits_ = nullptr;
};

} // namespace dxmt
