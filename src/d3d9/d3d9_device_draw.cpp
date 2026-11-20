// the geometry a Direct3D 9 draw reads, over the D3D11 device's buffers and draw path: the vertex and index buffers,
// the one stream source and the FVFs this build transforms, and the draws that read them (the render states, targets
// and viewport they are drawn under are d3d9_device_targets.cpp's, and the texture stage they sample
// d3d9_device_texture.cpp's).
#include "d3d9_device.hpp"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>

namespace mullion::d3d9 {

// the vertices (or indices, which are one per vertex of a primitive) a primitive count is made of, for every topology
// D3D9 accepts. D3DPT_TRIANGLEFAN has no D3D11 primitive and is counted like a strip -- n primitives have n + 2
// vertices -- and drawn as the triangle list it expands to (bind_fan_triangle_list below).
bool
MTLD3D9Device::topology_count(D3DPRIMITIVETYPE type, UINT primitive_count, UINT &count) {
  // the vertices (or indices) a primitive count is made of, in 64 bits: a count near UINT_MAX must be refused,
  // never narrowed into a smaller draw (Microsoft Learn, IDirect3DDevice9::DrawIndexedPrimitive)
  uint64_t vertices = 0;
  switch (type) {
  case D3DPT_POINTLIST:
    vertices = primitive_count;
    break;
  case D3DPT_LINELIST:
    vertices = (uint64_t)primitive_count * 2;
    break;
  case D3DPT_LINESTRIP:
    vertices = (uint64_t)primitive_count + 1;
    break;
  case D3DPT_TRIANGLELIST:
    vertices = (uint64_t)primitive_count * 3;
    break;
  case D3DPT_TRIANGLESTRIP:
  case D3DPT_TRIANGLEFAN:
    vertices = (uint64_t)primitive_count + 2;
    break;
  default:
    return false;
  }
  if (vertices > UINT_MAX)
    return false;
  count = (UINT)vertices;
  return true;
}

// D3D9 caps the primitives one call may draw (D3DCAPS9::MaxPrimitiveCount), and a count past it is refused rather
// than handed to the device as a count it never agreed to
bool
MTLD3D9Device::primitive_count_fits(UINT primitive_count) const {
  return primitive_count <= caps_.MaxPrimitiveCount;
}

// D3D9's DrawPrimitiveUP clears stream 0, so GetStreamSource reports NULL afterwards; the device's own stream state is
// cleared here as well, and a recording state block records the change
void
MTLD3D9Device::release_stream(UINT stream) {
  if (streams_[stream].buffer)
    streams_[stream].buffer->release_private_ref();
  streams_[stream].source = nullptr;
  streams_[stream].buffer = nullptr;
}

void
MTLD3D9Device::clear_stream(UINT stream) {
  release_stream(stream);
  streams_[stream].offset = 0;
  streams_[stream].stride = 0;
}

void
MTLD3D9Device::release_indices() {
  if (index_buffer_)
    index_buffer_->release_private_ref();
  indices_ = nullptr;
  index_buffer_ = nullptr;
}

void
MTLD3D9Device::bind_streams() {
  ID3D11Buffer *buffers[kMaxStreams] = {};
  UINT strides[kMaxStreams] = {};
  UINT offsets[kMaxStreams] = {};
  for (UINT stream = 0; stream < kMaxStreams; stream++) {
    if (streams_[stream].buffer) {
      buffers[stream] = streams_[stream].buffer->buffer();
      strides[stream] = streams_[stream].stride;
      offsets[stream] = streams_[stream].offset;
    }
  }
  context_->IASetVertexBuffers(0, kMaxStreams, buffers, strides, offsets);
}

void
MTLD3D9Device::unbind_stream_source() {
  // DrawPrimitiveUP / DrawIndexedPrimitiveUP clear stream 0 without recording it: a BeginStateBlock around a UP
  // draw still leaves GetStreamSource NULL afterwards (Wine device.c test_draw_primitive).
  clear_stream(0);
  bind_streams();
}

// DrawIndexedPrimitiveUP clears the index buffer too, so GetIndices reports NULL afterwards
void
MTLD3D9Device::unbind_indices() {
  release_indices();
  context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
}

HRESULT
MTLD3D9Device::bind_position_fvf_stream(UINT start_vertex, UINT vertex_count) {
  if (!streams_[0].buffer || streams_[0].stride != kPositionStride || !vertex_count)
    return D3DERR_INVALIDCALL;
  const UINT src_stride = kPositionStride;
  const UINT dst_stride = kTransformedStride;
  const uint64_t bytes = (uint64_t)vertex_count * dst_stride;
  if (bytes > UINT_MAX)
    return D3DERR_INVALIDCALL;
  if (!up_scratch_ || up_scratch_size_ < bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(bytes, 256);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    up_scratch_ = scratch;
    up_scratch_size_ = desc.ByteWidth;
  }
  const char *source = streams_[0].buffer->contents();
  if (!source)
    return D3DERR_INVALIDCALL;
  const UINT base = streams_[0].offset + start_vertex * src_stride;
  if ((uint64_t)base + (uint64_t)vertex_count * src_stride > streams_[0].buffer->size())
    return D3DERR_INVALIDCALL;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(up_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  auto *out = static_cast<TransformedVertex *>(mapped.pData);
  for (UINT at = 0; at < vertex_count; at++) {
    const float *xyz = reinterpret_cast<const float *>(source + base + at * src_stride);
    out[at].x = xyz[0];
    out[at].y = xyz[1];
    out[at].z = xyz[2];
    out[at].colour = 0xFFFFFFFFu;
  }
  context_->Unmap(up_scratch_.Get(), 0);
  ID3D11Buffer *buffer = up_scratch_.Get();
  const UINT stride = dst_stride;
  const UINT offset = 0;
  context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
  return S_OK;
}

// The format the fixed-function pipeline draws for the format the application named. A format with no diffuse colour is
// drawn with opaque white, which every layout of the pipeline supplies from a COLOR element; the pipeline has no shader
// without that element, so the draw gathers its vertices into the diffuse format's layout instead (Wine d3d9ex.c
// test_user_memory draws D3DFVF_XYZ | D3DFVF_TEX1 with no diffuse).
DWORD
MTLD3D9Device::packed_fixed_function_fvf(DWORD fvf) const {
  // D3DFVF_XYZ alone carries no colour, and the pipeline's layout reads the colour at the byte after the position: a
  // 12-byte vertex read that way finds the next vertex's position, which draws a position-shaped gradient instead of
  // D3D9's opaque white (Wine visual.c test_viewport draws D3DFVF_XYZ with lighting off and expects white). The
  // position-only draw therefore expands to the diffuse layout as every other no-diffuse format does, and the vertex
  // the stream holds is gathered with white where the FVF has no colour (pack_up_vertices and
  // bind_position_fvf_stream both fill it).
  // a one-component coordinate is a stride, not a shader: the pipeline's coordinate element reads the first two
  // components of a two-component coordinate, and the gather pads the missing v with D3D9's zero (Wine visual.c
  // texture_transform_flags_test's FLOAT1 declaration draw)
  if (fvf == (D3DFVF_XYZ | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT1 << 16)) ||
      fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT1 << 16)))
    return D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  if (fvf == kPositionFVF)
    return kTransformedFVF;
  if (fvf == kTransformedTexNoDiffuseFVF)
    return kTransformedTexturedFVF;
  if (fvf == kPreTransformedTexNoDiffuseFVF)
    return kPreTransformedTexturedFVF;
  if (fvf == kLitNoDiffuseFVF)
    return kLitFVF;
  if (fvf == kLitNoDiffuseTexturedFVF)
    return kLitTexturedFVF;
  return fvf;
}

// Gather the vertices a fixed-function draw reads into the canonical layout of `packed_fvf`. A declaration may place its
// elements in more than one stream, or at offsets of its own (Wine d3d9ex.c test_sysmem_draw splits position and colour
// over streams 0 and 1), which the pipeline's static input layouts cannot read; each element's bytes are copied from
// the stream and offset it names into the layout of the format the declaration stands for. An element the format has
// and the declaration does not is a colour, which D3D9 gives as opaque white.
HRESULT
MTLD3D9Device::bind_gathered_declaration_stream(
    const std::vector<D3DVERTEXELEMENT9> &elements, UINT vertex_count, DWORD packed_fvf, UINT instance
) {
  std::vector<D3DVERTEXELEMENT9> wanted;
  if (!elements_from_fvf(packed_fvf, wanted))
    return D3DERR_INVALIDCALL;
  const UINT stride = FixedFunctionPipeline::stride(packed_fvf);
  const uint64_t bytes = (uint64_t)vertex_count * stride;
  if (!vertex_count || !stride || bytes > UINT_MAX)
    return D3DERR_INVALIDCALL;
  if (!decl_scratch_ || decl_scratch_size_ < bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(bytes, 1024);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    decl_scratch_ = scratch;
    decl_scratch_size_ = desc.ByteWidth;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(decl_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  auto unmap_fail = [&]() {
    context_->Unmap(decl_scratch_.Get(), 0);
    return D3DERR_INVALIDCALL;
  };
  auto *output = static_cast<char *>(mapped.pData);
  memset(output, 0, (size_t)bytes);
  for (UINT vertex = 0; vertex < vertex_count; vertex++) {
    char *out = output + (size_t)vertex * stride;
    for (const auto &element : wanted) {
      const D3DVERTEXELEMENT9 *source = nullptr;
      for (const auto &candidate : elements) {
        if (candidate.Usage == element.Usage && candidate.UsageIndex == element.UsageIndex) {
          source = &candidate;
          break;
        }
      }
      if (!source) {
        if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex == 0) {
          *reinterpret_cast<D3DCOLOR *>(out + element.Offset) = 0xFFFFFFFFu;
          continue;
        }
        return unmap_fail();
      }
      const UINT size = declaration_element_size(source->Type);
      const UINT target_size = declaration_element_size(element.Type);
      // a texture coordinate packed into a wider canonical one keeps its components and leaves the rest zero, and one
      // wider than the canonical keeps the first components the pipeline reads
      if (!size || !target_size || (element.Usage != D3DDECLUSAGE_TEXCOORD && size != target_size) ||
          source->Stream >= kMaxStreams)
        return unmap_fail();
      const UINT copied = element.Usage == D3DDECLUSAGE_TEXCOORD ? std::min(size, target_size) : size;
      const StreamSlot &stream = streams_[source->Stream];
      const char *contents = stream.buffer ? stream.buffer->contents() : nullptr;
      if (!contents || !stream.stride)
        return unmap_fail();
      // a stream whose frequency names per-instance data is read at the vertices of the instance being gathered
      // (D3DSTREAMSOURCE_INSTANCEDATA's count is how many vertices one instance has)
      const DWORD frequency = stream_freq_[source->Stream];
      const uint64_t per_instance = (frequency & D3DSTREAMSOURCE_INSTANCEDATA) ? (frequency & 0x3FFFFFFFu) : 0;
      const uint64_t source_vertex = (uint64_t)vertex + (uint64_t)instance * per_instance;
      uint64_t at = 0;
      if (__builtin_mul_overflow(source_vertex, (uint64_t)stream.stride, &at) ||
          __builtin_add_overflow(at, (uint64_t)stream.offset + source->Offset, &at) || at + copied > stream.buffer->size())
        return unmap_fail();
      memcpy(out + element.Offset, contents + at, copied);
    }
  }
  context_->Unmap(decl_scratch_.Get(), 0);
  ID3D11Buffer *buffer = decl_scratch_.Get();
  const UINT offset = 0;
  context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
  return S_OK;
}

// The same gathering for the vertices DrawPrimitiveUP and DrawIndexedPrimitiveUP carry on the CPU: one array with one
// stride, read through the elements the declared format's layout names.
HRESULT
MTLD3D9Device::pack_up_vertices(
    const void *vertices, UINT stride, UINT vertex_count, DWORD declared_fvf, DWORD packed_fvf, void *destination
) {
  std::vector<D3DVERTEXELEMENT9> source, wanted;
  if (!elements_from_fvf(declared_fvf, source) || !elements_from_fvf(packed_fvf, wanted))
    return D3DERR_INVALIDCALL;
  const UINT packed_stride = FixedFunctionPipeline::stride(packed_fvf);
  if (!packed_stride)
    return D3DERR_INVALIDCALL;
  for (UINT vertex = 0; vertex < vertex_count; vertex++) {
    char *out = static_cast<char *>(destination) + (size_t)vertex * packed_stride;
    memset(out, 0, packed_stride);
    for (const auto &element : wanted) {
      const D3DVERTEXELEMENT9 *have = nullptr;
      for (const auto &candidate : source) {
        if (candidate.Usage == element.Usage && candidate.UsageIndex == element.UsageIndex) {
          have = &candidate;
          break;
        }
      }
      if (!have) {
        if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex == 0) {
          *reinterpret_cast<D3DCOLOR *>(out + element.Offset) = 0xFFFFFFFFu;
          continue;
        }
        return D3DERR_INVALIDCALL;
      }
      const UINT size = declaration_element_size(have->Type);
      const UINT target_size = declaration_element_size(element.Type);
      // a one-component coordinate becomes the pipeline's two-component one with v zero, as D3D9 leaves it, and a
      // wider one keeps the components the pipeline reads
      if (!size || !target_size || (element.Usage != D3DDECLUSAGE_TEXCOORD && size != target_size) ||
          (uint64_t)vertex * stride + have->Offset + std::min(size, target_size) > (uint64_t)UINT_MAX)
        return D3DERR_INVALIDCALL;
      memcpy(
          out + element.Offset, static_cast<const char *>(vertices) + (size_t)vertex * stride + have->Offset,
          element.Usage == D3DDECLUSAGE_TEXCOORD ? std::min(size, target_size) : size
      );
    }
  }
  return S_OK;
}

HRESULT
MTLD3D9Device::pack_uv_pair_vertices(
    const void *vertices, UINT stride, UINT vertex_count, DWORD declared_fvf, UINT uv0, UINT uv1, void *destination
) {
  // a declaration's own offsets and order are what its vertices are laid out in: only an FVF draw reads the format's
  // canonical layout (fixed_function_fvf derives the format the declaration names)
  std::vector<D3DVERTEXELEMENT9> derived;
  const std::vector<D3DVERTEXELEMENT9> *source = nullptr;
  if (declaration_inputs_) {
    source = &declaration_inputs_->elements();
  } else {
    if (!elements_from_fvf(declared_fvf, derived))
      return D3DERR_INVALIDCALL;
    source = &derived;
  }
  const D3DVERTEXELEMENT9 *position = nullptr, *colour = nullptr, *first = nullptr, *second = nullptr;
  for (const auto &element : *source) {
    if (element.Usage == D3DDECLUSAGE_POSITION && element.UsageIndex == 0)
      position = &element;
    else if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex == 0)
      colour = &element;
    else if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex == uv0)
      first = &element;
    else if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex == uv1)
      second = &element;
  }
  if (!position || !first || !second || declaration_element_size(position->Type) != 12 ||
      declaration_element_size(first->Type) != 8 || declaration_element_size(second->Type) != 8)
    return D3DERR_INVALIDCALL;
  for (UINT vertex = 0; vertex < vertex_count; vertex++) {
    char *out = static_cast<char *>(destination) + (size_t)vertex * 32;
    const char *in = static_cast<const char *>(vertices) + (size_t)vertex * stride;
    memcpy(out, in + position->Offset, 12);
    if (colour)
      memcpy(out + 12, in + colour->Offset, 4);
    else
      *reinterpret_cast<D3DCOLOR *>(out + 12) = 0xFFFFFFFFu;
    memcpy(out + 16, in + first->Offset, 8);
    memcpy(out + 24, in + second->Offset, 8);
  }
  return S_OK;
}

D3D11_PRIMITIVE_TOPOLOGY
MTLD3D9Device::topology_of(D3DPRIMITIVETYPE type) {
  switch (type) {
  case D3DPT_POINTLIST:
    return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
  case D3DPT_LINELIST:
    return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
  case D3DPT_LINESTRIP:
    return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
  case D3DPT_TRIANGLELIST:
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  case D3DPT_TRIANGLESTRIP:
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  default:
    return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  }
}

// The triangle list a D3DPT_TRIANGLEFAN draw is: D3D11 has no fan primitive, so the fan's indices are expanded into the
// list (v0,v1,v2), (v0,v2,v3), ... and drawn as a triangle list, which keeps every triangle the application asked for.
// `indices` is the application's index array, or this device's copy of its index buffer, at `index_size` bytes each;
// a non-indexed fan passes nullptr and its indices are the vertices from 0. The list is 32 bits wide whatever the
// source, so a 16-bit fan is not narrowed.
HRESULT
MTLD3D9Device::bind_fan_triangle_list(const void *indices, UINT index_size, UINT start_index, UINT primitive_count) {
  const uint64_t bytes = (uint64_t)primitive_count * 3 * sizeof(uint32_t);
  if (bytes > UINT_MAX)
    return D3DERR_INVALIDCALL;
  if (!fan_index_scratch_ || fan_index_scratch_size_ < bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(bytes, 256);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    fan_index_scratch_ = scratch;
    fan_index_scratch_size_ = desc.ByteWidth;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(fan_index_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  uint32_t *list = static_cast<uint32_t *>(mapped.pData);
  const char *source = static_cast<const char *>(indices);
  auto index_at = [&](UINT which) -> uint32_t {
    const UINT at = start_index + which;
    return index_size == 4 ? reinterpret_cast<const uint32_t *>(source)[at]
                           : reinterpret_cast<const uint16_t *>(source)[at];
  };
  // every triangle of a fan keeps the fan's first vertex as its own first vertex: (i0,i1,i2), (i0,i2,i3), ...
  // (Microsoft Learn, D3DPRIMITIVETYPE). Advancing the first vertex with the others would draw a strip instead
  const uint32_t centre = indices ? index_at(0) : 0;
  for (UINT at = 0; at < primitive_count; at++) {
    list[at * 3 + 0] = centre;
    list[at * 3 + 1] = indices ? index_at(at + 1) : (at + 1);
    list[at * 3 + 2] = indices ? index_at(at + 2) : (at + 2);
  }
  context_->Unmap(fan_index_scratch_.Get(), 0);
  context_->IASetIndexBuffer(fan_index_scratch_.Get(), DXGI_FORMAT_R32_UINT, 0);
  return S_OK;
}

// a draw's first half, which every draw shares: the target SetRenderTarget named (the back buffer until it names
// another), the depth-stencil surface it tests against, the render state and the viewport. The pipeline that draws it is
// chosen next: a game's own programs when either stage is one (d3d9_device_shader.cpp), the fixed-function pipeline
// otherwise
HRESULT
MTLD3D9Device::prepare_draw() {
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  // the targets SetRenderTarget set, from 0 to the highest: a pixel shader's oC0..oC3 land in them in order
  // (Microsoft Learn, IDirect3DDevice9::SetRenderTarget, D3DCAPS9::NumSimultaneousRTs)
  ID3D11RenderTargetView *targets[kMaxRenderTargets] = {};
  for (UINT at = 0; at < render_target_count_; at++)
    targets[at] = render_target_views_[at].Get();
  ID3D11DepthStencilView *depth_stencil = bound_depth_stencil();
  context_->OMSetRenderTargets(render_target_count_, targets, depth_stencil);
  // D3D9's z clip depends on the vertex path, not on ZENABLE alone: a programmable vertex shader's clip-space z is
  // clipped, a fixed-function XYZ draw's is too, and a fixed-function XYZRHW draw is clamped into the viewport's depth
  // range (Wine visual.c depth_clamp_test draws z 5 and 10 with XYZRHW and expects them drawn, while its XYZ quads are
  // clipped; zenable_test's no-depth-buffer pretransformed quad keeps z outside 0..1).
  const DWORD fixed_fvf = vertex_shader_ ? 0 : fixed_function_fvf();
  const bool pretransformed = fixed_fvf && (fixed_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
  state_.render().set_depth_clip(vertex_shader_ != nullptr || !pretransformed);
  hr = state_.render().apply(d3d11_device_.Get(), context_.Get());
  if (FAILED(hr))
    return hr;
  bind_viewport_state();
  if (vertex_shader_ || pixel_shader_)
    return prepare_shader_draw();
  return prepare_fixed_function_draw();
}

// what a fixed-function draw is, from the device state: the format (which picks the shaders and the input layout), the
// texture stage it samples, the transform a transformed vertex is multiplied by, and the lights and material a lit
// vertex's colour comes from. Working that out is the draw path's job; turning it into the D3D11 shaders, layouts and
// constant buffers a draw binds is the pipeline's (d3d9_fixed_function.hpp), which is the only place that touches them
DWORD
MTLD3D9Device::fixed_function_fvf() const {
  if (fvf_)
    return fvf_;
  if (!declaration_inputs_)
    return 0;
  DWORD derived = 0;
  if (fvf_from_elements(declaration_inputs_->elements(), derived))
    return derived;
  // a declaration whose elements are split over streams or laid out with offsets of its own still names a format; a
  // draw gathers its vertices into that format's layout (bind_gathered_declaration_stream)
  if (fvf_from_elements_any_stream(declaration_inputs_->elements(), derived))
    return derived;
  return 0;
}

namespace {

// what a stage's texture is, as the fixed-function pixel shaders' dims array reads it: 0 a 2D texture, 1 a cube, 2 a
// volume. a stage with no texture bound is 0, which samples the device's white texel.
UINT32
fixed_function_dimension(const IMTLD3D9Texture *texture) {
  if (!texture)
    return 0;
  switch (texture->dimension()) {
  case D3DRTYPE_CUBETEXTURE:
    return 1;
  case D3DRTYPE_VOLUMETEXTURE:
    return 2;
  default:
    return 0;
  }
}

// a declaration D3D9 blends without an FVF spelling: a position and a D3DCOLOR whose r, g and b are the three
// weights (Wine visual.c test_vertex_blending's D3DCOLOR case)
bool
packed_blend_declaration(const std::vector<D3DVERTEXELEMENT9> *elements) {
  // the declaration stores its elements without the D3DDECL_END terminator
  if (!elements || elements->size() < 2)
    return false;
  const D3DVERTEXELEMENT9 &position = (*elements)[0];
  const D3DVERTEXELEMENT9 &weights = (*elements)[1];
  return position.Stream == 0 && position.Offset == 0 && position.Type == D3DDECLTYPE_FLOAT3 &&
         position.Usage == D3DDECLUSAGE_POSITION && position.UsageIndex == 0 && weights.Stream == 0 &&
         weights.Offset == 12 && weights.Type == D3DDECLTYPE_D3DCOLOR &&
         weights.Usage == D3DDECLUSAGE_BLENDWEIGHT && weights.UsageIndex == 0;
}

} // namespace

HRESULT
MTLD3D9Device::prepare_fixed_function_draw() {
  const DWORD declared = fixed_function_fvf();
  // a multi-set draw is only staged by the UP paths: a streamed one has no gather, so it is refused rather than drawn
  // with one channel's coordinates on every stage
  if (!uv_pair_ && FixedFunctionPipeline::pair_format(declared))
    return D3DERR_INVALIDCALL;
  const bool uv_pair = uv_pair_;
  const DWORD fvf = uv_pair ? (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2) : declared;
  const bool packed_blend =
      !uv_pair && declared == 0 &&
      packed_blend_declaration(declaration_inputs_ ? &declaration_inputs_->elements() : nullptr);
  if (!FixedFunctionPipeline::supports(fvf) && !packed_blend)
    return D3DERR_INVALIDCALL;

  FixedFunctionDraw draw{};
  draw.viewport = viewport_;
  draw.diffuse = (fvf & D3DFVF_DIFFUSE) != 0;
  draw.uv_pair = uv_pair;
  draw.clip = clip_rect();
  draw.flat_shaded = state_.render().flat_shaded();
  draw.fog_table_mode = state_.render().fog_table_mode();
  {
    const DWORD vertex_mode = state_.render().fog_vertex_mode();
    // D3D9's fog is table fog or vertex-Z fog; with both modes D3DFOG_NONE, D3DRS_FOGENABLE fog nothing (Microsoft
    // Learn, D3DRS_FOGTABLEMODE and D3DRS_FOGVERTEXMODE)
    if (state_.render().fog_enabled() && draw.fog_table_mode != D3DFOG_NONE) {
      draw.fog_enabled = true;
      draw.fog_start = state_.render().fog_start();
      draw.fog_end = state_.render().fog_end();
      draw.fog_density = state_.render().fog_density();
    } else if (state_.render().fog_enabled() && vertex_mode != D3DFOG_NONE) {
      draw.fog_enabled = true;
      draw.fog_table_mode = vertex_mode;
      draw.fog_start = state_.render().fog_start();
      draw.fog_end = state_.render().fog_end();
      draw.fog_density = state_.render().fog_density();
    } else {
      draw.fog_enabled = false;
      draw.fog_start = state_.render().fog_start();
      draw.fog_end = state_.render().fog_end();
      draw.fog_density = state_.render().fog_density();
    }
  }
  {
    const DWORD fog_color = state_.render().fog_color();
    draw.fog_color[0] = ((fog_color >> 16) & 0xFF) / 255.0f;
    draw.fog_color[1] = ((fog_color >> 8) & 0xFF) / 255.0f;
    draw.fog_color[2] = (fog_color & 0xFF) / 255.0f;
    draw.fog_color[3] = ((fog_color >> 24) & 0xFF) / 255.0f;
  }
  draw.alpha_test = state_.render().alpha_test_enabled() && state_.render().alpha_func() != D3DCMP_ALWAYS;
  draw.alpha_func = state_.render().alpha_func();
  draw.alpha_ref = state_.render().alpha_ref() & 0xffu;
  // a lit draw with D3DRS_SPECULARENABLE on adds the lighting's specular (the lit vertex shader's COLOR1) after the
  // last blend stage, unclamped. The eight-stage combiner carries its own flag (board.misc below); this is the
  // two-stage path's, and it is gated on the lit FVF because the specular channels are the lighting's own output
  draw.spec_add = FixedFunctionPipeline::lit(fvf) && state_.render().lighting_enabled() &&
                  state_.render().specular_enabled();
  draw.fvf = fvf;
  draw.user_pixel = pixel_shader_ && !vertex_shader_;

  // whether the draw needs the eight-stage combiner, whose shaders carry every texture operation and argument the
  // two-stage extended path cannot express: a stage past 1 active (D3D9's MaxTextureBlendStages is eight), an
  // operation or argument the two-stage encoder does not map, a D3DTA_TEMP result, a texture-transform count past
  // COUNT2, or a vertex format with D3DFVF_SPECULAR data. A position-only or blended format stays on the two-stage
  // path, which is as far as this build's shaders for those formats go
  UINT blend_weights = 0;
  bool blend_diffuse = false;
  const bool blend_fvf = FixedFunctionPipeline::blend_format(fvf, blend_weights, blend_diffuse);
  auto two_stage_colour_op = [](DWORD op) {
    return op == D3DTOP_DISABLE || op == D3DTOP_SELECTARG1 || op == D3DTOP_SELECTARG2 || op == D3DTOP_MODULATE ||
           op == D3DTOP_ADD || op == D3DTOP_SUBTRACT;
  };
  auto two_stage_arg = [](DWORD arg) {
    // an argument the extended shader's encoder maps: the modifiers (a complement, an alpha replication) and the
    // spec, temp and constant arguments past the factor are the eight-stage combiner's
    if ((arg & D3DTA_COMPLEMENT) || (arg & D3DTA_ALPHAREPLICATE) ||
        (arg & D3DTA_SELECTMASK) > D3DTA_TFACTOR)
      return false;
    return true;
  };
  // a lit format's specular is the lighting's own COLOR1, which the two-stage lit shaders write: D3DFVF_SPECULAR is
  // then a material source, not an eight-stage argument, so it does not force the combiner here
  // a specular FVF takes the two-stage lit shaders only when lighting is actually on: the lit shaders' COLOR1 is the
  // lighting's own specular, while with lighting off the specular is the vertex's and the eight-stage combiner carries
  // its D3DFVF_SPECULAR channel through the stages and the fog
  const bool lighting_on = state_.render().lighting_enabled();
  bool needs_eight = (fvf & D3DFVF_SPECULAR) != 0 && !blend_fvf &&
                     !(FixedFunctionPipeline::lit(fvf) && lighting_on);
  if (!needs_eight && !blend_fvf) {
    for (UINT stage = 0; stage < kMaxBlendStages && !needs_eight; stage++) {
      const TextureStageState9 &one = texture_stages_[stage];
      if (stage >= 2)
        needs_eight = one.colour_op() != D3DTOP_DISABLE;
      else
        needs_eight = !two_stage_colour_op(one.colour_op()) || !two_stage_colour_op(one.alpha_op()) ||
                      !two_stage_arg(one.colour_arg1()) || !two_stage_arg(one.colour_arg2()) ||
                      !two_stage_arg(one.alpha_arg1()) || !two_stage_arg(one.alpha_arg2()) ||
                      !two_stage_arg(one.colour_arg0()) || !two_stage_arg(one.alpha_arg0()) ||
                      (one.result_arg() & D3DTA_SELECTMASK) == D3DTA_TEMP ||
                      one.texture_transform_flags() > D3DTTFF_COUNT2;
    }
  }
  // an untextured draw still combines its stages when they name anything but the skipped default: with no texture
  // bound to stage 0, its first texture argument means the stage is not processed and the plain untextured shader is
  // the vertex colour. A stage that adds, subtracts or selects the factor or the diffuse is the eight-stage
  // combiner's (Wine visual.c texop_range_test draws XYZ|DIFFUSE with no texture and two active stages)
  if (!needs_eight && !blend_fvf && !FixedFunctionPipeline::textured(fvf)) {
    for (UINT stage = 0; stage < 2 && !needs_eight; stage++) {
      const TextureStageState9 &one = texture_stages_[stage];
      const bool skipped = one.colour_arg1() == D3DTA_TEXTURE && !texture_sources_[stage];
      needs_eight = !skipped && (one.colour_op() != D3DTOP_DISABLE || one.alpha_op() != D3DTOP_DISABLE);
    }
  }
  // a coordinate three or four components wide is carried whole only by the wide family, which the eight-stage path
  // binds: the two-stage shaders read the coordinate's first two components and fill the rest themselves, so a game
  // pixel shader that reads t0.z or t0.w links against a value the vertex never supplied. texkill compares UVW in
  // ps_1_1 and all four from ps_2_0 up, and a ps_1_1 `mov r0, t0` shows every component (Wine visual.c texkill_test,
  // ids 6362..6452; Microsoft Learn, texkill - ps)
  if (!needs_eight && !blend_fvf && pixel_shader_ && !vertex_shader_ && FixedFunctionPipeline::texcoord_width(fvf) >= 3)
    needs_eight = true;
  FixedFunctionSampling sampling{};   // these outlive bind(), which reads their pointers
  FixedFunctionEightStage eight{};
  if (needs_eight) {
    // the eight stages' board: each stage's raw operation and argument values, the constant, factor, fog, alpha and
    // specular blocks, the texture matrices a transformed format's stages read, and the eight stages' samplers
    for (UINT stage = 0; stage < kMaxBlendStages; stage++) {
      const TextureStageState9 &one = texture_stages_[stage];
      one.eight_board(eight.board.ops[stage], eight.board.args[stage], eight.board.args0[stage]);
      // D3DTTFF_PROJECTED's divisor is the coordinate's own last component when the flags name no count, and a count
      // the vertex does not carry leaves the stage unprojected (Wine visual.c projected_textures_test: PROJECTED alone
      // divides a FLOAT3 coordinate by z and a FLOAT4 one by w, while COUNT3|PROJECTED over a FLOAT2 one draws
      // non-projected). The combiner's texture_coordinate() divides by z for COUNT3 and by w otherwise, so the flags
      // are normalised to the count the vertex can supply.
      const DWORD raw_flags = one.texture_transform_flags();
      if ((raw_flags & D3DTTFF_PROJECTED) || raw_flags == 0xffffffffu) {
        const UINT width = FixedFunctionPipeline::texcoord_width(fvf);
        const UINT named = raw_flags & 0xFu;
        UINT count = (raw_flags == 0xffffffffu || named == 0 || named > 4) ? width : named;
        if (count > width || count < 3)
          count = 0;
        eight.board.args[stage][3] = count ? (D3DTTFF_PROJECTED | count) : (raw_flags & 0xFu);
      }
      // a pre-transformed vertex's coordinates are the screen-space data as they were written, which the extended paths
      // read alone
      if (FixedFunctionPipeline::transformed(fvf))
        memcpy(eight.board.tex[stage], &state_.texture_transform(stage), sizeof(eight.board.tex[stage]));
      // a stage whose first colour argument is the texture with none bound to it is not processed at all (Microsoft
      // Learn, Texture Blending); D3DTOP_DISABLE also stops every stage after it
      if (one.colour_arg1() == D3DTA_TEXTURE && !texture_sources_[stage])
        eight.board.ops[stage][0] = D3DTOP_DISABLE;
      const DWORD constant = one.constant_value();
      eight.board.constants8[stage][0] = ((constant >> 16) & 0xFF) / 255.0f;
      eight.board.constants8[stage][1] = ((constant >> 8) & 0xFF) / 255.0f;
      eight.board.constants8[stage][2] = (constant & 0xFF) / 255.0f;
      eight.board.constants8[stage][3] = ((constant >> 24) & 0xFF) / 255.0f;
      // the stage's texture matrix: a pre-transformed vertex's coordinates are the screen-space data as they were
      // written, which the extended paths read alone
      eight.samplers[stage] = sampler_states_[stage].state(d3d11_device_.Get());
      if (!eight.samplers[stage])
        return D3DERR_INVALIDCALL;
      // the stage's texture at the binding slot its shader reads; bind_eight gives the white texel to a null one
      if (texture_sources_[stage])
        eight.views[stage] = texture_sources_[stage]->view();
      eight.board.dims[stage][0] = fixed_function_dimension(texture_sources_[stage]);
    }
    // D3DRS_TEXTUREFACTOR, as the shader reads a colour
    const DWORD factor = state_.render().texture_factor();
    eight.board.factor[0] = ((factor >> 16) & 0xFF) / 255.0f;
    eight.board.factor[1] = ((factor >> 8) & 0xFF) / 255.0f;
    eight.board.factor[2] = (factor & 0xFF) / 255.0f;
    eight.board.factor[3] = ((factor >> 24) & 0xFF) / 255.0f;
    eight.board.fog[0] = draw.fog_enabled ? 1.0f : 0.0f;
    eight.board.fog[1] = draw.fog_start;
    eight.board.fog[2] = draw.fog_end;
    eight.board.fog[3] = draw.fog_density;
    memcpy(eight.board.fogcolor, draw.fog_color, sizeof(eight.board.fogcolor));
    eight.board.alpha[0] = draw.alpha_test ? 1u : 0u;
    eight.board.alpha[1] = draw.alpha_func;
    eight.board.alpha[2] = draw.alpha_ref;
    eight.board.alpha[3] = draw.fog_table_mode;
    // D3DRS_SPECULARENABLE adds COLOR1 to the colour after the last stage (tests/d3d9/fixed_function_shaders.cpp)
    eight.board.misc[0] = state_.render().specular_enabled() ? 1u : 0u;
    eight.textured = FixedFunctionPipeline::textured(fvf);
    eight.vertex_specular = (fvf & D3DFVF_SPECULAR) != 0;
    eight.lit = FixedFunctionPipeline::lit(fvf) && lighting_on;
    draw.eight_stage = &eight;
  } else if (FixedFunctionPipeline::textured(fvf) &&
             (texture_sources_[0] || !texture_stages_[0].skipped_without_texture() ||
              // a game pixel shader bound with no vertex shader reads the FVF's texture coordinates even when no texture
              // is bound: D3D9's fixed-function vertex stage interpolates what the FVF names, and the untextured vertex
              // shader writes no oT0 (Wine visual.c cnd_test binds no texture and its pixel shader reads t0)
              (pixel_shader_ && !vertex_shader_))) {
    // the two-stage extended path: stages 0 and 1 only, the operations its shader carries
    {
      sampling.texture = texture_sources_[0] ? texture_sources_[0]->view() : nullptr;
      sampling.dims[0][0] = fixed_function_dimension(texture_sources_[0]);
      sampling.sampler = sampler_states_[0].state(d3d11_device_.Get());
      if (!sampling.sampler)
        return D3DERR_INVALIDCALL;
      texture_stages_[0].constants(sampling.ops, sampling.args);
      if (texture_stages_[1].colour_op() != D3DTOP_DISABLE &&
          (texture_sources_[1] || !texture_stages_[1].skipped_without_texture())) {
        sampling.stage1 = true;
        sampling.texture1 = texture_sources_[1] ? texture_sources_[1]->view() : nullptr;
        sampling.dims[1][0] = fixed_function_dimension(texture_sources_[1]);
        sampling.sampler1 = sampler_states_[1].state(d3d11_device_.Get());
        if (!sampling.sampler1)
          return D3DERR_INVALIDCALL;
        texture_stages_[1].constants(sampling.ops1, sampling.args1, true);
        // stage 1's own texture transform: its matrix and flags are not stage 0's (Wine visual.c test_texcoordindex's
        // D3DTTFF_COUNT2 stage 1 pass). D3DTTFF_PROJECTED alone behaves like COUNT4 | PROJECTED.
        DWORD flags1 = texture_stages_[1].texture_transform_flags();
        if (flags1 == D3DTTFF_PROJECTED)
          flags1 = D3DTTFF_COUNT4 | D3DTTFF_PROJECTED;
        const DWORD count1 = flags1 & 0xFu;
        if (FixedFunctionPipeline::transformed(fvf) && count1 >= 2 && count1 <= 4) {
          sampling.transform1 = flags1;
          memcpy(sampling.tex1, &state_.texture_transform(1), sizeof(sampling.tex1));
        }
      }
      // COUNT2, COUNT3 and COUNT4 transform the coordinate; COUNT1 is the passthrough Wine's test treats as
      // disable. a vertex the device transforms carries its coordinate through the matrix; a pre-transformed
      // (XYZRHW) one is the screen-space data as written and reads it without one (Wine visual.c
      // texture_transform_flags_test's XYZRHW quad)
      const DWORD transform_flags = texture_stages_[0].texture_transform_flags() & ~D3DTTFF_PROJECTED;
      if (FixedFunctionPipeline::transformed(fvf) && transform_flags >= D3DTTFF_COUNT2 &&
          transform_flags <= D3DTTFF_COUNT4) {
        sampling.transform0 = true;
        memcpy(sampling.tex0, &state_.texture_transform(0), sizeof(sampling.tex0));
      }
      // D3DRS_TEXTUREFACTOR, as the shader reads a colour
      const DWORD factor = state_.render().texture_factor();
      sampling.factor[0] = ((factor >> 16) & 0xFF) / 255.0f;
      sampling.factor[1] = ((factor >> 8) & 0xFF) / 255.0f;
      sampling.factor[2] = (factor & 0xFF) / 255.0f;
      sampling.factor[3] = ((factor >> 24) & 0xFF) / 255.0f;
      draw.sampling = &sampling;
    }
  }
  // a vertex the device transforms reaches the rasterizer as the clip position its matrices make, which the shader
  // computes from the composed matrix; one already in screen space is mapped through the viewport by its shader, so it
  // has no matrix to read
  float blend_worlds[64];
  float blend_view_projection[16];
  if (packed_blend || FixedFunctionPipeline::blend_format(fvf, blend_weights, blend_diffuse)) {
    if (packed_blend) {
      blend_weights = 3;
      draw.blend_packed = true;
    }
    draw.blend_weights = blend_weights;
    const DWORD mode = state_.render().vertex_blend();
    if (mode == D3DVBF_1WEIGHTS || mode == D3DVBF_2WEIGHTS || mode == D3DVBF_3WEIGHTS)
      draw.blend_active = std::min<UINT>(mode, blend_weights);
    for (UINT index = 0; index < 4; index++)
      memcpy(blend_worlds + index * 16, &state_.world_matrix(index), sizeof(D3DMATRIX));
    if (mode == D3DVBF_TWEENING) {
      const D3DMATRIX tweened = state_.blended_world();
      memcpy(blend_worlds, &tweened, sizeof(tweened));
      draw.blend_active = 0;
    }
    state_.view_projection(blend_view_projection);
    if (packed_blend)
      draw.world_view_projection = state_.world_view_projection();
    draw.blend_worlds = blend_worlds;
    draw.view_projection = blend_view_projection;
  }
  float clip_planes[DeviceState::kClipPlanes * 4];
  if (FixedFunctionPipeline::transformed(fvf)) {
    draw.world_view_projection = state_.world_view_projection();
    // the user clip planes D3DRS_CLIPPLANEENABLE names, which the fixed-function path takes in world space. the pair
    // shader has no clipped variant, so a gather draw stays on it rather than switching to the clipped shader
    if (!uv_pair && state_.clip_space_planes(true, clip_planes))
      draw.clip_planes = clip_planes;
  }
  // a vertex with a normal has its colour computed from the device's lights and material (d3d9_lights.cpp): the material,
  // the lights and the lighting states are worked out here, once per draw, from what the application set
  LightConstants lighting{};
  if (FixedFunctionPipeline::lit(fvf)) {
    state_.lighting_constants(lighting);
    draw.lighting = &lighting;
  }
  draw.position_only = (fvf == kPositionFVF);
  return fixed_function_.bind(context_.Get(), draw);
}

HRESULT
MTLD3D9Device::CreateVertexBuffer(
    UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **vertex_buffer, HANDLE *shared_handle
) {
  if (shared_handle)
    return extended_ ? D3DERR_NOTAVAILABLE : E_NOTIMPL;
  if (!vertex_buffer || !length)
    return D3DERR_INVALIDCALL;
  *vertex_buffer = nullptr;
  // Wine device.c test_buffer_alignment: D3DPOOL_SCRATCH vertex buffers are refused; SYSTEMMEM/MANAGED/DEFAULT work.
  // A D3D9Ex device refuses MANAGED too (Wine d3d9ex.c test_resource_access tests 14 to 16).
  if (pool == D3DPOOL_SCRATCH || (extended_ && pool == D3DPOOL_MANAGED))
    return D3DERR_INVALIDCALL;
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING | D3DUSAGE_NPATCHES))
    return D3DERR_INVALIDCALL;
  // a default-pool buffer belongs to the device the frames failed on, which a Reset replaces
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }

  // SYSTEMMEM is CPU-authored but still drawn: keep a DYNAMIC vertex buffer the IA can bind (Wine device.c
  // test_pinned_buffers draws SYSTEMMEM streams). SCRATCH vertex buffers are refused above.
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = (length + 15u) & ~15u; // a D3D11 buffer is mapped in whole units of 16 bytes; D3D9's length stays
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Buffer> buffer;
  if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &buffer))) {
    ERR("D3D9: no vertex buffer of ", length, " bytes");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9VertexBuffer> result;
  result.Attach(new MTLD3D9VertexBuffer(
      static_cast<IDirect3DDevice9 *>(this), buffer, context_, length, usage, pool, fvf
  ));
  *vertex_buffer = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateIndexBuffer(
    UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9 **index_buffer,
    HANDLE *shared_handle
) {
  if (shared_handle)
    return extended_ ? D3DERR_NOTAVAILABLE : E_NOTIMPL;
  if (!index_buffer || !length)
    return D3DERR_INVALIDCALL;
  *index_buffer = nullptr;
  if (pool == D3DPOOL_SCRATCH || (extended_ && pool == D3DPOOL_MANAGED))
    return D3DERR_INVALIDCALL;
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM)
    return D3DERR_INVALIDCALL;
  if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING))
    return D3DERR_INVALIDCALL;
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  const UINT index_size = format == D3DFMT_INDEX32 ? 4 : 2;
  if (length % index_size)
    return D3DERR_INVALIDCALL;

  // SYSTEMMEM index buffers are drawn the same way as managed/default ones: DYNAMIC + INDEX_BUFFER bind.
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = (length + 15u) & ~15u; // as a vertex buffer's: whole 16-byte units for the map
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Buffer> buffer;
  if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &buffer)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9IndexBuffer> result;
  result.Attach(new MTLD3D9IndexBuffer(
      static_cast<IDirect3DDevice9 *>(this), buffer, context_, length, usage, pool, format
  ));
  *index_buffer = result.Detach();
  return S_OK;
}

// the vertex formats this build's fixed-function path draws: a vertex in screen space (D3DFVF_XYZRHW) and one the
// device transforms (D3DFVF_XYZ), each with a diffuse colour and, optionally, one set of texture coordinates and a normal
// (which makes the draw lit: d3d9_lights.cpp). A failed SetFVF leaves the device's last format, as D3D9's does
HRESULT
MTLD3D9Device::SetFVF(DWORD fvf) {
  // SetFVF stores the format, and D3D9 also makes the declaration that format implies visible through
  // GetVertexDeclaration. Whether the fixed-function draw path can render it remains a draw-time question.
  if (!fvf) {
    // SetFVF(0) clears the format but leaves the declaration where it is: Wine's device test sets an explicit
    // declaration, calls SetFVF(0) and still expects GetVertexDeclaration to return that declaration
    fvf_ = 0;
    if (recording_)
      record(StateKey{StateKey::FVF, 0, 0});
    return S_OK;
  }
  // D3D9 makes one declaration per format and keeps it: asking for the same format again hands back the same object,
  // which Wine's device test checks by pointer identity and by the reference count it has after three Get calls.
  auto cached = fvf_declarations_.find(fvf);
  if (cached == fvf_declarations_.end()) {
    std::vector<D3DVERTEXELEMENT9> elements;
    if (!elements_from_fvf(fvf, elements))
      return D3DERR_INVALIDCALL;
    auto *converted =
        new MTLD3D9VertexDeclaration(static_cast<IDirect3DDevice9 *>(this), std::move(elements), false);
    converted->hold_device_cache();
    cached = fvf_declarations_.emplace(fvf, converted).first;
  }
  declaration_ = cached->second;
  declaration_inputs_ = cached->second;
  fvf_ = fvf;
  if (recording_)
    record(StateKey{StateKey::FVF, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetFVF(DWORD *fvf) {
  if (!fvf)
    return D3DERR_INVALIDCALL;
  *fvf = fvf_;
  return S_OK;
}

HRESULT
MTLD3D9Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) {
  if (stream >= kMaxStreams)
    return D3DERR_INVALIDCALL;
  if (!buffer) {
    // D3D9 keeps offset and stride when the buffer is cleared (Wine device.c: SetStreamSource(NULL) after offset 4
    // still reports offset 4; stateblock tests keep the captured stride on a cleared stream).
    release_stream(stream);
    if (recording_)
      record(StateKey{StateKey::Stream, stream, 0});
    return S_OK;
  }

  ComPtr<IMTLD3D9Buffer> ours;
  // D3D9 accepts stride 0 on a non-null stream: it is a stream the draw does not read, not an invalid binding
  if (FAILED(buffer->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      offset > ours->size())
    return D3DERR_INVALIDCALL;
  release_stream(stream);
  ours->add_private_ref();
  streams_[stream].source = buffer;
  streams_[stream].buffer = ours.Get();
  streams_[stream].offset = offset;
  streams_[stream].stride = stride;
  if (recording_)
    record(StateKey{StateKey::Stream, stream, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::restore_stream_source(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) {
  const HRESULT hr = SetStreamSource(stream, buffer, offset, stride);
  if (FAILED(hr) || buffer || stream >= kMaxStreams)
    return hr;
  streams_[stream].offset = 0;
  streams_[stream].stride = stride;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) {
  if (stream >= kMaxStreams || !buffer || !offset || !stride)
    return D3DERR_INVALIDCALL;
  *buffer = streams_[stream].source;
  if (*buffer)
    (*buffer)->AddRef();
  *offset = streams_[stream].offset;
  *stride = streams_[stream].stride;
  return S_OK;
}

// instancing is not drawn yet: what is accepted is the setting itself. A setting is a plain frequency (one or more),
// or one flag with a count: the indexed data stream is stream 0, and no stream but 0 takes INSTANCEDATA (Wine visual.c
// stream_test walks every accepted and refused setting and reads each one back). A refused setting leaves the stream's
// frequency as it was.
HRESULT
MTLD3D9Device::SetStreamSourceFreq(UINT stream, UINT setting) {
  if (stream >= kMaxStreams)
    return D3DERR_INVALIDCALL;
  const UINT count = setting & 0x3FFFFFFFu;
  const UINT flags = setting & ~0x3FFFFFFFu;
  if (!flags) {
    if (!count)
      return D3DERR_INVALIDCALL;
  } else {
    if (flags != D3DSTREAMSOURCE_INDEXEDDATA && flags != D3DSTREAMSOURCE_INSTANCEDATA)
      return D3DERR_INVALIDCALL;
    if (stream == 0 && flags == D3DSTREAMSOURCE_INSTANCEDATA)
      return D3DERR_INVALIDCALL;
  }
  stream_freq_[stream] = setting;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetStreamSourceFreq(UINT stream, UINT *setting) {
  if (stream >= kMaxStreams || !setting)
    return D3DERR_INVALIDCALL;
  *setting = stream_freq_[stream];
  return S_OK;
}

HRESULT
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *indices) {
  if (!indices) {
    release_indices();
    if (recording_)
      record(StateKey{StateKey::Indices, 0, 0});
    return S_OK;
  }
  ComPtr<IMTLD3D9Buffer> ours;
  if (FAILED(indices->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(ours.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  release_indices();
  ours->add_private_ref();
  indices_ = indices;
  index_buffer_ = ours.Get();
  if (recording_)
    record(StateKey{StateKey::Indices, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **indices) {
  if (!indices)
    return D3DERR_INVALIDCALL;
  *indices = indices_;
  if (*indices)
    (*indices)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!primitive_count_fits(primitive_count))
    return D3DERR_INVALIDCALL;
  const DWORD ff_fvf = fixed_function_fvf();
  // a declaration the pipeline's static layouts cannot read as it is (its elements split over streams, or placed at
  // offsets of its own) is gathered into the format it names
  bool gathered = false;
  std::vector<D3DVERTEXELEMENT9> gather_elements;
  // a fixed-function draw's stream is the FVF's size; a shader draw reads the stride the stream was set with
  if (!streams_[0].buffer)
    return D3DERR_INVALIDCALL;
  if (!vertex_shader_ && !pixel_shader_) {
    const bool packed_blend_fvf =
        ff_fvf == 0 && packed_blend_declaration(declaration_inputs_ ? &declaration_inputs_->elements() : nullptr);
    if ((!ff_fvf || !FixedFunctionPipeline::supports(ff_fvf)) && !packed_blend_fvf)
      return D3DERR_INVALIDCALL;
    if (declaration_inputs_ && !fvf_) {
      DWORD exact = 0;
      gathered = !fvf_from_elements(declaration_inputs_->elements(), exact);
      if (gathered)
        gather_elements = declaration_inputs_->elements();
    }
    if (!gathered && streams_[0].stride < FixedFunctionPipeline::stride(ff_fvf))
      return D3DERR_INVALIDCALL;
  } else if (!declaration_inputs_ && !fvf_) {
    return D3DERR_INVALIDCALL;
  }
  // Reject only arithmetic wrap: Windows/Wine succeed when a TRIANGLELIST reads past a short VB
  // (Wine device.c test_draw_primitive draws six list vertices from a four-vertex quad).
  for (UINT stream = 0; stream < kMaxStreams; stream++) {
    if (!streams_[stream].buffer)
      continue;
    uint64_t vertices = 0, bytes = 0, end = 0;
    if (__builtin_add_overflow((uint64_t)start_vertex, (uint64_t)count, &vertices) ||
        __builtin_mul_overflow(vertices, (uint64_t)streams_[stream].stride, &bytes) ||
        __builtin_add_overflow((uint64_t)streams_[stream].offset, bytes, &end))
      return D3DERR_INVALIDCALL;
  }

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  const bool position_fvf = !vertex_shader_ && ff_fvf == kPositionFVF;
  if (gathered) {
    if ((uint64_t)start_vertex + count > UINT_MAX)
      return D3DERR_INVALIDCALL;
    if (FAILED(hr = bind_gathered_declaration_stream(gather_elements, start_vertex + count, ff_fvf)))
      return hr;
  } else if (position_fvf) {
    if (FAILED(hr = bind_position_fvf_stream(start_vertex, count)))
      return hr;
  } else {
    bind_streams();
  }
  // a non-indexed draw is one instance: D3D9 reads the instance count from the stream D3DSTREAMSOURCE_INDEXEDDATA names,
  // and only DrawIndexedPrimitive uses it (Microsoft Learn, "Instancing"; Wine visual.c stream_test draws one triangle
  // per case with DrawPrimitive and asserts only the first instance's quadrant is drawn)
  const UINT draw_start = position_fvf ? 0 : start_vertex;
  // a point-filled primitive draws its vertices as single pixels, which is D3D11's point list: the topology the
  // application asked for is not rasterized (Microsoft Learn, D3DFILLMODE)
  if (state_.render().point_fill()) {
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context_->Draw(count, draw_start);
    note_gpu_draw();
    return S_OK;
  }
  // a fan is drawn as the triangle list its vertices expand to, from the start vertex the draw names
  if (type == D3DPT_TRIANGLEFAN) {
    if (FAILED(hr = bind_fan_triangle_list(nullptr, 0, 0, primitive_count)))
      return hr;
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->DrawIndexed(primitive_count * 3, 0, draw_start);
    note_gpu_draw();
    return S_OK;
  }
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->Draw(count, draw_start);
  note_gpu_draw();
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE type, INT base_vertex, UINT min_vertex, UINT vertex_count, UINT start_index, UINT primitive_count
) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  // D3DPT_POINTLIST is not a valid type for the indexed method (Microsoft Learn)
  if (type == D3DPT_POINTLIST)
    return D3DERR_INVALIDCALL;
  if (!primitive_count_fits(primitive_count))
    return D3DERR_INVALIDCALL;
  const DWORD ff_fvf = fixed_function_fvf();
  // Windows accepts DrawIndexedPrimitive with a null stream 0 after DrawPrimitiveUP cleared it (Wine device.c
  // test_draw_primitive); the index buffer and a vertex format are still required.
  if (!index_buffer_ || !vertex_count)
    return D3DERR_INVALIDCALL;
  // a declaration split over streams (or laid out at its own offsets) is gathered into the format it names
  bool gathered = false;
  std::vector<D3DVERTEXELEMENT9> gather_elements;
  if (!vertex_shader_ && !pixel_shader_) {
    const bool packed_blend_fvf =
        ff_fvf == 0 && packed_blend_declaration(declaration_inputs_ ? &declaration_inputs_->elements() : nullptr);
    if ((!ff_fvf || !FixedFunctionPipeline::supports(ff_fvf)) && !packed_blend_fvf)
      return D3DERR_INVALIDCALL;
    if (declaration_inputs_ && !fvf_) {
      DWORD exact = 0;
      gathered = !fvf_from_elements(declaration_inputs_->elements(), exact);
      if (gathered)
        gather_elements = declaration_inputs_->elements();
    }
    if (!gathered && streams_[0].buffer && streams_[0].stride < FixedFunctionPipeline::stride(ff_fvf))
      return D3DERR_INVALIDCALL;
  } else if (!declaration_inputs_ && !fvf_) {
    return D3DERR_INVALIDCALL;
  }
  // the vertices the indices may name, which a gathered buffer has to hold as well
  uint64_t gathered_vertices = 0;
  if (gathered) {
    const int64_t first_vertex = (int64_t)base_vertex + min_vertex;
    if (first_vertex < 0 ||
        __builtin_add_overflow((uint64_t)first_vertex, (uint64_t)vertex_count, &gathered_vertices) ||
        gathered_vertices > UINT_MAX)
      return D3DERR_INVALIDCALL;
  }

  // MinVertexIndex is the start of the range and NumVertices its length, not a value the other must exceed. The
  // index count and the range are widened before they are added or multiplied, so a start index near UINT_MAX is a
  // range past the index buffer rather than one that wraps back inside it.
  const UINT index_size = index_buffer_->format() == D3DFMT_INDEX32 ? 4 : 2;
  if (((uint64_t)start_index + count) * index_size > index_buffer_->size())
    return D3DERR_INVALIDCALL;
  if (streams_[0].buffer) {
    const int64_t first_vertex = (int64_t)base_vertex + min_vertex;
    if (first_vertex < 0)
      return D3DERR_INVALIDCALL;
    uint64_t end_vertex = 0;
    if (__builtin_add_overflow((uint64_t)first_vertex, (uint64_t)vertex_count, &end_vertex))
      return D3DERR_INVALIDCALL;
    // the range base + min + num names must sit in the vertices the stream 0 source (after its offset) holds; a
    // NumVertices past the buffer, or a base vertex that pushes it there, is refused (tests/d3d9/d3d9_draw_contract.cpp)
    if (streams_[0].stride && streams_[0].buffer->size() >= streams_[0].offset &&
        end_vertex > (streams_[0].buffer->size() - streams_[0].offset) / streams_[0].stride)
      return D3DERR_INVALIDCALL;
  }

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  if (!gathered)
    bind_streams();
  context_->IASetIndexBuffer(
      index_buffer_->buffer(),
      index_buffer_->format() == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0
  );
  // the number of instances is the count of the stream the draw names as indexed data (D3DSTREAMSOURCE_INDEXEDDATA),
  // whether or not that stream is the geometry's; a stream with INSTANCEDATA is read at the instance's own vertices
  // (Wine visual.c stream_test sets the count on stream 0 even when the geometry is another stream)
  UINT instances = 1;
  for (UINT stream = 0; stream < kMaxStreams; stream++) {
    if (stream_freq_[stream] & D3DSTREAMSOURCE_INDEXEDDATA) {
      instances = stream_freq_[stream] & 0x3FFFFFFFu;
      break;
    }
  }
  // Direct3D 11 reads every instance's vertices from the same buffer; a gathered declaration with per-instance streams
  // is staged one instance at a time instead, with the same indices drawing each (Wine visual.c stream_test's indexed
  // draw puts each instance's position on its own corner)
  if (gathered && instances > 1 && type != D3DPT_TRIANGLEFAN && !state_.render().point_fill()) {
    context_->IASetPrimitiveTopology(topology_of(type));
    for (UINT instance = 0; instance < instances; instance++) {
      if (FAILED(hr = bind_gathered_declaration_stream(
              gather_elements, (UINT)gathered_vertices, ff_fvf, instance
          )))
        return hr;
      context_->DrawIndexed(count, start_index, base_vertex);
    }
    note_gpu_draw();
    return S_OK;
  }
  if (gathered && FAILED(hr = bind_gathered_declaration_stream(gather_elements, (UINT)gathered_vertices, ff_fvf)))
    return hr;
  // as an unindexed point-filled draw, every index is one pixel
  if (state_.render().point_fill()) {
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    if (instances > 1)
      context_->DrawIndexedInstanced(count, instances, start_index, base_vertex, 0);
    else
      context_->DrawIndexed(count, start_index, base_vertex);
    note_gpu_draw();
    return S_OK;
  }
  // the fan's own indices, expanded into a triangle list; a 16-bit index buffer is read as 16-bit here and the list
  // is 32-bit
  if (type == D3DPT_TRIANGLEFAN) {
    if (FAILED(hr = bind_fan_triangle_list(index_buffer_->contents(), index_size, start_index, primitive_count)))
      return hr;
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT list = primitive_count * 3;
    if (instances > 1)
      context_->DrawIndexedInstanced(list, instances, 0, base_vertex, 0);
    else
      context_->DrawIndexed(list, 0, base_vertex);
    note_gpu_draw();
    return S_OK;
  }
  context_->IASetPrimitiveTopology(topology_of(type));
  if (instances > 1)
    context_->DrawIndexedInstanced(count, instances, start_index, base_vertex, 0);
  else
    context_->DrawIndexed(count, start_index, base_vertex);
  note_gpu_draw();
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertices, UINT stride
) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!primitive_count_fits(primitive_count))
    return D3DERR_INVALIDCALL;
  if (!vertices || !stride)
    return D3DERR_INVALIDCALL;
  const DWORD ff_fvf = fixed_function_fvf();
  const bool pair = !vertex_shader_ && FixedFunctionPipeline::pair_format(ff_fvf);
  UINT pair_uv0 = 0, pair_uv1 = 0;
  DWORD packed_fvf = ff_fvf;
  // a multi-coordinate-set draw gathers each stage's selected set into the canonical two-UV layout: stage 0's
  // D3DTSS_TEXCOORDINDEX into TEXCOORD0 and stage 1's into TEXCOORD1, which the pair shaders sample separately
  if (!vertex_shader_ && pair) {
    const DWORD index0 = texture_stages_[0].texcoord_index();
    const DWORD index1 = texture_stages_[1].texcoord_index();
    const UINT sets = (ff_fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if ((index0 & 0xFFFF0000u) || (index1 & 0xFFFF0000u) || stride < FixedFunctionPipeline::stride(ff_fvf) || !sets)
      return D3DERR_INVALIDCALL;
    pair_uv0 = std::min<UINT>(index0 & 0xFFFF, sets - 1);
    pair_uv1 = std::min<UINT>(index1 & 0xFFFF, sets - 1);
    packed_fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;
  }
  // a fixed-function transform runs whenever the draw has no vertex shader, even when a game pixel shader is bound
  // over it; only the exact no-diffuse formats are expanded, while an FVF with 3/4-component texcoords keeps the
  // passthrough path that projected textures need
  if (!vertex_shader_) {
    const DWORD packed = packed_fixed_function_fvf(ff_fvf);
    if (!pixel_shader_) {
      const bool packed_blend_fvf =
          ff_fvf == 0 && packed_blend_declaration(declaration_inputs_ ? &declaration_inputs_->elements() : nullptr);
      if ((!FixedFunctionPipeline::supports(ff_fvf) && !packed_blend_fvf) ||
          stride < (packed_blend_fvf ? 16u : FixedFunctionPipeline::stride(ff_fvf)))
        return D3DERR_INVALIDCALL;
      // the pair gather has already staged the canonical two-UV layout; the format's own packing would bind the
      // declaration's wider stride over those vertices
      if (!pair)
        packed_fvf = packed;
    } else if (packed != ff_fvf) {
      // a game pixel shader may draw a wider stride than the FVF names (Wine's coissue tests pass 24 bytes for
      // XYZ|TEX1); the named elements are still at their FVF offsets, so expand the no-diffuse format at that stride
      if (!FixedFunctionPipeline::supports(ff_fvf) || stride < FixedFunctionPipeline::stride(ff_fvf))
        return D3DERR_INVALIDCALL;
      if (!pair)
        packed_fvf = packed;
    }
  } else if (!declaration_inputs_ && !fvf_) {
    return D3DERR_INVALIDCALL;
  }

  const bool expand_format = packed_fvf != ff_fvf;
  const UINT bind_stride = expand_format ? FixedFunctionPipeline::stride(packed_fvf) : stride;
  const uint64_t bytes = (uint64_t)count * bind_stride;
  if (bytes > UINT_MAX)
    return D3DERR_INVALIDCALL;
  if (!up_scratch_ || up_scratch_size_ < bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(bytes, 1024);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    up_scratch_ = scratch;
    up_scratch_size_ = desc.ByteWidth;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(up_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  if (pair) {
    const HRESULT packed = pack_uv_pair_vertices(vertices, stride, count, ff_fvf, pair_uv0, pair_uv1, mapped.pData);
    if (FAILED(packed)) {
      context_->Unmap(up_scratch_.Get(), 0);
      return packed;
    }
  } else if (expand_format) {
    const HRESULT packed = pack_up_vertices(vertices, stride, count, ff_fvf, packed_fvf, mapped.pData);
    if (FAILED(packed)) {
      context_->Unmap(up_scratch_.Get(), 0);
      return packed;
    }
  } else {
    memcpy(mapped.pData, vertices, (size_t)((uint64_t)count * stride));
  }
  context_->Unmap(up_scratch_.Get(), 0);

  uv_pair_ = pair;
  HRESULT hr = prepare_draw();
  uv_pair_ = false;
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = up_scratch_.Get();
  UINT offset = 0;
  context_->IASetVertexBuffers(0, 1, &buffer, &bind_stride, &offset);
  if (state_.render().point_fill()) {
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context_->Draw(count, 0);
  } else if (type == D3DPT_TRIANGLEFAN) {
    if (FAILED(hr = bind_fan_triangle_list(nullptr, 0, 0, primitive_count))) {
      unbind_stream_source();
      return hr;
    }
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->DrawIndexed(primitive_count * 3, 0, 0);
  } else {
    context_->IASetPrimitiveTopology(topology_of(type));
    context_->Draw(count, 0);
  }
  note_gpu_draw();
  // Following any DrawPrimitiveUP call, the stream 0 settings GetStreamSource reports are NULL (Microsoft Learn)
  unbind_stream_source();
  return S_OK;
}

// a draw whose vertices and indices both come from the CPU: D3D9's DrawIndexedPrimitiveUP reads index_data and data as
// they are, with no stream or index buffer bound, so the two arrays are staged into buffers of this device's and drawn
// as a DrawIndexedPrimitive of them would be (the indices of the array address the vertices of the array, from vertex
// 0, which is why the draw's base vertex is 0)
HRESULT
MTLD3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data,
    D3DFORMAT index_format, const void *data, UINT stride
) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!primitive_count_fits(primitive_count))
    return D3DERR_INVALIDCALL;
  if (!index_data || !data || !stride)
    return D3DERR_INVALIDCALL;
  if (index_format != D3DFMT_INDEX16 && index_format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  const DWORD ff_fvf = fixed_function_fvf();
  // multi-set coordinates are gathered only by the DrawPrimitiveUP path; an indexed UP draw has no gather of them
  if (!vertex_shader_ && FixedFunctionPipeline::pair_format(ff_fvf))
    return D3DERR_INVALIDCALL;
  DWORD packed_fvf = ff_fvf;
  // a fixed-function transform runs whenever the draw has no vertex shader, even when a game pixel shader is bound
  // over it; only the exact no-diffuse formats are expanded, while an FVF with 3/4-component texcoords keeps the
  // passthrough path that projected textures need
  if (!vertex_shader_) {
    const DWORD packed = packed_fixed_function_fvf(ff_fvf);
    if (!pixel_shader_) {
      const bool packed_blend_fvf =
          ff_fvf == 0 && packed_blend_declaration(declaration_inputs_ ? &declaration_inputs_->elements() : nullptr);
      if ((!FixedFunctionPipeline::supports(ff_fvf) && !packed_blend_fvf) ||
          stride < (packed_blend_fvf ? 16u : FixedFunctionPipeline::stride(ff_fvf)))
        return D3DERR_INVALIDCALL;
      packed_fvf = packed;
    } else if (packed != ff_fvf) {
      // a game pixel shader may draw a wider stride than the FVF names (Wine's coissue tests pass 24 bytes for
      // XYZ|TEX1); the named elements are still at their FVF offsets, so expand the no-diffuse format at that stride
      if (!FixedFunctionPipeline::supports(ff_fvf) || stride < FixedFunctionPipeline::stride(ff_fvf))
        return D3DERR_INVALIDCALL;
      packed_fvf = packed;
    }
  } else if (!declaration_inputs_ && !fvf_) {
    return D3DERR_INVALIDCALL;
  }
  if (!vertex_count)
    return D3DERR_INVALIDCALL;
  // The first vertex of the array is at MinVertexIndex and the indices are absolute into that array, so only the
  // used range is uploaded and the draw's base vertex is -MinVertexIndex (Wine's d3d9 does the same). That keeps a
  // large minimum index from reading bytes below the vertices the caller named.
  if (min_vertex_idx > (UINT)INT_MAX)
    return D3DERR_INVALIDCALL;
  const bool expand_format = packed_fvf != ff_fvf;
  const UINT bind_stride = expand_format ? FixedFunctionPipeline::stride(packed_fvf) : stride;
  const uint64_t vertex_offset = (uint64_t)min_vertex_idx * stride;
  const uint64_t vertex_bytes = (uint64_t)vertex_count * bind_stride;
  const UINT index_size = index_format == D3DFMT_INDEX32 ? 4 : 2;
  const uint64_t index_bytes = (uint64_t)count * index_size;
  if (vertex_offset > SIZE_MAX || vertex_bytes > UINT_MAX || index_bytes > UINT_MAX)
    return D3DERR_INVALIDCALL;

  if (!up_scratch_ || up_scratch_size_ < vertex_bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(vertex_bytes, 1024);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    up_scratch_ = scratch;
    up_scratch_size_ = desc.ByteWidth;
  }
  if (!up_index_scratch_ || up_index_scratch_size_ < index_bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)std::max<uint64_t>(index_bytes, 256);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> scratch;
    if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, &scratch)))
      return D3DERR_INVALIDCALL;
    up_index_scratch_ = scratch;
    up_index_scratch_size_ = desc.ByteWidth;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(up_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  if (expand_format) {
    const HRESULT packed = pack_up_vertices(
        (const char *)data + (size_t)vertex_offset, stride, vertex_count, ff_fvf, packed_fvf, mapped.pData
    );
    if (FAILED(packed)) {
      context_->Unmap(up_scratch_.Get(), 0);
      return packed;
    }
  } else {
    memcpy(mapped.pData, (const char *)data + (size_t)vertex_offset, (size_t)((uint64_t)vertex_count * stride));
  }
  context_->Unmap(up_scratch_.Get(), 0);
  if (FAILED(context_->Map(up_index_scratch_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  memcpy(mapped.pData, index_data, (size_t)index_bytes);
  context_->Unmap(up_index_scratch_.Get(), 0);

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = up_scratch_.Get();
  UINT offset = 0;
  context_->IASetVertexBuffers(0, 1, &buffer, &bind_stride, &offset);
  context_->IASetIndexBuffer(
      up_index_scratch_.Get(), index_format == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0
  );
  if (state_.render().point_fill()) {
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context_->DrawIndexed(count, 0, -(INT)min_vertex_idx);
  } else if (type == D3DPT_TRIANGLEFAN) {
    // the array's indices, expanded, with the base vertex the array's first index names
    if (FAILED(hr = bind_fan_triangle_list(index_data, index_size, 0, primitive_count))) {
      unbind_stream_source();
      unbind_indices();
      return hr;
    }
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->DrawIndexed(primitive_count * 3, 0, -(INT)min_vertex_idx);
  } else {
    context_->IASetPrimitiveTopology(topology_of(type));
    context_->DrawIndexed(count, 0, -(INT)min_vertex_idx);
  }
  note_gpu_draw();
  // Following any DrawIndexedPrimitiveUP call, stream 0 and the index buffer are NULL (Microsoft Learn)
  unbind_stream_source();
  unbind_indices();
  return S_OK;
}

namespace {

// where ProcessVertices writes a screen-space vertex: the byte offsets of its position (four floats, x y z and rhw),
// its diffuse colour and its first texture coordinate set with that set's float count, and the vertex's size. an
// offset of UINT_MAX is an element the destination does not carry
struct ProcessedLayout {
  UINT position = UINT_MAX;
  UINT colour = UINT_MAX;
  UINT texcoord = UINT_MAX;
  UINT texcoord_floats = 0;
  UINT stride = 0;
};

// the bytes a declaration type takes, for the types a processed vertex's elements may be
UINT
declaration_type_bytes(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
  case D3DDECLTYPE_D3DCOLOR:
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
  case D3DDECLTYPE_FLOAT16_2:
    return 4;
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
  case D3DDECLTYPE_FLOAT16_4:
    return 8;
  case D3DDECLTYPE_FLOAT3:
    return 12;
  case D3DDECLTYPE_FLOAT4:
    return 16;
  default:
    return 0;
  }
}

// a destination FVF's layout: D3DFVF_XYZRHW, then the point size, the two colours and the texture coordinate sets
// with the sizes D3DFVF_TEXCOORDSIZEn gives them, in the order D3D9 packs an FVF's elements
bool
layout_of_fvf(DWORD fvf, ProcessedLayout &out) {
  if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || (fvf & D3DFVF_NORMAL))
    return false;
  UINT offset = 0;
  out.position = offset;
  offset += 16;
  if (fvf & D3DFVF_PSIZE)
    offset += 4;
  if (fvf & D3DFVF_DIFFUSE) {
    out.colour = offset;
    offset += 4;
  }
  if (fvf & D3DFVF_SPECULAR)
    offset += 4;
  const UINT sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (UINT set = 0; set < sets; set++) {
    static const UINT kFloats[4] = {2, 3, 4, 1}; // D3DFVF_TEXTUREFORMAT2, 3, 4 and 1
    const UINT floats = kFloats[(fvf >> (16 + set * 2)) & 3];
    if (set == 0) {
      out.texcoord = offset;
      out.texcoord_floats = floats;
    }
    offset += floats * 4;
  }
  out.stride = offset;
  return true;
}

// a destination declaration's layout: stream 0's D3DDECLUSAGE_POSITIONT as four floats, COLOR0 as a D3DCOLOR and
// TEXCOORD0 as one to four floats, and the vertex as long as the last of its elements reaches
bool
layout_of_declaration(const std::vector<D3DVERTEXELEMENT9> &elements, ProcessedLayout &out) {
  for (const D3DVERTEXELEMENT9 &element : elements) {
    if (element.Stream == 0xFF)
      break;
    if (element.Stream != 0)
      continue;
    const UINT bytes = declaration_type_bytes(element.Type);
    out.stride = std::max<UINT>(out.stride, element.Offset + bytes);
    if (element.Usage == D3DDECLUSAGE_POSITIONT && element.UsageIndex == 0 && element.Type == D3DDECLTYPE_FLOAT4)
      out.position = element.Offset;
    else if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex == 0 && element.Type == D3DDECLTYPE_D3DCOLOR)
      out.colour = element.Offset;
    else if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex == 0 && element.Type >= D3DDECLTYPE_FLOAT1 &&
             element.Type <= D3DDECLTYPE_FLOAT4) {
      out.texcoord = element.Offset;
      out.texcoord_floats = (UINT)element.Type - D3DDECLTYPE_FLOAT1 + 1;
    }
  }
  return out.position != UINT_MAX && out.stride;
}

float
unit(D3DCOLOR colour, unsigned shift) {
  return (float)((colour >> shift) & 0xFF) / 255.0f;
}

D3DCOLOR
pack_colour(const float rgba[4]) {
  auto byte = [](float value) -> DWORD {
    const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    return (DWORD)(clamped * 255.0f + 0.5f);
  };
  return D3DCOLOR_ARGB(byte(rgba[3]), byte(rgba[0]), byte(rgba[1]), byte(rgba[2]));
}

} // namespace

// IDirect3DDevice9::ProcessVertices: the vertices stream 0 holds from `source_start`, in the FVF the device draws with,
// put through the fixed-function transform and lighting a draw would put them through, and written as screen-space
// vertices into `destination` from `destination_index` (Microsoft Learn, IDirect3DDevice9::ProcessVertices). A vertex
// is multiplied by World * View * Projection, divided by its w and mapped through the viewport, x and y into its
// rectangle, z into MinZ..MaxZ, and its rhw is 1 / w; a lit format's colour is the lighting's (d3d9_lights.cpp), and
// another format's is its own. The destination's layout is the declaration the call names, or the FVF the buffer was
// made with, and it carries D3DFVF_XYZRHW's position. D3DPV_DONOTCOPYDATA writes only what the processing made: the
// position, and the colour of a lit vertex.
HRESULT
MTLD3D9Device::ProcessVertices(UINT source_start, UINT destination_index, UINT vertex_count,
                               IDirect3DVertexBuffer9 *destination, IDirect3DVertexDeclaration9 *destination_declaration,
                               DWORD flags) {
  if (!destination || !vertex_count)
    return D3DERR_INVALIDCALL;
  if (vertex_shader_) {
    ERR_ONCE("D3D9: ProcessVertices runs the fixed-function vertex pipeline; this call was made with a vertex shader "
             "bound.");
    return D3DERR_INVALIDCALL;
  }
  if (!FixedFunctionPipeline::transformed(fvf_)) {
    ERR_ONCE("D3D9: ProcessVertices transforms the formats the fixed-function path transforms (D3DFVF_XYZ with a "
             "diffuse colour, a normal and one texture coordinate set); this one is FVF 0x", std::hex, fvf_, ".");
    return D3DERR_INVALIDCALL;
  }
  const StreamSlot &stream = streams_[0];
  if (!stream.source || !stream.stride)
    return D3DERR_INVALIDCALL;

  ProcessedLayout layout{};
  if (destination_declaration) {
    ComPtr<IMTLD3D9VertexDeclaration> declaration;
    if (FAILED(destination_declaration->QueryInterface(
            IID_IMTLD3D9VertexDeclaration, reinterpret_cast<void **>(declaration.GetAddressOf())
        )) ||
        !layout_of_declaration(declaration->elements(), layout)) {
      ERR_ONCE("D3D9: ProcessVertices writes a declaration whose stream 0 has a D3DDECLUSAGE_POSITIONT of four floats.");
      return D3DERR_INVALIDCALL;
    }
  } else {
    D3DVERTEXBUFFER_DESC desc{};
    if (FAILED(destination->GetDesc(&desc)) || !layout_of_fvf(desc.FVF, layout)) {
      ERR_ONCE("D3D9: ProcessVertices writes a buffer made with an FVF of D3DFVF_XYZRHW, or the declaration it is given.");
      return D3DERR_INVALIDCALL;
    }
  }

  // the source vertices, read once: the source and the destination may be one buffer, which takes one lock at a time
  const UINT source_stride = stream.stride;
  const uint64_t source_first = (uint64_t)stream.offset + (uint64_t)source_start * source_stride;
  const uint64_t source_bytes = (uint64_t)vertex_count * source_stride;
  if (source_first + source_bytes > stream.buffer->size())
    return D3DERR_INVALIDCALL;
  std::vector<char> source((size_t)source_bytes);
  {
    void *bytes = nullptr;
    if (FAILED(stream.source->Lock((UINT)source_first, (UINT)source_bytes, &bytes, D3DLOCK_READONLY)) || !bytes)
      return D3DERR_INVALIDCALL;
    memcpy(source.data(), bytes, source.size());
    stream.source->Unlock();
  }

  ComPtr<IMTLD3D9Buffer> target;
  if (FAILED(destination->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(target.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  const uint64_t destination_first = (uint64_t)destination_index * layout.stride;
  const uint64_t destination_bytes = (uint64_t)vertex_count * layout.stride;
  if (destination_first + destination_bytes > target->size())
    return D3DERR_INVALIDCALL;

  const bool lit_format = FixedFunctionPipeline::lit(fvf_);
  // a format that can be lit is still the vertex's own colour while D3DRS_LIGHTING is off
  const bool lit = lit_format && state_.render().lighting_enabled();
  const bool textured = FixedFunctionPipeline::textured(fvf_);
  // the source's elements as the fixed-function path's formats place them (d3d9_fixed_function.hpp): a normal follows
  // the position and a colour follows the normal, but a lit format without D3DFVF_NORMAL has no normal and puts its
  // colour at 12, and a format without D3DFVF_DIFFUSE has no colour to read (D3D9's default is opaque white)
  const bool has_normal = (fvf_ & D3DFVF_NORMAL) != 0;
  const bool has_colour = (fvf_ & D3DFVF_DIFFUSE) != 0;
  const UINT normal_offset = 12;
  const UINT colour_offset = has_normal ? 24 : 12;
  const UINT texcoord_offset = (has_normal ? 24 : 12) + (has_colour ? 4 : 0);
  const bool copy_data = !(flags & D3DPV_DONOTCOPYDATA);
  const bool write_colour = layout.colour != UINT_MAX && (copy_data || lit);
  const bool write_texcoord = layout.texcoord != UINT_MAX && textured && copy_data;

  const float *matrix = state_.world_view_projection();
  LightConstants lighting{};
  if (lit)
    state_.lighting_constants(lighting);

  void *bytes = nullptr;
  if (FAILED(destination->Lock((UINT)destination_first, (UINT)destination_bytes, &bytes, 0)) || !bytes)
    return D3DERR_INVALIDCALL;
  for (UINT at = 0; at < vertex_count; at++) {
    const char *in = source.data() + (size_t)at * source_stride;
    char *out = static_cast<char *>(bytes) + (size_t)at * layout.stride;

    float position[3];
    memcpy(position, in, sizeof(position));
    float clip[4];
    for (int column = 0; column < 4; column++)
      clip[column] = position[0] * matrix[column] + position[1] * matrix[4 + column] +
                     position[2] * matrix[8 + column] + matrix[12 + column];
    const float rhw = clip[3] != 0.0f ? 1.0f / clip[3] : 0.0f;
    const float screen[4] = {
        (float)viewport_.X + (1.0f + clip[0] * rhw) * 0.5f * (float)viewport_.Width,
        (float)viewport_.Y + (1.0f - clip[1] * rhw) * 0.5f * (float)viewport_.Height,
        viewport_.MinZ + clip[2] * rhw * (viewport_.MaxZ - viewport_.MinZ),
        rhw,
    };
    memcpy(out + layout.position, screen, sizeof(screen));

    if (write_colour) {
      D3DCOLOR colour = has_colour ? 0 : 0xFFFFFFFF;
      if (has_colour)
        memcpy(&colour, in + colour_offset, sizeof(colour));
      if (lit) {
        float normal[3] = {0.0f, 0.0f, 0.0f};
        if (has_normal)
          memcpy(normal, in + normal_offset, sizeof(normal));
        const float own[4] = {unit(colour, 16), unit(colour, 8), unit(colour, 0), unit(colour, 24)};
        float shaded[4];
        light_vertex(lighting, position, normal, own, shaded);
        colour = pack_colour(shaded);
      }
      memcpy(out + layout.colour, &colour, sizeof(colour));
    }
    if (write_texcoord) {
      float uv[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      memcpy(uv, in + texcoord_offset, 2 * sizeof(float));
      memcpy(out + layout.texcoord, uv, layout.texcoord_floats * sizeof(float));
    }
  }
  destination->Unlock();
  return S_OK;
}

} // namespace mullion::d3d9
