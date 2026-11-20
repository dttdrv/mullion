// the device state's own arithmetic: the three transforms, their composition and a Reset's defaults (d3d9_device_state.hpp)
#include "d3d9_device_state.hpp"
#include <cmath>
#include <cstring>
#include <utility>

namespace mullion::d3d9 {

DeviceState::DeviceState() {
  reset();
}

void
DeviceState::reset() {
  render_ = RenderState();
  lights_ = Lights();
  view_ = projection_ = identity_matrix();
  for (auto &world : world_)
    world = identity_matrix();
  for (UINT at = 0; at < 8; at++)
    texture_[at] = identity_matrix();
  world_view_projection_current_ = false;
  cached_vertex_blend_ = 0xFFFFFFFF;
  memset(clip_planes_, 0, sizeof(clip_planes_));
}

namespace {

// the inverse of a 4x4 matrix by Gauss-Jordan elimination with partial pivoting, in doubles: false for a matrix that has
// none
bool
invert(const D3DMATRIX &matrix, double inverse[4][4]) {
  double work[4][8];
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++) {
      work[row][column] = matrix.m[row][column];
      work[row][column + 4] = row == column ? 1.0 : 0.0;
    }
  for (int column = 0; column < 4; column++) {
    int pivot = column;
    for (int row = column + 1; row < 4; row++)
      if (fabs(work[row][column]) > fabs(work[pivot][column]))
        pivot = row;
    if (fabs(work[pivot][column]) < 1e-12)
      return false;
    if (pivot != column)
      for (int at = 0; at < 8; at++)
        std::swap(work[pivot][at], work[column][at]);
    const double scale = 1.0 / work[column][column];
    for (int at = 0; at < 8; at++)
      work[column][at] *= scale;
    for (int row = 0; row < 4; row++) {
      if (row == column)
        continue;
      const double factor = work[row][column];
      for (int at = 0; at < 8; at++)
        work[row][at] -= factor * work[column][at];
    }
  }
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++)
      inverse[row][column] = work[row][column + 4];
  return true;
}

} // namespace

bool
DeviceState::clip_space_planes(bool world_space, float out[kClipPlanes * 4]) const {
  const DWORD enabled = render_.clip_plane_enable();
  if (!enabled)
    return false;
  memset(out, 0, kClipPlanes * 4 * sizeof(float));
  // a world position w becomes the clip position w * View * Projection (a row vector times M), and dot(w * M, q) is
  // dot(w, M q): the clip-space plane q whose M q is the world plane is the inverse of M times it
  double inverse[4][4] = {};
  if (world_space) {
    D3DMATRIX view_projection{};
    for (int row = 0; row < 4; row++)
      for (int column = 0; column < 4; column++) {
        float sum = 0.0f;
        for (int at = 0; at < 4; at++)
          sum += view_.m[row][at] * projection_.m[at][column];
        view_projection.m[row][column] = sum;
      }
    if (!invert(view_projection, inverse)) {
      ERR_ONCE("D3D9: the view and projection matrices have no inverse, so the user clip planes cannot be carried to "
               "clip space; the draw is not clipped by them.");
      return false;
    }
  }
  for (UINT plane = 0; plane < kClipPlanes; plane++) {
    if (!(enabled & (1u << plane)))
      continue;
    for (UINT row = 0; row < 4; row++) {
      if (!world_space) {
        out[plane * 4 + row] = clip_planes_[plane][row];
        continue;
      }
      double sum = 0.0;
      for (UINT at = 0; at < 4; at++)
        sum += inverse[row][at] * clip_planes_[plane][at];
      out[plane * 4 + row] = (float)sum;
    }
  }
  return true;
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
  const UINT which = (UINT)state;
  // D3DTS_WORLD is D3DTS_WORLDMATRIX(0), and the 256 matrices run from there (Microsoft Learn, D3DTS_WORLDMATRIX)
  if (which >= (UINT)D3DTS_WORLDMATRIX(0) && which < (UINT)D3DTS_WORLDMATRIX(256))
    return &world_[which - (UINT)D3DTS_WORLDMATRIX(0)];
  switch (which) {
  case D3DTS_VIEW:
    return &view_;
  case D3DTS_PROJECTION:
    return &projection_;
  default:
    if (which >= D3DTS_TEXTURE0 && which <= D3DTS_TEXTURE7)
      return &texture_[which - D3DTS_TEXTURE0];
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
  lights_.constants(blended_world(), view_, render_, out);
}

// D3DVBF_TWEENING draws with (1 - factor) * WORLD + factor * WORLD1 (Microsoft Learn, D3DRS_TWEENFACTOR). every other
// blend mode leaves WORLD as the matrix a single-matrix draw reads
D3DMATRIX
DeviceState::blended_world() const {
  if (render_.vertex_blend() != D3DVBF_TWEENING)
    return world_[0];
  const float factor = render_.tween_factor();
  D3DMATRIX world{};
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++)
      world.m[row][column] =
          world_[0].m[row][column] + (world_[1].m[row][column] - world_[0].m[row][column]) * factor;
  return world;
}

void
DeviceState::view_projection(float out[16]) const {
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++) {
      float sum = 0.0f;
      for (int at = 0; at < 4; at++)
        sum += view_.m[row][at] * projection_.m[at][column];
      out[row * 4 + column] = sum;
    }
}

const float *
DeviceState::world_view_projection() const {
  const DWORD blend = render_.vertex_blend();
  const DWORD tween = render_.tween_factor_bits();
  if (world_view_projection_current_ && cached_vertex_blend_ == blend && cached_tween_ == tween)
    return world_view_projection_;
  const D3DMATRIX world = blended_world();
  D3DMATRIX world_view{};
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++) {
      float sum = 0.0f;
      for (int at = 0; at < 4; at++)
        sum += world.m[row][at] * view_.m[at][column];
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
  cached_vertex_blend_ = blend;
  cached_tween_ = tween;
  return world_view_projection_;
}

} // namespace mullion::d3d9
