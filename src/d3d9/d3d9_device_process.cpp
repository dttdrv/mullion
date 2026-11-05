/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d9_device.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mullion {

namespace {

struct Vec4 {
  float v[4] = {0, 0, 0, 0};

  float &
  operator[](int i) {
    return v[i];
  }
  float
  operator[](int i) const {
    return v[i];
  }
};

float
Dot3(const Vec4 &a, const Vec4 &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec4
Normalized(Vec4 a) {
  float length = std::sqrt(Dot3(a, a));
  if (length > 0)
    for (int i = 0; i < 3; i++)
      a[i] /= length;
  return a;
}

Vec4
Mul(const Vec4 &a, const D3DMATRIX &m, bool point) {
  Vec4 r;
  for (int j = 0; j < 4; j++)
    r[j] = a[0] * m.m[0][j] + a[1] * m.m[1][j] + a[2] * m.m[2][j] + (point ? a[3] : 0.0f) * m.m[3][j];
  return r;
}

Vec4
FromColor(D3DCOLOR c) {
  return {{((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f}};
}

Vec4
FromColor(const D3DCOLORVALUE &c) {
  return {{c.r, c.g, c.b, c.a}};
}

D3DCOLOR
ToColor(const Vec4 &c) {
  auto byte = [](float f) { return (DWORD)std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f); };
  return byte(c[3]) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
}

bool
ReadElement(const BYTE *p, BYTE Type, Vec4 &Out) {
  Out = {{0, 0, 0, 1}};
  switch (Type) {
  case D3DDECLTYPE_FLOAT1:
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_FLOAT3:
  case D3DDECLTYPE_FLOAT4:
    std::memcpy(Out.v, p, (Type - D3DDECLTYPE_FLOAT1 + 1) * sizeof(float));
    return true;
  case D3DDECLTYPE_D3DCOLOR:
    Out = FromColor(*(const D3DCOLOR *)p);
    return true;
  case D3DDECLTYPE_UBYTE4:
    for (int i = 0; i < 4; i++)
      Out[i] = p[i];
    return true;
  default:
    return false;
  }
}

void
WriteElement(BYTE *p, BYTE Type, const Vec4 &Value) {
  switch (Type) {
  case D3DDECLTYPE_FLOAT1:
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_FLOAT3:
  case D3DDECLTYPE_FLOAT4:
    std::memcpy(p, Value.v, (Type - D3DDECLTYPE_FLOAT1 + 1) * sizeof(float));
    break;
  case D3DDECLTYPE_D3DCOLOR:
    *(D3DCOLOR *)p = ToColor(Value);
    break;
  default:
    WARN_ONCE("D3D9: ProcessVertices can't write declaration type ", (uint32_t)Type);
    break;
  }
}

UINT
ElementSize(BYTE Type) {
  switch (Type) {
  case D3DDECLTYPE_FLOAT1:
  case D3DDECLTYPE_D3DCOLOR:
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_UBYTE4N:
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
  default:
    return 16;
  }
}

struct Vertex {
  Vec4 position, normal, diffuse = {{1, 1, 1, 1}}, specular;
  Vec4 texcoords[8];
  bool has_normal = false, has_diffuse = false, has_specular = false;
};

} // namespace

// software vertex processing for the fixed-function pipeline: the same transform and lighting the generated
// vertex shader does, evaluated on the CPU into pretransformed vertices.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ProcessVertices(
    UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer,
    IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags
) {
  auto lock = Lock();
  if (!pDestBuffer || !vertex_declaration_)
    return D3DERR_INVALIDCALL;
  auto dst_buffer = static_cast<MTLD3D9VertexBuffer *>(pDestBuffer);
  D3DVERTEXBUFFER_DESC dst_desc;
  dst_buffer->GetDesc(&dst_desc);
  std::vector<D3DVERTEXELEMENT9> dst_elements;
  if (pVertexDecl)
    dst_elements = static_cast<MTLD3D9VertexDeclaration *>(pVertexDecl)->elements();
  else if (dst_desc.FVF)
    dst_elements = D3D9DeclarationFromFVF(dst_desc.FVF);
  UINT dst_stride = dst_desc.FVF ? D3D9FVFVertexSize(dst_desc.FVF) : 0;
  for (auto &e : dst_elements)
    if (e.Stream == 0)
      dst_stride = std::max(dst_stride, (UINT)e.Offset + ElementSize(e.Type));
  bool has_positiont = std::any_of(dst_elements.begin(), dst_elements.end(), [](auto &e) {
    return e.Usage == D3DDECLUSAGE_POSITIONT;
  });
  if (!has_positiont || !dst_stride || (DestIndex + VertexCount) * dst_stride > dst_desc.Size)
    return D3DERR_INVALIDCALL;

  if (vertex_shader_)
    return ProcessVerticesWithShader(SrcStartIndex, DestIndex, VertexCount, dst_buffer, dst_elements, dst_stride, Flags);

  std::array<const BYTE *, caps::MaxStreams> sources = {};
  std::vector<MTLD3D9VertexBuffer *> locked;
  for (auto &e : vertex_declaration_->elements()) {
    if (sources[e.Stream])
      continue;
    auto *vb = streams_[e.Stream].buffer.ptr();
    void *data = nullptr;
    if (!vb || FAILED(vb->storage().Lock(0, 0, &data, D3DLOCK_READONLY)))
      continue;
    locked.push_back(vb);
    sources[e.Stream] = (const BYTE *)data + streams_[e.Stream].offset;
  }
  BYTE *dst = nullptr;
  if (FAILED(dst_buffer->storage().Lock(DestIndex * dst_stride, VertexCount * dst_stride, (void **)&dst, 0))) {
    for (auto *vb : locked)
      vb->storage().Unlock();
    return D3DERR_INVALIDCALL;
  }

  auto &world = transforms_[D3DTS_WORLDMATRIX(0)];
  auto &view = transforms_[D3DTS_VIEW];
  D3DMATRIX world_view, wvp;
  auto multiply = [](const D3DMATRIX &a, const D3DMATRIX &b) {
    D3DMATRIX r;
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
  };
  world_view = multiply(world, view);
  wvp = multiply(world_view, transforms_[D3DTS_PROJECTION]);
  bool lighting = render_states_[D3DRS_LIGHTING];
  bool specular_lighting = render_states_[D3DRS_SPECULARENABLE];
  bool local_viewer = render_states_[D3DRS_LOCALVIEWER];
  bool color_vertex = render_states_[D3DRS_COLORVERTEX];

  for (UINT n = 0; n < VertexCount; n++) {
    Vertex v;
    for (auto &e : vertex_declaration_->elements()) {
      if (!sources[e.Stream])
        continue;
      const BYTE *p = sources[e.Stream] + (SrcStartIndex + n) * streams_[e.Stream].stride + e.Offset;
      Vec4 value;
      if (!ReadElement(p, e.Type, value))
        continue;
      if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0)
        v.position = value;
      else if (e.Usage == D3DDECLUSAGE_NORMAL && e.UsageIndex == 0)
        v.normal = value, v.has_normal = true;
      else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 0)
        v.diffuse = value, v.has_diffuse = true;
      else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 1)
        v.specular = value, v.has_specular = true;
      else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex < 8)
        v.texcoords[e.UsageIndex] = value;
    }

    Vec4 clip = Mul(v.position, wvp, true);
    float rhw = clip[3] != 0 ? 1.0f / clip[3] : 1.0f;
    Vec4 screen = {{
        viewport_.X + (clip[0] * rhw + 1) * viewport_.Width / 2,
        viewport_.Y + (1 - clip[1] * rhw) * viewport_.Height / 2,
        viewport_.MinZ + clip[2] * rhw * (viewport_.MaxZ - viewport_.MinZ),
        rhw,
    }};

    Vec4 diffuse = v.diffuse, specular = v.has_specular ? v.specular : Vec4{};
    if (lighting) {
      auto source = [&](D3DRENDERSTATETYPE State, const D3DCOLORVALUE &Material) {
        auto value = render_states_[State];
        if (color_vertex && value == D3DMCS_COLOR1 && v.has_diffuse)
          return v.diffuse;
        if (color_vertex && value == D3DMCS_COLOR2 && v.has_specular)
          return v.specular;
        return FromColor(Material);
      };
      Vec4 md = source(D3DRS_DIFFUSEMATERIALSOURCE, material_.Diffuse);
      Vec4 ma = source(D3DRS_AMBIENTMATERIALSOURCE, material_.Ambient);
      Vec4 ms = source(D3DRS_SPECULARMATERIALSOURCE, material_.Specular);
      Vec4 me = source(D3DRS_EMISSIVEMATERIALSOURCE, material_.Emissive);
      Vec4 p = Mul(v.position, world_view, true);
      Vec4 normal = Normalized(Mul(v.normal, world_view, false));
      Vec4 eye = local_viewer ? Normalized({{-p[0], -p[1], -p[2], 0}}) : Vec4{{0, 0, -1, 0}};
      Vec4 amb = FromColor((D3DCOLOR)render_states_[D3DRS_AMBIENT]), diff, spec;
      uint32_t count = 0;
      for (auto &[light, enabled] : lights_) {
        if (!enabled || count++ == 8)
          continue;
        Vec4 l;
        float att = 1;
        if (light.Type == D3DLIGHT_DIRECTIONAL) {
          l = Normalized(Mul({{-light.Direction.x, -light.Direction.y, -light.Direction.z, 0}}, view, false));
        } else {
          Vec4 position = Mul({{light.Position.x, light.Position.y, light.Position.z, 1}}, view, true);
          l = {{position[0] - p[0], position[1] - p[1], position[2] - p[2], 0}};
          float d = std::sqrt(Dot3(l, l));
          l = Normalized(l);
          float denominator = light.Attenuation0 + light.Attenuation1 * d + light.Attenuation2 * d * d;
          att = d <= light.Range && denominator > 0 ? 1.0f / denominator : 0.0f;
          if (light.Type == D3DLIGHT_SPOT) {
            Vec4 dir = Normalized(Mul({{light.Direction.x, light.Direction.y, light.Direction.z, 0}}, view, false));
            float rho = -Dot3(l, dir), inner = std::cos(light.Theta / 2), outer = std::cos(light.Phi / 2);
            float t = inner > outer ? std::clamp((rho - outer) / (inner - outer), 0.0f, 1.0f) : (rho > outer);
            att *= t > 0 ? std::pow(t, light.Falloff) : 0.0f;
          }
        }
        Vec4 la = FromColor(light.Ambient), ld = FromColor(light.Diffuse), ls = FromColor(light.Specular);
        for (int i = 0; i < 3; i++)
          amb[i] += la[i] * att;
        if (!v.has_normal)
          continue;
        float ndotl = Dot3(normal, l);
        for (int i = 0; i < 3; i++)
          diff[i] += ld[i] * std::max(ndotl, 0.0f) * att;
        if (specular_lighting && ndotl > 0) {
          Vec4 h = Normalized({{l[0] + eye[0], l[1] + eye[1], l[2] + eye[2], 0}});
          float s = std::pow(std::max(Dot3(normal, h), 1e-6f), material_.Power) * att;
          for (int i = 0; i < 3; i++)
            spec[i] += ls[i] * s;
        }
      }
      for (int i = 0; i < 3; i++) {
        diffuse[i] = me[i] + ma[i] * amb[i] + md[i] * diff[i];
        specular[i] = specular_lighting ? ms[i] * spec[i] : 0.0f;
      }
      diffuse[3] = md[3];
      specular[3] = v.has_specular ? v.specular[3] : 0.0f;
    }

    BYTE *out = dst + n * dst_stride;
    for (auto &e : dst_elements) {
      if (e.Stream != 0)
        continue;
      if (e.Usage == D3DDECLUSAGE_POSITIONT)
        WriteElement(out + e.Offset, e.Type, screen);
      else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 0)
        WriteElement(out + e.Offset, e.Type, diffuse);
      else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 1)
        WriteElement(out + e.Offset, e.Type, specular);
      else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex < 8 && !(Flags & D3DPV_DONOTCOPYDATA))
        WriteElement(out + e.Offset, e.Type, v.texcoords[e.UsageIndex]);
    }
  }

  dst_buffer->storage().Unlock();
  for (auto *vb : locked)
    vb->storage().Unlock();
  return D3D_OK;
}

// the vertex shader runs on the GPU with its output registers streamed to a buffer; the CPU then applies the
// viewport transform and writes the destination declaration, as Windows' software vertex processing does.
HRESULT
MTLD3D9Device::ProcessVerticesWithShader(
    UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, MTLD3D9VertexBuffer *pDest,
    const std::vector<D3DVERTEXELEMENT9> &DstElements, UINT DstStride, DWORD Flags
) {
  auto &outputs = vertex_shader_->code()->outputRegisters();
  uint32_t registers = 0;
  for (auto &out : outputs)
    registers = std::max(registers, out.reg + 1);
  if (!registers || !VertexCount)
    return D3D_OK;

  UINT stride = registers * 16;
  // outputs land at vertex_id - base_vertex, which a non-indexed draw counts from its start vertex
  Rc<Buffer> buffer = new Buffer((uint64_t)VertexCount * stride, GetMTLDevice());
  mullion::Flags<BufferAllocationFlag> flags;
#ifdef __i386__
  // a 32-bit process can only read memory placed in its address space
  flags.set(BufferAllocationFlag::CpuPlaced);
#endif
  auto allocation = buffer->allocate(flags);
  auto _ = buffer->rename(std::move(allocation));
  so_target_ = buffer;
  so_registers_ = registers;
  HRESULT hr = DrawPrimitive(D3DPT_POINTLIST, SrcStartIndex, VertexCount);
  so_target_ = nullptr;
  so_registers_ = 0;
  Emit([](ArgumentEncodingContext &enc) { enc.bindStreamOutputBuffer(0, {}, {}); });
  if (FAILED(hr))
    return hr;
  EndRenderPass();
  WaitUntilGPUIdle();

  auto find = [&](d3d9bc::Usage usage, uint32_t index) {
    for (auto &out : outputs)
      if (out.usage == usage && out.usage_index == index)
        return (int)out.reg;
    return -1;
  };
  BYTE *dst = nullptr;
  if (FAILED(pDest->storage().Lock(DestIndex * DstStride, VertexCount * DstStride, (void **)&dst, 0)))
    return D3DERR_INVALIDCALL;
  auto base = (const float *)buffer->current()->mappedMemory(0);
  auto read = [&](int reg, const float *vertex, Vec4 &out) {
    if (reg < 0)
      return false;
    std::memcpy(out.v, vertex + reg * 4, sizeof(out.v));
    return true;
  };
  int pos = find(d3d9bc::Usage::Position, 0);
  for (UINT n = 0; n < VertexCount; n++) {
    const float *vertex = base + (size_t)n * registers * 4;
    Vec4 clip, value;
    read(pos, vertex, clip);
    float rhw = clip[3] != 0 ? 1.0f / clip[3] : 1.0f;
    Vec4 screen = {{
        viewport_.X + (clip[0] * rhw + 1) * viewport_.Width / 2,
        viewport_.Y + (1 - clip[1] * rhw) * viewport_.Height / 2,
        viewport_.MinZ + clip[2] * rhw * (viewport_.MaxZ - viewport_.MinZ),
        rhw,
    }};
    BYTE *out = dst + n * DstStride;
    for (auto &e : DstElements) {
      if (e.Stream != 0)
        continue;
      if (e.Usage == D3DDECLUSAGE_POSITIONT) {
        WriteElement(out + e.Offset, e.Type, screen);
      } else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex < 2) {
        if (read(find(d3d9bc::Usage::Color, e.UsageIndex), vertex, value))
          WriteElement(out + e.Offset, e.Type, value);
      } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex < 8 && !(Flags & D3DPV_DONOTCOPYDATA)) {
        if (read(find(d3d9bc::Usage::TexCoord, e.UsageIndex), vertex, value))
          WriteElement(out + e.Offset, e.Type, value);
      } else if (e.Usage == D3DDECLUSAGE_PSIZE) {
        if (read(find(d3d9bc::Usage::PSize, 0), vertex, value))
          WriteElement(out + e.Offset, e.Type, value);
      }
    }
  }
  pDest->storage().Unlock();
  return D3D_OK;
}

} // namespace mullion
