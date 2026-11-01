#pragma once

#include "air_builder.hpp"
#include "llvm/IR/Value.h"
#include <optional>

namespace mullion::dxbc {

struct ConstantBufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata; // may be null
};

struct SamplerDescriptor {
  llvm::Value *SamplerHandle;
  llvm::Value *CubeSamplerHandle;
  llvm::Value *Metadata;
  llvm::Value *Border = nullptr; // SM50_SAMPLER_BORDER's address, when the binding can have one
  // a static sampler's, known when the pipeline is made: whether it filters anisotropically, whether its metadata
  // holds a LOD bias, and whether it is a minimum or maximum filter
  std::optional<bool> Anisotropic, Biased, Reduces;
};

struct TextureDescirptor {
  llvm::Value *ResourceHandle;
  llvm::Value *Metadata;
  bool GlobalCoherent;
  llvm::air::Texture::ResourceKind ResourceKind;
  llvm::air::Texture::ResourceKind ResourceKindLogical;
  llvm::air::Texture::MemoryAccess MemoryAccess;
  llvm::air::Texture::SampleType SampleType;
  llvm::Value *Range = nullptr; // where its format range is (SM50_SAMPLER_BORDER), when the binding has one
  bool IsUAV = false;
};

struct BufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata;
  uint32_t StructureStride;
  bool GlobalCoherent;
};

struct CounterDescriptor {
  llvm::Value *Pointer;
};

using RangeId = uint32_t;

class BindingMap {
public:
  virtual ~BindingMap() {};
  virtual llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  // the device's table of lock words for 64-bit atomics (word 0 its index mask), where the binding carries one
  virtual llvm::Value *
  GetAtomicLocks(llvm::air::AIRBuilder &Builder) {
    return nullptr;
  }
  virtual llvm::Value *
  GetSparseBufferMappings(llvm::air::AIRBuilder &Builder) {
    return nullptr;
  }
};

} // namespace mullion::dxbc