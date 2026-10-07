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

#pragma once
#include "llvm/ADT/Optional.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/FMF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

#include "../airconv_public.h"
#include "../dxbc_instructions.hpp"
#include "adt.hpp"
#include "air_builder.hpp"
#include "dxbc_binding_map.hpp"
#include "tl/generator.hpp"
#include "ftl.hpp"

namespace dxmt::dxbc {

using mask_t = uint32_t;

constexpr mask_t kMaskAll = 0b1111;
constexpr mask_t kMaskComponentX = 0b1;
constexpr mask_t kMaskComponentY = 0b10;
constexpr mask_t kMaskComponentZ = 0b100;
constexpr mask_t kMaskComponentW = 0b1000;
constexpr mask_t kMaskVecXY = 0b11;
constexpr mask_t kMaskVecXYZ = 0b111;

struct io_binding_map;
struct context;

struct TextureResourceHandle {
  llvm::air::Texture Texture;
  llvm::air::Texture::ResourceKind Logical;
  llvm::Value *Handle;
  llvm::Value *Metadata;
  Swizzle Swizzle;
  bool GlobalCoherent;
  llvm::Value *Range = nullptr;
};

struct BufferResourceHandle {
  llvm::Value *Pointer;
  llvm::Value *Metadata;    // can be null
  uint32_t StructureStride; // 0 if not structured
  Swizzle Swizzle;
  bool GlobalCoherent;
};

struct AtomicBufferResourceHandle {
  llvm::Value *Pointer;
  llvm::Value *Metadata;  // can be null
  uint32_t StructureStride; // 0 if not structured
  mask_t Mask;
  bool GlobalCoherent;
};

struct UAVCounterHandle {
  llvm::Value *Pointer;
};

struct SamplerHandle {
  llvm::Value *Handle;
  llvm::Value *HandleCube;
  llvm::Value *Bias;
  llvm::Value *Metadata;
  llvm::Value *Border;
};

// a register's interpolants, one per component the shader takes of it
struct InterpolantHandle {
  std::array<llvm::Value *, 4> Component;
  bool Perspective;
};

enum class TessellatorPartitioning {
  integer,
  pow2,
  fractional_odd,
  fractional_even,
};

enum class TessellatorOutputPrimitive {
  point,
  line,
  triangle,
  triangle_ccw,
};

class Converter {
public:
  Converter(llvm::air::AIRBuilder &air, context &ctx_legacy, io_binding_map &res_legacy) :
      air(air),
      ir(air.builder),
      ctx(ctx_legacy),
      res(res_legacy) {}

  /* Load Operands */

  llvm::Value *
  LoadOperandIndex(const uint32_t &SrcOpIndex) {
    return ir.getInt32(SrcOpIndex);
  }
  llvm::Value *LoadOperandIndex(const IndexByIndexableTempComponent &SrcOpIndex);
  llvm::Value *LoadOperandIndex(const IndexByTempComponent &SrcOpIndex);

  llvm::Value *
  LoadOperandIndex(const OperandIndex &SrcOpIndex) {
    return std::visit([this](auto &SrcOpIndex) { return this->LoadOperandIndex(SrcOpIndex); }, SrcOpIndex);
  }
  llvm::Value *LoadOperand(const SrcOperandConstantBuffer &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandImmediateConstantBuffer &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandImmediate32 &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandInput &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandIndexableInput &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandTemp &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandAttribute &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandIndexableTemp &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandInputICP &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandInputPC &SrcOp, mask_t Mask);
  llvm::Value *LoadOperand(const SrcOperandInputOCP &SrcOp, mask_t Mask);

  llvm::Value *
  LoadOperand(const SrcOperand &SrcOp, mask_t Mask) {
    return std::visit([Mask, this](auto &SrcOp) { return this->LoadOperand(SrcOp, Mask); }, SrcOp);
  }

  llvm::Value *ApplySrcModifier(SrcOperandCommon C, llvm::Value *Value, mask_t Mask);

  llvm::Optional<TextureResourceHandle> LoadTexture(const SrcOperandResource &SrcOp);
  llvm::Optional<TextureResourceHandle> LoadTexture(const SrcOperandUAV &SrcOp);
  llvm::Optional<TextureResourceHandle> LoadTexture(const AtomicDstOperandUAV &DstOp);
  llvm::Optional<TextureResourceHandle>
  LoadTexture(const std::variant<SrcOperandResource, SrcOperandUAV> &SrcOp) {
    return std::visit([this](auto &SrcOp) { return this->LoadTexture(SrcOp); }, SrcOp);
  }
  llvm::Optional<TextureResourceHandle>
  LoadTexture(const std::variant<AtomicDstOperandUAV, AtomicOperandTGSM> &SrcOp) {
    return std::visit(
        patterns{
            [](AtomicOperandTGSM &) { return llvm::Optional<TextureResourceHandle>(); },
            [this](auto &DstOp) { return this->LoadTexture(DstOp); }
        },
        SrcOp
    );
  }

  llvm::Optional<SamplerHandle> LoadSampler(const SrcOperandSampler &SrcOp);

  llvm::Optional<BufferResourceHandle> LoadBuffer(const SrcOperandResource &SrcOp);
  llvm::Optional<BufferResourceHandle> LoadBuffer(const SrcOperandUAV &SrcOp);
  llvm::Optional<AtomicBufferResourceHandle> LoadBuffer(const AtomicDstOperandUAV &DstOp);
  llvm::Optional<BufferResourceHandle> LoadBuffer(const SrcOperandTGSM &SrcOp);
  llvm::Optional<AtomicBufferResourceHandle> LoadBuffer(const AtomicOperandTGSM &DstOp);
  llvm::Optional<BufferResourceHandle>
  LoadBuffer(const std::variant<SrcOperandResource, SrcOperandUAV> &SrcOp) {
    return std::visit([this](auto &SrcOp) { return this->LoadBuffer(SrcOp); }, SrcOp);
  }
  llvm::Optional<BufferResourceHandle>
  LoadBuffer(const std::variant<SrcOperandResource, SrcOperandUAV, SrcOperandTGSM> &SrcOp) {
    return std::visit([this](auto &SrcOp) { return this->LoadBuffer(SrcOp); }, SrcOp);
  }
  llvm::Optional<AtomicBufferResourceHandle>
  LoadBuffer(const std::variant<AtomicDstOperandUAV, AtomicOperandTGSM> &SrcOp) {
    return std::visit([this](auto &DstOp) { return this->LoadBuffer(DstOp); }, SrcOp);
  }

  llvm::Optional<UAVCounterHandle> LoadCounter(const AtomicDstOperandUAV &SrcOp);

  llvm::Value *LoadAtomicOpAddress(const AtomicBufferResourceHandle &Handle, const SrcOperand &Address);

  llvm::Optional<InterpolantHandle> LoadInterpolant(uint32_t Index);

  /* Store Operands */

  void
  StoreOperand(const DstOperandNull &DstOp, llvm::Value *Value) {
    // NOP
  }
  void
  StoreOperand(const DstOperandSideEffect &DstOp, llvm::Value *Value) {
    // NOP
  }
  void StoreOperand(const DstOperandOutput &DstOp, llvm::Value *Value);
  void StoreOperand(const DstOperandOutputCoverageMask &DstOp, llvm::Value *Value);
  void StoreOperand(const DstOperandOutputStencilRef &DstOp, llvm::Value *Value);
  void StoreOperand(const DstOperandOutputDepth &DstOp, llvm::Value *Value);
  void StoreOperand(const DstOperandIndexableOutput &DstOp, llvm::Value *Value);
  void StoreOperand(const DstOperandTemp &DstOp, llvm::Value *Value);
  void StoreOperand(const DstOperandIndexableTemp &DstOp, llvm::Value *Value);
  void StoreOperandHull(const DstOperandOutput &DstOp, llvm::Value *Value);
  void StoreOperandHull(const DstOperandIndexableOutput &DstOp, llvm::Value *Value);

  void
  StoreOperand(const DstOperand &DstOp, llvm::Value *Value, bool Saturate = false) {
    std::visit(
        [=, this](auto &DstOp) {
          auto Value_ = Value;
          // sat(NaN) is 0 (D3D11.3 22.19.1), which Metal's fast variant leaves undefined
          if (DstOp._.write_type == OperandDataType::Float && Saturate)
            Value_ = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, Value, false);
          this->StoreOperand(DstOp, Value_);
        },
        DstOp
    );
  }

  void StoreFeedback(const std::optional<DstOperand> &Feedback, llvm::Value *Residency);

  void
  StoreOperandVec4(const DstOperand &DstOp, llvm::Value *ValueVec4, bool Saturate = false) {
    std::visit(
        [=, this](auto &DstOp) {
          auto Value_ = ValueVec4;
          if (DstOp._.write_type == OperandDataType::Float && Saturate)
            Value_ = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, ValueVec4, false);
          this->StoreOperand(DstOp, this->MaskSwizzle(Value_, DstOp._.mask));
        },
        DstOp
    );
  }

  /* Instructions */

  void
  operator()(const InstNop &) {
    // just do nothing
  }

  void
  operator()(const InstPixelDiscard &) {
    air.CreateDiscard();
  }

  void
  operator()(const InstSync &sync) {
    using namespace llvm::air;
    MemFlags mem_flag = sync.tgsm_memory_barrier ? MemFlags::Threadgroup : MemFlags::None;
    if (sync.uav_boundary != InstSync::UAVBoundary::none) {
      mem_flag |= MemFlags::Device | MemFlags::Texture;
      if (SupportsNonExecutionBarrier())
        air.CreateAtomicFence(
            mem_flag,
            // scopes are an enumeration: the device's contains the threadgroup's
            sync.uav_boundary == InstSync::UAVBoundary::global ? ThreadScope::Device : ThreadScope::Threadgroup
        );
      if (sync.tgsm_execution_barrier)
        air.CreateBarrier(SupportsNonExecutionBarrier() ? MemFlags::None : mem_flag);
    } else if (sync.tgsm_execution_barrier) {
      air.CreateBarrier(mem_flag);
    }
  }

  void operator()(const InstMov &);
  void operator()(const InstMovConditional &);
  void operator()(const InstSwapConditional &);
  void operator()(const InstDotProduct &);
  void operator()(const InstFloatUnaryOp &);
  void operator()(const InstFloatBinaryOp &);
  void operator()(const InstIntegerUnaryOp &);
  void operator()(const InstIntegerBinaryOp &);
  void operator()(const InstPartialDerivative &);

  void operator()(const InstConvert &);

  void operator()(const InstFloatCompare &);
  void operator()(const InstFloatMAD &);
  void operator()(const InstSinCos &);
  void operator()(const InstIntegerCompare &);
  void operator()(const InstIntegerMAD &);
  void operator()(const InstIntegerBinaryOpWithTwoDst &);

  void operator()(const InstExtractBits &);
  void operator()(const InstBitFiledInsert &);

  void operator()(const InstLoad &);
  void operator()(const InstLoadUAVTyped &);
  void operator()(const InstStoreUAVTyped &);

  void operator()(const InstSample &);
  void operator()(const InstSampleLOD &);
  void operator()(const InstSampleBias &);
  void operator()(const InstSampleDerivative &);
  void operator()(const InstSampleCompare &);
  void operator()(const InstGather &);
  void operator()(const InstGatherCompare &);
  void operator()(const InstCalcLOD &);
  void operator()(const InstResourceInfo &);

  void operator()(const InstSampleInfo &);
  void operator()(const InstSamplePos &);
  void operator()(const InstBufferInfo &);

  void operator()(const InstLoadRaw &);
  void operator()(const InstStoreRaw &);
  void operator()(const InstLoadStructured &);
  void operator()(const InstStoreStructured &);

  void operator()(const InstAtomicBinOp &);
  void operator()(const InstAtomicImmCmpExchange &);
  void operator()(const InstAtomicImmIncrement &);
  void operator()(const InstAtomicImmDecrement &);

  void operator()(const InstInterpolateCentroid &);
  void operator()(const InstInterpolateSample &);
  void operator()(const InstInterpolateOffset &);

  void operator()(const InstMaskedSumOfAbsDiff &);
  void operator()(const InstEmit &);
  void operator()(const InstCut &);

  /* Utils */

  bool
  IsNull(const DstOperand &DstOp) {
    return std::visit(patterns{[](DstOperandNull &) { return true; }, [](auto &) { return false; }}, DstOp);
  }

  mask_t
  GetMask(const DstOperand &DstOp) {
    return std::visit(
        patterns{
            [](DstOperandNull) { return (uint32_t)0b1111; },
            [](DstOperandSideEffect) { return (uint32_t)0b1111; },
            [](auto dst) { return dst._.mask; },
        },
        DstOp
    );
  }

  llvm::Value *
  MaxIfInMask(uint64_t Mask, llvm::Value *Value) {
    llvm::Value *Max = llvm::ConstantInt::getAllOnesValue(Value->getType());
    return ir.CreateSelect(ir.CreateIsNull(ir.CreateAnd(Value, Mask)), Value, Max);
  }

  int32_t
  ComponentFromScalarMask(mask_t Mask, Swizzle Swizzle) {
    switch (Mask) {
    case 0b1:
      return Swizzle[0];
    case 0b10:
      return Swizzle[1];
    case 0b100:
      return Swizzle[2];
    case 0b1000:
      return Swizzle[3];
    default: {
      // e.g. mov r1.xyzw v1.xxxx
      mask_t AccessMask = MemoryAccessMask(Mask, Swizzle);
      switch (AccessMask) {
      case 0b1:
      case 0b10:
      case 0b100:
      case 0b1000:
        return __builtin_ctz(AccessMask);
      default:
        break;
      }
      break;
    }
    }
    return -1;
  }

  llvm::Value *BitcastToFloat(llvm::Value *Value);
  llvm::Value *BitcastToInt32(llvm::Value *Value);
  llvm::Value *TruncAndBitcastToHalf(llvm::Value *Value);
  llvm::Value *ZExtAndBitcastToInt32(llvm::Value *Value);
  llvm::Value *ZExtAndBitcastToFloat(llvm::Value *Value);

  llvm::Value *MaskSwizzle(llvm::Value *Value, mask_t Mask, Swizzle Swizzle = swizzle_identity);

  llvm::Value *
  ExtractElement(llvm::Value *MaybeVector, uint64_t Element) {
    if (llvm::isa<llvm::VectorType>(MaybeVector->getType())) {
      return ir.CreateExtractElement(MaybeVector, Element);
    }
    return MaybeVector;
  }

  llvm::Value *
  VectorSplat(uint32_t ElementCount, llvm::Value *MaybeScaler) {
    if (auto VecTy = llvm::dyn_cast<llvm::FixedVectorType>(MaybeScaler->getType())) {
      if (VecTy->getNumElements() == ElementCount) {
        return MaybeScaler;
      }
      assert(0 && "invalid vector type");
    }
    return ir.CreateVectorSplat(ElementCount, MaybeScaler);
  }

  tl::generator<std::pair<uint32_t, uint32_t>>
  EnumerateComponents(mask_t Mask) {
    Mask &= kMaskAll;
    unsigned Index = 0;
    while (Mask) {
      co_yield std::pair{__builtin_ctz(Mask), Index++};
      Mask &= Mask - 1; // clear lowest set bit
    }
  }

  llvm::Value *
  ExtractFromCombinedMask(llvm::Value *Value, mask_t CombinedMask, mask_t Mask) {
    if (Mask == CombinedMask)
      return Value;

    switch (Mask) {
      case 0b1:
      case 0b10:
      case 0b100:
      case 0b1000:
        return ir.CreateExtractElement(Value, __builtin_ctz(Mask));
      default:
        break;
    }

    llvm::SmallVector<int, 4> Idx;
    unsigned SrcIdx = 0;
    for (unsigned Bit = 0; Bit < 4; ++Bit) {
      if (CombinedMask & (1 << Bit)) {
        if (Mask & (1 << Bit))
          Idx.push_back(SrcIdx);
        SrcIdx++;
      }
    }

    return ir.CreateShuffleVector(Value, Idx);
  }

  mask_t
  MemoryAccessMask(mask_t Mask, Swizzle Swizzle) {
    mask_t Ret = 0;
    for (unsigned Bit = 0; Bit < 4; ++Bit) {
      if (Mask & (1 << Bit)) {
        Ret |= (1 << Swizzle[Bit]);
      }
    }
    return Ret;
  }

  llvm::Value *
  DecodeTextureBufferOffset(llvm::Value *Metadata) {
    return ir.CreateTrunc(Metadata, air.getIntTy());
  }

  llvm::Value *
  DecodeTextureMinLODClamp(llvm::Value *Metadata) {
    return ir.CreateBitCast(ir.CreateTrunc(Metadata, air.getIntTy()), air.getFloatTy());
  }

  llvm::Value *
  DecodeSamplerBias(llvm::Value *Metadata) {
    return ir.CreateFMul(
        ir.CreateSIToFP(ir.CreateTrunc(Metadata, ir.getInt16Ty()), air.getFloatTy()),
        llvm::ConstantFP::get(air.getFloatTy(), std::ldexp(1.0, -SM50_SAMPLER_METADATA_LOD_FRACTION_BITS))
    );
  }

  // the resource's min-LOD clamp, raised by a shader clamp or applied to an explicit LOD (D3D11.3 5.8.2)
  llvm::Value *
  ClampMinLOD(llvm::Value *Metadata, llvm::Value *LOD) {
    auto Clamp = DecodeTextureMinLODClamp(Metadata);
    return LOD ? air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, Clamp, LOD) : Clamp;
  }

  // D3D's lod: the LOD a sample would use and the LOD before any clamp, both with the sampler's bias (D3D11.3
  // 22.5.6, 7.18.11). the clamp is the sampler's MinLOD and MaxLOD, the view's min-LOD clamp and its mips. `Level`,
  // unbiased, replaces Metal's where it has no derivatives
  std::pair<llvm::Value *, llvm::Value *> CalculateLOD(
      const TextureResourceHandle &Tex, llvm::Value *Sampler, llvm::Value *SamplerMetadata, llvm::Value *Coord,
      llvm::Value *Level = nullptr
  );

  // D3D's gather, with `Reference` its comparison (D3D11.3 22.4.2-22.4.5). an offset is the low 6 bits of each
  // component, signed; Metal's native offset reaches [-8, 7], so other offsets move the coordinate by whole texels of
  // the view's first mip. gather reads only that mip: a view min-LOD clamp that removes it gives the out-of-bounds 0,
  // compared against the reference with the sampler's comparison function (5.8.6.1)
  std::pair<llvm::Value *, llvm::Value *> Gather(
      const TextureResourceHandle &Tex, llvm::Value *Sampler, llvm::Value *SamplerMetadata, llvm::Value *Coord,
      llvm::Value *ArrayIndex, llvm::Value *Offset, llvm::Value *Component, llvm::Value *Reference,
      llvm::Value *Border = nullptr
  );

  // 1.0 when `Reference` passes the sampler's comparison function against `Texel`, else 0.0
  llvm::Value *SamplerCompare(llvm::Value *SamplerMetadata, llvm::Value *Reference, llvm::Value *Texel);

  // `Value`, what `Sampler` read (a sample, a gather of `Component`, or a comparison of either with `Reference`), with
  // the sampler's custom border color (SM50_SAMPLER_BORDER). `Read(Sampler)` repeats the read without its comparison
  llvm::Value *CustomBorder(
      const TextureResourceHandle &Tex, llvm::Value *Sampler, llvm::Value *Border, llvm::Value *SamplerMetadata,
      llvm::Value *Value, const std::function<llvm::Value *(llvm::Value *)> &Read, llvm::Value *Reference = nullptr,
      llvm::Value *Component = nullptr
  );

  // LOD is log2 of the gradient length, so a sampler bias b scales the gradients by 2^b (D3D11.3 7.18.11)
  llvm::Value *
  BiasGradient(llvm::Value *Gradient, llvm::Value *Bias) {
    auto Scale = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, Bias);
    auto Ty = llvm::dyn_cast<llvm::FixedVectorType>(Gradient->getType());
    return ir.CreateFMul(Gradient, Ty ? ir.CreateVectorSplat(Ty->getNumElements(), Scale) : Scale);
  }

  llvm::Value *LoadConstantBuffer(const ConstantBufferDescriptor &Buffer, llvm::Value *Row, int Component = -1);

  // a D3D texel address (texel coordinates, then the array slice) as Metal's position and array index. texel
  // buffers are addressed inside their view. a position past the view, or at a `Level` below the view's min-LOD
  // clamp (D3D11.3 5.8.5), becomes all ones: Metal reads zero there and drops the write
  std::pair<llvm::Value *, llvm::Value *>
  TexelAddress(const TextureResourceHandle &Tex, llvm::Value *Coord, const int32_t Offset[3], llvm::Value *Level);

  // D3D converts float to half toward zero, with overflow to the largest finite half (D3D11.3 3.2.2)
  llvm::Value *ConvertToHalfTowardZero(llvm::Value *Value);

  llvm::Value *FirstBit(IntegerUnaryOp Op, llvm::Value *Value);
  llvm::Value *SinCos(llvm::air::AIRBuilder::FPUnOp Op, llvm::Value *Value);
  llvm::Value *ExtractBits(llvm::Value *Width, llvm::Value *Offset, llvm::Value *Src, bool Signed);
  llvm::Value *InsertBits(llvm::Value *Width, llvm::Value *Offset, llvm::Value *Src, llvm::Value *Dst);
  llvm::Value *MaskedSumOfAbsDiff(llvm::Value *Ref, llvm::Value *Src, llvm::Value *Accum);
  // an eval_snapped offset in 1/16 pixel steps, [-8, 7]
  llvm::Value *InterpolateAtOffset(const InterpolantHandle &Itp, llvm::Value *Offset);
  // the register, each of its interpolants evaluated by `At`
  llvm::Value *
  Interpolate(const InterpolantHandle &Itp, auto &&At) {
    llvm::Value *Value = llvm::ConstantAggregateZero::get(air.getFloatTy(4));
    for (unsigned i = 0; i < 4; i++)
      if (Itp.Component[i])
        Value = ir.CreateInsertElement(Value, At(Itp.Component[i]), i);
    return Value;
  }

  // width, height, depth or array length, and mip count, as the D3D resinfo instruction returns them
  llvm::Value *TextureDimensions(const TextureResourceHandle &Tex, llvm::Value *Level);

  BufferResourceHandle
  MakeBufferHandle(const BufferDescriptor &Descriptor, Swizzle Swizzle) {
    return {
        Descriptor.Pointer, Descriptor.Metadata, Descriptor.StructureStride, Swizzle,
        Descriptor.GlobalCoherent && SupportsMemoryCoherency()
    };
  }

  TextureResourceHandle
  MakeTextureHandle(const TextureDescirptor &Descriptor, Swizzle Swizzle) {
    llvm::air::Texture Texture;
    Texture.kind = Descriptor.ResourceKind;
    Texture.memory_access = Descriptor.MemoryAccess;
    Texture.sample_type = Descriptor.SampleType;
    return {
        Texture,
        Descriptor.ResourceKindLogical,
        Descriptor.ResourceHandle,
        Descriptor.Metadata,
        Swizzle,
        Descriptor.GlobalCoherent && SupportsMemoryCoherency(),
        Descriptor.Range
    };
  }

  // a 3D UAV's W range, which a Metal view cannot select: its metadata holds the first W slice where a texture holds
  // its array length, and the W size in the low half (0: the whole texture). null for other textures
  llvm::Value *
  UAVWSize(const TextureResourceHandle &Tex) {
    if (Tex.Logical != llvm::air::Texture::texture3d || Tex.Texture.memory_access != llvm::air::Texture::acesss_readwrite)
      return nullptr;
    return ir.CreateTrunc(Tex.Metadata, air.getIntTy());
  }

  llvm::Value *
  DecodeTextureArrayLength(llvm::Value *Metadata) {
    return ir.CreateTrunc(ir.CreateLShr(Metadata, 32uLL), air.getIntTy());
  }

  llvm::Value *
  DecodeRawBufferByteLength(llvm::Value *Metadata) {
    return ir.CreateTrunc(Metadata, air.getIntTy());
  }

  llvm::Value *
  DecodeTextureBufferElement(llvm::Value *Metadata) {
    return ir.CreateTrunc(ir.CreateLShr(Metadata, 32uLL), air.getIntTy());
  }

  llvm::Value *ClampArrayIndex(llvm::Value *ShaderValue, llvm::Value *Metadata);


  // `Domain`: quad, triangle or isoline. `Capacity`: the workloads `DataPtr` has room for. `TessFactors`: the
  // inside's, then the edges'; an isoline's density, then its detail
  void HullGenerateWorkload(
      const char *Domain, llvm::Value *PatchIndex, llvm::Value *CountPtr, llvm::Value *DataPtr, uint32_t Capacity,
      TessellatorPartitioning Partitioning, llvm::ArrayRef<llvm::Value *> TessFactors
  );

  llvm::Value *DomainGetPatchIndex(llvm::Value *WorkloadIndex, llvm::Value *DataPtr);
  std::pair<llvm::Value *, llvm::Value *> DomainGetPrimitiveLocation(
      llvm::Value *WorkloadIndex, llvm::Value *PrimitiveIndex, llvm::Value *Corner, TessellatorOutputPrimitive Primitive,
      llvm::Value *DataPtr
  );

  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
  DomainGetLocation(llvm::Value *WorkloadIndex, llvm::Value *ThreadIndex, llvm::Value *DataPtr);

  // the same for a threadgroup that makes `Count` of the workload's primitives, from `First`, each of vertices of
  // its own
  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *> DomainGetPieceLocation(
      llvm::Value *WorkloadIndex, llvm::Value *First, llvm::Value *ThreadIndex, uint32_t Count,
      TessellatorOutputPrimitive Primitive, llvm::Value *DataPtr
  );
  void DomainGeneratePiece(
      llvm::Value *WorkloadIndex, llvm::Value *First, uint32_t Count, llvm::Value *DataPtr,
      TessellatorOutputPrimitive Primitive, llvm::Value *Behind
  );

  void
  // `Behind`: for each vertex, a bit for every cull distance it is behind, in threadgroup memory; null without any
  DomainGeneratePrimitives(
      llvm::Value *WorkloadIndex, llvm::Value *DataPtr, TessellatorOutputPrimitive Primitive, llvm::Value *Behind
  );

  llvm::Value * CreateGEPInt32WithBoundCheck(BufferResourceHandle &Buffer, llvm::Value* Index);

  llvm::Value * CreateGEPInt32WithBoundCheck(AtomicBufferResourceHandle &Buffer, llvm::Value* Index);

  [[nodiscard]]
  std::unique_ptr<llvm::IRBuilder<>::FastMathFlagGuard> UseFastMath(bool OptOut);

  bool
  IsConstantZero(llvm::Value *Value) const {
    return llvm::isa<llvm::Constant>(Value) && llvm::cast<llvm::Constant>(Value)->isZeroValue();
  }

  bool
  IsInfinity(llvm::Value *Value) const {
    return llvm::isa<llvm::ConstantFP>(Value) && llvm::cast<llvm::ConstantFP>(Value)->isInfinity();
  }

  bool SupportsMemoryCoherency() const;
  bool SupportsNonExecutionBarrier() const;

private:
  llvm::air::AIRBuilder &air;
  llvm::IRBuilderBase &ir;

  /* Legacy */
  context &ctx;
  io_binding_map &res;
};

} // namespace dxmt::dxbc