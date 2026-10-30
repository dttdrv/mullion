// the Direct3D 9 object itself: what Direct3DCreate9 makes, the adapters it
// enumerates and the devices it makes.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

// a new Direct3D 9 object over DXGI's adapters, which is where this build's
// Metal devices are; null when DXGI has no factory, which is what a machine
// without Metal looks like here
IDirect3D9 *create_direct3d();

} // namespace mullion::d3d9
