/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "d3d9.h"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include <utility>

namespace mullion {

// A public reference to a child keeps the device alive, as in native D3D9.
template <typename... Base> class MTLD3D9DeviceChild : public ComObject<Base...> {
public:
  MTLD3D9DeviceChild(IDirect3DDevice9Ex *pDevice) : device_(pDevice) {}

  ULONG STDMETHODCALLTYPE
  AddRef() override {
    uint32_t count = this->m_refCount++;
    if (unlikely(!count)) {
      this->AddRefPrivate();
      device_->AddRef();
    }
    return count + 1;
  }

  ULONG STDMETHODCALLTYPE
  Release() override {
    // implicit objects (back buffers, the implicit swap chain) start at 0, and releasing them there is a no-op
    if (unlikely(!this->m_refCount))
      return 0;
    uint32_t count = --this->m_refCount;
    if (unlikely(!count)) {
      IDirect3DDevice9Ex *device = device_;
      this->ReleasePrivate();
      device->Release();
    }
    return count;
  }

  IDirect3DDevice9Ex *
  GetParentDevice() const {
    return device_;
  }

protected:
  HRESULT
  GetDeviceInternal(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    device_->AddRef();
    *ppDevice = device_;
    return D3D_OK;
  }

  IDirect3DDevice9Ex *device_;
};

template <typename... Base> class MTLD3D9PrivateDataObject : public MTLD3D9DeviceChild<Base...> {
public:
  using MTLD3D9DeviceChild<Base...>::MTLD3D9DeviceChild;

  HRESULT STDMETHODCALLTYPE
  GetDevice(IDirect3DDevice9 **ppDevice) final {
    return this->GetDeviceInternal(ppDevice);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) final {
    if (!pData && SizeOfData)
      return D3DERR_INVALIDCALL;
    if (Flags & D3DSPD_IUNKNOWN) {
      if (SizeOfData != sizeof(IUnknown *))
        return D3DERR_INVALIDCALL;
      // pData is the interface itself; SizeOfData is sizeof(IUnknown *)
      return FAILED(private_data_.setInterface(refguid, static_cast<const IUnknown *>(pData)))
                 ? D3DERR_INVALIDCALL
                 : D3D_OK;
    }
    return FAILED(private_data_.setData(refguid, SizeOfData, pData)) ? D3DERR_INVALIDCALL : D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) final {
    // a missing GUID is reported before the size is checked, and leaves the size untouched
    UINT size = pSizeOfData ? *pSizeOfData : 0;
    HRESULT hr = private_data_.getData(refguid, &size, pSizeOfData ? pData : nullptr);
    if (hr == DXGI_ERROR_NOT_FOUND)
      return D3DERR_NOTFOUND;
    if (!pSizeOfData)
      return D3DERR_INVALIDCALL;
    *pSizeOfData = size;
    if (hr == DXGI_ERROR_MORE_DATA)
      return D3DERR_MOREDATA;
    return FAILED(hr) ? D3DERR_INVALIDCALL : D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE
  FreePrivateData(REFGUID refguid) final {
    return private_data_.setData(refguid, 0, nullptr) == S_OK ? D3D_OK : D3DERR_NOTFOUND;
  }

private:
  ConcurrentComPrivateData private_data_;
};

// DEFAULT pool resources the app holds references to; a non-Ex Reset refuses while any exist
void D3D9CountDefaultResource(IDirect3DDevice9Ex *pDevice, int Delta);

template <typename... Base> class MTLD3D9Resource : public MTLD3D9PrivateDataObject<Base...> {
public:
  // only managed textures and buffers keep a priority (Wine's test_resource_priority)
  MTLD3D9Resource(IDirect3DDevice9Ex *pDevice, D3DPOOL Pool, bool KeepsPriority = true) :
      MTLD3D9PrivateDataObject<Base...>(pDevice),
      keeps_priority_(KeepsPriority && Pool == D3DPOOL_MANAGED),
      default_pool_(Pool == D3DPOOL_DEFAULT) {}

  ULONG STDMETHODCALLTYPE
  AddRef() override {
    ULONG count = MTLD3D9PrivateDataObject<Base...>::AddRef();
    if (default_pool_ && count == 1)
      D3D9CountDefaultResource(this->device_, 1);
    return count;
  }

  ULONG STDMETHODCALLTYPE
  Release() override {
    // before the base release, which may drop the last device reference
    if (default_pool_ && this->m_refCount == 1)
      D3D9CountDefaultResource(this->device_, -1);
    return MTLD3D9PrivateDataObject<Base...>::Release();
  }

  DWORD STDMETHODCALLTYPE
  SetPriority(DWORD PriorityNew) override {
    if (!keeps_priority_)
      return 0;
    return std::exchange(priority_, PriorityNew);
  }

  DWORD STDMETHODCALLTYPE
  GetPriority() override {
    return priority_;
  }

  void STDMETHODCALLTYPE
  PreLoad() override {}

private:
  DWORD priority_ = 0;
  bool keeps_priority_;
  bool default_pool_;
};

} // namespace mullion
