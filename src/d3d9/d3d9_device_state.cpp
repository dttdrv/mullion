// the device state's own arithmetic: the three transforms, their composition and a Reset's defaults (d3d9_device_state.hpp)
#include "d3d9_device_state.hpp"
#include <utility>

namespace mullion::d3d9 {

void
DeviceState::reset() {
  render_ = RenderState();
  lights_ = Lights();
  world_ = view_ = projection_ = identity_matrix();
  world_view_projection_current_ = false;
}

// a matrix a caller may write is one the composition no longer matches
D3DMATRIX *
DeviceState::transform(D3DTRANSFORMSTATETYPE state) {
  D3DMATRIX *slot = const_cast<D3DMATRIX *>(std::as_const(*this).transform(state));
  if (slot)
    world_view_projection_current_ = false;
  return slot;
}

const D3DMATRIX *
DeviceState::transform(D3DTRANSFORMSTATETYPE state) const {
  switch ((UINT)state) {
  case D3DTS_WORLD:
    return &world_;
  case D3DTS_VIEW:
    return &view_;
  case D3DTS_PROJECTION:
    return &projection_;
  default:
    return nullptr;
  }
}

void
DeviceState::multiply(D3DTRANSFORMSTATETYPE state, const D3DMATRIX &matrix) {
  D3DMATRIX *slot = transform(state);
  if (!slot)
    return;
  const D3DMATRIX current = *slot;
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++) {
      float sum = 0.0f;
      for (int at = 0; at < 4; at++)
        sum += current.m[row][at] * matrix.m[at][column];
      slot->m[row][column] = sum;
    }
}

void
DeviceState::lighting_constants(LightConstants &out) const {
  lights_.constants(world_, view_, render_, out);
}

const float *
DeviceState::world_view_projection() const {
  if (world_view_projection_current_)
    return world_view_projection_;
  D3DMATRIX world_view{};
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++) {
      float sum = 0.0f;
      for (int at = 0; at < 4; at++)
        sum += world_.m[row][at] * view_.m[at][column];
      world_view.m[row][column] = sum;
    }
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++) {
      float sum = 0.0f;
      for (int at = 0; at < 4; at++)
        sum += world_view.m[row][at] * projection_.m[at][column];
      world_view_projection_[row * 4 + column] = sum;
    }
  world_view_projection_current_ = true;
  return world_view_projection_;
}

} // namespace mullion::d3d9
