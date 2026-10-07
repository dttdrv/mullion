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

#include "dxbc_converter_base.hpp"
#include "../dxbc_converter.hpp"
#include "air_builder.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

namespace dxmt::dxbc {

llvm::ArrayType *
GetArrayType(llvm::Value *Array) {
  return llvm::cast<llvm::ArrayType>(llvm::cast<llvm::PointerType>(Array->getType())->getNonOpaquePointerElementType());
}

llvm::Value *
Converter::LoadOperandIndex(const IndexByTempComponent &SrcOpIndex) {
  auto Comp = SrcOpIndex.component;
  SrcOperandTemp RelReg{
      ._ = {{Comp, Comp, Comp, Comp}, false, false, OperandDataType::Integer},
      .regid = SrcOpIndex.regid,
      .phase = SrcOpIndex.phase,
  };
  auto Rel = LoadOperand(RelReg, 0b1);
  return ir.CreateAdd(Rel, ir.getInt32(SrcOpIndex.offset));
}

llvm::Value *
Converter::LoadOperandIndex(const IndexByIndexableTempComponent &SrcOpIndex) {
  auto Comp = SrcOpIndex.component;
  SrcOperandIndexableTemp RelReg{
      ._ = {{Comp, Comp, Comp, Comp}, false, false, OperandDataType::Integer},
      .regfile = SrcOpIndex.regfile,
      .regindex = SrcOpIndex.regid,
      .phase = SrcOpIndex.phase,
  };
  auto Rel = LoadOperand(RelReg, 0b1);
  return ir.CreateAdd(Rel, ir.getInt32(SrcOpIndex.offset));
}

llvm::Value *
Converter::LoadOperand(const SrcOperandConstantBuffer &SrcOp, mask_t Mask) {

  auto RangeId = SrcOp.rangeid;

  auto descriptor = ctx.binding.GetConstantBuffer(air, RangeId, LoadOperandIndex(SrcOp.rangeindex));
  if (!descriptor)
    return ApplySrcModifier(SrcOp._, llvm::ConstantAggregateZero::get(air.getIntTy(4)), Mask);
  auto Row = LoadOperandIndex(SrcOp.regindex);
  return ApplySrcModifier(
      SrcOp._, LoadConstantBuffer(*descriptor, Row, ComponentFromScalarMask(Mask, SrcOp._.swizzle)), Mask
  );
}

llvm::Value *
Converter::LoadConstantBuffer(const ConstantBufferDescriptor &Buffer, llvm::Value *Row, int Component) {
  auto TyIntVec4 = air.getIntTy(4);
  llvm::Value *InBounds = nullptr;
  // rows past the bound buffer's size read 0 (D3D11.3 7.5); root CBVs carry no size
  if (Buffer.Metadata) {
    InBounds = ir.CreateICmpULT(Row, ir.CreateTrunc(ir.CreateLShr(Buffer.Metadata, 4), air.getIntTy()));
    Row = ir.CreateSelect(InBounds, Row, ir.getInt32(0));
  }
  auto Value =
      Component >= 0
          ? ir.CreateLoad(air.getIntTy(), ir.CreateGEP(TyIntVec4, Buffer.Pointer, {Row, ir.getInt32(Component)}))
          : ir.CreateLoad(TyIntVec4, ir.CreateGEP(TyIntVec4, Buffer.Pointer, {Row}));
  return InBounds ? ir.CreateSelect(InBounds, Value, llvm::Constant::getNullValue(Value->getType())) : Value;
}

llvm::Value *
Converter::LoadOperand(const SrcOperandImmediateConstantBuffer &SrcOp, mask_t Mask) {
  auto Handle = res.icb;
  auto TyHandle = GetArrayType(Handle);

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto TyInt = air.getIntTy();
    auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), LoadOperandIndex(SrcOp.regindex), ir.getInt32(Comp)});
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec4 = air.getIntTy(4);
  auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), LoadOperandIndex(SrcOp.regindex)});
  auto ValueIntVec4 = ir.CreateLoad(TyIntVec4, Ptr);
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandImmediate32 &SrcOp, mask_t Mask) {
  llvm::SmallVector<llvm::Constant *> Constants;
  switch (SrcOp._.read_type) {
  case OperandDataType::Float:
    switch (Mask) {
    case 0b1:
      return air.getFloat(SrcOp.fvalue[0]);
    case 0b10:
      return air.getFloat(SrcOp.fvalue[1]);
    case 0b100:
      return air.getFloat(SrcOp.fvalue[2]);
    case 0b1000:
      return air.getFloat(SrcOp.fvalue[3]);
    default:
      for (int i = 0; i < 4; i++) {
        if (Mask & (1 << i)) {
          Constants.push_back(air.getFloat(SrcOp.fvalue[i]));
        }
      }
      break;
    }
    break;
  case OperandDataType::Integer:
    switch (Mask) {
    case 0b1:
      return air.getInt(SrcOp.uvalue[0]);
    case 0b10:
      return air.getInt(SrcOp.uvalue[1]);
    case 0b100:
      return air.getInt(SrcOp.uvalue[2]);
    case 0b1000:
      return air.getInt(SrcOp.uvalue[3]);
    default:
      for (int i = 0; i < 4; i++) {
        if (Mask & (1 << i)) {
          Constants.push_back(air.getInt(SrcOp.uvalue[i]));
        }
      }
      break;
    }
    break;
  case OperandDataType::Half16X16:
    // in case f16to32 reads a literal (doesn't make much sense)
    switch (Mask) {
    case 0b1:
      return air.getHalf(SrcOp.uvalue[0]);
    case 0b10:
      return air.getHalf(SrcOp.uvalue[1]);
    case 0b100:
      return air.getHalf(SrcOp.uvalue[2]);
    case 0b1000:
      return air.getHalf(SrcOp.uvalue[3]);
    default:
      for (int i = 0; i < 4; i++) {
        if (Mask & (1 << i)) {
          Constants.push_back(air.getHalf(SrcOp.uvalue[i]));
        }
      }
      break;
    }
    break;
  default:
    llvm_unreachable("unhandled operand data type");
  }
  return llvm::ConstantVector::get(Constants);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandInput &SrcOp, mask_t Mask) {

  auto Handle = res.input.ptr_int4;
  auto TyHandle = GetArrayType(Handle);

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto TyInt = air.getIntTy();
    auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), ir.getInt32(SrcOp.regid), ir.getInt32(Comp)});
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec4 = air.getIntTy(4);
  auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), ir.getInt32(SrcOp.regid)});
  auto ValueIntVec4 = ir.CreateLoad(TyIntVec4, Ptr);
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandTemp &SrcOp, mask_t Mask) {

  auto Handle = res.temp.ptr_int4;

  if (SrcOp.phase != ~0u) {
    Handle = res.phases[SrcOp.phase].temp.ptr_int4;
  }
  auto TyHandle = GetArrayType(Handle);
  auto TyInt = air.getIntTy();

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto Ptr = ir.CreateGEP(
        TyHandle, Handle,
        {ir.getInt32(0), ir.CreateAdd(ir.CreateShl(ir.getInt32(SrcOp.regid), 2), ir.getInt32(Comp))}
    );
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  llvm::Value *ValueIntVec4 = llvm::PoisonValue::get(air.getIntTy(4));
  for (auto [DstComp, _] : EnumerateComponents(MemoryAccessMask(Mask, SrcOp._.swizzle))) {
    auto Ptr = ir.CreateGEP(
        TyHandle, Handle,
        {ir.getInt32(0), ir.CreateAdd(ir.CreateShl(ir.getInt32(SrcOp.regid), 2), ir.getInt32(DstComp))}
    );
    ValueIntVec4 = ir.CreateInsertElement(ValueIntVec4, ir.CreateLoad(TyInt, Ptr), DstComp);
  }
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandAttribute &SrcOp, mask_t Mask) {
  using shader::common::InputAttribute;
  llvm::Value *Ret = llvm::PoisonValue::get(air.getIntTy(4));
  switch (SrcOp.attribute) {
  case InputAttribute::VertexId:
  case InputAttribute::InstanceId:
    assert(0 && "never reached: should be handled separately");
    break;
  case InputAttribute::ThreadId: {
    Ret = res.thread_id_arg;
    break;
  }
  case InputAttribute::ThreadIdInGroup: {
    Ret = res.thread_id_in_group_arg;
    break;
  }
  case InputAttribute::ThreadGroupId: {
    Ret = res.thread_group_id_arg;
    break;
  }
  case InputAttribute::ThreadIdInGroupFlatten: {
    Ret = res.thread_id_in_group_flat_arg;
    break;
  }

  case InputAttribute::OutputControlPointId:
  case InputAttribute::ForkInstanceId:
  case InputAttribute::JoinInstanceId: {
    Ret = res.thread_id_in_patch;
    break;
  }
  case InputAttribute::CoverageMask: {
    Ret = ctx.pso_sample_mask != 0xffffffff ? ir.CreateAnd(res.coverage_mask_arg, ctx.pso_sample_mask)
                                            : res.coverage_mask_arg;
    break;
  }
  case InputAttribute::Domain: {
    Ret = res.domain;
    break;
  }
  case InputAttribute::PrimitiveId: {
    Ret = res.patch_id;
    break;
  }
  case InputAttribute::GSInstanceId: {
    Ret = res.gs_instance_id;
    break;
  }
  }

  return ApplySrcModifier(SrcOp._, Ret, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandIndexableTemp &SrcOp, mask_t Mask) {

  indexable_register_file regfile;

  if (SrcOp.phase != ~0u) {
    regfile = res.phases[SrcOp.phase].indexable_temp_map[SrcOp.regfile];
  } else {
    regfile = res.indexable_temp_map[SrcOp.regfile];
  }

  auto Handle = regfile.ptr_int_vec;
  auto TyHandle = GetArrayType(Handle);
  auto Index = LoadOperandIndex(SrcOp.regindex);
  auto TyInt = air.getIntTy();

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto Ptr = ir.CreateGEP(
        TyHandle, Handle,
        {ir.getInt32(0), ir.CreateAdd(ir.CreateMul(Index, ir.getInt32(regfile.vec_size)), ir.getInt32(Comp))}
    );
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec = air.getIntTy(regfile.vec_size);
  llvm::Value *ValueIntVec = llvm::PoisonValue::get(TyIntVec);
  for (auto [DstComp, _] : EnumerateComponents(MemoryAccessMask(Mask, SrcOp._.swizzle))) {
    auto Ptr = ir.CreateGEP(
        TyHandle, Handle,
        {ir.getInt32(0), ir.CreateAdd(ir.CreateMul(Index, ir.getInt32(regfile.vec_size)), ir.getInt32(DstComp))}
    );
    ValueIntVec = ir.CreateInsertElement(ValueIntVec, ir.CreateLoad(TyInt, Ptr), DstComp);
  }
  return ApplySrcModifier(SrcOp._, ValueIntVec, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandIndexableInput &SrcOp, mask_t Mask) {

  auto Handle = res.input.ptr_int4;
  auto TyHandle = GetArrayType(Handle);

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto TyInt = air.getIntTy();
    auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), LoadOperandIndex(SrcOp.regindex), ir.getInt32(Comp)});
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec4 = air.getIntTy(4);
  auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), LoadOperandIndex(SrcOp.regindex)});
  auto ValueIntVec4 = ir.CreateLoad(TyIntVec4, Ptr);
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandInputICP &SrcOp, mask_t Mask) {
  /* applies to both hull and domain shader, in this case input is a "2d" array */
  auto Handle = res.input.ptr_int4;
  auto TyHandle = GetArrayType(Handle);

  auto CPId = LoadOperandIndex(SrcOp.cpid);
  auto RegId = LoadOperandIndex(SrcOp.regid);

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto TyInt = air.getIntTy();
    auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), CPId, RegId, ir.getInt32(Comp)});
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec4 = air.getIntTy(4);
  auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), CPId, RegId});
  auto ValueIntVec4 = ir.CreateLoad(TyIntVec4, Ptr);
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandInputPC &SrcOp, mask_t Mask) {

  auto Handle = res.patch_constant_output.ptr_int4;
  auto TyHandle = GetArrayType(Handle);

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto TyInt = air.getIntTy();
    auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), LoadOperandIndex(SrcOp.regindex), ir.getInt32(Comp)});
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec4 = air.getIntTy(4);
  auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), LoadOperandIndex(SrcOp.regindex)});
  auto ValueIntVec4 = ir.CreateLoad(TyIntVec4, Ptr);
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::LoadOperand(const SrcOperandInputOCP &SrcOp, mask_t Mask) {
  /* applies to both hull and domain shader, in this case input is a "2d" array */
  auto Handle = res.output.ptr_int4;
  auto TyHandle = GetArrayType(Handle);

  auto CPId = LoadOperandIndex(SrcOp.cpid);
  auto RegId = LoadOperandIndex(SrcOp.regid);

  if (auto Comp = ComponentFromScalarMask(Mask, SrcOp._.swizzle); Comp >= 0) {
    auto TyInt = air.getIntTy();
    auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), CPId, RegId, ir.getInt32(Comp)});
    auto ValueInt = ir.CreateLoad(TyInt, Ptr);
    return ApplySrcModifier(SrcOp._, ValueInt, Mask);
  }

  auto TyIntVec4 = air.getIntTy(4);
  auto Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), CPId, RegId});
  auto ValueIntVec4 = ir.CreateLoad(TyIntVec4, Ptr);
  return ApplySrcModifier(SrcOp._, ValueIntVec4, Mask);
}

llvm::Value *
Converter::ApplySrcModifier(SrcOperandCommon C, llvm::Value *Value, mask_t Mask) {
  using namespace llvm::air;

  Value = MaskSwizzle(Value, Mask, C.swizzle);
  switch (C.read_type) {
  case OperandDataType::Float:
    Value = BitcastToFloat(Value);
    if (C.abs)
      Value = air.CreateFPUnOp(AIRBuilder::fabs, Value);
    if (C.neg)
      Value = ir.CreateFNeg(Value);
    break;
  case OperandDataType::Integer:
    Value = BitcastToInt32(Value);
    if (C.neg)
      Value = ir.CreateNeg(Value);
    break;
  case OperandDataType::Half16X16:
    Value = TruncAndBitcastToHalf(Value);
    if (C.neg)
      Value = ir.CreateFNeg(Value);
    break;
  }
  return Value;
}

llvm::Optional<TextureResourceHandle>
Converter::LoadTexture(const SrcOperandResource &SrcOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetSRVTexture(air, SrcOp.range_id, LoadOperandIndex(SrcOp.index));
  if (!descriptor)
    return {};

  return MakeTextureHandle(*descriptor, SrcOp.read_swizzle);
}

llvm::Optional<TextureResourceHandle>
Converter::LoadTexture(const SrcOperandUAV &SrcOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetUAVTexture(air, SrcOp.range_id, LoadOperandIndex(SrcOp.index));
  if (!descriptor)
    return {};

  return MakeTextureHandle(*descriptor, SrcOp.read_swizzle);
}

llvm::Optional<TextureResourceHandle>
Converter::LoadTexture(const AtomicDstOperandUAV &DstOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetUAVTexture(air, DstOp.range_id, LoadOperandIndex(DstOp.index));
  if (!descriptor)
    return {};

  return MakeTextureHandle(*descriptor, swizzle_identity);
}

llvm::Optional<BufferResourceHandle>
Converter::LoadBuffer(const SrcOperandResource &SrcOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetSRVBuffer(air, SrcOp.range_id, LoadOperandIndex(SrcOp.index));

  if (!descriptor)
    return {};

  return MakeBufferHandle(*descriptor, SrcOp.read_swizzle);
}

llvm::Optional<BufferResourceHandle>
Converter::LoadBuffer(const SrcOperandUAV &SrcOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetUAVBuffer(air, SrcOp.range_id, LoadOperandIndex(SrcOp.index));
  if (!descriptor)
    return {};

  return MakeBufferHandle(*descriptor, SrcOp.read_swizzle);
}

llvm::Optional<AtomicBufferResourceHandle>
Converter::LoadBuffer(const AtomicDstOperandUAV &DstOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetUAVBuffer(air, DstOp.range_id, LoadOperandIndex(DstOp.index));
  if (!descriptor)
    return {};

  return llvm::Optional<AtomicBufferResourceHandle>(
      {descriptor->Pointer, descriptor->Metadata, descriptor->StructureStride, DstOp.mask,
       descriptor->GlobalCoherent && SupportsMemoryCoherency()}
  );
}

llvm::Optional<BufferResourceHandle>
Converter::LoadBuffer(const SrcOperandTGSM &SrcOp) {
  auto [stride, tgsm_h] = res.tgsm_map[SrcOp.id];

  llvm::Value *IntPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(tgsm_h->getValueType(), tgsm_h, ir.getInt32(0));

  IntPtr = ir.CreatePointerCast(IntPtr, ir.getInt32Ty()->getPointerTo(tgsm_h->getAddressSpace()));

  return llvm::Optional<BufferResourceHandle>({IntPtr, nullptr, stride, SrcOp.read_swizzle, false});
}
llvm::Optional<AtomicBufferResourceHandle>
Converter::LoadBuffer(const AtomicOperandTGSM &DstOp) {
  auto [stride, tgsm_h] = res.tgsm_map[DstOp.id];

  llvm::Value *IntPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(tgsm_h->getValueType(), tgsm_h, ir.getInt32(0));

  IntPtr = ir.CreatePointerCast(IntPtr, ir.getInt32Ty()->getPointerTo(tgsm_h->getAddressSpace()));

  return llvm::Optional<AtomicBufferResourceHandle>({IntPtr, nullptr, stride, DstOp.mask, false});
}

llvm::Optional<UAVCounterHandle>
Converter::LoadCounter(const AtomicDstOperandUAV &SrcOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetUAVCounter(air, SrcOp.range_id, LoadOperandIndex(SrcOp.index));
  if (!descriptor)
    return {};

  return llvm::Optional<UAVCounterHandle>({descriptor->Pointer});
}

llvm::Optional<SamplerHandle>
Converter::LoadSampler(const SrcOperandSampler &SrcOp) {
  using namespace llvm::air;

  auto descriptor = ctx.binding.GetSampler(air, SrcOp.range_id, LoadOperandIndex(SrcOp.index));
  if (!descriptor)
    return {};

  return llvm::Optional<SamplerHandle>(
      {descriptor->SamplerHandle, descriptor->CubeSamplerHandle, DecodeSamplerBias(descriptor->Metadata),
       descriptor->Metadata, descriptor->Border}
  );
}

void
Converter::StoreOperand(const DstOperandOutput &DstOp, llvm::Value *Value) {
  if (ctx.shader_type == microsoft::D3D11_SB_HULL_SHADER && DstOp.phase == ~0u)
    return StoreOperandHull(DstOp, Value);

  auto ValueInt = ZExtAndBitcastToInt32(Value);

  auto Handle = res.output.ptr_int4;
  if (DstOp.phase != ~0u) {
    Handle = res.patch_constant_output.ptr_int4;
  }
  auto TyHandle = GetArrayType(Handle);

  if ((DstOp._.mask & kMaskAll) == kMaskAll) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), ir.getInt32(DstOp.regid)});
    ir.CreateStore(VectorSplat(4, ValueInt), Ptr);
    return;
  }
  for (auto [DstComp, SrcComp] : EnumerateComponents(DstOp._.mask)) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), ir.getInt32(DstOp.regid), ir.getInt32(DstComp)});
    ir.CreateStore(ExtractElement(ValueInt, SrcComp), Ptr);
  }
}

void
Converter::StoreOperandHull(const DstOperandOutput &DstOp, llvm::Value *Value) {
  auto ValueInt = ZExtAndBitcastToInt32(Value);

  auto Handle = res.output.ptr_int4;
  auto TyHandle = GetArrayType(Handle);

  auto CPId = res.thread_id_in_patch;
  auto RegId = ir.getInt32(DstOp.regid);

  if ((DstOp._.mask & kMaskAll) == kMaskAll) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), CPId, RegId});
    ir.CreateStore(VectorSplat(4, ValueInt), Ptr);
    return;
  }
  for (auto [DstComp, SrcComp] : EnumerateComponents(DstOp._.mask)) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), CPId, RegId, ir.getInt32(DstComp)});
    ir.CreateStore(ExtractElement(ValueInt, SrcComp), Ptr);
  }
}

void
Converter::StoreOperand(const DstOperandOutputCoverageMask &DstOp, llvm::Value *Value) {
  auto ValueInt = ZExtAndBitcastToInt32(Value);
  auto Ptr = ir.CreateConstGEP1_32(ir.getInt32Ty(), res.coverage_mask_reg, 0);
  ir.CreateStore(ExtractElement(ValueInt, 0), Ptr);
}

void
Converter::StoreOperand(const DstOperandOutputStencilRef &DstOp, llvm::Value *Value) {
  auto ValueInt = ZExtAndBitcastToInt32(Value);
  auto Ptr = ir.CreateConstGEP1_32(ir.getInt32Ty(), res.stencil_ref_reg, 0);
  ir.CreateStore(ExtractElement(ValueInt, 0), Ptr);
}

void
Converter::StoreOperand(const DstOperandOutputDepth &DstOp, llvm::Value *Value) {
  auto ValueFloat = ZExtAndBitcastToFloat(Value);
  auto Ptr = ir.CreateConstGEP1_32(ir.getFloatTy(), res.depth_output_reg, 0);
  ir.CreateStore(ExtractElement(ValueFloat, 0), Ptr);
}

void
Converter::StoreOperand(const DstOperandIndexableOutput &DstOp, llvm::Value *Value) {
  if (ctx.shader_type == microsoft::D3D11_SB_HULL_SHADER && DstOp.phase == ~0u)
    return StoreOperandHull(DstOp, Value);

  auto ValueInt = ZExtAndBitcastToInt32(Value);

  auto Handle = res.output.ptr_int4;
  if (DstOp.phase != ~0u) {
    Handle = res.patch_constant_output.ptr_int4;
  }
  auto TyHandle = GetArrayType(Handle);
  auto Index = LoadOperandIndex(DstOp.regindex);

  if ((DstOp._.mask & kMaskAll) == kMaskAll) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), Index});
    ir.CreateStore(VectorSplat(4, ValueInt), Ptr);
    return;
  }
  for (auto [DstComp, SrcComp] : EnumerateComponents(DstOp._.mask)) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), Index, ir.getInt32(DstComp)});
    ir.CreateStore(ExtractElement(ValueInt, SrcComp), Ptr);
  }
}

void
Converter::StoreOperandHull(const DstOperandIndexableOutput &DstOp, llvm::Value *Value) {
  auto ValueInt = ZExtAndBitcastToInt32(Value);

  auto Handle = res.output.ptr_int4;
  auto TyHandle = GetArrayType(Handle);
  auto Index = ir.CreateAdd(
      ir.CreateMul(res.thread_id_in_patch, ir.getInt32(res.output_element_count)), LoadOperandIndex(DstOp.regindex)
  );

  if ((DstOp._.mask & kMaskAll) == kMaskAll) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), Index});
    ir.CreateStore(VectorSplat(4, ValueInt), Ptr);
    return;
  }
  for (auto [DstComp, SrcComp] : EnumerateComponents(DstOp._.mask)) {
    auto Ptr = ir.CreateInBoundsGEP(TyHandle, Handle, {ir.getInt32(0), Index, ir.getInt32(DstComp)});
    ir.CreateStore(ExtractElement(ValueInt, SrcComp), Ptr);
  }
}

void
Converter::StoreOperand(const DstOperandTemp &DstOp, llvm::Value *Value) {
  auto ValueInt = ZExtAndBitcastToInt32(Value);

  auto Handle = res.temp.ptr_int4;

  if (DstOp.phase != ~0u) {
    Handle = res.phases[DstOp.phase].temp.ptr_int4;
  }
  auto TyHandle = GetArrayType(Handle);

  for (auto [DstComp, SrcComp] : EnumerateComponents(DstOp._.mask)) {
    auto Ptr = ir.CreateInBoundsGEP(
        TyHandle, Handle,
        {ir.getInt32(0), ir.CreateAdd(ir.CreateShl(ir.getInt32(DstOp.regid), 2), ir.getInt32(DstComp))}
    );
    ir.CreateStore(ExtractElement(ValueInt, SrcComp), Ptr);
  }
}

void
Converter::StoreOperand(const DstOperandIndexableTemp &DstOp, llvm::Value *Value) {
  auto ValueInt = ZExtAndBitcastToInt32(Value);

  indexable_register_file regfile;
  if (DstOp.phase != ~0u) {
    regfile = res.phases[DstOp.phase].indexable_temp_map[DstOp.regfile];
  } else {
    regfile = res.indexable_temp_map[DstOp.regfile];
  }

  auto Handle = regfile.ptr_int_vec;
  auto TyHandle = GetArrayType(Handle);
  auto Index = LoadOperandIndex(DstOp.regindex);

  for (auto [DstComp, SrcComp] : EnumerateComponents(DstOp._.mask)) {
    auto Ptr = ir.CreateInBoundsGEP(
        TyHandle, Handle,
        {ir.getInt32(0), ir.CreateAdd(ir.CreateMul(Index, ir.getInt32(regfile.vec_size)), ir.getInt32(DstComp))}
    );
    ir.CreateStore(ExtractElement(ValueInt, SrcComp), Ptr);
  }
}

llvm::Value *
Converter::BitcastToFloat(llvm::Value *Value) {
  auto Ty = Value->getType();
  if (Ty->getScalarType()->isFloatTy()) {
    return Value;
  }
  if (auto TyVec = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
    return ir.CreateBitCast(Value, air.getFloatTy(TyVec->getNumElements()));
  }
  return ir.CreateBitCast(Value, ir.getFloatTy());
}

llvm::Value *
Converter::BitcastToInt32(llvm::Value *Value) {
  auto Ty = Value->getType();
  if (Ty->getScalarType()->isIntegerTy(32)) {
    return Value;
  }
  if (auto TyVec = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
    return ir.CreateBitCast(Value, air.getIntTy(TyVec->getNumElements()));
  }
  return ir.CreateBitCast(Value, ir.getInt32Ty());
}

llvm::Value *
Converter::TruncAndBitcastToHalf(llvm::Value *Value) {
  auto Ty = Value->getType();
  if (Ty->getScalarType()->isHalfTy()) {
    return Value;
  }
  if (auto TyVec = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
    auto ElementCount = TyVec->getNumElements();
    auto TyShortVec = llvm::FixedVectorType::get(ir.getInt16Ty(), ElementCount);
    if (!Ty->getScalarType()->isIntegerTy(32)) {
      Value = ir.CreateBitCast(Value, air.getIntTy(ElementCount));
    }
    return ir.CreateBitCast(ir.CreateTrunc(Value, TyShortVec), air.getHalfTy(ElementCount));
  }
  if (!Ty->getScalarType()->isIntegerTy(32)) {
    Value = ir.CreateBitCast(Value, ir.getInt32Ty());
  }
  return ir.CreateBitCast(ir.CreateTrunc(Value, ir.getInt16Ty()), air.getHalfTy());
}

llvm::Value *
Converter::ZExtAndBitcastToInt32(llvm::Value *Value) {
  auto Ty = Value->getType();
  if (Ty->getScalarType()->isHalfTy()) {
    if (auto TyVec = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
      auto ElementCount = TyVec->getNumElements();
      auto TyShortVec = llvm::FixedVectorType::get(ir.getInt16Ty(), ElementCount);
      return ir.CreateZExt(ir.CreateBitCast(Value, TyShortVec), air.getIntTy(ElementCount));
    }
    return ir.CreateZExt(ir.CreateBitCast(Value, ir.getInt16Ty()), ir.getInt32Ty());
  }
  return BitcastToInt32(Value);
}

llvm::Value *
Converter::ZExtAndBitcastToFloat(llvm::Value *Value) {
  auto Ty = Value->getType();
  if (Ty->getScalarType()->isHalfTy()) {
    // doesn't make much sense but just in case
    Value = ZExtAndBitcastToInt32(Value);
  }
  return BitcastToFloat(Value);
}

llvm::Value *
Converter::MaskSwizzle(llvm::Value *Value, mask_t Mask, Swizzle Swizzle) {
  if (!isa<llvm::FixedVectorType>(Value->getType())) {
    switch (Mask) {
    case 0b1:
    case 0b10:
    case 0b100:
    case 0b1000:
      return Value;
    case 0b1100:
    case 0b0110:
    case 0b0011:
    case 0b1010:
    case 0b0101:
    case 0b1001:
      return ir.CreateVectorSplat(2, Value);
    case 0b1101:
    case 0b1011:
    case 0b1110:
    case 0b0111:
      return ir.CreateVectorSplat(3, Value);
    case 0b1111:
      return ir.CreateVectorSplat(4, Value);
    default:
      break;
    }
    return Value;
  }
  switch (Mask) {
  case 0b1:
    return ir.CreateExtractElement(Value, Swizzle[0]);
  case 0b10:
    return ir.CreateExtractElement(Value, Swizzle[1]);
  case 0b100:
    return ir.CreateExtractElement(Value, Swizzle[2]);
  case 0b1000:
    return ir.CreateExtractElement(Value, Swizzle[3]);
  case 0b1100:
    return ir.CreateShuffleVector(Value, {Swizzle[2], Swizzle[3]});
  case 0b0110:
    return ir.CreateShuffleVector(Value, {Swizzle[1], Swizzle[2]});
  case 0b0011:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[1]});
  case 0b1010:
    return ir.CreateShuffleVector(Value, {Swizzle[1], Swizzle[3]});
  case 0b0101:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[2]});
  case 0b1001:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[3]});
  case 0b1101:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[2], Swizzle[3]});
  case 0b1011:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[1], Swizzle[3]});
  case 0b1110:
    return ir.CreateShuffleVector(Value, {Swizzle[1], Swizzle[2], Swizzle[3]});
  case 0b0111:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[1], Swizzle[2]});
  case 0b1111:
    return ir.CreateShuffleVector(Value, {Swizzle[0], Swizzle[1], Swizzle[2], Swizzle[3]});
  default:
    break;
  }
  return Value;
}

void
Converter::operator()(const InstMov &mov) {
  auto FM = UseFastMath(mov._.precise_mask);
  mask_t Mask = GetMask(mov.dst);
  auto Value = LoadOperand(mov.src, Mask);
  StoreOperand(mov.dst, Value, mov._.saturate);
}

void
Converter::operator()(const InstMovConditional &movc) {
  auto FM = UseFastMath(movc._.precise_mask);
  mask_t Mask = GetMask(movc.dst);
  auto Src0 = LoadOperand(movc.src0, Mask);
  auto Src1 = LoadOperand(movc.src1, Mask);
  auto Cond = LoadOperand(movc.src_cond, Mask);
  auto CondZero = llvm::Constant::getNullValue(Cond->getType());
  auto Result = ir.CreateSelect(ir.CreateICmpNE(Cond, CondZero), Src0, Src1);
  StoreOperand(movc.dst, Result, movc._.saturate);
}
void
Converter::operator()(const InstSwapConditional &swapc) {
  mask_t Mask0 = GetMask(swapc.dst0);
  mask_t Mask1 = GetMask(swapc.dst1);
  mask_t MaskCombined = Mask0 | Mask1;

  auto Src0 = LoadOperand(swapc.src0, MaskCombined);
  auto Src1 = LoadOperand(swapc.src1, MaskCombined);
  auto Cond = LoadOperand(swapc.src_cond, MaskCombined);

  auto Result0 = ir.CreateSelect(
      ir.CreateIsNotNull(ExtractFromCombinedMask(Cond, MaskCombined, Mask0)),
      ExtractFromCombinedMask(Src1, MaskCombined, Mask0), ExtractFromCombinedMask(Src0, MaskCombined, Mask0)
  );
  auto Result1 = ir.CreateSelect(
      ir.CreateIsNotNull(ExtractFromCombinedMask(Cond, MaskCombined, Mask1)),
      ExtractFromCombinedMask(Src0, MaskCombined, Mask1), ExtractFromCombinedMask(Src1, MaskCombined, Mask1)
  );

  StoreOperand(swapc.dst0, Result0);
  StoreOperand(swapc.dst1, Result1);
}

void
Converter::operator()(const InstDotProduct &dot) {
  auto FM = UseFastMath(dot._.precise_mask);
  static mask_t DimensionMask[] = {kMaskVecXY, kMaskVecXYZ, kMaskAll};
  if (dot.dimension < 2 || dot.dimension > 4) {
    return;
  }
  mask_t Mask = DimensionMask[dot.dimension - 2];
  auto LHS = LoadOperand(dot.src0, Mask);
  auto RHS = LoadOperand(dot.src1, Mask);
  auto Result = air.CreateDotProduct(LHS, RHS);
  StoreOperand(dot.dst, Result, dot._.saturate);
}

void
Converter::operator()(const InstFloatUnaryOp &unary) {
  using namespace llvm::air;

  auto FM = UseFastMath(unary._.precise_mask);
  mask_t Mask = GetMask(unary.dst);
  auto Value = LoadOperand(unary.src, Mask);

  switch (unary.op) {
  case FloatUnaryOp::Log2:
    Value = air.CreateFPUnOp(AIRBuilder::log2, Value);
    break;
  case FloatUnaryOp::Exp2:
    Value = air.CreateFPUnOp(AIRBuilder::exp2, Value);
    break;
  case FloatUnaryOp::Rcp:
    Value = ir.CreateFDiv(llvm::ConstantFP::get(Value->getType(), 1.0), Value);
    break;
  case FloatUnaryOp::Rsq:
    Value = air.CreateFPUnOp(AIRBuilder::rsqrt, Value);
    break;
  case FloatUnaryOp::Sqrt:
    Value = air.CreateFPUnOp(AIRBuilder::sqrt, Value);
    break;
  case FloatUnaryOp::Fraction:
    Value = air.CreateFPUnOp(AIRBuilder::fract, Value);
    break;
  case FloatUnaryOp::RoundNearestEven:
    Value = air.CreateFPUnOp(AIRBuilder::rint, Value);
    break;
  case FloatUnaryOp::RoundNegativeInf:
    Value = air.CreateFPUnOp(AIRBuilder::floor, Value);
    break;
  case FloatUnaryOp::RoundPositiveInf:
    Value = air.CreateFPUnOp(AIRBuilder::ceil, Value);
    break;
  case FloatUnaryOp::RoundZero:
    Value = air.CreateFPUnOp(AIRBuilder::trunc, Value);
    break;
  }

  StoreOperand(unary.dst, Value, unary._.saturate);
}

void
Converter::operator()(const InstFloatBinaryOp &bin) {
  using namespace llvm::air;

  auto FM = UseFastMath(bin._.precise_mask);
  mask_t Mask = GetMask(bin.dst);
  auto LHS = LoadOperand(bin.src0, Mask);
  auto RHS = LoadOperand(bin.src1, Mask);

  llvm::Value *Result = llvm::PoisonValue::get(LHS->getType());

  switch (bin.op) {
  case FloatBinaryOp::Add:
    Result = ir.CreateFAdd(LHS, RHS);
    break;
  case FloatBinaryOp::Mul: {
    Result = ir.CreateFMul(LHS, RHS);
    break;
  }
  case FloatBinaryOp::Div:
    Result = ir.CreateFDiv(LHS, RHS);
    break;
  // the operand that is not NaN (D3D11.3 22.10.10), which Metal's fast variants leave undefined
  case FloatBinaryOp::Min: {
    Result = air.CreateFPBinOp(AIRBuilder::fmin, LHS, RHS, false);
    break;
  }
  case FloatBinaryOp::Max: {
    Result = air.CreateFPBinOp(AIRBuilder::fmax, LHS, RHS, false);
    break;
  }
  }

  StoreOperand(bin.dst, Result, bin._.saturate);
}

void
Converter::operator()(const InstIntegerUnaryOp &unary) {
  using namespace llvm::air;

  mask_t Mask = GetMask(unary.dst);
  auto Value = LoadOperand(unary.src, Mask);

  switch (unary.op) {
  case IntegerUnaryOp::Neg:
    Value = ir.CreateNeg(Value);
    break;
  case IntegerUnaryOp::Not:
    Value = ir.CreateNot(Value);
    break;
  case IntegerUnaryOp::ReverseBits:
    Value = air.CreateIntUnOp(AIRBuilder::reverse_bits, Value);
    break;
  case IntegerUnaryOp::CountBits:
    Value = air.CreateIntUnOp(AIRBuilder::popcount, Value);
    break;
  case IntegerUnaryOp::FirstHiBitSigned:
  case IntegerUnaryOp::FirstHiBit:
  case IntegerUnaryOp::FirstLowBit:
    Value = FirstBit(unary.op, Value);
    break;
  }

  StoreOperand(unary.dst, Value);
}

llvm::Value *
Converter::FirstBit(IntegerUnaryOp Op, llvm::Value *Value) {
  switch (Op) {
  case IntegerUnaryOp::FirstHiBitSigned:
    Value = ir.CreateSelect(
        ir.CreateIsNotNeg(Value), air.CreateCountZero(Value, false),
        ir.CreateAdd(
            air.CreateCountZero(ir.CreateShl(ir.CreateNot(Value), 1ull), false),
            llvm::ConstantInt::get(Value->getType(), 1)
        )
    );
    break;
  case IntegerUnaryOp::FirstHiBit:
    Value = air.CreateCountZero(Value, false);
    break;
  default:
    Value = air.CreateCountZero(Value, true);
    break;
  }
  // a count of the operand's whole width, or more, says no bit was found
  auto Bits = Value->getType()->getScalarSizeInBits();
  return MaxIfInMask(llvm::maskTrailingOnes<uint64_t>(Bits) - (Bits - 1), Value);
}

llvm::Value *
Converter::SinCos(llvm::air::AIRBuilder::FPUnOp Op, llvm::Value *Value) {
  // Metal's fast sine and cosine are both 0 from 2^23 radians on and of an infinity, where D3D's are in [-1, 1] and
  // NaN (D3D11.3 22.10.20), and a tangent made of them is 0 / 0. the precise ones are right for every value
  return air.CreateFPUnOp(Op, Value, false);
}

void
Converter::operator()(const InstIntegerBinaryOp &bin) {
  using namespace llvm::air;

  mask_t Mask = GetMask(bin.dst);
  auto LHS = LoadOperand(bin.src0, Mask);
  auto RHS = LoadOperand(bin.src1, Mask);

  llvm::Value *Result = llvm::PoisonValue::get(LHS->getType());

  switch (bin.op) {
  case IntegerBinaryOp::UMin:
    Result = air.CreateIntBinOp(AIRBuilder::min, LHS, RHS);
    break;
  case IntegerBinaryOp::UMax:
    Result = air.CreateIntBinOp(AIRBuilder::max, LHS, RHS);
    break;
  case IntegerBinaryOp::IMin:
    Result = air.CreateIntBinOp(AIRBuilder::min, LHS, RHS, true);
    break;
  case IntegerBinaryOp::IMax:
    Result = air.CreateIntBinOp(AIRBuilder::max, LHS, RHS, true);
    break;
  case IntegerBinaryOp::IShl:
    Result = ir.CreateShl(LHS, ir.CreateAnd(RHS, 0x1f));
    break;
  case IntegerBinaryOp::IShr:
    Result = ir.CreateAShr(LHS, ir.CreateAnd(RHS, 0x1f));
    break;
  case IntegerBinaryOp::UShr:
    Result = ir.CreateLShr(LHS, ir.CreateAnd(RHS, 0x1f));
    break;
  case IntegerBinaryOp::Xor:
    Result = ir.CreateXor(LHS, RHS);
    break;
  case IntegerBinaryOp::Or:
    Result = ir.CreateOr(LHS, RHS);
    break;
  case IntegerBinaryOp::And:
    Result = ir.CreateAnd(LHS, RHS);
    break;
  case IntegerBinaryOp::Add:
    Result = ir.CreateAdd(LHS, RHS);
    break;
  }

  StoreOperand(bin.dst, Result);
}

void
Converter::operator()(const InstPartialDerivative &deriv) {
  auto FM = UseFastMath(deriv._.precise_mask);
  mask_t Mask = GetMask(deriv.dst);
  auto SrcValue = LoadOperand(deriv.src, Mask);
  auto Result = air.CreateDerivative(SrcValue, deriv.ddy);
  StoreOperand(deriv.dst, Result, deriv._.saturate);
}

void
Converter::operator()(const InstConvert &convert) {
  using namespace llvm::air;

  mask_t Mask = GetMask(convert.dst);
  auto Value = LoadOperand(convert.src, Mask);

  switch (convert.op) {
  case ConversionOp::HalfToFloat:
    Value = air.CreateConvertToFloat(Value);
    break;
  case ConversionOp::FloatToHalf:
    Value = ConvertToHalfTowardZero(Value);
    break;
  case ConversionOp::FloatToSigned:
    Value = air.CreateConvertToSigned(Value);
    break;
  case ConversionOp::SignedToFloat:
    Value = air.CreateConvertToFloat(Value);
    break;
  case ConversionOp::FloatToUnsigned:
    Value = air.CreateConvertToUnsigned(Value);
    break;
  case ConversionOp::UnsignedToFloat:
    Value = air.CreateConvertToFloat(Value, Signedness::Unsigned);
    break;
  }

  StoreOperand(convert.dst, Value);
}

void
Converter::operator()(const InstFloatCompare &cmp) {
  auto FM = UseFastMath(cmp._.precise_mask);
  mask_t Mask = GetMask(cmp.dst);
  auto LHS = LoadOperand(cmp.src0, Mask);
  auto RHS = LoadOperand(cmp.src1, Mask);

  llvm::CmpInst::Predicate Pred = {};

  switch (cmp.cmp) {
  case FloatComparison::Equal:
    Pred = llvm::CmpInst::FCMP_OEQ;
    break;
  case FloatComparison::NotEqual:
    Pred = llvm::CmpInst::FCMP_UNE;
    break;
  case FloatComparison::GreaterEqual:
    Pred = llvm::CmpInst::FCMP_OGE;
    break;
  case FloatComparison::LessThan:
    Pred = llvm::CmpInst::FCMP_OLT;
    break;
  }

  llvm::Value * Result = ir.CreateFCmp(Pred, LHS, RHS);

  StoreOperand(cmp.dst, ir.CreateSExt(Result, Result->getType()->getWithNewBitWidth(32)));
}

void
Converter::operator()(const InstFloatMAD &mad) {
  auto FM = UseFastMath(mad._.precise_mask);
  mask_t Mask = GetMask(mad.dst);
  auto A = LoadOperand(mad.src0, Mask);
  auto B = LoadOperand(mad.src1, Mask);
  auto C = LoadOperand(mad.src2, Mask);

  StoreOperand(mad.dst, air.CreateFMA(A, B, C), mad._.saturate);
}

void
Converter::operator()(const InstSinCos &sincos) {
  using namespace llvm::air;

  auto FM = UseFastMath(sincos._.precise_mask);
  mask_t MaskCos = GetMask(sincos.dst_cos);
  mask_t MaskSin = GetMask(sincos.dst_sin);
  mask_t MaskCombined = MaskCos | MaskSin;

  auto Src = LoadOperand(sincos.src, MaskCombined);

  auto SrcCos = ExtractFromCombinedMask(Src, MaskCombined, MaskCos);
  auto SrcSin = ExtractFromCombinedMask(Src, MaskCombined, MaskSin);

  if (!IsNull(sincos.dst_cos))
    StoreOperand(sincos.dst_cos, SinCos(AIRBuilder::cos, SrcCos), sincos._.saturate);
  if (!IsNull(sincos.dst_sin))
    StoreOperand(sincos.dst_sin, SinCos(AIRBuilder::sin, SrcSin), sincos._.saturate);
}

void
Converter::operator()(const InstIntegerCompare &cmp) {
  mask_t Mask = GetMask(cmp.dst);
  auto LHS = LoadOperand(cmp.src0, Mask);
  auto RHS = LoadOperand(cmp.src1, Mask);

  llvm::CmpInst::Predicate Pred = llvm::CmpInst::ICMP_EQ;

  switch (cmp.cmp) {
  case IntegerComparison::Equal:
    Pred = llvm::CmpInst::ICMP_EQ;
    break;
  case IntegerComparison::NotEqual:
    Pred = llvm::CmpInst::ICMP_NE;
    break;
  case IntegerComparison::SignedLessThan:
    Pred = llvm::CmpInst::ICMP_SLT;
    break;
  case IntegerComparison::SignedGreaterEqual:
    Pred = llvm::CmpInst::ICMP_SGE;
    break;
  case IntegerComparison::UnsignedLessThan:
    Pred = llvm::CmpInst::ICMP_ULT;
    break;
  case IntegerComparison::UnsignedGreaterEqual:
    Pred = llvm::CmpInst::ICMP_UGE;
    break;
  }

  auto Result = ir.CreateICmp(Pred, LHS, RHS);

  StoreOperand(cmp.dst, ir.CreateSExt(Result, Result->getType()->getWithNewBitWidth(32)));
}

void
Converter::operator()(const InstIntegerMAD &mad) {
  mask_t Mask = GetMask(mad.dst);
  auto A = LoadOperand(mad.src0, Mask);
  auto B = LoadOperand(mad.src1, Mask);
  auto C = LoadOperand(mad.src2, Mask);

  auto Result = ir.CreateAdd(ir.CreateMul(A, B), C);

  StoreOperand(mad.dst, Result);
}

void
Converter::operator()(const InstIntegerBinaryOpWithTwoDst &bin) {
  using namespace llvm;
  using namespace llvm::air;

  mask_t MaskHi = GetMask(bin.dst_hi);
  mask_t MaskLo = GetMask(bin.dst_low);
  mask_t MaskCombined = MaskHi | MaskLo;

  auto Src0 = LoadOperand(bin.src0, MaskCombined);
  auto Src1 = LoadOperand(bin.src1, MaskCombined);

  auto Src0Hi = ExtractFromCombinedMask(Src0, MaskCombined, MaskHi);
  auto Src0Lo = ExtractFromCombinedMask(Src0, MaskCombined, MaskLo);
  auto Src1Hi = ExtractFromCombinedMask(Src1, MaskCombined, MaskHi);
  auto Src1Lo = ExtractFromCombinedMask(Src1, MaskCombined, MaskLo);

  switch (bin.op) {
  case IntegerBinaryOpWithTwoDst::IMul:
  case IntegerBinaryOpWithTwoDst::UMul: {
    bool Signed = bin.op == IntegerBinaryOpWithTwoDst::IMul;
    if (!IsNull(bin.dst_hi))
      StoreOperand(bin.dst_hi, air.CreateIntBinOp(AIRBuilder::mul_hi, Src0Hi, Src1Hi, Signed));
    if (!IsNull(bin.dst_low))
      StoreOperand(bin.dst_low, ir.CreateMul(Src0Lo, Src1Lo));
    break;
  }
  case IntegerBinaryOpWithTwoDst::UDiv:
    if (!IsNull(bin.dst_hi))
      StoreOperand(
          bin.dst_hi,
          ir.CreateSelect(
              ir.CreateIsNull(Src1Hi), ConstantInt::getAllOnesValue(Src1Hi->getType()), ir.CreateUDiv(Src0Hi, Src1Hi)
          )
      );
    if (!IsNull(bin.dst_low))
      StoreOperand(
          bin.dst_low,
          ir.CreateSelect(
              ir.CreateIsNull(Src1Lo), ConstantInt::getAllOnesValue(Src1Lo->getType()), ir.CreateURem(Src0Lo, Src1Lo)
          )
      );
    break;
  case IntegerBinaryOpWithTwoDst::UAddCarry: {
    if (!IsNull(bin.dst_hi))
      StoreOperand(bin.dst_hi, ir.CreateAdd(Src0Hi, Src1Hi));
    if (!IsNull(bin.dst_low)) {
      auto ResultTuple = ir.CreateBinaryIntrinsic(Intrinsic::uadd_with_overflow, Src0Lo, Src1Lo);
      StoreOperand(
          bin.dst_low, ir.CreateZExt(
                           ir.CreateExtractValue(ResultTuple, 1),
                           ResultTuple->getType()->getStructElementType(1)->getWithNewBitWidth(32)
                       )
      );
    }
    break;
  }
  case IntegerBinaryOpWithTwoDst::USubBorrow:
    if (!IsNull(bin.dst_hi))
      StoreOperand(bin.dst_hi, ir.CreateSub(Src0Hi, Src1Hi));
    if (!IsNull(bin.dst_low)) {
      auto ResultTuple = ir.CreateBinaryIntrinsic(Intrinsic::usub_with_overflow, Src0Lo, Src1Lo);
      StoreOperand(
          bin.dst_low, ir.CreateZExt(
                           ir.CreateExtractValue(ResultTuple, 1),
                           ResultTuple->getType()->getStructElementType(1)->getWithNewBitWidth(32)
                       )
      );
    }
    break;
  }
}

void
Converter::operator()(const InstExtractBits &extract) {
  mask_t Mask = GetMask(extract.dst);
  StoreOperand(
      extract.dst, ExtractBits(
                       LoadOperand(extract.src0, Mask), LoadOperand(extract.src1, Mask),
                       LoadOperand(extract.src2, Mask), extract.is_signed
                   )
  );
}

llvm::Value *
Converter::ExtractBits(llvm::Value *Src0, llvm::Value *Src1, llvm::Value *Src2, bool Signed) {
  auto Width = ir.CreateAnd(Src0, 0x1F);
  auto Offset = ir.CreateAnd(Src1, 0X1F);
  auto WidthAddOffset = ir.CreateAdd(Width, Offset);
  auto Constant_32 = llvm::ConstantInt::get(Width->getType(), 32);
  auto ClampedSrc2 = ir.CreateShl(Src2, ir.CreateSub(Constant_32, WidthAddOffset));

  llvm::Value *NeedClamp, *NoClamp, *Clamped;

  if (Signed) {
    NeedClamp = ir.CreateICmpSLT(WidthAddOffset, Constant_32);
    NoClamp = ir.CreateAShr(Src2, Offset);
    Clamped = ir.CreateAShr(ClampedSrc2, ir.CreateSub(Constant_32, Width));
  } else {
    NeedClamp = ir.CreateICmpULT(WidthAddOffset, Constant_32);
    NoClamp = ir.CreateLShr(Src2, Offset);
    Clamped = ir.CreateLShr(ClampedSrc2, ir.CreateSub(Constant_32, Width));
  }
  return ir.CreateSelect(
      ir.CreateIsNull(Width), llvm::ConstantInt::get(Width->getType(), 0), ir.CreateSelect(NeedClamp, Clamped, NoClamp)
  );
}
void
Converter::operator()(const InstBitFiledInsert &bfi) {
  mask_t Mask = GetMask(bfi.dst);
  StoreOperand(
      bfi.dst, InsertBits(
                   LoadOperand(bfi.src0, Mask), LoadOperand(bfi.src1, Mask), LoadOperand(bfi.src2, Mask),
                   LoadOperand(bfi.src3, Mask)
               )
  );
}

llvm::Value *
Converter::InsertBits(llvm::Value *Src0, llvm::Value *Src1, llvm::Value *Src2, llvm::Value *Src3) {
  auto Width = ir.CreateAnd(Src0, 0x1F);
  auto Offset = ir.CreateAnd(Src1, 0X1F);
  auto Constant_1 = llvm::ConstantInt::get(Width->getType(), 1);
  auto Bitmask = ir.CreateShl(ir.CreateSub(ir.CreateShl(Constant_1, Width), Constant_1), Offset);

  return ir.CreateOr(ir.CreateAnd(ir.CreateShl(Src2, Offset), Bitmask), ir.CreateAnd(Src3, ir.CreateNot(Bitmask)));
}

void
Converter::StoreFeedback(const std::optional<DstOperand> &Feedback, llvm::Value *Residency) {
  // Metal's residency sets bit 0 where a texel read was not mapped; D3D's feedback is nonzero where all were, as it is
  // for reads that report no residency
  if (Feedback)
    StoreOperand(
        *Feedback, Residency ? ir.CreateZExt(ir.CreateICmpEQ(ir.CreateAnd(Residency, 1), llvm::ConstantInt::get(Residency->getType(), 0)), ir.getInt32Ty())
                             : ir.getInt32(~0u)
    );
}

void
Converter::operator()(const InstLoad &load) {
  auto Tex = LoadTexture(load.src_resource);
  if (!Tex)
    return;
  auto Coord = LoadOperand(load.src_address, kMaskAll);
  auto SampleIndex = load.src_sample_index ? LoadOperand(load.src_sample_index.value(), kMaskComponentX) : nullptr;
  auto LOD = ir.CreateExtractElement(Coord, 3);
  auto [Address, ArrayIndex] = TexelAddress(*Tex, Coord, load.offsets, SampleIndex ? nullptr : LOD);

  auto [Value, Residency] =
      air.CreateRead(Tex->Texture, Tex->Handle, Address, ArrayIndex, SampleIndex, LOD, Tex->GlobalCoherent);

  StoreOperand(load.dst, MaskSwizzle(Value, GetMask(load.dst), Tex->Swizzle));
  StoreFeedback(load.feedback, Residency);
}
void
Converter::operator()(const InstLoadUAVTyped &load) {
  using namespace llvm::air;

  auto Tex = LoadTexture(load.src_uav);
  if (!Tex)
    return;
  const int32_t NoOffset[3] = {};
  auto [Address, ArrayIndex] = TexelAddress(*Tex, LoadOperand(load.src_address, kMaskAll), NoOffset, nullptr);

  if (Tex->Texture.memory_access == Texture::acesss_readwrite)
    air.CreateTextureFence(Tex->Texture, Tex->Handle);

  auto [Value, Residency] =
      air.CreateRead(Tex->Texture, Tex->Handle, Address, ArrayIndex, nullptr, air.getInt(0), Tex->GlobalCoherent);

  StoreOperand(load.dst, MaskSwizzle(Value, GetMask(load.dst), Tex->Swizzle));
  StoreFeedback(load.feedback, Residency);
}

void
Converter::operator()(const InstStoreUAVTyped &store) {
  auto Tex = LoadTexture(store.dst);
  if (!Tex.hasValue())
    return;
  const int32_t NoOffset[3] = {};
  auto [Address, ArrayIndex] = TexelAddress(*Tex, LoadOperand(store.src_address, kMaskAll), NoOffset, nullptr);

  auto Value = LoadOperand(store.src, kMaskAll);

  air.CreateWrite(Tex->Texture, Tex->Handle, Address, ArrayIndex, nullptr, ir.getInt32(0), Value, Tex->GlobalCoherent);
}

std::pair<llvm::Value *, llvm::Value *>
Converter::TexelAddress(const TextureResourceHandle &Tex, llvm::Value *Coord, const int32_t Offset[3], llvm::Value *Level) {
  auto &Info = llvm::air::AIRBuilder::getTextureInfo(Tex.Logical);
  unsigned Dimension = Info.coord_dimension;
  auto Ty = air.getTextureRWPositionType(Tex.Texture); // 1d textures are 2d in Metal
  llvm::Value *Address =
      Dimension == 1 && !Ty->isVectorTy() ? ir.CreateExtractElement(Coord, 0ull) : llvm::Constant::getNullValue(Ty);
  llvm::SmallVector<int32_t, 3> Offsets(
      Offset, Offset + (Ty->isVectorTy() ? cast<llvm::FixedVectorType>(Ty)->getNumElements() : 1)
  );
  for (unsigned i = Dimension; i < Offsets.size(); i++)
    Offsets[i] = 0;
  if (Ty->isVectorTy())
    for (unsigned i = 0; i < Dimension; i++)
      Address = ir.CreateInsertElement(Address, ir.CreateExtractElement(Coord, i), i);
  auto OffsetValue = Ty->isVectorTy() ? llvm::ConstantDataVector::get(
                                            air.getContext(),
                                            llvm::ArrayRef<uint32_t>((const uint32_t *)Offsets.data(), Offsets.size())
                                        )
                                      : (llvm::Constant *)air.getInt(Offsets[0]);
  llvm::Value *OOB = nullptr;
  if (Tex.Logical == llvm::air::Texture::texture_buffer) {
    OOB = ir.CreateICmpUGE(ir.CreateAdd(Address, OffsetValue), DecodeTextureBufferElement(Tex.Metadata));
    Address = ir.CreateAdd(Address, DecodeTextureBufferOffset(Tex.Metadata));
  } else if (Level && Info.is_mipmaped) {
    // the view keeps the mip that holds its min-LOD clamp and every smaller one
    auto MinLevel = air.CreateFPUnOp(llvm::air::AIRBuilder::floor, DecodeTextureMinLODClamp(Tex.Metadata));
    OOB = ir.CreateICmpULT(Level, air.CreateConvertToUnsigned(MinLevel));
  }
  if (!OffsetValue->isNullValue())
    Address = ir.CreateAdd(Address, OffsetValue);
  if (auto Size = UAVWSize(Tex)) {
    // z counts from the view's first W slice and ends at its size
    auto Z = ir.CreateExtractElement(Address, 2);
    auto Ranged = ir.CreateICmpNE(Size, air.getInt(0));
    auto Outside = ir.CreateAnd(Ranged, ir.CreateICmpUGE(Z, Size));
    OOB = OOB ? ir.CreateOr(OOB, Outside) : Outside;
    auto First = ir.CreateSelect(Ranged, DecodeTextureArrayLength(Tex.Metadata), air.getInt(0));
    Address = ir.CreateInsertElement(Address, ir.CreateAdd(Z, First), 2);
  }
  if (OOB)
    Address = ir.CreateSelect(OOB, llvm::ConstantInt::getAllOnesValue(Address->getType()), Address);
  return {Address, Info.is_array ? ir.CreateExtractElement(Coord, Dimension) : nullptr};
}

llvm::Value *
Converter::TextureDimensions(const TextureResourceHandle &Tex, llvm::Value *Level) {
  using namespace llvm::air;
  auto &Info = AIRBuilder::getTextureInfo(Tex.Logical);
  auto Query = [&](Texture::Query Query, llvm::Value *Level) {
    return air.CreateTextureQuery(Tex.Texture, Tex.Handle, Query, Level);
  };
  // a cube reports the size of a face
  unsigned Dimension = Info.is_cube ? 2 : Info.coord_dimension;
  const Texture::Query Sizes[] = {Texture::width, Texture::height, Texture::depth};
  llvm::Value *Value = llvm::ConstantAggregateZero::get(air.getIntTy(4));
  for (unsigned i = 0; i < Dimension; i++)
    Value = ir.CreateInsertElement(Value, Query(Sizes[i], Level), i);
  if (auto Size = UAVWSize(Tex))
    Value = ir.CreateInsertElement(
        Value, ir.CreateSelect(ir.CreateICmpNE(Size, air.getInt(0)), Size, ir.CreateExtractElement(Value, 2)), 2
    );
  if (Info.is_array)
    Value = ir.CreateInsertElement(Value, Query(Texture::array_length, ir.getInt32(0)), Dimension);
  return ir.CreateInsertElement(
      Value, Info.is_mipmaped ? Query(Texture::num_mip_levels, ir.getInt32(0)) : ir.getInt32(1), 3
  );
}

llvm::Value *
Converter::ConvertToHalfTowardZero(llvm::Value *Value) {
  // Metal rounds to nearest even; step a result that rounded away from zero back by one ulp. that also turns the
  // overflow to infinity into the largest finite half
  auto Half = air.CreateConvertToHalf(Value);
  auto Bits = ir.CreateBitCast(Half, Value->getType()->getWithNewType(ir.getInt16Ty()));
  auto Away = ir.CreateFCmpOGT(
      air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, air.CreateConvertToFloat(Half)),
      air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, Value)
  );
  auto Stepped = ir.CreateSub(Bits, llvm::ConstantInt::get(Bits->getType(), 1));
  return ir.CreateBitCast(ir.CreateSelect(Away, Stepped, Bits), Half->getType());
}

llvm::Value *
Converter::ClampArrayIndex(llvm::Value *ShaderValue, llvm::Value *Metadata) {
  using namespace llvm::air;
  auto rounded = air.CreateFPUnOp(AIRBuilder::rint, ShaderValue);
  auto integer = air.CreateConvertToSigned(rounded);
  auto positive_integer = air.CreateIntBinOp(AIRBuilder::max, integer, ir.getInt32(0), true);
  auto max_index = ir.CreateSub(DecodeTextureArrayLength(Metadata), ir.getInt32(1));
  return air.CreateIntBinOp(AIRBuilder::min, positive_integer, max_index, true);
}

void
Converter::operator()(const InstSample &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  auto MinLODClamp = DecodeTextureMinLODClamp(Tex->Metadata);
  if (sample.min_lod_clamp) {
    auto ShaderClamp = LoadOperand(sample.min_lod_clamp.value(), kMaskComponentX);
    MinLODClamp = air.CreateFPBinOp(AIRBuilder::fmax, MinLODClamp, ShaderClamp);
  }

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;

  switch (Tex->Logical) {
  case Texture::texture1d:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    break;
  case Texture::texture1d_array:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentY);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::depth2d:
  case Texture::texture2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    break;
  case Texture::depth2d_array:
  case Texture::texture2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::texture3d:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    break;
  default:
    return;
  }

  auto Read = [&](llvm::Value *S) {
    return air.CreateSample(
        Tex->Texture, Tex->Handle, S, Coord, ArrayIndex, sample.offsets, sample_bias{Sampler->Bias},
        sample_min_lod_clamp{MinLODClamp}
    );
  };
  auto [Value, Residency] = Read(SamplerHandle);
  Value = CustomBorder(*Tex, SamplerHandle, Sampler->Border, Sampler->Metadata, Value, [&](auto S) { return Read(S).first; });

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstSampleLOD &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;

  switch (Tex->Logical) {
  case Texture::texture1d:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    break;
  case Texture::texture1d_array:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentY);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::depth2d:
  case Texture::texture2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    break;
  case Texture::depth2d_array:
  case Texture::texture2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::texture3d:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    break;
  default:
    return;
  }

  llvm::Value *LOD =
      ClampMinLOD(Tex->Metadata, ir.CreateFAdd(LoadOperand(sample.src_lod, kMaskComponentX), Sampler->Bias));

  auto Read = [&](llvm::Value *S) {
    return air.CreateSample(Tex->Texture, Tex->Handle, S, Coord, ArrayIndex, sample.offsets, sample_level{LOD});
  };
  auto [Value, Residency] = Read(SamplerHandle);
  Value = CustomBorder(*Tex, SamplerHandle, Sampler->Border, Sampler->Metadata, Value, [&](auto S) { return Read(S).first; });

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstSampleBias &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  auto MinLODClamp = DecodeTextureMinLODClamp(Tex->Metadata);
  if (sample.min_lod_clamp) {
    auto ShaderClamp = LoadOperand(sample.min_lod_clamp.value(), kMaskComponentX);
    MinLODClamp = air.CreateFPBinOp(AIRBuilder::fmax, MinLODClamp, ShaderClamp);
  }

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;

  switch (Tex->Logical) {
  case Texture::texture1d:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    break;
  case Texture::texture1d_array:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentY);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::depth2d:
  case Texture::texture2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    break;
  case Texture::depth2d_array:
  case Texture::texture2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::texture3d:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    break;
  default:
    return;
  }

  auto Bias = ir.CreateFAdd(Sampler->Bias, LoadOperand(sample.src_bias, kMaskComponentX));

  auto Read = [&](llvm::Value *S) {
    return air.CreateSample(
        Tex->Texture, Tex->Handle, S, Coord, ArrayIndex, sample.offsets, sample_bias{Bias},
        sample_min_lod_clamp{MinLODClamp}
    );
  };
  auto [Value, Residency] = Read(SamplerHandle);
  Value = CustomBorder(*Tex, SamplerHandle, Sampler->Border, Sampler->Metadata, Value, [&](auto S) { return Read(S).first; });

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstSampleDerivative &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  auto MinLODClamp = DecodeTextureMinLODClamp(Tex->Metadata);
  if (sample.min_lod_clamp) {
    auto ShaderClamp = LoadOperand(sample.min_lod_clamp.value(), kMaskComponentX);
    MinLODClamp = air.CreateFPBinOp(AIRBuilder::fmax, MinLODClamp, ShaderClamp);
  }

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;
  llvm::Value *DDX = nullptr;
  llvm::Value *DDY = nullptr;

  switch (Tex->Logical) {
  case Texture::texture1d:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    DDX = LoadOperand(sample.src_x_derivative, kMaskComponentX);
    DDX = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), DDX, 0ull);
    DDY = LoadOperand(sample.src_y_derivative, kMaskComponentX);
    DDY = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), DDY, 0ull);
    break;
  case Texture::texture1d_array:
    Coord = LoadOperand(sample.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    DDX = LoadOperand(sample.src_x_derivative, kMaskComponentX);
    DDX = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), DDX, 0ull);
    DDY = LoadOperand(sample.src_y_derivative, kMaskComponentX);
    DDY = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), DDY, 0ull);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentY);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::depth2d:
  case Texture::texture2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    DDX = LoadOperand(sample.src_x_derivative, kMaskVecXY);
    DDY = LoadOperand(sample.src_y_derivative, kMaskVecXY);
    break;
  case Texture::depth2d_array:
  case Texture::texture2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    DDX = LoadOperand(sample.src_x_derivative, kMaskVecXY);
    DDY = LoadOperand(sample.src_y_derivative, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::texture3d:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    DDX = LoadOperand(sample.src_x_derivative, kMaskVecXYZ);
    DDY = LoadOperand(sample.src_y_derivative, kMaskVecXYZ);
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    DDX = LoadOperand(sample.src_x_derivative, kMaskVecXYZ);
    DDY = LoadOperand(sample.src_y_derivative, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    DDX = LoadOperand(sample.src_x_derivative, kMaskVecXYZ);
    DDY = LoadOperand(sample.src_y_derivative, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    break;
  default:
    return;
  }

  auto Read = [&](llvm::Value *S) {
    return air.CreateSampleGrad(
        Tex->Texture, Tex->Handle, S, Coord, ArrayIndex, BiasGradient(DDX, Sampler->Bias),
        BiasGradient(DDY, Sampler->Bias), MinLODClamp, sample.offsets
    );
  };
  auto [Value, Residency] = Read(SamplerHandle);
  Value = CustomBorder(*Tex, SamplerHandle, Sampler->Border, Sampler->Metadata, Value, [&](auto S) { return Read(S).first; });

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstSampleCompare &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  auto MinLODClamp = DecodeTextureMinLODClamp(Tex->Metadata);
  if (sample.min_lod_clamp) {
    auto ShaderClamp = LoadOperand(sample.min_lod_clamp.value(), kMaskComponentX);
    MinLODClamp = air.CreateFPBinOp(AIRBuilder::fmax, MinLODClamp, ShaderClamp);
  }

  auto Reference = LoadOperand(sample.src_reference, kMaskComponentX);

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;

  switch (Tex->Logical) {
  case Texture::depth2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    break;
  case Texture::depth2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    break;
  case Texture::texture3d:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    break;
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    break;
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    break;
  default:
    return;
  }

  auto [Value, Residency] = //
      sample.level_zero     //
          ? air.CreateSampleCmp(
                Tex->Texture, Tex->Handle, SamplerHandle, Coord, ArrayIndex, Reference, sample.offsets,
                sample_level{ClampMinLOD(Tex->Metadata, Sampler->Bias)}
            )
          : air.CreateSampleCmp(
                Tex->Texture, Tex->Handle, SamplerHandle, Coord, ArrayIndex, Reference, sample.offsets,
                sample_bias{Sampler->Bias}, sample_min_lod_clamp{MinLODClamp}
            );
  Value = CustomBorder(
      *Tex, SamplerHandle, Sampler->Border, Sampler->Metadata, Value,
      [&](llvm::Value *S) {
        return (sample.level_zero ? air.CreateSample(
                                        Tex->Texture, Tex->Handle, S, Coord, ArrayIndex, sample.offsets,
                                        sample_level{ClampMinLOD(Tex->Metadata, Sampler->Bias)}
                                    )
                                  : air.CreateSample(
                                        Tex->Texture, Tex->Handle, S, Coord, ArrayIndex, sample.offsets,
                                        sample_bias{Sampler->Bias}, sample_min_lod_clamp{MinLODClamp}
                                    ))
            .first;
      },
      Reference
  );

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstGather &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  auto Component = ir.getInt32(sample.src_sampler.gather_channel);

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;
  llvm::Value *Offset = nullptr;

  switch (Tex->Logical) {
  case Texture::depth2d:
  case Texture::texture2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    Offset = LoadOperand(sample.offset, kMaskVecXY);
    break;
  case Texture::depth2d_array:
  case Texture::texture2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    Offset = LoadOperand(sample.offset, kMaskVecXY);
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    Offset = LoadOperand(sample.offset, kMaskVecXYZ);
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    Offset = LoadOperand(sample.offset, kMaskVecXYZ);
    break;
  default:
    return;
  }

  auto [Value, Residency] =
      Gather(*Tex, SamplerHandle, Sampler->Metadata, Coord, ArrayIndex, Offset, Component, nullptr, Sampler->Border);

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstGatherCompare &sample) {
  using namespace llvm::air;

  auto Tex = LoadTexture(sample.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(sample.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  auto Reference = LoadOperand(sample.src_reference, kMaskComponentX);

  llvm::Value *Coord = nullptr;
  llvm::Value *ArrayIndex = nullptr;
  llvm::Value *Offset = nullptr;

  switch (Tex->Logical) {
  case Texture::depth2d:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    Offset = LoadOperand(sample.offset, kMaskVecXY);
    break;
  case Texture::depth2d_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXY);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentZ);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    Offset = LoadOperand(sample.offset, kMaskVecXY);
    break;
  case Texture::depthcube:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    Offset = LoadOperand(sample.offset, kMaskVecXYZ);
    break;
  case Texture::depthcube_array:
    Coord = LoadOperand(sample.src_address, kMaskVecXYZ);
    ArrayIndex = LoadOperand(sample.src_address, kMaskComponentW);
    ArrayIndex = ClampArrayIndex(ArrayIndex, Tex->Metadata);
    SamplerHandle = Sampler->HandleCube;
    Offset = LoadOperand(sample.offset, kMaskVecXYZ);
    break;
  default:
    return;
  }

  auto [Value, Residency] =
      Gather(*Tex, SamplerHandle, Sampler->Metadata, Coord, ArrayIndex, Offset, nullptr, Reference, Sampler->Border);

  StoreOperand(sample.dst, MaskSwizzle(Value, GetMask(sample.dst), Tex->Swizzle));
  StoreFeedback(sample.feedback, Residency);
}

void
Converter::operator()(const InstCalcLOD &lod) {
  using namespace llvm::air;

  auto Tex = LoadTexture(lod.src_resource);
  if (!Tex)
    return;

  auto Sampler = LoadSampler(lod.src_sampler);
  if (!Sampler)
    return;

  auto SamplerHandle = Sampler->Handle;

  llvm::Value *Coord = nullptr;

  switch (Tex->Logical) {
  case Texture::texture1d:
  case Texture::texture1d_array:
    Coord = LoadOperand(lod.src_address, kMaskComponentX);
    Coord = ir.CreateInsertElement(llvm::ConstantFP::getNullValue(air.getFloatTy(2)), Coord, 0ull);
    break;
  case Texture::depth2d:
  case Texture::texture2d:
  case Texture::depth2d_array:
  case Texture::texture2d_array:
    Coord = LoadOperand(lod.src_address, kMaskVecXY);
    break;
  case Texture::texture3d:
    Coord = LoadOperand(lod.src_address, kMaskVecXYZ);
    break;
  case Texture::texturecube:
  case Texture::depthcube:
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = LoadOperand(lod.src_address, kMaskVecXYZ);
    SamplerHandle = Sampler->HandleCube;
    break;
  default:
    return;
  }

  auto [Clamped, Unclamped] = CalculateLOD(*Tex, SamplerHandle, Sampler->Metadata, Coord);

  StoreOperand(
      lod.dst,
      MaskSwizzle(
          ir.CreateInsertElement(
              ir.CreateInsertElement(llvm::ConstantAggregateZero::get(air.getFloatTy(4)), Clamped, (uint64_t)0),
              Unclamped, 1
          ),
          GetMask(lod.dst), Tex->Swizzle
      )
  );
}

std::pair<llvm::Value *, llvm::Value *>
Converter::CalculateLOD(
    const TextureResourceHandle &Tex, llvm::Value *Sampler, llvm::Value *SamplerMetadata, llvm::Value *Coord,
    llvm::Value *Level
) {
  using namespace llvm::air;
  if (!Level)
    Level = air.CreateCalculateLOD(Tex.Texture, Tex.Handle, Sampler, Coord).second;
  auto Unclamped = ir.CreateFAdd(Level, DecodeSamplerBias(SamplerMetadata));
  // SM50_SAMPLER_METADATA: MaxLOD is stored as its distance from the largest field value
  auto FieldMax = ir.getInt32((1 << SM50_SAMPLER_METADATA_LOD_BITS) - 1);
  auto Field = [&](unsigned Shift) {
    return ir.CreateAnd(ir.CreateTrunc(ir.CreateLShr(SamplerMetadata, Shift), air.getIntTy()), FieldMax);
  };
  auto LOD = [&](llvm::Value *Field) {
    return ir.CreateFMul(
        air.CreateConvertToFloat(Field),
        llvm::ConstantFP::get(air.getFloatTy(), std::ldexp(1.0, -SM50_SAMPLER_METADATA_LOD_FRACTION_BITS))
    );
  };
  auto Mips = air.CreateTextureQuery(Tex.Texture, Tex.Handle, Texture::num_mip_levels, ir.getInt32(0));
  auto Last = air.CreateConvertToFloat(ir.CreateSub(Mips, ir.getInt32(1)), Signedness::Unsigned);
  // the sampler's clamp first, MinLOD over MaxLOD where they cross (D3D11.3 7.18.11), within the mips there are; then
  // the view's min-LOD clamp (5.8.6.1); a view clamp past the view's last mip leaves no mips, and 0 (5.8.5)
  auto Clamped = ClampMinLOD(
      Tex.Metadata,
      air.CreateFPBinOp(
          AIRBuilder::fmin,
          air.CreateFPBinOp(
              AIRBuilder::fmax, LOD(Field(SM50_SAMPLER_METADATA_MIN_LOD)),
              air.CreateFPBinOp(
                  AIRBuilder::fmin, Unclamped, LOD(ir.CreateSub(FieldMax, Field(SM50_SAMPLER_METADATA_MAX_LOD)))
              )
          ),
          Last
      )
  );
  auto Empty = ir.CreateFCmpOGT(DecodeTextureMinLODClamp(Tex.Metadata), Last);
  return {ir.CreateSelect(Empty, llvm::ConstantFP::get(air.getFloatTy(), 0.0), Clamped), Unclamped};
}

std::pair<llvm::Value *, llvm::Value *>
Converter::Gather(
    const TextureResourceHandle &Tex, llvm::Value *Sampler, llvm::Value *SamplerMetadata, llvm::Value *Coord,
    llvm::Value *ArrayIndex, llvm::Value *Offset, llvm::Value *Component, llvm::Value *Reference, llvm::Value *Border
) {
  using namespace llvm;
  // immediate texel offsets span [-8, 7] (D3D11_COMMONSHADER_TEXEL_OFFSET_MAX_NEGATIVE/POSITIVE), as Metal's native
  // offset does; gather4_po honors the low 6 bits of each offset, signed (D3D11.3 22.4.4)
  constexpr int64_t ImmediateOffsetMin = -8, ImmediateOffsetMax = 7;
  constexpr unsigned ProgrammableOffsetBits = 6;
  auto OffsetTy = cast<FixedVectorType>(Offset->getType());
  auto Native = [](Constant *C) {
    auto I = dyn_cast_or_null<ConstantInt>(C);
    return I && I->getSExtValue() >= ImmediateOffsetMin && I->getSExtValue() <= ImmediateOffsetMax;
  };
  bool InRange = isa<Constant>(Offset);
  for (unsigned i = 0; InRange && i < OffsetTy->getNumElements(); i++)
    InRange = Native(cast<Constant>(Offset)->getAggregateElement(i));
  if (!InRange && OffsetTy->getNumElements() == 2) {
    auto Shift = ConstantInt::get(OffsetTy, OffsetTy->getScalarSizeInBits() - ProgrammableOffsetBits);
    auto Texels = air.CreateConvertToFloat(ir.CreateAShr(ir.CreateShl(Offset, Shift), Shift));
    auto Size = air.CreateConvertToFloat(
        ir.CreateShuffleVector(TextureDimensions(Tex, ir.getInt32(0)), {0, 1}), llvm::air::Signedness::Unsigned
    );
    Coord = ir.CreateFAdd(Coord, ir.CreateFDiv(Texels, Size));
    Offset = Constant::getNullValue(OffsetTy);
  }
  auto [Value, Residency] =
      Reference ? air.CreateGatherCompare(Tex.Texture, Tex.Handle, Sampler, Coord, ArrayIndex, Reference, Offset)
                : air.CreateGather(Tex.Texture, Tex.Handle, Sampler, Coord, ArrayIndex, Offset, Component);
  // a comparison gathers the red of depth
  Value = CustomBorder(
      Tex, Sampler, Border, SamplerMetadata, Value,
      [&](llvm::Value *S) {
        return air.CreateGather(Tex.Texture, Tex.Handle, S, Coord, ArrayIndex, Offset, Component ? Component : ir.getInt32(0))
            .first;
      },
      Reference, Component
  );
  // the reference against the out-of-bounds 0
  llvm::Value *OutOfBounds = Constant::getNullValue(Value->getType());
  if (Reference)
    OutOfBounds = ir.CreateVectorSplat(
        cast<FixedVectorType>(Value->getType())->getNumElements(),
        SamplerCompare(SamplerMetadata, Reference, ConstantFP::get(air.getFloatTy(), 0))
    );
  // the view's first mip, the only one a gather reads, is gone when the min-LOD clamp reaches the next mip, or passes
  // the view's last one (D3D11.3 5.8.5): 1, or anything above 0 for a single-mip view
  auto Clamp = DecodeTextureMinLODClamp(Tex.Metadata);
  auto LastMip = air.CreateConvertToFloat(
      ir.CreateSub(air.CreateTextureQuery(Tex.Texture, Tex.Handle, llvm::air::Texture::num_mip_levels, ir.getInt32(0)), ir.getInt32(1)),
      llvm::air::Signedness::Unsigned
  );
  auto Removed = ir.CreateOr(
      ir.CreateFCmpOGE(Clamp, ConstantFP::get(air.getFloatTy(), 1)), ir.CreateFCmpOGT(Clamp, LastMip)
  );
  return {ir.CreateSelect(Removed, OutOfBounds, Value), Residency};
}

llvm::Value *
Converter::SamplerCompare(llvm::Value *SamplerMetadata, llvm::Value *Reference, llvm::Value *Texel) {
  // WMTCompareFunction's bits are less, equal, greater, and a function passes an outcome when it has all of its bits.
  // an unordered (NaN) outcome counts as less and greater, so only NotEqual and Always pass it, as IEEE comparison does
  auto Outcome = ir.CreateSelect(
      ir.CreateFCmpOLT(Reference, Texel), ir.getInt32(1),
      ir.CreateSelect(
          ir.CreateFCmpOEQ(Reference, Texel), ir.getInt32(2),
          ir.CreateSelect(ir.CreateFCmpOGT(Reference, Texel), ir.getInt32(4), ir.getInt32(5))
      )
  );
  auto Function = ir.CreateTrunc(ir.CreateLShr(SamplerMetadata, SM50_SAMPLER_METADATA_COMPARE), air.getIntTy());
  return air.CreateConvertToFloat(
      ir.CreateZExt(ir.CreateICmpEQ(ir.CreateAnd(Function, Outcome), Outcome), air.getIntTy()),
      llvm::air::Signedness::Unsigned
  );
}

llvm::Value *
Converter::CustomBorder(
    const TextureResourceHandle &Tex, llvm::Value *Sampler, llvm::Value *Border, llvm::Value *SamplerMetadata,
    llvm::Value *Value, const std::function<llvm::Value *(llvm::Value *)> &Read, llvm::Value *Reference,
    llvm::Value *Component
) {
  using namespace llvm;
  using namespace llvm::air;
  // D3D's cubes ignore address modes
  if (!Border || !Tex.Range || AIRBuilder::getTextureInfo(Tex.Logical).is_cube)
    return Value;
  // only samplers with a border pay for it
  auto Head = ir.GetInsertBlock();
  auto Tail = ir.GetInsertPoint() == Head->end() ? llvm::BasicBlock::Create(ctx.llvm, "", Head->getParent())
                                                 : SplitBlock(Head, &*ir.GetInsertPoint());
  if (auto Branch = Head->getTerminator())
    Branch->eraseFromParent();
  auto Custom = llvm::BasicBlock::Create(ctx.llvm, "custom_border", Head->getParent(), Tail);
  ir.SetInsertPoint(Head);
  ir.CreateCondBr(ir.CreateICmpNE(Border, ir.getInt64(0)), Custom, Tail);

  // the color, clamped to the texture format's range (D3D11.3 7.18.9.1)
  ir.SetInsertPoint(Custom);
  auto TyBorder = StructType::get(ctx.llvm, {air.getSamplerHandleType(), ir.getInt64Ty(), air.getFloatTy(4)});
  auto Ptr = ir.CreateIntToPtr(Border, TyBorder->getPointerTo(2));
  auto White = ir.CreateLoad(TyBorder->getElementType(0), ir.CreateStructGEP(TyBorder, Ptr, 0));
  auto Range = ir.CreateLoad(air.getFloatTy(2), ir.CreatePointerCast(Tex.Range, air.getFloatTy(2)->getPointerTo(2)));
  auto Clamp = [&](llvm::Value *V) {
    auto Bound = [&](uint64_t i) {
      auto B = ir.CreateExtractElement(Range, i);
      auto Ty = dyn_cast<FixedVectorType>(V->getType());
      return Ty ? ir.CreateVectorSplat(Ty->getNumElements(), B) : B;
    };
    return air.CreateFPBinOp(AIRBuilder::fmin, air.CreateFPBinOp(AIRBuilder::fmax, V, Bound(0)), Bound(1));
  };
  auto Color = Clamp(ir.CreateLoad(TyBorder->getElementType(2), ir.CreateStructGEP(TyBorder, Ptr, 2)));

  // per channel, the white and black reads differ by the border's weight
  auto Black = Reference ? Read(Sampler) : Value;
  auto Integer = Tex.Texture.sample_type != Texture::sample_float && !Reference;
  auto Weight = Integer ? ir.CreateSub(Read(White), Black) : ir.CreateFSub(Read(White), Black);
  auto Splat = [&](llvm::Value *S) {
    auto Ty = dyn_cast<FixedVectorType>(Weight->getType());
    return Ty ? ir.CreateVectorSplat(Ty->getNumElements(), S) : S;
  };
  llvm::Value *Fixed;
  if (Reference) {
    // the border's weight moves from the comparison with Metal's 0 to the one with the color's red
    auto Clamped = Clamp(Reference);
    auto Change = ir.CreateFSub(
        SamplerCompare(SamplerMetadata, Clamped, ir.CreateExtractElement(Color, uint64_t(0))),
        SamplerCompare(SamplerMetadata, Clamped, ConstantFP::get(air.getFloatTy(), 0))
    );
    Fixed = ir.CreateFAdd(Value, ir.CreateFMul(Weight, Splat(Change)));
  } else {
    // a gather reads one channel of each texel, a depth texture its red
    llvm::Value *Channel = Color;
    if (Component || !Weight->getType()->isVectorTy())
      Channel = Splat(ir.CreateExtractElement(Color, Component ? Component : ir.getInt32(0)));
    Fixed = Integer ? ir.CreateAdd(
                          Value, ir.CreateMul(
                                     Weight, Tex.Texture.sample_type == Texture::sample_int
                                                 ? air.CreateConvertToSigned(Channel)
                                                 : air.CreateConvertToUnsigned(Channel)
                                 )
                      )
                    : ir.CreateFAdd(Value, ir.CreateFMul(Weight, Channel));
  }
  auto From = ir.GetInsertBlock();
  ir.CreateBr(Tail);

  ir.SetInsertPoint(Tail, Tail->begin());
  auto Result = ir.CreatePHI(Value->getType(), 2);
  Result->addIncoming(Value, Head);
  Result->addIncoming(Fixed, From);
  return Result;
}

void
Converter::operator()(const InstResourceInfo &resinfo) {
  auto Tex = LoadTexture(resinfo.src_resource);
  if (!Tex || Tex->Logical == llvm::air::Texture::texture_buffer)
    return;

  auto Value = TextureDimensions(*Tex, LoadOperand(resinfo.src_mip_level, kMaskComponentX));
  if (resinfo.modifier != InstResourceInfo::M::uint)
    Value = air.CreateConvertToFloat(Value);
  // rcp inverts the sizes, not the mip count
  if (resinfo.modifier == InstResourceInfo::M::rcp)
    Value = ir.CreateInsertElement(
        ir.CreateFDiv(llvm::ConstantFP::get(air.getFloatTy(4), 1.0), Value), ir.CreateExtractElement(Value, 3), 3
    );

  StoreOperand(resinfo.dst, MaskSwizzle(Value, GetMask(resinfo.dst), Tex->Swizzle));
}

void
Converter::operator()(const InstSampleInfo &sample) {
  using namespace llvm::air;

  llvm::Optional<TextureResourceHandle> Tex =
      sample.src ? LoadTexture(sample.src.value()) : llvm::Optional<TextureResourceHandle>{};

  llvm::Value *SampleCount = nullptr;

  if (!Tex) {
    if (sample.src.has_value()) {
      SampleCount = ir.getInt32(0);
    } else {
      SampleCount = air.CreateGetNumSamples();
    }
  } else {
    SampleCount = air.CreateTextureQuery(Tex->Texture, Tex->Handle, Texture::num_samples, ir.getInt32(0));
  }

  if (!sample.uint_result) {
    SampleCount = air.CreateConvertToFloat(SampleCount);
  }

  llvm::Value *ValueVec4 = llvm::ConstantAggregateZero::get(sample.uint_result ? air.getIntTy(4) : air.getFloatTy(4));
  ValueVec4 = ir.CreateInsertElement(ValueVec4, SampleCount, (uint64_t)0);

  StoreOperand(sample.dst, MaskSwizzle(ValueVec4, GetMask(sample.dst), sample.read_swizzle));
}

void
Converter::operator()(const InstSamplePos &sample) {
  using namespace llvm::air;

  llvm::Optional<TextureResourceHandle> Tex =
      sample.src ? LoadTexture(sample.src.value()) : llvm::Optional<TextureResourceHandle>{};

  llvm::Value *SampleCount = nullptr;

  if (!Tex) {
    if (sample.src.has_value()) {
      SampleCount = ir.getInt32(0);
    } else {
      SampleCount = air.CreateGetNumSamples();
    }
  } else {
    SampleCount = air.CreateTextureQuery(Tex->Texture, Tex->Handle, Texture::num_samples, ir.getInt32(0));
  }

  llvm::Value *Index = LoadOperand(sample.src_sample_index, kMaskComponentX);

  llvm::Value *ValueVec4 = llvm::ConstantAggregateZero::get(air.getFloatTy(2));
  llvm::Value *Pos = air.CreateSamplePos(SampleCount, Index);
  ValueVec4 = ir.CreateShuffleVector(Pos, ValueVec4, {0, 1, 2, 3});

  StoreOperand(sample.dst, MaskSwizzle(ValueVec4, GetMask(sample.dst), sample.read_swizzle));
}

void
Converter::operator()(const InstBufferInfo &bufinfo) {
  auto Buf = LoadBuffer(bufinfo.src);

  llvm::Value *Value = ir.getInt32(0);

  if (Buf && Buf->Metadata) {
    Value = DecodeRawBufferByteLength(Buf->Metadata);
    if (Buf->StructureStride) {
      Value = ir.CreateUDiv(Value, ir.getInt32(Buf->StructureStride));
    }
  } else if (auto Tex = LoadTexture(bufinfo.src); Tex) {
    Value = DecodeTextureBufferElement(Tex->Metadata);
  }

  StoreOperand(bufinfo.dst, Value);
}

void
Converter::operator()(const InstLoadRaw &load) {
  using namespace llvm::air;

  auto Buf = LoadBuffer(load.src);
  if (!Buf)
    return;

  mask_t Mask = GetMask(load.dst);
  bool Volatile = cast<llvm::PointerType>(Buf->Pointer->getType())->getAddressSpace() == 3;

  auto Index = ir.CreateLShr(LoadOperand(load.src_byte_offset, kMaskComponentX), 2);

  StoreFeedback(load.feedback, nullptr);

  if (auto Comp = ComponentFromScalarMask(Mask, Buf->Swizzle); Comp >= 0) {
    auto Ptr = CreateGEPInt32WithBoundCheck(Buf.value(), ir.CreateAdd(Index, llvm::ConstantInt::get(Index->getType(), Comp)));
    auto ValueInt = Buf->GlobalCoherent ? (llvm::Value *)air.CreateDeviceCoherentLoad(ir.getInt32Ty(), Ptr)
                                        : (llvm::Value *)ir.CreateLoad(ir.getInt32Ty(), Ptr, Volatile);
    return StoreOperand(load.dst, MaskSwizzle(ValueInt, Mask));
  }

  llvm::Value *ValueVec = llvm::PoisonValue::get(air.getIntTy(4));
  for (auto [DstComp, _] : EnumerateComponents(MemoryAccessMask(Mask, Buf->Swizzle))) {
    auto Ptr = CreateGEPInt32WithBoundCheck(Buf.value(), ir.CreateAdd(Index, llvm::ConstantInt::get(Index->getType(), DstComp)));
    auto ValueInt = Buf->GlobalCoherent ? (llvm::Value *)air.CreateDeviceCoherentLoad(ir.getInt32Ty(), Ptr)
                                        : (llvm::Value *)ir.CreateLoad(ir.getInt32Ty(), Ptr, Volatile);
    ValueVec = ir.CreateInsertElement(ValueVec, ValueInt, DstComp);
  }
  StoreOperand(load.dst, MaskSwizzle(ValueVec, Mask, Buf->Swizzle));
}

void
Converter::operator()(const InstLoadStructured &load) {
  using namespace llvm::air;

  auto Buf = LoadBuffer(load.src);
  if (!Buf)
    return;

  mask_t Mask = GetMask(load.dst);
  bool Volatile = cast<llvm::PointerType>(Buf->Pointer->getType())->getAddressSpace() == 3;

  // in 64 bits: an element index scaled by the stride can wrap 32 and pass the bounds check
  auto IndexStruct = ir.CreateMul(
      ir.getInt64(Buf->StructureStride >> 2), ir.CreateZExt(LoadOperand(load.src_address, kMaskComponentX), ir.getInt64Ty())
  );
  auto Index = ir.CreateAdd(
      IndexStruct, ir.CreateZExt(ir.CreateLShr(LoadOperand(load.src_byte_offset, kMaskComponentX), 2), ir.getInt64Ty())
  );

  StoreFeedback(load.feedback, nullptr);

  if (auto Comp = ComponentFromScalarMask(Mask, Buf->Swizzle); Comp >= 0) {
    auto Ptr = CreateGEPInt32WithBoundCheck(Buf.value(), ir.CreateAdd(Index, llvm::ConstantInt::get(Index->getType(), Comp)));
    auto ValueInt = Buf->GlobalCoherent ? (llvm::Value *)air.CreateDeviceCoherentLoad(ir.getInt32Ty(), Ptr)
                                        : (llvm::Value *)ir.CreateLoad(ir.getInt32Ty(), Ptr, Volatile);
    return StoreOperand(load.dst, MaskSwizzle(ValueInt, Mask));
  }

  llvm::Value *ValueVec = llvm::PoisonValue::get(air.getIntTy(4));
  for (auto [DstComp, _] : EnumerateComponents(MemoryAccessMask(Mask, Buf->Swizzle))) {
    auto Ptr = CreateGEPInt32WithBoundCheck(Buf.value(), ir.CreateAdd(Index, llvm::ConstantInt::get(Index->getType(), DstComp)));
    auto ValueInt = Buf->GlobalCoherent ? (llvm::Value *)air.CreateDeviceCoherentLoad(ir.getInt32Ty(), Ptr)
                                        : (llvm::Value *)ir.CreateLoad(ir.getInt32Ty(), Ptr, Volatile);
    ValueVec = ir.CreateInsertElement(ValueVec, ValueInt, DstComp);
  }
  StoreOperand(load.dst, MaskSwizzle(ValueVec, Mask, Buf->Swizzle));
}

void
Converter::operator()(const InstStoreRaw &store) {
  using namespace llvm::air;

  auto Buf = LoadBuffer(store.dst);
  if (!Buf)
    return;
  switch (Buf->Mask) {
  case 0b1:
  case 0b11:
  case 0b111:
  case 0b1111:
    break;
  default:
    return;
  }

  bool Volatile = cast<llvm::PointerType>(Buf->Pointer->getType())->getAddressSpace() == 3;
  if (air.DiscardWrites && !Volatile)
    return;

  auto Index = ir.CreateLShr(LoadOperand(store.dst_byte_offset, kMaskComponentX), 2);

  auto Value = LoadOperand(store.src, Buf->Mask);

  for (auto [DstComp, _] : EnumerateComponents(Buf->Mask)) {
    auto Ptr = CreateGEPInt32WithBoundCheck(Buf.value(), ir.CreateAdd(Index, llvm::ConstantInt::get(Index->getType(), DstComp)));
    if (Buf->GlobalCoherent)
      air.CreateDeviceCoherentStore(ExtractElement(Value, DstComp), Ptr);
    else
      ir.CreateStore(ExtractElement(Value, DstComp), Ptr, Volatile);
  }
}

void
Converter::operator()(const InstStoreStructured &store) {
  using namespace llvm::air;

  auto Buf = LoadBuffer(store.dst);
  if (!Buf)
    return;
  switch (Buf->Mask) {
  case 0b1:
  case 0b11:
  case 0b111:
  case 0b1111:
    break;
  default:
    return;
  }

  bool Volatile = cast<llvm::PointerType>(Buf->Pointer->getType())->getAddressSpace() == 3;
  if (air.DiscardWrites && !Volatile)
    return;

  // in 64 bits: an element index scaled by the stride can wrap 32 and pass the bounds check
  auto IndexStruct = ir.CreateMul(
      ir.getInt64(Buf->StructureStride >> 2), ir.CreateZExt(LoadOperand(store.dst_address, kMaskComponentX), ir.getInt64Ty())
  );
  auto Index = ir.CreateAdd(
      IndexStruct, ir.CreateZExt(ir.CreateLShr(LoadOperand(store.dst_byte_offset, kMaskComponentX), 2), ir.getInt64Ty())
  );

  auto Value = LoadOperand(store.src, Buf->Mask);

  for (auto [DstComp, _] : EnumerateComponents(Buf->Mask)) {
    auto Ptr = CreateGEPInt32WithBoundCheck(Buf.value(), ir.CreateAdd(Index, llvm::ConstantInt::get(Index->getType(), DstComp)));
    if (Buf->GlobalCoherent)
      air.CreateDeviceCoherentStore(ExtractElement(Value, DstComp), Ptr);
    else
      ir.CreateStore(ExtractElement(Value, DstComp), Ptr, Volatile);
  }
}

llvm::Value *
Converter::LoadAtomicOpAddress(const AtomicBufferResourceHandle &Handle, const SrcOperand &Address) {
  if (Handle.StructureStride > 0) {
    auto Address2D = LoadOperand(Address, kMaskVecXY);
    // in 64 bits, as for structured loads and stores
    return ir.CreateLShr(
        ir.CreateAdd(
            ir.CreateMul(ir.getInt64(Handle.StructureStride), ir.CreateZExt(ExtractElement(Address2D, 0), ir.getInt64Ty())),
            ir.CreateZExt(ExtractElement(Address2D, 1), ir.getInt64Ty())
        ),
        2
    );
  }
  auto Address1D = LoadOperand(Address, kMaskComponentX);
  return ir.CreateLShr(Address1D, 2);
}

void
Converter::operator()(const InstAtomicBinOp &atomic) {
  using namespace llvm;
  using namespace llvm::air;

  AtomicRMWInst::BinOp Op;

  switch (atomic.op) {
  case AtomicBinaryOp::And:
    Op = AtomicRMWInst::And;
    break;
  case AtomicBinaryOp::Or:
    Op = AtomicRMWInst::Or;
    break;
  case AtomicBinaryOp::Xor:
    Op = AtomicRMWInst::Xor;
    break;
  case AtomicBinaryOp::Add:
    Op = AtomicRMWInst::Add;
    break;
  case AtomicBinaryOp::IMax:
    Op = AtomicRMWInst::Max;
    break;
  case AtomicBinaryOp::IMin:
    Op = AtomicRMWInst::Min;
    break;
  case AtomicBinaryOp::UMax:
    Op = AtomicRMWInst::UMax;
    break;
  case AtomicBinaryOp::UMin:
    Op = AtomicRMWInst::UMin;
    break;
  case AtomicBinaryOp::Xchg:
    Op = AtomicRMWInst::Xchg;
    break;
  }

  auto Buf = LoadBuffer(atomic.dst);

  if (Buf) {
    auto IntPtrOffset = LoadAtomicOpAddress(Buf.getValue(), atomic.dst_address);
    auto Ptr = ir.CreateGEP(ir.getInt32Ty(), Buf->Pointer, {IntPtrOffset});
    auto Value = air.CreateAtomicRMW(Op, Ptr, LoadOperand(atomic.src, kMaskComponentX));
    StoreOperand(atomic.dst_original, Value);
    return;
  }

  auto Tex = LoadTexture(atomic.dst);

  if (Tex) {
    const int32_t NoOffset[3] = {};
    auto [Address, ArrayIndex] = TexelAddress(*Tex, LoadOperand(atomic.dst_address, kMaskAll), NoOffset, nullptr);
    auto Value =
        air.CreateAtomicRMW(Tex->Texture, Tex->Handle, Op, Address, LoadOperand(atomic.src, kMaskAll), ArrayIndex);
    StoreOperand(atomic.dst_original, Value);
    return;
  }
}

void
Converter::operator()(const InstAtomicImmCmpExchange &atomic) {
  using namespace llvm;
  using namespace llvm::air;

  auto Buf = LoadBuffer(atomic.dst_resource);

  if (Buf) {
    auto IntPtrOffset = LoadAtomicOpAddress(Buf.getValue(), atomic.dst_address);
    auto Ptr = ir.CreateGEP(ir.getInt32Ty(), Buf->Pointer, {IntPtrOffset});
    if (air.DiscardWrites && Ptr->getType()->getPointerAddressSpace() != 3) {
      StoreOperand(atomic.dst, ir.CreateLoad(ir.getInt32Ty(), Ptr));
      return;
    }
    auto Value = ir.CreateAtomicCmpXchg(
        Ptr, LoadOperand(atomic.src0, kMaskComponentX), LoadOperand(atomic.src1, kMaskComponentX), {},
        AtomicOrdering::Monotonic, AtomicOrdering::Monotonic
    );
    StoreOperand(atomic.dst, ir.CreateExtractValue(Value, 0));
    return;
  }

  auto Tex = LoadTexture(atomic.dst_resource);

  if (Tex) {
    const int32_t NoOffset[3] = {};
    auto [Address, ArrayIndex] = TexelAddress(*Tex, LoadOperand(atomic.dst_address, kMaskAll), NoOffset, nullptr);
    auto [Value, Flag_DISCARDED] = air.CreateAtomicCmpXchg(
        Tex->Texture, Tex->Handle, Address, LoadOperand(atomic.src0, kMaskAll), LoadOperand(atomic.src1, kMaskAll),
        ArrayIndex
    );
    StoreOperand(atomic.dst, Value);
    return;
  }
}

void
Converter::operator()(const InstAtomicImmIncrement &atomic) {
  using namespace llvm;
  using namespace llvm::air;

  auto Ctr = LoadCounter(atomic.uav);
  if (!Ctr)
    return;

  auto Value = air.CreateAtomicRMW(AtomicRMWInst::Add, Ctr->Pointer, ir.getInt32(1));
  StoreOperand(atomic.dst, Value);
}
void
Converter::operator()(const InstAtomicImmDecrement &atomic) {
  using namespace llvm;
  using namespace llvm::air;

  auto Ctr = LoadCounter(atomic.uav);
  if (!Ctr)
    return;

  auto Value = air.CreateAtomicRMW(AtomicRMWInst::Sub, Ctr->Pointer, ir.getInt32(1));
  // imm_atomic_consume returns new value
  StoreOperand(atomic.dst, ir.CreateSub(Value, ir.getInt32(1)));
}

llvm::Optional<InterpolantHandle>
Converter::LoadInterpolant(uint32_t Index) {
  if (!res.interpolant_map.contains(Index))
    return {};

  auto &interpolant = res.interpolant_map[Index];
  InterpolantHandle handle{{}, interpolant.perspective};
  for (unsigned i = 0; i < 4; i++)
    if (~interpolant.component[i])
      handle.Component[i] = ctx.function->getArg(interpolant.component[i]);
  return handle;
}

void
Converter::operator()(const InstInterpolateCentroid &eval) {
  auto Itp = LoadInterpolant(eval.regid);
  if (!Itp)
    return;

  auto Value = Interpolate(*Itp, [&](auto Handle) { return air.CreateInterpolateAtCentroid(Handle, Itp->Perspective); });
  StoreOperand(eval.dst, MaskSwizzle(Value, GetMask(eval.dst), eval.read_swizzle));
}

void
Converter::operator()(const InstInterpolateSample &eval) {
  auto Itp = LoadInterpolant(eval.regid);
  if (!Itp)
    return;

  auto Sample = LoadOperand(eval.sample_index, kMaskComponentX);
  auto Value = Interpolate(*Itp, [&](auto Handle) { return air.CreateInterpolateAtSample(Handle, Sample, Itp->Perspective); });
  StoreOperand(eval.dst, MaskSwizzle(Value, GetMask(eval.dst), eval.read_swizzle));
}

void
Converter::operator()(const InstInterpolateOffset &eval) {
  auto Itp = LoadInterpolant(eval.regid);
  if (!Itp)
    return;

  auto Value = InterpolateAtOffset(*Itp, LoadOperand(eval.offset, kMaskVecXY));
  StoreOperand(eval.dst, MaskSwizzle(Value, GetMask(eval.dst), eval.read_swizzle));
}

llvm::Value *
Converter::InterpolateAtOffset(const InterpolantHandle &Itp, llvm::Value *Offset) {
  // truncated = (offset.xy + 8) & 0b1111
  auto Truncated = ir.CreateAnd(ir.CreateAdd(Offset, air.getInt2(8, 8)), air.getInt2(0b1111, 0b1111));
  auto OffsetFloat = ir.CreateFMul(air.CreateConvertToFloat(Truncated), air.getFloat2(1.0f / 16.0f, 1.0f / 16.0f));
  return Interpolate(Itp, [&](auto Handle) { return air.CreateInterpolateAtOffset(Handle, OffsetFloat, Itp.Perspective); });
}

void
Converter::operator()(const InstMaskedSumOfAbsDiff &msad) {
  using namespace llvm;

  mask_t Mask = GetMask(msad.dst);
  StoreOperand(
      msad.dst,
      MaskedSumOfAbsDiff(LoadOperand(msad.src0, Mask), LoadOperand(msad.src1, Mask), LoadOperand(msad.src2, Mask))
  );
}

llvm::Value *
Converter::MaskedSumOfAbsDiff(llvm::Value *Ref, llvm::Value *Src, llvm::Value *Accum) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(Ref->getType());
  Ops.push_back(Ref);
  Tys.push_back(Src->getType());
  Ops.push_back(Src);
  Tys.push_back(Accum->getType());
  Ops.push_back(Accum);

  std::string FnName = "dxmt.msad";
  FnName += air.getTypeOverloadSuffix(Ref->getType());

  auto Fn = air.getModule()->getOrInsertFunction(FnName, FunctionType::get(Accum->getType(), Tys, false), Attrs);
  return ir.CreateCall(Fn, Ops);
}

void
Converter::operator()(const InstEmit &Inst) {
  if (res.call_emit(Inst.stream).build(ctx).takeError()) {
    // TODO
  }
}
void
Converter::operator()(const InstCut &Inst) {
  if (res.call_cut(Inst.stream).build(ctx).takeError()) {
    // TODO
  }
}

void
Converter::HullGenerateWorkload(
    const char *Domain, llvm::Value *PatchIndex, llvm::Value *CountPtr, llvm::Value *DataPtr, uint32_t Capacity,
    TessellatorPartitioning Partitioning, llvm::ArrayRef<llvm::Value *> TessFactors
) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{3U, Attribute::get(Context, Attribute::AttrKind::WriteOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops{PatchIndex, CountPtr, DataPtr, ir.getInt32(Capacity), ir.getInt32((uint32_t)Partitioning)};
  Ops.append(TessFactors.begin(), TessFactors.end());
  SmallVector<Type *> Tys;
  for (auto Op : Ops)
    Tys.push_back(Op->getType());

  auto Fn = air.getModule()->getOrInsertFunction(
      std::string("dxmt.generate_workload.") + Domain, llvm::FunctionType::get(air.getVoidTy(), Tys, false), Attrs
  );
  ir.CreateCall(Fn, Ops);
}

llvm::Value *
Converter::DomainGetPatchIndex(llvm::Value *WorkloadIndex, llvm::Value *DataPtr) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(WorkloadIndex->getType());
  Ops.push_back(WorkloadIndex);
  Tys.push_back(DataPtr->getType());
  Ops.push_back(DataPtr);

  std::string FnName = "dxmt.get_domain_patch_index";
  auto Fn = air.getModule()->getOrInsertFunction(FnName, llvm::FunctionType::get(air.getIntTy(), Tys, false), Attrs);
  return ir.CreateCall(Fn, Ops);
}

std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
Converter::DomainGetLocation(llvm::Value *WorkloadIndex, llvm::Value *ThreadIndex, llvm::Value *DataPtr) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{3U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {3U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(WorkloadIndex->getType());
  Ops.push_back(WorkloadIndex);
  Tys.push_back(ThreadIndex->getType());
  Ops.push_back(ThreadIndex);
  Tys.push_back(DataPtr->getType());
  Ops.push_back(DataPtr);

  auto Fn = air.getModule()->getOrInsertFunction(
      "dxmt.get_domain_location",
      llvm::FunctionType::get(
          llvm::StructType::create(Context, {air.getFloatTy(2), air.getByteTy(), air.getByteTy()}, ""), Tys, false
      ),
      Attrs
  );
  auto Return = ir.CreateCall(Fn, Ops);
  return {
      ir.CreateExtractValue(Return, 0ull), ir.CreateIsNotNull(ir.CreateExtractValue(Return, 1)),
      ir.CreateIsNotNull(ir.CreateExtractValue(Return, 2))
  };
}

// the domain location of a corner of the workload's primitive, and whether the workload has that primitive
std::pair<llvm::Value *, llvm::Value *>
Converter::DomainGetPrimitiveLocation(
    llvm::Value *WorkloadIndex, llvm::Value *PrimitiveIndex, llvm::Value *Corner, TessellatorOutputPrimitive Primitive,
    llvm::Value *DataPtr
) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{6U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {6U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );

  uint32_t Vertices = Primitive == TessellatorOutputPrimitive::point ? 1
                      : Primitive == TessellatorOutputPrimitive::line ? 2
                                                                      : 3;
  SmallVector<Value *> Ops{
      WorkloadIndex, PrimitiveIndex, Corner, air.getInt(Vertices),
      air.getInt(Primitive == TessellatorOutputPrimitive::triangle_ccw), DataPtr
  };
  SmallVector<Type *> Tys;
  for (auto Op : Ops)
    Tys.push_back(Op->getType());

  auto Fn = air.getModule()->getOrInsertFunction(
      "dxmt.get_domain_primitive_location",
      llvm::FunctionType::get(
          llvm::StructType::create(Context, {air.getFloatTy(2), air.getByteTy(), air.getByteTy()}, ""), Tys, false
      ),
      Attrs
  );
  auto Return = ir.CreateCall(Fn, Ops);
  return {ir.CreateExtractValue(Return, 0ull), ir.CreateIsNotNull(ir.CreateExtractValue(Return, 1))};
}

static uint32_t
PrimitiveVertices(TessellatorOutputPrimitive Primitive) {
  return Primitive == TessellatorOutputPrimitive::point ? 1 : Primitive == TessellatorOutputPrimitive::line ? 2 : 3;
}

std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
Converter::DomainGetPieceLocation(
    llvm::Value *WorkloadIndex, llvm::Value *First, llvm::Value *ThreadIndex, uint32_t Count,
    TessellatorOutputPrimitive Primitive, llvm::Value *DataPtr
) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{6U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {6U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );
  SmallVector<Value *> Ops{
      WorkloadIndex, First, ThreadIndex, air.getInt(Count), air.getInt(PrimitiveVertices(Primitive)),
      air.getInt(Primitive == TessellatorOutputPrimitive::triangle_ccw), DataPtr
  };
  SmallVector<Type *> Tys;
  for (auto Op : Ops)
    Tys.push_back(Op->getType());
  auto Fn = air.getModule()->getOrInsertFunction(
      "dxmt.get_domain_piece_location",
      llvm::FunctionType::get(
          llvm::StructType::create(Context, {air.getFloatTy(2), air.getByteTy(), air.getByteTy()}, ""), Tys, false
      ),
      Attrs
  );
  auto Return = ir.CreateCall(Fn, Ops);
  return {
      ir.CreateExtractValue(Return, 0ull), ir.CreateIsNotNull(ir.CreateExtractValue(Return, 1)),
      ir.CreateIsNotNull(ir.CreateExtractValue(Return, 2))
  };
}

void
Converter::DomainGeneratePiece(
    llvm::Value *WorkloadIndex, llvm::Value *First, uint32_t Count, llvm::Value *DataPtr,
    TessellatorOutputPrimitive Primitive, llvm::Value *Behind
) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{4U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {4U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {6U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );
  auto TyBehind = ir.getInt32Ty()->getPointerTo(3);
  SmallVector<Value *> Ops{
      WorkloadIndex, First, air.getInt(Count), air.getInt(PrimitiveVertices(Primitive)), DataPtr, air.getMeshHandle(),
      Behind ? Behind : llvm::Constant::getNullValue(TyBehind)
  };
  SmallVector<Type *> Tys;
  for (auto Op : Ops)
    Tys.push_back(Op->getType());
  auto Fn = air.getModule()->getOrInsertFunction(
      "dxmt.domain_generate_piece", llvm::FunctionType::get(air.getVoidTy(), Tys, false), Attrs
  );
  ir.CreateCall(Fn, Ops);
}

void
Converter::DomainGeneratePrimitives(
    llvm::Value *WorkloadIndex, llvm::Value *DataPtr, TessellatorOutputPrimitive Primitive, llvm::Value *Behind
) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {3U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(WorkloadIndex->getType());
  Ops.push_back(WorkloadIndex);
  Tys.push_back(DataPtr->getType());
  Ops.push_back(DataPtr);
  Tys.push_back(air.getMeshHandleType());
  Ops.push_back(air.getMeshHandle());
  // threadgroup memory
  Tys.push_back(ir.getInt32Ty()->getPointerTo(3));
  Ops.push_back(Behind ? Behind : llvm::Constant::getNullValue(Tys.back()));

  std::string FnName = "dxmt.domain_generate_primitives";
  switch (Primitive) {
  case TessellatorOutputPrimitive::point:
    FnName += ".point";
    break;
  case TessellatorOutputPrimitive::line:
    FnName += ".line";
    break;
  case TessellatorOutputPrimitive::triangle:
    FnName += ".triangle";
    break;
  case TessellatorOutputPrimitive::triangle_ccw:
    FnName += ".triangle_ccw";
    break;
  }
  auto Fn = air.getModule()->getOrInsertFunction(FnName, llvm::FunctionType::get(air.getVoidTy(), Tys, false), Attrs);
  ir.CreateCall(Fn, Ops);
}

llvm::Value *
Converter::CreateGEPInt32WithBoundCheck(BufferResourceHandle &Buffer, llvm::Value *Index) {
  auto Addr = ir.CreateGEP(ir.getInt32Ty(), Buffer.Pointer, {Index});
  if (!Buffer.Metadata) {
    return Addr;
  }
  auto ByteLength = DecodeRawBufferByteLength(Buffer.Metadata);
  auto Dwords = ir.CreateZExtOrTrunc(ir.CreateLShr(ByteLength, 2), Index->getType());
  return ir.CreateSelect(ir.CreateICmpULT(Index, Dwords), Addr, llvm::Constant::getNullValue(Addr->getType()));
}

llvm::Value *
Converter::CreateGEPInt32WithBoundCheck(AtomicBufferResourceHandle &Buffer, llvm::Value *Index) {
  auto Addr = ir.CreateGEP(ir.getInt32Ty(), Buffer.Pointer, {Index});
  if (!Buffer.Metadata) {
    return Addr;
  }
  auto ByteLength = DecodeRawBufferByteLength(Buffer.Metadata);
  auto Dwords = ir.CreateZExtOrTrunc(ir.CreateLShr(ByteLength, 2), Index->getType());
  return ir.CreateSelect(ir.CreateICmpULT(Index, Dwords), Addr, llvm::Constant::getNullValue(Addr->getType()));
}

std::unique_ptr<llvm::IRBuilder<>::FastMathFlagGuard>
Converter::UseFastMath(bool OptOut) {
  if (OptOut)
    return nullptr;
  auto Guard = std::make_unique<llvm::IRBuilder<>::FastMathFlagGuard>(ir);
  llvm::FastMathFlags FMF;

  /**
  TODO(airconv): provides option to allow NaN/INF
  TODO(invariance-analysis): for now fp contraction&refactoring is simply disabled for pre-raster stage to reduce the
  chance of depth prepass z-fighting. However if the game (e.g. SotTR) uses two versions of expression (mul+add vs. fma)
  this is not handled yet (need further investigation, probably need a pass for un-fuse/re-fuse)
  */
  if (ctx.shader_type == microsoft::D3D11_SB_COMPUTE_SHADER || ctx.shader_type == microsoft::D3D10_SB_PIXEL_SHADER) {
    FMF.setAllowContract();
    FMF.setAllowReassoc();
    FMF.setAllowReciprocal();
  }

  FMF.setApproxFunc();
  FMF.setNoSignedZeros();
  ir.setFastMathFlags(FMF);
  return Guard;
}

bool
Converter::SupportsMemoryCoherency() const {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

bool
Converter::SupportsNonExecutionBarrier() const {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

} // namespace dxmt::dxbc