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

#include "Metal.hpp"
#include "airconv_public.h"
#include "tessellation_limits.hpp"
#include "config/config.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_shader.hpp"
#include "log/log.hpp"

namespace mullion {

class MTLCompiledTessellationPipelineImpl: public MTLCompiledTessellationMeshPipeline {
public:
  MTLCompiledTessellationPipelineImpl(MTLD3D11Device *pDevice,
                                  const MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount), desc(*pDesc) {
    if (pDesc->RasterizationEnabled && !pDesc->PixelShader)
      num_rtvs = 0;
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(
                                    pDesc->ColorAttachmentFormats[i]))
                                << i);
    }

    hull_reflection = pDesc->HullShader->reflection();
    auto &domain_reflection = pDesc->DomainShader->reflection();
    // what a vertex of the domain shader's takes of a mesh, and the largest factor, which only the configuration
    // lowers
    mesh_vertex_size = domain_reflection.PostTessellator.MeshVertexSize;
    max_potential_factor = UINT32_MAX;
    if (uint32_t tess_factor_config = Config::getInstance().getOption<int32_t>("d3d11.maxTessFactor", 0u);
        tess_factor_config >= 4 && tess_factor_config <= 64)
      max_potential_factor = tess_factor_config;

    // Metal's tessellator, where it tessellates as Direct3D does: a triangle or quad domain to triangles, partitioned to
    // whole factors, which pow2 is once its hull shader rounds them (D3D11.3 11.7.7.1). its hull kernel runs before
    // the pass, so its stages write no UAVs; a geometry shader, clip distances the pixel shader tests and cull
    // distances, which a vertex function has none of, keep the mesh pipeline, which also takes the indirect draws and
    // those under pipeline statistics. d3d11.nativeTessellation = False keeps it for every draw
    auto partitioning = microsoft::D3D11_SB_TESSELLATOR_PARTITIONING(hull_reflection.Tessellator.Partition);
    auto domain = microsoft::D3D11_SB_TESSELLATOR_DOMAIN(hull_reflection.Tessellator.Domain);
    output = hull_reflection.Tessellator.OutputPrimitive;
    static const bool native_enabled = Config::getInstance().getOption<bool>("d3d11.nativeTessellation", true);
    native = native_enabled && !pDesc->GeometryShader && !pDesc->SOLayout && pDesc->RasterizationEnabled &&
             !pDesc->ClipDistances && !pDesc->PipelineStatistics && !domain_reflection.CullDistances &&
             !pDesc->VertexShader->reflection().UAVSlotMask && !hull_reflection.UAVSlotMask &&
             (domain == microsoft::D3D11_SB_TESSELLATOR_DOMAIN_TRI ||
              domain == microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD) &&
             (partitioning == microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER ||
              partitioning == microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2) &&
             (output == MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW || output == MTL_TESSELLATOR_TRIANGLE_CCW);
    quad = domain == microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD;

    GetTessellationShaders();
    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_pixel_shader(ShaderVariantPixel{
          pDesc->SampleMask, pDesc->BlendState->IsDualSourceBlending(),
          depth_stencil_format == WMTPixelFormatInvalid,
          unorm_output_reg_mask, pDesc->PipelineStatistics, pDesc->BlendState->IsAlphaToCoverage(),
          pDesc->CenteredSamples, pDesc->SingleRasterSample, pDesc->PairedSamples, pDesc->ClipDistances},
          {rtv_formats, num_rtvs}, pDesc->BlendState, pDesc->PixelMinMaxFiltering);
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
    }
  }

  // the vertex-hull and domain shaders, for what the device says a mesh grid may have, and how many object
  // threadgroups then share a patch group's mesh threadgroups
  void GetTessellationShaders() {
    max_mesh_threadgroups = device_->GetMullionDevice().maxMeshThreadgroups();
    auto partitioning = microsoft::D3D11_SB_TESSELLATOR_PARTITIONING(hull_reflection.Tessellator.Partition);
    auto domain = microsoft::D3D11_SB_TESSELLATOR_DOMAIN(hull_reflection.Tessellator.Domain);
    auto factor = dxbc::get_final_factor(hull_reflection.Tessellator.MaxFactor, partitioning, max_potential_factor).second;
    parts = dxbc::tessellation_grids(
                dxbc::get_max_potential_workload_count(factor, domain, partitioning) * (32 / hull_reflection.ThreadsPerPatch),
                // a geometry shader runs in the mesh stage after the domain shader, a threadgroup for each primitive
                desc.GeometryShader
                    ? dxbc::get_max_workload_vertices(factor, domain)
                    : dxbc::tessellation_pieces(
                          factor, domain,
                          microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE(hull_reflection.Tessellator.OutputPrimitive),
                          mesh_vertex_size
                      ).count,
                desc.GeometryShader ? desc.GeometryShader->reflection().GeometryShader.InstanceCount : 1,
                max_mesh_threadgroups
    ).parts;
    VertexHullShader = desc.HullShader->get_shader(ShaderVariantTessellationVertexHull{
        desc.InputLayout, desc.VertexShader, desc.IndexBufferFormat, max_potential_factor, mesh_vertex_size,
        desc.GeometryShader, max_mesh_threadgroups});
    DomainShader = desc.DomainShader->get_shader(ShaderVariantTessellationDomain{
        desc.HullShader, max_potential_factor, mesh_vertex_size, !desc.RasterizationEnabled, desc.GeometryShader,
        max_mesh_threadgroups, desc.ClipDistances != 0});
    if (!native)
      return;
    HullKernel = desc.HullShader->get_shader(ShaderVariantTessellationVertexHull{
        desc.InputLayout, desc.VertexShader, desc.IndexBufferFormat, max_potential_factor, mesh_vertex_size, nullptr,
        max_mesh_threadgroups, true});
    PatchShader = desc.DomainShader->get_shader(ShaderVariantTessellationDomain{
        desc.HullShader, max_potential_factor, mesh_vertex_size, false, nullptr, max_mesh_threadgroups, false, true});
  }

  void GetPipeline(MTL_COMPILED_TESSELLATION_MESH_PIPELINE *pPipeline, bool statistics) final {
    if (statistics && description->PixelShader && !description->PipelineStatistics) {
      auto desc = *description;
      desc.PipelineStatistics = true;
      MTLCompiledTessellationMeshPipeline *pipeline;
      device_->CreateTessellationMeshPipeline(&desc, &pipeline);
      return pipeline->GetPipeline(pPipeline);
    }
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_rasterization_, hull_reflection.NumOutputElement,
                  hull_reflection.ThreadsPerPatch, parts, state_hull_, state_patches_,
                  hull_reflection.Tessellator.PatchGroupSize, quad ? 12u : 8u};
  }

  ThreadpoolWork *RunThreadpoolWork() {

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vhs, ds, ps;

    if (!VertexHullShader->GetShader(&vhs)) {
      return VertexHullShader;
    }
    if (!vhs.Function) {
      ERR("Failed to create tess-mesh PSO: Invalid vertex-hull shader.");
      return this;
    }
    if (!DomainShader->GetShader(&ds)) {
      return DomainShader;
    }
    if (!ds.Function) {
      ERR("Failed to create tess-mesh PSO: Invalid domain shader.");
      return this;
    }
    if (PixelShader) {
      if (!PixelShader->GetShader(&ps)) {
        return PixelShader;
      }
      if (!ps.Function) {
        ERR("Failed to create mesh PSO: Invalid pixel shader.");
        return this;
      }
    }
    MTL_COMPILED_SHADER hull{}, patch{};
    if (native && !HullKernel->GetShader(&hull))
      return HullKernel;
    if (native && !PatchShader->GetShader(&patch))
      return PatchShader;

    WMTMeshRenderPipelineInfo info;
    WMT::InitializeMeshRenderPipelineInfo(info);

    info.object_function = vhs.Function;
    info.mesh_function = ds.Function;
    info.payload_memory_length = 0;

    // HACK for AMDGPU: always reserve all payload capability
    if (!device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7))
      info.payload_memory_length = 16384;

    info.immutable_object_buffers =
        (1 << 16) | (1 << 21) | (1 << 27) | (1 << 28) | (1 << 29) | (1 << 30);
    info.immutable_mesh_buffers = (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

    if (PixelShader) {
      info.fragment_function = ps.Function;
    }
    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor(
          (WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }
    // explicit shader coverage disables A2C (D3D11.3 17.18)
    if (description->PixelShader && description->PixelShader->reflection().PixelShader.HasCoverageOutput)
      info.alpha_to_coverage_enabled = false;

    info.raster_sample_count = SampleCount;
    // so far it's a fixed constant
    info.mesh_tgsize_is_multiple_of_sgwidth = true;
    // total threads is 32
    // FIXME: might be different on AMD GPU, if it's ever supported
    info.object_tgsize_is_multiple_of_sgwidth = true;

    state_rasterization_ = device_->GetMTLDevice().newRenderPipelineState(info, err);
    if (state_rasterization_ == nullptr) {
      ERR("Failed to create tessellation raster PSO: ",
          err.description().getUTF8String());
      return this;
    }
    if (native)
      CreateTessellator(hull, patch);

    return this;
  }

  // Metal's tessellator: the hull kernel, and the pipeline of the patches it writes, whose vertex function is the
  // domain shader. one Metal does not make leaves the draws on the mesh pipeline
  void CreateTessellator(const MTL_COMPILED_SHADER &hull, const MTL_COMPILED_SHADER &patch) {
    if (!hull.Function || !patch.Function) {
      ERR("Failed to create the tessellator: invalid hull kernel or domain shader");
      return;
    }
    WMT::Reference<WMT::Error> err;
    WMTComputePipelineInfo hull_info;
    WMT::InitializeComputePipelineInfo(hull_info);
    hull_info.compute_function = hull.Function;
    hull_info.tgsize_is_multiple_of_sgwidth = true;
    auto kernel = device_->GetMTLDevice().newComputePipelineState(hull_info, err);
    if (!kernel) {
      ERR("Failed to create the tessellator's hull kernel: ", err.description().getUTF8String());
      return;
    }

    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);
    info.vertex_function = patch.Function;
    MTL_COMPILED_SHADER ps;
    if (PixelShader && PixelShader->GetShader(&ps))
      info.fragment_function = ps.Function;
    info.rasterization_enabled = true;
    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }
    if (depth_stencil_format != WMTPixelFormatInvalid)
      info.depth_pixel_format = depth_stencil_format;
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2)
      info.stencil_pixel_format = depth_stencil_format;
    if (pBlendState)
      pBlendState->SetupMetalPipelineDescriptor((WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    // explicit shader coverage disables A2C (D3D11.3 17.18)
    if (description->PixelShader && description->PixelShader->reflection().PixelShader.HasCoverageOutput)
      info.alpha_to_coverage_enabled = false;
    info.raster_sample_count = SampleCount;
    info.immutable_vertex_buffers = (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    // the hull kernel rounds pow2's factors to whole ones, and writes each instance's after the instance before
    info.tessellation_partition_mode = WMTTessellationPartitionModeInteger;
    info.tessellation_factor_step = WMTTessellationFactorStepFunctionPerPatchAndPerInstance;
    // Metal turns its triangles the other way round the domain's points from Direct3D's: the reference's clockwise
    // triangles are Metal's counterclockwise ones (d3d11_tessellator, culling back faces)
    info.tessellation_output_winding_order =
        output == MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW ? WMTWindingCounterClockwise : WMTWindingClockwise;
    // the most Direct3D's tessellator takes (D3D11_TESSELLATOR_MAX_TESSELLATION_FACTOR)
    info.max_tessellation_factor = 64;
    auto patches = device_->GetMTLDevice().newRenderPipelineState(info, err);
    if (!patches) {
      ERR("Failed to create the tessellator's pipeline: ", err.description().getUTF8String());
      return;
    }
    state_hull_ = std::move(kernel);
    state_patches_ = std::move(patches);
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  UINT num_rtvs;
  UINT ps_valid_render_targets;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_rasterization_;
  // Metal's tessellator (CreateTessellator)
  WMT::Reference<WMT::ComputePipelineState> state_hull_;
  WMT::Reference<WMT::RenderPipelineState> state_patches_;
  bool RasterizationEnabled;
  UINT SampleCount;

  MTL_SHADER_REFLECTION hull_reflection;

  MTL_GRAPHICS_PIPELINE_DESC desc;
  uint32_t max_potential_factor;
  uint32_t mesh_vertex_size;
  uint32_t max_mesh_threadgroups;
  uint32_t parts;
  CompiledShader *VertexHullShader;
  CompiledShader *PixelShader;
  CompiledShader *DomainShader;
  // whether the pipeline has Metal's tessellator, and its hull kernel and domain shader
  bool native = false;
  bool quad = false;
  MTL_TESSELLATOR_OUTPUT_PRIMITIVE output;
  CompiledShader *HullKernel = nullptr;
  CompiledShader *PatchShader = nullptr;
};

std::unique_ptr<MTLCompiledTessellationMeshPipeline>
CreateTessellationMeshPipeline(MTLD3D11Device *pDevice,
                               MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledTessellationPipelineImpl>(pDevice, pDesc);
}

} // namespace mullion