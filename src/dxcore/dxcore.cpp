/*
 * Copyright 2026 Feifan He for CodeWeavers
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

// DXCore: the adapter enumeration a DX12 game or launcher uses before it has a device, over the adapters DXGI
// already exposes. Wine ships dxcore.dll as a stub that answers E_NOINTERFACE to every interface, and DXCore has to
// agree with DXGI about who the adapters are: an adapter is the same LUID here as the one IDXGIAdapter1::GetDesc
// reports and the one D3DKMT and DISPLAYCONFIG_PATH_SOURCE_INFO name, which is what makes
// "DXCore's adapters are DXGI's adapters" (tests/d3d12/d3d12_adapters.cpp) hold.
#include "dxcore_interface.h"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <cstring>
#include <dxgi1_4.h>
#include <vector>

namespace mullion {

Logger Logger::s_instance("dxcore.log");

namespace {

class MTLDXCoreAdapterFactory;

// the attributes this library's adapters have: they make devices of both Direct3D 12 and 11, on a GPU
bool
AttributesSupported(uint32_t count, const GUID *attributes) {
  for (uint32_t i = 0; i < count; i++) {
    const GUID &attribute = attributes[i];
    if (attribute != DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS && attribute != DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS &&
        attribute != DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE && attribute != DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU)
      return false;
  }
  return true;
}

class MTLDXCoreAdapter : public ComObject<IDXCoreAdapter> {

public:
  MTLDXCoreAdapter(IDXGIAdapter1 *adapter, const DXGI_ADAPTER_DESC1 &desc, IDXCoreAdapterFactory *factory)
      : m_adapter(adapter), m_desc(desc), m_factory(factory) {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXCoreAdapter))
      *ppvObject = ref(this);
    else
      return E_NOINTERFACE;

    return S_OK;
  }

  // a LUID is what a caller looks an adapter up by, and an adapter that has one is the one that provides it: the
  // adapter itself does not go away while this object lives
  bool STDMETHODCALLTYPE IsValid() final { return true; }

  bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID attributeGUID) final {
    return attributeGUID == DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS ||
           attributeGUID == DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS ||
           attributeGUID == DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE ||
           attributeGUID == DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU;
  }

  bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty property) final {
    switch (property) {
    case DXCoreAdapterProperty::InstanceLuid:
    case DXCoreAdapterProperty::DriverDescription:
    case DXCoreAdapterProperty::HardwareID:
    case DXCoreAdapterProperty::DedicatedAdapterMemory:
    case DXCoreAdapterProperty::DedicatedSystemMemory:
    case DXCoreAdapterProperty::SharedSystemMemory:
    case DXCoreAdapterProperty::IsHardware:
    case DXCoreAdapterProperty::IsIntegrated:
    case DXCoreAdapterProperty::IsDetachable:
      return true;
    default:
      return false;
    }
  }

  // what DXGI's description of the same adapter holds: "InstanceLuid ... representing the adapter", the driver's
  // description, and its memory in the segments a Direct3D 12 caller budgets against
  HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty property, size_t bufferSize, void *propertyData) final {
    if (!propertyData)
      return E_POINTER;

    switch (property) {
    case DXCoreAdapterProperty::InstanceLuid:
      return Copy(bufferSize, propertyData, m_desc.AdapterLuid);
    case DXCoreAdapterProperty::DriverDescription:
      return Copy(bufferSize, propertyData, m_desc.Description);
    case DXCoreAdapterProperty::HardwareID: {
      DXCoreHardwareID id{};
      id.vendorID = m_desc.VendorId;
      id.deviceID = m_desc.DeviceId;
      id.subSysID = m_desc.SubSysId;
      id.revision = m_desc.Revision;
      return Copy(bufferSize, propertyData, id);
    }
    case DXCoreAdapterProperty::DedicatedAdapterMemory:
      return Copy(bufferSize, propertyData, uint64_t(m_desc.DedicatedVideoMemory));
    case DXCoreAdapterProperty::DedicatedSystemMemory:
      return Copy(bufferSize, propertyData, uint64_t(m_desc.DedicatedSystemMemory));
    case DXCoreAdapterProperty::SharedSystemMemory:
      return Copy(bufferSize, propertyData, uint64_t(m_desc.SharedSystemMemory));
    case DXCoreAdapterProperty::IsHardware:
      return Copy(bufferSize, propertyData, !(m_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
    case DXCoreAdapterProperty::IsIntegrated:
      // a device with one pool of memory feeds its display from it: no memory of its own
      return Copy(bufferSize, propertyData, m_desc.DedicatedVideoMemory == 0);
    case DXCoreAdapterProperty::IsDetachable:
      return Copy(bufferSize, propertyData, false);
    default:
      return E_INVALIDARG;
    }
  }

  HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty property, size_t *bufferSize) final {
    if (!bufferSize)
      return E_POINTER;

    switch (property) {
    case DXCoreAdapterProperty::InstanceLuid:
      *bufferSize = sizeof(m_desc.AdapterLuid);
      return S_OK;
    case DXCoreAdapterProperty::DriverDescription:
      *bufferSize = sizeof(m_desc.Description);
      return S_OK;
    case DXCoreAdapterProperty::HardwareID:
      *bufferSize = sizeof(DXCoreHardwareID);
      return S_OK;
    case DXCoreAdapterProperty::DedicatedAdapterMemory:
    case DXCoreAdapterProperty::DedicatedSystemMemory:
    case DXCoreAdapterProperty::SharedSystemMemory:
      *bufferSize = sizeof(uint64_t);
      return S_OK;
    case DXCoreAdapterProperty::IsHardware:
    case DXCoreAdapterProperty::IsIntegrated:
    case DXCoreAdapterProperty::IsDetachable:
      *bufferSize = sizeof(bool);
      return S_OK;
    default:
      return E_INVALIDARG;
    }
  }

  // the states are a driver's counters; this library keeps none of what a caller would read here
  bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState state) final { return false; }

  HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                       const void *inputStateDetails, size_t outputBufferSize,
                                       void *outputBuffer) final {
    if (outputBuffer)
      memset(outputBuffer, 0, outputBufferSize);
    return E_INVALIDARG;
  }

  bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState state) final { return false; }

  HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                     const void *inputStateDetails, size_t inputDataSize,
                                     const void *inputData) final {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) final {
    return m_factory->QueryInterface(riid, ppvFactory);
  }

private:
  template <typename T>
  static HRESULT
  Copy(size_t bufferSize, void *propertyData, const T &value) {
    if (bufferSize < sizeof(T))
      return E_INVALIDARG;
    memcpy(propertyData, &value, sizeof(T));
    return S_OK;
  }

  static HRESULT
  Copy(size_t bufferSize, void *propertyData, uint64_t value) {
    return Copy<uint64_t>(bufferSize, propertyData, value);
  }

  static HRESULT
  Copy(size_t bufferSize, void *propertyData, bool value) {
    return Copy<bool>(bufferSize, propertyData, value);
  }

  Com<IDXGIAdapter1> m_adapter;
  DXGI_ADAPTER_DESC1 m_desc;
  Com<IDXCoreAdapterFactory> m_factory;
};

class MTLDXCoreAdapterList : public ComObject<IDXCoreAdapterList> {

public:
  MTLDXCoreAdapterList(IDXCoreAdapterFactory *factory, std::vector<Com<IDXCoreAdapter>> adapters)
      : m_factory(factory), m_adapters(std::move(adapters)) {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXCoreAdapterList))
      *ppvObject = ref(this);
    else
      return E_NOINTERFACE;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(uint32_t index, REFIID riid, void **ppvAdapter) final {
    if (ppvAdapter == nullptr)
      return E_POINTER;

    *ppvAdapter = nullptr;

    if (index >= m_adapters.size())
      return E_INVALIDARG;

    return m_adapters[index]->QueryInterface(riid, ppvAdapter);
  }

  uint32_t STDMETHODCALLTYPE GetAdapterCount() final { return uint32_t(m_adapters.size()); }

  // the list is built on demand, from the devices Metal has at that moment
  bool STDMETHODCALLTYPE IsStale() final { return false; }

  HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) final {
    return m_factory->QueryInterface(riid, ppvFactory);
  }

  // the preferences rank adapters of different kinds; there is one kind here, so the order already holds
  HRESULT STDMETHODCALLTYPE Sort(uint32_t numPreferences, const DXCoreAdapterPreference *preferences) final {
    if (numPreferences && !preferences)
      return E_INVALIDARG;

    for (uint32_t i = 0; i < numPreferences; i++)
      if (!IsAdapterPreferenceSupported(preferences[i]))
        return E_INVALIDARG;

    return S_OK;
  }

  bool STDMETHODCALLTYPE IsAdapterPreferenceSupported(DXCoreAdapterPreference preference) final {
    return preference == DXCoreAdapterPreference::Hardware || preference == DXCoreAdapterPreference::HighPerformance;
  }

private:
  Com<IDXCoreAdapterFactory> m_factory;
  std::vector<Com<IDXCoreAdapter>> m_adapters;
};

class MTLDXCoreAdapterFactory : public ComObject<IDXCoreAdapterFactory> {

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXCoreAdapterFactory))
      *ppvObject = ref(this);
    else
      return E_NOINTERFACE;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateAdapterList(uint32_t numAttributes, const GUID *filterAttributes, REFIID riid,
                                              void **ppvAdapterList) final {
    if (ppvAdapterList == nullptr)
      return E_POINTER;

    *ppvAdapterList = nullptr;

    // an adapter has every attribute the list asks for, or the list is empty: the attributes are how a caller picks
    // graphics adapters from compute ones, and there are no compute-only adapters here
    if (!numAttributes || !filterAttributes || !AttributesSupported(numAttributes, filterAttributes))
      return E_INVALIDARG;

    Com<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      ERR("DXCore: Failed to create a DXGI factory");
      return hr;
    }

    std::vector<Com<IDXCoreAdapter>> adapters;
    bool compute_only = false;
    for (uint32_t a = 0; a < numAttributes; a++)
      compute_only |= filterAttributes[a] == DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE;
    // compute-only adapters are the ones with no display: every adapter here drives one, so that list is empty
    for (UINT i = 0; !compute_only; i++) {
      Com<IDXGIAdapter1> adapter;
      DXGI_ADAPTER_DESC1 desc;
      if (factory->EnumAdapters1(i, &adapter) != S_OK)
        break;
      if (FAILED(adapter->GetDesc1(&desc)))
        continue;
      adapters.push_back(Com<IDXCoreAdapter>(new MTLDXCoreAdapter(adapter.ptr(), desc, this)));
    }

    Com<IDXCoreAdapterList> list(new MTLDXCoreAdapterList(this, std::move(adapters)));
    return list->QueryInterface(riid, ppvAdapterList);
  }

  // the LUID is the adapter's: a caller that has one from DXGI, D3DKMT or a display path finds the same adapter here
  HRESULT STDMETHODCALLTYPE GetAdapterByLuid(const LUID &adapterLUID, REFIID riid, void **ppvAdapter) final {
    if (ppvAdapter == nullptr)
      return E_POINTER;

    *ppvAdapter = nullptr;

    Com<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
      return hr;

    Com<IDXGIAdapter1> adapter;
    if (FAILED(hr = factory->EnumAdapterByLuid(adapterLUID, IID_PPV_ARGS(&adapter))))
      return hr == DXGI_ERROR_NOT_FOUND ? E_INVALIDARG : hr;

    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(hr = adapter->GetDesc1(&desc)))
      return hr;

    Com<IDXCoreAdapter> core(new MTLDXCoreAdapter(adapter.ptr(), desc, this));
    return core->QueryInterface(riid, ppvAdapter);
  }

  // the notifications are of an adapter going away or its budget changing, which a device of this library does not
  // report: a caller that asks is told the type is not supported, and never registered for
  bool STDMETHODCALLTYPE IsNotificationTypeSupported(DXCoreNotificationType notificationType) final { return false; }

  HRESULT STDMETHODCALLTYPE RegisterEventNotification(IUnknown *dxCoreObject, DXCoreNotificationType notificationType,
                                                      PFN_DXCORE_NOTIFICATION_CALLBACK callbackFunction,
                                                      void *callbackContext, uint32_t *eventCookie) final {
    if (eventCookie)
      *eventCookie = 0;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE UnregisterEventNotification(uint32_t eventCookie) final { return E_NOTIMPL; }
};

} // namespace

extern "C" HRESULT WINAPI
DXCoreCreateAdapterFactory(REFIID riid, void **ppvFactory) {
  Com<IDXCoreAdapterFactory> factory(new MTLDXCoreAdapterFactory());
  HRESULT hr = factory->QueryInterface(riid, ppvFactory);
  return hr;
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return TRUE;
}

} // namespace mullion
