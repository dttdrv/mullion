#include "d3d9_fixed_function.hpp"
#include "d3d9_clip_blit_shaders.hpp"
#include "d3d9_dxbc.hpp"
#include "d3d9_fixed_function_shaders.hpp"
#include "d3d9_ff_eight_stage_shaders.hpp"
#include "d3d9_ff_extended_shaders.hpp"
#include "d3d9_ff_ext_vs_shaders.hpp"
#include "d3d9_lights.hpp"
#include "d3d9_lighting_shaders.hpp"
#include "d3d9_transform_shaders.hpp"
#include "d3d9_transform_user_shader.hpp"
#include "d3d9_transform3d_shaders.hpp"

namespace mullion::d3d9 {

namespace {

// the vertex shader's input: the position and the vertex colour, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// a vertex the device transforms reads its position and colour, and the textured one the stage's coordinates: the
// transform shaders take three floats of position and two of texture coordinate, which is what D3DFVF_XYZ and
// D3DFVF_TEX1's default of two coordinates are
const D3D11_INPUT_ELEMENT_DESC kTransformedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kTransformedTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// a one-component D3DTSS_TEXCOORDINDEX declaration (D3DFVF_TEXCOORDSIZE1) is still a device-transformed XYZ vertex;
// its v is D3D9's fill 1, which the gather supplies, and its w is 0, so the U1 shader writes (u, 1, 0, 0)
const D3D11_INPUT_ELEMENT_DESC kTransformedU1TexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// a volume stage's coordinate has a third component that selects the slice; the transform's volume shader carries it
// through, and these are its layouts with and without D3DFVF_DIFFUSE between the position and the coordinate
const D3D11_INPUT_ELEMENT_DESC kTransformed3dTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kTransformed3dDiffuseTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the same with the stage's texture coordinates, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// a vertex the device transforms and lights: the normal comes straight after the position, which is the order
// D3DFVF_XYZ | D3DFVF_NORMAL puts them in, and the colour and the stage's coordinates after it
const D3D11_INPUT_ELEMENT_DESC kLitLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the pre-transformed and transformed families with D3DFVF_SPECULAR, which the eight-stage combiner reads as COLOR1.
// the specular DWORD sits straight after the diffuse, the order their FVF masks put them in, and the coordinates after
// both colours
const D3D11_INPUT_ELEMENT_DESC kPreSpecLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kPreSpecTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kTransformSpecLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kTransformSpecTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kLitTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the normal-less lit formats: the colours the FVF carries, at the offsets D3DFVF_DIFFUSE and D3DFVF_SPECULAR put them
const D3D11_INPUT_ELEMENT_DESC kLitNoNormalLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC kLitNoNormalDiffuseLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC kLitNoNormalSpecularLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC kLitNoNormalDiffuseSpecularLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the pixel shader's stage constants, as the shader declares them (d3d9_fixed_function_shaders.hpp)
struct StageConstants {
  UINT32 ops[4];
  UINT32 args[4];
  float factor[4];
};

struct StageConstantsExtended {
  UINT32 ops[4];
  UINT32 args[4];
  float factor[4];
  UINT32 ops1[4];
  UINT32 args1[4];
  float fog[4];
  float fogcolor[4];
  UINT32 alpha[4];
  float tex0[16];
  // stage 1's matrix and its D3DTSS_TEXTURETRANSFORMFLAGS (the extended two-stage shader applies each stage's own)
  float tex1[16];
  // x: D3DRS_SPECULARENABLE, the extended specular shaders' (the others leave it unread); y: texcoord count; z:
  // stage 0's matrix enabled; w: stage 1's flags (0 disabled, 2..4 COUNT2..COUNT4, 256 the PROJECTED bit)
  UINT32 misc[4];
  // per stage x: 0 a 2D texture, 1 a cube, 2 a volume, which is the sampling instruction the stage reads (the shader's
  // dims, d3d9_ff_extended_shaders.hpp)
  UINT32 dims[2][4];
};

void
fill_stage_constants(StageConstantsExtended &constants, const FixedFunctionDraw &draw, const FixedFunctionSampling *sampling) {
  if (sampling) {
    memcpy(constants.ops, sampling->ops, sizeof(constants.ops));
    memcpy(constants.args, sampling->args, sizeof(constants.args));
    memcpy(constants.factor, sampling->factor, sizeof(constants.factor));
    memcpy(constants.ops1, sampling->ops1, sizeof(constants.ops1));
    memcpy(constants.args1, sampling->args1, sizeof(constants.args1));
    memcpy(constants.tex0, sampling->tex0, sizeof(constants.tex0));
    memcpy(constants.tex1, sampling->tex1, sizeof(constants.tex1));
    memcpy(constants.dims, sampling->dims, sizeof(constants.dims));
  }
  constants.fog[0] = draw.fog_enabled ? 1.0f : 0.0f;
  constants.fog[1] = draw.fog_start;
  constants.fog[2] = draw.fog_end;
  constants.fog[3] = draw.fog_density;
  memcpy(constants.fogcolor, draw.fog_color, sizeof(constants.fogcolor));
  constants.alpha[0] = draw.alpha_test ? 1u : 0u;
  constants.alpha[1] = draw.alpha_func;
  constants.alpha[2] = draw.alpha_ref;
  constants.alpha[3] = draw.fog_table_mode;
  // x: the specular add, y: the declaration's texcoord component count (0 for a declaration draw, whose shader already
  // writes every component), z: whether stage 0's texture matrix is enabled (fixed_function_shaders.cpp's shade)
  const DWORD texcoord_format = (draw.fvf >> 16) & 3;
  constants.misc[0] = draw.spec_add ? 1u : 0u;
  constants.misc[1] = draw.fvf ? (texcoord_format == 1 ? 3u : texcoord_format == 2 ? 4u : texcoord_format == 3 ? 1u : 2u)
                                : 0u;
  constants.misc[2] = sampling && sampling->transform0 ? 1u : 0u;
  constants.misc[3] = sampling ? sampling->transform1 : 0u;
}

// a blended vertex: p' = sum(w_i * p * World_i) * View * Projection, the last weight being 1 minus the rest (Microsoft
// Learn, Vertex Blending). `active` is how many of the vertex's weights D3DRS_VERTEXBLEND applies: 0 is WORLD alone
// (D3DVBF_DISABLE, D3DVBF_0WEIGHTS, or a tween already baked into WORLD). One shader per count, so the count is not a
// constant the program has to branch on. cb0 holds four world matrices, then view*projection. A matrix is stored as
// D3D9 stores it, a row to a register, and a position is a row vector times that: p.x*row0 + p.y*row1 + p.z*row2 +
// p.w*row3 (the same product mul() gives a row_major float4x4).
std::vector<DWORD>
blend_vertex_shader(UINT weights, UINT active, bool diffuse) {
  using namespace dxbc;
  dxbc::Writer w(dxbc::Stage::Vertex);
  dxbc::SignatureElement position;
  position.semantic = "POSITION";
  position.reg = 0;
  position.mask = 0x7;
  w.input(position);
  dxbc::SignatureElement blend;
  blend.semantic = "BLENDWEIGHT";
  blend.reg = 1;
  blend.mask = (1u << weights) - 1;
  w.input(blend);
  dxbc::SignatureElement colour;
  colour.semantic = "COLOR";
  colour.reg = 2;
  colour.mask = 0xF;
  if (diffuse)
    w.input(colour);
  dxbc::SignatureElement out_position;
  out_position.semantic = "SV_Position";
  out_position.reg = 0;
  out_position.mask = 0xF;
  out_position.system_value = D3D_NAME_POSITION;
  w.output(out_position);
  dxbc::SignatureElement out_colour;
  out_colour.semantic = "COLOR";
  out_colour.reg = 1;
  out_colour.mask = 0xF;
  w.output(out_colour);

  w.temps(4);
  w.declare_input(0, 0x7);
  w.declare_input(1, (1u << weights) - 1);
  if (diffuse)
    w.declare_input(2, 0xF);
  w.declare_output_siv(0, D3D_NAME_POSITION);
  w.declare_output(1);
  w.declare_constant_buffer(0, 20);

  w.mov(dxbc::dest_temp(0, 0x7), dxbc::src_input(0));
  w.mov(dxbc::dest_temp(0, 0x8), dxbc::src_immediate(0, 0, 0, 1));

  // p' = p.x*row0 + p.y*row1 + p.z*row2 + p.w*row3. `matrix` 0..3 are the worlds, 4 is view*projection
  auto mul_matrix = [&](UINT source, UINT matrix, UINT dest) {
    w.mov(dxbc::dest_temp(dest), dxbc::src_immediate(0, 0, 0, 0));
    const DWORD component[4] = {0x00, 0x55, 0xAA, 0xFF};
    for (UINT row = 0; row < 4; row++)
      w.instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(dest), dxbc::src_temp(source, component[row]), dxbc::src_constant(0, matrix * 4 + row),
           dxbc::src_temp(dest)}
      );
  };

  if (active == 0) {
    mul_matrix(0, 0, 1);
  } else {
    w.mov(dxbc::dest_temp(1), dxbc::src_immediate(0, 0, 0, 0));
    w.mov(dxbc::dest_temp(3, 0x1), dxbc::src_immediate(1, 0, 0, 0));
    for (UINT index = 0; index < active; index++) {
      const DWORD swizzle = index == 0 ? 0x00 : index == 1 ? 0x55 : 0xAA;
      mul_matrix(0, index, 2);
      w.instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(1), dxbc::src_temp(2), dxbc::src_input(1, swizzle), dxbc::src_temp(1)}
      );
      w.instruction(
          D3D10_SB_OPCODE_ADD, false,
          {dxbc::dest_temp(3, 0x1), dxbc::src_temp(3, 0x00), dxbc::src_input(1, swizzle, D3D10_SB_OPERAND_MODIFIER_NEG)}
      );
    }
    // the weight the vertex does not carry: 1 minus the ones it does, applied to the next world matrix
    mul_matrix(0, active, 2);
    w.instruction(
        D3D10_SB_OPCODE_MAD, false,
        {dxbc::dest_temp(1), dxbc::src_temp(2), dxbc::src_temp(3, 0x00), dxbc::src_temp(1)}
    );
  }
  mul_matrix(1, 4, 2);
  w.mov(dxbc::dest_output(0), dxbc::src_temp(2));
  // a vertex with no colour channel is white, D3D9's default for a format without D3DFVF_DIFFUSE
  w.mov(dxbc::dest_output(1), diffuse ? dxbc::src_input(2) : dxbc::src_immediate(1, 1, 1, 1));
  return w.container();
}

} // namespace

bool
FixedFunctionPipeline::blend_format(DWORD fvf, UINT &weights, bool &diffuse) {
  // a blended vertex is a position, its blend weights and at most a diffuse colour, and nothing else this path reads
  // (Microsoft Learn, D3DFVF_XYZB1); a format without D3DFVF_DIFFUSE draws white. the last weight is 1 minus the sum,
  // so it is not in the vertex
  const DWORD rest = fvf & ~D3DFVF_POSITION_MASK;
  if (rest != 0 && rest != D3DFVF_DIFFUSE)
    return false;
  diffuse = rest == D3DFVF_DIFFUSE;
  const DWORD position = fvf & D3DFVF_POSITION_MASK;
  if (position == D3DFVF_XYZB1)
    weights = 1;
  else if (position == D3DFVF_XYZB2)
    weights = 2;
  else if (position == D3DFVF_XYZB3)
    weights = 3;
  else
    return false;
  return true;
}

namespace {

// D3DFVF_TEXCOORDSIZE*n*'s encoding is not the component count: 0 names two components, 1 three, 2 four and 3 one
// (d3d9types.h's D3DFVF_TEXTUREFORMAT2, 3, 4 and 1)
UINT
texcoord_components(DWORD fvf) {
  switch ((fvf >> 16) & 3) {
  case 0:
    return 2;
  case 1:
    return 3;
  case 2:
    return 4;
  default:
    return 1;
  }
}

} // namespace

UINT
FixedFunctionPipeline::texcoord_width(DWORD fvf) {
  return fvf ? texcoord_components(fvf) : 0;
}

bool
FixedFunctionPipeline::pair_format(DWORD fvf) {
  const UINT sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  if (sets < 2 || sets > 8)
    return false;
  for (UINT at = 0; at < sets; at++) {
    if (((fvf >> (16 + at * 2)) & 0x3) != D3DFVF_TEXTUREFORMAT2)
      return false;
  }
  const DWORD family = fvf & ~(D3DFVF_TEXCOUNT_MASK | 0xFFFF0000u);
  return family == D3DFVF_XYZ || family == (D3DFVF_XYZ | D3DFVF_DIFFUSE);
}

bool
FixedFunctionPipeline::supports(DWORD fvf) {
  UINT weights = 0;
  bool blend_diffuse = false;
  if (blend_format(fvf, weights, blend_diffuse))
    return true;
  if (pair_format(fvf))
    return true;
  // D3DFVF_TEXCOORDSIZE*n* only changes the vertex's stride, not the shader or the layout: the families' coordinate
  // element reads the first two components at the same offset whatever the size, so every size 1..4 is supported
  // (0 is the default of two). a size the FVF does not declare leaves the family's own two-coordinate form
  const DWORD family = fvf & ~0x70000u;
  // only the three-component coordinate takes this path: the volume transform shader carries its slice, while the
  // other sizes keep the passthrough vertex shader's layout, which reads the components where the FVF puts them
  if (texcoord_components(fvf) == 3 &&
      (family == (kTransformedTexNoDiffuseFVF & ~0x70000u) ||
       family == (kPreTransformedTexNoDiffuseFVF & ~0x70000u)))
    return true;
  // a one-component coordinate is the two-component family's vertex with its v left to the gather (Wine visual.c
  // texture_transform_flags_test's FLOAT1 declaration)
  const DWORD base = family & ~D3DFVF_TEXCOUNT_MASK;
  if (texcoord_components(fvf) == 1 && (base == D3DFVF_XYZ || base == (D3DFVF_XYZ | D3DFVF_DIFFUSE)))
    return true;
  return family == kPositionFVF || family == kPreTransformedFVF || family == kPreTransformedTexturedFVF ||
         family == kPreTransformedSpecFVF || family == kPreTransformedSpecTexturedFVF ||
         family == kPreTransformedTexNoDiffuseFVF || family == kTransformedTexNoDiffuseFVF ||
         family == kTransformedFVF || family == kTransformedTexturedFVF || family == kTransformedSpecFVF ||
         family == kTransformedSpecTexturedFVF || family == kLitFVF || family == kLitTexturedFVF ||
         family == kLitNoDiffuseFVF || family == kLitNoDiffuseTexturedFVF ||
         family == kLitNoNormalSpecularFVF;
}

bool
FixedFunctionPipeline::lit(DWORD fvf) {
  const DWORD family = fvf & ~0x70000u;
  return family == kLitFVF || family == kLitTexturedFVF || family == kLitNoDiffuseFVF ||
         family == kLitNoDiffuseTexturedFVF || family == kPositionFVF || family == kTransformedFVF ||
         family == kTransformedSpecFVF || family == kLitNoNormalSpecularFVF;
}

bool
FixedFunctionPipeline::transformed(DWORD fvf) {
  // a lit vertex is one the device transforms too: its position takes the same matrices. a blended vertex is
  // transformed by the world matrices its weights name
  UINT weights = 0;
  bool blend_diffuse = false;
  if (blend_format(fvf, weights, blend_diffuse))
    return true;
  // the pair shader transforms the gathered position through the composed matrix, so its canonical format is a
  // device-transformed one even though no other list names it
  if (pair_format(fvf))
    return true;
  if (texcoord_components(fvf) == 3 && (fvf & ~0x70000u) == (kTransformedTexNoDiffuseFVF & ~0x70000u))
    return true;
  // a one-component coordinate is still an XYZ vertex: D3DFVF_TEXCOORDSIZE1 changes the stride, not the position
  // space (Wine visual.c texture_transform_flags_test's FLOAT1 declaration quad is XYZ, not XYZRHW)
  const DWORD float1_base = (fvf & ~0x70000u) & ~D3DFVF_TEXCOUNT_MASK;
  if (texcoord_components(fvf) == 1 &&
      (float1_base == D3DFVF_XYZ || float1_base == (D3DFVF_XYZ | D3DFVF_DIFFUSE)))
    return true;
  const DWORD family = fvf & ~0x70000u;
  return family == kPositionFVF || family == kTransformedFVF || family == kTransformedTexturedFVF ||
         family == kTransformedTexNoDiffuseFVF || family == kTransformedSpecFVF ||
         family == kTransformedSpecTexturedFVF || lit(fvf);
}

bool
FixedFunctionPipeline::textured(DWORD fvf) {
  if (pair_format(fvf))
    return true;
  if (texcoord_components(fvf) == 3 &&
      ((fvf & ~0x70000u) == (kTransformedTexNoDiffuseFVF & ~0x70000u) ||
       (fvf & ~0x70000u) == (kPreTransformedTexNoDiffuseFVF & ~0x70000u)))
    return true;
  const DWORD float1_base = (fvf & ~0x70000u) & ~D3DFVF_TEXCOUNT_MASK;
  if (texcoord_components(fvf) == 1 &&
      (float1_base == D3DFVF_XYZ || float1_base == (D3DFVF_XYZ | D3DFVF_DIFFUSE)))
    return true;
  const DWORD family = fvf & ~0x70000u;
  return family == kPreTransformedTexturedFVF || family == kPreTransformedSpecTexturedFVF ||
         family == kPreTransformedTexNoDiffuseFVF || family == kTransformedTexturedFVF ||
         family == kTransformedTexNoDiffuseFVF || family == kTransformedSpecTexturedFVF ||
         family == kLitTexturedFVF || family == kLitNoDiffuseTexturedFVF;
}

UINT
FixedFunctionPipeline::stride(DWORD fvf) {
  UINT weights = 0;
  bool blend_diffuse = false;
  if (blend_format(fvf, weights, blend_diffuse))
    return 12 + weights * 4 + (blend_diffuse ? 4 : 0);
  if (pair_format(fvf))
    return 12 + ((fvf & D3DFVF_DIFFUSE) ? 4u : 0u) +
           8u * ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
  // the stride is what the format's own layout is, composed in D3D9's order: the position (a blend weight after it),
  // the normal, the diffuse and specular colours, then each coordinate set's declared components. Deriving it rather
  // than listing the families keeps a size the list forgets from being read at another format's width (a four-component
  // no-diffuse coordinate is 28 bytes, not the 20 the pre-transformed family's own stride is).
  DWORD position = fvf & D3DFVF_POSITION_MASK;
  UINT bytes = position == D3DFVF_XYZRHW ? 16u : 12u;
  if (position == D3DFVF_XYZB1 || position == D3DFVF_XYZB2 || position == D3DFVF_XYZB3)
    bytes = 12u + 4u * (position == D3DFVF_XYZB1 ? 1u : position == D3DFVF_XYZB2 ? 2u : 3u);
  if (fvf & D3DFVF_NORMAL)
    bytes += 12;
  if (fvf & D3DFVF_DIFFUSE)
    bytes += 4;
  if (fvf & D3DFVF_SPECULAR)
    bytes += 4;
  const UINT sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (UINT set = 0; set < sets && set < 8; set++) {
    // D3DFVF_TEXCOORDSIZE's encoding is inverted: 0 two components, 1 three, 2 four and 3 one
    switch ((fvf >> (16 + set * 2)) & 3) {
    case 1:
      bytes += 12;
      break;
    case 2:
      bytes += 16;
      break;
    case 3:
      bytes += 4;
      break;
    default:
      bytes += 8;
      break;
    }
  }
  return bytes;
}

HRESULT
FixedFunctionPipeline::initialize(ID3D11Device *device) {
  HRESULT hr = device->CreateVertexShader(
      kPreTransformedVertexShader, sizeof(kPreTransformedVertexShader), nullptr, &vertex_shader_
  );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kPreTransformedTexturedVertexShader, sizeof(kPreTransformedTexturedVertexShader), nullptr,
        &textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(kVertexColourPixelShader, sizeof(kVertexColourPixelShader), nullptr, &pixel_shader_);
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kFlatVertexColourPixelShader, sizeof(kFlatVertexColourPixelShader), nullptr, &flat_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kTexturedPixelShader, sizeof(kTexturedPixelShader), nullptr, &textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kFlatTexturedPixelShader, sizeof(kFlatTexturedPixelShader), nullptr, &flat_textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedColourPixelShader, sizeof(kExtendedColourPixelShader), nullptr, &extended_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedFlatColourPixelShader, sizeof(kExtendedFlatColourPixelShader), nullptr, &extended_flat_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedTexturedPixelShader, sizeof(kExtendedTexturedPixelShader), nullptr, &extended_textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedFlatTexturedPixelShader, sizeof(kExtendedFlatTexturedPixelShader), nullptr,
        &extended_flat_textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedSpecColourPixelShader, sizeof(kExtendedSpecColourPixelShader), nullptr,
        &extended_spec_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedSpecFlatColourPixelShader, sizeof(kExtendedSpecFlatColourPixelShader), nullptr,
        &extended_spec_flat_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedSpecTexturedPixelShader, sizeof(kExtendedSpecTexturedPixelShader), nullptr,
        &extended_spec_textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedSpecFlatTexturedPixelShader, sizeof(kExtendedSpecFlatTexturedPixelShader), nullptr,
        &extended_spec_flat_textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreTransformedLayout, ARRAYSIZE(kPreTransformedLayout), kPreTransformedVertexShader,
        sizeof(kPreTransformedVertexShader), &vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreTransformedTexturedLayout, ARRAYSIZE(kPreTransformedTexturedLayout), kPreTransformedTexturedVertexShader,
        sizeof(kPreTransformedTexturedVertexShader), &textured_vertex_layout_
    );
  // the transform's vertex shaders, which read the composed matrix at b0 (d3d9_transform_shaders.hpp)
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformedVertexShader, sizeof(kTransformedVertexShader), nullptr, &transformed_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformedTexturedVertexShader, sizeof(kTransformedTexturedVertexShader), nullptr,
        &transformed_textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformedTexturedUserVertexShader, sizeof(kTransformedTexturedUserVertexShader), nullptr,
        &transformed_textured_user_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformedLayout, ARRAYSIZE(kTransformedLayout), kTransformedVertexShader, sizeof(kTransformedVertexShader),
        &transformed_vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformedTexturedLayout, ARRAYSIZE(kTransformedTexturedLayout), kTransformedTexturedVertexShader,
        sizeof(kTransformedTexturedVertexShader), &transformed_textured_vertex_layout_
    );
  // the two-UV family the multi-coordinate-set gather draws through: canonical position, colour and two float2
  // coordinates, matching the layout the device writes
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformedPairVertexShader, sizeof(kTransformedPairVertexShader), nullptr, &transformed_pair_vertex_shader_
    );
  if (SUCCEEDED(hr)) {
    static const D3D11_INPUT_ELEMENT_DESC kPairLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(
        kPairLayout, ARRAYSIZE(kPairLayout), kTransformedPairVertexShader, sizeof(kTransformedPairVertexShader),
        &transformed_pair_layout_
    );
  }
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedTexturedUv1PixelShader, sizeof(kExtendedTexturedUv1PixelShader), nullptr,
        &extended_textured_uv1_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kExtendedFlatTexturedUv1PixelShader, sizeof(kExtendedFlatTexturedUv1PixelShader), nullptr,
        &extended_flat_textured_uv1_pixel_shader_
    );
  // the volume variant of the transformed vertex shader and its two layouts: the no-diffuse one's shader has no
  // colour input, so its layout carries only the position and the coordinate
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformed3dTexturedVertexShader, sizeof(kTransformed3dTexturedVertexShader), nullptr, &volume_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformed3dNoDiffuseTexturedVertexShader, sizeof(kTransformed3dNoDiffuseTexturedVertexShader), nullptr,
        &volume_no_diffuse_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformed3dTexturedLayout, ARRAYSIZE(kTransformed3dTexturedLayout),
        kTransformed3dNoDiffuseTexturedVertexShader, sizeof(kTransformed3dNoDiffuseTexturedVertexShader),
        &volume_textured_vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformed3dDiffuseTexturedLayout, ARRAYSIZE(kTransformed3dDiffuseTexturedLayout),
        kTransformed3dTexturedVertexShader, sizeof(kTransformed3dTexturedVertexShader),
        &volume_diffuse_textured_vertex_layout_
    );
  if (FAILED(hr)) {
    ERR("D3D9: the volume transform shaders did not load: 0x", std::hex, hr);
    return hr;
  }
  // the lit vertex shaders, which read the composed matrix at b0 and the material, the lights and the lighting states
  // at b1 (d3d9_lighting_shaders.hpp)
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(kLitVertexShader, sizeof(kLitVertexShader), nullptr, &lit_vertex_shader_);
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitTexturedVertexShader, sizeof(kLitTexturedVertexShader), nullptr, &lit_textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitLayout, ARRAYSIZE(kLitLayout), kLitVertexShader, sizeof(kLitVertexShader), &lit_vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitTexturedLayout, ARRAYSIZE(kLitTexturedLayout), kLitTexturedVertexShader, sizeof(kLitTexturedVertexShader),
        &lit_textured_vertex_layout_
    );
  // the normal-less lit formats: one shader and layout per combination of the colours the FVF can carry
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitNoNormalVertexShader, sizeof(kLitNoNormalVertexShader), nullptr, &lit_no_normal_vertex_shader_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitNoNormalDiffuseVertexShader, sizeof(kLitNoNormalDiffuseVertexShader), nullptr,
        &lit_no_normal_vertex_shader_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitNoNormalSpecularVertexShader, sizeof(kLitNoNormalSpecularVertexShader), nullptr,
        &lit_no_normal_vertex_shader_[2]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitNoNormalDiffuseSpecularVertexShader, sizeof(kLitNoNormalDiffuseSpecularVertexShader), nullptr,
        &lit_no_normal_vertex_shader_[3]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitNoNormalLayout, ARRAYSIZE(kLitNoNormalLayout), kLitNoNormalVertexShader, sizeof(kLitNoNormalVertexShader),
        &lit_no_normal_layout_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitNoNormalDiffuseLayout, ARRAYSIZE(kLitNoNormalDiffuseLayout), kLitNoNormalDiffuseVertexShader,
        sizeof(kLitNoNormalDiffuseVertexShader), &lit_no_normal_layout_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitNoNormalSpecularLayout, ARRAYSIZE(kLitNoNormalSpecularLayout), kLitNoNormalSpecularVertexShader,
        sizeof(kLitNoNormalSpecularVertexShader), &lit_no_normal_layout_[2]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitNoNormalDiffuseSpecularLayout, ARRAYSIZE(kLitNoNormalDiffuseSpecularLayout),
        kLitNoNormalDiffuseSpecularVertexShader, sizeof(kLitNoNormalDiffuseSpecularVertexShader),
        &lit_no_normal_layout_[3]
    );
  // the clipped variants of the transformed and lit shaders, which read the planes at b2
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedTransformedVertexShader, sizeof(kClippedTransformedVertexShader), nullptr,
        &clipped_transformed_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedTransformedTexturedVertexShader, sizeof(kClippedTransformedTexturedVertexShader), nullptr,
        &clipped_transformed_textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedLitVertexShader, sizeof(kClippedLitVertexShader), nullptr, &clipped_lit_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedLitTexturedVertexShader, sizeof(kClippedLitTexturedVertexShader), nullptr,
        &clipped_lit_textured_vertex_shader_
    );
  if (FAILED(hr)) {
    ERR("D3D9: the fixed-function shaders this build carries do not load: 0x", std::hex, hr);
    return hr;
  }

  // the eight-stage combiner's eight pixel-shader combinations and the vertex shaders its paths set: the pixel
  // shaders read the stages' board (fill_fixed_function_eight, d3d9_device_draw.cpp), and a vertex shader comes
  // from the FVF's family and whether it carries D3DFVF_SPECULAR (the COLOR1 the D3DTA_SPECULAR argument reads) or
  // is lit (whose COLOR1 is the lighting's own specular)
  static const struct {
    const unsigned char *blob;
    size_t size;
    int flat, spec, textured;
  } eight_ps_creates[] = {
      {kEightStageColourPixelShader, sizeof(kEightStageColourPixelShader), 0, 0, 0},
      {kEightStageFlatColourPixelShader, sizeof(kEightStageFlatColourPixelShader), 1, 0, 0},
      {kEightStageColourSpecPixelShader, sizeof(kEightStageColourSpecPixelShader), 0, 1, 0},
      {kEightStageFlatColourSpecPixelShader, sizeof(kEightStageFlatColourSpecPixelShader), 1, 1, 0},
      {kEightStageTexturedPixelShader, sizeof(kEightStageTexturedPixelShader), 0, 0, 1},
      {kEightStageFlatTexturedPixelShader, sizeof(kEightStageFlatTexturedPixelShader), 1, 0, 1},
      {kEightStageTexturedSpecPixelShader, sizeof(kEightStageTexturedSpecPixelShader), 0, 1, 1},
      {kEightStageFlatTexturedSpecPixelShader, sizeof(kEightStageFlatTexturedSpecPixelShader), 1, 1, 1},
  };
  for (const auto &one : eight_ps_creates) {
    if (SUCCEEDED(hr))
      hr = device->CreatePixelShader(
          one.blob, one.size, nullptr, &eight_pixel_[one.flat][one.spec][one.textured]
      );
  }
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kPreExtVertexShaderSpec, sizeof(kPreExtVertexShaderSpec), nullptr, &eight_pre_spec_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kPreExtVertexShaderSpecU2, sizeof(kPreExtVertexShaderSpecU2), nullptr, &eight_pre_spec_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformExtVertexShaderSpec, sizeof(kTransformExtVertexShaderSpec), nullptr, &eight_transform_spec_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformExtVertexShaderSpecU2, sizeof(kTransformExtVertexShaderSpecU2), nullptr, &eight_transform_spec_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kPreExtVertexShaderU2, sizeof(kPreExtVertexShaderU2), nullptr, &eight_pre_textured_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformExtVertexShaderU2, sizeof(kTransformExtVertexShaderU2), nullptr, &eight_transform_textured_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformExtVertexShaderU1, sizeof(kTransformExtVertexShaderU1), nullptr, &eight_transform_u1_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformedU1TexturedLayout, ARRAYSIZE(kTransformedU1TexturedLayout), kTransformExtVertexShaderU1,
        sizeof(kTransformExtVertexShaderU1), &eight_transform_u1_layout_
    );
  // the wide-coordinate family: a stage declared with a three- or four-component coordinate reads every component it
  // carries, so a projected stage's division is by the vertex's z or w. The layouts pair each shader with the elements
  // it declares, at the offsets D3DFVF_XYZ/XYZRHW, the colour(s) and the coordinate put them.
  if (SUCCEEDED(hr)) {
    struct WideShader {
      const unsigned char *blob;
      size_t size;
    };
    // [pre-transformed?][no colour, diffuse, specular][width 3 or 4]: index 0 is the vertex the device transforms,
    // index 1 the one already in screen space, which is also the offset each layout's position element starts at
    const WideShader wide[2][3][2] = {
        {{{kTransformExtVertexShaderNoDiffuseU3, sizeof(kTransformExtVertexShaderNoDiffuseU3)},
          {kTransformExtVertexShaderNoDiffuseU4, sizeof(kTransformExtVertexShaderNoDiffuseU4)}},
         {{kTransformExtVertexShaderU3, sizeof(kTransformExtVertexShaderU3)},
          {kTransformExtVertexShaderU4, sizeof(kTransformExtVertexShaderU4)}},
         {{kTransformExtVertexShaderSpecU3, sizeof(kTransformExtVertexShaderSpecU3)},
          {kTransformExtVertexShaderSpecU4, sizeof(kTransformExtVertexShaderSpecU4)}}},
        {{{kPreExtVertexShaderNoDiffuseU3, sizeof(kPreExtVertexShaderNoDiffuseU3)},
          {kPreExtVertexShaderNoDiffuseU4, sizeof(kPreExtVertexShaderNoDiffuseU4)}},
         {{kPreExtVertexShaderU3, sizeof(kPreExtVertexShaderU3)},
          {kPreExtVertexShaderU4, sizeof(kPreExtVertexShaderU4)}},
         {{kPreExtVertexShaderSpecU3, sizeof(kPreExtVertexShaderSpecU3)},
          {kPreExtVertexShaderSpecU4, sizeof(kPreExtVertexShaderSpecU4)}}},
    };
    for (UINT pre = 0; pre < 2 && SUCCEEDED(hr); pre++) {
      for (UINT colour = 0; colour < 3 && SUCCEEDED(hr); colour++) {
        for (UINT width = 0; width < 2 && SUCCEEDED(hr); width++) {
          const WideShader &shader = wide[pre][colour][width];
          hr = device->CreateVertexShader(shader.blob, shader.size, nullptr, &wide_vertex_shader_[pre][colour][width]);
          if (FAILED(hr))
            break;
          D3D11_INPUT_ELEMENT_DESC layout[4]{};
          UINT count = 0;
          UINT offset = pre ? 16 : 12;
          layout[count++] = {"POSITION", 0, pre ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT, 0,
                             0, D3D11_INPUT_PER_VERTEX_DATA, 0};
          if (colour >= 1) {
            layout[count++] = {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
            offset += 4;
          }
          if (colour >= 2) {
            layout[count++] = {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
            offset += 4;
          }
          layout[count++] = {"TEXCOORD", 0,
                             width == 0 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offset,
                             D3D11_INPUT_PER_VERTEX_DATA, 0};
          hr = device->CreateInputLayout(
              layout, count, shader.blob, shader.size, &wide_vertex_layout_[pre][colour][width]
          );
        }
      }
    }
  }
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedTransformExtSpecVertexShader, sizeof(kClippedTransformExtSpecVertexShader), nullptr,
        &eight_clipped_transform_spec_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedTransformExtSpecTexturedVertexShader, sizeof(kClippedTransformExtSpecTexturedVertexShader), nullptr,
        &eight_clipped_transform_spec_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedTransformExtVertexShader, sizeof(kClippedTransformExtVertexShader), nullptr,
        &eight_clipped_transform_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedTransformExtTexturedVertexShader, sizeof(kClippedTransformExtTexturedVertexShader), nullptr,
        &eight_clipped_transform_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitSpecVertexShader, sizeof(kLitSpecVertexShader), nullptr, &eight_lit_spec_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitSpecTexturedVertexShader, sizeof(kLitSpecTexturedVertexShader), nullptr, &eight_lit_spec_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedLitSpecVertexShader, sizeof(kClippedLitSpecVertexShader), nullptr, &eight_clipped_lit_spec_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kClippedLitSpecTexturedVertexShader, sizeof(kClippedLitSpecTexturedVertexShader), nullptr,
        &eight_clipped_lit_spec_[1]
    );
  // the COLOR1 input layouts the D3DFVF_SPECULAR formats read, and the paired COLOR0/1 vertex shaders they match
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreSpecLayout, ARRAYSIZE(kPreSpecLayout), kPreExtVertexShaderSpec, sizeof(kPreExtVertexShaderSpec),
        &eight_pre_spec_layout_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreSpecTexturedLayout, ARRAYSIZE(kPreSpecTexturedLayout), kPreExtVertexShaderSpecU2,
        sizeof(kPreExtVertexShaderSpecU2), &eight_pre_spec_layout_[1]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformSpecLayout, ARRAYSIZE(kTransformSpecLayout), kTransformExtVertexShaderSpec,
        sizeof(kTransformExtVertexShaderSpec), &eight_transform_spec_layout_[0]
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformSpecTexturedLayout, ARRAYSIZE(kTransformSpecTexturedLayout), kTransformExtVertexShaderSpecU2,
        sizeof(kTransformExtVertexShaderSpecU2), &eight_transform_spec_layout_[1]
    );
  // the stages' board the eight-stage pixel shaders read at b0
  if (SUCCEEDED(hr)) {
    D3D11_BUFFER_DESC stage8_desc{};
    stage8_desc.ByteWidth = sizeof(FixedFunctionSampling8); // 1104 bytes, the pixel shader's declared size
    stage8_desc.Usage = D3D11_USAGE_DYNAMIC;
    stage8_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    stage8_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&stage8_desc, nullptr, &stage8_);
  }
  if (FAILED(hr)) {
    ERR("D3D9: the eight-stage combiner's shaders this build carries do not load: 0x", std::hex, hr);
    return hr;
  }

  // what the vertex shader maps screen space with: the viewport's origin and size
  D3D11_BUFFER_DESC viewport_desc{};
  viewport_desc.ByteWidth = 16;
  viewport_desc.Usage = D3D11_USAGE_DYNAMIC;
  viewport_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  viewport_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&viewport_desc, nullptr, &viewport_);
  if (FAILED(hr))
    return hr;

  // the composed world, view and projection a transformed vertex is multiplied by, which changes every time one of the
  // three does: written before each transformed draw
  D3D11_BUFFER_DESC transform_desc{};
  transform_desc.ByteWidth = 16 * sizeof(float);
  transform_desc.Usage = D3D11_USAGE_DYNAMIC;
  transform_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  transform_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&transform_desc, nullptr, &transform_);
  if (FAILED(hr))
    return hr;

  // the four world matrices and the view*projection a blended vertex reads. which of its weights apply is which shader
  // is bound, not a constant (blend_vertex_shader)
  D3D11_BUFFER_DESC blend_desc{};
  blend_desc.ByteWidth = 20 * 4 * sizeof(float);
  blend_desc.Usage = D3D11_USAGE_DYNAMIC;
  blend_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  blend_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&blend_desc, nullptr, &blend_constants_);
  if (FAILED(hr))
    return hr;
  const DXGI_FORMAT weight_format[3] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT};
  for (UINT weights = 1; weights <= 3 && SUCCEEDED(hr); weights++) {
    for (UINT active = 0; active <= weights && SUCCEEDED(hr); active++) {
      for (UINT colour = 0; colour < 2 && SUCCEEDED(hr); colour++) {
        blend_dxbc_[weights - 1][active][colour] = blend_vertex_shader(weights, active, colour != 0);
        hr = device->CreateVertexShader(
            blend_dxbc_[weights - 1][active][colour].data(),
            blend_dxbc_[weights - 1][active][colour].size() * sizeof(DWORD), nullptr,
            &blend_vs_[weights - 1][active][colour]
        );
      }
    }
    if (FAILED(hr))
      break;
    for (UINT colour = 0; colour < 2 && SUCCEEDED(hr); colour++) {
      const D3D11_INPUT_ELEMENT_DESC layout[3] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"BLENDWEIGHT", 0, weight_format[weights - 1], 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12 + weights * 4, D3D11_INPUT_PER_VERTEX_DATA, 0},
      };
      // a format without D3DFVF_DIFFUSE has no colour element to read
      hr = device->CreateInputLayout(
          layout, colour ? 3u : 2u, blend_dxbc_[weights - 1][0][colour].data(),
          blend_dxbc_[weights - 1][0][colour].size() * sizeof(DWORD), &blend_layout_[weights - 1][colour]
      );
    }
  }
  if (SUCCEEDED(hr)) {
    // a D3DCOLOR blend-weight declaration packs the three weights into the colour's r, g and b
    const D3D11_INPUT_ELEMENT_DESC packed[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(
        packed, 2, blend_dxbc_[2][0][0].data(), blend_dxbc_[2][0][0].size() * sizeof(DWORD), &blend_packed_layout_
    );
  }
  if (FAILED(hr)) {
    ERR("D3D9: the vertex-blending shaders did not load: 0x", std::hex, hr);
    return hr;
  }

  // the lights, the material and the lighting states a lit draw reads, written before each lit draw
  D3D11_BUFFER_DESC lighting_desc{};
  lighting_desc.ByteWidth = sizeof(LightConstants);
  lighting_desc.Usage = D3D11_USAGE_DYNAMIC;
  lighting_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  lighting_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&lighting_desc, nullptr, &lighting_);
  if (FAILED(hr))
    return hr;

  // the six clip planes a clipped draw reads at b2, written before each one
  D3D11_BUFFER_DESC clip_desc{};
  clip_desc.ByteWidth = 6 * 4 * sizeof(float);
  clip_desc.Usage = D3D11_USAGE_DYNAMIC;
  clip_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  clip_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&clip_desc, nullptr, &clip_planes_);
  if (FAILED(hr))
    return hr;

  // what the textured and extended pixel shaders' stage reads: the one-stage constants the original blob uses, and
  // the extra stage, fog, alpha test and texture matrix the extended blob adds after them
  D3D11_BUFFER_DESC stage_desc{};
  stage_desc.ByteWidth = sizeof(StageConstantsExtended);
  stage_desc.Usage = D3D11_USAGE_DYNAMIC;
  stage_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&stage_desc, nullptr, &stage_);
  if (FAILED(hr))
    return hr;

  // the one white texel a stage with no texture bound samples
  const DWORD white = 0xFFFFFFFF;
  D3D11_SUBRESOURCE_DATA initial{};
  initial.pSysMem = &white;
  initial.SysMemPitch = sizeof(white);
  D3D11_TEXTURE2D_DESC white_desc{};
  white_desc.Width = 1;
  white_desc.Height = 1;
  white_desc.MipLevels = 1;
  white_desc.ArraySize = 1;
  white_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  white_desc.SampleDesc.Count = 1;
  white_desc.Usage = D3D11_USAGE_DEFAULT;
  white_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  hr = device->CreateTexture2D(&white_desc, &initial, &white_texture_);
  if (SUCCEEDED(hr))
    hr = device->CreateShaderResourceView(white_texture_.Get(), nullptr, &white_view_);
  if (FAILED(hr))
    ERR("D3D9: the white texel a stage without a texture samples is not there: 0x", std::hex, hr);
  return hr;
}

void
FixedFunctionPipeline::release_device() {
  vertex_shader_.Reset();
  textured_vertex_shader_.Reset();
  transformed_vertex_shader_.Reset();
  transformed_textured_vertex_shader_.Reset();
  transformed_textured_user_vertex_shader_.Reset();
  pixel_shader_.Reset();
  flat_pixel_shader_.Reset();
  textured_pixel_shader_.Reset();
  flat_textured_pixel_shader_.Reset();
  extended_pixel_shader_.Reset();
  extended_flat_pixel_shader_.Reset();
  extended_textured_pixel_shader_.Reset();
  extended_flat_textured_pixel_shader_.Reset();
  vertex_layout_.Reset();
  textured_vertex_layout_.Reset();
  transformed_vertex_layout_.Reset();
  transformed_textured_vertex_layout_.Reset();
  transformed_pair_vertex_shader_.Reset();
  transformed_pair_layout_.Reset();
  extended_textured_uv1_pixel_shader_.Reset();
  extended_flat_textured_uv1_pixel_shader_.Reset();
  volume_vertex_shader_.Reset();
  volume_no_diffuse_vertex_shader_.Reset();
  volume_textured_vertex_layout_.Reset();
  volume_diffuse_textured_vertex_layout_.Reset();
  lit_vertex_shader_.Reset();
  lit_textured_vertex_shader_.Reset();
  lit_vertex_layout_.Reset();
  lit_textured_vertex_layout_.Reset();
  clipped_transformed_vertex_shader_.Reset();
  clipped_transformed_textured_vertex_shader_.Reset();
  clipped_lit_vertex_shader_.Reset();
  clipped_lit_textured_vertex_shader_.Reset();
  for (auto &one : eight_pixel_)
    for (auto &two : one)
      for (auto &three : two)
        three.Reset();
  for (auto &one : eight_pre_spec_)
    one.Reset();
  for (auto &one : eight_transform_spec_)
    one.Reset();
  eight_pre_textured_.Reset();
  eight_transform_textured_.Reset();
  eight_transform_u1_.Reset();
  eight_transform_u1_layout_.Reset();
  for (auto &one : wide_vertex_shader_)
    for (auto &two : one)
      for (auto &three : two)
        three.Reset();
  for (auto &one : wide_vertex_layout_)
    for (auto &two : one)
      for (auto &three : two)
        three.Reset();
  for (auto &one : eight_clipped_transform_spec_)
    one.Reset();
  for (auto &one : eight_clipped_transform_)
    one.Reset();
  for (auto &one : eight_lit_spec_)
    one.Reset();
  for (auto &one : eight_clipped_lit_spec_)
    one.Reset();
  for (auto &one : eight_pre_spec_layout_)
    one.Reset();
  for (auto &one : eight_transform_spec_layout_)
    one.Reset();
  stage8_.Reset();
  clip_planes_.Reset();
  lighting_.Reset();
  transform_.Reset();
  for (UINT weights = 0; weights < 3; weights++) {
    for (UINT active = 0; active < 4; active++)
      for (UINT colour = 0; colour < 2; colour++)
        blend_vs_[weights][active][colour].Reset();
    for (UINT colour = 0; colour < 2; colour++)
      blend_layout_[weights][colour].Reset();
  }
  blend_packed_layout_.Reset();
  blend_constants_.Reset();
  viewport_.Reset();
  stage_.Reset();
  white_texture_.Reset();
  white_view_.Reset();
}

HRESULT
FixedFunctionPipeline::bind_stage_constants(ID3D11DeviceContext *context, const FixedFunctionDraw &draw) {
  if (!stage_)
    return D3DERR_INVALIDCALL;
  StageConstantsExtended constants{};
  fill_stage_constants(constants, draw, nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(stage_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  memcpy(mapped.pData, &constants, sizeof(constants));
  context->Unmap(stage_.Get(), 0);
  ID3D11Buffer *stage = stage_.Get();
  context->PSSetConstantBuffers(0, 1, &stage);
  return S_OK;
}

HRESULT
FixedFunctionPipeline::bind(ID3D11DeviceContext *context, const FixedFunctionDraw &draw) {
  if (draw.eight_stage)
    return bind_eight(context, draw);
  const FixedFunctionSampling *sampling = draw.sampling;
  const float *world_view_projection = draw.world_view_projection;
  const LightConstants *lighting = draw.lighting;
  if (!vertex_shader_ || !textured_vertex_shader_ || !pixel_shader_ || !flat_pixel_shader_ ||
      !textured_pixel_shader_ || !flat_textured_pixel_shader_ || !vertex_layout_ || !textured_vertex_layout_ ||
      !volume_vertex_shader_ || !volume_no_diffuse_vertex_shader_ || !volume_textured_vertex_layout_ ||
      !volume_diffuse_textured_vertex_layout_ || !viewport_)
    return D3DERR_INVALIDCALL;
  if (draw.uv_pair && (!transformed_pair_vertex_shader_ || !transformed_pair_layout_ ||
                       !extended_textured_uv1_pixel_shader_ || !extended_flat_textured_uv1_pixel_shader_))
    return D3DERR_INVALIDCALL;
  // a vertex the device transforms needs the transform's shaders and the matrix they read
  const bool transformed = world_view_projection != nullptr;
  const bool blended = draw.blend_weights >= 1 && draw.blend_weights <= 3;
  const UINT blend_colour = draw.diffuse ? 1u : 0u;
  if (transformed && (!transformed_vertex_shader_ || !transformed_textured_vertex_shader_ ||
                      !transformed_textured_user_vertex_shader_ ||
                      !transformed_vertex_layout_ || !transformed_textured_vertex_layout_ || !transform_))
    return D3DERR_INVALIDCALL;
  // a lit vertex needs the lit ones as well, and the material and lights they read at b1. D3DRS_LIGHTING off is the
  // vertex's own colour even for a format that can be lit, so it takes the unlit shaders and layouts: the lit ones place
  // a normal between the position and the colour, which a format without D3DFVF_NORMAL does not have
  const bool lit = lighting != nullptr;
  const bool lighting_on = lit && lighting->flags[3] > 0.5f;
  const UINT lit_colours = (draw.fvf & D3DFVF_DIFFUSE ? 1u : 0u) | (draw.fvf & D3DFVF_SPECULAR ? 2u : 0u);
  const bool lit_no_normal = lit && draw.fvf != 0 && (draw.fvf & D3DFVF_NORMAL) == 0;
  // a clipped vertex is transformed by the clip-plane shader rather than the plain one
  const bool clipped = transformed && draw.clip_planes && !blended;
  // a no-normal format with lighting off takes the unlit shader: the lit no-normal shader has no early-out
  // for D3DRS_LIGHTING and would shade it black. a format with a normal keeps the lit shader, whose layout
  // has the normal between the position and the colour, and whose shader does output the vertex colour
  const bool lit_shader = lit && (lighting_on || !lit_no_normal);
  // a game pixel shader bound over this draw reads TEXCOORD0 where the selected VS writes it: the unlit transform,
  // immediate and pre-transformed shaders write it at 2 (textured or not, since the untextured ones write D3D9's
  // default too), and the lit one reserves 2 for COLOR1 and writes it at 3
  texcoord_slot_ = lit_shader ? 3 : 2;
  // a volume stage samples through the coordinate's third component, so its draw takes the transform shader that
  // carries it rather than the two-component one
  const bool volume0 = sampling && sampling->dims[0][0] == 2;
  if (lit && (!lit_vertex_shader_ || !lit_textured_vertex_shader_ || !lit_vertex_layout_ ||
              !lit_textured_vertex_layout_ || !lighting_ || !transform_))
    return D3DERR_INVALIDCALL;
  if (lit_no_normal && (!lit_no_normal_vertex_shader_[lit_colours] || !lit_no_normal_layout_[lit_colours]))
    return D3DERR_INVALIDCALL;

  // D3D9's integer pixel centers need the half pixel this rasterizer's do not have (d3d9_common.hpp)
  D3D11_VIEWPORT d3d11_viewport =
      to_d3d11_viewport(draw.viewport, draw.viewport.MinZ, draw.viewport.MaxZ);
  context->RSSetViewports(1, &d3d11_viewport);
  // D3D9's viewport is a clip rectangle as well as a place, and the scissor rectangle narrows it: the device's clip
  // rectangle is both (d3d9_device_targets.cpp)
  const D3D11_RECT scissor{draw.clip.left, draw.clip.top, draw.clip.right, draw.clip.bottom};
  context->RSSetScissorRects(1, &scissor);
  // the user clip planes cut a vertex the device transforms; one already in screen space is past them. a blended
  // vertex is transformed by its own shader, which does not read the planes
  const UINT blend_active = draw.blend_active > draw.blend_weights ? draw.blend_weights : draw.blend_active;
  if (blended && (!blend_vs_[draw.blend_weights - 1][blend_active][blend_colour] ||
                  !(draw.blend_packed ? blend_packed_layout_ : blend_layout_[draw.blend_weights - 1][blend_colour]) ||
                  !blend_constants_))
    return D3DERR_INVALIDCALL;
  if (clipped && (!clipped_transformed_vertex_shader_ || !clipped_transformed_textured_vertex_shader_ ||
                  !clipped_lit_vertex_shader_ || !clipped_lit_textured_vertex_shader_ || !clip_planes_))
    return D3DERR_INVALIDCALL;
  if (blended)
    context->VSSetShader(blend_vs_[draw.blend_weights - 1][blend_active][blend_colour].Get(), nullptr, 0);
  else if (clipped)
    context->VSSetShader(
        lit_shader ? (sampling ? clipped_lit_textured_vertex_shader_.Get() : clipped_lit_vertex_shader_.Get())
                   : (sampling ? clipped_transformed_textured_vertex_shader_.Get()
                               : clipped_transformed_vertex_shader_.Get()),
        nullptr, 0
    );
  else if (volume0 && transformed && !lit)
    context->VSSetShader(
        draw.diffuse ? volume_vertex_shader_.Get() : volume_no_diffuse_vertex_shader_.Get(), nullptr, 0
    );
  else if (draw.uv_pair)
    context->VSSetShader(transformed_pair_vertex_shader_.Get(), nullptr, 0);
  else
    context->VSSetShader(
        lit_shader ? (lit_no_normal ? lit_no_normal_vertex_shader_[lit_colours].Get()
                                    : sampling ? lit_textured_vertex_shader_.Get() : lit_vertex_shader_.Get())
            : transformed ? (sampling ? (draw.user_pixel ? transformed_textured_user_vertex_shader_.Get()
                                                          : transformed_textured_vertex_shader_.Get())
                                      : transformed_vertex_shader_.Get())
                          : (sampling ? textured_vertex_shader_.Get() : vertex_shader_.Get()),
        nullptr, 0
    );
  // Lit no-diffuse draws expand to the with-DIFFUSE layouts (white inserted) before bind
  context->IASetInputLayout(
      draw.uv_pair ? transformed_pair_layout_.Get()
      : blended ? (draw.blend_packed ? blend_packed_layout_.Get()
                                  : blend_layout_[draw.blend_weights - 1][blend_colour].Get())
      : volume0 && transformed && !lit
          ? (draw.diffuse ? volume_diffuse_textured_vertex_layout_.Get() : volume_textured_vertex_layout_.Get())
      : lit_shader ? (lit_no_normal ? lit_no_normal_layout_[lit_colours].Get()
                                    : sampling ? lit_textured_vertex_layout_.Get() : lit_vertex_layout_.Get())
          : transformed ? (sampling ? transformed_textured_vertex_layout_.Get() : transformed_vertex_layout_.Get())
                        : (sampling ? textured_vertex_layout_.Get() : vertex_layout_.Get())
  );
  // the extended pixel shaders are the dimension-aware ones: they sample a stage's 2D texture, cube or volume through
  // the instruction its dims name. the original textured blobs declare a Texture2D unconditionally, so a stage that
  // samples anything else takes the extended path even when it asks for no fog, alpha test or second stage
  const bool extra = draw.fog_enabled || draw.alpha_test || draw.spec_add ||
                     (sampling && (sampling->stage1 || sampling->transform0 || sampling->dims[0][0] != 0 ||
                                   sampling->dims[1][0] != 0));
  const bool spec = draw.spec_add;
  context->PSSetShader(
      sampling ? (extra ? spec ? (draw.flat_shaded ? extended_spec_flat_textured_pixel_shader_.Get()
                                                   : extended_spec_textured_pixel_shader_.Get())
                                 : (draw.uv_pair
                                        ? (draw.flat_shaded ? extended_flat_textured_uv1_pixel_shader_.Get()
                                                            : extended_textured_uv1_pixel_shader_.Get())
                                        : (draw.flat_shaded ? extended_flat_textured_pixel_shader_.Get()
                                                            : extended_textured_pixel_shader_.Get()))
                        : (draw.flat_shaded ? flat_textured_pixel_shader_.Get() : textured_pixel_shader_.Get()))
               : (extra ? spec ? (draw.flat_shaded ? extended_spec_flat_pixel_shader_.Get()
                                                   : extended_spec_pixel_shader_.Get())
                                : (draw.flat_shaded ? extended_flat_pixel_shader_.Get()
                                                    : extended_pixel_shader_.Get())
                        : (draw.flat_shaded ? flat_pixel_shader_.Get() : pixel_shader_.Get())),
      nullptr, 0
  );

  if (sampling || extra) {
    StageConstantsExtended constants{};
    fill_stage_constants(constants, draw, sampling);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(stage_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(stage_.Get(), 0);
    ID3D11Buffer *stage = stage_.Get();
    context->PSSetConstantBuffers(0, 1, &stage);
  }

  if (sampling) {
    // each stage's resource at the slot its dimension declares: 2D at t0/t1, a cube at t8/t9 and a volume at t16/t17.
    // a stage with no texture bound samples the device's white 2D texel (dims 0), so it is placed at t0/t1
    ID3D11ShaderResourceView *views[18] = {};
    const UINT stages_bound = extra ? 2 : 1;
    for (UINT stage = 0; stage < stages_bound; stage++) {
      const UINT32 dimension = sampling->dims[stage][0];
      ID3D11ShaderResourceView *view = stage == 0 ? sampling->texture : sampling->texture1;
      const UINT slot = dimension == 1 ? 8u : dimension == 2 ? 16u : 0u;
      views[slot + stage] = view ? view : (dimension == 0 ? white_view_.Get() : nullptr);
      if (!views[slot + stage])
        return D3DERR_INVALIDCALL;
    }
    if (!sampling->sampler)
      return D3DERR_INVALIDCALL;
    ID3D11SamplerState *samplers[2] = {sampling->sampler, sampling->sampler1 ? sampling->sampler1 : sampling->sampler};
    context->PSSetShaderResources(0, 18, views);
    context->PSSetSamplers(0, stages_bound, samplers);
  } else {
    // an untextured draw samples nothing: the texture a draw before it sampled is not left bound to this one
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (blended) {
    float payload[20 * 4] = {};
    if (draw.blend_worlds)
      memcpy(payload, draw.blend_worlds, 64 * sizeof(float));
    if (draw.view_projection)
      memcpy(payload + 64, draw.view_projection, 16 * sizeof(float));
    if (FAILED(context->Map(blend_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, payload, sizeof(payload));
    context->Unmap(blend_constants_.Get(), 0);
    ID3D11Buffer *constants = blend_constants_.Get();
    context->VSSetConstantBuffers(0, 1, &constants);
    return S_OK;
  }
  if (transformed) {
    // the clip position is the vertex times the device's matrices: the divide by w and the viewport's mapping are the
    // rasterizer's, from the viewport bound above (Direct3D's fixed-function transform, Microsoft Learn, "Transforms")
    if (FAILED(context->Map(transform_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, world_view_projection, 16 * sizeof(float));
    context->Unmap(transform_.Get(), 0);
    ID3D11Buffer *transform = transform_.Get();
    context->VSSetConstantBuffers(0, 1, &transform);
    if (clipped) {
      if (FAILED(context->Map(clip_planes_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return D3DERR_INVALIDCALL;
      memcpy(mapped.pData, draw.clip_planes, 6 * 4 * sizeof(float));
      context->Unmap(clip_planes_.Get(), 0);
      ID3D11Buffer *planes = clip_planes_.Get();
      context->VSSetConstantBuffers(2, 1, &planes);
    }
    if (lit) {
      // the material, the lights and the lighting states the vertex's colour is computed from, at b1
      if (FAILED(context->Map(lighting_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return D3DERR_INVALIDCALL;
      memcpy(mapped.pData, lighting, sizeof(LightConstants));
      context->Unmap(lighting_.Get(), 0);
      ID3D11Buffer *lights = lighting_.Get();
      context->VSSetConstantBuffers(1, 1, &lights);
    }
    return S_OK;
  }
  // a vertex already in screen space is mapped through the viewport by the shader, since it is past the divide and the
  // rasterizer would otherwise map it a second time
  if (FAILED(context->Map(viewport_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  const float constant[4] = {(float)draw.viewport.X, (float)draw.viewport.Y, (float)draw.viewport.Width,
                             (float)draw.viewport.Height};
  memcpy(mapped.pData, constant, sizeof(constant));
  context->Unmap(viewport_.Get(), 0);
  ID3D11Buffer *buffer = viewport_.Get();
  context->VSSetConstantBuffers(0, 1, &buffer);
  return S_OK;
}

HRESULT
FixedFunctionPipeline::bind_eight(ID3D11DeviceContext *context, const FixedFunctionDraw &draw) {
  const FixedFunctionEightStage *stages = draw.eight_stage;
  if (!stages || !stage8_)
    return D3DERR_INVALIDCALL;
  if (!eight_pixel_[0][0][0] || !eight_pixel_[1][1][1] || !eight_pre_spec_[0] || !eight_pre_spec_[1] ||
      !eight_transform_spec_[0] || !eight_transform_spec_[1] || !eight_pre_spec_layout_[0] ||
      !eight_pre_spec_layout_[1] || !eight_transform_spec_layout_[0] || !eight_transform_spec_layout_[1] ||
      !eight_clipped_transform_spec_[0] || !eight_clipped_transform_spec_[1] || !eight_clipped_transform_[0] ||
      !eight_clipped_transform_[1] || !eight_lit_spec_[0] || !eight_lit_spec_[1] || !eight_clipped_lit_spec_[0] ||
      !eight_clipped_lit_spec_[1] || !eight_pre_textured_ || !eight_transform_textured_ || !eight_transform_u1_ ||
      !eight_transform_u1_layout_ ||
      !lit_vertex_layout_ || !lit_textured_vertex_layout_ ||
      !transformed_vertex_layout_ || !transformed_textured_vertex_layout_ || !vertex_layout_ ||
      !textured_vertex_layout_ || !lit_vertex_shader_ || !transformed_vertex_shader_ || !vertex_shader_ ||
      !transform_ || !lighting_ || !clip_planes_ || !viewport_)
    return D3DERR_INVALIDCALL;

  const bool textured = stages->textured;
  const bool lit = stages->lit;
  const bool transformed = draw.world_view_projection != nullptr;
  const bool clipped = transformed && draw.clip_planes;
  // the TEXCOORD0 register this eight-stage family writes: every unlit vertex shader writes it at 2, textured or not
  // (the untextured ones write D3D9's default so a game pixel shader bound over the draw still links); transformed
  // specular and lit reserve 2 for COLOR1 and write it at 3
  texcoord_slot_ = 3;
  if (!lit && !(stages->vertex_specular && transformed))
    texcoord_slot_ = 2;
  const D3D11_VIEWPORT d3d11_viewport{(float)draw.viewport.X, (float)draw.viewport.Y, (float)draw.viewport.Width,
                                      (float)draw.viewport.Height, draw.viewport.MinZ, draw.viewport.MaxZ};
  context->RSSetViewports(1, &d3d11_viewport);
  const D3D11_RECT scissor{draw.clip.left, draw.clip.top, draw.clip.right, draw.clip.bottom};
  context->RSSetScissorRects(1, &scissor);

  // the vertex shader its FVF's family and specular data set, and the layout that pairs it: the lit one carries the
  // lighting's specular on COLOR1 itself, the pre-transformed and transformed ones read D3DFVF_SPECULAR's DWORD when
  // the format has it (which no two-stage family does, so a spec draw's shader is one of the ext family's own
  // SPECULAR builds, matching the COLOR1 layout the stages' inputs read), and the clipped variants read the clip planes
  const bool spec_vs = stages->vertex_specular;
  // a transformed volume draw whose declaration carries three texture components reads the volume shader, which keeps
  // the third component and fills w with one; the two-component eight-stage shader drops them, turning a texture matrix
  // whose last row is nonzero into zero (Wine visual.c texture_transform_flags_test's volume quads)
  const bool volume3d = transformed && !lit && !spec_vs && !clipped && stages->textured &&
                        stages->board.dims[0][0] == 2 && (((draw.fvf >> 16) & 3) == 1);
  // a one-component transformed coordinate is the U1 shader's (u, 1, 0, 0), not the U2 family's (u, v, 1, 0)
  const bool float1_uv = transformed && !lit && !spec_vs && !clipped && stages->textured &&
                         (((draw.fvf >> 16) & 3) == 3);
  // a coordinate three or four components wide has to reach the pixel shader whole: the two-component shaders fill z
  // and w with one, which makes a D3DTTFF_PROJECTED stage divide by 1 instead of by the vertex's z or w. The wide
  // family's shaders pass the declared components through, and their layouts declare the coordinate at the width the
  // FVF names. The colour the FVF carries picks which of the family's three shaders is the one.
  const UINT coordinate_width = draw.fvf ? texcoord_components(draw.fvf) : 0;
  const bool wide_uv = transformed && !lit && !clipped && textured && coordinate_width >= 3 && !volume3d && !float1_uv;
  const UINT wide_colour = spec_vs ? 2u : draw.diffuse ? 1u : 0u;
  if (wide_uv && (!wide_vertex_shader_[transformed ? 0 : 1][wide_colour][coordinate_width - 3] ||
                  !wide_vertex_layout_[transformed ? 0 : 1][wide_colour][coordinate_width - 3]))
    return D3DERR_INVALIDCALL;
  if (lit)
    context->VSSetShader(
        clipped ? (textured ? eight_clipped_lit_spec_[1].Get() : eight_clipped_lit_spec_[0].Get())
                : (textured ? eight_lit_spec_[1].Get() : eight_lit_spec_[0].Get()),
        nullptr, 0
    );
  else if (volume3d)
    context->VSSetShader(
        draw.diffuse ? volume_vertex_shader_.Get() : volume_no_diffuse_vertex_shader_.Get(), nullptr, 0
    );
  else if (float1_uv)
    context->VSSetShader(eight_transform_u1_.Get(), nullptr, 0);
  else if (wide_uv)
    context->VSSetShader(
        wide_vertex_shader_[transformed ? 0 : 1][wide_colour][coordinate_width - 3].Get(), nullptr, 0
    );
  else if (transformed)
    context->VSSetShader(
        clipped ? (spec_vs ? (textured ? eight_clipped_transform_spec_[1].Get() : eight_clipped_transform_spec_[0].Get())
                          : (textured ? eight_clipped_transform_[1].Get() : eight_clipped_transform_[0].Get()))
                : spec_vs ? (textured ? eight_transform_spec_[1].Get() : eight_transform_spec_[0].Get())
                            : (textured ? eight_transform_textured_.Get() : transformed_vertex_shader_.Get()),
        nullptr, 0
    );
  else
    context->VSSetShader(
        spec_vs ? (textured ? eight_pre_spec_[1].Get() : eight_pre_spec_[0].Get())
                : (textured ? eight_pre_textured_.Get() : vertex_shader_.Get()),
        nullptr, 0
    );
  // the D3DFVF_SPECULAR formats' COLOR1 layouts, and the same ones the lit and non-specular families already strip
  const bool spec_fvf = stages->vertex_specular && !lit;
  if (spec_fvf)
    context->IASetInputLayout(
        transformed ? (textured ? eight_transform_spec_layout_[1].Get() : eight_transform_spec_layout_[0].Get())
                    : (textured ? eight_pre_spec_layout_[1].Get() : eight_pre_spec_layout_[0].Get())
    );
  else if (volume3d)
    context->IASetInputLayout(
        draw.diffuse ? volume_diffuse_textured_vertex_layout_.Get() : volume_textured_vertex_layout_.Get()
    );
  else if (float1_uv)
    context->IASetInputLayout(eight_transform_u1_layout_.Get());
  else if (wide_uv)
    context->IASetInputLayout(wide_vertex_layout_[transformed ? 0 : 1][wide_colour][coordinate_width - 3].Get());
  else
    context->IASetInputLayout(
        lit ? (textured ? lit_textured_vertex_layout_.Get() : lit_vertex_layout_.Get())
            : transformed
                ? (textured ? transformed_textured_vertex_layout_.Get() : transformed_vertex_layout_.Get())
                : (textured ? textured_vertex_layout_.Get() : vertex_layout_.Get())
    );

  // the pixel shader its COLOR1 source picks: the vertex shaders of a lit draw and of a D3DFVF_SPECULAR format carry
  // it, the others leave it absent, which is D3D9's zero for an argument the vertex does not define
  const bool spec_combo = lit || stages->vertex_specular;
  context->PSSetShader(
      eight_pixel_[draw.flat_shaded ? 1 : 0][spec_combo ? 1 : 0][textured ? 1 : 0].Get(), nullptr, 0
  );

  // the stages' board at the pixel shader's b0
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(stage8_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  memcpy(mapped.pData, &stages->board, sizeof(FixedFunctionSampling8));
  context->Unmap(stage8_.Get(), 0);
  ID3D11Buffer *stage8 = stage8_.Get();
  context->PSSetConstantBuffers(0, 1, &stage8);

  // the eight stages' views and samplers every combination reads: each stage's resource sits at the slot its
  // dimension declares (2D at t0..t7, cube at t8..t15, volume at t16..t23), and a stage with no texture bound gets the
  // white texel at its 2D slot (dims 0)
  ID3D11ShaderResourceView *views[24] = {};
  ID3D11SamplerState *samplers[8];
  for (UINT stage = 0; stage < 8; stage++) {
    const UINT32 dimension = stages->board.dims[stage][0];
    ID3D11ShaderResourceView *view = stages->views[stage];
    const UINT slot = dimension == 1 ? 8u : dimension == 2 ? 16u : 0u;
    if (!view) {
      if (dimension != 0)
        return D3DERR_INVALIDCALL;
      view = white_view_.Get();
    }
    views[slot + stage] = view;
    samplers[stage] = stages->samplers[stage];
    if (!views[slot + stage] || !samplers[stage])
      return D3DERR_INVALIDCALL;
  }
  context->PSSetShaderResources(0, 24, views);
  context->PSSetSamplers(0, 8, samplers);

  // the vertex shaders' own constants: the composed matrix a transformed vertex reads, the lighting's, and the planes
  if (transformed) {
    if (FAILED(context->Map(transform_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, draw.world_view_projection, 16 * sizeof(float));
    context->Unmap(transform_.Get(), 0);
    ID3D11Buffer *transform = transform_.Get();
    context->VSSetConstantBuffers(0, 1, &transform);
    if (clipped) {
      if (FAILED(context->Map(clip_planes_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return D3DERR_INVALIDCALL;
      memcpy(mapped.pData, draw.clip_planes, 6 * 4 * sizeof(float));
      context->Unmap(clip_planes_.Get(), 0);
      ID3D11Buffer *planes = clip_planes_.Get();
      context->VSSetConstantBuffers(2, 1, &planes);
    }
    if (lit) {
      if (FAILED(context->Map(lighting_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return D3DERR_INVALIDCALL;
      memcpy(mapped.pData, draw.lighting, sizeof(LightConstants));
      context->Unmap(lighting_.Get(), 0);
      ID3D11Buffer *lights = lighting_.Get();
      context->VSSetConstantBuffers(1, 1, &lights);
    }
    return S_OK;
  }
  // a vertex already in screen space is mapped through the viewport by its shader
  if (FAILED(context->Map(viewport_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  const float constant[4] = {(float)draw.viewport.X, (float)draw.viewport.Y, (float)draw.viewport.Width,
                             (float)draw.viewport.Height};
  memcpy(mapped.pData, constant, sizeof(constant));
  context->Unmap(viewport_.Get(), 0);
  ID3D11Buffer *viewport_buffer = viewport_.Get();
  context->VSSetConstantBuffers(0, 1, &viewport_buffer);
  return S_OK;
}

} // namespace mullion::d3d9
