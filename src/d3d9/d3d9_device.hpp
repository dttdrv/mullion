// a Direct3D 9 device over a D3D11 device: what CreateDevice makes, and the
// frames its implicit swap chain shows. the device is defined by its own
// source, since nothing else of this build sees it, only the interfaces it has.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

// makes a device over `adapter` for `window`, with the implicit swap chain
// CreateDevice's present parameters ask for
HRESULT create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
                      const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
                      IDirect3DDevice9 **device);

} // namespace mullion::d3d9
