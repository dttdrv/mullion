#pragma once
#include <algorithm>
#include <cstdint>

namespace mullion {

// Metal textureBuffer max width is 2^28 texels (Metal Feature Set Tables). a D3D view may start past that, so
// each view is a Metal texture of its own window rather than the whole allocation (GitHub #67).
inline constexpr uint64_t kMetalTextureBufferMaxWidth = 1ull << 28;

// the Metal window of a typed buffer view: offset aligned down to a multiple of max(texel, 16), length covering the
// view, width capped at 2^28 texels. `element_offset` is the shader index of the view's first element in that texture.
struct TypedBufferWindow {
  uint64_t byte_offset = 0;
  uint64_t byte_length = 0;
  uint32_t element_offset = 0;
  bool capped = false;
};

inline TypedBufferWindow
typedBufferWindow(uint64_t byte_offset, uint64_t byte_length, uint32_t texel_size) {
  TypedBufferWindow window{};
  if (!texel_size)
    return window;
  const uint64_t align = std::max<uint64_t>(texel_size, 16);
  window.byte_offset = (byte_offset / align) * align;
  uint64_t extra = byte_offset - window.byte_offset;
  uint64_t length = byte_length ? byte_length + extra : extra;
  uint64_t width = length / texel_size;
  if (width > kMetalTextureBufferMaxWidth) {
    window.capped = true;
    width = kMetalTextureBufferMaxWidth;
    length = width * texel_size;
  }
  if (!width)
    width = 1;
  window.byte_length = width * texel_size;
  window.element_offset = uint32_t(extra / texel_size);
  return window;
}

} // namespace mullion
