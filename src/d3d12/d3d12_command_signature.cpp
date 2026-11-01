/*
 * Copyright 2026 Feifan He for CodeWeavers
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

#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "mullion_diag.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_command_allocator.hpp"

namespace mullion {

constexpr auto kSharedHeader = R"(
#include <metal_stdlib>

using namespace metal;

struct mullion_compute_command_data {
  command_buffer cmd_buf;
  ulong max_count;
  device uint * max_count_buffer;
  device char * argument_buffer;
  device ulong * static_samplers;
  device ulong * rootsig_qwords;
  uint rootsig_qwords_stride;
  packed_uint3 tgsize;
  device char * rays;
  uint ray_flags;
  device ulong *statistics;
  packed_uint2 execution_range;
  device packed_uint3 * dispatches;
};

struct d3d12_draw_arguments {
  uint vertex_count_per_instance;
  uint instance_count;
  uint start_vertex_location;
  uint start_instance_location;
};

struct d3d12_draw_indexed_arguments {
  uint index_count_per_instance;
  uint instance_count;
  uint start_index_location;
  int base_vertex_location;
  uint start_instance_location;
};

struct d3d12_vertex_buffer_view {
  device void * buffer;
  uint size_in_bytes;
  uint stride_in_bytes;
};

struct d3d12_index_buffer_view {
  device void * buffer;
  uint size_in_bytes;
  uint format;
};

// IndirectMeshDraw
struct mullion_indirect_mesh_draw {
  d3d12_index_buffer_view view;
  uint control_points;
  uint padding[3];
  uint arguments[5];
  packed_uint3 threadgroups;
  uint zero[4];
};

struct mullion_vertex_buffer {
  device void * buffer;
  uint stride;
  uint length;
};

struct mullion_draw_statistics {
  ulong pixels, primitives, vertices;
};

struct mullion_render_command_data {
  command_buffer cmd_buf;
  ulong max_count;
  device uint * max_count_buffer;
  device char * argument_buffer;
  device ulong * static_samplers;
  device ulong * rootsig_qwords;
  uint rootsig_qwords_stride;
  uint primitive_type;
  device char * vertex_buffer;
  device void * index_buffer;
  uint index_buffer_format;
  uint vertex_argbuf_stride;
  device d3d12_index_buffer_view * index_buffer_view;
  uint control_points;
  uint threads_per_patch;
  uint tessellation_parts;
  uint geometry_threads;
  uint geometry_increment;
  uint geometry_max_warps;
  packed_uint3 object_threads;
  packed_uint3 mesh_threads;
  uint vertex_slots;
  uint index_buffer_size;
  device uint * most;
  device uint * omitted;
  device mullion_indirect_mesh_draw * draws;
  device ushort * zeros;
  uint zero_count;
  device mullion_draw_statistics *pixel_statistics;
  device float *depth_ranges, *raster_scale;
  device mullion_draw_statistics *statistics;
  device uint *so_targets;
  ulong so_scratch_size;
  uint so_invocations;
  device uint *so_lacked;
  uint statistics_first, statistics_step, statistics_cut, statistics_cuts;
  packed_uint2 execution_range;
  uint extras_claimed, extras_done;
};

)";

class MTLD3D12CommandSignatureImpl : public MTLD3D12Pageable<MTLD3D12CommandSignature> {
  WMT::Reference<WMT::Library> statistics_library_;
  WMT::Reference<WMT::ComputePipelineState> statistics_resolver_;
  mullion::mutex statistics_mutex_;

public:
  MTLD3D12CommandSignatureImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12CommandSignature>(pDevice) {}

  WMT::ComputePipelineState StatisticsResolver() override {
    std::lock_guard lock(statistics_mutex_);
    if (!statistics_resolver_) {
      WMT::Reference<WMT::Error> error;
      auto function = statistics_library_.newFunction("count_indirect_strips");
      statistics_resolver_ = device_->GetMTLDevice().newComputePipelineState(function, error);
      if (!statistics_resolver_) {
        ERR("Failed to create indirect statistics pipeline: ", error.description().getUTF8String());
        abort();
      }
    }
    return statistics_resolver_;
  }

  HRESULT
  Initialize(const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature) {
    Com<ID3D12VersionedRootSignatureDeserializer> deserializer;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_desc = nullptr;
    if (pRootSignature) {
      const void *blob;
      auto size = static_cast<MTLD3D12RootSignature *>(pRootSignature)->GetBlob(&blob);
      HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&deserializer));
      if (FAILED(hr))
        return hr;
      deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &root_desc);
    }
    std::stringstream source;
    D3D12_INDIRECT_ARGUMENT_TYPE side_effect = ~(D3D12_INDIRECT_ARGUMENT_TYPE){};
    UpdateRootArguments = false;
    UpdateVertexBuffers = false;
    uint32_t ib_index = ~-0u;
    UINT bytes = 0;
    UINT next_root = 0, next_constant = 0;

    source << kSharedHeader;

    source << "struct __attribute__ ((packed)) d3d12_arguments {\n";

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      if (~side_effect != 0u)
        return FAIL(E_INVALIDARG);
      auto &arg = pDesc->pArgumentDescs[i];
      std::optional<D3D12_ROOT_PARAMETER_TYPE> parameter_type;
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
        parameter_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        parameter_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        parameter_type = D3D12_ROOT_PARAMETER_TYPE_SRV;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        parameter_type = D3D12_ROOT_PARAMETER_TYPE_UAV;
        break;
      default:
        break;
      }
      // DirectX-Specs, Indirect Drawing, Validation: root slots and constant ranges are ordered and compatible.
      if (parameter_type) {
        UINT index = arg.Constant.RootParameterIndex;
        if (!root_desc || index >= root_desc->Desc_1_1.NumParameters || index < next_root)
          return FAIL(E_INVALIDARG);
        auto &parameter = root_desc->Desc_1_1.pParameters[index];
        if (parameter.ParameterType != *parameter_type)
          return FAIL(E_INVALIDARG);
        if (arg.Type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT) {
          auto &constant = arg.Constant;
          auto end = uint64_t(constant.DestOffsetIn32BitValues) + constant.Num32BitValuesToSet;
          if (!constant.Num32BitValuesToSet ||
              (index == next_root && constant.DestOffsetIn32BitValues < next_constant) ||
              end > parameter.Constants.Num32BitValues)
            return FAIL(E_INVALIDARG);
          next_root = index;
          next_constant = constant.DestOffsetIn32BitValues + constant.Num32BitValuesToSet;
        } else {
          next_root = index + 1;
          next_constant = 0;
        }
      }
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        bytes += sizeof(D3D12_DISPATCH_ARGUMENTS);
        side_effect = arg.Type;
        source << "packed_uint3 dispatch;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
        bytes += sizeof(D3D12_DRAW_ARGUMENTS);
        side_effect = arg.Type;
        source << "d3d12_draw_arguments draw;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
        bytes += sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        side_effect = arg.Type;
        source << "d3d12_draw_indexed_arguments draw_indexed;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
        auto slot = arg.VertexBuffer.Slot;
        if (slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || (ResetVertexBuffers & (1u << slot)))
          return FAIL(E_INVALIDARG);
        ResetVertexBuffers |= 1u << slot;
        bytes += sizeof(D3D12_VERTEX_BUFFER_VIEW);
        UpdateVertexBuffers = true;
        source << "d3d12_vertex_buffer_view vb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
        bytes += sizeof(D3D12_INDEX_BUFFER_VIEW);
        if (ib_index != ~0u)
          return FAIL(E_INVALIDARG);
        ib_index = i;
        source << "d3d12_index_buffer_view ib;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
        bytes += arg.Constant.Num32BitValuesToSet * sizeof(UINT);
        UpdateRootArguments = true;
        for (unsigned j = 0; j < arg.Constant.Num32BitValuesToSet; j++) {
          source << "uint constant_" << i << "_" << j << ";\n";
        }
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
        bytes += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        UpdateRootArguments = true;
        source << "ulong cb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
        bytes += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        UpdateRootArguments = true;
        source << "ulong srv_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        bytes += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        UpdateRootArguments = true;
        source << "ulong uav_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: {
        bytes += sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
        side_effect = arg.Type;
        source << "packed_uint3 dispatch_mesh;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: {
        bytes += sizeof(D3D12_DISPATCH_RAYS_DESC);
        side_effect = arg.Type;
        // D3D12_DISPATCH_RAYS_DESC
        source << "ulong ray_generation_record, ray_generation_size, miss_table, miss_size, miss_stride, hit_group_table, "
                  "hit_group_size, hit_group_stride, callable_table, callable_size, callable_stride;\n"
                  "packed_uint3 ray_dimensions;\n";
        break;
      }
      default:
        return FAIL(E_INVALIDARG);
      }
    }
    source << "};\n\n";

    // DirectX-Specs "Indirect Drawing": an index buffer goes with an indexed draw, the stride is a multiple of four
    // that holds the arguments, and a root signature is given if and only if an argument changes a root argument
    if (~side_effect == 0 || (ib_index != ~0u && side_effect != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) ||
        pDesc->ByteStride % sizeof(UINT) || pDesc->ByteStride < bytes || UpdateRootArguments != !!pRootSignature)
      return FAIL(E_INVALIDARG);
    // ray dispatches are no commands of an indirect command buffer: the resolver leaves an IndirectRays for each,
    // which ExecuteIndirect dispatches the pipeline's kernel with
    bool rays = side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    bool is_compute = rays || side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    if (is_compute && UpdateVertexBuffers)
      return FAIL(E_INVALIDARG);
    bool indexed = side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    // a tessellated draw runs each group of patches in one object threadgroup of 32 threads (airconv's layout), a
    // geometry pipeline's its vertices likewise. for those the resolver leaves an IndirectMeshDraw (command_data.draws):
    // what the command's object stage reads and the draw's threadgroups, none for a command that draws nothing. an
    // ordinary pipeline's draw follows as the else branch
    auto tessellate = [&](const char *args, const char *count, const char *view, size_t words) {
      source << "if (command_data.draws) {\n";
      source << "device mullion_indirect_mesh_draw &draw = command_data.draws[i];\n";
      source << "draw.view = *(" << view << ");\n";
      source << "draw.control_points = command_data.control_points;\n";
      source << "for (uint word = 0; word < " << words << "; word++)\n";
      source << "draw.arguments[word] = reinterpret_cast<device uint *>(&arg." << args << ")[word];\n";
      source << "uint patches = command_data.threads_per_patch ? arg." << args << "." << count
             << " / command_data.control_points : 0, per_group = 32 / max(command_data.threads_per_patch, 1u);\n";
      source << "if (arg." << args << "." << count << " && arg." << args << ".instance_count)\n";
      source << "draw.threadgroups = command_data.geometry_threads ? uint3((arg." << args << "." << count
             << " - 1) / command_data.geometry_increment + 1, arg." << args << ".instance_count, 1) : "
             << "uint3((patches + per_group - 1) / per_group, arg." << args
             << ".instance_count, patches ? command_data.tessellation_parts : 0);\n";
      // a geometry command over the mesh threadgroups a draw may start draws nothing
      source << "ulong object_groups = ulong(draw.threadgroups.x) * draw.threadgroups.y;\n";
      source << "if (command_data.omitted && object_groups > command_data.geometry_max_warps) {\n";
      source << "command_data.omitted[i] = uint(min(object_groups, ulong(UINT_MAX)));\n";
      source << "draw.threadgroups = uint3(0);\n";
      source << "}\n";
      // stream output's grid, when the command's scratch (SM50StreamOutputScratch) fits, and the words of it its
      // passes add to (SM50StreamOutputZeroed): the filled sizes, then per stream a word for each object threadgroup
      // and each block of them. a command whose scratch does not fit is left out, with what it needed
      constexpr uint32_t block = 1u << SM50_STREAM_OUTPUT_BLOCK_SHIFT;
      uint64_t start = SM50StreamOutputZeroed(0), per_total = (SM50StreamOutputZeroed(block) - start) / (block + 1),
               per_count = SM50StreamOutputScratch(1, 1) - SM50StreamOutputScratch(1, 0);
      source << "if (command_data.so_targets) {\n";
      source << "ulong groups = ulong(draw.threadgroups.x) * draw.threadgroups.y;\n";
      source << "ulong zeroed = groups ? " << start << " + " << per_total << " * (groups + ((groups + " << block - 1
             << ") >> " << SM50_STREAM_OUTPUT_BLOCK_SHIFT << ")) : 0;\n";
      source << "ulong need = zeroed + " << per_count << " * groups * command_data.so_invocations;\n";
      source << "if (need > command_data.so_scratch_size) {\n";
      source << "command_data.so_lacked[i] = uint(min(need, ulong(UINT_MAX)));\n";
      source << "draw.threadgroups = uint3(0);\n";
      source << "zeroed = 0;\n";
      source << "}\n";
      source << "device uint *targets = command_data.so_targets + i * " << sizeof(SM50_STREAM_OUTPUT_TARGETS) / 4 << ";\n";
      source << "targets[" << offsetof(SM50_STREAM_OUTPUT_TARGETS, warps) / 4 << "] = draw.threadgroups.x;\n";
      source << "targets[" << offsetof(SM50_STREAM_OUTPUT_TARGETS, instances) / 4 << "] = draw.threadgroups.y;\n";
      source << "draw.zero[0] = uint(zeroed / 4);\n";
      source << "draw.zero[1] = 1;\n";
      source << "}\n";
      source << "} else\n";
    };
    // MULLION_D3D12_GPU_ERRORS: the largest numbers the commands ask for, which the report of a failed command buffer
    // gives with the pass: instances, vertices or indices, their start, the base vertex as written, start instance
    auto most = [&](const char *args, const char *count, const char *start, const char *base) {
      if (!diag::errors)
        return;
      const char *of[] = {"instance_count", count, start, base, "start_instance_location"};
      for (unsigned word = 0; word < std::size(of); word++)
        source << "atomic_fetch_max_explicit(reinterpret_cast<device atomic_uint *>(command_data.most) + " << word
               << ", uint(arg." << args << "." << of[word] << "), memory_order_relaxed);\n";
      source << "if (i == 0) command_data.most[" << std::size(of) << "] = count;\n";
    };
    CommandType = side_effect;
    UpdateIndexBuffer = ib_index != ~0u;

    // an indexed signature's resolver is this, then what it does once all its threads have (below)
    if (indexed)
      source << "static void resolve_command(uint x, device mullion_render_command_data &command_data) {\n";
    else
      source << "[[kernel]] void resolve_indirect_commands([[thread_position_in_grid]] uint x, device "
             << (is_compute ? "mullion_compute_command_data" : "mullion_render_command_data")
             << " &command_data [[buffer(30)]]) {\n";
    // each thread resolves one command
    source << "ulong i = x;\n";
    source << "uint count = command_data.max_count_buffer ? "
              "command_data.max_count_buffer[0] : command_data.max_count;\n";
    if (!rays && !indexed) {
      source << "if (i == 0) command_data.execution_range = uint2(0, "
                "min(count, uint(command_data.max_count))";
      source << ");\n";
    }
    source << "if (i >= command_data.max_count) return;\n";
    if (is_compute && !rays)
      source << "if (command_data.statistics) command_data.statistics[i] = 0;\n";
    if (!is_compute)
      source << "if (command_data.statistics && !command_data.statistics_cuts) {\n"
                "command_data.statistics[i] = {};\n"
                "if (!i) *command_data.pixel_statistics = {};\n"
                "}\n";
    if (!rays)
      source << (is_compute ? "if (i >= count && !command_data.dispatches) return;\n"
                              : "if (i >= count && !command_data.draws) return;\n");
    source << "{\n";
    source << "device d3d12_arguments& arg = reinterpret_cast<device d3d12_arguments *>("
              "command_data.argument_buffer + i * "
           << pDesc->ByteStride << ")[0];\n";
    if (rays) {
      source << "device char *ray = command_data.rays + i * " << sizeof(IndirectRays) << ";\n";
      source << "device packed_uint3 &threadgroups = *reinterpret_cast<device packed_uint3 *>(ray + "
             << offsetof(IndirectRays, threadgroups) << ");\n";
      source << "threadgroups = uint3(0);\n";
    } else if (is_compute) {
      source << "if (command_data.dispatches) command_data.dispatches[i] = uint3(0);\n"
                "else { compute_command cmd(command_data.cmd_buf, i); cmd.reset(); }\n";
    } else {
      // each command's draw is command i of the indirect command buffer (an indexed one's extras are past them, below)
      source << "render_command cmd(command_data.cmd_buf, i);\n";
      // no command to write where each command's draw is left in an IndirectMeshDraw, with no threadgroups so far
      source << "if (command_data.draws) command_data.draws[i].threadgroups = uint3(0), command_data.draws[i].zero[0] = 0;\n"
                "else cmd.reset();\n";
      source << "if (command_data.omitted) command_data.omitted[i] = 0;\n";
      source << "if (command_data.so_lacked) command_data.so_lacked[i] = 0;\n";
    }
    source << "if (i >= count) return;\n";
    source << "device ulong * rootsig_qwords = command_data.rootsig_qwords + "
              "(i * command_data.rootsig_qwords_stride);\n";
    if (!is_compute)
      source << "device mullion_vertex_buffer * vertex_buffer = "
                "reinterpret_cast<device mullion_vertex_buffer *>(command_data.vertex_buffer + "
                "(i * command_data.vertex_argbuf_stride));\n";

    // the caller binds what an IndirectMeshDraw's stages read; a command of the indirect command buffer binds its own
    if (!is_compute)
      source << "if (!command_data.draws) {\n";
    // a command of the indirect command buffer binds what the command's arguments change; with none, it inherits them
    auto bind = [&](const char *command) {
      if (UpdateRootArguments || UpdateVertexBuffers || UpdateIndexBuffer) {
          source << command << ".set_vertex_buffer(vertex_buffer," << SM50_BINDING_INDEX_VERTEX_BUFFER << ");\n";
          source << command << ".set_vertex_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
          source << command << ".set_vertex_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS
                 << ");\n";
          source << command << ".set_fragment_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
          source << command << ".set_fragment_buffer(command_data.static_samplers,"
                 << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
          source << "if (command_data.pixel_statistics) " << command
                 << ".set_fragment_buffer(command_data.pixel_statistics," << SM50_BINDING_INDEX_PIPELINE_STATISTICS
                 << ");\n";
          source << command << ".set_fragment_buffer(command_data.depth_ranges," << SM50_BINDING_INDEX_DEPTH_RANGES
                 << ");\n";
          source << command << ".set_vertex_buffer(command_data.raster_scale," << SM50_BINDING_INDEX_RASTER_SCALE
                 << ");\n";
      }
    };
    if (!is_compute)
      bind("cmd");
    if (!is_compute)
      source << "}\n";

    auto statistics = [&](bool indexed) {
      const char *arguments = indexed ? "arg.draw_indexed" : "arg.draw";
      source << "if (command_data.statistics && command_data.statistics_first && !command_data.statistics_cuts) {\n"
             << "ulong vertices = " << arguments
             << (indexed ? ".index_count_per_instance" : ".vertex_count_per_instance") << ", instances = " << arguments
             << ".instance_count;\n"
                "ulong first = command_data.statistics_first, step = command_data.statistics_step;\n"
                "ulong primitives = vertices >= first ? (vertices - first) / step + 1 : 0;\n";
      source << "command_data.statistics[i].vertices = vertices * instances;\n"
                "command_data.statistics[i].primitives = primitives * instances;\n}\n";
    };

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
        statistics(false);
        most("draw", "vertex_count_per_instance", "start_vertex_location", "start_vertex_location");
        tessellate(
            "draw", "vertex_count_per_instance", "command_data.index_buffer_view",
            sizeof(D3D12_DRAW_ARGUMENTS) / sizeof(UINT)
        );
        source << "cmd.draw_primitives((primitive_type)command_data.primitive_type, "
                  "arg.draw.start_vertex_location, "
                  "arg.draw.vertex_count_per_instance, arg.draw.instance_count, "
                  "arg.draw.start_instance_location);\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
        if (ib_index == ~0u) {
          source << "bool ib32bit = command_data.index_buffer_format == 42;\n";
          source << "device void* ib = command_data.index_buffer;\n";
          source << "uint ib_bytes = command_data.index_buffer_size;\n";
        }
        statistics(true);
        most("draw_indexed", "index_count_per_instance", "start_index_location", "base_vertex_location");
        tessellate(
            "draw_indexed", "index_count_per_instance",
            // a command without an index buffer reads its indices from the list's empty view
            ib_index == ~0u ? "command_data.index_buffer_view" : "(arg.ib.buffer ? &arg.ib : command_data.index_buffer_view)",
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / sizeof(UINT)
        );
        // Metal does not apply the view bounds or D3D's unsigned byte-address wrap (D3D11.3 8.6.1, 8.19.1, 8.19.2): the
        // indices run from the start to where the address wraps, then on from the view's first
        auto draw = [&](const char *command, const char *count, const char *indices) {
          source << "if (" << count << ") " << command << ".draw_indexed_primitives((primitive_type)command_data.primitive_type, "
                 << count << ", " << indices << ", arg.draw_indexed.instance_count, arg.draw_indexed.base_vertex_location, "
                    "arg.draw_indexed.start_instance_location);\n";
        };
        source << "{\n";
        source << "uint ib_size = ib32bit ? sizeof(uint) : sizeof(ushort), ib_there = ib_bytes / ib_size;\n";
        source << "ulong period = (ulong(1) << 32) / ib_size, count = arg.draw_indexed.index_count_per_instance;\n";
        source << "uint start = uint(arg.draw_indexed.start_index_location % period);\n";
        source << "uint before = uint(min(ulong(count), period - start)), after = uint(count - before);\n";
        source << "uint inside = start < ib_there ? min(before, ib_there - start) : 0, wrapped_inside = min(after, ib_there);\n";
        source << "if (ib32bit) {\n";
        draw("cmd", "inside", "reinterpret_cast<device uint *>(ib) + start");
        source << "} else {\n";
        draw("cmd", "inside", "reinterpret_cast<device ushort *>(ib) + start");
        source << "}\n";
        // the rest are extra commands, past the draws in the order the draws claim them (command_data.extras_claimed):
        // the indices past the view, which draw zeros (command_data.zeros), and where the byte address wraps, the
        // view's from its start, then zeros again. a draw rarely has any, so the commands run are the draws and as many
        // extras as there are, not kIndexedCommands for every draw
        source << "uint rest = command_data.zeros ? min(before - inside, command_data.zero_count) : 0;\n";
        source << "uint wrapped_rest = command_data.zeros ? min(after - wrapped_inside, command_data.zero_count) : 0;\n";
        source << "uint extras = command_data.zeros ? uint(rest != 0) + uint(wrapped_inside != 0) + uint(wrapped_rest != 0) : 0;\n";
        source << "if (extras) {\n";
        // past the draws run: the command count, which `count` here is not (the draw's index count is)
        source << "uint at = min(command_data.max_count_buffer ? command_data.max_count_buffer[0] : "
                  "uint(command_data.max_count), uint(command_data.max_count)) + atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint *>("
                  "&command_data.extras_claimed), extras, memory_order_relaxed);\n";
        auto extra = [&](const char *count, auto indices) {
          source << "if (" << count << ") {\n";
          source << "render_command extra(command_data.cmd_buf, at++);\n";
          source << "extra.reset();\n";
          bind("extra");
          indices();
          source << "}\n";
        };
        extra("rest", [&] { draw("extra", "rest", "command_data.zeros"); });
        extra("wrapped_inside", [&] {
          source << "if (ib32bit) {\n";
          draw("extra", "wrapped_inside", "reinterpret_cast<device uint *>(ib)");
          source << "} else {\n";
          draw("extra", "wrapped_inside", "reinterpret_cast<device ushort *>(ib)");
          source << "}\n";
        });
        extra("wrapped_rest", [&] { draw("extra", "wrapped_rest", "command_data.zeros"); });
        source << "}\n";
        source << "}\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        source << "if (command_data.statistics) command_data.statistics[i] = ulong(arg.dispatch.x) * arg.dispatch.y * "
                  "arg.dispatch.z * command_data.tgsize.x * command_data.tgsize.y * command_data.tgsize.z;\n";
        source << "if (command_data.dispatches) command_data.dispatches[i] = arg.dispatch;\n"
                  "else {\n"
                  "compute_command cmd(command_data.cmd_buf, i);\n";
        if (UpdateRootArguments) {
          source << "cmd.set_kernel_buffer(rootsig_qwords, " << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
          source << "cmd.set_kernel_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
        }
        source << "cmd.concurrent_dispatch_threadgroups(arg.dispatch, command_data.tgsize);\n";
        source << "}\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: {
        auto field = [&](size_t offset, const char *type, const char *value) {
          source << "*reinterpret_cast<device " << type << " *>(ray + " << offset << ") = " << value << ";\n";
        };
        field(offsetof(SM50_RAY_DISPATCH, ray_generation_record), "ulong", "arg.ray_generation_record");
        field(offsetof(SM50_RAY_DISPATCH, miss_table), "ulong", "arg.miss_table");
        field(offsetof(SM50_RAY_DISPATCH, miss_stride), "ulong", "arg.miss_stride");
        field(offsetof(SM50_RAY_DISPATCH, hit_group_table), "ulong", "arg.hit_group_table");
        field(offsetof(SM50_RAY_DISPATCH, hit_group_stride), "ulong", "arg.hit_group_stride");
        field(offsetof(SM50_RAY_DISPATCH, callable_table), "ulong", "arg.callable_table");
        field(offsetof(SM50_RAY_DISPATCH, callable_stride), "ulong", "arg.callable_stride");
        field(offsetof(SM50_RAY_DISPATCH, dimensions), "packed_uint3", "arg.ray_dimensions");
        field(offsetof(SM50_RAY_DISPATCH, pipeline_flags), "uint", "command_data.ray_flags");
        // whole threadgroups along x, which the kernel cuts to the width
        source << "threadgroups = uint3((arg.ray_dimensions.x + command_data.tgsize.x - 1) / command_data.tgsize.x, "
                  "arg.ray_dimensions.y, arg.ray_dimensions.z);\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: {
        source << "if (all(uint3(arg.dispatch_mesh) != 0))\n";
        source << "command_data.draws[i].threadgroups = arg.dispatch_mesh;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
        auto slot = arg.VertexBuffer.Slot;
        // the table holds the slots the pipeline's input layout uses, in order: a slot's entry is as far in as there
        // are used slots below it
        source << "if (command_data.vertex_slots >> " << slot << " & 1) vertex_buffer[popcount(command_data.vertex_slots & "
               << ((1u << slot) - 1) << "u)] = {arg.vb_" << i << ".buffer,arg.vb_" << i << ".stride_in_bytes,arg.vb_" << i
               << ".size_in_bytes};\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
        source << "bool ib32bit = arg.ib.format == 42;\n";
        source << "device void* ib = arg.ib.buffer;\n";
        source << "uint ib_bytes = arg.ib.size_in_bytes;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
        auto parameter_index = arg.Constant.RootParameterIndex;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2 + arg.Constant.DestOffsetIn32BitValues, arg.Constant.Num32BitValuesToSet);
        for (unsigned j = 0; j < arg.Constant.Num32BitValuesToSet; j++) {
          source << "reinterpret_cast<device uint *>(rootsig_qwords + " << offset << ")["
                 << (j + arg.Constant.DestOffsetIn32BitValues) << "] = arg.constant_" << i << "_" << j << ";\n";
        }
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
        auto parameter_index = arg.ConstantBufferView.RootParameterIndex;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2, 2);
        source << "rootsig_qwords[" << offset << "] = arg.cb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
        auto parameter_index = arg.ShaderResourceView.RootParameterIndex;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2, 2);
        source << "rootsig_qwords[" << offset << "] = arg.srv_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        auto parameter_index = arg.UnorderedAccessView.RootParameterIndex;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2, 2);
        source << "rootsig_qwords[" << offset << "] = arg.uav_" << i << ";\n";
        break;
      }
      default:
        return FAIL(E_INVALIDARG);
      }
    }

    source << "}\n"
              "};\n";

    // an indexed signature's resolver sets the commands to run once all its threads have claimed their extras: the
    // draws and the extras after them. the last thread to finish reads the claims, and leaves both counts at zero for
    // the next execution. a device fence orders each thread's claim before its finishing, and the claims before the
    // last thread's reading of them. the threads past the count write no command, so none races an extra
    if (indexed)
      source << "[[kernel]] void resolve_indirect_commands([[thread_position_in_grid]] uint x, "
                "[[threads_per_grid]] uint threads, device mullion_render_command_data &command_data [[buffer(30)]]) {\n"
                "resolve_command(x, command_data);\n"
                "atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n"
                "if (atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint *>(&command_data.extras_done), 1, "
                "memory_order_relaxed) != threads - 1) return;\n"
                "atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n"
                "uint claimed = atomic_exchange_explicit(reinterpret_cast<device atomic_uint *>(&command_data.extras_claimed), "
                "0, memory_order_relaxed);\n"
                "atomic_store_explicit(reinterpret_cast<device atomic_uint *>(&command_data.extras_done), 0, "
                "memory_order_relaxed);\n"
                "uint count = command_data.max_count_buffer ? command_data.max_count_buffer[0] : command_data.max_count;\n"
                "command_data.execution_range = uint2(0, min(count, uint(command_data.max_count)) + claimed);\n}\n";
    if (indexed) {
      source << "[[kernel]] void count_indirect_strips(device mullion_render_command_data &data [[buffer(30)]],\n"
                "uint3 group [[threadgroup_position_in_grid]], uint lane [[thread_index_in_threadgroup]],\n"
                "uint3 group_size [[threads_per_threadgroup]]) {\n"
                "uint i = group.x, lanes = group_size.x;\n"
                "if (i >= data.max_count) return;\n"
                "uint count = data.max_count_buffer ? min(ulong(*data.max_count_buffer), data.max_count)\n"
                "                                  : data.max_count;\n"
                "if (!lane) { data.statistics[i] = {}; if (!i) *data.pixel_statistics = {}; }\n"
                "if (i >= count) return;\n"
                "device d3d12_arguments &arg = *reinterpret_cast<device d3d12_arguments *>(data.argument_buffer + i * "
             << pDesc->ByteStride << ");\n";
      if (ib_index != ~0u)
        source << "auto ib = arg.ib;\n";
      else
        source << "d3d12_index_buffer_view ib{data.index_buffer, data.index_buffer_size, data.index_buffer_format};\n";
      source << "ulong vertices = arg.draw_indexed.index_count_per_instance;\n"
                "ulong instances = arg.draw_indexed.instance_count;\n"
                "uint first = data.statistics_first, step = data.statistics_step;\n"
                "uint scanners = step == 1 ? lanes : 1;\n"
                "ulong length = (vertices + scanners - 1) / scanners;\n"
                "ulong lo = min(ulong(lane) * length, vertices), hi = min(lo + length, vertices);\n"
                "uint run = 0, primitives = 0, stride = ib.format == " << DXGI_FORMAT_R32_UINT
             << " ? sizeof(uint) : sizeof(ushort);\n"
                "for (ulong at = lo - min(lo, ulong(first - 1)); at < hi; at++) {\n"
                "uint offset = (arg.draw_indexed.start_index_location + uint(at)) * stride, value = 0;\n"
                "if (ib.buffer && ulong(offset) + stride <= ib.size_in_bytes) {\n"
                "auto address = reinterpret_cast<device const uchar *>(ib.buffer) + offset;\n"
                "value = stride == sizeof(uint) ? *reinterpret_cast<device const uint *>(address)\n"
                "                              : *reinterpret_cast<device const ushort *>(address);\n}\n"
                "run = value == data.statistics_cut ? 0 : run + 1;\n"
                "if (at >= lo && run >= first && (run - first) % step == 0) primitives++;\n}\n"
                "uint total = simd_sum(primitives);\n"
                "if (!lane) { data.statistics[i].vertices = vertices * instances;\n"
                "data.statistics[i].primitives = ulong(total) * instances; }\n}\n";
    }

    WMT::Reference<WMT::Error> err;
    auto lib = device_->GetMTLDevice().newLibraryWithSource(source.view(), err);

    if (!lib) {
      ERR("Failed to compile command signature resolve shader: ", err.description().getUTF8String());
      return E_FAIL;
    }

    if (indexed)
      statistics_library_ = lib;
    auto function = lib.newFunction("resolve_indirect_commands");

    if (!function) {
      ERR("Failed to create command signature resolve shader");
      return E_FAIL;
    }

    (is_compute ? compute_resolver : render_resolver) = device_->GetMTLDevice().newComputePipelineState(function, err);

    if (err) {
      ERR("Failed to compile command signature resolve pso: ", err.description().getUTF8String());
      return E_FAIL;
    }

    return S_OK;
  };

  ~MTLD3D12CommandSignatureImpl() {}

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12CommandSignature: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }
};

HRESULT
CreateCommandSignature(
    MTLD3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature,
    REFIID riid, void **ppCommandSignature
) {
  auto sig = Com(new MTLD3D12CommandSignatureImpl(pDevice));
  HRESULT hr = sig->Initialize(pDesc, pRootSignature);
  if (FAILED(hr))
    return hr;
  return sig->QueryInterface(riid, ppCommandSignature);
}

} // namespace mullion