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

#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "dxmt_format.hpp"
#include "dxmt_stream_output.hpp"
#include "tessellation_limits.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "util_env.hpp"
#include <span>
#include <fstream>

namespace dxmt {

constexpr WMTCompareFunction kCompareFunctionMap[] = {
    WMTCompareFunctionNever, // padding 0
    WMTCompareFunctionNever, // 1 - 1
    WMTCompareFunctionLess,    WMTCompareFunctionEqual,    WMTCompareFunctionLessEqual,
    WMTCompareFunctionGreater, WMTCompareFunctionNotEqual, WMTCompareFunctionGreaterEqual,
    WMTCompareFunctionAlways // 8 - 1
};

constexpr WMTStencilOperation kStencilOperationMap[] = {
    WMTStencilOperationZero, // invalid
    WMTStencilOperationKeep,
    WMTStencilOperationZero,
    WMTStencilOperationReplace,
    // D3D11_STENCIL_OP_INCR_SAT: Increment the stencil value by 1, and clamp
    // the result.
    WMTStencilOperationIncrementClamp,
    WMTStencilOperationDecrementClamp,
    WMTStencilOperationInvert,
    // D3D11_STENCIL_OP_INCR:Increment the stencil value by 1, and wrap the
    // result if necessary.

    WMTStencilOperationIncrementWrap,
    WMTStencilOperationDecrementWrap,

};

constexpr WMTBlendOperation kBlendOpMap[] = {
    WMTBlendOperationAdd, // padding 0
    WMTBlendOperationAdd, WMTBlendOperationSubtract, WMTBlendOperationReverseSubtract,
    WMTBlendOperationMin, WMTBlendOperationMax,
};

constexpr WMTLogicOperation kLogicOpMap[] = {
    WMTLogicOperationClear,        WMTLogicOperationSet,         WMTLogicOperationCopy,
    WMTLogicOperationCopyInverted, WMTLogicOperationNoOp,        WMTLogicOperationInvert,
    WMTLogicOperationAnd,          WMTLogicOperationNand,        WMTLogicOperationOr,
    WMTLogicOperationNor,          WMTLogicOperationXor,         WMTLogicOperationEquiv,
    WMTLogicOperationAndReverse,   WMTLogicOperationAndInverted, WMTLogicOperationOrReverse,
    WMTLogicOperationOrInverted,
};

constexpr WMTBlendFactor kBlendFactorMap[] = {
    WMTBlendFactorZero, // padding 0
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,       // invalid,12
    WMTBlendFactorZero,       // invalid,13
    WMTBlendFactorBlendColor, // BLEND_FACTOR
    WMTBlendFactorOneMinusBlendColor,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
    WMTBlendFactorBlendAlpha,
    WMTBlendFactorOneMinusBlendAlpha,
};

constexpr WMTBlendFactor kBlendAlphaFactorMap[] = {
    WMTBlendFactorZero, // padding 0
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,       // invalid,12
    WMTBlendFactorZero,       // invalid,13
    WMTBlendFactorBlendAlpha, // BLEND_FACTOR
    WMTBlendFactorOneMinusBlendAlpha,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
};

constexpr WMTColorWriteMask kColorWriteMaskMap[] = {
    // 0000
    WMTColorWriteMaskNone,
    // 0001
    WMTColorWriteMaskRed,
    // 0010
    WMTColorWriteMaskGreen,
    // 0011,
    WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    // 0100
    WMTColorWriteMaskBlue,
    // 0101
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    // 0110
    WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    // 0111
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,

    // 1000
    WMTColorWriteMaskAlpha,
    // 1001
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed,
    // 1010
    WMTColorWriteMaskAlpha | WMTColorWriteMaskGreen,
    // 1011,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    // 1100
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue,
    // 0101
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    // 1110
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    // 1111
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
};

HRESULT
ExtractMTLInputLayoutElements(
    MTLD3D12Device *device, const void *pShaderBytecodeWithInputSignature,
    const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs, uint32_t NumElements, SM50_IA_INPUT_ELEMENT *pInputLayout,
    uint32_t *pNumElementsOut
) {

  using namespace microsoft;
  uint16_t append_offset[32] = {0};
  uint32_t register_mask = 0;

  CSignatureParser parser;
  HRESULT hr = DXBCGetInputSignature(pShaderBytecodeWithInputSignature, &parser);
  if (FAILED(hr)) {
    return hr;
  }
  const D3D11_SIGNATURE_PARAMETER *pParameters;
  auto num_parameters = parser.GetParameters(&pParameters);

  UINT attribute_count = 0;
  for (UINT j = 0; j < NumElements; j++) {
    auto &desc = pInputElementDescs[j];

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, metal_format))) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_FAIL;
    }

    if (!metal_format.AttributeFormat) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_INVALIDARG;
    }
    if (!metal_format.BytesPerTexel) {
      ERR("CreateInputLayout: not an ordinary or packed format: ", desc.Format);
      return E_INVALIDARG;
    }
    auto aligned_byte_offset = desc.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
                                   ? align(append_offset[desc.InputSlot], std::min(4u, metal_format.BytesPerTexel))
                                   : desc.AlignedByteOffset;
    append_offset[desc.InputSlot] = aligned_byte_offset + metal_format.BytesPerTexel;

    auto pSig = std::find_if(pParameters, pParameters + num_parameters, [&](const D3D11_SIGNATURE_PARAMETER &inputSig) {
      return desc.SemanticIndex == inputSig.SemanticIndex && strcasecmp(desc.SemanticName, inputSig.SemanticName) == 0;
    });
    if (pSig == pParameters + num_parameters)
      continue; // shader has no such input register, so skip it
    auto &inputSig = *pSig;
    auto &attribute = pInputLayout[attribute_count++];

    attribute.format = metal_format.AttributeFormat;

    attribute.slot = desc.InputSlot;
    attribute.reg = inputSig.Register;
    attribute.aligned_byte_offset = aligned_byte_offset;
    // the layout stride is provided in IASetVertexBuffer
    attribute.step_function = desc.InputSlotClass;
    attribute.step_rate =
        desc.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? desc.InstanceDataStepRate : 1;
    register_mask |= (1 << inputSig.Register);
  }
  for (UINT i = 0; i < num_parameters; i++) {
    auto &inputSig = pParameters[i];
    if (inputSig.SystemValue != D3D10_SB_NAME_UNDEFINED)
      continue; // ignore SIV & SGV
    if (!(register_mask & (1 << inputSig.Register))) {
      WARN(
          "CreateInputLayout: Vertex shader expects ", inputSig.SemanticName, "_", inputSig.SemanticIndex,
          " but it's not in input layout element descriptors"
      );
      return E_INVALIDARG;
    }
  }
  *pNumElementsOut = attribute_count;

  return S_OK;
}

void
MTLD3D12PipelineState::DumpShaders(std::initializer_list<D3D12_SHADER_BYTECODE> Shaders) {
  static const auto dir = env::getEnvVar("DXMT_SHADER_DUMP_PATH");
  if (dir.empty())
    return;
  for (auto &Shader : Shaders) {
    if (!Shader.pShaderBytecode)
      continue;
    auto path = dir + "\\" + Sha1HashState::compute(Shader.pShaderBytecode, Shader.BytecodeLength).string() + ".dxbc";
    std::ofstream(path, std::ios::binary).write((const char *)Shader.pShaderBytecode, Shader.BytecodeLength);
    ERR("Dumped shader ", path);
  }
}

HRESULT
MTLD3D12PipelineState::InitializeShader(
    D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl
) {
  using namespace microsoft;
  HRESULT hr;
  CDXBCParser Parser;
  if (FAILED(hr = Parser.ReadDXBC(Bytecode.pShaderBytecode, Bytecode.BytecodeLength)))
    return hr;

  SM50Error error;
  if (SM50Initialize(Bytecode.pShaderBytecode, Bytecode.BytecodeLength, ppShader, pRefl, &error)) {
    ERR("Failed to initialize shader: ", SM50GetErrorMessageString(error));
    DumpShaders({Bytecode});
    return E_FAIL;
  }

  return S_OK;
}

class MTLD3D12GraphicsPipelineStateImpl : public MTLD3D12Pageable<MTLD3D12GraphicsPipelineState> {
protected:
  MTL_SHADER_REFLECTION ref_vs;
  MTL_SHADER_REFLECTION ref_ps;

  WMT::Reference<WMT::DepthStencilState> dsso;
  WMT::Reference<WMT::DepthStencilState> dsso_depth_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_stencil_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_no_stencil;
  WMT::Reference<WMT::DepthStencilState> dsso_readonly_no_stencil;

  // what a geometry pipeline's Metal pipelines are made of. a pipeline without a geometry shader keeps it, for the
  // topologies with adjacency a draw may bring: Metal has none, D3D12_PRIMITIVE_TOPOLOGY_TYPE does not tell them
  // from the others, and only a mesh stage can leave the adjacent vertices out (D3D11.3 8.15)
  struct GeometryInputs {
    std::string vs;                          // bytecode
    Com<ID3D12RootSignature> root_signature; // none when the bytecode embeds it
    std::vector<SM50_IA_INPUT_ELEMENT> elements;
    SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout;
    std::vector<SM50_STREAM_OUTPUT_ELEMENT2> so_elements;
    SM50_SHADER_STREAM_OUTPUT_DATA so;
    bool stream_output;
    SM50_SHADER_COMMON_DATA common;
    WMTMeshRenderPipelineInfo info;
    WMT::Reference<WMT::Function> ps;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut;
    uint32_t primitive; // D3D10_SB_PRIMITIVE of the topology type
  };
  std::unique_ptr<GeometryInputs> adjacency_inputs_;
  dxmt::mutex adjacency_mutex_;
  // by stream output's counting pass and strip
  WMT::Reference<WMT::RenderPipelineState> adjacency_[2][2];

  // every stage binds the pipeline's root signature, or the one its own bytecode embeds
  static SM50_SHADER_COMPILATION_ARGUMENT_DATA *
  RootSignature(
      SM50_SHADER_ROOT_SIGNATURE_DATA &rootsig, ID3D12RootSignature *pRootSignature, D3D12_SHADER_BYTECODE Bytecode,
      void *next
  ) {
    rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
    if (pRootSignature) {
      rootsig.bytecode_length = static_cast<MTLD3D12RootSignature *>(pRootSignature)->GetBlob(&rootsig.bytecode);
    } else {
      rootsig.bytecode = Bytecode.pShaderBytecode;
      rootsig.bytecode_length = Bytecode.BytecodeLength;
    }
    rootsig.next = next;
    return (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig;
  }

  // the Metal function of an airconv compilation, which it frees
  WMT::Reference<WMT::Function>
  Function(
      int status, sm50_bitcode_t bitcode, sm50_error_t sm50_err, const char *name,
      std::initializer_list<D3D12_SHADER_BYTECODE> Shaders
  ) {
    WMT::Reference<WMT::Function> func;
    WMT::Reference<WMT::Error> err;
    if (status) {
      ERR("Failed to compile ", name, ": ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      DumpShaders(Shaders);
      return func;
    }
    SM50_COMPILED_BITCODE compiled;
    SM50GetCompiledBitcode(bitcode, &compiled);
    auto library = device_->GetMTLDevice().newLibrary(WMT::MakeDispatchData(compiled.Data, compiled.Size), err);
    if (!library || !(func = library.newFunction(name)))
      ERR("Failed to load ", name, ": ", library ? "no such function" : err.description().getUTF8String());
    SM50DestroyBitcode(bitcode);
    return func;
  }

  // a geometry pipeline's Metal pipeline for lists or strips of `primitive` (D3D10_SB_PRIMITIVE): the vertex shader
  // in the object stage, reading indices through the index buffer view each draw binds, and in the mesh stage the
  // geometry shader, or without one what passes the primitives on to the rasterizer (`pass_through`), or nothing.
  // stream output's counting pass draws nothing
  WMT::Reference<WMT::RenderPipelineState>
  GeometryVariant(
      GeometryInputs &in, sm50_shader_t shader_vs, sm50_shader_t shader_gs, D3D12_SHADER_BYTECODE GS, bool strip,
      bool counting, uint32_t primitive, bool pass_through
  ) {
    WMT::Reference<WMT::RenderPipelineState> variant;
    WMT::Reference<WMT::Error> err;
    D3D12_SHADER_BYTECODE VS{in.vs.data(), in.vs.size()};
    SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso_gs{};
    pso_gs.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
    pso_gs.strip_topology = strip;
    pso_gs.strip_cut = in.strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pso_gs.strip_cut_index = in.strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF ? 0xffff : ~0u;
    pso_gs.input_primitive = primitive;
    pso_gs.pass_through = pass_through && !counting;
    pso_gs.next = &in.common;
    in.so.counting = counting;
    in.so.elements = in.so_elements.data();
    in.so.next = &in.common;
    if (in.stream_output)
      pso_gs.next = &in.so;
    in.ia_layout.elements = in.elements.data();
    in.ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_VIEW;
    in.ia_layout.next = &pso_gs;
    SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_vs, rootsig_gs;
    sm50_bitcode_t bitcode = nullptr;
    sm50_error_t sm50_err = nullptr;
    int status = SM50CompileGeometryPipelineVertex(
        shader_vs, shader_gs, RootSignature(rootsig_vs, in.root_signature.ptr(), VS, &in.ia_layout), "vsgs_main",
        &bitcode, &sm50_err
    );
    auto vs_func = Function(status, bitcode, sm50_err, "vsgs_main", {VS, GS});
    if (!vs_func)
      return variant;
    status = SM50CompileGeometryPipelineGeometry(
        shader_vs, shader_gs, RootSignature(rootsig_gs, in.root_signature.ptr(), shader_gs ? GS : VS, &pso_gs),
        "gs_main", &bitcode, &sm50_err
    );
    auto gs_func = Function(status, bitcode, sm50_err, "gs_main", {VS, GS});
    if (!gs_func)
      return variant;
    auto info = in.info;
    info.object_function = vs_func.handle;
    info.mesh_function = gs_func.handle;
    bool rasterized = (shader_gs || pass_through) && !counting && (!in.stream_output || in.so.rasterized_stream != ~0u);
    info.fragment_function = rasterized ? in.ps.handle : obj_handle_t{};
    info.rasterization_enabled = rasterized;
    info.payload_memory_length = SM50_GEOMETRY_PAYLOAD_SIZE;
    if (!(variant = device_->GetMTLDevice().newRenderPipelineState(info, err))) {
      ERR("Failed to create geometry PSO: ", err.description().getUTF8String());
      DumpShaders({VS, GS});
    }
    return variant;
  }

public:
  MTLD3D12GraphicsPipelineStateImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12GraphicsPipelineState>(pDevice) {
    IsComputePipelineState = FALSE;
  }

  // made when first drawn
  WMT::RenderPipelineState
  AdjacencyPipeline(bool strip, bool counting) final {
    std::lock_guard<dxmt::mutex> lock(adjacency_mutex_);
    auto &variant = adjacency_[counting][strip];
    if (!variant && adjacency_inputs_) {
      auto &in = *adjacency_inputs_;
      SM50Shader shader_vs;
      MTL_SHADER_REFLECTION reflection;
      if (SUCCEEDED(InitializeShader({in.vs.data(), in.vs.size()}, &shader_vs, &reflection)))
        variant = GeometryVariant(
            in, shader_vs, {}, {}, strip, counting,
            in.primitive == D3D_PRIMITIVE_LINE ? D3D_PRIMITIVE_LINE_ADJ : D3D_PRIMITIVE_TRIANGLE_ADJ, true
        );
    }
    return variant;
  }

  bool
  BlendFactorIsDualSource(D3D12_BLEND Blend) {
    return (Blend >= D3D12_BLEND_SRC1_COLOR) && (Blend <= D3D12_BLEND_INV_SRC1_ALPHA);
  }

  template <typename RenderPipelineInfo>
  HRESULT
  InitializePSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, RenderPipelineInfo &info, bool &dual_source_blending) {
    HRESULT hr;
    MTL_DXGI_FORMAT_DESC format_desc;
    uint32_t effective_dual_source_rtvs = 0;
    for (unsigned i = 0; i < pDesc->NumRenderTargets; i++) {
      if (pDesc->RTVFormats[i] == DXGI_FORMAT_UNKNOWN)
        continue;
      if (i >= 2 && dual_source_blending)
        break;
      auto &rt = info.colors[i];
      auto Format = pDesc->RTVFormats[i];
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc))) {
        return hr;
      }
      rt.pixel_format = format_desc.PixelFormat;

      auto renderTarget = pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0];

      if (renderTarget.BlendEnable && renderTarget.LogicOpEnable)
        return E_INVALIDARG;

      if (pDesc->BlendState.IndependentBlendEnable && renderTarget.LogicOpEnable)
        return E_INVALIDARG;

      rt.write_mask = kColorWriteMaskMap[renderTarget.RenderTargetWriteMask];

      if (rt.pixel_format == WMTPixelFormatRGB9E5Float)
        rt.write_mask = (rt.write_mask & ~WMTColorWriteMaskAlpha) ? WMTColorWriteMaskAll : 0;

      if (renderTarget.BlendEnable) {
        if (!any_bit_set(device_->GetMTLPixelFormatCapability(rt.pixel_format) & FormatCapability::Blend)) {
          WARN("CreateGraphicsPipelineState: pixel format ", rt.pixel_format, " is not blendable");
          return E_INVALIDARG;
        }
        if (BlendFactorIsDualSource(renderTarget.SrcBlendAlpha) || BlendFactorIsDualSource(renderTarget.SrcBlend) ||
            BlendFactorIsDualSource(renderTarget.DestBlendAlpha) || BlendFactorIsDualSource(renderTarget.DestBlend)) {
          dual_source_blending = true;
        }
        rt.alpha_blend_operation = kBlendOpMap[renderTarget.BlendOpAlpha];
        rt.rgb_blend_operation = kBlendOpMap[renderTarget.BlendOp];
        rt.blending_enabled = renderTarget.BlendEnable;
        rt.src_alpha_blend_factor = kBlendAlphaFactorMap[renderTarget.SrcBlendAlpha];
        rt.src_rgb_blend_factor = kBlendFactorMap[renderTarget.SrcBlend];
        rt.dst_alpha_blend_factor = kBlendAlphaFactorMap[renderTarget.DestBlendAlpha];
        rt.dst_rgb_blend_factor = kBlendFactorMap[renderTarget.DestBlend];
      }
      if (i < 2)
        effective_dual_source_rtvs++;
    }

    if (dual_source_blending && effective_dual_source_rtvs > 1)
      return E_INVALIDARG;

    // one logic op, the first target's, for all: independent blending cannot carry one
    if (pDesc->BlendState.RenderTarget[0].LogicOpEnable) {
#ifdef DXMT_NO_PRIVATE_API
      return E_INVALIDARG;
#else
      info.logic_operation_enabled = true;
      info.logic_operation = kLogicOpMap[pDesc->BlendState.RenderTarget[0].LogicOp];
#endif
    }

    if (pDesc->DSVFormat != DXGI_FORMAT_UNKNOWN) {
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), pDesc->DSVFormat, format_desc))) {
        return hr;
      }
      auto dsv_flags = DepthStencilPlanarFlags(format_desc.PixelFormat);
      if (dsv_flags & 1)
        info.depth_pixel_format = format_desc.PixelFormat;
      if (dsv_flags & 2)
        info.stencil_pixel_format = format_desc.PixelFormat;
    }

    if constexpr (std::is_same_v<RenderPipelineInfo, WMTRenderPipelineInfo>) {
      switch (pDesc->PrimitiveTopologyType) {
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        info.input_primitive_topology = WMTPrimitiveTopologyClassPoint;
        break;
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        info.input_primitive_topology = WMTPrimitiveTopologyClassLine;
        break;
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH:
        info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
        break;
      default:
        break;
      }
    }

    info.raster_sample_count = pDesc->SampleDesc.Count;
    info.support_indirect_command_buffers = true;

    info.alpha_to_coverage_enabled = pDesc->BlendState.AlphaToCoverageEnable && !ref_ps.PixelShader.HasCoverageOutput;

    return S_OK;
  }

  void
  InitializeDSSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
    WMTDepthStencilInfo info;
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = false;
    info.front_stencil.enabled = false;
    info.back_stencil.enabled = false;
    if (pDesc->DepthStencilState.DepthEnable) {
      info.depth_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.DepthFunc];
      info.depth_write_enabled = pDesc->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    }

    if (pDesc->DepthStencilState.StencilEnable) {
      info.front_stencil.enabled = true;
      info.back_stencil.enabled = true;

      info.front_stencil.depth_stencil_pass_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilPassOp];
      info.front_stencil.stencil_fail_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilFailOp];
      info.front_stencil.depth_fail_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilDepthFailOp];
      info.front_stencil.stencil_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.FrontFace.StencilFunc];
      info.front_stencil.write_mask = pDesc->DepthStencilState.StencilWriteMask;
      info.front_stencil.read_mask = pDesc->DepthStencilState.StencilReadMask;

      info.back_stencil.depth_stencil_pass_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilPassOp];
      info.back_stencil.stencil_fail_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilFailOp];
      info.back_stencil.depth_fail_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilDepthFailOp];
      info.back_stencil.stencil_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.BackFace.StencilFunc];
      info.back_stencil.write_mask = pDesc->DepthStencilState.StencilWriteMask;
      info.back_stencil.read_mask = pDesc->DepthStencilState.StencilReadMask;
    }

    auto metal = device_->GetMTLDevice();
    dsso = metal.newDepthStencilState(info);
    {
      auto info_depth_readonly = info;
      info_depth_readonly.depth_write_enabled = false;
      dsso_depth_readonly = metal.newDepthStencilState(info_depth_readonly);
    }
    {
      auto info_stencil_readonly = info;
      info_stencil_readonly.back_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.back_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_stencil_readonly.back_stencil.depth_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.depth_fail_op = WMTStencilOperationKeep;
      dsso_stencil_readonly = metal.newDepthStencilState(info_stencil_readonly);
    }
    {
      auto info_readonly = info;
      info_readonly.depth_write_enabled = false;
      info_readonly.back_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_readonly.back_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_readonly.back_stencil.depth_fail_op = WMTStencilOperationKeep;
      info_readonly.front_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_readonly.front_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_readonly.front_stencil.depth_fail_op = WMTStencilOperationKeep;
      dsso_readonly = metal.newDepthStencilState(info_readonly);
    }
    {
      auto info_nostencil = info;
      info_nostencil.back_stencil.enabled = false;
      info_nostencil.front_stencil.enabled = false;
      dsso_no_stencil = metal.newDepthStencilState(info_nostencil);
    }
    {
      auto info_nostencil = info;
      info_nostencil.depth_write_enabled = false;
      info_nostencil.back_stencil.enabled = false;
      info_nostencil.front_stencil.enabled = false;
      dsso_readonly_no_stencil = metal.newDepthStencilState(info_nostencil);
    }
  }

  void
  InitializeRasterizerState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
    fill_mode =
        pDesc->RasterizerState.FillMode == D3D12_FILL_MODE_SOLID ? WMTTriangleFillModeFill : WMTTriangleFillModeLines;
    switch (pDesc->RasterizerState.CullMode) {
    case D3D12_CULL_MODE_BACK:
      cull_mode = WMTCullModeBack;
      break;
    case D3D12_CULL_MODE_FRONT:
      cull_mode = WMTCullModeFront;
      break;
    case D3D12_CULL_MODE_NONE:
      cull_mode = WMTCullModeNone;
      break;
    }
    depth_clip_mode = pDesc->RasterizerState.DepthClipEnable ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
    depth_bias = pDesc->RasterizerState.DepthBias;
    scole_scale = pDesc->RasterizerState.SlopeScaledDepthBias;
    depth_bias_clamp = pDesc->RasterizerState.DepthBiasClamp;
    winding = pDesc->RasterizerState.FrontCounterClockwise ? WMTWindingCounterClockwise : WMTWindingClockwise;
    forced_sample_count = pDesc->RasterizerState.ForcedSampleCount;
  }

  // AS and MS make a mesh shader pipeline: the amplification shader, if any, is Metal's object stage, the mesh shader
  // its mesh stage
  virtual HRESULT
  Initialize(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, D3D12_SHADER_BYTECODE AS = {}, D3D12_SHADER_BYTECODE MS = {}) {
    stream_output = pDesc->StreamOutput.NumEntries;
    mesh_shader = MS.pShaderBytecode;

    bool tessellation = pDesc->HS.pShaderBytecode;
    // stream output runs through the geometry pipeline, with or without a geometry shader; after a tessellator the
    // geometry shader runs in the tessellation pipeline's mesh stage
    geometry_shader = !tessellation && (pDesc->GS.pShaderBytecode || stream_output);
    if (tessellation && stream_output) {
      ERR("CreatePipelineState: stream output of a tessellated draw is not implemented yet");
      return E_NOTIMPL;
    }
    if (tessellation != !!pDesc->DS.pShaderBytecode)
      return E_INVALIDARG;

    if (!pDesc->VS.pShaderBytecode && !mesh_shader) {
      ERR("no vertex shader");
      return E_INVALIDARG;
    }

    HRESULT hr;

    SM50Shader shader_vs, shader_hs, shader_ds, shader_gs, shader_ps, shader_as, shader_ms;
    MTL_SHADER_REFLECTION ref_hs, ref_ds, ref_gs, ref_as, ref_ms;
    auto metal = device_->GetMTLDevice();
    WMT::Reference<WMT::Error> err;

    SM50_SHADER_COMMON_DATA common;
    common.flags = {};
    common.type = SM50_SHADER_COMMON;
    common.metal_version = SM50_SHADER_METAL_310;
    common.simd_width = device_->GetSIMDWidth();
    common.next = nullptr;

    auto root_signature = [&](SM50_SHADER_ROOT_SIGNATURE_DATA &rootsig, D3D12_SHADER_BYTECODE Bytecode, void *next) {
      return RootSignature(rootsig, pDesc->pRootSignature, Bytecode, next);
    };
    auto function = [&](int status, sm50_bitcode_t bitcode, sm50_error_t sm50_err, const char *name) {
      return Function(status, bitcode, sm50_err, name, {pDesc->VS, pDesc->HS, pDesc->DS, pDesc->PS});
    };

    if (mesh_shader) {
      if (FAILED(hr = InitializeShader(MS, &shader_ms, &ref_ms)) ||
          (AS.pShaderBytecode && FAILED(hr = InitializeShader(AS, &shader_as, &ref_as))))
        return hr;
      std::copy_n(ref_ms.ThreadgroupSize, 3, mesh_threads);
      if (AS.pShaderBytecode)
        std::copy_n(ref_as.ThreadgroupSize, 3, object_threads);
    } else if (FAILED(hr = InitializeShader(pDesc->VS, &shader_vs, &ref_vs)))
      return hr;
    if (tessellation && (FAILED(hr = InitializeShader(pDesc->HS, &shader_hs, &ref_hs)) ||
                         FAILED(hr = InitializeShader(pDesc->DS, &shader_ds, &ref_ds))))
      return hr;
    if (pDesc->GS.pShaderBytecode && FAILED(hr = InitializeShader(pDesc->GS, &shader_gs, &ref_gs)))
      return hr;
    // the stage before stream output and the rasterizer: the geometry shader, or the vertex shader
    auto last_stage = pDesc->GS.pShaderBytecode ? pDesc->GS : pDesc->VS;
    std::vector<SM50_STREAM_OUTPUT_ELEMENT2> so_elements;
    SM50_SHADER_STREAM_OUTPUT_DATA so_data{};
    if (stream_output) {
      if (FAILED(hr = ExtractStreamOutputElements(
              last_stage.pShaderBytecode, std::span(pDesc->StreamOutput.pSODeclaration, pDesc->StreamOutput.NumEntries),
              so_elements
          )))
        return hr;
      so_data.type = SM50_SHADER_STREAM_OUTPUT;
      so_data.num_elements = so_elements.size();
      so_data.elements = so_elements.data();
      for (uint32_t b = 0; b < std::min(pDesc->StreamOutput.NumStrides, 4u); b++)
        so_data.strides[b] = pDesc->StreamOutput.pBufferStrides[b];
      so_data.rasterized_stream = pDesc->StreamOutput.RasterizedStream == D3D12_SO_NO_RASTERIZED_STREAM
                                      ? ~0u
                                      : pDesc->StreamOutput.RasterizedStream;
      so_data.next = &common;
      gs_instances = sm50_shader_t(shader_gs) ? ref_gs.GeometryShader.InstanceCount : 1;
    }
    // a vertex shader's cull distances discard whole primitives (D3D11.3 15.4.2), which Metal can only by primitive:
    // the mesh stage passes the vertex shader's primitives on, less those
    vertex_registers = mesh_shader ? 0 : ref_vs.NumOutputElement;
    bool plain = !mesh_shader && !tessellation && !pDesc->GS.pShaderBytecode;
    bool pass_through = plain && ref_vs.CullDistances;
    geometry_shader |= pass_through;
    adjacency = plain && (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ||
                          pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    bool mesh = tessellation || geometry_shader || mesh_shader;
    // the alpha-to-coverage state reads the pixel shader's reflection
    if (pDesc->PS.pShaderBytecode && FAILED(hr = InitializeShader(pDesc->PS, &shader_ps, &ref_ps)))
      return hr;

    SM50_SHADER_IA_INPUT_LAYOUT_DATA data_ia_layout;
    data_ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
    std::vector<SM50_IA_INPUT_ELEMENT> elements(pDesc->InputLayout.NumElements);
    // a mesh shader has no input assembler
    if (mesh_shader)
      elements.clear();
    hr = mesh_shader ? S_OK
                     : ExtractMTLInputLayoutElements(
                           device_, pDesc->VS.pShaderBytecode, pDesc->InputLayout.pInputElementDescs,
                           pDesc->InputLayout.NumElements, elements.data(), &data_ia_layout.num_elements
                       );
    if (FAILED(hr))
      return hr;
    if (mesh_shader)
      data_ia_layout.num_elements = 0;
    elements.resize(data_ia_layout.num_elements);
    data_ia_layout.elements = elements.data();
    slot_mask = 0;
    for (auto &element : elements)
      slot_mask |= (1 << element.slot);
    data_ia_layout.slot_mask = slot_mask;

    WMT::Reference<WMT::Function> ps_func;
    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);
    WMTMeshRenderPipelineInfo mesh_info;
    WMT::InitializeMeshRenderPipelineInfo(mesh_info);
    bool dual_source_blending = false;
    if (FAILED(hr = mesh ? InitializePSO(pDesc, mesh_info, dual_source_blending)
                         : InitializePSO(pDesc, info, dual_source_blending)))
      return hr;

    if (pDesc->PS.pShaderBytecode) {
      auto sha1 = Sha1HashState::compute(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);

      std::string ps_name = "ps_main" + sha1.string().substr(0, 8);

      SM50_SHADER_PSO_PIXEL_SHADER_DATA data_ps;
      data_ps.dual_source_blending = dual_source_blending;
      data_ps.disable_depth_output = false;
      data_ps.unorm_output_reg_mask = 0;
      data_ps.sample_mask = pDesc->SampleMask;
      data_ps.type = SM50_SHADER_PSO_PIXEL_SHADER;
      data_ps.next = &common;

      memset(data_ps.pixel_formats, 0, sizeof(data_ps.pixel_formats));
      for (unsigned i = 0; i < pDesc->NumRenderTargets; i++) {
        data_ps.pixel_formats[i] =
            ORIGINAL_FORMAT(mesh ? mesh_info.colors[i].pixel_format : info.colors[i].pixel_format);
      }

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      sm50_bitcode_t bitcode = nullptr;
      sm50_error_t sm50_err = nullptr;
      int status = SM50Compile(shader_ps, root_signature(rootsig, pDesc->PS, &data_ps), ps_name.c_str(), &bitcode, &sm50_err);
      if (!(ps_func = function(status, bitcode, sm50_err, ps_name.c_str())))
        return E_FAIL;
    }

    auto geometry_inputs = [&] {
      auto in = std::make_unique<GeometryInputs>();
      in->vs.assign((const char *)pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
      in->root_signature = pDesc->pRootSignature;
      in->elements = elements;
      in->ia_layout = data_ia_layout;
      in->so_elements = so_elements;
      in->so = so_data;
      in->stream_output = stream_output;
      in->common = common;
      in->ps = ps_func;
      in->strip_cut = pDesc->IBStripCutValue;
      in->primitive = pDesc->PrimitiveTopologyType;
      WMT::InitializeMeshRenderPipelineInfo(in->info);
      bool dual_source = false;
      if (FAILED(InitializePSO(pDesc, in->info, dual_source)))
        in.reset();
      return in;
    };

    if (mesh_shader) {
      auto compile = [&](sm50_shader_t shader, D3D12_SHADER_BYTECODE code, void *args, const char *name) {
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
        sm50_bitcode_t bitcode = nullptr;
        sm50_error_t sm50_err = nullptr;
        int status = SM50Compile(shader, root_signature(rootsig, code, args), name, &bitcode, &sm50_err);
        return function(status, bitcode, sm50_err, name);
      };
      // the mesh shader names its outputs as the pixel shader's inputs
      SM50_SHADER_MESH_SHADER_DATA link{&common, SM50_SHADER_MESH_SHADER, pDesc->PS.pShaderBytecode};
      WMT::Reference<WMT::Function> as_func, ms_func = compile(shader_ms, MS, &link, "ms_main");
      if (!ms_func || (AS.pShaderBytecode && !(as_func = compile(shader_as, AS, &common, "as_main")))) {
        DumpShaders({AS, MS});
        return E_FAIL;
      }
      mesh_info.object_function = as_func.handle;
      mesh_info.mesh_function = ms_func.handle;
      mesh_info.fragment_function = ps_func.handle;
      mesh_info.rasterization_enabled = true;
      // the most a payload may be (D3D12 mesh shader specification, "Amplification shader output payload")
      mesh_info.payload_memory_length = 16384;
      if (!(pso = metal.newRenderPipelineState(mesh_info, err))) {
        ERR("Failed to create mesh shader PSO: ", err.description().getUTF8String());
        DumpShaders({AS, MS, pDesc->PS});
        return E_FAIL;
      }
    } else if (geometry_shader) {
      // a strip's primitives share vertices, so strips and lists each have their pipeline
      auto inputs = geometry_inputs();
      if (!inputs)
        return E_FAIL;
      // D3D12_PRIMITIVE_TOPOLOGY_TYPE's point, line and triangle are D3D10_SB_PRIMITIVE's
      auto variant = [&](bool strip, bool counting = false) {
        return GeometryVariant(
            *inputs, shader_vs, shader_gs, pDesc->GS, strip, counting, pDesc->PrimitiveTopologyType, pass_through
        );
      };
      bool strips = sm50_shader_t(shader_gs) ? ref_gs.GeometryShader.Primitive != D3D_PRIMITIVE_POINT
                              : pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      if (!(pso = variant(false)) || (strips && !(pso_strip = variant(true))))
        return E_FAIL;
      if (stream_output && (!(so_count = variant(false, true)) || (strips && !(so_count_strip = variant(true, true)))))
        return E_FAIL;
      if (adjacency)
        adjacency_inputs_ = std::move(inputs);
      // without a geometry shader, the vertex shader also rasterizes as usual, unless the mesh stage does
      if (stream_output && !sm50_shader_t(shader_gs) && !pass_through && so_data.rasterized_stream == 0) {
        if (FAILED(hr = InitializePSO(pDesc, info, dual_source_blending)))
          return hr;
        data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
        data_ia_layout.next = &common;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
        sm50_bitcode_t bitcode = nullptr;
        sm50_error_t sm50_err = nullptr;
        int status = SM50Compile(shader_vs, root_signature(rootsig, pDesc->VS, &data_ia_layout), "vs_main", &bitcode, &sm50_err);
        auto vs_func = function(status, bitcode, sm50_err, "vs_main");
        if (!vs_func)
          return E_FAIL;
        info.vertex_function = vs_func.handle;
        info.fragment_function = ps_func.handle;
        if (!(so_raster = metal.newRenderPipelineState(info, err))) {
          ERR("Failed to create PSO: ", err.description().getUTF8String());
          return E_FAIL;
        }
      }
    } else if (!tessellation) {
      data_ia_layout.next = &common;
      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      sm50_bitcode_t bitcode = nullptr;
      sm50_error_t sm50_err = nullptr;
      int status = SM50Compile(shader_vs, root_signature(rootsig, pDesc->VS, &data_ia_layout), "vs_main", &bitcode, &sm50_err);
      auto vs_func = function(status, bitcode, sm50_err, "vs_main");
      if (!vs_func)
        return E_FAIL;
      info.vertex_function = vs_func.handle;
      info.fragment_function = ps_func.handle;
      if (!(pso = metal.newRenderPipelineState(info, err))) {
        ERR("Failed to create PSO: ", err.description().getUTF8String());
        DumpShaders({pDesc->VS, pDesc->PS});
        return E_FAIL;
      }
      if (adjacency && !(adjacency_inputs_ = geometry_inputs()))
        return E_FAIL;
    } else {
      // every factor there is, and what a vertex of the domain shader's takes of a mesh
      SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess{};
      pso_tess.type = SM50_SHADER_PSO_TESSELLATOR;
      pso_tess.max_potential_tess_factor = D3D12_TESSELLATOR_MAX_TESSELLATION_FACTOR;
      pso_tess.mesh_vertex_size = ref_ds.PostTessellator.MeshVertexSize;
      pso_tess.geometry = shader_gs;
      pso_tess.next = &common;
      mesh_info.fragment_function = ps_func.handle;
      mesh_info.rasterization_enabled = true;
      // the vertex and hull stages run 32 threads per object threadgroup, the domain stage 32 per mesh threadgroup
      mesh_info.mesh_tgsize_is_multiple_of_sgwidth = true;
      mesh_info.object_tgsize_is_multiple_of_sgwidth = true;
      threads_per_patch = ref_hs.ThreadsPerPatch;

      auto build = [&]() -> HRESULT {
        // a geometry shader's primitives each take a mesh threadgroup; a grid has as many as the device lets it, and
        // the object threadgroups of several grids share a patch group's
        pso_tess.max_mesh_threadgroups = device_->max_mesh_threadgroups;
        auto partitioning = microsoft::D3D11_SB_TESSELLATOR_PARTITIONING(ref_hs.Tessellator.Partition);
        auto domain = microsoft::D3D11_SB_TESSELLATOR_DOMAIN(ref_hs.Tessellator.Domain);
        auto factor = dxbc::get_final_factor(ref_hs.Tessellator.MaxFactor, partitioning, pso_tess.max_potential_tess_factor).second;
        bool geometry = sm50_shader_t(shader_gs);
        tessellation_parts = dxbc::tessellation_grids(
                                 dxbc::get_max_potential_workload_count(factor, domain, partitioning) * (32 / threads_per_patch),
                                 geometry ? dxbc::get_max_workload_vertices(factor, domain)
                                          : dxbc::tessellation_pieces(
                                                factor, domain,
                                                microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE(ref_hs.Tessellator.OutputPrimitive),
                                                pso_tess.mesh_vertex_size
                                            ).count,
                                 geometry ? ref_gs.GeometryShader.InstanceCount : 1, pso_tess.max_mesh_threadgroups
        ).parts;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_ds, rootsig_gs;
        root_signature(rootsig_gs, pDesc->GS, &pso_tess);
        rootsig_gs.type = SM50_SHADER_ROOT_SIGNATURE2;
        sm50_bitcode_t bitcode = nullptr;
        sm50_error_t sm50_err = nullptr;
        int status = SM50CompileTessellationPipelineDomain(
            shader_hs, shader_ds, root_signature(rootsig_ds, pDesc->DS, &rootsig_gs), "ds_main", &bitcode, &sm50_err
        );
        auto ds_func = function(status, bitcode, sm50_err, "ds_main");
        if (!ds_func)
          return E_FAIL;
        mesh_info.mesh_function = ds_func.handle;

        // the object stage fetches indices itself, through the index buffer view each draw binds
        data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_VIEW;
        data_ia_layout.next = &pso_tess;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_hs, rootsig_vs;
        root_signature(rootsig_vs, pDesc->VS, &data_ia_layout);
        rootsig_vs.type = SM50_SHADER_ROOT_SIGNATURE2;
        status = SM50CompileTessellationPipelineHull(
            shader_vs, shader_hs, root_signature(rootsig_hs, pDesc->HS, &rootsig_vs), "vshs_main", &bitcode, &sm50_err
        );
        auto vshs_func = function(status, bitcode, sm50_err, "vshs_main");
        if (!vshs_func)
          return E_FAIL;
        mesh_info.object_function = vshs_func.handle;
        if (!(pso = metal.newRenderPipelineState(mesh_info, err))) {
          ERR("Failed to create tessellation PSO: ", err.description().getUTF8String());
          DumpShaders({pDesc->VS, pDesc->HS, pDesc->DS, pDesc->GS, pDesc->PS});
          return E_FAIL;
        }
        return S_OK;
      };
      if (FAILED(hr = build()))
        return hr;
      // only a pipeline tells what a grid may have: the device's first is built to ask, then built for it
      if (!pso_tess.max_mesh_threadgroups) {
        device_->max_mesh_threadgroups = std::min<uint64_t>(pso.maxTotalThreadgroupsPerMeshGrid(), UINT32_MAX);
        if (FAILED(hr = build()))
          return hr;
      }
    }

    // DSSO
    InitializeDSSO(pDesc);

    InitializeRasterizerState(pDesc);

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12GraphicsPipelineState: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) {
    return GetCachedPipelineBlob(blob);
  }

  virtual WMT::DepthStencilState
  GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) {
    if (!DSVPlanar)
      return device_->default_depth_stencil_state;

    if (DSVPlanar == 1) {
      if (DSVReadonlyFlags & 1)
        return dsso_readonly_no_stencil;
      else
        return dsso_no_stencil;
    }

    assert(DSVPlanar == 3);

    switch (DSVReadonlyFlags) {
    case 3:
      return dsso_readonly;
    case 2:
      return dsso_stencil_readonly;
    case 1:
      return dsso_depth_readonly;
    default:
      return dsso;
    }
  }
};

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState,
    bool depth_bounds, D3D12_SHADER_BYTECODE AS, D3D12_SHADER_BYTECODE MS
) {
  InitReturnPtr(ppPipelineState);
  if (HRESULT hr = CheckCachedPipeline(pDesc->CachedPSO); FAILED(hr))
    return hr;
  auto pso = Com(new MTLD3D12GraphicsPipelineStateImpl(pDevice));
  pso->depth_bounds = depth_bounds && pDevice->GetMTLDevice().supportsFamily(WMTGPUFamilyApple10);
  HRESULT hr = pso->Initialize(pDesc, AS, MS);
  if (FAILED(hr))
    return hr;
  return pso->QueryInterface(riid, ppPipelineState);
};

} // namespace dxmt