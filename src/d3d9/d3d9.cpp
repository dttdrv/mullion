// Direct3D 9's two exports: the object, and the extended one this milestone has
// no need of (an ex object is the same device with the additions 9Ex has, and
// nothing keeps a frame's statistic yet).
#include "d3d9_direct3d.hpp"

using namespace mullion;

// the log of this library alone, as each of this build's libraries has its own
Logger Logger::s_instance("d3d9.log");

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
  if (sdk_version != D3D_SDK_VERSION) {
    ERR("D3D9: Direct3DCreate9 asked for SDK version ", sdk_version, ", this build is ", D3D_SDK_VERSION, ".");
    return nullptr;
  }
  return d3d9::create_direct3d();
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex **) {
  ERR_ONCE("D3D9: Direct3DCreate9Ex is not implemented yet.");
  return E_NOTIMPL;
}
