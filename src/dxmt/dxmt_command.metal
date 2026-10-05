/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Paweł Kołodziejski
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

#include <metal_common>
#include <metal_texture>
#include <metal_math>
#include <metal_integer>
#include <metal_stdlib>
#include "../airconv/airconv_ray.h"
#include "dxmt_command_constants.hpp"

using namespace metal;
using namespace dxmt;

[[kernel]] void clear_texture_1d_uint(
    texture1d<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  if(width > pos) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_1d_array_uint(
    texture1d_array<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint array_len = tex.get_array_size();
  if(width > pos.x && array_len > pos.y) {
    tex.write(value, pos.x, pos.y);
  }
}

[[kernel]] void clear_texture_2d_uint(
    texture2d<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  if(width > pos.x && height > pos.y) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_2d_array_uint(
    texture2d_array<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint array_len = tex.get_array_size();
  if(width > pos.x && height > pos.y && array_len > pos.z) {
    tex.write(value, pos.xy, pos.z);
  }
}

[[kernel]] void clear_texture_3d_uint(
    texture3d<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint depth = tex.get_depth();
  if(width > pos.x && height > pos.y && depth > pos.z) {
    tex.write(value, pos);
  }
}

struct DXMTClearTextureBufferUInt {
  uint4 value;
  uint offset;
  uint size;
};

[[kernel]] void clear_texture_buffer_uint(
    texture_buffer<uint, access::read_write> tex [[texture(0)]],
    constant DXMTClearTextureBufferUInt& args [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint offset = pos + args.offset;
  if (args.size > pos && width > offset) {
    tex.write(args.value, offset);
  }
}

[[kernel]] void clear_buffer_uint(
    device uint* tex [[buffer(0)]],
    constant uint& size [[buffer(2)]],
    constant uint4& value [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  if(pos < size) {
    tex[pos] = value.x;
  }
}

[[kernel]] void clear_texture_1d_float(
    texture1d<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  if(width > pos) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_1d_array_float(
    texture1d_array<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint array_len = tex.get_array_size();
  if(width > pos.x && array_len > pos.y) {
    tex.write(value, pos.x, pos.y);
  }
}

[[kernel]] void clear_texture_2d_float(
    texture2d<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  if(width > pos.x && height > pos.y) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_2d_array_float(
    texture2d_array<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint array_len = tex.get_array_size();
  if(width > pos.x && height > pos.y && array_len > pos.z) {
    tex.write(value, pos.xy, pos.z);
  }
}

[[kernel]] void clear_texture_3d_float(
    texture3d<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint depth = tex.get_depth();
  if(width > pos.x && height > pos.y && depth > pos.z) {
    tex.write(value, pos);
  }
}

struct DXMTClearTextureBufferFloat {
  float4 value;
  uint offset;
  uint size;
};

[[kernel]] void clear_texture_buffer_float(
    texture_buffer<float, access::read_write> tex [[texture(0)]],
    constant DXMTClearTextureBufferFloat& args [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint offset = pos + args.offset;
  if (args.size > pos && width > offset) {
    tex.write(args.value, offset);
  }
}

struct clear_data {
  float4 position [[position]];
  uint array_index [[render_target_array_index]];
};

[[vertex]] clear_data vs_clear_rt(
  ushort id [[vertex_id]],
  ushort instance_id [[instance_id]]
) {
  clear_data output;
  float2 uv = float2((id << 1) & 2, id & 2);
  output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  output.array_index = instance_id;
  return output;
}

[[fragment]] float4 fs_clear_rt_float(
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return clear_value;
}

[[fragment]] uint4 fs_clear_rt_uint(
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return (uint4)clear_value;
}

[[fragment]] int4 fs_clear_rt_sint(
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return (int4)clear_value;
}

struct depth_out {
  float depth [[depth(any)]];
};

[[fragment]] depth_out fs_clear_rt_depth (
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return {clear_value.x};
}

// a resolve: each destination sample from its pixel's samples in a multisampled source, by a D3D12_RESOLVE_MODE
enum { resolve_decompress, resolve_min, resolve_max, resolve_average };

struct resolve_arguments {
  int2 offset; // of the source from the destination
  uint mode;
};

template <typename Texel, typename Source>
Texel resolve(Source src, float4 position, uint sample, constant resolve_arguments &args) {
  uint2 at = uint2(int2(position.xy) + args.offset);
  // nothing is compressed in Metal: a sample is its own value
  if (args.mode == resolve_decompress)
    return src.read(at, sample);
  Texel value = src.read(at, 0);
  for (uint i = 1; i < src.get_num_samples(); i++) {
    Texel next = src.read(at, i);
    value = args.mode == resolve_min ? min(value, next) : args.mode == resolve_max ? max(value, next) : value + next;
  }
  return args.mode == resolve_average ? value / Texel(src.get_num_samples()) : value;
}

[[fragment]] float4 fs_resolve_float(
  float4 position [[position]],
  uint sample [[sample_id]],
  texture2d_ms<float> src [[texture(0)]],
  constant resolve_arguments &args [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return resolve<float4>(src, position, sample, args);
}

[[fragment]] uint4 fs_resolve_uint(
  float4 position [[position]],
  uint sample [[sample_id]],
  texture2d_ms<uint> src [[texture(0)]],
  constant resolve_arguments &args [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return resolve<uint4>(src, position, sample, args);
}

[[fragment]] int4 fs_resolve_sint(
  float4 position [[position]],
  uint sample [[sample_id]],
  texture2d_ms<int> src [[texture(0)]],
  constant resolve_arguments &args [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return resolve<int4>(src, position, sample, args);
}

[[fragment]] depth_out fs_resolve_depth(
  float4 position [[position]],
  uint sample [[sample_id]],
  depth2d_ms<float> src [[texture(0)]],
  constant resolve_arguments &args [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return {resolve<float>(src, position, sample, args)};
}

struct stencil_out {
  uint stencil [[stencil]];
};

[[fragment]] stencil_out fs_resolve_stencil(
  float4 position [[position]],
  uint sample [[sample_id]],
  texture2d_ms<uint> src [[texture(0)]],
  constant resolve_arguments &args [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return {resolve<uint4>(src, position, sample, args).x};
}

struct present_data {
  float4 position [[position]];
  float2 uv [[user(coord)]];
};

[[vertex]] present_data vs_present_quad(ushort id [[vertex_id]]) {
  present_data output;
  float2 uv = float2((id << 1) & 2, id & 2);
	output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  output.uv = uv;
  return output;
}

constant constexpr float PQ_M1 = 0.1593017578125;
constant constexpr float PQ_M2 = 78.84375;
constant constexpr float PQ_C1 = 0.8359375;
constant constexpr float PQ_C2 = 18.8515625;
constant constexpr float PQ_C3 = 18.6875;

float linear_to_pq(float linear) {
  linear = linear / 10000;
  return pow((PQ_C1 + PQ_C2 * pow(abs(linear), PQ_M1)) / (1.0f + PQ_C3 * pow(abs(linear), PQ_M1)), PQ_M2);
}

float3 linear_to_pq(float3 linear) {
  linear = linear / 10000;
  return pow((PQ_C1 + PQ_C2 * pow(abs(linear), PQ_M1)) / (1.0f + PQ_C3 * pow(abs(linear), PQ_M1)), PQ_M2);
}

float pq_to_linear(float norm) {
  return 10000 * pow(abs(max(pow(norm, 1.0 / PQ_M2) -PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * pow(abs(norm), 1.0f / PQ_M2))), 1.0f / PQ_M1 );
}

float3 pq_to_linear(float3 norm) {
  return 10000 * pow(abs(max(pow(norm, 1.0 / PQ_M2) -PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * pow(abs(norm), 1.0f / PQ_M2))), 1.0f / PQ_M1 );
}

// See BT.2408 Annex 5
float EETF(float E, float Lw, float Lb, float Lmax, float Lmin) {
  float pq_Lb = linear_to_pq(Lb);
  float pqdiff_LwLb = linear_to_pq(Lw) - pq_Lb;
  float min_lum = (linear_to_pq(Lmin) - pq_Lb) / pqdiff_LwLb;
  float max_lum = (linear_to_pq(Lmax) - pq_Lb) / pqdiff_LwLb;
  E = (E - pq_Lb) / pqdiff_LwLb;
  float KS = 1.5 * max_lum - 0.5;
  float b = min_lum;
  if (E >= KS) {
    float T = (E - KS) /  (1 - KS);
    float T_2 = T * T;
    float T_3 = T_2 * T;
    E = (2 * T_3 - 3 * T_2 + 1) * KS 
        + (T_3 - 2 * T_2 + T) * (1 - KS)
        + (- 2 * T_3 + 3 * T_2) * max_lum;
  }
  E = E + b * pow(1 - E, 4);
  return E * pqdiff_LwLb + pq_Lb;
}

constant bool present_backbuffer_size_matched [[function_constant(kPresentFCIndex_BackbufferSizeMatched)]];
constant bool present_backbuffer_is_srgb [[function_constant(kPresentFCIndex_BackbufferIsSRGB)]];
constant bool present_hdr_pq [[function_constant(kPresentFCIndex_HDRPQ)]];
constant bool present_with_hdr_metadata [[function_constant(kPresentFCIndex_WithHDRMetadata)]];
constant bool present_backbuffer_is_ms [[function_constant(kPresentFCIndex_BackbufferIsMS)]];
constant bool present_backbuffer_is_not_ms = !present_backbuffer_is_ms;
constant bool present_gamma_enabled [[function_constant(kPresentFCIndex_GammaEnabled)]];

constexpr sampler s(coord::normalized);
constexpr sampler gamma_sampler(coord::normalized, filter::linear, address::clamp_to_edge);

struct DXMTPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
  // DXMTPresentation: the part of the back buffer that is presented, from its origin, as a fraction of each side;
  // the target's size over the picture's; the picture's quarter turns in the back buffer, as rows and what they
  // add; and what is around a picture smaller than the target
  float2 source;
  float2 scale;
  float4 turn;
  float2 shift;
  float4 background;
};

float3 to_srgb(float3 linear) {
  return select(1.055 * pow(linear, 1.0 / 2.4) - 0.055, linear * 12.92, linear < 0.0031308); 
}

[[fragment]] float4 fs_present_quad(
    present_data input [[stage_in]],
    texture2d<float, access::sample> source [[texture(0), function_constant(present_backbuffer_is_not_ms)]],
    texture2d_ms<float, access::read> source_ms [[texture(0), function_constant(present_backbuffer_is_ms)]],
    texture2d<float, access::sample> gamma_lut [[texture(1), function_constant(present_gamma_enabled)]],
    constant DXMTPresentMetadata& meta [[buffer(0)]]
) {
  float4 output = float4(0);
  // where the pixel is in the picture, in parts of it, and in the back buffer, which holds the picture turned
  float2 at = input.uv * meta.scale;
  float2 uv = (float2(dot(meta.turn.xy, at), dot(meta.turn.zw, at)) + meta.shift) * meta.source;
  if (present_backbuffer_is_ms) {
    const uint count = source_ms.get_num_samples();
    uint2 size = uint2(source_ms.get_width(), source_ms.get_height());
    for (uint i = 0; i < count; i++) {
      output += source_ms.read(min(uint2(uv * float2(size)), size - 1), i);
    }
    output /= count;
  } else {
    output = present_backbuffer_size_matched
      ? source.read(uint2(input.position.xy))
      : source.sample(s, uv);
  }
  if (any(at >= 1))
    output = meta.background;
  float3 output_rgb = output.xyz;
  if (present_gamma_enabled && !present_hdr_pq && !present_with_hdr_metadata) {
    output_rgb = float3(
        gamma_lut.sample(gamma_sampler, float2(output_rgb.r, 0.5)).r,
        gamma_lut.sample(gamma_sampler, float2(output_rgb.g, 0.5)).g,
        gamma_lut.sample(gamma_sampler, float2(output_rgb.b, 0.5)).b);
  }
  float edr_scale = meta.edr_scale;
  if (present_backbuffer_is_srgb)
    output_rgb = to_srgb(output_rgb);
  if (present_hdr_pq)
    output_rgb = pq_to_linear(output_rgb);
  else if (present_with_hdr_metadata)
    output_rgb = output_rgb * 80; // to nits
  if (present_with_hdr_metadata) {
    // tone mapping: BT.2408 EETF + maxRGB
    float max_content_luminance = meta.max_content_luminance;
    float min_content_luminance = 1; // in case of division by 0
    float max_display_luminance = meta.max_display_luminance;
    float min_display_luminance = 0;
    float3 clamped = clamp(output_rgb, float3(min_content_luminance), float3(max_content_luminance));
    float3 clamped_pq = linear_to_pq(clamped);
    float maxRGB = max3(clamped_pq.x, clamped_pq.y, clamped_pq.z);
    float maxRGB2 = EETF(
      maxRGB, max_content_luminance, min_content_luminance, max_display_luminance, min_display_luminance
    );
    output_rgb = pq_to_linear(maxRGB2) / pq_to_linear(maxRGB) * clamped;
  }
  output_rgb *= edr_scale;
  if (present_hdr_pq)
    output_rgb = linear_to_pq(output_rgb);
  else if (present_with_hdr_metadata)
    output_rgb = output_rgb / 80;
  return float4(output_rgb, output.w);
}

struct DXMTDispatchArguments {
  uint x;
  uint y;
  uint z;
};

struct DXMTTSDispatchMarshal {
  constant uint2& draw_arguments; // (vertex|index_count, instance_count)
  device DXMTDispatchArguments& dispatch_arguments_out;
  ulong max_object_threadgroups;
  ushort control_point_count;
  ushort patch_per_group;
  ushort parts;
  ushort end_of_command;
};

[[vertex]] void ts_draw_arguments_marshal(
    constant DXMTTSDispatchMarshal* tasks [[buffer(kCustomBufferArgumentIndex0)]]
) {
  uint index = 0;
  for(;;) {
    constant DXMTTSDispatchMarshal& task = tasks[index];

    device DXMTDispatchArguments& output = task.dispatch_arguments_out;

    uint patch_count_per_instance = task.draw_arguments.x / (uint)task.control_point_count;
    // rounded up without wrapping; zero when no patch is complete
    uint x = patch_count_per_instance / task.patch_per_group + (patch_count_per_instance % task.patch_per_group != 0);
    if (x == 0 || task.draw_arguments.y == 0 || (ulong)x * task.draw_arguments.y * task.parts > task.max_object_threadgroups) {
      output.x = 0;
      output.y = 0;
      output.z = 0;
    } else {
      output.x = x;
      output.y = task.draw_arguments.y;
      output.z = task.parts;
    }

    if (task.end_of_command)
      break;
    index++;
  };
}

// DrawAuto's arguments: the vertices of `stride` that stream output filled a buffer with past `offset`, as one
// instance; and for a draw that streams out again, its object threadgroups of `vertex_count_per_warp`
struct DXMTDrawAutoMarshal {
  constant ulong& filled;
  device uint4& draw_arguments_out; // (vertex_count, instance_count, start_vertex, start_instance)
  device uint& warps_out;
  uint offset;
  uint stride;
  uint vertex_count_per_warp;
  uint end_of_command;
};

[[vertex]] void draw_auto_arguments_marshal(
    constant DXMTDrawAutoMarshal* tasks [[buffer(kCustomBufferArgumentIndex0)]]
) {
  uint index = 0;
  for(;;) {
    constant DXMTDrawAutoMarshal& task = tasks[index];

    uint count = task.filled > task.offset ? uint((task.filled - task.offset) / task.stride) : 0;
    device uint4& draw_arguments = task.draw_arguments_out;
    device uint& warps = task.warps_out;
    draw_arguments = uint4(count, 1, 0, 0);
    warps = count / task.vertex_count_per_warp + (count % task.vertex_count_per_warp != 0);

    if (task.end_of_command)
      break;
    index++;
  };
}

struct DXMTGSDispatchMarshal {
  constant uint2& draw_arguments; // (vertex|index_count, instance_count)
  device DXMTDispatchArguments& dispatch_arguments_out;
  ulong max_object_threadgroups;
  uint vertex_count_per_warp;
  uint end_of_command;
};

[[vertex]] void gs_draw_arguments_marshal(
    constant DXMTGSDispatchMarshal* tasks [[buffer(kCustomBufferArgumentIndex0)]]
) {
  uint index = 0;
  for(;;) {
    constant DXMTGSDispatchMarshal& task = tasks[index];

    device DXMTDispatchArguments& output = task.dispatch_arguments_out;

    uint x = task.draw_arguments.x / task.vertex_count_per_warp + (task.draw_arguments.x % task.vertex_count_per_warp != 0);
    if (x == 0 || task.draw_arguments.y == 0 || (ulong)x * task.draw_arguments.y > task.max_object_threadgroups) {
      output.x = 0;
      output.y = 0;
      output.z = 0;
    } else {
      output.x = x;
      output.y = task.draw_arguments.y;
      output.z = 1;
    }

    if (task.end_of_command)
      break;
    index++;
  };
}

struct depth_stencil_out {
  float depth [[depth(any)]];
  uint stencil [[stencil]]; 
};

struct packed_d32s8x24 {
  float depth;
  uchar stencil;
  uchar unused[3];
};

struct linear_texture_desc {
  uint bytes_per_row;
  uint bytes_per_image;
};

[[fragment]] depth_stencil_out fs_copy_from_buffer_d32s8(
  present_data input [[stage_in]],
  device char* buffer [[buffer(kCustomBufferArgumentIndex0)]],
  constant linear_texture_desc& desc [[buffer(kCustomBufferArgumentIndex1)]]
) {
  depth_stencil_out result;
  uint2 pos = uint2(input.position.xy);
  uint buffer_offset = pos.x * sizeof(packed_d32s8x24) 
                        + pos.y * desc.bytes_per_row;
  packed_d32s8x24 data = *reinterpret_cast<device packed_d32s8x24 *>(buffer + buffer_offset);
  result.depth = data.depth;
  result.stencil = data.stencil;
  return result;
}

[[fragment]] depth_stencil_out fs_copy_from_buffer_d24s8(
  present_data input [[stage_in]],
  device char* buffer [[buffer(kCustomBufferArgumentIndex0)]],
  constant linear_texture_desc& desc [[buffer(kCustomBufferArgumentIndex1)]]
) {
  depth_stencil_out result;
  uint2 pos = uint2(input.position.xy);
  uint buffer_offset = pos.x * sizeof(uint) 
                        + pos.y * desc.bytes_per_row;
  uint data = *reinterpret_cast<device uint *>(buffer + buffer_offset);
  result.depth = float(data & 0xffffff) / float(0xffffff);
  result.stencil = data >> 24;
  return result;
}

[[kernel]] void cs_copy_to_buffer_d32s8(
  texture2d<float, access::read> tex_depth [[texture(0)]],
  texture2d<uint, access::read> tex_stencil [[texture(1)]],
  device char* buffer [[buffer(0)]],
  constant linear_texture_desc& desc [[buffer(1)]],
  ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex_depth.get_width();
  uint height = tex_depth.get_height();
  uint buffer_offset = pos.x * sizeof(packed_d32s8x24) 
                        + pos.y * desc.bytes_per_row;
  if (width > pos.x && height > pos.y) {
    auto dst = reinterpret_cast<device packed_d32s8x24 *>(buffer + buffer_offset);
    dst->depth = tex_depth.read(pos).x;
    dst->stencil = tex_stencil.read(pos).x;
  }
}

[[kernel]] void cs_copy_to_buffer_d24s8(
  texture2d<float, access::read> tex_depth [[texture(0)]],
  texture2d<uint, access::read> tex_stencil [[texture(1)]],
  device char* buffer [[buffer(0)]],
  constant linear_texture_desc& desc [[buffer(1)]],
  ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex_depth.get_width();
  uint height = tex_depth.get_height();
  uint buffer_offset = pos.x * sizeof(uint) 
                        + pos.y * desc.bytes_per_row;
  if (width > pos.x && height > pos.y) {
    auto dst = reinterpret_cast<device uint *>(buffer + buffer_offset);
    float depth = tex_depth.read(pos).x;
    uint stencil = tex_stencil.read(pos).x;
    *dst = (uint(0xffffff * clamp(depth, 0.0, 1.0)) & 0xffffff) | (stencil << 24);
  }
}

struct DXMTClearFloatMetadata {
  float4 value;
  uint2 offset;
  uint2 size;
};

struct DXMTClearUintMetadata {
  uint4 value;
  uint2 offset;
  uint2 size;
};

[[kernel]] void cs_clear_texture2d_float(
    texture2d<float, access::write> tex [[texture(0)]],
    constant DXMTClearFloatMetadata& meta [[buffer(1)]],
    uint2 pos [[thread_position_in_grid]]
) {
  tex.write(meta.value, meta.offset + pos);
}

[[kernel]] void cs_clear_texture2d_array_float(
    texture2d_array<float, access::write> tex [[texture(0)]],
    constant DXMTClearFloatMetadata& meta [[buffer(1)]],
    uint3 pos [[thread_position_in_grid]]
) {
  tex.write(meta.value, meta.offset + pos.xy, pos.z);
}

[[kernel]] void cs_clear_texture3d_float(
    texture3d<float, access::write> tex [[texture(0)]],
    constant DXMTClearFloatMetadata& meta [[buffer(1)]],
    uint2 pos [[thread_position_in_grid]]
) {
  auto depth = tex.get_depth();
  for (uint i = 0; i < depth; i++) {
    uint2 final_pos = meta.offset + pos;
    tex.write(meta.value, uint3(final_pos.x, final_pos.y, i));
  }
}

[[kernel]] void cs_clear_texture2d_uint(
    texture2d<uint, access::write> tex [[texture(0)]],
    constant DXMTClearUintMetadata& meta [[buffer(1)]],
    uint2 pos [[thread_position_in_grid]]
) {
  tex.write(meta.value, meta.offset + pos);
}

[[kernel]] void cs_clear_texture2d_array_uint(
    texture2d_array<uint, access::write> tex [[texture(0)]],
    constant DXMTClearUintMetadata& meta [[buffer(1)]],
    uint3 pos [[thread_position_in_grid]]
) {
  tex.write(meta.value, meta.offset + pos.xy, pos.z);
}

[[kernel]] void cs_clear_texture3d_uint(
    texture3d<uint, access::write> tex [[texture(0)]],
    constant DXMTClearUintMetadata& meta [[buffer(1)]],
    uint2 pos [[thread_position_in_grid]]
) {
  auto depth = tex.get_depth();
  for (uint i = 0; i < depth; i++) {
    uint2 final_pos = meta.offset + pos;
    tex.write(meta.value, uint3(final_pos.x, final_pos.y, i));
  }
}

[[kernel]] void cs_clear_tbuffer_float(
    texture_buffer<float, access::write> tex [[texture(0)]],
    constant DXMTClearFloatMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  tex.write(meta.value, meta.offset.x + pos);
}

[[kernel]] void cs_clear_tbuffer_uint(
    texture_buffer<uint, access::write> tex [[texture(0)]],
    constant DXMTClearUintMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  tex.write(meta.value, meta.offset.x + pos);
}

[[kernel]] void cs_clear_buffer_float(
    device float* buffer [[buffer(0)]],
    constant DXMTClearFloatMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  buffer[meta.offset.x + pos] = meta.value.x;
}

[[kernel]] void cs_clear_buffer_uint(
    device uint* buffer [[buffer(0)]],
    constant DXMTClearUintMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  buffer[meta.offset.x + pos] = meta.value.x;
}

[[kernel]] void cs_downscale_dilated_mv(
  uint2 pos [[thread_position_in_grid]],
  constant float2& mv_scale [[buffer(0)]],
  texture2d<float, access::read> dilated [[texture(0)]],
  texture2d<float, access::write> downscaled [[texture(1)]]
) {
  float2 source_size = float2(dilated.get_width(), dilated.get_height());
  float2 viewport_size = float2(downscaled.get_width(), downscaled.get_height());
  float2 scale = viewport_size / source_size;
  float2 sample_uv = (float2(pos) + 0.5) / viewport_size;
  uint2 sample_pos = uint2(sample_uv * source_size);
  float2 hi_mv = dilated.read(sample_pos).xy;
  float2 hi_mv_pixel = hi_mv * mv_scale;
  float2 lo_mv_pixel = hi_mv_pixel * scale;
  downscaled.write(lo_mv_pixel.xyxy, pos); 
}

[[kernel]] void tile_barrier(ushort2 pos [[thread_position_in_threadgroup]]) {
  // empty
}

// an occlusion query's result: the visibility counts of the pass segments it was active in, which the heap buffer
// holds at the `slots` indices it names, summed; a binary query's is whether any sample passed
struct OcclusionSum {
  uint query;
  uint first;
  uint count;
  uint binary;
};

[[kernel]] void sum_occlusion(
    constant OcclusionSum *sums [[buffer(0)]],
    constant uint *slots [[buffer(1)]],
    device const ulong *counts [[buffer(2)]],
    device ulong *results [[buffer(3)]],
    uint i [[thread_position_in_grid]]
) {
  OcclusionSum sum = sums[i];
  ulong total = 0;
  for (uint k = 0; k < sum.count; k++)
    total += counts[slots[sum.first + k]];
  results[sum.query] = sum.binary ? ulong(total != 0) : total;
}

// a predication region (SetPredication): each entry's GPU heap dword gets the value it was recorded with, or when the
// predicate skips the region's work, the one that runs none of it; `skips` says which, for the work the CPU leaves out
struct PredicatedWord {
  uint word;
  uint value;
  uint skipped;
};

struct PredicationRegion {
  uint count;
  uint entries;
  uint skip_if_zero;
  uint skips;
};

[[kernel]] void predicate(
    device const ulong *predicate [[buffer(0)]],
    device PredicationRegion *region [[buffer(1)]],
    device uint *heap [[buffer(2)]]
) {
  bool skip = (*predicate == 0) == bool(region->skip_if_zero);
  region->skips = skip;
  device const PredicatedWord *entries = (device const PredicatedWord *)(heap + region->entries);
  for (uint i = 0; i < region->count; i++)
    heap[entries[i].word] = skip ? entries[i].skipped : entries[i].value;
}

// sampler feedback transcoding (TranscodeFeedback in dxmt_command_feedback.hpp): a map has one texel per mip region of
// its paired texture's first mip, holding a bit for each mip wanted: bit n in the first region of each 2^n by 2^n
// block of regions, which is that mip's region
struct DXMTFeedbackMetadata {
  uint2 map_offset;
  uint2 offset;
  uint level;
  uint mips;
  uint min_mip;
  uint pitch;
  uint clear;
};

typedef texture2d_array<uint, access::read_write> feedback_map;

// where the map keeps `mip`'s bit for a region. a mip's own grid of regions lies over the whole map, whatever the
// sizes divide to ("Example of non-power-of-two feedback maps behavior"): a MipRegionUsed map's `region` is one of
// its mip's grid, and a MinMip map's is one of the first mip's, which wants the region of `mip` over its center
static uint2 feedback_region(feedback_map map, constant DXMTFeedbackMetadata &meta, uint2 region, uint mip) {
  uint2 grid = uint2(map.get_width(), map.get_height()), coarse = max(grid >> mip, uint2(1));
  if (meta.min_mip)
    region = (2 * region + 1) * coarse / (2 * grid);
  return min(region, coarse - 1) << mip;
}

// a MinMip map's region gives the first mip wanted there or 0xff, a MipRegionUsed map's whether its mip is wanted
static uint feedback_decode(feedback_map map, constant DXMTFeedbackMetadata &meta, uint3 pos) {
  for (uint mip = meta.level; mip < meta.level + meta.mips; mip++)
    if (map.read(feedback_region(map, meta, meta.map_offset + pos.xy, mip), pos.z).x >> mip & 1)
      return meta.min_mip ? mip : 0xff;
  return meta.min_mip ? 0xff : 0;
}

static void feedback_encode(feedback_map map, constant DXMTFeedbackMetadata &meta, uint3 pos, uint value) {
  uint2 region = meta.map_offset + pos.xy;
  uint mip = meta.min_mip ? value : meta.level;
  if (meta.clear)
    map.atomic_fetch_and(feedback_region(map, meta, region, meta.level), pos.z, meta.min_mip ? 0u : ~(1u << meta.level));
  else if (meta.min_mip ? mip < 32 : value != 0)
    map.atomic_fetch_or(feedback_region(map, meta, region, mip), pos.z, 1u << mip);
}

static uint feedback_byte(constant DXMTFeedbackMetadata &meta, uint3 pos) {
  return (meta.offset.y + pos.y) * meta.pitch + meta.offset.x + pos.x;
}

[[kernel]] void cs_feedback_decode(
    feedback_map map [[texture(0)]],
    texture2d_array<uint, access::write> decoded [[texture(1)]],
    constant DXMTFeedbackMetadata &meta [[buffer(2)]],
    uint3 pos [[thread_position_in_grid]]
) {
  decoded.write(feedback_decode(map, meta, pos), meta.offset + pos.xy, pos.z);
}

[[kernel]] void cs_feedback_decode_buffer(
    feedback_map map [[texture(0)]],
    device uchar *decoded [[buffer(1)]],
    constant DXMTFeedbackMetadata &meta [[buffer(2)]],
    uint3 pos [[thread_position_in_grid]]
) {
  decoded[feedback_byte(meta, pos)] = feedback_decode(map, meta, pos);
}

[[kernel]] void cs_feedback_encode(
    feedback_map map [[texture(0)]],
    texture2d_array<uint, access::read> decoded [[texture(1)]],
    constant DXMTFeedbackMetadata &meta [[buffer(2)]],
    uint3 pos [[thread_position_in_grid]]
) {
  feedback_encode(map, meta, pos, decoded.read(meta.offset + pos.xy, pos.z).x);
}

[[kernel]] void cs_feedback_encode_buffer(
    feedback_map map [[texture(0)]],
    device const uchar *decoded [[buffer(1)]],
    constant DXMTFeedbackMetadata &meta [[buffer(2)]],
    uint3 pos [[thread_position_in_grid]]
) {
  feedback_encode(map, meta, pos, decoded[feedback_byte(meta, pos)]);
}

// a top-level acceleration structure's instances, from Direct3D 12's (D3D12_RAYTRACING_INSTANCE_DESC) to Metal's
// (MTLIndirectAccelerationStructureInstanceDescriptor). both start with a row-major 3x4 matrix, and Direct3D's
// instance flags are Metal's instance options bit for bit. an instance names its bottom-level structure by the
// address of its memory, which starts with the resource ID of the Metal structure (AccelerationStructureHeader);
// one without a structure is inactive (DXR spec, "Inactive primitives and instances"), which a mask of 0 makes it
struct DXMTInstanceConversion {
  ulong instances; // the address of the instances, or of their addresses
  ulong kept;      // where the instances are kept as they are, after their structures' addresses, or 0
  uint pointers;
  uint count;
};

struct D3D12Instance {
  float transform[12];
  uint id_and_mask;                // 24 and 8 bits
  uint hit_group_and_flags;        // 24 and 8 bits
  ulong structure;
};

struct MetalInstance {
  float transform[12];
  uint options;
  uint mask;
  uint intersection_function_table_offset;
  uint user_id;
  ulong structure;
};

[[kernel]] void acceleration_structure_instances(
    constant DXMTInstanceConversion &conversion [[buffer(0)]],
    device MetalInstance *converted [[buffer(1)]],
    uint index [[thread_position_in_grid]]
) {
  device const D3D12Instance *instance =
      conversion.pointers ? (device const D3D12Instance *)(((device const ulong *)conversion.instances)[index])
                          : (device const D3D12Instance *)conversion.instances + index;
  device MetalInstance &out = converted[index];
  for (uint i = 0; i < 12; i++)
    out.transform[i] = instance->transform[i];
  ulong structure = instance->structure;
  out.options = instance->hit_group_and_flags >> 24 & 0xf;
  out.mask = structure ? instance->id_and_mask >> 24 : 0;
  out.intersection_function_table_offset = instance->hit_group_and_flags & 0xffffff;
  out.user_id = instance->id_and_mask & 0xffffff;
  out.structure = structure ? *(device const ulong *)structure : 0;
  if (conversion.kept) {
    device ulong *structures = (device ulong *)conversion.kept;
    structures[index] = structure;
    ((device D3D12Instance *)(structures + conversion.count))[index] = *instance;
  }
}

// DXR pipelines. every shader of a state object is a visible function of one signature, called by its slot in the
// pipeline's function table: the kernel calls the ray generation shader, and shaders call what follows (TraceRay,
// ReportHit and CallShader are calls of the first slots). a trace is an intersection query that runs the hit groups'
// any hit and intersection shaders as it meets their geometry, then a closest hit or a miss shader (DXR spec,
// "TraceRay control flow"). a shader reads its system values and finds its resources through its context
using namespace metal::raytracing;

struct DXMTRayContext;
// the context, then by shader kind: payload and attributes, a callable's parameter, a trace's arguments and payload
typedef void DXMTRayShader(thread DXMTRayContext &, thread void *, thread void *);

// AccelerationStructureHeader (d3d12_acceleration_structure.hpp)
struct DXMTRayScene {
  instance_acceleration_structure structure;
  ulong instances;
};

// mirrored by ray_context_type in dxil_converter.cpp
struct DXMTRayContext {
  visible_function_table<DXMTRayShader> shaders;
  constant SM50_RAY_DISPATCH *dispatch;
  constant ulong *root_arguments;
  constant ulong *static_samplers;
  ulong local_arguments; // the address of the shader record's local root arguments
  thread void *payload;  // of the ray, for the any hit shaders an intersection shader's reports run
  uint index[3];
  uint ray_flags;
  float world_origin[3];
  float tmin;
  float world_direction[3];
  float tcurrent;
  float object_origin[3];
  uint instance_index;
  float object_direction[3];
  uint instance_id;
  float object_to_world[12]; // row-major 3x4
  float world_to_object[12];
  uint geometry_index;
  uint primitive_index;
  uint hit_kind;
  uint verdict; // SM50_RAY_VERDICT, of an any hit shader
  // ReportHit: the hit an intersection shader reports, and whether it was accepted
  float reported_t;
  uint reported_kind;
  uint reported_size; // of its attributes
  uint accepted;
  // for the intersection shader's reports: the hit group's any hit shader if the geometry is not opaque, whether the
  // search has ended, and whether a report was accepted; and CallShader's index
  uint any_hit;
  uint ended;
  uint committed;
  uint callable;
  uint attributes[8]; // of the accepted report (D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES)
};

static_assert(sizeof(DXMTRayContext) == 304, "ray_context_type mirrors this layout");

static device const SM50_RAY_SHADER_IDENTIFIER *ray_record(ulong table, ulong stride, uint index) {
  return (device const SM50_RAY_SHADER_IDENTIFIER *)(table + stride * index);
}

static void ray_shader(thread DXMTRayContext &c, device const SM50_RAY_SHADER_IDENTIFIER *record, uint function,
                       thread void *first, thread void *second) {
  c.local_arguments = (ulong)record + sizeof(SM50_RAY_SHADER_IDENTIFIER);
  c.shaders[function](c, first, second);
}

static void ray_matrix(thread float *out, float4x3 m) {
  for (uint row = 0; row < 3; row++)
    for (uint column = 0; column < 4; column++)
      out[row * 4 + column] = m[column][row];
}

enum {
  kRayFlagAcceptFirstHit = 0x4,
  kRayFlagSkipClosestHit = 0x8,
  // D3D12_HIT_KIND_TRIANGLE_FRONT_FACE and _BACK_FACE
  kHitKindFront = 0xfe,
  kHitKindBack = 0xff,
};

[[visible]] void dxmt_ray_trace(thread DXMTRayContext &parent, thread void *arguments, thread void *payload) {
  thread SM50_RAY_TRACE &trace = *(thread SM50_RAY_TRACE *)arguments;
  // the shaders of this ray have their own context: the caller's system values are its own ray's
  DXMTRayContext c = parent;
  constant SM50_RAY_DISPATCH &dispatch = *c.dispatch;
  c.payload = payload;
  c.ray_flags = trace.flags;
  for (uint i = 0; i < 3; i++) {
    c.world_origin[i] = trace.origin[i];
    c.world_direction[i] = trace.direction[i];
  }
  c.tmin = trace.tmin;
  c.tcurrent = trace.tmax;

  intersection_type committed = intersection_type::none;
  uint attributes[8];
  thread float2 &barycentrics = *(thread float2 *)attributes;
  uint procedural_kind = 0;
  if (trace.scene) {
    device DXMTRayScene *scene = (device DXMTRayScene *)trace.scene;
    // RAY_FLAG's pairs are Metal's modes, 1 for the first flag and 2 for the second, but for facing, where
    // RAY_FLAG_CULL_BACK_FACING_TRIANGLES comes first and Metal's front mode does
    intersection_params params;
    uint flags = trace.flags | dispatch.pipeline_flags;
    uint cull = flags >> 4 & 3;
    params.force_opacity((forced_opacity)(flags & 3));
    params.set_triangle_cull_mode((triangle_cull_mode)(cull >> 1 | (cull << 1 & 2)));
    params.set_opacity_cull_mode((opacity_cull_mode)(flags >> 6 & 3));
    params.set_geometry_cull_mode((geometry_cull_mode)(flags >> 8 & 3));
    params.accept_any_intersection(flags & kRayFlagAcceptFirstHit);
    intersection_query<instancing, triangle_data> query;
    query.reset(
        ray(float3(trace.origin[0], trace.origin[1], trace.origin[2]),
            float3(trace.direction[0], trace.direction[1], trace.direction[2]), trace.tmin, trace.tmax),
        scene->structure, trace.mask & 0xff, params
    );
    // the hit group of a geometry of an instance ("Hit group table indexing"); the instance's contribution is its
    // intersection function table offset in MTLIndirectAccelerationStructureInstanceDescriptor
    auto hit_group = [&](uint instance, uint geometry) {
      uint contribution = *(device const uint *)(scene->instances + instance * 72 + 56);
      return ray_record(
          dispatch.hit_group_table, dispatch.hit_group_stride,
          trace.ray_contribution + trace.geometry_multiplier * geometry + contribution
      );
    };
    bool ended = false;
    while (!ended && query.next()) {
      auto record = hit_group(query.get_candidate_instance_id(), query.get_candidate_geometry_id());
      c.instance_index = query.get_candidate_instance_id();
      c.instance_id = query.get_candidate_user_instance_id();
      c.geometry_index = query.get_candidate_geometry_id();
      c.primitive_index = query.get_candidate_primitive_id();
      float3 origin = query.get_candidate_ray_origin(), direction = query.get_candidate_ray_direction();
      for (uint i = 0; i < 3; i++) {
        c.object_origin[i] = origin[i];
        c.object_direction[i] = direction[i];
      }
      ray_matrix(c.object_to_world, query.get_candidate_object_to_world_transform());
      ray_matrix(c.world_to_object, query.get_candidate_world_to_object_transform());
      // the ray's extent ends at the closest hit so far
      float closest = query.get_committed_intersection_type() == intersection_type::none
                          ? trace.tmax
                          : query.get_committed_distance();
      c.tcurrent = closest;
      if (query.get_candidate_intersection_type() == intersection_type::triangle) {
        // a triangle that is not opaque: its any hit shader decides, and without one it is a hit
        if (record->function[1]) {
          float2 candidate = query.get_candidate_triangle_barycentric_coord();
          c.tcurrent = query.get_candidate_triangle_distance();
          c.hit_kind = query.is_candidate_triangle_front_facing() ? kHitKindFront : kHitKindBack;
          c.verdict = SM50_RAY_VERDICT_ACCEPT;
          ray_shader(c, record, record->function[1], payload, &candidate);
          if (c.verdict == SM50_RAY_VERDICT_IGNORE)
            continue;
          ended = c.verdict == SM50_RAY_VERDICT_END_SEARCH;
        }
        query.commit_triangle_intersection();
        ended |= bool(trace.flags & kRayFlagAcceptFirstHit);
      } else if (record->function[2]) {
        // a procedural primitive: its intersection shader reports hits (dxmt_ray_report_hit)
        c.any_hit = query.is_candidate_non_opaque_bounding_box() ? record->function[1] : 0;
        c.ended = c.committed = 0;
        ray_shader(c, record, record->function[2], payload, nullptr);
        if (c.committed) {
          query.commit_bounding_box_intersection(c.tcurrent);
          procedural_kind = c.hit_kind;
          for (uint i = 0; i < 8; i++)
            attributes[i] = c.attributes[i];
        }
        ended = c.ended;
      }
    }
    committed = query.get_committed_intersection_type();
    if (committed != intersection_type::none) {
      auto record = hit_group(query.get_committed_instance_id(), query.get_committed_geometry_id());
      c.instance_index = query.get_committed_instance_id();
      c.instance_id = query.get_committed_user_instance_id();
      c.geometry_index = query.get_committed_geometry_id();
      c.primitive_index = query.get_committed_primitive_id();
      float3 origin = query.get_committed_ray_origin(), direction = query.get_committed_ray_direction();
      for (uint i = 0; i < 3; i++) {
        c.object_origin[i] = origin[i];
        c.object_direction[i] = direction[i];
      }
      ray_matrix(c.object_to_world, query.get_committed_object_to_world_transform());
      ray_matrix(c.world_to_object, query.get_committed_world_to_object_transform());
      c.tcurrent = query.get_committed_distance();
      c.hit_kind = procedural_kind;
      if (committed == intersection_type::triangle) {
        barycentrics = query.get_committed_triangle_barycentric_coord();
        c.hit_kind = query.is_committed_triangle_front_facing() ? kHitKindFront : kHitKindBack;
      }
      if (!(trace.flags & kRayFlagSkipClosestHit) && record->function[0])
        ray_shader(c, record, record->function[0], payload, attributes);
      return;
    }
  }
  // a ray that hit nothing still ends where it was to
  c.tcurrent = trace.tmax;
  auto record = ray_record(dispatch.miss_table, dispatch.miss_stride, trace.miss_index);
  if (dispatch.miss_table && record->function[0])
    ray_shader(c, record, record->function[0], payload, nullptr);
}

// an intersection shader's ReportHit: the hit is accepted if it is within the ray's extent, which for a procedural
// primitive includes its ends ("Ray extents"), and the any hit shader of geometry that is not opaque does not ignore
// it. it then ends the extent. a hit that ends the search also stops the intersection shader, which returns when
// it sees `ended` (dxil_converter.cpp)
[[visible]] void dxmt_ray_report_hit(thread DXMTRayContext &c, thread void *attributes, thread void *) {
  c.accepted = 0;
  float t = c.reported_t;
  if (c.ended || !(t >= c.tmin && t <= c.tcurrent))
    return;
  float closest = c.tcurrent;
  uint kind = c.hit_kind;
  c.tcurrent = t;
  c.hit_kind = c.reported_kind;
  if (c.any_hit) {
    c.verdict = SM50_RAY_VERDICT_ACCEPT;
    c.shaders[c.any_hit](c, c.payload, attributes);
    if (c.verdict == SM50_RAY_VERDICT_IGNORE) {
      c.tcurrent = closest;
      c.hit_kind = kind;
      return;
    }
    c.ended = c.verdict == SM50_RAY_VERDICT_END_SEARCH;
  }
  for (uint i = 0; i < (c.reported_size + 3) / 4 && i < 8; i++)
    c.attributes[i] = ((thread uint *)attributes)[i];
  c.accepted = c.committed = 1;
  c.ended |= uint(bool(c.ray_flags & kRayFlagAcceptFirstHit));
}

[[visible]] void dxmt_ray_call(thread DXMTRayContext &parent, thread void *parameter, thread void *) {
  constant SM50_RAY_DISPATCH &dispatch = *parent.dispatch;
  auto record = ray_record(dispatch.callable_table, dispatch.callable_stride, parent.callable);
  if (!dispatch.callable_table || !record->function[0])
    return;
  DXMTRayContext c = parent;
  ray_shader(c, record, record->function[0], parameter, nullptr);
}

[[kernel]] void dispatch_rays(
    constant ulong *root_arguments [[buffer(0)]],
    constant ulong *static_samplers [[buffer(1)]],
    constant SM50_RAY_DISPATCH &dispatch [[buffer(SM50_RAY_BINDING_DISPATCH)]],
    visible_function_table<DXMTRayShader> shaders [[buffer(SM50_RAY_BINDING_FUNCTIONS)]],
    uint3 index [[thread_position_in_grid]]
) {
  auto record = (device const SM50_RAY_SHADER_IDENTIFIER *)dispatch.ray_generation_record;
  if (!dispatch.ray_generation_record || !record->function[0])
    return;
  DXMTRayContext c = {};
  c.shaders = shaders;
  c.dispatch = &dispatch;
  c.root_arguments = root_arguments;
  c.static_samplers = static_samplers;
  for (uint i = 0; i < 3; i++)
    c.index[i] = index[i];
  ray_shader(c, record, record->function[0], nullptr, nullptr);
}
