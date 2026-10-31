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
#include "d3d12_pipeline.hpp"
#include "mullion_format.hpp"
#include "mullion_stream_output.hpp"
#include "mullion_shader_cache.hpp"
#include "tessellation_limits.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "util_env.hpp"
#include <span>
#include <fstream>
#include <mutex>
#include <array>
#include <unordered_set>
#include <deque>
#include <variant>

namespace mullion {

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
    WMTBlendFactorBlendAlpha, // ALPHA_FACTOR
    WMTBlendFactorOneMinusBlendAlpha,
};
static_assert(std::size(kBlendAlphaFactorMap) == std::size(kBlendFactorMap));

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
  // MULLION_TRACE_SHADER=heavy: into shaders/ next to the trace log
  static const auto dir = [] {
    auto given = env::getEnvVar("MULLION_SHADER_DUMP_PATH");
    if (!given.empty() || !trace::Heavy(trace::Shader) || trace::WindowsDirectory().empty())
      return given;
    auto shaders = trace::WindowsDirectory() + "/shaders";
    env::createDirectory(shaders);
    return shaders;
  }();
  if (dir.empty())
    return;
  // a shader shared by many pipelines is written once
  static std::mutex mutex;
  static std::unordered_set<std::string> written;
  for (auto &Shader : Shaders) {
    if (!Shader.pShaderBytecode)
      continue;
    auto hash = Sha1HashState::compute(Shader.pShaderBytecode, Shader.BytecodeLength).string();
    {
      std::lock_guard lock(mutex);
      if (!written.insert(hash).second)
        continue;
    }
    auto path = dir + "\\" + hash + ".dxbc";
    std::ofstream(path, std::ios::binary).write((const char *)Shader.pShaderBytecode, Shader.BytecodeLength);
    DEBUG("Dumped shader ", path);
  }
}

void
MTLD3D12PipelineState::Name(std::initializer_list<D3D12_SHADER_BYTECODE> Shaders) {
  for (auto &Shader : Shaders)
    if (Shader.pShaderBytecode)
      name += Sha1HashState::compute(Shader.pShaderBytecode, Shader.BytecodeLength).string() + " ";
  static const bool dump_all = env::getEnvVar("MULLION_SHADER_DUMP_ALL") == "1";
  if (dump_all || Logger::logLevel() == LogLevel::Trace || trace::Heavy(trace::Shader))
    DumpShaders(Shaders);
}

HRESULT
MTLD3D12PipelineState::InitializeShader(
    MTLD3D12Device *Device, D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl
) {
  using namespace microsoft;
  if (!ShaderContainerHolds(Bytecode.pShaderBytecode, Bytecode.BytecodeLength) ||
      !ShaderFeaturesSupported(Device, Bytecode.pShaderBytecode, Bytecode.BytecodeLength))
    return FAIL(E_INVALIDARG);

  SM50Error error;
  if (SM50Initialize(Bytecode.pShaderBytecode, Bytecode.BytecodeLength, ppShader, pRefl, &error)) {
    ERR(
        "Failed to initialize shader ", Sha1HashState::compute(Bytecode.pShaderBytecode, Bytecode.BytecodeLength).string(),
        ": ", SM50GetErrorMessageString(error)
    );
    DumpShaders({Bytecode});
    return FAIL(E_INVALIDARG);
  }

  return S_OK;
}

template <typename Desc>
static HRESULT
GetPipelineKey(const Desc &desc, Sha1Digest &key, bool depth_bounds = false,
               D3D12_SHADER_BYTECODE as = {}, D3D12_SHADER_BYTECODE ms = {},
               const D3D12_DEPTH_STENCIL_DESC2 *depth_stencil2 = nullptr) {
  Sha1HashState h;
  bool valid = true;
  auto hash = [&](const auto &...values) { (h.update(values), ...); };
  auto bytes = [&](const void *data, size_t size) {
    hash(uint64_t(size));
    if (size && !data)
      valid = false;
    else if (size)
      h.update(data, size);
  };
  const void *root = nullptr;
  UINT root_size = 0;
  if (desc.pRootSignature) {
    static_cast<MTLD3D12RootSignature *>(desc.pRootSignature)->GetBlob(&root);
    if (FAILED(microsoft::DXBCGetRootSignature(root, &root, &root_size)))
      return FAIL(E_INVALIDARG);
  }
  bytes(root, root_size);
  constexpr bool compute = std::is_same_v<Desc, D3D12_COMPUTE_PIPELINE_STATE_DESC>;
  hash(compute, desc.NodeMask, desc.Flags);
  if constexpr (compute) {
    auto &[signature, cs, node, cached, flags] = desc;
    bytes(cs.pShaderBytecode, cs.BytecodeLength);
  } else {
    // whole bindings make header additions visible, without putting padding or addresses in the key
    auto &[signature, vs, ps, ds, hs, gs, so, blend, mask, raster, depth, layout, cut, topology, count, formats,
           dsv, samples, node, cached, flags] = desc;
    if (count > std::size(formats) || layout.NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT ||
        (layout.NumElements && !layout.pInputElementDescs) || (so.NumEntries && !so.pSODeclaration) ||
        so.NumStrides > D3D12_SO_BUFFER_SLOT_COUNT || (so.NumStrides && !so.pBufferStrides))
      return FAIL(E_INVALIDARG);
    for (auto shader : {vs, ps, ds, hs, gs, as, ms})
      bytes(shader.pShaderBytecode, shader.BytecodeLength);
    hash(depth_bounds, so.NumEntries, so.NumStrides, so.RasterizedStream);
    for (UINT i = 0; i < so.NumEntries; i++) {
      auto &[stream, name, index, start, components, slot] = so.pSODeclaration[i];
      bytes(name, name ? strlen(name) + 1 : 0);
      hash(stream, index, start, components, slot);
    }
    bytes(so.pBufferStrides, so.NumStrides * sizeof(*so.pBufferStrides));
    auto &[coverage, independent, targets] = blend;
    hash(coverage, independent);
    for (auto &target : std::span(targets, count)) {
      auto &[enabled, logic, src, dst, op, src_alpha, dst_alpha, op_alpha, logic_op, write_mask] = target;
      hash(enabled, logic, src, dst, op, src_alpha, dst_alpha, op_alpha, logic_op, write_mask);
    }
    auto &[fill, cull, front, bias, clamp, slope, clip, multisample, aa, forced, conservative] = raster;
    hash(fill, cull, front, bias, clamp, slope, clip, multisample, aa, forced, conservative);
    auto &[enabled, write, function, stencil, read_mask, write_mask, front_face, back_face] = depth;
    hash(enabled, write, function, stencil, read_mask, write_mask);
    if (depth_stencil2 && (depth_stencil2->BackFace.StencilReadMask != read_mask ||
                           depth_stencil2->BackFace.StencilWriteMask != write_mask))
      hash(depth_stencil2->BackFace.StencilReadMask, depth_stencil2->BackFace.StencilWriteMask);
    for (auto &face : {front_face, back_face}) {
      auto &[fail, depth_fail, pass, compare] = face;
      hash(fail, depth_fail, pass, compare);
    }
    hash(layout.NumElements);
    for (UINT i = 0; i < layout.NumElements; i++) {
      auto &[name, index, format, slot, offset, classification, rate] = layout.pInputElementDescs[i];
      if (!name)
        return FAIL(E_INVALIDARG);
      bytes(name, strlen(name) + 1);
      hash(index, format, slot, offset, classification, rate);
    }
    auto &[sample_count, quality] = samples;
    hash(mask, cut, topology, count, dsv, sample_count, quality);
    bytes(formats, count * sizeof(*formats));
  }
  key = h.final();
  return valid ? S_OK : FAIL(E_INVALIDARG);
}

HRESULT
PipelineKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &Desc, Sha1Digest &Key) {
  return GetPipelineKey(Desc, Key);
}

HRESULT
PipelineKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &Desc, Sha1Digest &Key, bool DepthBounds,
            D3D12_SHADER_BYTECODE AS, D3D12_SHADER_BYTECODE MS, const D3D12_DEPTH_STENCIL_DESC2 *DepthStencil2) {
  return GetPipelineKey(Desc, Key, DepthBounds, AS, MS, DepthStencil2);
}

WMT::Reference<WMT::Function>
CompileFunction(
    MTLD3D12Device *device, MTLD3D12PipelineState *pipeline, std::span<const D3D12_SHADER_BYTECODE> shaders,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *arguments, const char *name,
    const std::function<int(SM50_SHADER_COMPILATION_ARGUMENT_DATA *, const char *, sm50_bitcode_t *, sm50_error_t *)>
        &compile
) {
  Sha1HashState h;
  auto hash = [&](const auto &...values) { (h.update(values), ...); };
  auto bytes = [&](const void *data, size_t size) {
    h.update(uint64_t(size));
    if (size)
      h.update(data, size);
  };
  bytes("d3d12", strlen("d3d12"));
  hash(AIRCONV_VERSION, device->GetMTLDevice().registryID(), uint64_t(shaders.size()));
  bytes(name, strlen(name));
  for (auto shader : shaders)
    bytes(shader.pShaderBytecode, shader.BytecodeLength);
  for (auto arg = arguments; arg; arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next) {
    hash(arg->type);
    // binding the whole structure makes new members require a key change, without hashing padding or pointers
    switch (arg->type) {
    case SM50_SHADER_COMMON: {
      auto &[next, type, metal, flags, width] = *(SM50_SHADER_COMMON_DATA *)arg;
      hash(metal, flags, width);
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      auto &[next, type, mask, dual, depth, unorm, formats, statistics, coverage, centered, single, clips, blend, bounds,
             late, conservative, coarse, rate, quads] = *(SM50_SHADER_PSO_PIXEL_SHADER_DATA *)arg;
      hash(mask, dual, depth, unorm, formats, statistics, coverage, centered, single, clips, blend, bounds, late,
           conservative, coarse, rate, quads);
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      auto &[next, type, format, slots, count, elements] = *(SM50_SHADER_IA_INPUT_LAYOUT_DATA *)arg;
      hash(format, slots, count);
      for (uint32_t i = 0; i < count; i++) {
        auto &[reg, slot, offset, attribute, step, rate] = elements[i];
        hash(reg, slot, offset, attribute, uint32_t(step), uint32_t(rate));
      }
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      auto &[next, type, strip, cut, index, primitive, pass, statistics, clips, quads, barycentrics, conservative, flat,
             noperspective] = *(SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *)arg;
      hash(strip, cut, index, primitive, pass, statistics, clips, quads, barycentrics, conservative, flat, noperspective);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      auto &data = *(SM50_SHADER_GS_PASS_THROUGH_DATA *)arg;
      hash(data.DataEncoded, data.RasterizationDisabled, data.ClipDistanceVaryings);
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      auto &[next, type, factor, size, groups, geometry, conservative, flat, noperspective, barycentrics] =
          *(SM50_SHADER_PSO_TESSELLATOR_DATA *)arg;
      hash(factor, size, groups, bool(geometry), conservative, flat, noperspective, barycentrics);
      break;
    }
    case SM50_SHADER_ROOT_SIGNATURE:
    case SM50_SHADER_ROOT_SIGNATURE2: {
      auto [next, type, code, length, samplers] = *(SM50_SHADER_ROOT_SIGNATURE_DATA *)arg;
      UINT size;
      if (FAILED(microsoft::DXBCGetRootSignature(code, &code, &size)))
        return {};
      bytes(code, size);
      hash(samplers);
      break;
    }
    case SM50_SHADER_STREAM_OUTPUT: {
      auto &[next, type, count, elements, strides, rasterized, counting] = *(SM50_SHADER_STREAM_OUTPUT_DATA *)arg;
      hash(count, strides, rasterized, counting);
      // offsets count preceding elements of each slot, including gaps (D3D11.3 14.3)
      for (uint32_t i = 0; i < count; i++)
        hash(elements[i].reg_id, elements[i].component, elements[i].stream, elements[i].output_slot);
      break;
    }
    case SM50_SHADER_MESH_SHADER: {
      auto &[next, type, pixel, conservative, amplified, triangles, slices, flat, noperspective, barycentrics] =
          *(SM50_SHADER_MESH_SHADER_DATA *)arg;
      hash(bool(pixel), conservative, amplified, triangles, slices, flat, noperspective, barycentrics);
      break;
    }
    default:
      ERR("Unknown shader compilation argument: ", arg->type);
      return {};
    }
  }
  auto key = h.final();
  WMT::Reference<WMT::DispatchData> stored;
  {
    std::lock_guard lock(pipeline->shader_mutex);
    if (auto found = pipeline->cached_shaders.find(key); found != pipeline->cached_shaders.end())
      stored = found->second;
  }
  if (stored) {
    WMT::Reference<WMT::Error> error;
    auto library = device->GetMTLDevice().newLibrary(stored, error);
    auto function = library ? library.newFunction(name) : WMT::Reference<WMT::Function>{};
    if (!function)
      ERR("Failed to load library shader ", name, ": ",
          library ? "no such function" : error.description().getUTF8String());
    return function;
  }
  auto &cache = ShaderCache::getInstance(WMTMetalVersion(device->GetMetalVersion()));
  bool enabled = false;
  WMT::Reference<WMT::Error> error;
  auto cached = [&]() -> WMT::Reference<WMT::Function> {
    WMT::Reference<WMT::DispatchData> data;
    if (auto reader = cache.getReader()) {
      enabled = true;
      data = reader->get(key);
    }
    if (data) {
      auto library = device->GetMTLDevice().newLibrary(data, error);
      if (auto function = library ? library.newFunction(name) : WMT::Reference<WMT::Function>{}) {
        std::lock_guard lock(pipeline->shader_mutex);
        pipeline->cached_shaders.emplace(key, data);
        return function;
      }
      if (auto writer = cache.getWriter())
        writer->set(key, {});
    }
    return {};
  };
  if (auto function = cached())
    return function;
  static mullion::mutex pending_mutex;
  static mullion::condition_variable completed;
  static std::unordered_set<Sha1Digest> pending;
  struct Conversion {
    Sha1Digest key;
    Conversion(Sha1Digest key) : key(key) {}
    ~Conversion() {
      {
        std::lock_guard lock(pending_mutex);
        pending.erase(key);
      }
      completed.notify_all();
    }
  };
  std::optional<Conversion> conversion;
  if (enabled) {
    {
      std::unique_lock lock(pending_mutex);
      completed.wait(lock, [&] { return !pending.contains(key); });
      pending.insert(key);
      conversion.emplace(key);
    }
    if (auto function = cached())
      return function;
  }
  SM50ShaderBitcode bitcode;
  SM50Error sm50_error;
  if (compile(arguments, name, &bitcode, &sm50_error)) {
    ERR("Failed to compile ", name, ": ", SM50GetErrorMessageString(sm50_error));
    for (auto shader : shaders)
      MTLD3D12PipelineState::DumpShaders({shader});
    return {};
  }
  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);
  auto data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  auto library = device->GetMTLDevice().newLibrary(data, error);
  auto function = library ? library.newFunction(name) : WMT::Reference<WMT::Function>{};
  if (!function) {
    ERR("Failed to load ", name, ": ", library ? "no such function" : error.description().getUTF8String());
    return {};
  }
  if (auto writer = cache.getWriter())
    writer->set(key, data);
  {
    std::lock_guard lock(pipeline->shader_mutex);
    pipeline->cached_shaders.emplace(key, data);
  }
  return function;
}

class MTLD3D12GraphicsPipelineStateImpl : public MTLD3D12PipelineObject<MTLD3D12GraphicsPipelineState> {
protected:
  // held for the pipeline's life, so that pipelines and variants made of the same shaders share their initialization
  SM50Shader shader_vs, shader_hs, shader_ds, shader_gs, shader_ps, shader_as, shader_ms;
  MTL_SHADER_REFLECTION ref_vs;
  MTL_SHADER_REFLECTION ref_ps{};

  struct PipelineDescriptor {
    std::variant<WMTRenderPipelineInfo, WMTMeshRenderPipelineInfo> info;
    std::vector<WMT::Reference<WMT::Function>> functions;
    std::unordered_map<uint64_t, WMT::Reference<WMT::RenderPipelineState>> variants;
    // the geometry shader that counts pipeline statistics, made when first used
    std::function<WMT::Reference<WMT::Function>()> statistics_geometry_shader;
    WMT::Reference<WMT::Function> statistics_gs;
  };
  std::unordered_map<obj_handle_t, PipelineDescriptor> pipeline_descriptors_;
  mullion::mutex render_targets_mutex_;
  // the pixel shader made again when first needed: counting pipeline statistics, with its depth and stencil tests
  // after it (an occlusion query and the depth bounds test of a GPU without one), of coarse pixels (variable rate
  // shading), or blending only the targets a pass binds, as Metal refuses a pipeline whose shader reads a target it has
  // no format for
  std::function<WMT::Reference<WMT::Function>(bool statistics, bool late, bool coarse, uint32_t blends, bool depth)>
      pixel_shader_;
  std::unordered_map<uint32_t, WMT::Reference<WMT::Function>> pixel_shaders_;
  // the targets the pixel shader blends itself (ShaderBlend)
  uint32_t blend_mask_ = 0;

  template <typename Info>
  WMT::Reference<WMT::RenderPipelineState>
  CreatePipeline(const Info &info, WMT::Reference<WMT::Error> &error,
                 std::function<WMT::Reference<WMT::Function>()> statistics_geometry_shader = {}) {
    auto pipeline = device_->GetMTLDevice().newRenderPipelineState(info, error);
    if (!pipeline || (!rtv_mask && !dsv_mask && !pixel_shader && !statistics_geometry_shader))
      return pipeline;
    std::lock_guard<mullion::mutex> lock(render_targets_mutex_);
    auto &descriptor = pipeline_descriptors_[pipeline.handle];
    descriptor.info = info;
    descriptor.statistics_geometry_shader = std::move(statistics_geometry_shader);
    descriptor.functions.emplace_back(WMT::Function(info.fragment_function));
    if constexpr (std::is_same_v<Info, WMTRenderPipelineInfo>) {
      descriptor.functions.emplace_back(WMT::Function(info.vertex_function));
    } else {
      descriptor.functions.emplace_back(WMT::Function(info.object_function));
      descriptor.functions.emplace_back(WMT::Function(info.mesh_function));
    }
    return pipeline;
  }

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
    std::string vs, gs;                      // bytecode
    Com<ID3D12RootSignature> root_signature;
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
    bool clip_distance_varyings;
    bool quad_lines, barycentrics;
    // conservative rasterization, and how the pixel shader interpolates its inputs
    bool conservative;
    uint32_t flat_inputs, noperspective_inputs;
  };
  std::shared_ptr<GeometryInputs> topology_inputs_;
  // whether a pipeline's geometry shader can count pipeline statistics
  bool statistics_geometry_ = false;
  mullion::mutex topology_mutex_;
  // by topology and stream output's counting pass
  std::unordered_map<uint32_t, WMT::Reference<WMT::RenderPipelineState>> topology_variants_;

  static SM50_SHADER_COMPILATION_ARGUMENT_DATA *
  RootSignature(
      SM50_SHADER_ROOT_SIGNATURE_DATA &rootsig, ID3D12RootSignature *pRootSignature, void *next
  ) {
    rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
    rootsig.static_samplers = 0;
    rootsig.bytecode_length = static_cast<MTLD3D12RootSignature *>(pRootSignature)->GetBlob(&rootsig.bytecode);
    rootsig.next = next;
    return (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig;
  }

  // a geometry pipeline's Metal pipeline for lists or strips of `primitive` (D3D10_SB_PRIMITIVE): the vertex shader
  // in the object stage, reading indices through the index buffer view each draw binds, and in the mesh stage the
  // geometry shader, or without one what passes the primitives on to the rasterizer (`pass_through`), or nothing.
  // stream output's counting pass draws nothing
  // the geometry stage's arguments, its stream output in `so`
  static SM50_SHADER_PSO_GEOMETRY_SHADER_DATA
  GeometryArguments(
      GeometryInputs &in, SM50_SHADER_STREAM_OUTPUT_DATA &so, bool strip, bool counting, uint32_t primitive,
      bool pass_through, bool statistics
  ) {
    SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso_gs{};
    pso_gs.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
    pso_gs.strip_topology = strip;
    pso_gs.strip_cut = in.strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pso_gs.strip_cut_index = in.strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF ? 0xffff : ~0u;
    pso_gs.input_primitive = primitive;
    pso_gs.pass_through = pass_through && !counting;
    pso_gs.pipeline_statistics = statistics;
    pso_gs.clip_distance_varyings = in.clip_distance_varyings;
    pso_gs.quad_lines = in.quad_lines && pass_through && !counting;
    pso_gs.conservative = in.conservative && !counting && (!in.gs.empty() || pass_through);
    pso_gs.barycentrics = in.barycentrics && (pso_gs.quad_lines || pso_gs.conservative);
    pso_gs.flat_inputs = in.flat_inputs;
    pso_gs.noperspective_inputs = in.noperspective_inputs;
    so = in.so;
    so.counting = counting;
    so.elements = in.so_elements.data();
    so.next = &in.common;
    pso_gs.next = in.stream_output ? (void *)&so : (void *)&in.common;
    return pso_gs;
  }

  WMT::Reference<WMT::Function>
  GeometryFunction(
      GeometryInputs &in, sm50_shader_t shader_vs, sm50_shader_t shader_gs, D3D12_SHADER_BYTECODE GS,
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA &pso_gs, HRESULT &status
  ) {
    SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
    return CompileFunction(
        device_, this, std::array{D3D12_SHADER_BYTECODE{in.vs.data(), in.vs.size()}, GS},
        RootSignature(rootsig, in.root_signature.ptr(), &pso_gs), "gs_main",
        [&](auto args, auto name, auto bitcode, auto error) {
          auto result = SM50CompileGeometryPipelineGeometry(shader_vs, shader_gs, args, name, bitcode, error);
          if (result)
            status = E_INVALIDARG;
          return result;
        }
    );
  }

  WMT::Reference<WMT::RenderPipelineState>
  GeometryVariant(
      const std::shared_ptr<GeometryInputs> &inputs, sm50_shader_t shader_vs, sm50_shader_t shader_gs,
      D3D12_SHADER_BYTECODE GS, bool strip, bool counting, uint32_t primitive, bool pass_through, HRESULT &status
  ) {
    status = E_FAIL;
    auto &in = *inputs;
    WMT::Reference<WMT::RenderPipelineState> variant;
    WMT::Reference<WMT::Error> err;
    D3D12_SHADER_BYTECODE VS{in.vs.data(), in.vs.size()};
    SM50_SHADER_STREAM_OUTPUT_DATA so;
    auto pso_gs = GeometryArguments(in, so, strip, counting, primitive, pass_through, false);
    in.ia_layout.elements = in.elements.data();
    in.ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_VIEW;
    in.ia_layout.next = &pso_gs;
    SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_vs;
    auto args_vs = RootSignature(rootsig_vs, in.root_signature.ptr(), &in.ia_layout);
    auto vs_func = CompileFunction(
        device_, this, std::array{VS, GS}, args_vs, "vsgs_main", [&](auto args, auto name, auto bitcode, auto error) {
          auto result = SM50CompileGeometryPipelineVertex(shader_vs, shader_gs, args, name, bitcode, error);
          if (result)
            status = E_INVALIDARG;
          return result;
        }
    );
    if (!vs_func)
      return variant;
    auto gs_func = GeometryFunction(in, shader_vs, shader_gs, GS, pso_gs, status);
    if (!gs_func)
      return variant;
    // a pipeline statistics query's variant counts in the geometry shader as well
    std::function<WMT::Reference<WMT::Function>()> statistics;
    if (shader_gs && !counting) {
      statistics_geometry_ = true;
      statistics = [this, inputs, strip, primitive, pass_through] {
        auto &in = *inputs;
        D3D12_SHADER_BYTECODE VS{in.vs.data(), in.vs.size()}, GS{in.gs.data(), in.gs.size()};
        SM50Shader shader_vs, shader_gs;
        MTL_SHADER_REFLECTION reflection;
        if (FAILED(InitializeShader(device_, VS, &shader_vs, &reflection)) ||
            FAILED(InitializeShader(device_, GS, &shader_gs, &reflection)))
          return WMT::Reference<WMT::Function>{};
        SM50_SHADER_STREAM_OUTPUT_DATA so;
        auto pso_gs = GeometryArguments(in, so, strip, false, primitive, pass_through, true);
        HRESULT status;
        return GeometryFunction(in, shader_vs, shader_gs, GS, pso_gs, status);
      };
    }
    auto info = in.info;
    info.object_function = vs_func.handle;
    info.mesh_function = gs_func.handle;
    bool rasterized = (shader_gs || pass_through) && !counting && (!in.stream_output || so.rasterized_stream != ~0u);
    info.fragment_function = rasterized ? in.ps.handle : obj_handle_t{};
    info.rasterization_enabled = rasterized;
    info.payload_memory_length = SM50_GEOMETRY_PAYLOAD_SIZE;
    if (!(variant = CreatePipeline(info, err, std::move(statistics)))) {
      ERR("Failed to create geometry PSO of shaders ", name, ": ", err.description().getUTF8String());
      DumpShaders({VS, GS});
    }
    return variant;
  }

  // an object function for a conservative mesh shader without an amplification shader: each of its threadgroups, one
  // thread for each of the D3D grid's groups, launches the group's `slices` mesh threadgroups and gives them its ID as
  // the payload (SM50_SHADER_MESH_SHADER_DATA)
  WMT::Reference<WMT::Function>
  ConservativeObject(uint32_t slices) {
    auto source = std::string(R"(#include <metal_stdlib>
using namespace metal;
struct Group {
  uint3 id;
};
[[object]] void conservative_object(uint3 group [[threadgroup_position_in_grid]], object_data Group &payload [[payload]],
                                    mesh_grid_properties grid) {
  payload.id = group;
  grid.set_threadgroups_per_grid(uint3()") + std::to_string(slices) + R"(, 1, 1));
}
)";
    WMT::Reference<WMT::Error> err;
    auto library = device_->GetMTLDevice().newLibraryWithSource(source, err);
    if (!library) {
      ERR("Failed to compile the conservative rasterizer's object function: ", err.description().getUTF8String());
      return {};
    }
    return library.newFunction("conservative_object");
  }

  // a fragment function for a pipeline without a pixel shader that does what a pixel shader would: discards the pixels
  // whose squares miss the polygon the mesh stage passes on in SM50_CONSERVATIVE_VARYINGS, as airconv's conservative
  // pixel shaders do, writing their depth; and of no sample in the depth bounds of a GPU without the test, keeping the
  // samples that are, as SM50_SHADER_PSO_PIXEL_SHADER_DATA's depth_bounds does
  WMT::Reference<WMT::Function>
  TestFragment(bool conservative, bool bounds) {
    auto corners = std::to_string(SM50_CONSERVATIVE_CORNERS);
    std::string source = "#include <metal_stdlib>\nusing namespace metal;\nstruct In {\n  float4 position [[position]];\n";
    for (uint32_t i = 0; conservative && i < SM50_CONSERVATIVE_VARYINGS; i++)
      source += "  float v" + std::to_string(i) + " [[user(mullion_conservative" + std::to_string(i) + "), flat]];\n";
    source += "};\nstruct Out {\n";
    source += conservative ? "  float z [[depth(any)]];\n" : "";
    source += bounds ? "  uint samples [[sample_mask]];\n" : "";
    source += "};\nfragment Out test_fragment(In in [[stage_in]]";
    if (bounds)
      source += ", constant uint2 &bounds [[buffer(" + std::to_string(SM50_BINDING_INDEX_DEPTH_BOUNDS) +
                ")]], device const uchar *samples [[buffer(" + std::to_string(SM50_BINDING_INDEX_DEPTH_BOUNDS_SAMPLES) + ")]]";
    source += ") {\n  Out out;\n";
    if (conservative) {
      source += "  float2 v[] = {";
      for (uint32_t k = 0; k < SM50_CONSERVATIVE_CORNERS; k++)
        source += "{in.v" + std::to_string(2 * k) + ", in.v" + std::to_string(2 * k + 1) + "}, ";
      source += R"(};
  const float half_pixel = 0.5 + 1.0 / 256;
  float2 center = floor(in.position.xy) + 0.5, low = v[0], high = v[0];
  float area = 0;
  for (int k = 0; k < )" + corners + R"(; k++) {
    float2 a = v[k], b = v[(k + 1) % )" + corners + R"(];
    low = min(low, a), high = max(high, a), area += a.x * b.y - b.x * a.y;
  }
  bool outside = any(center + half_pixel < low) || any(center - half_pixel > high);
  float inward = area > 0 ? 1 : -1;
  for (int k = 0; k < )" + corners + R"(; k++) {
    float2 a = v[k], b = v[(k + 1) % )" + corners + R"(];
    float2 n = float2(a.y - b.y, b.x - a.x) * inward;
    outside |= dot(n, center - a) + (abs(n.x) + abs(n.y)) * half_pixel < 0;
  }
  if (outside)
    discard_fragment();
  out.z = in.position.z;
)";
    }
    if (bounds)
      source += R"(  out.samples = bounds.x ? samples[uint(in.position.y) * bounds.y + uint(in.position.x)] : ~0u;
  if (!out.samples)
    discard_fragment();
)";
    source += "  return out;\n}\n";
    WMT::Reference<WMT::Error> err;
    auto library = device_->GetMTLDevice().newLibraryWithSource(source, err);
    if (!library) {
      ERR("Failed to compile the fragment function of a pipeline without a pixel shader: ", err.description().getUTF8String());
      return {};
    }
    return library.newFunction("test_fragment");
  }

public:
  MTLD3D12GraphicsPipelineStateImpl(MTLD3D12Device *pDevice) :
      MTLD3D12PipelineObject<MTLD3D12GraphicsPipelineState>(pDevice) {
    IsComputePipelineState = FALSE;
  }

  // made when first drawn
  WMT::RenderPipelineState
  TopologyPipeline(D3D12_PRIMITIVE_TOPOLOGY topology, bool counting) final {
    bool patch = topology >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
                 topology <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
    if (patch != patches)
      return {};
    std::lock_guard<mullion::mutex> lock(topology_mutex_);
    auto &variant = topology_variants_[topology * 2 + counting];
    if (!variant && topology_inputs_) {
      auto start = device_->NamesPasses() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
      auto &in = *topology_inputs_;
      SM50Shader shader_vs;
      MTL_SHADER_REFLECTION reflection;
      HRESULT hr;
      bool strip = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ || topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
      auto primitive = patch ? D3D_PRIMITIVE_1_CONTROL_POINT_PATCH + (topology - D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
                       : in.primitive == D3D_PRIMITIVE_LINE ? D3D_PRIMITIVE_LINE_ADJ
                                                            : D3D_PRIMITIVE_TRIANGLE_ADJ;
      // patches only stream out: nothing rasterizes them without a tessellator
      if (SUCCEEDED(InitializeShader(device_, {in.vs.data(), in.vs.size()}, &shader_vs, &reflection)))
        variant = GeometryVariant(topology_inputs_, shader_vs, {}, {}, strip, counting, primitive, !patch, hr);
      if (variant && device_->NamesPasses())
        device_->PipelineMade(name, "graphics", start);
    }
    return variant;
  }

  bool
  BlendFactorIsDualSource(D3D12_BLEND Blend) {
    return (Blend >= D3D12_BLEND_SRC1_COLOR) && (Blend <= D3D12_BLEND_INV_SRC1_ALPHA);
  }

  template <typename RenderPipelineInfo>
  HRESULT
  InitializePSO(
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, RenderPipelineInfo &info, bool &dual_source_blending,
      SM50_SHADER_BLEND (&blend)[8]
  ) {
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
      auto renderTarget = pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0];

      if (renderTarget.BlendEnable && renderTarget.LogicOpEnable)
        return FAIL(E_INVALIDARG);

      if (pDesc->BlendState.IndependentBlendEnable && renderTarget.LogicOpEnable)
        return FAIL(E_INVALIDARG);

      // Windows accepts non-renderable formats as null slots (vkd3d-proton test_render_target_support_validation).
      if (!any_bit_set(device_->GetMTLPixelFormatCapability(format_desc.PixelFormat) & FormatCapability::Color))
        continue;
      rt.pixel_format = format_desc.PixelFormat;
      rtv_mask |= 1u << i;

      // an undeclared output leaves the target alone even with blending (D3D11.3 16.9.1)
      rt.write_mask = (ref_ps.PixelShader.ValidRenderTargets & (1u << i))
                          ? RenderTargetWriteMask(rt.pixel_format, kColorWriteMaskMap[renderTarget.RenderTargetWriteMask])
                          : WMTColorWriteMaskNone;

      if (rt.pixel_format == WMTPixelFormatRGB9E5Float)
        rt.write_mask = (rt.write_mask & ~WMTColorWriteMaskAlpha) ? WMTColorWriteMaskAll : 0;

      // blend modes have no effect on undeclared outputs (D3D11.3 16.9.1).
      if (renderTarget.BlendEnable && (ref_ps.PixelShader.ValidRenderTargets & (1u << i))) {
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
      if (ref_ps.PixelShader.ValidRenderTargets & (1u << i))
        blend[i] = ShaderBlend(rt, renderTarget, renderTarget.LogicOpEnable);
      if (i < 2)
        effective_dual_source_rtvs++;
    }

    if (dual_source_blending && effective_dual_source_rtvs > 1)
      return FAIL(E_INVALIDARG);

    if (pDesc->DSVFormat != DXGI_FORMAT_UNKNOWN) {
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), pDesc->DSVFormat, format_desc))) {
        return hr;
      }
      auto dsv_flags = dsv_mask = DepthStencilPlanarFlags(format_desc.PixelFormat);
      if (dsv_flags & 1)
        info.depth_pixel_format = format_desc.PixelFormat;
      if (dsv_flags & 2)
        info.stencil_pixel_format = format_desc.PixelFormat;
    } else if (auto &ds = pDesc->DepthStencilState;
               !pDesc->RasterizerState.ForcedSampleCount &&
               ((ds.DepthEnable && (ds.DepthFunc != D3D12_COMPARISON_FUNC_ALWAYS ||
                                    ds.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO || ds.StencilEnable)) ||
                ds.StencilEnable || depth_bounds || depth_bounds_emulated)) {
      // D3D12 asks for the bound view's format, but titles that test depth or stencil without one rely on drivers
      // testing the view bound (vkd3d-proton keeps it: "A workaround for SottR, which creates pipelines with
      // DSV_UNKNOWN, but still insists on using a depth buffer", libs/vkd3d/state.c). a test of ALWAYS that writes
      // nothing, with no stencil test, is off, as there. a forced sample count rasterizes with no view and no depth
      // test (D3D11.3 3.5.6.2), so such a pipeline keeps the view out as before
      dsv_mask = 3;
      dsv_any_format = true;
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

    // a description left at zero has one sample (Unreal Engine 5's pipelines; vkd3d-proton draws them so). a pass
    // with no targets rasterizes the forced count (d3d12_command_list.cpp), and its pipeline with it
    info.raster_sample_count = !rtv_mask && !dsv_mask && pDesc->RasterizerState.ForcedSampleCount
                                   ? pDesc->RasterizerState.ForcedSampleCount
                                   : std::max(pDesc->SampleDesc.Count, 1u);
    // refused before its shaders are compiled for a pipeline Metal would refuse
    if (!device_->GetMTLDevice().supportsTextureSampleCount(info.raster_sample_count)) {
      WARN("InitializePSO: ", info.raster_sample_count, " samples, which this GPU does not rasterize");
      return FAIL(E_INVALIDARG);
    }
    // Metal refuses a mesh pipeline that asks for indirect command buffers on GPUs before Apple9, where mesh draws
    // never go in them (d3d12_command_allocator.cpp)
    info.support_indirect_command_buffers = std::is_same_v<RenderPipelineInfo, WMTRenderPipelineInfo> ||
                                            device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple9);

    // Metal takes no alpha-to-coverage with an integer target 0, where the pixel shader makes the coverage
    info.alpha_to_coverage_enabled = pDesc->BlendState.AlphaToCoverageEnable && !ref_ps.PixelShader.HasCoverageOutput &&
                                     !IsIntegerFormat(WMTPixelFormat(ORIGINAL_FORMAT(info.colors[0].pixel_format)));
    for (unsigned i = 0; i < std::size(blend); i++)
      blend_mask_ |= uint32_t(blend[i].blend) << i;
    shader_blend = blend_mask_;

    return S_OK;
  }

  void
  InitializeDSSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, const D3D12_DEPTH_STENCIL_DESC2 *depth_stencil2) {
    WMTDepthStencilInfo info;
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = false;
    info.front_stencil.enabled = false;
    info.back_stencil.enabled = false;
    if (pDesc->DepthStencilState.DepthEnable) {
      info.depth_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.DepthFunc];
      info.depth_write_enabled = pDesc->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    }
    writes_depth = info.depth_write_enabled;

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
      info.back_stencil.write_mask = depth_stencil2 ? depth_stencil2->BackFace.StencilWriteMask
                                                  : pDesc->DepthStencilState.StencilWriteMask;
      info.back_stencil.read_mask = depth_stencil2 ? depth_stencil2->BackFace.StencilReadMask
                                                 : pDesc->DepthStencilState.StencilReadMask;
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
  Initialize(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, D3D12_SHADER_BYTECODE AS, D3D12_SHADER_BYTECODE MS,
             const D3D12_DEPTH_STENCIL_DESC2 *depth_stencil2) {
    const auto &rasterizer = pDesc->RasterizerState;
    if ((rasterizer.FillMode != D3D12_FILL_MODE_SOLID && rasterizer.FillMode != D3D12_FILL_MODE_WIREFRAME) ||
        rasterizer.CullMode < D3D12_CULL_MODE_NONE || rasterizer.CullMode > D3D12_CULL_MODE_BACK)
      return E_INVALIDARG;
    // D3D12_RASTERIZER_DESC, ForcedSampleCount: only 0, 1, 4, 8 and optionally 16 are valid; Metal rasterizes the
    // sample counts its textures take
    if (rasterizer.ForcedSampleCount != 0 && rasterizer.ForcedSampleCount != 1 && rasterizer.ForcedSampleCount != 4 &&
        rasterizer.ForcedSampleCount != 8 && rasterizer.ForcedSampleCount != 16)
      return E_INVALIDARG;
    if (rasterizer.ForcedSampleCount > 1 && !device_->GetMTLDevice().supportsTextureSampleCount(rasterizer.ForcedSampleCount))
      return E_INVALIDARG;
    if (rasterizer.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF) {
      D3D12_FEATURE_DATA_D3D12_OPTIONS support{};
      HRESULT hr = device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &support, sizeof(support));
      if (FAILED(hr))
        return hr;
      if (rasterizer.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON ||
          support.ConservativeRasterizationTier == D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED ||
          rasterizer.FillMode != D3D12_FILL_MODE_SOLID)
        return E_INVALIDARG;
    }
    auto start = device_->NamesPasses() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    stream_output = pDesc->StreamOutput.NumEntries;
    mesh_shader = MS.pShaderBytecode;
    pixel_shader = pDesc->PS.pShaderBytecode;
    strip_cut = pDesc->IBStripCutValue;

    bool tessellation = pDesc->HS.pShaderBytecode;
    // stream output runs through the geometry pipeline, with or without a geometry shader; after a tessellator the
    // geometry shader runs in the tessellation pipeline's mesh stage
    geometry_shader = !tessellation && (pDesc->GS.pShaderBytecode || stream_output);
    if (tessellation && stream_output) {
      ERR("CreatePipelineState: stream output of a tessellated draw is not implemented yet");
      return E_NOTIMPL;
    }
    if (tessellation != !!pDesc->DS.pShaderBytecode)
      return FAIL(E_INVALIDARG);

    if (!pDesc->VS.pShaderBytecode && !mesh_shader) {
      ERR("no vertex shader");
      return E_INVALIDARG;
    }

    HRESULT hr;

    MTL_SHADER_REFLECTION ref_hs, ref_ds, ref_gs, ref_as, ref_ms;
    WMT::Reference<WMT::Error> err;

    SM50_SHADER_COMMON_DATA common;
    common.flags = SM50_SHADER_FLAG(device_->GetShaderFlags() | (borderless ? SM50_SHADER_FLAG_NO_CUSTOM_BORDER : 0));
    common.type = SM50_SHADER_COMMON;
    common.metal_version = device_->GetMetalVersion();
    common.simd_width = device_->GetSIMDWidth();
    common.next = nullptr;

    auto root_signature = [&](SM50_SHADER_ROOT_SIGNATURE_DATA &rootsig, void *next) {
      return RootSignature(rootsig, pDesc->pRootSignature, next);
    };
    auto function = [&](sm50_shader_t shader, D3D12_SHADER_BYTECODE code, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                        const char *name, D3D12_SHADER_BYTECODE linked = {}) {
      auto shaders = std::array{code, linked};
      return CompileFunction(
          device_, this, std::span(shaders).first(linked.pShaderBytecode ? 2 : 1), args, name,
          [shader](auto args, auto name, auto bitcode, auto error) {
            return SM50Compile(shader, args, name, bitcode, error);
          }
      );
    };
    Name({pDesc->VS, pDesc->HS, pDesc->DS, pDesc->GS, pDesc->PS, MS, AS});

    if (mesh_shader) {
      if (FAILED(hr = InitializeShader(device_, MS, &shader_ms, &ref_ms)) ||
          (AS.pShaderBytecode && FAILED(hr = InitializeShader(device_, AS, &shader_as, &ref_as))))
        return hr;
      std::copy_n(ref_ms.ThreadgroupSize, 3, mesh_threads);
      if (AS.pShaderBytecode)
        std::copy_n(ref_as.ThreadgroupSize, 3, object_threads);
    } else if (FAILED(hr = InitializeShader(device_, pDesc->VS, &shader_vs, &ref_vs)))
      return hr;
    if (tessellation && (FAILED(hr = InitializeShader(device_, pDesc->HS, &shader_hs, &ref_hs)) ||
                         FAILED(hr = InitializeShader(device_, pDesc->DS, &shader_ds, &ref_ds))))
      return hr;
    if (pDesc->GS.pShaderBytecode && FAILED(hr = InitializeShader(device_, pDesc->GS, &shader_gs, &ref_gs)))
      return hr;
    gs_instances = sm50_shader_t(shader_gs) ? ref_gs.GeometryShader.InstanceCount : 1;
    // the stage before stream output and the rasterizer: the geometry shader, or the vertex shader
    auto last_stage = pDesc->GS.pShaderBytecode ? pDesc->GS : pDesc->VS;
    std::vector<SM50_STREAM_OUTPUT_ELEMENT2> so_elements;
    SM50_SHADER_STREAM_OUTPUT_DATA so_data{};
    if (stream_output) {
      if (FAILED(hr = ExtractStreamOutputElements(
              last_stage.pShaderBytecode, std::span(pDesc->StreamOutput.pSODeclaration, pDesc->StreamOutput.NumEntries),
              std::span(pDesc->StreamOutput.pBufferStrides, pDesc->StreamOutput.NumStrides), so_elements
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
    }
    // a vertex shader's cull distances discard whole primitives (D3D11.3 15.4.2), which Metal can only by primitive:
    // the mesh stage passes the vertex shader's primitives on, less those. it also rasterizes what stream output
    // without a geometry shader streams out of the object stage
    vertex_registers = mesh_shader ? 0 : ref_vs.NumOutputElement;
    bool plain = !mesh_shader && !tessellation && !pDesc->GS.pShaderBytecode;
    // conservative rasterization dilates a vertex shader's triangles in the mesh stage, which passes them on
    // (D3D12_CONSERVATIVE_RASTERIZATION_TIER_1); it does not apply to points and lines (D3D11.3 15.17)
    bool requested = pDesc->RasterizerState.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    conservative = requested && (!plain || pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    // a geometry shader's triangles are grown where it emits them, when they fit a mesh; a domain shader's
    // triangles in pieces of their workloads, when one of its triangles fits one; after a mesh shader, or where they
    // do not fit, those triangles rasterize as usual for now
    bool geometry_grows = !mesh_shader && pDesc->GS.pShaderBytecode && ref_gs.GeometryShader.ConservativeFits;
    // a mesh shader's triangles in slices of its groups' (SM50_SHADER_MESH_SHADER_DATA), when its outputs fit, in one
    // slice where the shader writes resources
    bool mesh_grows = mesh_shader && ref_ms.MeshConservativeTriangles;
    auto tessellated = microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE(tessellation ? ref_hs.Tessellator.OutputPrimitive : 0);
    bool domain_grows = tessellation && !pDesc->GS.pShaderBytecode &&
                        (tessellated == microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW ||
                         tessellated == microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW) &&
                        dxbc::tessellation_pieces(
                            1, microsoft::D3D11_SB_TESSELLATOR_DOMAIN(ref_hs.Tessellator.Domain), tessellated,
                            ref_ds.PostTessellator.MeshVertexSize, true
                        ).primitives;
    // points and lines rasterize as usual, a geometry shader's or a tessellator's too
    bool triangles = mesh_shader                ? true
                     : pDesc->GS.pShaderBytecode ? ref_gs.GeometryShader.ConservativeFits
                     : tessellation          ? tessellated == microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW ||
                                         tessellated == microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW
                                             : true;
    if (conservative && !plain && !triangles)
      conservative = false;
    if (conservative && !plain && !geometry_grows && !domain_grows && !mesh_grows) {
      static std::once_flag warned;
      std::call_once(warned, [] {
        WARN("CreatePipelineState: conservative rasterization of primitives too large to grow, or after a mesh shader "
             "that writes resources, is not implemented yet; those pipelines rasterize as usual");
      });
      conservative = false;
    }
    // and passes lines on as quadrilaterals (D3D11.3 15.14.1, SM50_SHADER_PSO_GEOMETRY_SHADER_DATA)
    bool quad_lines =
        plain && pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE && pDesc->RasterizerState.MultisampleEnable;
    bool pass_through = plain && pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH &&
                        (ref_vs.CullDistances || (stream_output && so_data.rasterized_stream == 0) || quad_lines ||
                         conservative);
    geometry_shader |= pass_through;
    mesh_instances = sm50_shader_t(shader_gs) ? gs_instances : pass_through;
    adjacency = plain && (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ||
                          pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    patches = plain && stream_output && pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    bool mesh = tessellation || geometry_shader || mesh_shader;
    // the alpha-to-coverage state reads the pixel shader's reflection
    if (pDesc->PS.pShaderBytecode && FAILED(hr = InitializeShader(device_, pDesc->PS, &shader_ps, &ref_ps)))
      return hr;
    // a pixel shader whose runs are seen tests the last pre-raster stage's clip distances itself (D3D11.3 15.4.1),
    // where Metal would cut the primitive and run a pixel on the cut once for each piece. a geometry shader after
    // tessellation still clips in Metal, and so does a conservative rasterizer's grown triangle, whose varyings are
    // linear in the window
    uint32_t clips = pDesc->PS.pShaderBytecode && !mesh_shader && !(tessellation && pDesc->GS.pShaderBytecode) &&
                             !conservative &&
                             (ref_ps.UAVSlotMask || ref_ps.PixelShader.HasCoverageInput)
                         ? (pDesc->GS.pShaderBytecode ? ref_gs : tessellation ? ref_ds : ref_vs).ClipDistances
                         : 0;
    // what the stage before the rasterizer takes of it
    SM50_SHADER_GS_PASS_THROUGH_DATA clip_data{};
    clip_data.type = SM50_SHADER_GS_PASS_THROUGH;
    clip_data.DataEncoded = ~0u;
    clip_data.ClipDistanceVaryings = clips;
    clip_data.next = &common;
    uses_uav = (mesh_shader ? ref_ms.UAVSlotMask | (AS.pShaderBytecode ? ref_as.UAVSlotMask : 0) : ref_vs.UAVSlotMask) ||
               (tessellation && (ref_hs.UAVSlotMask || ref_ds.UAVSlotMask)) ||
               (pDesc->GS.pShaderBytecode && ref_gs.UAVSlotMask) || (pDesc->PS.pShaderBytecode && ref_ps.UAVSlotMask);

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
    SM50_SHADER_BLEND blend[8] = {};
    if (FAILED(hr = mesh ? InitializePSO(pDesc, mesh_info, dual_source_blending, blend)
                         : InitializePSO(pDesc, info, dual_source_blending, blend)))
      return hr;

    if (pDesc->PS.pShaderBytecode) {
      auto sha1 = Sha1HashState::compute(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);

      std::string ps_name = "ps_main" + sha1.string().substr(0, 8);

      SM50_SHADER_PSO_PIXEL_SHADER_DATA data_ps;
      data_ps.dual_source_blending = dual_source_blending;
      data_ps.disable_depth_output =
          (mesh ? mesh_info.depth_pixel_format : info.depth_pixel_format) == WMTPixelFormatInvalid;
      data_ps.unorm_output_reg_mask = 0;
      // the sample mask selects output samples, which rendering with UAVs alone has none of (D3D11.3 3.5.6.4)
      bool attached = pDesc->DSVFormat != DXGI_FORMAT_UNKNOWN ||
                      std::any_of(pDesc->RTVFormats, pDesc->RTVFormats + pDesc->NumRenderTargets,
                                  [](auto format) { return format != DXGI_FORMAT_UNKNOWN; });
      data_ps.sample_mask = attached ? pDesc->SampleMask : ~0u;
      data_ps.type = SM50_SHADER_PSO_PIXEL_SHADER;
      data_ps.next = &common;
      data_ps.pipeline_statistics = false;
      data_ps.alpha_to_coverage = pDesc->BlendState.AlphaToCoverageEnable && !ref_ps.PixelShader.HasCoverageOutput &&
                                  !(mesh ? mesh_info.alpha_to_coverage_enabled : info.alpha_to_coverage_enabled);
      // one sample rasterizes at the pixel center, and the targets' samples with it (D3D11.3 3.5.6.2)
      data_ps.single_raster_sample = attached && pDesc->RasterizerState.ForcedSampleCount == 1;
      data_ps.clip_distances = clips;
      std::copy(std::begin(blend), std::end(blend), data_ps.blend);
      data_ps.centered_samples =
          data_ps.single_raster_sample || pDesc->SampleDesc.Quality == DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN;
      data_ps.depth_bounds = depth_bounds_emulated;
      data_ps.late_tests = false;
      data_ps.conservative = conservative;
      data_ps.quad_lines = quad_lines && ref_ps.PixelShader.Barycentrics;
      data_ps.coarse_shading = false;
      // the primitive's shading rate, from the stage before the rasterizer: a domain or mesh shader has none
      data_ps.shading_rate_varying = sm50_shader_t(shader_gs)      ? ref_gs.ShadingRateOutput
                                     : tessellation || mesh_shader ? 0
                                                                   : ref_vs.ShadingRateOutput;
      coarse_shading = !ref_ps.PixelShader.FineShading;

      std::array<WMTPixelFormat, std::size(data_ps.pixel_formats)> formats{};
      for (unsigned i = 0; i < pDesc->NumRenderTargets; i++)
        formats[i] = mesh ? mesh_info.colors[i].pixel_format : info.colors[i].pixel_format;
      auto outputs = PixelShaderOutputFormats(ref_ps.PixelShader, {formats.data(), pDesc->NumRenderTargets},
                                             dual_source_blending);
      std::copy(outputs.begin(), outputs.end(), data_ps.pixel_formats);

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      if (!(ps_func = function(shader_ps, pDesc->PS, root_signature(rootsig, &data_ps), ps_name.c_str())))
        return FAIL(E_INVALIDARG);
      pixel_shader_ =
          [this, code = std::string((const char *)pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength),
           signature = Com<ID3D12RootSignature>(pDesc->pRootSignature), data_ps, common,
           ps_name](bool statistics, bool late, bool coarse, uint32_t blends, bool depth) mutable {
            D3D12_SHADER_BYTECODE bytecode{code.data(), code.size()};
            SM50Shader shader;
            MTL_SHADER_REFLECTION reflection;
            if (FAILED(InitializeShader(device_, bytecode, &shader, &reflection)))
              return WMT::Reference<WMT::Function>{};
            auto data = data_ps;
            data.pipeline_statistics = statistics;
            data.late_tests = late;
            data.coarse_shading = coarse;
            // Metal refuses a pipeline whose shader outputs depth without a depth format: the shader's depth is kept
            // only for a variant that has one (RenderTargets)
            data.disable_depth_output &= !depth;
            for (unsigned i = 0; i < std::size(data.blend); i++)
              if (!(blends >> i & 1))
                data.blend[i] = {};
            data.next = &common;
            SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
            auto arguments = RootSignature(rootsig, signature.ptr(), &data);
            auto name =
                ps_name + "_" + std::to_string(blends | statistics << 8 | late << 9 | coarse << 10 | depth << 11);
            return CompileFunction(device_, this, std::span(&bytecode, 1), arguments, name.c_str(),
                                   [&shader](auto args, auto name, auto bitcode, auto error) {
                                     return SM50Compile(shader, args, name, bitcode, error);
                                   });
          };
    }

    // without a pixel shader, one that only does the tests a pixel shader would
    if ((conservative || depth_bounds_emulated) && !ps_func && !(ps_func = TestFragment(conservative, depth_bounds_emulated)))
      return E_FAIL;

    auto geometry_inputs = [&] {
      auto in = std::make_shared<GeometryInputs>();
      in->vs.assign((const char *)pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
      if (pDesc->GS.pShaderBytecode)
        in->gs.assign((const char *)pDesc->GS.pShaderBytecode, pDesc->GS.BytecodeLength);
      in->root_signature = pDesc->pRootSignature;
      in->elements = elements;
      in->ia_layout = data_ia_layout;
      in->so_elements = so_elements;
      in->so = so_data;
      in->stream_output = stream_output;
      in->common = common;
      in->ps = ps_func;
      in->clip_distance_varyings = clips;
      in->quad_lines = quad_lines;
      in->barycentrics = ps_func && ref_ps.PixelShader.Barycentrics;
      in->strip_cut = pDesc->IBStripCutValue;
      in->primitive = pDesc->PrimitiveTopologyType;
      in->conservative = conservative;
      in->flat_inputs = ps_func ? ref_ps.PixelShader.FlatInputs : 0;
      in->noperspective_inputs = ps_func ? ref_ps.PixelShader.NoPerspectiveInputs : 0;
      WMT::InitializeMeshRenderPipelineInfo(in->info);
      bool dual_source = false;
      SM50_SHADER_BLEND blend[8] = {};
      if (FAILED(InitializePSO(pDesc, in->info, dual_source, blend)))
        in.reset();
      return in;
    };

    if (mesh_shader) {
      auto compile = [&](sm50_shader_t shader, D3D12_SHADER_BYTECODE code, void *args, const char *name,
                         D3D12_SHADER_BYTECODE linked = {}) {
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
        return function(shader, code, root_signature(rootsig, args), name, linked);
      };
      // the mesh shader names its outputs as the pixel shader's inputs
      SM50_SHADER_MESH_SHADER_DATA link{&common, SM50_SHADER_MESH_SHADER, pDesc->PS.pShaderBytecode};
      if (conservative) {
        link.conservative = true;
        link.amplified = AS.pShaderBytecode;
        link.triangles = ref_ms.MeshConservativeTriangles;
        link.slices = (std::max(ref_ms.MeshMaxPrimitives, 1u) + link.triangles - 1) / link.triangles;
        link.flat_inputs = pixel_shader ? ref_ps.PixelShader.FlatInputs : 0;
        link.noperspective_inputs = pixel_shader ? ref_ps.PixelShader.NoPerspectiveInputs : 0;
        link.barycentrics = pixel_shader && ref_ps.PixelShader.Barycentrics;
      }
      // the amplification shader asks for a conservative mesh shader's slices
      SM50_SHADER_MESH_SHADER_DATA amplified = link;
      amplified.pixel_shader_bytecode = nullptr;
      WMT::Reference<WMT::Function> as_func, ms_func = compile(shader_ms, MS, &link, "ms_main", pDesc->PS);
      if (!ms_func ||
          (AS.pShaderBytecode && !(as_func = compile(shader_as, AS, conservative ? (void *)&amplified : &common, "as_main")))) {
        DumpShaders({AS, MS});
        return FAIL(E_INVALIDARG);
      }
      if (conservative && !AS.pShaderBytecode && !(as_func = ConservativeObject(link.slices)))
        return E_FAIL;
      mesh_info.object_function = as_func.handle;
      mesh_info.mesh_function = ms_func.handle;
      mesh_info.fragment_function = ps_func.handle;
      mesh_info.rasterization_enabled = true;
      // the most a payload may be (D3D12 mesh shader specification, "Amplification shader output payload")
      mesh_info.payload_memory_length = 16384;
      if (!(pso = CreatePipeline(mesh_info, err))) {
        ERR("Failed to create mesh shader PSO of shaders ", name, ": ", err.description().getUTF8String());
        DumpShaders({AS, MS, pDesc->PS});
        return E_FAIL;
      }
    } else if (geometry_shader) {
      // a strip's primitives share vertices, so strips and lists each have their pipeline
      auto inputs = geometry_inputs();
      if (!inputs)
        return FAIL(E_FAIL);
      // D3D12_PRIMITIVE_TOPOLOGY_TYPE's point, line and triangle are D3D10_SB_PRIMITIVE's
      auto variant = [&](bool strip, bool counting = false) {
        return GeometryVariant(
            inputs, shader_vs, shader_gs, pDesc->GS, strip, counting, pDesc->PrimitiveTopologyType, pass_through, hr
        );
      };
      bool strips = sm50_shader_t(shader_gs) ? ref_gs.GeometryShader.Primitive != D3D_PRIMITIVE_POINT
                              : pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      if (!patches && (!(pso = variant(false)) || (strips && !(pso_strip = variant(true)))))
        return hr;
      if (!patches && stream_output &&
          (!(so_count = variant(false, true)) || (strips && !(so_count_strip = variant(true, true)))))
        return hr;
      if (adjacency || patches)
        topology_inputs_ = inputs;
    } else if (!tessellation) {
      data_ia_layout.next = &clip_data;
      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      auto vs_func = function(shader_vs, pDesc->VS, root_signature(rootsig, &data_ia_layout), "vs_main");
      if (!vs_func)
        return FAIL(E_INVALIDARG);
      info.vertex_function = vs_func.handle;
      info.fragment_function = ps_func.handle;
      if (!(pso = CreatePipeline(info, err))) {
        ERR("Failed to create PSO of shaders ", name, ": ", err.description().getUTF8String());
        DumpShaders({pDesc->VS, pDesc->PS});
        return E_FAIL;
      }
      if (adjacency && !(topology_inputs_ = geometry_inputs()))
        return FAIL(E_FAIL);
    } else {
      // every factor there is, and what a vertex of the domain shader's takes of a mesh
      SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess{};
      pso_tess.type = SM50_SHADER_PSO_TESSELLATOR;
      pso_tess.max_potential_tess_factor = D3D12_TESSELLATOR_MAX_TESSELLATION_FACTOR;
      pso_tess.mesh_vertex_size = ref_ds.PostTessellator.MeshVertexSize;
      pso_tess.geometry = shader_gs;
      pso_tess.conservative = conservative;
      pso_tess.flat_inputs = pixel_shader ? ref_ps.PixelShader.FlatInputs : 0;
      pso_tess.noperspective_inputs = pixel_shader ? ref_ps.PixelShader.NoPerspectiveInputs : 0;
      pso_tess.barycentrics = conservative && pixel_shader && ref_ps.PixelShader.Barycentrics;
      pso_tess.next = &clip_data;
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
                                                pso_tess.mesh_vertex_size, pso_tess.conservative
                                            ).count,
                                 geometry ? ref_gs.GeometryShader.InstanceCount : 1, pso_tess.max_mesh_threadgroups
        ).parts;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_ds, rootsig_gs;
        root_signature(rootsig_gs, &pso_tess);
        rootsig_gs.type = SM50_SHADER_ROOT_SIGNATURE2;
        auto args_ds = root_signature(rootsig_ds, &rootsig_gs);
        auto ds_func = CompileFunction(
            device_, this, std::array{pDesc->HS, pDesc->DS, pDesc->GS}, args_ds, "ds_main",
            [&](auto args, auto name, auto bitcode, auto error) {
              return SM50CompileTessellationPipelineDomain(shader_hs, shader_ds, args, name, bitcode, error);
            }
        );
        if (!ds_func)
          return FAIL(E_INVALIDARG);
        mesh_info.mesh_function = ds_func.handle;

        // the object stage fetches indices itself, through the index buffer view each draw binds
        data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_VIEW;
        data_ia_layout.next = &pso_tess;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_hs, rootsig_vs;
        root_signature(rootsig_vs, &data_ia_layout);
        rootsig_vs.type = SM50_SHADER_ROOT_SIGNATURE2;
        auto args_hs = root_signature(rootsig_hs, &rootsig_vs);
        auto vshs_func = CompileFunction(
            device_, this, std::array{pDesc->VS, pDesc->HS, pDesc->GS}, args_hs, "vshs_main",
            [&](auto args, auto name, auto bitcode, auto error) {
              return SM50CompileTessellationPipelineHull(shader_vs, shader_hs, args, name, bitcode, error);
            }
        );
        if (!vshs_func)
          return FAIL(E_INVALIDARG);
        mesh_info.object_function = vshs_func.handle;
        pipeline_descriptors_.erase(pso.handle);
        if (!(pso = CreatePipeline(mesh_info, err))) {
          ERR("Failed to create tessellation PSO of shaders ", name, ": ", err.description().getUTF8String());
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
    InitializeDSSO(pDesc, depth_stencil2);

    InitializeRasterizerState(pDesc);
    if (quad_lines)
      RasterizeAsLines(*this);

    if (device_->NamesPasses())
      device_->PipelineMade(name, "graphics", start);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12GraphicsPipelineState: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12PipelineObject::QueryInterface(riid, ppvObject);
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) {
    return GetCachedPipelineBlob(blob);
  }

  WMT::RenderPipelineState
  RenderTargets(
      WMT::RenderPipelineState Pipeline, uint32_t Mask, uint32_t DSVPlanar, bool Statistics, bool Occluded, bool Coarse,
      WMTPixelFormat DSVFormat
  ) override {
    Mask &= rtv_mask;
    DSVPlanar &= dsv_mask;
    // a pipeline of any depth stencil format is made without one, which a pass without a view keeps
    bool any_format = dsv_any_format && DSVPlanar;
    Statistics &= pixel_shader || statistics_geometry_;
    Coarse &= pixel_shader && coarse_shading;
    // a pixel shader's discard of the depth bounds test takes its samples out of an occlusion query only before the
    // depth and stencil tests
    bool late = Occluded && depth_bounds_emulated && pixel_shader;
    if (!Pipeline ||
        (!Statistics && !late && !Coarse && Mask == rtv_mask && (dsv_any_format ? !DSVPlanar : DSVPlanar == dsv_mask)))
      return Pipeline;
    std::lock_guard<mullion::mutex> lock(render_targets_mutex_);
    auto described = pipeline_descriptors_.find(Pipeline.handle);
    // a pipeline with nothing to vary has no descriptor
    if (described == pipeline_descriptors_.end())
      return Pipeline;
    auto &descriptor = described->second;
    auto key = Mask | (DSVPlanar << D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) |
               (uint32_t(Statistics) << (D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 2)) |
               (uint32_t(late) << (D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 3)) |
               (uint32_t(Coarse) << (D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 4)) |
               (uint64_t(any_format ? DSVFormat : WMTPixelFormatInvalid) << 32);
    auto found = descriptor.variants.find(key);
    if (found != descriptor.variants.end())
      return found->second;
    WMT::Reference<WMT::Error> error;
    // Metal's pipeline formats must match its pass, including the slots with no texture (Metal Programming Guide,
    // Graphics Rendering: Render Command Encoder, Creating and Configuring a Render Pipeline Descriptor)
    auto pipeline = std::visit(
        [&](auto info) {
          // a pipeline without a pixel shader may have the depth bounds test's fragment function, which has no variants
          // a pipeline of any depth stencil format was converted without the depth its shader outputs, which a
          // variant with the view's depth keeps
          bool depth = any_format && (DSVPlanar & 1) && ref_ps.PixelShader.DepthOutput;
          if (auto blends = blend_mask_ & Mask;
              pixel_shader && (Statistics || late || Coarse || blends != blend_mask_ || depth)) {
            auto &ps = pixel_shaders_[blends | Statistics << 8 | late << 9 | Coarse << 10 | depth << 11];
            if (!ps && !(ps = pixel_shader_(Statistics, late, Coarse, blends, depth)))
              return WMT::Reference<WMT::RenderPipelineState>{};
            info.fragment_function = ps;
          }
          if constexpr (std::is_same_v<decltype(info), WMTMeshRenderPipelineInfo>)
            if (Statistics && descriptor.statistics_geometry_shader) {
              if (!descriptor.statistics_gs &&
                  !(descriptor.statistics_gs = descriptor.statistics_geometry_shader()))
                return WMT::Reference<WMT::RenderPipelineState>{};
              info.mesh_function = descriptor.statistics_gs;
            }
          for (unsigned i = 0; i < std::size(info.colors); i++)
            if (!(Mask & (1u << i)))
              info.colors[i] = {.pixel_format = WMTPixelFormatInvalid};
          if (!(DSVPlanar & 1))
            info.depth_pixel_format = WMTPixelFormatInvalid;
          else if (any_format)
            info.depth_pixel_format = DSVFormat;
          if (!(DSVPlanar & 2))
            info.stencil_pixel_format = WMTPixelFormatInvalid;
          else if (any_format)
            info.stencil_pixel_format = DSVFormat;
          return device_->GetMTLDevice().newRenderPipelineState(info, error);
        },
        descriptor.info
    );
    if (!pipeline) {
      ERR("Failed to create attachment PSO of shaders ", name, ": ", error.description().getUTF8String());
      return {};
    }
    return descriptor.variants.emplace(key, std::move(pipeline)).first->second;
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
    bool depth_bounds, D3D12_SHADER_BYTECODE AS, D3D12_SHADER_BYTECODE MS, const PipelineShaderCache *cached,
    const D3D12_DEPTH_STENCIL_DESC2 *depth_stencil2
) {
  InitReturnPtr(ppPipelineState);
  if (HRESULT hr = CheckCachedPipeline(pDesc->CachedPSO); FAILED(hr))
    return hr;
  auto desc = *pDesc;
  Com<ID3D12RootSignature> embedded;
  if (!desc.pRootSignature) {
    const void *signature = nullptr;
    UINT signature_size = 0;
    // one stage can supply the signature; every embedded signature must match (Microsoft Learn, Creating a Root
    // Signature, "Root Signature in Pipeline State Objects"; vkd3d-proton test_root_signature_embedded)
    for (auto code : {desc.VS, desc.HS, desc.DS, desc.GS, desc.PS, AS, MS}) {
      if (!code.pShaderBytecode)
        continue;
      if (!ShaderContainerHolds(code.pShaderBytecode, code.BytecodeLength))
        return FAIL(E_INVALIDARG);
      const void *part;
      UINT size;
      if (FAILED(microsoft::DXBCGetRootSignature(code.pShaderBytecode, &part, &size)))
        continue;
      if (signature) {
        if (size != signature_size || memcmp(part, signature, size))
          return FAIL(E_INVALIDARG);
      } else {
        HRESULT hr = CreateRootSignature(
            pDevice, 0, code.pShaderBytecode, code.BytecodeLength, __uuidof(ID3D12RootSignature),
            reinterpret_cast<void **>(&embedded)
        );
        if (FAILED(hr))
          return hr;
        signature = part;
        signature_size = size;
      }
    }
    if (!embedded)
      return FAIL(E_INVALIDARG);
    desc.pRootSignature = embedded.ptr();
  }
  auto pso = Com(new MTLD3D12GraphicsPipelineStateImpl(pDevice));
  if (HRESULT hr = PipelineKey(*pDesc, pso->library_key, depth_bounds, AS, MS, depth_stencil2); FAILED(hr))
    return hr;
  PipelineStateStream stream{.desc_graphics = *pDesc, .desc_as = AS, .desc_ms = MS, .depth_bounds = depth_bounds};
  if (depth_stencil2)
    stream.depth_stencil2 = *depth_stencil2;
  pso->borderless = Borderless(pDevice, stream);
  if (cached)
    pso->cached_shaders = *cached;
  pso->depth_bounds = depth_bounds && pDevice->GetMTLDevice().supportsFamily(WMTGPUFamilyApple10);
  pso->depth_bounds_emulated = depth_bounds && !pso->depth_bounds;
  HRESULT hr = pso->Initialize(&desc, AS, MS, depth_stencil2);
  if (FAILED(hr))
    return hr;
  return pso->QueryInterface(riid, ppPipelineState);
};

std::shared_ptr<const PipelineStateStream>
Borderless(MTLD3D12Device *device, const PipelineStateStream &stream) {
  if (device->custom_borders)
    return {};
  struct Owned : PipelineStateStream {
    std::deque<std::string> bytes;
    std::vector<D3D12_SO_DECLARATION_ENTRY> outputs;
    std::vector<UINT> strides;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    Com<ID3D12RootSignature> signature;
  };
  auto owned = std::make_shared<Owned>();
  static_cast<PipelineStateStream &>(*owned) = stream;
  auto keep = [&](const void *data, size_t size) { return owned->bytes.emplace_back((const char *)data, size).data(); };
  auto &cs = owned->desc_cs;
  auto &graphics = owned->desc_graphics;
  for (auto code : {&cs.CS, &graphics.VS, &graphics.PS, &graphics.DS, &graphics.HS, &graphics.GS, &owned->desc_as,
                    &owned->desc_ms})
    if (code->pShaderBytecode)
      code->pShaderBytecode = keep(code->pShaderBytecode, code->BytecodeLength);
  cs.CachedPSO = graphics.CachedPSO = {};
  owned->signature = cs.pRootSignature ? cs.pRootSignature : graphics.pRootSignature;
  auto &so = graphics.StreamOutput;
  owned->outputs.assign(so.pSODeclaration, so.pSODeclaration + so.NumEntries);
  for (auto &output : owned->outputs)
    if (output.SemanticName)
      output.SemanticName = keep(output.SemanticName, strlen(output.SemanticName) + 1);
  so.pSODeclaration = owned->outputs.data();
  owned->strides.assign(so.pBufferStrides, so.pBufferStrides + so.NumStrides);
  so.pBufferStrides = owned->strides.data();
  auto &layout = graphics.InputLayout;
  owned->elements.assign(layout.pInputElementDescs, layout.pInputElementDescs + layout.NumElements);
  for (auto &element : owned->elements)
    element.SemanticName = keep(element.SemanticName, strlen(element.SemanticName) + 1);
  layout.pInputElementDescs = owned->elements.data();
  return owned;
}

ID3D12PipelineState *
MTLD3D12PipelineState::Bordered(MTLD3D12Device *Device) {
  std::lock_guard<mullion::mutex> lock(shader_mutex);
  if (!bordered && FAILED(borderless->Create(Device, __uuidof(ID3D12PipelineState), (void **)&bordered)))
    ERR("pipeline ", name, ": the variant that emulates custom borders failed");
  return bordered.ptr();
}

HRESULT
PipelineStateStream::Key(Sha1Digest &key) const {
  return desc_cs.CS.pShaderBytecode ? PipelineKey(desc_cs, key)
                                  : PipelineKey(desc_graphics, key, depth_bounds, desc_as, desc_ms,
                                                depth_stencil2 ? &*depth_stencil2 : nullptr);
}

HRESULT
PipelineStateStream::Create(
    MTLD3D12Device *device, REFIID iid, void **pipeline, const PipelineShaderCache *cached
) const {
  auto compute = desc_cs;
  auto graphics = desc_graphics;
  if (cached)
    compute.CachedPSO = {}, graphics.CachedPSO = {};
  return compute.CS.pShaderBytecode
             ? CreateComputePipelineState(device, &compute, iid, pipeline, cached)
             : CreateGraphicsPipelineState(device, &graphics, iid, pipeline, depth_bounds, desc_as, desc_ms, cached,
                                           depth_stencil2 ? &*depth_stencil2 : nullptr);
}

} // namespace mullion
