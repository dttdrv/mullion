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

#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "mullion_sampler.hpp"
#include "util_math.hpp"
#include "util_md5.hpp"
#include <bit>
#include <cstring>
#include <vector>
#include "../d3d10/d3d10_blob.hpp"
#include "../airconv/dxbc_root_signature.hpp"

namespace mullion {

#pragma region Serialization

template <typename ROOT_SIGNATURE_DESC>
HRESULT
SerializeVersionedRootSignatureImpl(
    const ROOT_SIGNATURE_DESC *pDesc, D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob
) {
  using ROOT_PARAMETER = GetType<ROOT_SIGNATURE_DESC>::ROOT_PARAMETER;
  using ROOT_DESCRIPTOR = GetType<ROOT_SIGNATURE_DESC>::ROOT_DESCRIPTOR;
  using DESCRIPTOR_RANGE = GetType<ROOT_SIGNATURE_DESC>::DESCRIPTOR_RANGE;
  using STATIC_SAMPLER = GetType<ROOT_SIGNATURE_DESC>::STATIC_SAMPLER;

  struct segment {
    size_t offset;
    size_t size_aligned;
    void *ptr;
    segment(size_t offset, size_t size) {
      this->offset = offset;
      size_aligned = align(size, 4);
      ptr = malloc(size_aligned);
      memset(ptr, 0, size_aligned);
    }
    ~segment() {
      free(ptr);
    };
    segment(const segment &copy) = delete;
    segment(segment &&move) {
      ptr = move.ptr;
      size_aligned = move.size_aligned;
      offset = move.offset;
      move.ptr = nullptr;
      move.size_aligned = 0;
      move.offset = 0;
    };
  };
  std::vector<segment> segments;
  size_t total_offset = 0;

  auto add_segment = [&]<typename TYPE>(size_t count, TYPE **ptr) -> size_t {
    if (count == 0) {
      *ptr = nullptr;
      return total_offset;
    }
    size_t size = sizeof(TYPE) * count;
    segments.emplace_back(total_offset, size);
    total_offset += size;
    *ptr = reinterpret_cast<TYPE *>(segments.back().ptr);
    return segments.back().offset;
  };

  RawRootSignatureDesc *RootSig = nullptr;

  add_segment(1, &RootSig);
  if (!RootSig)
    return FAIL(E_OUTOFMEMORY);

  RootSig->Version = (uint32_t)Version;
  RootSig->Flags = (uint32_t)pDesc->Flags;
  RootSig->NumParameters = pDesc->NumParameters;
  RootSig->NumStaticSamplers = pDesc->NumStaticSamplers;

  /* Root Parameters */

  RawRootParameter *Parameters = nullptr;
  RootSig->RootParametersOffset = add_segment(pDesc->NumParameters, &Parameters);
  if (pDesc->NumParameters && !Parameters)
    return FAIL(E_OUTOFMEMORY);

  for (UINT i = 0; i < pDesc->NumParameters; i++) {
    const ROOT_PARAMETER *RootParameter = &pDesc->pParameters[i];
    RawRootParameter *Out = &Parameters[i];
    Out->ParameterType = (uint32_t)RootParameter->ParameterType;
    Out->ShaderVisibility = (uint32_t)RootParameter->ShaderVisibility;
    switch (RootParameter->ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      RawRootDescriptorTable *OutTable = nullptr;
      Out->PayloadOffset = add_segment(1, &OutTable);
      if (!OutTable)
        return FAIL(E_OUTOFMEMORY);
      OutTable->NumDescriptorRanges = RootParameter->DescriptorTable.NumDescriptorRanges;
      DESCRIPTOR_RANGE *OutRanges = nullptr;
      OutTable->DescriptorRangesOffset = add_segment(RootParameter->DescriptorTable.NumDescriptorRanges, &OutRanges);
      if (OutTable->NumDescriptorRanges && !OutRanges)
        return FAIL(E_OUTOFMEMORY);
      for (UINT j = 0; j < RootParameter->DescriptorTable.NumDescriptorRanges; j++) {
        const DESCRIPTOR_RANGE *Range = &RootParameter->DescriptorTable.pDescriptorRanges[j];
        DESCRIPTOR_RANGE *OutRange = &OutRanges[j];
        OutRange->RangeType = Range->RangeType;
        OutRange->NumDescriptors = Range->NumDescriptors;
        OutRange->BaseShaderRegister = Range->BaseShaderRegister;
        OutRange->RegisterSpace = Range->RegisterSpace;
        OutRange->OffsetInDescriptorsFromTableStart = Range->OffsetInDescriptorsFromTableStart;
        SetFlag(OutRange, Range);
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
      D3D12_ROOT_CONSTANTS *OutConstants = nullptr;
      Out->PayloadOffset = add_segment(1, &OutConstants);
      if (!OutConstants)
        return FAIL(E_OUTOFMEMORY);
      *OutConstants = RootParameter->Constants;
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV: {
      ROOT_DESCRIPTOR *OutDescriptor = nullptr;
      Out->PayloadOffset = add_segment(1, &OutDescriptor);
      if (!OutDescriptor)
        return FAIL(E_OUTOFMEMORY);
      OutDescriptor->ShaderRegister = RootParameter->Descriptor.ShaderRegister;
      OutDescriptor->RegisterSpace = RootParameter->Descriptor.RegisterSpace;
      SetFlag(OutDescriptor, &RootParameter->Descriptor);
      break;
    }
    }
  }

  /* Static Samplers */

  STATIC_SAMPLER *StaticSamplers = nullptr;
  RootSig->StaticSamplersOffset = add_segment(pDesc->NumStaticSamplers, &StaticSamplers);
  if (pDesc->NumStaticSamplers) {
    if (!StaticSamplers)
      return FAIL(E_OUTOFMEMORY);
    memcpy(StaticSamplers, pDesc->pStaticSamplers, sizeof(STATIC_SAMPLER) * pDesc->NumStaticSamplers);
  }

  struct DXBC_ROOT_SIGNATURE_BLOB_HEADER {
    microsoft::DXBCHeader DXBCHeader;
    UINT RTSOBlobOffset;
    microsoft::DXBCBlobHeader RTSOBlobHeader;
  };

  auto TotalSize = sizeof(DXBC_ROOT_SIGNATURE_BLOB_HEADER) + total_offset;

  ID3D10Blob *blob = nullptr;
  HRESULT hr = CreateBlobFromMalloc(TotalSize, &blob);
  if (FAILED(hr))
    return hr;
  char *OutBlob = reinterpret_cast<char *>(blob->GetBufferPointer());

  DXBC_ROOT_SIGNATURE_BLOB_HEADER *pHeader = (DXBC_ROOT_SIGNATURE_BLOB_HEADER *)OutBlob;
  pHeader->DXBCHeader.DXBCHeaderFourCC = microsoft::DXBC_FOURCC_NAME;
  pHeader->DXBCHeader.BlobCount = 1;
  pHeader->DXBCHeader.Version = {1, 0};
  pHeader->DXBCHeader.ContainerSizeInBytes = TotalSize;
  pHeader->DXBCHeader.Hash = {};

  pHeader->RTSOBlobOffset = sizeof(DXBC_ROOT_SIGNATURE_BLOB_HEADER) - sizeof(microsoft::DXBCBlobHeader);
  pHeader->RTSOBlobHeader.BlobFourCC = microsoft::DXBC_RootSignature;
  pHeader->RTSOBlobHeader.BlobSize = total_offset;

  for (auto &seg : segments) {
    memcpy(OutBlob + sizeof(DXBC_ROOT_SIGNATURE_BLOB_HEADER) + seg.offset, seg.ptr, seg.size_aligned);
  }

  auto hash = md5::hashDxbcBinary(OutBlob, TotalSize);
  memcpy(pHeader->DXBCHeader.Hash.Digest, hash.data.data(), sizeof(md5::Digest));

  *ppBlob = blob;
  return hr;
};

extern "C" HRESULT WINAPI
D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignatureDesc, ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob
) {
  // InitReturnPtr(ppBlob); // deliberately, don't touch it
  InitReturnPtr(ppErrorBlob);

  if (!ppBlob)
    return FAIL(E_INVALIDARG);

  switch (pRootSignatureDesc->Version) {
  default:
    break;
  case D3D_ROOT_SIGNATURE_VERSION_1:
    return SerializeVersionedRootSignatureImpl<D3D12_ROOT_SIGNATURE_DESC>(
        &pRootSignatureDesc->Desc_1_0, D3D_ROOT_SIGNATURE_VERSION_1, ppBlob, ppErrorBlob
    );
  case D3D_ROOT_SIGNATURE_VERSION_1_1:
    return SerializeVersionedRootSignatureImpl<D3D12_ROOT_SIGNATURE_DESC1>(
        &pRootSignatureDesc->Desc_1_1, D3D_ROOT_SIGNATURE_VERSION_1_1, ppBlob, ppErrorBlob
    );
  case RootSignatureVersion12:
    return SerializeVersionedRootSignatureImpl<RootSignatureDesc2>(
        &reinterpret_cast<const VersionedRootSignatureDesc2 *>(pRootSignatureDesc)->Desc_1_2,
        RootSignatureVersion12, ppBlob, ppErrorBlob
    );
  }

  return FAIL(E_INVALIDARG);
}

extern "C" HRESULT WINAPI
D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *pRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob **ppBlob,
    ID3DBlob **ppErrorBlob
) {
  // InitReturnPtr(ppBlob); // deliberately, don't touch it
  InitReturnPtr(ppErrorBlob);

  if (!ppBlob || Version != D3D_ROOT_SIGNATURE_VERSION_1_0)
    return FAIL(E_INVALIDARG);

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned_desc;
  versioned_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  versioned_desc.Desc_1_0 = *pRootSignatureDesc;
  return D3D12SerializeVersionedRootSignature(&versioned_desc, ppBlob, ppErrorBlob);
}

#pragma endregion
#pragma region Deserialization

template <typename Base> class MTLD3D12RootSignatureDeserializer : public ComObject<Base> {
public:
  virtual HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    if (riid == __uuidof(Base)) {
      *ppvObject = ref_and_cast<Base>(this);
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  virtual const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetRootSignatureDesc() {
    return &impl_.desc_1_0_.Desc_1_0;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetRootSignatureDescAtVersion(
      D3D_ROOT_SIGNATURE_VERSION Version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **ppDesc
  ) {
    switch (Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
      *ppDesc = &impl_.desc_1_0_;
      break;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
      *ppDesc = &impl_.desc_1_1_;
      break;
    case RootSignatureVersion12:
      *ppDesc = reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&impl_.desc_1_2_);
      break;
    default:
      return FAIL(E_INVALIDARG);
    }
    return S_OK;
  }

  virtual const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetUnconvertedRootSignatureDesc() {
    switch (impl_.raw_root_sig_->Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
      return &impl_.desc_1_0_;
    case RootSignatureVersion12:
      return reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&impl_.desc_1_2_);
    default:
      return &impl_.desc_1_1_;
    }
  }

  MTLD3D12RootSignatureDeserializer() {}

  HRESULT
  Deserialize(const void *pBytecode, SIZE_T DataSize) {
    HRESULT hr = impl_.Deserialize(pBytecode, DataSize);
    if (FAILED(hr))
      return hr;
    if constexpr (std::is_same_v<Base, ID3D12VersionedRootSignatureDeserializer>) {
      if (impl_.desc_1_2_.Version != RootSignatureVersion12) {
        hr = impl_.ConvertVersionedRootSignature(impl_.desc_1_1_.Desc_1_1, impl_.desc_1_2_.Desc_1_2);
        impl_.desc_1_2_.Version = RootSignatureVersion12;
      }
    }
    return hr;
  }

private:
  RootSignatureDeserializer impl_;
};

// the serialized root signature in a container of the length the caller gives. what is not a container is an invalid
// argument; a whole container with no root signature in it is E_FAIL on Windows (vkd3d-proton's
// test_root_signature_empty_blob: "Has to be E_FAIL, not E_INVALIDARG, oddly enough")
static HRESULT
RootSignaturePart(
    const void *pBytecode, SIZE_T Length, const void **ppPart, UINT *pSize, LPCWSTR Name = nullptr, bool *FromLibrary = nullptr
) {
  if (!pBytecode || Length < sizeof(microsoft::DXBCHeader) ||
      microsoft::DXBCGetSizeAssumingValidPointer(pBytecode) > Length ||
      !ShaderContainerHolds(pBytecode, microsoft::DXBCGetSizeAssumingValidPointer(pBytecode)))
    return FAIL(E_INVALIDARG);
  if (!Name && SUCCEEDED(microsoft::DXBCGetRootSignature(pBytecode, ppPart, pSize)))
    return S_OK;
  bool found = false;
  for (auto &subobject : ReadSubobjects({pBytecode, Length})) {
    bool global = subobject.type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    bool local = subobject.type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    if (!(Name ? (global || local) && subobject.name == Name : global))
      continue;
    if (found || subobject.bytes.empty())
      return FAIL(E_INVALIDARG);
    found = true;
    *ppPart = subobject.bytes.data();
    *pSize = subobject.bytes.size();
  }
  if (FromLibrary)
    *FromLibrary = found;
  return found ? S_OK : FAIL(E_FAIL);
}

extern "C" HRESULT WINAPI
D3D12CreateVersionedRootSignatureDeserializer(
    const void *pBytecode, SIZE_T DataSize, REFIID iid, void **ppDeserializer
) {
  InitReturnPtr(ppDeserializer);

  const void *pRawRootSig;
  UINT RawRootSigSize;
  HRESULT hr = RootSignaturePart(pBytecode, DataSize, &pRawRootSig, &RawRootSigSize);
  if (FAILED(hr))
    return hr;

  auto deserializer = Com(new MTLD3D12RootSignatureDeserializer<ID3D12VersionedRootSignatureDeserializer>());
  hr = deserializer->Deserialize(pRawRootSig, RawRootSigSize);
  if (FAILED(hr))
    return hr;
  return deserializer->QueryInterface(iid, ppDeserializer);
}

HRESULT
DeserializeLibraryRootSignature(const void *Library, SIZE_T Size, LPCWSTR Name, REFIID riid, void **Deserializer) {
  InitReturnPtr(Deserializer);
  if (!Name)
    return FAIL(E_INVALIDARG);
  const void *part;
  UINT length;
  HRESULT hr = RootSignaturePart(Library, Size, &part, &length, Name);
  if (FAILED(hr))
    return hr;
  auto deserializer = Com(new MTLD3D12RootSignatureDeserializer<ID3D12VersionedRootSignatureDeserializer>());
  hr = deserializer->Deserialize(part, length);
  return FAILED(hr) ? hr : deserializer->QueryInterface(riid, Deserializer);
}

extern "C" HRESULT WINAPI
D3D12CreateRootSignatureDeserializer(const void *pBytecode, SIZE_T DataSize, REFIID iid, void **ppDeserializer) {
  InitReturnPtr(ppDeserializer);

  const void *pRawRootSig;
  UINT RawRootSigSize;
  HRESULT hr = RootSignaturePart(pBytecode, DataSize, &pRawRootSig, &RawRootSigSize);
  if (FAILED(hr))
    return hr;

  auto deserializer = Com(new MTLD3D12RootSignatureDeserializer<ID3D12RootSignatureDeserializer>());
  hr = deserializer->Deserialize(pRawRootSig, RawRootSigSize);
  if (FAILED(hr))
    return hr;
  return deserializer->QueryInterface(iid, ppDeserializer);
}

#pragma endregion

// each parameter's first qword in the root arguments, its type and the 32-bit values it has when it is constants,
// and the qwords they take together
struct RootSignatureSlots {
  std::vector<uint32_t> qword_offsets;
  std::vector<D3D12_ROOT_PARAMETER_TYPE> types;
  std::vector<uint32_t> constants;
  uint32_t qwords = 0;
};

inline RootSignatureSlots
CalculateRootSignatureQwordOffsets(const D3D12_ROOT_SIGNATURE_DESC1 &Desc) {
  RootSignatureSlots slots;

  for (unsigned i = 0; i < Desc.NumParameters; i++) {
    auto &Parameter = Desc.pParameters[i];

    switch (Parameter.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      slots.qword_offsets.push_back(slots.qwords++);
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      slots.qword_offsets.push_back(slots.qwords);
      slots.qwords += (Parameter.Constants.Num32BitValues / 2) + (Parameter.Constants.Num32BitValues & 1);
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      slots.qword_offsets.push_back(slots.qwords++);
      break;
    default:
      continue;
    }
    slots.types.push_back(Parameter.ParameterType);
    slots.constants.push_back(
        Parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? Parameter.Constants.Num32BitValues : 0
    );
  }

  return slots;
}

class MTLD3D12RootSignatureImpl : public MTLD3D12DeviceChild<MTLD3D12RootSignature> {

  std::vector<uint8_t> blob_;
  LPCWSTR subobject_;
  std::vector<uint32_t> qword_offsets_;
  std::vector<D3D12_ROOT_PARAMETER_TYPE> slot_types_;
  std::vector<uint32_t> slot_constants_;

  std::vector<Rc<Sampler>> static_samplers_; // which is not really "static"
  /**
  It's supposed to be implemented by `constexpr sampler` in Metal shader, but
  for unknown reason it lacks of `mirrored_clamp_to_edge` address mode.
  We may lower that with an extra `abs()` to sample coord though...
  Let's do it in the simple way
  */
  std::vector<uint64_t> static_samplers_encoded_;
  WMT::Reference<WMT::Buffer> static_samplers_buffer_;

public:
  // of a container's root signature, or of the root signature subobject of that name in a library
  MTLD3D12RootSignatureImpl(
      MTLD3D12Device *pDevice, const void *pBytecode, SIZE_T BytecodeLength, LPCWSTR Subobject = nullptr
  ) :
      MTLD3D12DeviceChild<MTLD3D12RootSignature>(pDevice),
      subobject_(Subobject) {
    blob_.resize(BytecodeLength);
    memcpy(blob_.data(), pBytecode, BytecodeLength);
  }

  ~MTLD3D12RootSignatureImpl() {
    // shaders read the static samplers by the buffer's address: it stays for the work in flight (~MTLD3D12Buffer)
    if (static_samplers_buffer_)
      EndAfterWorkInFlight(device_, [device = device_, buffer = std::move(static_samplers_buffer_)] {
        device->UnregisterResidency(buffer);
      });
  }

  HRESULT
  Initialize() {
    const void *pRawRootSig;
    UINT RawRootSigSize;
    bool from_library = false;
    HRESULT hr =
        RootSignaturePart(blob_.data(), blob_.size(), &pRawRootSig, &RawRootSigSize, subobject_, &from_library);
    // the name is the caller's, for the length of the call
    subobject_ = nullptr;
    if (FAILED(hr))
      return hr;

    uint32_t Version;
    if (RawRootSigSize < sizeof(Version))
      return FAIL(E_INVALIDARG);
    memcpy(&Version, pRawRootSig, sizeof(Version));
    D3D12_FEATURE_DATA_ROOT_SIGNATURE support{RootSignatureVersion12};
    hr = device_->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &support, sizeof(support));
    if (FAILED(hr))
      return hr;
    if (support.HighestVersion < Version)
      return FAIL(E_INVALIDARG);

    RootSignatureDeserializer deserializer;
    hr = deserializer.Deserialize(pRawRootSig, RawRootSigSize);
    if (FAILED(hr))
      return hr;

    auto &desc = deserializer.desc_1_1_.Desc_1_1;
    if (from_library) {
      Com<ID3DBlob> serialized;
      hr = D3D12SerializeVersionedRootSignature(&deserializer.desc_1_1_, &serialized, nullptr);
      if (FAILED(hr))
        return hr;
      auto bytes = static_cast<const uint8_t *>(serialized->GetBufferPointer());
      blob_.assign(bytes, bytes + serialized->GetBufferSize());
      if (FAILED(microsoft::DXBCGetRootSignature(blob_.data(), &pRawRootSig, &RawRootSigSize)))
        return FAIL(E_FAIL);
    }
    Digest = Sha1HashState::compute(pRawRootSig, RawRootSigSize);

    if (desc.NumParameters) {
      const bool local = desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
      constexpr auto local_limit =
          D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE - D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
      uint64_t root_cost = 0, local_bytes = 0;
      for (unsigned i = 0; i < desc.NumParameters; i++) {
        if (!desc.pParameters)
          return FAIL(E_INVALIDARG);
        auto &parameter = desc.pParameters[i];
        // DXR, Shader table memory initialization: local arguments use their byte sizes and alignment.
        if (local) {
          if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            local_bytes += uint64_t(parameter.Constants.Num32BitValues) * sizeof(UINT);
          else {
            auto bytes = parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE
                             ? sizeof(D3D12_GPU_DESCRIPTOR_HANDLE) : sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
            local_bytes = align(local_bytes, uint64_t(bytes)) + bytes;
          }
          if (local_bytes > local_limit)
            return FAIL(E_INVALIDARG);
        }
        // Microsoft Learn, Root Signature Limits: constants cost a DWORD, tables one, root descriptors two.
        switch (parameter.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
          root_cost++;
          break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
          root_cost += parameter.Constants.Num32BitValues;
          break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: {
          root_cost += sizeof(D3D12_GPU_VIRTUAL_ADDRESS) / sizeof(UINT);
          auto flags = parameter.Descriptor.Flags;
          constexpr auto data = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE |
                                D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE |
                                D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
          if ((flags & ~data) || std::popcount(UINT(flags)) > 1)
            return FAIL(E_INVALIDARG);
          break;
        }
        }
        if (!local && root_cost > D3D12_MAX_ROOT_COST)
          return FAIL(E_INVALIDARG);
        if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
          bool is_normal_range = false, is_sampler_range = false;
          for (unsigned j = 0; j < parameter.DescriptorTable.NumDescriptorRanges; j++) {
            if (!parameter.DescriptorTable.pDescriptorRanges)
              return FAIL(E_INVALIDARG);
            auto &range = parameter.DescriptorTable.pDescriptorRanges[j];
            auto flags = range.Flags;
            constexpr auto data = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE |
                                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE |
                                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
            constexpr auto descriptors = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                         D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS;
            // Resource Binding: static-bounds descriptors keep static properties, so cannot also be volatile.
            if ((flags & ~(data | descriptors)) || std::popcount(UINT(flags & data)) > 1 ||
                std::popcount(UINT(flags & descriptors)) > 1 ||
                ((flags & D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) &&
                 (flags & D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC)) ||
                (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER &&
                 (flags & data)))
              return FAIL(E_INVALIDARG);
            switch (range.RangeType) {
            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
            case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
            case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
              is_normal_range = true;
              break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
              is_sampler_range = true;
              break;
            default:
              return FAIL(E_INVALIDARG);
            }
          }
          if (is_normal_range && is_sampler_range)
            return FAIL(E_INVALIDARG);
        }
      }
    }

    // two bindings that one stage sees (the same visibility, or one of them every stage's) cannot name one register
    // of one space: Direct3D 12 refuses the root signature (Microsoft's verifier, DirectXShaderCompiler's
    // DxilRootSignatureValidator.cpp, AddRegisterRange; vkd3d-proton's test_create_root_signature on Windows).
    // a root constant buffer or constants take a `b` register, a static sampler an `s` one
    struct Bound {
      UINT kind, space;
      uint64_t first, last;
      D3D12_SHADER_VISIBILITY stages;
    };
    std::vector<Bound> bound;
    auto bind = [&](UINT kind, UINT space, UINT first, UINT count, D3D12_SHADER_VISIBILITY stages) {
      // an unbounded range's count is UINT_MAX: it takes every register from its first
      Bound next{kind, space, first, uint64_t(first) + count - 1, stages};
      for (auto &b : bound)
        if (b.kind == kind && b.space == space && b.first <= next.last && next.first <= b.last &&
            (b.stages == stages || b.stages == D3D12_SHADER_VISIBILITY_ALL || stages == D3D12_SHADER_VISIBILITY_ALL))
          return false;
      bound.push_back(next);
      return true;
    };
    for (unsigned i = 0; i < desc.NumParameters; i++) {
      auto &parameter = desc.pParameters[i];
      bool free = true;
      switch (parameter.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        for (unsigned j = 0; free && j < parameter.DescriptorTable.NumDescriptorRanges; j++) {
          auto &range = parameter.DescriptorTable.pDescriptorRanges[j];
          free = !range.NumDescriptors || bind(range.RangeType, range.RegisterSpace, range.BaseShaderRegister,
                                               range.NumDescriptors, parameter.ShaderVisibility);
        }
        break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        free = bind(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, parameter.Constants.RegisterSpace,
                    parameter.Constants.ShaderRegister, 1, parameter.ShaderVisibility);
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        free = bind(parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV   ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                    : parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                                                               : D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                    parameter.Descriptor.RegisterSpace, parameter.Descriptor.ShaderRegister, 1,
                    parameter.ShaderVisibility);
        break;
      }
      if (!free)
        return FAIL(E_INVALIDARG);
    }
    for (unsigned i = 0; i < desc.NumStaticSamplers; i++)
      if (!desc.pStaticSamplers ||
          !bind(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, desc.pStaticSamplers[i].RegisterSpace,
                desc.pStaticSamplers[i].ShaderRegister, 1, desc.pStaticSamplers[i].ShaderVisibility))
        return FAIL(E_INVALIDARG);

    NumStaticSamplers = desc.NumStaticSamplers;
    if (NumStaticSamplers) {
      for (unsigned i = 0; i < desc.NumStaticSamplers; i++) {
        // a static sampler is the device's sampler of its description, as one in a heap is
        MTLD3D12CachedSampler cached;
        hr = GetSampler(device_, SamplerDescription(desc.pStaticSamplers[i]), cached);
        if (FAILED(hr))
          return hr;
        auto &sampler = cached.sampler;
        static_samplers_encoded_.push_back(sampler->sampler_state_handle);
        static_samplers_encoded_.push_back(sampler->sampler_state_cube_handle);
        static_samplers_encoded_.push_back(sampler->metadata);
        static_samplers_encoded_.push_back(0 /* padding */);

        static_samplers_.emplace_back(std::move(sampler));
      }
      EncodedStaticSamplers = static_samplers_encoded_.data();
      if (desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) {
        WMTBufferInfo info{static_samplers_encoded_.size() * sizeof(uint64_t), WMTResourceStorageModeShared};
        static_samplers_buffer_ = device_->GetMTLDevice().newBuffer(info);
        if (!static_samplers_buffer_)
          return FAIL(E_OUTOFMEMORY);
        memcpy(info.memory.get(), EncodedStaticSamplers, info.length);
        StaticSamplersAddress = info.gpu_address;
        device_->RegisterResidency(static_samplers_buffer_);
      }
    }

    auto slots = CalculateRootSignatureQwordOffsets(desc);
    qword_offsets_ = std::move(slots.qword_offsets);
    slot_types_ = std::move(slots.types);
    slot_constants_ = std::move(slots.constants);
    auto total_qwords = slots.qwords;
    /* after the parameters, in the order the shader compiler declares them (dxbc_binding_rootsig.cpp) */
    if (desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)
      ResourceHeapQword = total_qwords++;
    if (desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED)
      SamplerHeapQword = total_qwords++;
    AtomicLocksQword = total_qwords++;
    UploadQwords = total_qwords;
    StreamOutput = desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    ParameterSlots = qword_offsets_.size();
    SlotQwordOffsets = qword_offsets_.data();
    SlotTypes = slot_types_.data();
    SlotConstants = slot_constants_.data();

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12RootSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12RootSignature), riid)) {
      WARN("D3D12RootSignature: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12DeviceChild::QueryInterface(riid, ppvObject);
  }

  UINT
  GetBlob(const void **ppBlob) {
    *ppBlob = blob_.data();
    return blob_.size();
  }
};

HRESULT
CreateRootSignature(
    MTLD3D12Device *pDevice, UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid,
    void **ppRootSignature
) {
  InitReturnPtr(ppRootSignature);

  auto root_sig = Com(new MTLD3D12RootSignatureImpl(pDevice, pBytecode, BytecodeLength));
  HRESULT hr = root_sig->Initialize();
  if (FAILED(hr))
    return hr;
  return root_sig->QueryInterface(riid, ppRootSignature);
}

HRESULT
CreateLibraryRootSignature(
    MTLD3D12Device *pDevice, const void *pLibrary, SIZE_T LibraryLength, LPCWSTR Name, REFIID riid,
    void **ppRootSignature
) {
  InitReturnPtr(ppRootSignature);
  if (!Name)
    return FAIL(E_INVALIDARG);
  if (!pLibrary || !LibraryLength)
    return FAIL(E_INVALIDARG);

  auto root_sig = Com(new MTLD3D12RootSignatureImpl(pDevice, pLibrary, LibraryLength, Name));
  HRESULT hr = root_sig->Initialize();
  if (FAILED(hr))
    return hr;
  return root_sig->QueryInterface(riid, ppRootSignature);
}

} // namespace mullion
