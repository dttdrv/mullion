// DXCore Interface
// Copyright (C) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include <unknwn.h>
#include <cstdint>

// declarations used here from Microsoft DirectX-Headers, include/directx/dxcore_interface.h
enum class DXCoreAdapterProperty : uint32_t {
  InstanceLuid = 0,
  DriverVersion = 1,
  DriverDescription = 2,
  HardwareID = 3,
  KmdModelVersion = 4,
  ComputePreemptionGranularity = 5,
  GraphicsPreemptionGranularity = 6,
  DedicatedAdapterMemory = 7,
  DedicatedSystemMemory = 8,
  SharedSystemMemory = 9,
  AcgCompatible = 10,
  IsHardware = 11,
  IsIntegrated = 12,
  IsDetachable = 13,
  HardwareIDParts = 14,
  PhysicalAdapterCount = 15,
  AdapterEngineCount = 16,
  AdapterEngineName = 17,
};
enum class DXCoreAdapterState : uint32_t {
  IsDriverUpdateInProgress = 0,
  AdapterMemoryBudget = 1,
  AdapterMemoryUsageBytes = 2,
  AdapterMemoryUsageByProcessBytes = 3,
  AdapterEngineRunningTimeMicroseconds = 4,
  AdapterEngineRunningTimeByProcessMicroseconds = 5,
  AdapterTemperatureCelsius = 6,
  AdapterInUseProcessCount = 7,
  AdapterInUseProcessSet = 8,
  AdapterEngineFrequencyHertz = 9,
  AdapterMemoryFrequencyHertz = 10,
};
enum class DXCoreNotificationType : uint32_t {
  AdapterListStale = 0,
  AdapterNoLongerValid = 1,
  AdapterBudgetChange = 2,
  AdapterHardwareContentProtectionTeardown = 3,
};
enum class DXCoreAdapterPreference : uint32_t {
  Hardware = 0,
  MinimumPower = 1,
  HighPerformance = 2,
};

struct DXCoreHardwareID {
  uint32_t vendorID;
  uint32_t deviceID;
  uint32_t subSysID;
  uint32_t revision;
};

using PFN_DXCORE_NOTIFICATION_CALLBACK = void(STDMETHODCALLTYPE *)(DXCoreNotificationType, IUnknown *, void *);

struct IDXCoreAdapter : public IUnknown {
  virtual bool STDMETHODCALLTYPE IsValid() = 0;
  virtual bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID attributeGUID) = 0;
  virtual bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty property) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty property, size_t bufferSize,
                                                void *propertyData) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty property, size_t *bufferSize) = 0;
  virtual bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState property) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                               const void *inputStateDetails, size_t outputBufferSize,
                                               void *outputBuffer) = 0;
  virtual bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState property) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                             const void *inputStateDetails, size_t inputDataSize,
                                             const void *inputData) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapter, 0xf0db4c7f, 0xfe5a, 0x42a2, 0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e)

struct IDXCoreAdapterList : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetAdapter(uint32_t index, REFIID riid, void **ppvAdapter) = 0;
  virtual uint32_t STDMETHODCALLTYPE GetAdapterCount() = 0;
  virtual bool STDMETHODCALLTYPE IsStale() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) = 0;
  virtual HRESULT STDMETHODCALLTYPE Sort(uint32_t numPreferences, const DXCoreAdapterPreference *preferences) = 0;
  virtual bool STDMETHODCALLTYPE IsAdapterPreferenceSupported(DXCoreAdapterPreference preference) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapterList, 0x526c7776, 0x40e9, 0x459b, 0xb7, 0x11, 0xf3, 0x2a, 0xd7, 0x6d, 0xfc, 0x28)

struct IDXCoreAdapterFactory : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateAdapterList(uint32_t numAttributes, const GUID *filterAttributes, REFIID riid,
                                                      void **ppvAdapterList) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdapterByLuid(const LUID &adapterLUID, REFIID riid, void **ppvAdapter) = 0;
  virtual bool STDMETHODCALLTYPE IsNotificationTypeSupported(DXCoreNotificationType notificationType) = 0;
  virtual HRESULT STDMETHODCALLTYPE RegisterEventNotification(IUnknown *dxCoreObject,
                                                              DXCoreNotificationType notificationType,
                                                              PFN_DXCORE_NOTIFICATION_CALLBACK callbackFunction,
                                                              void *callbackContext, uint32_t *eventCookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnregisterEventNotification(uint32_t eventCookie) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapterFactory, 0x78ee5945, 0xc36e, 0x4b13, 0xa6, 0x69, 0x00, 0x5d, 0xd1, 0x1c, 0x0f, 0x06)

// the attributes an adapter list is filtered by
static const GUID DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS = {
    0x8c47866b, 0x7583, 0x450d, {0xf0, 0xf0, 0x6b, 0xad, 0xa8, 0x95, 0xaf, 0x4b}};
static const GUID DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS = {
    0x0c9ece4d, 0x2f6e, 0x4f01, {0x8c, 0x96, 0xe8, 0x9e, 0x33, 0x1b, 0x47, 0xb1}};
static const GUID DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE = {
    0x248e2800, 0xa793, 0x4724, {0xab, 0xaa, 0x23, 0xa6, 0xde, 0x1b, 0xe0, 0x90}};
static const GUID DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU = {
    0xb69eb219, 0x3ded, 0x4464, {0x97, 0x9f, 0xa0, 0x0b, 0xd4, 0x68, 0x70, 0x06}};
