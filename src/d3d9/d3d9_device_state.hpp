// the device state a draw is made from, in one place: the render states, the three transforms a vertex is multiplied by
// (and their composition), and the eight lights with their enables and the material. A draw reads all of it, a Reset
// puts all of it back to the defaults a device is made with, and a state block captures and restores it through the
// device's calls.
#pragma once
#include "d3d9_common.hpp"
#include "d3d9_lights.hpp"
#include "d3d9_state.hpp"

namespace mullion::d3d9 {

class DeviceState {
public:
  // the transforms start at the identity, the render states at the defaults D3DRENDERSTATETYPE documents, and the
  // lights and material at D3D9's (every light off, the material's diffuse white)
  DeviceState() = default;

  RenderState &render() {
    return render_;
  }
  // a const device reads it too: the clip rectangle a draw is cut by depends on D3DRS_SCISSORTESTENABLE
  // (d3d9_device_targets.cpp)
  const RenderState &render() const {
    return render_;
  }
  Lights &lights() {
    return lights_;
  }

  // the state a Reset loses comes back as a device is made with it (IDirect3DDevice9::Reset: "all state information is
  // lost"). D3DRS_ZENABLE is the one state whose default depends on the chain the device made, so the device sets it
  // after this (d3d9_device.cpp)
  void reset();

  // The transform a D3DTRANSFORMSTATETYPE names, among the three this build keeps. D3D9's device has 256 world
  // matrices, which is what vertex blending reads; this build blends no vertices, so it keeps the one a device without
  // blending uses (D3DTS_WORLD, which is D3DTS_WORLDMATRIX(0)) and a texture coordinate transform is not performed by
  // this build's texture stage either, so both are null rather than a matrix no draw reads
  // (IDirect3DDevice9::SetTransform).
  D3DMATRIX *transform(D3DTRANSFORMSTATETYPE state);
  const D3DMATRIX *transform(D3DTRANSFORMSTATETYPE state) const;
  const D3DMATRIX &texture_transform(UINT stage) const {
    return texture_[stage < 8 ? stage : 0];
  }
  // D3D9's world * matrix: the device's matrix and the one given are multiplied, so a vertex is transformed by both, the
  // device's first (IDirect3DDevice9::MultiplyTransform, "this method multiplies the world matrix by the specified
  // matrix"). The transform is a row vector's, so the product is the device's matrix times the given one.
  void multiply(D3DTRANSFORMSTATETYPE state, const D3DMATRIX &matrix);

  // D3D9's composed transform for a vertex the device transforms: v' = v * World * View * Projection, a row vector
  // times the three matrices in that order (Microsoft Learn, "Transforms"). The product is kept in D3D9's row-major
  // float order, which is what the transform's vertex shader reads at b0 for a row-major float4x4. Worked out when one
  // of the three changes rather than at every draw.
  const float *world_view_projection() const;

  // the material, lights and lighting states a lit draw reads at b1, worked out from this state when the draw is
  // prepared rather than when a light is set (d3d9_lights.cpp)
  void lighting_constants(LightConstants &out) const;

private:
  RenderState render_;
  Lights lights_;
  D3DMATRIX world_ = identity_matrix();
  D3DMATRIX view_ = identity_matrix();
  D3DMATRIX projection_ = identity_matrix();
  D3DMATRIX texture_[8] = {identity_matrix(), identity_matrix(), identity_matrix(), identity_matrix(),
                           identity_matrix(), identity_matrix(), identity_matrix(), identity_matrix()};
  mutable float world_view_projection_[16] = {};
  mutable bool world_view_projection_current_ = false;
};

} // namespace mullion::d3d9
