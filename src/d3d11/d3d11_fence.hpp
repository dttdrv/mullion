#pragma once
#include "d3d11_device.hpp"

namespace mullion {

struct MTLD3D11Fence : public ID3D11Fence {
  WMT::Reference<WMT::SharedEvent> event;
  // a fence made with D3D11_FENCE_FLAG_SHARED carries the bootstrap name of its mach port, so every shared handle
  // made from it can reopen the same fence on another device or in another process
  bool shared = false;
  char mach_port_name[54] = {};
};

HRESULT
CreateFence(MTLD3D11Device *pDevice, UINT64 InitialValue,
            D3D11_FENCE_FLAG Flags, REFIID riid, void **ppFence);

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource,
                REFIID riid, void **ppFence);

} // namespace mullion
