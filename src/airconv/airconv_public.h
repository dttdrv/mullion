#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#define AIRCONV_VERSION 31

#ifdef __cplusplus
#include <string>

enum class SM50BindingType : uint32_t {
  ConstantBuffer,
  Sampler,
  SRV,
  UAV,
};
#else
typedef uint32_t SM50BindingType;
#endif

enum SM50_BINDING_INDEX: uint32_t {
  SM50_BINDING_INDEX_CONSTANT_BUFFER = 0,
  SM50_BINDING_INDEX_ARGUMENT_TABLE = 1,
  SM50_BINDING_INDEX_VERTEX_BUFFER = 2,
  SM50_BINDING_INDEX_INDEX_BUFFER = 3,
  SM50_BINDING_INDEX_DRAW_ARGUMENTS = 4,
  SM50_BINDING_INDEX_INDIRECT_ARGUMENTS = 4,
  SM50_BINDING_INDEX_STREAM_OUTPUT0 = 5,
  /* For multiple stages in the same shader function */
  SM50_BINDING_INDEX_CONSTANT_BUFFER2 = 6,
  SM50_BINDING_INDEX_ARGUMENT_TABLE2 = 7,
  /* the draw's control points per patch, for a hull shader that declares none (SM50_INDEX_BUFFER_FORMAT_VIEW only) */
  SM50_BINDING_INDEX_PATCH_SIZE = 8,

  SM50_BINDING_INDEX_ROOT_ARGUMENTS = 0,
  SM50_BINDING_INDEX_STATIC_SAMPLERS = 1,
};

/* the bytes before the lock table of 64-bit atomics, which every root signature's arguments name, are the header of
 * an acceleration structure of nothing: what a null structure is to a ray query */
#define SM50_NULL_ACCELERATION_STRUCTURE_HEADER_SIZE 16

enum MTL_SM50_SHADER_ARGUMENT_FLAG : uint32_t {
  MTL_SM50_SHADER_ARGUMENT_BUFFER = 1 << 0,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE = 1 << 1,
  MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH = 1 << 2,
  MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER = 1 << 3,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP = 1 << 4,
  MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET = 1 << 5,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY = 1 << 6,
  MTL_SM50_SHADER_ARGUMENT_READ_ACCESS = 1 << 10,
  MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS = 1 << 11,
};

struct MTL_SM50_SHADER_ARGUMENT {
  SM50BindingType Type;
  /**
  bind point of it's corresponding resource space
  constant buffer:    cb1 -> 1
  srv:                t10 -> 10
  uav:                u0  -> 0
  sampler:            s2  -> 2
  */
  uint32_t SM50BindingSlot;
  enum MTL_SM50_SHADER_ARGUMENT_FLAG Flags;
  uint32_t StructurePtrOffset;
};

enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE {
  MTL_TESSELLATOR_OUTPUT_POINT = 1,
  MTL_TESSELLATOR_OUTPUT_LINE = 2,
  MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW = 3,
  MTL_TESSELLATOR_TRIANGLE_CCW = 4
};

struct MTL_TESSELLATOR_REFLECTION {
  uint32_t Partition;
  float MaxFactor;
  enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE OutputPrimitive;
  uint32_t Domain; /* D3D11_SB_TESSELLATOR_DOMAIN */
};

struct MTL_GEOMETRY_SHADER_PASS_THROUGH {
  uint8_t RenderTargetArrayIndexReg;
  uint8_t RenderTargetArrayIndexComponent;
  uint8_t ViewportArrayIndexReg;
  uint8_t ViewportArrayIndexComponent;
};

struct MTL_GEOMETRY_SHADER_REFLECTION {
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t GSPassThrough;
  };
  uint32_t Primitive;
  uint32_t InstanceCount;
};

struct MTL_POST_TESSELLATOR_REFLECTION {
  /* the bytes of a vertex of the domain shader's in a mesh */
  uint32_t MeshVertexSize;
};

struct MTL_PIXEL_SHADER_REFLECTION {
  uint32_t ValidRenderTargets;
  uint32_t HasCoverageOutput;
};

struct MTL_SHADER_REFLECTION {
  uint32_t ConstanttBufferTableBindIndex;
  uint32_t ArgumentBufferBindIndex;
  uint32_t NumConstantBuffers;
  uint32_t NumArguments;
  union {
    uint32_t ThreadgroupSize[3];
    struct MTL_TESSELLATOR_REFLECTION Tessellator;
    struct MTL_GEOMETRY_SHADER_REFLECTION GeometryShader;
    struct MTL_POST_TESSELLATOR_REFLECTION PostTessellator;
    /**
    \deprecated for compatibility, use PixelShader.ValidRenderTargets instead
     */
    uint32_t PSValidRenderTargets;
    struct MTL_PIXEL_SHADER_REFLECTION PixelShader;
  };
  uint16_t ConstantBufferSlotMask;
  uint16_t SamplerSlotMask;
  uint64_t UAVSlotMask;
  uint64_t SRVSlotMaskHi;
  uint64_t SRVSlotMaskLo;
  uint32_t NumOutputElement;
  uint32_t ThreadsPerPatch;
  uint32_t ArgumentTableQwords;
  /* how many cull distances the stage outputs */
  uint32_t CullDistances;
  /* a compute shader whose threadgroups wait for one another (a loop reads globally coherent memory): its dispatch
     may be made in parts, each with the stage-in region's origin at the part's first group */
  uint32_t GroupsWorkTogether;
};

#if defined(__LP64__) || defined(_WIN64)
typedef void *sm50_ptr64_t;
#else
typedef struct sm50_ptr64_t {
  uint64_t impl;

#ifdef __cplusplus
  sm50_ptr64_t() {
    impl = 0;
  }

  sm50_ptr64_t(void * ptr) {
    impl = (uint64_t)ptr;
  }

  sm50_ptr64_t(uint64_t v) {
    impl = v;
  }

  operator uint64_t () const {
    return impl;
  }
#endif

} sm50_ptr64_t;
#endif

typedef sm50_ptr64_t sm50_shader_t;
typedef sm50_ptr64_t sm50_bitcode_t;
typedef sm50_ptr64_t sm50_error_t;

#ifdef _WIN32
#ifdef WIN_EXPORT
#define AIRCONV_API __declspec(dllexport)
#else
#define AIRCONV_API __declspec(dllimport)
#endif
#else
#define AIRCONV_API __attribute__((sysv_abi))
#endif

struct SM50_COMPILED_BITCODE {
  sm50_ptr64_t Data;
  uint64_t Size;
};

#ifdef __cplusplus

inline uint32_t
GetArgumentIndex(SM50BindingType Type, uint32_t SM50BindingSlot) {
  switch (Type) {
  // a constant buffer's pointer, then its size
  case SM50BindingType::ConstantBuffer:
    return SM50BindingSlot * 2;
  case SM50BindingType::Sampler:
    return SM50BindingSlot + 32;
  case SM50BindingType::SRV:
    return SM50BindingSlot * 3 + 128;
  case SM50BindingType::UAV:
    return SM50BindingSlot * 3 + 512;
  }
};

inline uint32_t GetArgumentIndex(struct MTL_SM50_SHADER_ARGUMENT &Argument) {
  return GetArgumentIndex(Argument.Type, Argument.SM50BindingSlot);
};

extern "C" {
#endif

enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE {
  SM50_SHADER_COMMON = 2,
  SM50_SHADER_PSO_PIXEL_SHADER = 3,
  SM50_SHADER_IA_INPUT_LAYOUT = 4,
  SM50_SHADER_GS_PASS_THROUGH = 5,
  SM50_SHADER_PSO_GEOMETRY_SHADER = 6,
  SM50_SHADER_PSO_TESSELLATOR = 7,
  SM50_SHADER_ROOT_SIGNATURE = 8,
  SM50_SHADER_ROOT_SIGNATURE2 = 9,
  SM50_SHADER_STREAM_OUTPUT = 10,
  SM50_SHADER_MESH_SHADER = 11,
  SM50_SHADER_RAY_SHADER = 12,
  SM50_SHADER_LIBRARIES = 13,
  SM50_SHADER_ARGUMENT_TYPE_MAX = 0xffffffff,
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_STREAM_OUTPUT_ELEMENT2 {
  uint32_t reg_id;
  uint32_t component;
  uint32_t stream;
  uint32_t output_slot;
  uint32_t offset;
};

/* a mesh shader names its outputs as the pixel shader's inputs of the same semantics (null without one) */
struct SM50_SHADER_MESH_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const void *pixel_shader_bytecode;
};

/* a DXIL library compiles one function at a time: its ray tracing shader `name`, as a visible function of a ray
   tracing pipeline (airconv_ray.h). SM50_SHADER_ROOT_SIGNATURE is the shader's global root signature, and
   SM50_SHADER_ROOT_SIGNATURE2 its local one, whose arguments are in its shader record */
struct SM50_SHADER_RAY_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const char *name;
};

/* a ray tracing shader of a DXIL library. `kind` is DXIL's shader kind */
enum SM50_RAY_SHADER_KIND {
  SM50_RAY_SHADER_RAY_GENERATION = 7,
  SM50_RAY_SHADER_INTERSECTION = 8,
  SM50_RAY_SHADER_ANY_HIT = 9,
  SM50_RAY_SHADER_CLOSEST_HIT = 10,
  SM50_RAY_SHADER_MISS = 11,
  SM50_RAY_SHADER_CALLABLE = 12,
};

struct SM50_RAY_SHADER_INFO {
  uint32_t kind;
  uint32_t payload_size;   /* of its payload, or a callable shader's parameter */
  uint32_t attribute_size; /* of a hit's attributes */
};

struct SM50_SHADER_STREAM_OUTPUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_elements;
  const struct SM50_STREAM_OUTPUT_ELEMENT2 *elements;
  uint32_t strides[4];
  uint32_t rasterized_stream; /* ~0u for none */
  bool counting;
};

/* what a stream output draw binds at SM50_BINDING_INDEX_STREAM_OUTPUT0: each slot's buffer, size and BufferFilledSize
   address (0 when unbound), and a scratch area zeroed for the draw: the filled sizes as the draw starts (4 qwords),
   then per stream each object threadgroup's primitive total, then each geometry invocation's */
struct SM50_STREAM_OUTPUT_TARGETS {
  uint64_t address[4];
  uint64_t size[4];
  uint64_t filled[4];
  uint64_t scratch;
  uint32_t warps;     /* object threadgroups per instance */
  uint32_t instances;
  uint64_t statistics[4]; /* per stream, an active query's D3D12_QUERY_DATA_SO_STATISTICS to add to, or 0 */
};

enum SM50_SHADER_METAL_VERSION {
  SM50_SHADER_METAL_310 = 310,
  SM50_SHADER_METAL_320 = 320,
  SM50_SHADER_METAL_MAX = 0xffffffff,
};

/* the sampler metadata qword that shaders read next to the sampler states: MipLODBias as a signed 16-bit LOD in
   bits 0-15, then whether the minification, magnification and mip filters are linear and MaxAnisotropy (1 unless the
   filter is anisotropic), then MinLOD and (the largest LOD field value - MaxLOD) as unsigned LOD fields, then the
   comparison function (WMTCompareFunction), then the address modes of u and v as
   u + SM50_SAMPLER_ADDRESS_COUNT * v. LODs have D3D's 8 fractional bits (D3D11.3 7.18.16) */
enum SM50_SAMPLER_METADATA {
  SM50_SAMPLER_METADATA_LOD_FRACTION_BITS = 8,
  SM50_SAMPLER_METADATA_LOD_BITS = 12, /* up to LOD 15.996, past the last mip of a 16384 texel texture */
  SM50_SAMPLER_METADATA_MIN_LINEAR = 16,
  SM50_SAMPLER_METADATA_MAG_LINEAR = 17,
  SM50_SAMPLER_METADATA_MIP_LINEAR = 18,
  SM50_SAMPLER_METADATA_ANISOTROPY = 19,
  SM50_SAMPLER_METADATA_ANISOTROPY_BITS = 5,
  SM50_SAMPLER_METADATA_MIN_LOD = 32,
  SM50_SAMPLER_METADATA_MAX_LOD = 44,
  SM50_SAMPLER_METADATA_COMPARE = 56,
  SM50_SAMPLER_METADATA_ADDRESS = 59,
};

/* WMTSamplerAddressMode, with one border */
enum SM50_SAMPLER_ADDRESS {
  SM50_SAMPLER_ADDRESS_CLAMP,
  SM50_SAMPLER_ADDRESS_MIRROR_ONCE,
  SM50_SAMPLER_ADDRESS_REPEAT,
  SM50_SAMPLER_ADDRESS_MIRROR,
  SM50_SAMPLER_ADDRESS_BORDER,
  SM50_SAMPLER_ADDRESS_COUNT,
};

/* a sampler descriptor's fourth qword: the address of its SM50_SAMPLER_BORDER, or 0 for Metal's own border colors. the
   sampler state borders in transparent black and its twin in opaque white, so per channel their difference is the
   border's weight in the filter. a texture read with it carries its format's range (D3D11.3 7.18.9.1) as two floats,
   low and high, in its descriptor's third qword */
struct SM50_SAMPLER_BORDER {
  uint64_t white_sampler;
  uint64_t padding;
  float color[4];
};

enum SM50_SHADER_FLAG {
  SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO = 1 << 0,
  SM50_SHADER_FLAG_DEFUSE_FMA = 1 << 1,
};

struct SM50_SHADER_COMMON_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flags;
  /* the device's SIMD-group width (a compute pipeline's thread execution width), for vertex shaders' waves; 0 when
     unknown */
  uint32_t simd_width;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  /** MTLPixelFormat */
  uint32_t pixel_formats[8];
};

struct SM50_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  /** MTLAttributeFormat */
  uint32_t format;
  uint32_t step_function: 1;
  uint32_t step_rate: 31;
};

enum SM50_INDEX_BUFFER_FORMAT {
  SM50_INDEX_BUFFER_FORMAT_NONE = 0,
  SM50_INDEX_BUFFER_FORMAT_UINT16 = 1,
  SM50_INDEX_BUFFER_FORMAT_UINT32 = 2,
  /* the index buffer binding holds the draw's D3D12_INDEX_BUFFER_VIEW, whose format (DXGI_FORMAT_R16_UINT, R32_UINT,
     or UNKNOWN for a draw without indices) is read per draw, as are D3D12_DRAW_ARGUMENTS or
     D3D12_DRAW_INDEXED_ARGUMENTS to match */
  SM50_INDEX_BUFFER_FORMAT_VIEW = 3,
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  struct SM50_IA_INPUT_ELEMENT *elements;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

/* a geometry pipeline's object threadgroup takes a warp of the draw's vertices: as many whole input primitives as
   its threads, no more than a SIMD group's, and its payload hold. the payload has four words, then the vertices'
   output registers, and nine words at its end (dxbc_converter_gs.cpp) */
enum {
  SM50_GEOMETRY_PAYLOAD_SIZE = 16256,
  SM50_GEOMETRY_WARP_THREADS = 32,
};
struct SM50_GEOMETRY_WARP {
  uint32_t threads;    /* of an object threadgroup: the vertices it reads */
  uint32_t vertices;   /* how far on the next threadgroup starts */
  uint32_t primitives; /* whole primitives, each a mesh threadgroup; none when not one fits */
};
/* for input primitives of `per_primitive` vertices, from a vertex shader with `registers` output registers */
static inline struct SM50_GEOMETRY_WARP
SM50GeometryWarp(uint32_t per_primitive, bool strip, uint32_t registers) {
  uint32_t room = (SM50_GEOMETRY_PAYLOAD_SIZE / 4 - 4 - 9) / (4 * (registers ? registers : 1));
  uint32_t threads = room < SM50_GEOMETRY_WARP_THREADS ? room : SM50_GEOMETRY_WARP_THREADS;
  struct SM50_GEOMETRY_WARP warp = {0, 0, 0};
  if (threads < per_primitive)
    return warp;
  /* a strip's primitives share vertices, each starting one after the last. those of a triangle strip with adjacency
     start two after, and the threads read them primitive by primitive, as they do a list's */
  if (strip && per_primitive != 6) {
    warp.primitives = warp.vertices = threads - (per_primitive - 1);
    warp.threads = threads;
  } else {
    warp.primitives = threads / per_primitive;
    warp.threads = warp.primitives * per_primitive;
    warp.vertices = strip ? warp.primitives * 2 : warp.threads;
  }
  return warp;
}

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
  /* whether an index cuts a strip, and which, with an index buffer view only (D3D12's IBStripCutValue; D3D11 always
     cuts at its format's all-ones) */
  bool strip_cut;
  uint32_t strip_cut_index;
  /* without a geometry shader: the input primitive (D3D10_SB_PRIMITIVE), with its adjacency, and whether the mesh
     stage passes the vertex shader's primitives on to the rasterizer, less their adjacent vertices and the
     primitives their cull distances discard. otherwise the pipeline only streams output */
  uint32_t input_primitive;
  bool pass_through;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
  /* the domain shader's MeshVertexSize: a workload whose vertices are too large for a mesh is made by several mesh
     threadgroups */
  uint32_t mesh_vertex_size;
  /* a geometry shader after the domain shader, or 0: the mesh stage then runs both, in a mesh threadgroup for each
     primitive a workload may have and each geometry instance. an object threadgroup starts at most
     `max_mesh_threadgroups` of them, which lowers the factor (MTLRenderPipelineState's
     maxTotalThreadgroupsPerMeshGrid); 0 while not known, when the pipeline is built to tell it */
  uint32_t max_mesh_threadgroups;
  sm50_shader_t geometry;
};

/* the libraries whose functions a library's shader may call: those of its state object */
struct SM50_SHADER_LIBRARIES_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const sm50_shader_t *libraries;
  uint32_t count;
};

struct SM50_SHADER_ROOT_SIGNATURE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const void* bytecode;
  size_t bytecode_length;
  /* of a ray tracing shader's local root signature: the GPU address of its static samplers, each four qwords as in
   * the buffer at SM50_BINDING_INDEX_STATIC_SAMPLERS */
  uint64_t static_samplers;
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
);
AIRCONV_API void SM50Destroy(sm50_shader_t pShader);
AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode);
AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize);
AIRCONV_API void SM50FreeError(sm50_error_t pError);

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
);

/* the ray tracing shader `Index` of a DXIL library and its function's name, as much as fits with its terminator;
   0 past the last one */
AIRCONV_API int SM50GetRayShader(
  sm50_shader_t pShader, uint32_t Index, char *pName, size_t NameSize, struct SM50_RAY_SHADER_INFO *pInfo
);

#ifdef __cplusplus
};

inline std::string SM50GetErrorMessageString(sm50_error_t pError) {
  std::string str;
  str.resize(256);
  auto size = SM50GetErrorMessage(pError, str.data(), str.size());
  str.resize(size);
  return str;
};

#endif

#endif
