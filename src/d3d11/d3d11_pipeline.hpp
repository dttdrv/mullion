#pragma once

#include "Metal.hpp"
#include "d3d11_device.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_state_object.hpp"
#include "util_hash.hpp"

struct MTL_GRAPHICS_PIPELINE_DESC {
  ManagedShader VertexShader;
  ManagedShader HullShader;
  ManagedShader DomainShader;
  ManagedShader GeometryShader;
  ManagedShader PixelShader;
  IMTLD3D11BlendState *BlendState;
  ManagedInputLayout InputLayout;
  IMTLD3D11StreamOutputLayout *SOLayout;
  UINT NumColorAttachments;
  WMTPixelFormat ColorAttachmentFormats[8];
  WMTPixelFormat DepthStencilFormat;
  WMTPrimitiveTopologyClass TopologyClass;
  bool RasterizationEnabled;
  uint8_t SampleCount;
  bool GSStripTopology;
  SM50_INDEX_BUFFER_FORMAT IndexBufferFormat;
  uint32_t SampleMask;
  // the render targets' samples all lie at the pixel center (D3D11_CENTER_MULTISAMPLE_PATTERN)
  bool CenteredSamples;
  // one sample rasterizes for multisampled targets (ForcedSampleCount 1)
  bool SingleRasterSample;
  // each raster sample stands for two of D3D's (RasterSampleCount)
  bool PairedSamples;
  // the last pre-raster stage's clip distances, which the pixel shader tests (SM50_SHADER_PSO_PIXEL_SHADER_DATA)
  uint32_t ClipDistances;
  uint32_t GSPassthrough;
  // a geometry pipeline without a geometry shader: its input primitive (D3D10_SB_PRIMITIVE), and whether the mesh
  // stage passes the vertex shader's primitives on to the rasterizer; without that it only streams output
  uint32_t InputPrimitive;
  bool PassThrough;
  // the lines it passes on are quadrilaterals (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA)
  bool QuadLines;
  bool PipelineStatistics = false;
  bool PixelMinMaxFiltering = true;
};

struct MTL_COMPUTE_PIPELINE_DESC {
  ManagedShader ComputeShader;
};

struct MTL_COMPILED_GRAPHICS_PIPELINE {
  WMT::RenderPipelineState PipelineState;
  // with stream output: the pass that counts before PipelineState writes, and the geometry shader's instances
  WMT::RenderPipelineState StreamOutputCount;
  uint32_t GeometryInstances;
  // the mesh threadgroups an object threadgroup starts for each primitive: the geometry shader's instances, 1 when
  // the mesh stage only passes primitives on, 0 when it starts none (stream output alone)
  uint32_t MeshInstances;
};

struct MTL_COMPILED_COMPUTE_PIPELINE {
  WMT::ComputePipelineState PipelineState;
};

struct MTL_COMPILED_TESSELLATION_MESH_PIPELINE {
  WMT::RenderPipelineState PipelineState;
  uint32_t NumControlPointOutputElement;
  uint32_t ThreadsPerPatch;
  // the object threadgroups that share a patch group's mesh threadgroups (tessellation_parts)
  uint32_t Parts;
};

namespace std {
template <> struct hash<MTL_GRAPHICS_PIPELINE_DESC> {
  size_t operator()(const MTL_GRAPHICS_PIPELINE_DESC &v) const noexcept {
    mullion::HashState state;
    state.add((size_t)v.VertexShader);
    state.add((size_t)v.PixelShader);
    state.add((size_t)v.HullShader);
    state.add((size_t)v.DomainShader);
    state.add((size_t)v.GeometryShader);
    state.add((size_t)v.InputLayout);
    // the blending the pipeline takes of its state, which states made apart share (IMTLD3D11BlendState::PipelineHash)
    state.add(v.BlendState ? v.BlendState->PipelineHash(v.NumColorAttachments) : 0);
    state.add((size_t)v.DepthStencilFormat);
    state.add((size_t)v.TopologyClass);
    state.add((size_t)v.IndexBufferFormat);
    state.add((size_t)v.GSStripTopology);
    state.add((size_t)v.SampleMask);
    state.add(v.CenteredSamples);
    state.add(v.SingleRasterSample);
    state.add(v.PairedSamples);
    state.add(v.ClipDistances);
    state.add((size_t)v.GSPassthrough);
    state.add((size_t)v.SOLayout);
    state.add((size_t)v.InputPrimitive);
    state.add((size_t)v.PassThrough);
    state.add(v.QuadLines);
    state.add(v.PipelineStatistics);
    state.add(v.PixelMinMaxFiltering);
    state.add((size_t)v.SampleCount);
    state.add((size_t)v.NumColorAttachments);
    for (unsigned i = 0; i < v.NumColorAttachments; i++) {
      state.add(v.ColorAttachmentFormats[i]);
    }
    return state;
  };
};
template <> struct equal_to<MTL_GRAPHICS_PIPELINE_DESC> {
  bool operator()(const MTL_GRAPHICS_PIPELINE_DESC &x,
                  const MTL_GRAPHICS_PIPELINE_DESC &y) const {
    if (x.NumColorAttachments != y.NumColorAttachments)
      return false;
    for (unsigned i = 0; i < x.NumColorAttachments; i++) {
      if (x.ColorAttachmentFormats[i] != y.ColorAttachmentFormats[i])
        return false;
    }
    if (x.BlendState != y.BlendState &&
        (!x.BlendState || !y.BlendState || !x.BlendState->PipelineEqual(y.BlendState, x.NumColorAttachments)))
      return false;
    return (x.VertexShader == y.VertexShader) &&
           (x.PixelShader == y.PixelShader) && (x.HullShader == y.HullShader) &&
           (x.DomainShader == y.DomainShader) && 
           (x.GeometryShader == y.GeometryShader) &&
           (x.InputLayout == y.InputLayout) &&
           (x.DepthStencilFormat == y.DepthStencilFormat) &&
           (x.TopologyClass == y.TopologyClass) &&
           (x.RasterizationEnabled == y.RasterizationEnabled) &&
           (x.GSStripTopology == y.GSStripTopology) &&
           (x.SampleCount == y.SampleCount) &&
           (x.IndexBufferFormat == y.IndexBufferFormat) &&
           (x.SampleMask == y.SampleMask) && (x.CenteredSamples == y.CenteredSamples) &&
           (x.SingleRasterSample == y.SingleRasterSample) && (x.PairedSamples == y.PairedSamples) &&
           (x.ClipDistances == y.ClipDistances) &&
           (x.GSPassthrough == y.GSPassthrough) && (x.SOLayout == y.SOLayout) &&
           (x.InputPrimitive == y.InputPrimitive) && (x.PassThrough == y.PassThrough) && (x.QuadLines == y.QuadLines) &&
           (x.PipelineStatistics == y.PipelineStatistics) && (x.PixelMinMaxFiltering == y.PixelMinMaxFiltering);
  }
};
} // namespace std

namespace mullion {

class MTLCompiledGraphicsPipeline : public ThreadpoolWork {
public:
  /**
  NOTE: the current thread is blocked if it's not ready
   */
  const MTL_GRAPHICS_PIPELINE_DESC *description;
  virtual void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pGraphicsPipeline, bool statistics = false) = 0;
};

class MTLCompiledComputePipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pComputePipeline) = 0;
};

class MTLCompiledGeometryPipeline : public ThreadpoolWork {
public:
  const MTL_GRAPHICS_PIPELINE_DESC *description;
  virtual void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pGeometryPipeline, bool statistics = false) = 0;
};

class MTLCompiledTessellationMeshPipeline : public ThreadpoolWork {
public:
  const MTL_GRAPHICS_PIPELINE_DESC *description;
  virtual void GetPipeline(MTL_COMPILED_TESSELLATION_MESH_PIPELINE *pTessellationPipeline, bool statistics = false) = 0;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

std::unique_ptr<MTLCompiledComputePipeline> CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader);

std::unique_ptr<MTLCompiledGeometryPipeline>
CreateGeometryPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

std::unique_ptr<MTLCompiledTessellationMeshPipeline>
CreateTessellationMeshPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

}; // namespace mullion