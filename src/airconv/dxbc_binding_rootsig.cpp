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

#include "air_signature.hpp"
#include "dxbc_converter.hpp"
#include "dxbc_root_signature.hpp"
#include "shader_common.hpp"
#include "llvm/IR/DerivedTypes.h"
#include <algorithm>
#include <cassert>

namespace dxmt::dxbc {

using namespace llvm::air;

class RootSignatureBindingMap : public BindingMap {
public:
  // the root arguments and the static samplers are arguments of the function, or what a ray tracing shader took
  // from its context (bind_rootsig_arguments)
  llvm::Value *
  Table(llvm::air::AIRBuilder &AIR, uint32_t TableIndex) {
    if (TableIndex == RootSignatureArgumentIndex && RootArguments)
      return RootArguments;
    if (TableIndex == StaticSamplerArgumentIndex && StaticSamplers)
      return StaticSamplers;
    return AIR.builder.GetInsertBlock()->getParent()->getArg(TableIndex);
  }

  llvm::Value *
  GetArgument(llvm::air::AIRBuilder &AIR, uint32_t TableIndex, uint32_t Index) {
    auto &B = AIR.builder;
    auto Arg = Table(AIR, TableIndex);
    auto TyArg = llvm::cast<llvm::PointerType>(Arg->getType())->getNonOpaquePointerElementType();
    auto TyStruct = llvm::cast<llvm::StructType>(TyArg);
    return B.CreateLoad(TyStruct->getElementType(Index), B.CreateStructGEP(TyStruct, Arg, Index));
  };

  std::pair<llvm::Value *, llvm::Value *>
  GetBufferDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, llvm::Type *HandleType, RangeId RangeId,
      uint32_t DescriptorOffset
  ) {
    auto TyBufferDescriptor = llvm::StructType::get(
        AIR.getContext(), {HandleType, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto Pointer = B.CreatePointerCast(
        IntPtr, TyBufferDescriptor->getPointerTo(IntPtr->getType()->getPointerAddressSpace())
    );
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return {
        B.CreateLoad(
            HandleType, B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(0) /* pointer */})
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(1) /* metadata */})
        )
    };
  }

  llvm::Value *
  GetUAVCounterDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset
  ) {
    auto TyCounter = AIR.getIntTy()->getPointerTo(1);
    auto TyBufferDescriptor = llvm::StructType::get(
        AIR.getContext(), {llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext()),
                           TyCounter, llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto Pointer = B.CreatePointerCast(
        IntPtr, TyBufferDescriptor->getPointerTo(IntPtr->getType()->getPointerAddressSpace())
    );
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return B.CreateLoad(
        TyCounter, B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(2) /* metadata */})
    );
  }

  llvm::Value *
  GetRootConstantPointer(llvm::air::AIRBuilder &AIR, uint32_t TableIndex, uint32_t Index) {
    auto &B = AIR.builder;
    auto Arg = Table(AIR, TableIndex);
    auto TyArg = llvm::cast<llvm::PointerType>(Arg->getType())->getNonOpaquePointerElementType();
    auto TyStruct = llvm::cast<llvm::StructType>(TyArg);
    return B.CreatePointerCast(
        B.CreateStructGEP(TyStruct, Arg, Index), AIR.getIntTy(4)->getPointerTo(Arg->getType()->getPointerAddressSpace())
    );
  };

  virtual llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = ConstantBuffers.find(Range);
    if (Iter == ConstantBuffers.end())
      return {};
    auto &CBuf = Iter->second.first;
    auto DescriptorOffset = Iter->second.second;
    if (DescriptorOffset == ~0u - 1) {
      auto Pointer = GetRootConstantPointer(Builder, RootSignatureArgumentIndex, CBuf.arg_index);
      return ConstantBufferDescriptor{Pointer, nullptr};
    }
    if (DescriptorOffset == ~0u) {
      auto Pointer = GetArgument(Builder, RootSignatureArgumentIndex, CBuf.arg_index);
      return ConstantBufferDescriptor{Pointer, nullptr};
    }
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, CBuf.arg_index);
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy(4)->getPointerTo(2), CBuf.range.lower_bound, DescriptorOffset
    );
    return ConstantBufferDescriptor{Pointer, Metadata};
  }

  SamplerDescriptor
  GetSamplerDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset
  ) {
    auto TySampleHandler = AIR.getSamplerHandleType();
    auto TyInt64 = llvm::Type::getInt64Ty(AIR.getContext());
    auto TySamplerDescriptor =
        llvm::StructType::get(AIR.getContext(), {TySampleHandler, TySampleHandler, TyInt64, TyInt64});
    auto &B = AIR.builder;
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    auto Field = [&](llvm::Type *Ty, unsigned Field) {
      return B.CreateLoad(
          Ty, B.CreateGEP(
                  TySamplerDescriptor, B.CreatePointerCast(IntPtr, TySamplerDescriptor->getPointerTo(2)),
                  {IdxDescriptor, AIR.getInt(Field)}
              )
      );
    };
    // sampler, cube_sampler, metadata, border
    return {Field(TySampleHandler, 0), Field(TySampleHandler, 1), Field(TyInt64, 2), Field(TyInt64, 3)};
  }

  virtual llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto Iter = Samplers.find(Range);
    if (Iter == Samplers.end()) {
      return {};
    }
    auto &Sampler = Iter->second.first;
    auto DescriptorOffset = Iter->second.second;
    if (DescriptorOffset == ~0u) {
      if (~StaticSamplerArgumentIndex == 0)
        return {};
      return GetSamplerDescriptor(
          Builder, Table(Builder, StaticSamplerArgumentIndex), Index,
          Sampler.range.lower_bound, Sampler.arg_index
      );
    }
    if (~RootSignatureArgumentIndex == 0)
      return {};

    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, Sampler.arg_index);
    return GetSamplerDescriptor(Builder, HeapPointer, Index, Sampler.range.lower_bound, DescriptorOffset);
  }

  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
  GetTextureDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, Texture::ResourceKind Kind, RangeId RangeId,
      uint32_t DescriptorOffset
  ) {
    auto TyTextureHandle = AIR.getTextureHandleType(Kind);
    auto TyTextureDescriptor = llvm::StructType::get(
        AIR.getContext(), {TyTextureHandle, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto Pointer = B.CreatePointerCast(
        IntPtr, TyTextureDescriptor->getPointerTo(IntPtr->getType()->getPointerAddressSpace())
    );
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return {
        B.CreateLoad(
            TyTextureHandle, //
            B.CreateGEP(TyTextureDescriptor, Pointer, {IdxDescriptor, AIR.getInt(0) /* pointer */})
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(TyTextureDescriptor, Pointer, {IdxDescriptor, AIR.getInt(1) /* metadata */})
        ),
        B.CreateGEP(
            TyTextureDescriptor, Pointer, {IdxDescriptor, AIR.getInt(2) /* the format range (SM50_SAMPLER_BORDER) */}
        )
    };
  }

  virtual llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = SRVs.find(Range);
    if (Iter == SRVs.end())
      return {};
    auto &SRV = Iter->second.first;
    if (SRV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, SRV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto ResourceKindLogical = air::to_air_resource_type(SRV.resource_type, SRV.compared);
    auto ResourceKind = air::lowering_texture_1d_to_2d(ResourceKindLogical);
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(SRV.scaler_type)
    );
    auto MemoryAccess = SRV.sampled ? Texture::MemoryAccess::access_sample : Texture::MemoryAccess::access_read;

    auto [Handle, Metadata, FormatRange] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, SRV.range.lower_bound, DescriptorOffset);
    return TextureDescirptor{Handle,       Metadata,   false, ResourceKind, ResourceKindLogical,
                             MemoryAccess, SampleType, FormatRange};
  }

  virtual llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = UAVs.find(Range);
    if (Iter == UAVs.end())
      return {};
    auto &UAV = Iter->second.first;
    if (UAV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, UAV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto ResourceKindLogical = air::to_air_resource_type(UAV.resource_type);
    auto ResourceKind = air::lowering_texture_1d_to_2d(ResourceKindLogical);
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(UAV.scaler_type)
    );
    auto MemoryAccess = UAV.written
                            ? (UAV.read ? Texture::MemoryAccess::acesss_readwrite : Texture::MemoryAccess::access_write)
                            : Texture::MemoryAccess::access_read;
    auto [Handle, Metadata, FormatRange] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, UAV.range.lower_bound, DescriptorOffset);
    return TextureDescirptor{Handle,       Metadata,  UAV.global_coherent, ResourceKind, ResourceKindLogical,
                             MemoryAccess, SampleType};
  }

  virtual llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = SRVs.find(Range);
    if (Iter == SRVs.end())
      return {};
    auto &SRV = Iter->second.first;
    if (SRV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, SRV.arg_index);
    if (DescriptorOffset == ~0u) {
      return BufferDescriptor{HeapPointer, Builder.builder.getInt64(0xffffffff), SRV.structure_stride, false};
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), SRV.range.lower_bound, DescriptorOffset
    );

    return BufferDescriptor{Pointer, Metadata, SRV.structure_stride, false};
  }

  virtual llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = UAVs.find(Range);
    if (Iter == UAVs.end())
      return {};
    auto &UAV = Iter->second.first;
    if (UAV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, UAV.arg_index);
    if (DescriptorOffset == ~0u) {
      return BufferDescriptor{
          HeapPointer, Builder.builder.getInt64(0xffffffff), UAV.structure_stride, UAV.global_coherent
      };
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), UAV.range.lower_bound, DescriptorOffset
    );

    return BufferDescriptor{Pointer, Metadata, UAV.structure_stride, UAV.global_coherent};
  }

  virtual llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = UAVs.find(Range);
    if (Iter == UAVs.end())
      return {};
    auto &UAV = Iter->second.first;
    if (UAV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, UAV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can not have counters associated
    auto Pointer = GetUAVCounterDescriptor(Builder, HeapPointer, Index, UAV.range.lower_bound, DescriptorOffset);

    return CounterDescriptor{Pointer};
  }

  llvm::Value *
  GetAtomicLocks(llvm::air::AIRBuilder &Builder) override {
    return GetArgument(Builder, RootSignatureArgumentIndex, AtomicLocksArgIndex);
  }

  uint32_t RootSignatureArgumentIndex = ~0u;
  uint32_t StaticSamplerArgumentIndex = ~0u;
  uint32_t AtomicLocksArgIndex = ~0u;
  llvm::StructType *RootArgumentsType = nullptr;
  llvm::Value *RootArguments = nullptr;
  llvm::Value *StaticSamplers = nullptr;

  std::map<RangeId, std::pair<ConstantBufferInfo, uint64_t>> ConstantBuffers;
  std::map<RangeId, std::pair<SamplerInfo, uint64_t>> Samplers;
  std::map<RangeId, std::pair<ShaderResourceViewInfo, uint64_t>> SRVs;
  std::map<RangeId, std::pair<UnorderedAccessViewInfo, uint64_t>> UAVs;
};

std::unique_ptr<BindingMap>
setup_binding_rootsig(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type, const void *bytecode, size_t bytecode_length,
    uint32_t root_sig_slot, uint32_t static_sampler_slot, bool local
) {
  using namespace microsoft;
  using namespace dxmt::dxbc;
  using namespace dxmt::air;
  using namespace dxmt::shader::common;

  auto binding_map = std::make_unique<RootSignatureBindingMap>();

  const void *pRawRootSig;
  UINT RawRootSigSize;
  HRESULT hr = microsoft::DXBCGetRootSignature(bytecode, &pRawRootSig, &RawRootSigSize);
  if (FAILED(hr))
    return {};

  RootSignatureDeserializer deserializer;
  hr = deserializer.Deserialize(pRawRootSig, RawRootSigSize);
  if (FAILED(hr))
    return {};

  auto &root_sig = deserializer.desc_1_1_.Desc_1_1;

  air::ArgumentBufferBuilder builder;
  unsigned inc_attribute_index = 0;
  // root constants: their first field and their DWORDs
  std::vector<std::pair<uint32_t, uint32_t>> constants_fields;

  auto check_visibility = [=](D3D12_SHADER_VISIBILITY visibility) -> bool {
    if (visibility == D3D12_SHADER_VISIBILITY_ALL)
      return true;
    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_VERTEX:
      if (shader_type == D3D10_SB_VERTEX_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_HULL:
      if (shader_type == D3D11_SB_HULL_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_DOMAIN:
      if (shader_type == D3D11_SB_DOMAIN_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_GEOMETRY:
      if (shader_type == D3D10_SB_GEOMETRY_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_PIXEL:
      if (shader_type == D3D10_SB_PIXEL_SHADER)
        return true;
      break;
    default:
      break;
    }
    // D3D12_SHADER_VISIBILITY_AMPLIFICATION and _MESH, which the bundled header does not have
    if (visibility == D3D12_SHADER_VISIBILITY(6))
      return shader_type == D3D12_SB_AMPLIFICATION_SHADER;
    if (visibility == D3D12_SHADER_VISIBILITY(7))
      return shader_type == D3D12_SB_MESH_SHADER;
    return false;
  };

  auto range_contained = [=](const ResourceRange &range, const D3D12_DESCRIPTOR_RANGE1 &range_rootsig) -> bool {
    if (range.lower_bound < range_rootsig.BaseShaderRegister)
      return false;
    if (range_rootsig.NumDescriptors == ~0u)
      return true;
    if (range.size == ~0u)
      return true;
    if (range.lower_bound + range.size <= range_rootsig.BaseShaderRegister + range_rootsig.NumDescriptors)
      return true;
    return false;
  };

  for (unsigned i = 0; i < root_sig.NumParameters; i++) {
    auto &Parameter = root_sig.pParameters[i];

    switch (Parameter.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      auto &table = Parameter.DescriptorTable;
      unsigned descriptor_offset = 0;
      auto table_index = builder.Size();
      std::optional<uint32_t> raster_order_group;
      for (unsigned i = 0; check_visibility(Parameter.ShaderVisibility) && i < table.NumDescriptorRanges; i++) {
        auto &range = table.pDescriptorRanges[i];
        auto calculated_descriptor_offset = range.OffsetInDescriptorsFromTableStart == ~0u
                                                ? descriptor_offset
                                                : range.OffsetInDescriptorsFromTableStart;
        descriptor_offset = calculated_descriptor_offset + range.NumDescriptors;
        switch (range.RangeType) {
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: {
          for (auto &[_, srv] : shader_info->srvMap) {
            if (srv.range.space != range.RegisterSpace)
              continue;
            if (range_contained(srv.range, range)) {
              auto range_id = srv.range.range_id;
              binding_map->SRVs[range_id] = {
                  srv, calculated_descriptor_offset + (srv.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->SRVs[range_id].first.arg_index = table_index;
            }
          }
          break;
        }
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
          for (auto &[_, uav] : shader_info->uavMap) {
            if (uav.range.space != range.RegisterSpace)
              continue;
            if (range_contained(uav.range, range)) {
              auto range_id = uav.range.range_id;
              binding_map->UAVs[range_id] = {
                  uav, calculated_descriptor_offset + (uav.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->UAVs[range_id].first.arg_index = table_index;
              if (uav.rasterizer_order)
                raster_order_group = 1;
            }
          }
          break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
          for (auto &[_, cbuf] : shader_info->cbufferMap) {
            if (cbuf.range.space != range.RegisterSpace)
              continue;
            if (range_contained(cbuf.range, range)) {
              auto range_id = cbuf.range.range_id;
              binding_map->ConstantBuffers[range_id] = {
                  cbuf, calculated_descriptor_offset + (cbuf.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->ConstantBuffers[range_id].first.arg_index = table_index;
            }
          }
          break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
          for (auto &[_, sampler] : shader_info->samplerMap) {
            if (sampler.range.space != range.RegisterSpace)
              continue;
            if (range_contained(sampler.range, range)) {
              auto range_id = sampler.range.range_id;
              binding_map->Samplers[range_id] = {
                  sampler, calculated_descriptor_offset + (sampler.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->Samplers[range_id].first.arg_index = table_index;
            }
          }
          break;
        default:
          assert(0 && "unknown range type");
          return {};
        }
      }
      // ordered descriptor reads precede the UAV accesses that depend on them (MSL 5.2.1.2).
      builder.DefineBuffer(
          "table" + std::to_string(i), raster_order_group ? AddressSpace::device : AddressSpace::constant,
          MemoryAccess::read, msl_int, inc_attribute_index++, raster_order_group
      );
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
      auto &constants = Parameter.Constants;
      auto qwords = (constants.Num32BitValues >> 1) + (constants.Num32BitValues & 1);
      auto first = builder.DefineInteger64("root_constant" + std::to_string(i), inc_attribute_index++);
      for (unsigned x = 1; x < qwords; x++) {
        builder.DefineInteger64("root_constant" + std::to_string(i) + "_" + std::to_string(x), inc_attribute_index++);
      }
      constants_fields.push_back({first, constants.Num32BitValues});
      if (!check_visibility(Parameter.ShaderVisibility))
        break;
      for (auto &[_, cbuf] : shader_info->cbufferMap) {
        if (cbuf.range.lower_bound == constants.ShaderRegister && cbuf.range.space == constants.RegisterSpace) {
          auto range_id = cbuf.range.range_id;
          binding_map->ConstantBuffers[range_id] = {cbuf, (~0u - 1)};
          binding_map->ConstantBuffers[range_id].first.arg_index = first;
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_CBV: {
      auto &descriptor = Parameter.Descriptor;
      auto descriptor_index = builder.DefineBuffer(
          "cb" + std::to_string(i), AddressSpace::constant, MemoryAccess::read, msl_uint4, inc_attribute_index++
      );
      if (!check_visibility(Parameter.ShaderVisibility))
        break;
      for (auto &[_, cbuf] : shader_info->cbufferMap) {
        if (cbuf.range.lower_bound == descriptor.ShaderRegister && cbuf.range.space == descriptor.RegisterSpace) {
          auto range_id = cbuf.range.range_id;
          binding_map->ConstantBuffers[range_id] = {cbuf, ~0u};
          binding_map->ConstantBuffers[range_id].first.arg_index = descriptor_index;
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_SRV: {
      auto &descriptor = Parameter.Descriptor;
      auto descriptor_index = builder.DefineBuffer(
          "t" + std::to_string(i), AddressSpace::device, MemoryAccess::read, msl_uint, inc_attribute_index++
      );
      if (!check_visibility(Parameter.ShaderVisibility))
        break;
      for (auto &[_, srv] : shader_info->srvMap) {
        if (srv.range.lower_bound == descriptor.ShaderRegister && srv.range.space == descriptor.RegisterSpace) {
          assert(srv.resource_type == shader::common::ResourceType::NonApplicable);
          auto range_id = srv.range.range_id;
          binding_map->SRVs[range_id] = {srv, ~0u};
          binding_map->SRVs[range_id].first.arg_index = descriptor_index;
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_UAV: {
      auto &descriptor = Parameter.Descriptor;
      auto descriptor_index = builder.Size();
      std::optional<uint32_t> raster_order_group;
      if (check_visibility(Parameter.ShaderVisibility))
        for (auto &[_, uav] : shader_info->uavMap) {
          if (uav.range.lower_bound == descriptor.ShaderRegister && uav.range.space == descriptor.RegisterSpace) {
            assert(uav.resource_type == shader::common::ResourceType::NonApplicable);
            auto range_id = uav.range.range_id;
            binding_map->UAVs[range_id] = {uav, ~0u};
            binding_map->UAVs[range_id].first.arg_index = descriptor_index;
            raster_order_group = uav.rasterizer_order ? std::optional(1) : std::nullopt;
          }
        }
      builder.DefineBuffer(
          "u" + std::to_string(i), AddressSpace::device, MemoryAccess::read_write /* FIXME */, msl_uint,
          inc_attribute_index++, raster_order_group
      );
      break;
    }
    }
  }

  uint32_t parameter_fields = builder.Size();
  // directly indexed heaps, declared after the parameters as the root signature lays them out (ResourceHeapQword,
  // SamplerHeapQword). a heap-space range (SM 6.6 createHandleFromHeap) covers its heap from descriptor 0
  auto bind_heap = [&](auto &binding, auto &ranges, uint32_t heap_index) {
    for (auto &[_, info] : ranges)
      if (info.range.space == kDescriptorHeapSpace) {
        binding[info.range.range_id] = {info, 0};
        binding[info.range.range_id].first.arg_index = heap_index;
      }
  };
  // D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED and _SAMPLER_HEAP_DIRECTLY_INDEXED, which the native
  // headers' enumeration stops before
  constexpr uint32_t kResourceHeapDirectlyIndexed = 0x400, kSamplerHeapDirectlyIndexed = 0x800;
  if (root_sig.Flags & kResourceHeapDirectlyIndexed) {
    auto ordered = std::any_of(shader_info->uavMap.begin(), shader_info->uavMap.end(), [&](auto &entry) {
      return entry.second.range.space == kDescriptorHeapSpace && entry.second.rasterizer_order;
    });
    auto heap_index = builder.DefineBuffer(
        "resource_heap", ordered ? AddressSpace::device : AddressSpace::constant, MemoryAccess::read, msl_int,
        inc_attribute_index++, ordered ? std::optional(1) : std::nullopt
    );
    bind_heap(binding_map->SRVs, shader_info->srvMap, heap_index);
    bind_heap(binding_map->UAVs, shader_info->uavMap, heap_index);
    bind_heap(binding_map->ConstantBuffers, shader_info->cbufferMap, heap_index);
  }
  if (root_sig.Flags & kSamplerHeapDirectlyIndexed)
    bind_heap(
        binding_map->Samplers, shader_info->samplerMap,
        builder.DefineBuffer("sampler_heap", AddressSpace::constant, MemoryAccess::read, msl_int, inc_attribute_index++)
    );
  // then the lock table of 64-bit atomics (AtomicLocksQword)
  binding_map->AtomicLocksArgIndex =
      builder.DefineBuffer("atomic_locks", AddressSpace::device, MemoryAccess::read_write, msl_uint, inc_attribute_index++);

  auto [type, metadata] = builder.Build(module.getContext(), module.getDataLayout());
  binding_map->RootArgumentsType = type;
  if (local) {
    // a shader record's local root arguments: each aligned to its own size, in the parameters' order, so that root
    // constants are DWORDs with no padding of their own (DXR spec, "Shader table memory initialization"), where the
    // global block gives them whole qwords. the record has only the parameters' arguments
    std::vector<llvm::Type *> fields;
    std::vector<uint32_t> moved(type->getNumElements());
    auto constants = constants_fields.begin();
    for (uint32_t i = 0; i < parameter_fields;) {
      moved[i] = fields.size();
      if (constants != constants_fields.end() && constants->first == i) {
        fields.insert(fields.end(), constants->second, llvm::Type::getInt32Ty(module.getContext()));
        i += (constants->second + 1) / 2;
        ++constants;
      } else {
        fields.push_back(type->getElementType(i++));
      }
    }
    binding_map->RootArgumentsType = llvm::StructType::get(module.getContext(), fields);
    auto move = [&](auto &ranges) {
      std::erase_if(ranges, [&](auto &range) { return range.second.first.arg_index >= parameter_fields; });
      for (auto &[_, range] : ranges)
        range.first.arg_index = moved[range.first.arg_index];
    };
    move(binding_map->ConstantBuffers);
    move(binding_map->Samplers);
    move(binding_map->SRVs);
    move(binding_map->UAVs);
    binding_map->RootSignatureArgumentIndex = 0;
  } else {
    binding_map->RootSignatureArgumentIndex = func_signature.DefineInput(air::ArgumentBindingIndirectBuffer{
        .location_index = root_sig_slot, // SM50_BINDING_INDEX_ROOT_ARGUMENTS
        .array_size = 1,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::constant,
        .struct_type = type,
        .struct_type_info = metadata,
        .arg_name = "rootsig",
    });
  }

  for (unsigned i = 0; i < root_sig.NumStaticSamplers; i++) {
    auto &State = root_sig.pStaticSamplers[i];

    for (auto &[_, sampler] : shader_info->samplerMap) {
      if (sampler.range.space != State.RegisterSpace)
        continue;
      if (sampler.range.lower_bound != State.ShaderRegister)
        continue;
      if (!check_visibility(State.ShaderVisibility))
        break;
      auto range_id = sampler.range.range_id;
      binding_map->Samplers[range_id] = {sampler, ~0u};
      binding_map->Samplers[range_id].first.arg_index = i;
    }
  }

  // ordinary stages need every resource covered; ray shaders share global and local maps (Microsoft Learn,
  // Creating a Root Signature, "Root Signature in Pipeline State Objects")
  if (shader_type != kShaderLibrary &&
      (binding_map->ConstantBuffers.size() != shader_info->cbufferMap.size() ||
       binding_map->Samplers.size() != shader_info->samplerMap.size() ||
       binding_map->SRVs.size() != shader_info->srvMap.size() || binding_map->UAVs.size() != shader_info->uavMap.size()))
    return {};
  if (root_sig.NumStaticSamplers == 0)
    return binding_map;

  // a shader record has no static samplers: its shader is told where they are (bind_rootsig_arguments), which
  // stands for an argument after the record
  binding_map->StaticSamplerArgumentIndex =
      local ? binding_map->RootSignatureArgumentIndex + 1
            : func_signature.DefineInput(air::ArgumentBindingBuffer{
                  .buffer_size = {},
                  .location_index = static_sampler_slot, // SM50_BINDING_INDEX_STATIC_SAMPLERS
                  .array_size = 0,
                  .memory_access = air::MemoryAccess::read,
                  .address_space = air::AddressSpace::constant,
                  .type = air::msl_ulong,
                  .arg_name = "static_samplers",
                  .raster_order_group = {},
              });

  return binding_map;
}

void
bind_rootsig_arguments(
    BindingMap &map, llvm::IRBuilder<> &builder, llvm::Value *arguments, llvm::Value *static_samplers
) {
  auto &rootsig = static_cast<RootSignatureBindingMap &>(map);
  rootsig.RootArguments = builder.CreatePointerCast(
      arguments, rootsig.RootArgumentsType->getPointerTo(arguments->getType()->getPointerAddressSpace())
  );
  rootsig.StaticSamplers = static_samplers;
}

class CombinedBindingMap : public BindingMap {
public:
  std::unique_ptr<BindingMap> maps[2];

  template <typename Get>
  auto
  First(Get &&get) -> decltype(get(*maps[0])) {
    for (auto &map : maps)
      if (map)
        if (auto found = get(*map))
          return found;
    return {};
  }

  llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetConstantBuffer(Builder, Range, Index); });
  }
  llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetSampler(Builder, Range, Index); });
  }
  llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetSRVTexture(Builder, Range, Index); });
  }
  llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetUAVTexture(Builder, Range, Index); });
  }
  llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetSRVBuffer(Builder, Range, Index); });
  }
  llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetUAVBuffer(Builder, Range, Index); });
  }
  llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetUAVCounter(Builder, Range, Index); });
  }
  // the device's lock table comes with the global root arguments, which are the second map
  llvm::Value *
  GetAtomicLocks(llvm::air::AIRBuilder &Builder) override {
    return maps[1] ? maps[1]->GetAtomicLocks(Builder) : nullptr;
  }
};

std::unique_ptr<BindingMap>
combine_bindings(std::unique_ptr<BindingMap> first, std::unique_ptr<BindingMap> second) {
  auto combined = std::make_unique<CombinedBindingMap>();
  combined->maps[0] = std::move(first);
  combined->maps[1] = std::move(second);
  return combined;
}

} // namespace dxmt::dxbc
