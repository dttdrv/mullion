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
#include "d3d12_pageable.hpp"
#include "d3d12_command_allocator.hpp"

namespace dxmt {

constexpr auto kSharedHeader = R"(
#include <metal_stdlib>

using namespace metal;

struct dxmt_compute_command_data {
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
struct dxmt_indirect_mesh_draw {
  d3d12_index_buffer_view view;
  uint control_points;
  uint padding[3];
  uint arguments[5];
  packed_uint3 threadgroups;
};

struct dxmt_vertex_buffer {
  device void * buffer;
  uint stride;
  uint length;
};

struct dxmt_render_command_data {
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
  packed_uint3 object_threads;
  packed_uint3 mesh_threads;
  uint vertex_slots;
  uint index_buffer_size;
  device uint * most;
  device dxmt_indirect_mesh_draw * draws;
  device ushort * zeros;
  uint zero_count;
};

)";

class MTLD3D12CommandSignatureImpl : public MTLD3D12Pageable<MTLD3D12CommandSignature> {

public:
  MTLD3D12CommandSignatureImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12CommandSignature>(pDevice) {}

  HRESULT
  Initialize(const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature) {
    std::stringstream source;
    D3D12_INDIRECT_ARGUMENT_TYPE side_effect = ~(D3D12_INDIRECT_ARGUMENT_TYPE){};
    UpdateRootArguments = false;
    UpdateVertexBuffers = false;
    uint32_t ib_index = ~-0u;
    UINT bytes = 0;

    source << kSharedHeader;

    source << "struct __attribute__ ((packed)) d3d12_arguments {\n";

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      if (~side_effect != 0u)
        return E_INVALIDARG;
      auto &arg = pDesc->pArgumentDescs[i];
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
        bytes += sizeof(D3D12_VERTEX_BUFFER_VIEW);
        UpdateVertexBuffers = true;
        source << "d3d12_vertex_buffer_view vb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
        bytes += sizeof(D3D12_INDEX_BUFFER_VIEW);
        if (ib_index != ~0u)
          return E_INVALIDARG;
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
        return E_INVALIDARG;
      }
    }
    source << "};\n\n";

    // DirectX-Specs "Indirect Drawing": an index buffer goes with an indexed draw, the stride is a multiple of four
    // that holds the arguments, and a root signature is given if and only if an argument changes a root argument
    if (~side_effect == 0 || (ib_index != ~0u && side_effect != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) ||
        pDesc->ByteStride % sizeof(UINT) || pDesc->ByteStride < bytes || UpdateRootArguments != !!pRootSignature)
      return E_INVALIDARG;
    // ray dispatches are no commands of an indirect command buffer: the resolver leaves an IndirectRays for each,
    // which ExecuteIndirect dispatches the pipeline's kernel with
    bool rays = side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    bool is_compute = rays || side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    bool indexed = side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    // a tessellated draw runs each group of patches in one object threadgroup of 32 threads (airconv's layout), which
    // reads the command's arguments and index buffer view; the command's draw follows as the else branch
    // a geometry pipeline's draw runs its vertices in object threadgroups likewise
    // where the resolver leaves IndirectMeshDraws (command_data.draws) it writes what the command's object stage reads
    // and the threadgroups the same draw would have, none for a command that draws nothing
    auto tessellate = [&](const char *args, const char *count, const char *view, size_t words) {
      source << "if (command_data.draws) {\n";
      source << "device dxmt_indirect_mesh_draw &draw = command_data.draws[i];\n";
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
      source << "} else if (command_data.geometry_threads) {\n";
      source << "cmd.set_object_buffer(&arg." << args << "," << SM50_BINDING_INDEX_DRAW_ARGUMENTS << ");\n";
      source << "cmd.set_object_buffer(" << view << "," << SM50_BINDING_INDEX_INDEX_BUFFER << ");\n";
      source << "if (arg." << args << "." << count << " && arg." << args << ".instance_count)\n";
      source << "cmd.draw_mesh_threadgroups(uint3((arg." << args << "." << count
             << " - 1) / command_data.geometry_increment + 1, arg." << args
             << ".instance_count, 1), uint3(command_data.geometry_threads, 1, 1), uint3(1, 1, 1));\n";
      source << "} else if (command_data.threads_per_patch) {\n";
      source << "uint patches = arg." << args << "." << count << " / command_data.control_points, "
             << "per_group = 32 / command_data.threads_per_patch;\n";
      source << "cmd.set_object_buffer(&arg." << args << "," << SM50_BINDING_INDEX_DRAW_ARGUMENTS << ");\n";
      source << "cmd.set_object_buffer(" << view << "," << SM50_BINDING_INDEX_INDEX_BUFFER << ");\n";
      // the patch size follows the command list's view, 16 bytes on
      source << "cmd.set_object_buffer(reinterpret_cast<device uint *>(command_data.index_buffer_view + 1),"
             << SM50_BINDING_INDEX_PATCH_SIZE << ");\n";
      source << "if (patches && arg." << args << ".instance_count)\n";
      source << "cmd.draw_mesh_threadgroups(uint3((patches + per_group - 1) / per_group, arg." << args
             << ".instance_count, command_data.tessellation_parts), uint3(command_data.threads_per_patch, per_group, 1), "
             << "uint3(32, 1, 1));\n";
      source << "} else\n";
    };
    // DXMT_D3D12_GPU_ERRORS: the largest numbers the commands ask for, which the report of a failed command buffer
    // gives with the pass: instances, vertices or indices, their start, the base vertex as written, start instance
    auto most = [&](const char *args, const char *count, const char *start, const char *base) {
      if (!device_->NamesPasses())
        return;
      const char *of[] = {"instance_count", count, start, base, "start_instance_location"};
      for (unsigned word = 0; word < std::size(of); word++)
        source << "command_data.most[" << word << "] = max(command_data.most[" << word << "], uint(arg." << args << "."
               << of[word] << "));\n";
      source << "command_data.most[" << std::size(of) << "] = count;\n";
    };
    CommandType = side_effect;
    UpdateIndexBuffer = ib_index != ~0u;

    source << "[[kernel]] void resolve_indirect_commands([[thread_position_in_grid]] uint x, constant "
           << (is_compute ? "dxmt_compute_command_data" : "dxmt_render_command_data")
           << " &command_data [[buffer(30)]]) {\n";
    source << "if (x !=0 ) return;\n";

    source << "uint count = command_data.max_count_buffer ? "
              "command_data.max_count_buffer[0] : command_data.max_count;\n";
    source << "for (uint i = 0; i < command_data.max_count; i++) {\n";
    source << "device d3d12_arguments& arg = reinterpret_cast<device d3d12_arguments *>("
              "command_data.argument_buffer + i * "
           << pDesc->ByteStride << ")[0];\n";
    if (rays) {
      source << "device char *ray = command_data.rays + i * " << sizeof(IndirectRays) << ";\n";
      source << "device packed_uint3 &threadgroups = *reinterpret_cast<device packed_uint3 *>(ray + "
             << offsetof(IndirectRays, threadgroups) << ");\n";
      source << "threadgroups = uint3(0);\n";
    } else {
      // an indexed draw's indices past their view are a second command, `rest`, which draws zeros (command_data.zeros)
      source << (is_compute ? "compute_command" : "render_command") << " cmd(command_data.cmd_buf, "
             << (indexed ? "command_data.zeros ? 2 * i : i" : "i") << ");\n";
      if (indexed)
        source << "render_command rest(command_data.cmd_buf, 2 * i + 1);\n";
      // no command to write where each command's draw is left in an IndirectMeshDraw, with no threadgroups so far
      source << (is_compute ? "cmd.reset();\n"
                            : "if (command_data.draws) command_data.draws[i].threadgroups = uint3(0); else cmd.reset();\n");
      if (indexed)
        source << "if (command_data.zeros) rest.reset();\n";
    }
    source << "if (i >= count) continue;\n";
    source << "device ulong * rootsig_qwords = command_data.rootsig_qwords + "
              "(i * command_data.rootsig_qwords_stride);\n";
    if (!is_compute)
      source << "device dxmt_vertex_buffer * vertex_buffer = "
                "reinterpret_cast<device dxmt_vertex_buffer *>(command_data.vertex_buffer + "
                "(i * command_data.vertex_argbuf_stride));\n";

    // under tessellation the vertex and hull shaders run in the object stage, the domain shader in the mesh stage, and
    // every command binds its buffers, since each binds its own arguments
    if (!is_compute) {
      // a mesh shader pipeline has no vertex buffers
      source << "if (command_data.draws) {\n";
      source << "} else if (command_data.threads_per_patch || command_data.geometry_threads || command_data.mesh_threads.x) {\n";
      source << "if (!command_data.mesh_threads.x)\n";
      source << "cmd.set_object_buffer(vertex_buffer," << SM50_BINDING_INDEX_VERTEX_BUFFER << ");\n";
      for (auto stage : {"object", "mesh", "fragment"}) {
        source << "cmd.set_" << stage << "_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
        source << "cmd.set_" << stage << "_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS
               << ");\n";
      }
      source << "} else {\n";
    }
    if (UpdateRootArguments || UpdateVertexBuffers || UpdateIndexBuffer) {
      if (!is_compute) {
        for (auto command : {"cmd", "rest"}) {
          if (command == std::string_view("rest")) {
            if (!indexed)
              break;
            source << "if (command_data.zeros) {\n";
          }
          source << command << ".set_vertex_buffer(vertex_buffer," << SM50_BINDING_INDEX_VERTEX_BUFFER << ");\n";
          source << command << ".set_vertex_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
          source << command << ".set_vertex_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS
                 << ");\n";
          source << command << ".set_fragment_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
          source << command << ".set_fragment_buffer(command_data.static_samplers,"
                 << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
        }
        if (indexed)
          source << "}\n";
      } else if (!rays) {
        source << "cmd.set_kernel_buffer(rootsig_qwords, " << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
        source << "cmd.set_kernel_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
      }
    }
    if (!is_compute)
      source << "}\n";

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
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
        most("draw_indexed", "index_count_per_instance", "start_index_location", "base_vertex_location");
        tessellate(
            "draw_indexed", "index_count_per_instance",
            // a command without an index buffer reads its indices from the list's empty view
            ib_index == ~0u ? "command_data.index_buffer_view" : "(arg.ib.buffer ? &arg.ib : command_data.index_buffer_view)",
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / sizeof(UINT)
        );
        // indices past the view's end read as 0 in Direct3D (D3D11.3 8.19.2), which draws its vertex 0 again; Metal
        // would read what lies behind the view, mapped or not: the command draws the indices the view has, and
        // `rest` as many zeros as are past it, up to the zeros there are
        source << "{\n";
        source << "uint ib_there = ib_bytes / (ib32bit ? 4 : 2), ib_start = arg.draw_indexed.start_index_location;\n";
        source << "uint ib_count = ib_start < ib_there ? min(arg.draw_indexed.index_count_per_instance, ib_there - "
                  "ib_start) : 0;\n";
        source << "if (ib32bit) {\n";
        source << "cmd.draw_indexed_primitives((primitive_type)command_data.primitive_type, "
                  "ib_count, "
                  "reinterpret_cast<device uint *>(ib) + arg.draw_indexed.start_index_location, "
                  "arg.draw_indexed.instance_count, arg.draw_indexed.base_vertex_location, "
                  "arg.draw_indexed.start_instance_location);\n";
        source << "} else {\n";
        source << "cmd.draw_indexed_primitives((primitive_type)command_data.primitive_type, "
                  "ib_count, "
                  "reinterpret_cast<device ushort *>(ib) + arg.draw_indexed.start_index_location, "
                  "arg.draw_indexed.instance_count, arg.draw_indexed.base_vertex_location, "
                  "arg.draw_indexed.start_instance_location);\n";
        source << "}\n";
        source << "uint ib_rest = min(arg.draw_indexed.index_count_per_instance - ib_count, command_data.zero_count);\n";
        source << "if (command_data.zeros && ib_rest)\n";
        source << "rest.draw_indexed_primitives((primitive_type)command_data.primitive_type, ib_rest, "
                  "command_data.zeros, arg.draw_indexed.instance_count, arg.draw_indexed.base_vertex_location, "
                  "arg.draw_indexed.start_instance_location);\n";
        source << "}\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        source << "cmd.concurrent_dispatch_threadgroups(arg.dispatch, command_data.tgsize);\n";
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
        source << "if (any(uint3(arg.dispatch_mesh) == 0)) {\n";
        source << "} else if (command_data.draws)\n";
        source << "command_data.draws[i].threadgroups = arg.dispatch_mesh;\n";
        source << "else\n";
        source << "cmd.draw_mesh_threadgroups(uint3(arg.dispatch_mesh), uint3(command_data.object_threads), "
                  "uint3(command_data.mesh_threads));\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
        auto slot = arg.VertexBuffer.Slot;
        ResetVertexBuffers |= 1u << slot;
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
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots)
          return E_INVALIDARG;
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
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots)
          return E_INVALIDARG;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2, 2);
        source << "rootsig_qwords[" << offset << "] = arg.cb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
        auto parameter_index = arg.ShaderResourceView.RootParameterIndex;
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots)
          return E_INVALIDARG;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2, 2);
        source << "rootsig_qwords[" << offset << "] = arg.srv_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        auto parameter_index = arg.UnorderedAccessView.RootParameterIndex;
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots)
          return E_INVALIDARG;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        ResetRootDwords.emplace_back(offset * 2, 2);
        source << "rootsig_qwords[" << offset << "] = arg.uav_" << i << ";\n";
        break;
      }
      default:
        return E_INVALIDARG;
      }
    }

    source << "}\n"
              "};\n";

    WMT::Reference<WMT::Error> err;
    auto lib = device_->GetMTLDevice().newLibraryWithSource(source.view(), err);

    if (!lib) {
      ERR("Failed to compile command signature resolve shader: ", err.description().getUTF8String());
      return E_FAIL;
    }

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
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12CommandSignature: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
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

} // namespace dxmt