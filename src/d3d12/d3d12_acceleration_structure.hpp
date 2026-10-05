#pragma once

#include "d3d12_device.hpp"

namespace dxmt {

// what the memory of a Direct3D 12 acceleration structure starts with. the GPU finds a structure by its address
// through it: a top-level build for its instances' structures, a shader for the one it traces
struct AccelerationStructureHeader {
  uint64_t structure; // the Metal structure's resource ID
  uint64_t instances; // of a top-level structure: the GPU address of its instances as Metal built them
};

static_assert(sizeof(AccelerationStructureHeader) == SM50_NULL_ACCELERATION_STRUCTURE_HEADER_SIZE);

// the size Metal's compaction would give a structure, written by the GPU once asked for
// (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE)
struct CompactedSize {
  obj_handle_t buffer = 0;
  uint64_t offset = 0;
  uint64_t *value = nullptr; // 0 until the GPU has written it
};

// what a structure was built from, as it was when the build ran: Metal has no way to read a structure, so the copies
// that give one back (serialize, visualization) give back these. a structure's clones share them
struct AccelerationStructureInputs : std::enable_shared_from_this<AccelerationStructureInputs> {
  MTLD3D12Device *device;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags;
  // a bottom-level structure's geometries; an address is an offset into `data`, or 0 as it was
  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
  uint32_t instance_count = 0;
  // the geometries' transforms, indices, vertices and boxes, or a top-level structure's instances: the addresses of
  // their structures, then the instances themselves (D3D12_RAYTRACING_INSTANCE_DESC)
  WMT::Reference<WMT::Buffer> data;
  uint64_t data_address = 0, size = 0;

  AccelerationStructureInputs(
      MTLD3D12Device *device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags
  ) : device(device), type(type), flags(flags) {}
  bool
  top() const {
    return type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  }
  uint32_t
  count() const {
    return top() ? instance_count : geometries.size();
  }
  ~AccelerationStructureInputs();
};

// a serialized structure is its header, a top-level structure's pointers, this, then what it counts: instances, or
// geometries and, where the alignment of structures' inputs next comes (`data`), their data
struct SerializedInputs {
  uint32_t type, flags, count, reserved;
  uint64_t size;
};

// the alignment of every part of a structure's inputs, which is that of its transforms, the strictest
constexpr uint64_t kAccelerationStructureInputAlignment = D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT;
// what Metal wants a geometry's elements' offset in their buffer to be a multiple of (its validation of an
// acceleration structure's descriptor: "Vertex buffer offset must be a multiple of 4 bytes")
constexpr uint64_t kAccelerationStructureElementAlignment = 4;

// where a serialized structure with this many pointers has its inputs' count
constexpr uint64_t
SerializedInputsOffset(uint64_t pointers) {
  return sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER) + pointers * sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
}

// where a serialized structure's parts lie
struct SerializedLayout {
  uint64_t pointers, inputs, described, data, size;

  SerializedLayout(bool top, uint64_t count, uint64_t data_size) {
    pointers = SerializedInputsOffset(0);
    inputs = SerializedInputsOffset(top ? count : 0);
    described = inputs + sizeof(SerializedInputs);
    data = top ? described + count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)
               : align(described + count * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC), kAccelerationStructureInputAlignment);
    size = data + (top ? 0 : data_size);
  }
};

// where a structure decoded for tools has its parts: a header, the geometries or instances, the geometries' data
struct VisualizationLayout {
  uint64_t described = sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER), data, size;

  VisualizationLayout(bool top, uint64_t count, uint64_t data_size) {
    data = top ? described + count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)
               : align(described + count * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC), kAccelerationStructureInputAlignment);
    size = data + (top ? 0 : data_size);
  }
};

// what serialized structures of this layout say they are from (DXR, "CheckDriverMatchingIdentifier"): inputs are
// rebuilt, so any device takes them
constexpr D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER kSerializedIdentifier = {
    {0x9d3c1f52, 0x6a0e, 0x4b7d, {0x8e, 0x21, 0x4d, 0x55, 0x4c, 0x4c, 0x49, 0x4f}}, {1}
};

// the addresses a geometry has, in the order their data is kept
inline std::array<D3D12_GPU_VIRTUAL_ADDRESS *, 3>
AccelerationStructureGeometryAddresses(D3D12_RAYTRACING_GEOMETRY_DESC &geometry) {
  if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
    return {&geometry.AABBs.AABBs.StartAddress};
  auto &triangles = geometry.Triangles;
  return {&triangles.Transform3x4, &triangles.IndexBuffer, &triangles.VertexBuffer.StartAddress};
}

// the stride of elements in memory: the value's low 32 bits (DXR, "D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE")
inline uint32_t
AccelerationStructureStride(const D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE &elements) {
  return (uint32_t)elements.StrideInBytes;
}

// the bytes of a vertex's position: its format's, less the alpha of a format with four like components, which a
// build ignores so that other data can lie there ("D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC", VertexFormat)
inline uint32_t
AccelerationStructureVertexSize(MTLD3D12Device *device, DXGI_FORMAT Format) {
  MTL_DXGI_FORMAT_DESC vertex{};
  MTLQueryDXGIFormat(device->GetMTLDevice(), Format, vertex);
  switch (Format) {
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return vertex.BytesPerTexel / 4 * 3;
  default:
    return vertex.BytesPerTexel;
  }
}

// a geometry's elements a stride apart, vertices or boxes: how many, how long each is, and their stride
struct AccelerationStructureElements {
  uint64_t count;
  uint32_t size, stride;
};

inline AccelerationStructureElements
AccelerationStructureGeometryElements(MTLD3D12Device *device, const D3D12_RAYTRACING_GEOMETRY_DESC &geometry) {
  if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
    return {geometry.AABBs.AABBCount, sizeof(D3D12_RAYTRACING_AABB), AccelerationStructureStride(geometry.AABBs.AABBs)};
  auto &triangles = geometry.Triangles;
  return {triangles.VertexCount, AccelerationStructureVertexSize(device, triangles.VertexFormat),
          AccelerationStructureStride(triangles.VertexBuffer)};
}

// how much of memory a build reads at each of a geometry's addresses
inline std::array<uint64_t, 3>
AccelerationStructureGeometryLengths(MTLD3D12Device *device, const D3D12_RAYTRACING_GEOMETRY_DESC &geometry) {
  // of elements a stride apart, the last is only as long as itself
  auto elements = AccelerationStructureGeometryElements(device, geometry);
  uint64_t spread = elements.count ? (elements.count - 1) * elements.stride + elements.size : 0;
  if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
    return {spread};
  auto &triangles = geometry.Triangles;
  MTL_DXGI_FORMAT_DESC index{};
  MTLQueryDXGIFormat(device->GetMTLDevice(), triangles.IndexFormat, index);
  return {sizeof(FLOAT[3][4]), triangles.IndexCount * index.BytesPerTexel, spread};
}

// the build that gives back the structure serialized at `Address`: `bytes` is its memory up to the end of what
// `described` counts. geometry is read where it lies; a top-level structure's instances, each with the structure
// its pointer names now, are in `instances` for the caller to place
inline void
DeserializedInputs(
    const SerializedInputs &described, const char *bytes, D3D12_GPU_VIRTUAL_ADDRESS Address,
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> &geometries, std::vector<D3D12_RAYTRACING_INSTANCE_DESC> &instances
) {
  inputs = {(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE)described.type,
            (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)described.flags, described.count,
            D3D12_ELEMENTS_LAYOUT_ARRAY};
  bool top = inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  SerializedLayout layout(top, described.count, described.size);
  if (top) {
    instances.resize(described.count);
    memcpy(instances.data(), bytes + layout.described, described.count * sizeof(instances[0]));
    for (uint32_t i = 0; i < described.count; i++)
      memcpy(&instances[i].AccelerationStructure, bytes + layout.pointers + i * sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
             sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
    return;
  }
  geometries.resize(described.count);
  memcpy(geometries.data(), bytes + layout.described, described.count * sizeof(geometries[0]));
  for (auto &geometry : geometries)
    for (auto address : AccelerationStructureGeometryAddresses(geometry))
      if (address && *address)
        *address += Address + layout.data;
  inputs.pGeometryDescs = geometries.data();
}

// the Metal side of a Direct3D 12 acceleration structure, which the device keeps by GPU address
struct AccelerationStructure {
  MTLD3D12Device *device;
  WMT::Reference<WMT::AccelerationStructure> structure;
  // a top-level structure's instances, as MTLIndirectAccelerationStructureInstanceDescriptor
  WMT::Reference<WMT::Buffer> instances;
  AccelerationStructureHeader header{};
  uint64_t size = 0;
  uint32_t instance_count = 0;
  CompactedSize compacted_size;
  // what the queue has run of it so far: whether a build or a copy has filled it, and of what inputs. lists are
  // recorded in any order, so only the queue knows
  bool built = false;
  std::shared_ptr<const AccelerationStructureInputs> inputs;

  AccelerationStructure(MTLD3D12Device *device, uint64_t size, uint32_t instance_count);
  ~AccelerationStructure();
};

// the size of a structure or of scratch memory as Direct3D 12 has it: Metal's, at the alignment of structures, and
// never less than a header
constexpr uint64_t
AccelerationStructureSize(uint64_t size) {
  return align(
      std::max(size, (uint64_t)sizeof(AccelerationStructureHeader)),
      (uint64_t)D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT
  );
}

// MTLIndirectAccelerationStructureInstanceDescriptor: a row-major 3x4 matrix, options, mask, intersection function
// table offset, user ID and the structure's resource ID
constexpr uint64_t kAccelerationStructureInstanceSize = 72;

// the vertex formats ray tracing takes (DXR spec, D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC): a position is the first
// three components, or two with z = 0
inline WMTAttributeFormat
AccelerationStructureVertexFormat(DXGI_FORMAT Format) {
  switch (Format) {
  case DXGI_FORMAT_R32G32_FLOAT:
    return WMTAttributeFormatFloat2;
  case DXGI_FORMAT_R32G32B32_FLOAT:
    return WMTAttributeFormatFloat3;
  case DXGI_FORMAT_R16G16_FLOAT:
    return WMTAttributeFormatHalf2;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTAttributeFormatHalf3;
  case DXGI_FORMAT_R16G16_SNORM:
    return WMTAttributeFormatShort2Normalized;
  case DXGI_FORMAT_R16G16B16A16_SNORM:
    return WMTAttributeFormatShort3Normalized;
  case DXGI_FORMAT_R16G16_UNORM:
    return WMTAttributeFormatUShort2Normalized;
  case DXGI_FORMAT_R16G16B16A16_UNORM:
    return WMTAttributeFormatUShort3Normalized;
  case DXGI_FORMAT_R8G8_SNORM:
    return WMTAttributeFormatChar2Normalized;
  case DXGI_FORMAT_R8G8B8A8_SNORM:
    return WMTAttributeFormatChar3Normalized;
  case DXGI_FORMAT_R8G8_UNORM:
    return WMTAttributeFormatUChar2Normalized;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTAttributeFormatUChar3Normalized;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTAttributeFormatUInt1010102Normalized;
  default:
    return WMTAttributeFormatInvalid;
  }
}

// Metal's description of a build's inputs. `geometries` holds a bottom-level structure's NumDescs geometries; a
// top-level structure's instances come from `instances`, converted, and their count from a uint32 in `count`. a
// prebuild query has no buffers to give: its addresses may be placeholders, and sizes do not depend on them
inline WMTAccelerationStructureInfo
AccelerationStructureInfo(
    MTLD3D12Device *device, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
    WMTAccelerationStructureGeometry *geometries, bool buffers, obj_handle_t instances = 0, obj_handle_t count = 0,
    uint64_t count_offset = 0
) {
  WMTAccelerationStructureInfo info{};
  // fast traces are what Metal builds for unless told otherwise
  if (inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
    info.usage |= WMTAccelerationStructureUsageRefit;
  if (inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
    info.usage |= WMTAccelerationStructureUsagePreferFastBuild;
  if (inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
    info.instances = true;
    info.max_instance_count = inputs.NumDescs;
    info.instance_buffer = instances;
    info.instance_count_buffer = count;
    info.instance_count_offset = count_offset;
    return info;
  }
  auto at = [&](D3D12_GPU_VIRTUAL_ADDRESS address, obj_handle_t &buffer, uint64_t &offset) {
    auto allocation = buffers && address ? device->LookupBufferByVA(address, &offset) : nullptr;
    buffer = allocation ? allocation->buffer().handle : 0;
  };
  info.geometries.set(geometries);
  info.geometry_count = inputs.NumDescs;
  for (UINT i = 0; i < inputs.NumDescs; i++) {
    auto &desc = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? inputs.pGeometryDescs[i] : *inputs.ppGeometryDescs[i];
    auto &geometry = geometries[i] = {};
    geometry.opaque = desc.Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.no_duplicate_any_hit = desc.Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
    // a stride of zero, every element at one address, is not Metal's to say: the build gives it the element repeated
    auto elements = AccelerationStructureGeometryElements(device, desc);
    geometry.stride = elements.stride ? elements.stride : elements.size;
    if (desc.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
      at(desc.AABBs.AABBs.StartAddress, geometry.buffer, geometry.offset);
      geometry.count = desc.AABBs.AABBCount;
      continue;
    }
    auto &triangles = desc.Triangles;
    at(triangles.VertexBuffer.StartAddress, geometry.buffer, geometry.offset);
    geometry.vertex_format = AccelerationStructureVertexFormat(triangles.VertexFormat);
    geometry.count = triangles.VertexCount / 3;
    if (triangles.IndexFormat != DXGI_FORMAT_UNKNOWN) {
      at(triangles.IndexBuffer, geometry.index_buffer, geometry.index_offset);
      geometry.index_type = triangles.IndexFormat == DXGI_FORMAT_R16_UINT ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;
      geometry.count = triangles.IndexCount / 3;
    }
    at(triangles.Transform3x4, geometry.transform_buffer, geometry.transform_offset);
  }
  return info;
}

} // namespace dxmt
