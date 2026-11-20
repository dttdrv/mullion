// the Direct3D 9 object itself: what Direct3DCreate9 makes, the adapters it
// enumerates and the devices it makes.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

// a new Direct3D 9 object over DXGI's adapters, which is where this build's
// Metal devices are; null when DXGI has no factory, which is what a machine
// without Metal looks like here. `extended` is what Direct3DCreate9Ex asks for:
// the same object also answers IDirect3D9Ex, and CreateDeviceEx makes a device
// that answers IDirect3DDevice9Ex.
IDirect3D9 *create_direct3d(bool extended = false);

// Fullscreen exclusive mode tracking for GetAdapterDisplayMode (Wine device.c test_display_modes).
void d3d9_enter_exclusive_mode(IDirect3D9 *d3d, UINT width, UINT height);
void d3d9_leave_exclusive_mode(IDirect3D9 *d3d);

} // namespace mullion::d3d9
