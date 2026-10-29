#pragma once

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d11_3.h"
#include "d3d11_multithread.hpp"
#include "dxgi_interfaces.h"
#include "mullion_device.hpp"
#include "mullion_format.hpp"

struct MTL_GRAPHICS_PIPELINE_DESC;
struct MTL_COMPUTE_PIPELINE_DESC;
struct IMTLD3D11RasterizerState;

DEFINE_COM_INTERFACE("3a3f085a-d0fe-4324-b0ae-fe04de18571c",
                     IMTLD3D11DeviceContext)
    : public ID3D11DeviceContext4 {
  virtual void WaitUntilGPUIdle() = 0;
  virtual void PrepareFlush() = 0;
  virtual void UnbindResource(ID3D11Resource *pResource) = 0;
  virtual void Commit() = 0;
  virtual HRESULT WaitEvent(WMT::SharedEvent event, UINT64 value, ID3D12Fence *fence) = 0;
  virtual HRESULT SignalFence(ID3D12Fence *fence, UINT64 value) = 0;
  virtual HRESULT ReturnResource(ID3D11Resource *resource, std::function<void(mullion::ArgumentEncodingContext &)> &&before) = 0;
  virtual void SOGetTargetsWithOffsets(UINT NumBuffers, REFIID riid, void ** ppSOTargets, UINT *pOffsets) = 0;
  virtual HRESULT AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) = 0;
  virtual HRESULT ReleaseSync(ID3D11Resource *pResource, UINT64 Key) = 0;
};

namespace mullion {

class MTLCompiledGraphicsPipeline;
class MTLCompiledComputePipeline;
class MTLCompiledGeometryPipeline;
class MTLCompiledTessellationMeshPipeline;

class MTLD3D11Device : public ID3D11Device5 {
public:

  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;

  virtual D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() = 0;

  virtual HRESULT CreateGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                         MTLCompiledGraphicsPipeline *
                                             *ppPipeline) = 0;

  virtual HRESULT CreateComputePipeline(MTL_COMPUTE_PIPELINE_DESC * pDesc,
                                        MTLCompiledComputePipeline *
                                            *ppPipeline) = 0;

  virtual HRESULT CreateGeometryPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                             MTLCompiledGeometryPipeline *
                                                 *ppPipeline) = 0;

  virtual HRESULT CreateTessellationMeshPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                             MTLCompiledTessellationMeshPipeline *
                                                 *ppPipeline) = 0;

  virtual bool IsTraced() = 0;

  virtual IMTLD3D11RasterizerState *GetDefaultRasterizerState() = 0;

  virtual Device& GetMullionDevice() = 0;

  virtual void CreateCommandList(ID3D11CommandList** pCommandList) = 0;

  virtual FormatCapability GetMTLPixelFormatCapability(WMTPixelFormat Format) = 0;

  virtual IMTLD3D11DeviceContext *GetImmediateContextPrivate() = 0;

  virtual unsigned int GetDirectXVersion() = 0;

  // A command buffer the GPU failed, or a Metal error the device cannot continue after: the first reason is the one the
  // device keeps, and every registration hears of it (ID3D11Device4::RegisterDeviceRemovedEvent)
  virtual void STDMETHODCALLTYPE LoseDevice(HRESULT Reason) = 0;
  // the reason the device was removed, S_OK while it is not: the queue's fault, or the one LoseDevice was given
  virtual HRESULT STDMETHODCALLTYPE DeviceRemovalReason() = 0;

  d3d11_device_mutex mutex;
  bool shares_with_d3d12 = false;
};

D3D_FEATURE_LEVEL GetMaxFeatureLevel(WMT::Device device);

Com<IMTLDXGIDevice> CreateD3D11Device(std::unique_ptr<Device> &&device,
                                      IMTLDXGIAdapter *pAdapter,
                                      D3D_FEATURE_LEVEL FeatureLevel,
                                      UINT Flags);
} // namespace mullion
