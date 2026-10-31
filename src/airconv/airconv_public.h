#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"
#include "airconv_sparse.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#define AIRCONV_VERSION 90

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
  /* a geometry draw's first object threadgroup and instance (uint2): not zero where a draw runs in parts */
  SM50_BINDING_INDEX_DRAW_ORIGIN = 9,
  SM50_BINDING_INDEX_PIPELINE_STATISTICS = SM50_BINDING_INDEX_DRAW_ORIGIN + 1,
  /* each of the SM50_VIEWPORTS viewports' MinDepth and MaxDepth (float2), for a pixel shader that writes depth, then
     the blend factor (float4), for one that blends with it (SM50_SHADER_BLEND) */
  SM50_BINDING_INDEX_DEPTH_RANGES = SM50_BINDING_INDEX_PIPELINE_STATISTICS + 1,
  /* what the stage before the rasterizer multiplies a position's x and y by (float), the inverse of the factor
     Metal's viewports are larger by than the application's; then the width and height of each of the
     SM50_VIEWPORTS viewports, as the application gave them */
  SM50_BINDING_INDEX_RASTER_SCALE = SM50_BINDING_INDEX_DEPTH_RANGES + 1,
  /* the depth bounds test of a GPU without one, a pixel shader's (SM50_SHADER_PSO_PIXEL_SHADER_DATA's depth_bounds):
     whether the draw has it and the row pitch of the samples that pass it, two uints, then those samples, a byte for
     each pixel */
  SM50_BINDING_INDEX_DEPTH_BOUNDS = SM50_BINDING_INDEX_PIPELINE_STATISTICS + 2,
  SM50_BINDING_INDEX_DEPTH_BOUNDS_SAMPLES = SM50_BINDING_INDEX_DEPTH_BOUNDS + 1,
  /* a conservative rasterizer's mesh stage: the viewports, SM50_CONSERVATIVE_VIEWPORTS */
  SM50_BINDING_INDEX_VIEWPORTS = SM50_BINDING_INDEX_PIPELINE_STATISTICS + 1,
  /* variable rate shading, a pixel shader's (SM50_SHADER_PSO_PIXEL_SHADER_DATA's coarse_shading): the rates and
     combiners (SM50_SHADING_RATE), then the shading rate image's bytes, a D3D12_SHADING_RATE for each tile */
  SM50_BINDING_INDEX_SHADING_RATE = SM50_BINDING_INDEX_DEPTH_BOUNDS_SAMPLES + 1,
  SM50_BINDING_INDEX_SHADING_RATE_IMAGE = SM50_BINDING_INDEX_SHADING_RATE + 1,

  SM50_BINDING_INDEX_ROOT_ARGUMENTS = 0,
  SM50_BINDING_INDEX_STATIC_SAMPLERS = 1,
};

/* the viewports a primitive may choose from (D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) */
#define SM50_VIEWPORTS 16

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
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLE = 1 << 7,
  MTL_SM50_SHADER_ARGUMENT_READ_ACCESS = 1 << 10,
  MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS = 1 << 11,
};

// a texture argument's stand-in for an unbound texture: its Metal texture type (MTLTextureType) and the type it is
// read and written as, which name the texture the runtime binds in its place
#define MTL_SM50_SHADER_ARGUMENT_TEXTURE_TYPE_SHIFT 16
#define MTL_SM50_SHADER_ARGUMENT_SAMPLE_TYPE_SHIFT 20
enum SM50_SAMPLE_TYPE : uint32_t {
  SM50_SAMPLE_FLOAT,
  SM50_SAMPLE_UINT,
  SM50_SAMPLE_INT,
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
  /* whether a conservative rasterizer can grow the triangles it emits: three vertices for each, as many as its strip
     of the most vertices has, fit a mesh (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's conservative) */
  uint32_t ConservativeFits;
};

struct MTL_POST_TESSELLATOR_REFLECTION {
  /* the bytes of a vertex of the domain shader's in a mesh */
  uint32_t MeshVertexSize;
};

struct MTL_PIXEL_SHADER_REFLECTION {
  uint32_t ValidRenderTargets;
  uint32_t HasCoverageOutput;
  uint32_t SintRenderTargets;
  uint32_t UintRenderTargets;
  uint32_t HasCoverageInput;
  /* the input registers the shader takes flat (integers among them), and those it interpolates without perspective:
     what a conservative rasterizer's dilated vertices extrapolate (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA) */
  uint32_t FlatInputs;
  uint32_t NoPerspectiveInputs;
  /* the shader runs for each pixel or sample whatever the shading rate: it runs per sample, outputs depth or stencil
     reference, or evaluates its inputs where it chooses */
  uint32_t FineShading;
  /* the shader reads barycentrics (SV_Barycentrics) */
  uint32_t Barycentrics;
  /* the shader outputs depth (SV_Depth, SV_DepthGreaterEqual or SV_DepthLessEqual) */
  uint32_t DepthOutput;
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
  /* how many cull distances the stage outputs, and clip distances */
  uint32_t CullDistances;
  uint32_t ClipDistances;
  /* a compute shader whose threadgroups wait for one another (a loop reads globally coherent memory): its dispatch
     may be made in parts, each with the stage-in region's origin at the part's first group */
  uint32_t GroupsWorkTogether;
  /* a compute shader with 64-bit atomics on resources, whose emulation's locks wait for the threadgroups that hold
     them: on a GPU without native 64-bit min and max its dispatch is made in parts as GroupsWorkTogether's */
  uint32_t DeviceAtomics64;
  /* a mesh shader's triangles a Metal mesh can grow for conservative rasterization (SM50_SHADER_MESH_SHADER_DATA's
     triangles), 0 where it cannot: lines, a shader that writes resources (its group runs once a slice), or outputs too
     large; and the most primitives it outputs */
  uint32_t MeshConservativeTriangles;
  uint32_t MeshMaxPrimitives;
  /* a vertex or geometry shader's SV_ShadingRate: its varying, as 1 + register * 4 + component, or 0 */
  uint32_t ShadingRateOutput;
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
  /* conservative rasterization of the mesh shader's triangles (as SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's): a D3D
     group's mesh is made by `slices` Metal mesh threadgroups, each growing `triangles` of its triangles
     (MTL_SHADER_REFLECTION's MeshConservativeTriangles). with an amplification shader
     (`amplified`), its grid's x has `slices` threadgroups for each of the D3D grid's; without one the pipeline's object
     function launches them, and gives the group's ID as the payload (ConservativeObject) */
  bool conservative;
  bool amplified;
  uint32_t triangles;
  uint32_t slices;
  uint32_t flat_inputs;
  uint32_t noperspective_inputs;
  bool barycentrics;
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
   then per stream each object threadgroup's primitive total, each block's of 1 << SM50_STREAM_OUTPUT_BLOCK_SHIFT
   groups, then each geometry invocation's (SM50StreamOutputScratch) */
enum { SM50_STREAM_OUTPUT_BLOCK_SHIFT = 10 };
struct SM50_STREAM_OUTPUT_TARGETS {
  uint64_t address[4];
  uint64_t size[4];
  uint64_t filled[4];
  uint64_t scratch;
  uint32_t warps;     /* object threadgroups per instance */
  uint32_t instances;
  uint64_t statistics[4]; /* per stream, an active query's D3D12_QUERY_DATA_SO_STATISTICS to add to, or 0 */
};
/* the bytes of a draw's scratch area before the invocations' counts, which it starts with zeroed, for `groups` object
   threadgroups */
static inline uint64_t
SM50StreamOutputZeroed(uint64_t groups) {
  return 8 * 4 + 4 * 4 * (groups + ((groups + (1u << SM50_STREAM_OUTPUT_BLOCK_SHIFT) - 1) >> SM50_STREAM_OUTPUT_BLOCK_SHIFT));
}
/* the bytes of the whole area, for object threadgroups of at most `invocations` geometry invocations each */
static inline uint64_t
SM50StreamOutputScratch(uint64_t groups, uint32_t invocations) {
  return SM50StreamOutputZeroed(groups) + 4 * 4 * groups * invocations;
}

enum SM50_SHADER_METAL_VERSION {
  SM50_SHADER_METAL_310 = 310,
  SM50_SHADER_METAL_320 = 320,
  SM50_SHADER_METAL_400 = 400,
  SM50_SHADER_METAL_MAX = 0xffffffff,
};

/* a texel buffer's metadata qword is its first element, in the view's Metal texels, then its element count. Metal has
   no 96-bit pixel format, so an R32G32B32 view is one of R32 texels, three to an element, marked by the count's top
   bit: a view has at most 2^27 elements (D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP) */
#define SM50_TEXEL_BUFFER_METADATA_RGB32 (1u << 31)
// a texture's or texel buffer's metadata: its texture stands for an unbound one, and what it answers is dropped. the
// bit is above every array length, W slice and element count the metadata holds
#define SM50_TEXTURE_METADATA_UNBOUND (1ull << 62)

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

/* the view location qword after its format range: bit 7 of the first mip byte is spare (D3D12_REQ_MIP_LEVELS) */
enum SM50_TEXTURE_VIEW_METADATA : uint64_t {
  SM50_TEXTURE_VIEW_METADATA_FIRST_MIP_BITS = 7,
  SM50_TEXTURE_VIEW_METADATA_SPARSE_WRITE = 1ull << SM50_TEXTURE_VIEW_METADATA_FIRST_MIP_BITS,
};

/* the low word of a multisample texture's metadata: a one-sample view, which reads a regular texture since Metal's
   MS textures need more than one sample (D3D11.3 22.4.7), and the center pattern, whose samples all lie at the pixel
   center (19.2.4.1) */
enum SM50_TEXTURE_MULTISAMPLE_METADATA : uint64_t {
  SM50_TEXTURE_MULTISAMPLE_METADATA_SINGLE = 1 << 0,
  SM50_TEXTURE_MULTISAMPLE_METADATA_CENTERED = 1 << 1,
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
  /* the device has no 64-bit atomics (before Apple8): its 64-bit minimum and maximum take locks as the others do */
  SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64 = 1 << 2,
  /* culling a line of a mesh line strip culls the next one too (before Apple9): a culled line collapses instead */
  SM50_SHADER_FLAG_COLLAPSE_CULLED_LINES = 1 << 3,
  /* a group sync does not make globallycoherent memory visible to other groups (before Apple9): in shaders whose
     groups work together, each group sync takes a device-scope fence */
  SM50_SHADER_FLAG_COHERENT_GROUP_SYNC = 1 << 5,
  /* no sampler has a border color Metal lacks (SM50_SAMPLER_BORDER): samples leave out the custom border's emulation */
  SM50_SHADER_FLAG_NO_CUSTOM_BORDER = 1 << 6,
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

#define SM50_PIXEL_FORMAT_CHANNELS (1u << 16)
#define SM50_PIXEL_FORMAT_CHANNEL(format, channel) (((format) >> (8 + 2 * (channel))) & 3)

/* a render target the pixel shader blends with what it holds, where Metal's blender cannot keep D3D's results: a
   float target, through whose every weight D3D propagates NaNs and signed zeros (D3D11.3 17.5), and logic ops, whose
   results keep the channel's own bits (D3D11.3 17.4). the operations and weights are D3D12_BLEND_OP, D3D12_BLEND and
   D3D12_LOGIC_OP values, which D3D11's equal; a weight of the blend factor reads it after the depth ranges at
   SM50_BINDING_INDEX_DEPTH_RANGES */
struct SM50_SHADER_BLEND {
  uint8_t blend, logic;
  uint8_t src, dst, op, src_alpha, dst_alpha, op_alpha, logic_op;
  /* the bits of each stored channel of a logic op's target, in the target's order */
  uint8_t bits[4];
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  /** MTLPixelFormat in the low 8 bits; with SM50_PIXEL_FORMAT_CHANNELS, SM50_PIXEL_FORMAT_CHANNEL names the output
      component each stored channel takes */
  uint32_t pixel_formats[8];
  bool pipeline_statistics;
  /** o0.a becomes the coverage mask, for a render target 0 Metal takes no alpha-to-coverage with */
  bool alpha_to_coverage;
  /** the render target samples all lie at the pixel center (D3D11.3 19.2.4.1) */
  bool centered_samples;
  /** one sample rasterizes for render targets of more (ForcedSampleCount 1, D3D11.3 3.5.6.2) */
  bool single_raster_sample;
  /* how many clip distances reach the shader as varyings, to test at each sample (D3D11.3 15.4.1) */
  uint32_t clip_distances;
  struct SM50_SHADER_BLEND blend[8];
  /* the depth bounds test, which a GPU without it has the pixel shader do: at SM50_BINDING_INDEX_DEPTH_BOUNDS_SAMPLES a
     byte for each pixel holds, a bit for each sample, the samples whose depth is in the bounds, and at
     SM50_BINDING_INDEX_DEPTH_BOUNDS whether the draw tests and the bytes' row pitch; a pixel of none is discarded and
     the others keep only those samples */
  bool depth_bounds;
  /* the depth and stencil tests after the shader, whatever it declares: a discard then takes a sample out of an
     occlusion query, which the test of depth_bounds must do */
  bool late_tests;
  /* conservative rasterization: the pixel shader runs only where the pixel's square overlaps the primitive the mesh
     stage passes on in SM50_CONSERVATIVE_VARYINGS (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's conservative), takes every
     sample as covered, and divides its perspective inputs by SM50_CONSERVATIVE_RCP_W's */
  bool conservative;
  /* variable rate shading (D3D12 Variable Rate Shading, tier 2): a pixel shader of coarse pixels, two wide or high at
     most, whose rate is SM50_SHADING_RATE's at SM50_BINDING_INDEX_SHADING_RATE combined with the primitive's and the
     image's. it does nothing where MTL_PIXEL_SHADER_REFLECTION's FineShading */
  bool coarse_shading;
  /* the primitive's rate, the varying of the stage before (MTL_SHADER_REFLECTION's ShadingRateOutput), or 0 */
  uint32_t shading_rate_varying;
  /* lines reach the rasterizer as quadrilaterals (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's quad_lines), whose
     barycentrics are SM50_BARYCENTRIC_ROWS */
  bool quad_lines;
};

/* conservative rasterization's per-primitive varyings, flat floats named "mullion_conservative0" and on: the corners
   of the polygon the rasterizer takes in the window (x, y each), the triangle as clipped, the last repeated where it
   has fewer */
#define SM50_CONSERVATIVE_CORNERS 5
#define SM50_CONSERVATIVE_VARYINGS (2 * SM50_CONSERVATIVE_CORNERS)
/* then the reciprocal of the primitive's w, "mullion_conservative10" to 13, for the pixel shader's inputs at the
   center, at the centroid, at the sample and where it evaluates them. the grown triangle's points have a w of 1, so
   what Metal interpolates is linear in the window: their inputs are what is, an input over w, the points' own 1/w */
#define SM50_CONSERVATIVE_RCP_W 4
/* what a mesh stage whose triangles are not the primitive's (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's quad_lines)
   passes a pixel shader that reads barycentrics: each point's weights of the primitive's corners times its 1/w, then
   the weights, floats named "mullion_barycentrics0" to 5, which Metal interpolates linearly in the window */
#define SM50_BARYCENTRIC_ROWS 6
/* what a conservative rasterizer's mesh stage reads: the viewports by viewport array index (x, y, width, height), then
   whether the pipeline clips depth and whether its front faces are counterclockwise (1 or 0) */
struct SM50_CONSERVATIVE_VIEWPORTS {
  float viewports[16][4];
  float state[4];
};

/* what a coarse shading pixel shader binds at SM50_BINDING_INDEX_SHADING_RATE: RSSetShadingRate's rate and combiners,
   and the shading rate image's size in tiles, 0 without one; its bytes are at SM50_BINDING_INDEX_SHADING_RATE_IMAGE,
   a row of image_width after another */
struct SM50_SHADING_RATE {
  uint32_t base;
  uint32_t combiners[2];
  uint32_t image_width;
  uint32_t image_height;
};

struct SM50_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  /** MTLAttributeFormat */
  uint32_t format;
  uint32_t step_function;
  uint32_t step_rate;
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
  /* the clip distances leave as varyings, which the pixel shader tests (SM50_SHADER_PSO_PIXEL_SHADER_DATA) */
  bool ClipDistanceVaryings;
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

/* the 64-bit counts a draw's shaders add to at SM50_BINDING_INDEX_PIPELINE_STATISTICS, in its words' order: a pixel
   shader's invocations; a geometry shader's invocations, its primitives, those it rasterizes, and the input primitives
   it takes, which the rasterizer counted in their place */
enum SM50_PIPELINE_STATISTIC : uint32_t {
  SM50_STATISTIC_PIXELS = 0,
  SM50_STATISTIC_GS_INVOCATIONS = 3,
  SM50_STATISTIC_GS_PRIMITIVES,
  SM50_STATISTIC_RASTERIZED,
  SM50_STATISTIC_GS_INPUTS,
  SM50_STATISTIC_COUNT,
};

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
  /* the geometry shader adds to the draw's statistics (SM50_PIPELINE_STATISTIC) */
  bool pipeline_statistics;
  /* as SM50_SHADER_GS_PASS_THROUGH_DATA's */
  bool clip_distance_varyings;
  /* the lines passed on are quadrilaterals (D3D11.3 3.4.5): two counterclockwise triangles, which the pipeline
     draws with no culling, no depth bias, solid fill and counterclockwise front faces, as lines are drawn */
  bool quad_lines;
  /* the pixel shader reads barycentrics, which quadrilaterals and conservative triangles pass on as
     SM50_BARYCENTRIC_ROWS */
  bool barycentrics;
  /* passing triangles through, conservative rasterization: each is one Metal triangle around it, its attributes
     extrapolated as the pixel shader interpolates them (MTL_PIXEL_SHADER_REFLECTION's FlatInputs,
     NoPerspectiveInputs), and it gives the pixel shader the polygon to test pixels against. the viewports are at
     SM50_BINDING_INDEX_VIEWPORTS */
  bool conservative;
  uint32_t flat_inputs;
  uint32_t noperspective_inputs;
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
  /* conservative rasterization of the triangles the tessellator outputs, as SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's:
     every mesh threadgroup makes a piece of its workload's (tessellation_pieces) */
  bool conservative;
  uint32_t flat_inputs;
  uint32_t noperspective_inputs;
  bool barycentrics;
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
enum SM50_COMPILE_RESULT { SM50_COMPILE_SUCCESS, SM50_COMPILE_FAILED, SM50_COMPILE_UNRESOLVED };
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
