// the geometry a Direct3D 9 draw reads, over the D3D11 device's buffers and draw path: the vertex and index buffers,
// the one stream source and the FVFs this build transforms, and the draws that read them (the render states, targets
// and viewport they are drawn under are d3d9_device_targets.cpp's, and the texture stage they sample
// d3d9_device_texture.cpp's).
#include "d3d9_device.hpp"
#include <algorithm>

namespace mullion::d3d9 {

// the vertices (or indices, which are one per vertex of a primitive) a primitive count is made of, for the topologies
// D3D11 has a primitive for. D3DPT_TRIANGLEFAN has none: this milestone refuses it rather than draws it wrong.
bool
MTLD3D9Device::topology_count(D3DPRIMITIVETYPE type, UINT primitive_count, UINT &count) {
  switch (type) {
  case D3DPT_POINTLIST:
    count = primitive_count;
    return true;
  case D3DPT_LINELIST:
    count = primitive_count * 2;
    return true;
  case D3DPT_LINESTRIP:
    count = primitive_count + 1;
    return true;
  case D3DPT_TRIANGLELIST:
    count = primitive_count * 3;
    return true;
  case D3DPT_TRIANGLESTRIP:
    count = primitive_count + 2;
    return true;
  default:
    return false;
  }
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

// a draw's target: the surface SetRenderTarget named (the back buffer until it names another), the depth-stencil
// surface a draw tests against, the render state, and the viewport, shaders and texture stage the fixed-function path
// binds
HRESULT
MTLD3D9Device::prepare_draw() {
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  ID3D11RenderTargetView *target = current_target_view_.Get();
  context_->OMSetRenderTargets(1, &target, depth_stencil_view_.Get());
  hr = render_state_.apply(d3d11_device_.Get(), context_.Get());
  if (FAILED(hr))
    return hr;
  bind_viewport_state();

  // a game's own shaders are the pipeline when either stage is one (d3d9_device_shader.cpp)
  if (vertex_shader_ || pixel_shader_)
    return prepare_shader_draw();

  if (!FixedFunctionPipeline::supports(fvf_))
    return D3DERR_INVALIDCALL;
  // what the texture stage does for this draw: a format with texture coordinates draws through it, and a stage whose
  // first colour argument is the texture is not processed at all while no texture is bound (Microsoft Learn, Texture
  // Blending), which is what the untextured shaders already draw -- the vertex colour, colour and alpha both. the
  // fixed-function blender has one stage, which is stage 0's
  FixedFunctionSampling sampling{};
  const FixedFunctionSampling *bound = nullptr;
  if (FixedFunctionPipeline::textured(fvf_) && (texture_sources_[0] || !texture_stage_.skipped_without_texture())) {
    sampling.texture = texture_sources_[0] ? texture_sources_[0]->view() : nullptr;
    sampling.sampler = sampler_states_[0].state(d3d11_device_.Get());
    if (!sampling.sampler)
      return D3DERR_INVALIDCALL;
    texture_stage_.constants(sampling.ops, sampling.args);
    // D3DRS_TEXTUREFACTOR, as the shader reads a colour
    const DWORD factor = render_state_.texture_factor();
    sampling.factor[0] = ((factor >> 16) & 0xFF) / 255.0f;
    sampling.factor[1] = ((factor >> 8) & 0xFF) / 255.0f;
    sampling.factor[2] = (factor & 0xFF) / 255.0f;
    sampling.factor[3] = ((factor >> 24) & 0xFF) / 255.0f;
    bound = &sampling;
  }
  return fixed_function_.bind(context_.Get(), viewport_, render_state_.flat_shaded(), bound);
}

HRESULT
MTLD3D9Device::CreateVertexBuffer(
    UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **vertex_buffer, HANDLE *shared_handle
) {
  if (!vertex_buffer || shared_handle || !length)
    return D3DERR_INVALIDCALL;
  *vertex_buffer = nullptr;
  // a managed buffer is the same buffer here: its bytes are the CPU's when it is locked and the device's otherwise,
  // which is what every buffer of this milestone is
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING | D3DUSAGE_NPATCHES))
    return D3DERR_INVALIDCALL;

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
  if (!index_buffer || shared_handle || !length)
    return D3DERR_INVALIDCALL;
  *index_buffer = nullptr;
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_SOFTWAREPROCESSING))
    return D3DERR_INVALIDCALL;
  const UINT index_size = format == D3DFMT_INDEX32 ? 4 : 2;
  if (length % index_size)
    return D3DERR_INVALIDCALL;

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

// the one vertex format this milestone transforms; a failed SetFVF leaves the device's last format, as D3D9's does
HRESULT
MTLD3D9Device::SetFVF(DWORD fvf) {
  if (!fvf) {
    fvf_ = 0;
    return S_OK;
  }
  if (!FixedFunctionPipeline::supports(fvf)) {
    ERR_ONCE("D3D9: only D3DFVF_XYZRHW | D3DFVF_DIFFUSE, with or without one set of texture coordinates "
             "(D3DFVF_TEX1), is transformed yet.");
    return D3DERR_INVALIDCALL;
  }
  fvf_ = fvf;
  // an FVF and a declaration are exclusive, as D3D9's are: the last one set is the draw's
  declaration_.Reset();
  declaration_inputs_.Reset();
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
  if (stream != 0)
    return D3DERR_INVALIDCALL;
  if (!buffer) {
    if (offset || stride)
      return D3DERR_INVALIDCALL;
    stream_source_.Reset();
    stream_buffer_.Reset();
    stream_offset_ = 0;
    stream_stride_ = 0;
    if (recording_)
      record(StateKey{StateKey::Stream, stream, 0});
    return S_OK;
  }

  ComPtr<IMTLD3D9Buffer> ours;
  if (FAILED(buffer->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(ours.GetAddressOf()))) || !stride ||
      offset > ours->size())
    return D3DERR_INVALIDCALL;
  stream_source_ = buffer;
  stream_buffer_ = ours;
  stream_offset_ = offset;
  stream_stride_ = stride;
  if (recording_)
    record(StateKey{StateKey::Stream, stream, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) {
  if (stream != 0 || !buffer || !offset || !stride)
    return D3DERR_INVALIDCALL;
  *buffer = stream_source_.Get();
  if (*buffer)
    (*buffer)->AddRef();
  *offset = stream_offset_;
  *stride = stream_stride_;
  return S_OK;
}

// instancing is not drawn yet: the setting of a stream that does not instance is the one thing this accepts
HRESULT
MTLD3D9Device::SetStreamSourceFreq(UINT stream, UINT setting) {
  return stream == 0 && setting == 1 ? S_OK : D3DERR_INVALIDCALL;
}

HRESULT
MTLD3D9Device::GetStreamSourceFreq(UINT stream, UINT *setting) {
  if (stream != 0 || !setting)
    return D3DERR_INVALIDCALL;
  *setting = 1;
  return S_OK;
}

HRESULT
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *indices) {
  if (!indices) {
    indices_.Reset();
    index_buffer_.Reset();
    if (recording_)
      record(StateKey{StateKey::Indices, 0, 0});
    return S_OK;
  }
  ComPtr<IMTLD3D9Buffer> ours;
  if (FAILED(indices->QueryInterface(IID_IMTLD3D9Buffer, reinterpret_cast<void **>(ours.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  indices_ = indices;
  index_buffer_ = ours;
  if (recording_)
    record(StateKey{StateKey::Indices, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **indices) {
  if (!indices)
    return D3DERR_INVALIDCALL;
  *indices = indices_.Get();
  if (*indices)
    (*indices)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count) {
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  // a fixed-function draw's stream is the FVF's size; a shader draw reads the stride the stream was set with
  if (!stream_buffer_ || (!vertex_shader_ && !pixel_shader_ && stream_stride_ != FixedFunctionPipeline::stride(fvf_)))
    return D3DERR_INVALIDCALL;
  // the vertices the draw reads are the stream's, from the offset it was set with
  const uint64_t vertices = (uint64_t)start_vertex + count;
  if (stream_offset_ + vertices * stream_stride_ > stream_buffer_->size())
    return D3DERR_INVALIDCALL;

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = stream_buffer_->buffer();
  context_->IASetVertexBuffers(0, 1, &buffer, &stream_stride_, &stream_offset_);
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->Draw(count, start_vertex);
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE type, INT base_vertex, UINT min_vertex, UINT vertex_count, UINT start_index, UINT primitive_count
) {
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!stream_buffer_ || (!vertex_shader_ && !pixel_shader_ && stream_stride_ != FixedFunctionPipeline::stride(fvf_)) ||
      !index_buffer_ || !vertex_count || min_vertex >= vertex_count)
    return D3DERR_INVALIDCALL;

  const UINT index_size = index_buffer_->format() == D3DFMT_INDEX32 ? 4 : 2;
  if ((uint64_t)(start_index + count) * index_size > index_buffer_->size())
    return D3DERR_INVALIDCALL;
  const int64_t first_vertex = (int64_t)base_vertex + min_vertex;
  const uint64_t stream_vertices = (stream_buffer_->size() - stream_offset_) / stream_stride_;
  if (first_vertex < 0 || (uint64_t)first_vertex + vertex_count > stream_vertices)
    return D3DERR_INVALIDCALL;

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = stream_buffer_->buffer();
  context_->IASetVertexBuffers(0, 1, &buffer, &stream_stride_, &stream_offset_);
  context_->IASetIndexBuffer(
      index_buffer_->buffer(),
      index_buffer_->format() == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0
  );
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->DrawIndexed(count, start_index, base_vertex);
  return S_OK;
}

HRESULT
MTLD3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertices, UINT stride
) {
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!vertices || !stride)
    return D3DERR_INVALIDCALL;
  if (!vertex_shader_ && !pixel_shader_ &&
      (!FixedFunctionPipeline::supports(fvf_) || stride != FixedFunctionPipeline::stride(fvf_)))
    return D3DERR_INVALIDCALL;
  if ((vertex_shader_ || pixel_shader_) && (!vertex_shader_ || !pixel_shader_ || !declaration_inputs_))
    return D3DERR_INVALIDCALL;

  const uint64_t bytes = (uint64_t)count * stride;
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
  memcpy(mapped.pData, vertices, (size_t)bytes);
  context_->Unmap(up_scratch_.Get(), 0);

  HRESULT hr = prepare_draw();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *buffer = up_scratch_.Get();
  UINT offset = 0;
  context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->Draw(count, 0);
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
  UINT count = 0;
  if (!topology_count(type, primitive_count, count))
    return D3DERR_INVALIDCALL;
  if (!primitive_count)
    return S_OK;
  if (!index_data || !data || !stride)
    return D3DERR_INVALIDCALL;
  if (index_format != D3DFMT_INDEX16 && index_format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  if (!vertex_shader_ && !pixel_shader_ &&
      (!FixedFunctionPipeline::supports(fvf_) || stride != FixedFunctionPipeline::stride(fvf_)))
    return D3DERR_INVALIDCALL;
  if ((vertex_shader_ || pixel_shader_) && (!vertex_shader_ || !pixel_shader_ || !declaration_inputs_))
    return D3DERR_INVALIDCALL;
  // the vertices the indices can name: D3D9 asks for at least MinVertexIndex + NumVertices of them
  const uint64_t vertices = (uint64_t)min_vertex_idx + vertex_count;
  if (!vertex_count || !vertices)
    return D3DERR_INVALIDCALL;

  const UINT index_size = index_format == D3DFMT_INDEX32 ? 4 : 2;
  const uint64_t vertex_bytes = vertices * stride;
  const uint64_t index_bytes = (uint64_t)count * index_size;
  if (vertex_bytes > UINT_MAX || index_bytes > UINT_MAX)
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
  memcpy(mapped.pData, data, (size_t)vertex_bytes);
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
  context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
  context_->IASetIndexBuffer(
      up_index_scratch_.Get(), index_format == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0
  );
  context_->IASetPrimitiveTopology(topology_of(type));
  context_->DrawIndexed(count, 0, 0);
  return S_OK;
}

} // namespace mullion::d3d9
