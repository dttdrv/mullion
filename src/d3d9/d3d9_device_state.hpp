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
  DeviceState();

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

  // The transform a D3DTRANSFORMSTATETYPE names. D3D9's device has 256 world matrices (D3DTS_WORLDMATRIX, Microsoft
  // Learn, D3DTRANSFORMSTATETYPE); vertex blending reads the first four of them, and tweening reads the first two.
  // A texture coordinate transform is kept for the stage that multiplies by it.
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
  // one of the 256 world matrices, and the view * projection a blended vertex multiplies after the worlds
  const D3DMATRIX &world_matrix(UINT index) const {
    return world_[index < 256 ? index : 0];
  }
  // WORLD, or the tween of WORLD and WORLD1 when D3DRS_VERTEXBLEND is D3DVBF_TWEENING
  D3DMATRIX blended_world() const;
  void view_projection(float out[16]) const;

  // the material, lights and lighting states a lit draw reads at b1, worked out from this state when the draw is
  // prepared rather than when a light is set (d3d9_lights.cpp)
  void lighting_constants(LightConstants &out) const;

  // D3D9's six user clip planes as SetClipPlane gave them, each the four coefficients of ax + by + cz + dw (Microsoft
  // Learn, IDirect3DDevice9::SetClipPlane). They start at zero, and a Reset puts them back there
  static constexpr UINT kClipPlanes = 6;
  void set_clip_plane(UINT index, const float *plane) {
    memcpy(clip_planes_[index], plane, sizeof(clip_planes_[index]));
  }
  void clip_plane(UINT index, float *plane) const {
    memcpy(plane, clip_planes_[index], sizeof(clip_planes_[index]));
  }
  // the planes a draw is cut by, in clip space, a disabled one zero (which cuts nothing): false while
  // D3DRS_CLIPPLANEENABLE names none. A fixed-function draw's planes are in world space and are carried to clip space
  // through the inverse of View * Projection, so that a plane dotted with the clip position is the plane dotted with the
  // world position; a vertex shader's are already in clip space (D3DRS_CLIPPLANEENABLE)
  bool clip_space_planes(bool world_space, float out[kClipPlanes * 4]) const;

private:
  RenderState render_;
  Lights lights_;
  D3DMATRIX world_[256] = {};
  D3DMATRIX view_ = identity_matrix();
  D3DMATRIX projection_ = identity_matrix();
  D3DMATRIX texture_[8] = {identity_matrix(), identity_matrix(), identity_matrix(), identity_matrix(),
                           identity_matrix(), identity_matrix(), identity_matrix(), identity_matrix()};
  mutable float world_view_projection_[16] = {};
  mutable bool world_view_projection_current_ = false;
  mutable DWORD cached_vertex_blend_ = 0xFFFFFFFF;
  mutable DWORD cached_tween_ = 0;
  float clip_planes_[kClipPlanes][4] = {};
};

} // namespace mullion::d3d9
