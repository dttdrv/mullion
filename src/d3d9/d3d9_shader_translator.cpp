// the d3dbc translator: a game's shader program, read as Direct3D 9's tokens and written as the shader model 5 program
// this build's D3D11 device compiles (d3d9_dxbc.hpp). the instructions this slice reads are the arithmetic and the
// matrix multiplies a vertex shader transforms with, and the same arithmetic for a pixel shader's colour, texture
// sampling (including a vertex shader's texldl and a pixel shader of model 1.x, whose r0 is the colour and whose `tex`
// samples the stage of that register), and the flow control the caps name. an instruction this slice does not read is
// refused with the reason the log carries, rather than translated into something the game did not write.
#include "d3d9_shader_translator.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <memory>

namespace mullion::d3d9 {

// the DXBC token format's names, which its header keeps in a namespace of its own
using namespace microsoft;

namespace {

constexpr UINT kFloatConstants = 256;
constexpr UINT kIntConstants = 16;
constexpr UINT kBoolConstants = 16;
// the constant buffers a D3D11 shader reads its D3D9 tables from: the float registers, the integer ones and the booleans
constexpr UINT kFloatSlot = 0;
constexpr UINT kIntSlot = 1;
constexpr UINT kBoolSlot = 2;
// and the one the alpha test's reference is read from, which a pixel program with the test carries (D3DRS_ALPHAREF): one
// register, the reference as a float in 0 to 255 (D3D9 compares an alpha in those units)
constexpr UINT kAlphaSlot = 3;
// a vertex program's user clip planes, in clip space: six registers at b3, which only a pixel program's alpha test uses
// otherwise
constexpr UINT kClipSlot = 3;
// a draw whose vertex stage reads D3D9's transformed (POSITIONT) vertices takes the screen-space position the
// declaration carries, which the conversion maps through the viewport with (b4: x, y, width, height)
constexpr UINT kViewportSlot = 4;
constexpr UINT kClipPlanes = 6;
// the texture stages, and so the samplers, a pixel shader's program may sample (D3D9's eight: the device's caps say
// MaxSimultaneousTextures is eight)
// ps_3_0's sixteen texture samplers (s0-s15), which is the caps' MaxSimultaneousTextures; a ps_2_0 program reads the
// first sixteen sampler registers too, with eight texture coordinates
constexpr UINT kSamplers = 16;
// a vs_3_0 vertex shader samples through four vertex texture samplers of its own (D3DVERTEXTEXTURESAMPLER0..3, the API
// stages 257..260, which the device binds at its s0..s3). that is not the pixel shader's MaxSimultaneousTextures count:
// a vertex shader's sampler registers stop at s3 whatever the pixel stages hold
constexpr UINT kVertexSamplers = 4;

// the SM5 output slots a D3D9 vertex shader's outputs are written as: position first, then fog and point size, the
// colours and the texture coordinates
// the registers a vertex shader's outputs are written to, and a pixel shader's inputs read from: the two stages link by
// register (airconv names a varying after its register and component), so an output this build writes has to be the
// register the pixel shader reads it at, which is what `output_slot` asks the pixel shader's signature for. the
// position is register 0, as every compiled shader has it, and the pixel shader's inputs follow it: the two colours,
// then the texture coordinates. vs_2_0 has oT0-oT7. airconv sizes max_output_register from the DXBC signature (no
// 8-register cap in dxbc_signature.cpp); this packing is position + two colours + eight texcoords
constexpr UINT kSlotPosition = 0;
constexpr UINT kSlotColour = 1;
constexpr UINT kSlotTexcoord = 3;
constexpr UINT kTexcoordSlots = 8;
// oFog / FOG interpolator after the eight texcoords (position + two colours + t0-t7)
constexpr UINT kSlotFog = 11;
// ps_3_0's inputs are declared by the usage their dcl names: whatever the declaration carries but the two colours and
// the texture coordinates (blendweight, normal, fog, depth, tangent, binormal, ...) reads the interpolator whose
// semantic that usage spells, which starts past the fixed-function slots and is allocated per translation
constexpr UINT kSlotDeclaration = 12;
constexpr UINT kMaxOutputRegisters = 24;
// fog parameters share the alpha-test constant buffer (b3): c0 = alpha ref, c1 = start/end/density, c2 = fog colour
constexpr UINT kFogParamsReg = 1;
constexpr UINT kFogColorReg = 2;
// log2(e), so EXP (base-2) yields e^x for D3D9's EXP/EXP2 fog
constexpr float kLog2e = 1.4426950408889634f;

// a source modifier as d3d9types.h packs it (D3DSPSM_*, already shifted down here)
enum SourceModifier {
  kModNone = 0,
  kModNegate = 1,
  kModBias = 2,
  kModBiasNegate = 3,
  kModSign = 4,
  kModSignNegate = 5,
  kModComplement = 6,
  kModX2 = 7,
  kModX2Negate = 8,
  kModDz = 9,
  kModDw = 10,
  kModAbs = 11,
  kModAbsNegate = 12,
  kModNot = 13,
};

struct Param {
  UINT regtype = 0;
  UINT regnum = 0;
  DWORD mask = 0xF;      // a destination's write mask
  DWORD swizzle = 0xE4;  // a source's swizzle
  DWORD modifier = 0;
  DWORD shift = 0;
  bool relative = false; // D3DSHADER_ADDRMODE_RELATIVE: c[a0.x + regnum]
  UINT address_reg = 0;
};

// the register a token names: d3dbc keeps the type's low three bits above the register number and the bits the types
// past the seventh need (color and depth outputs, the integer and boolean constant tables) in bits 11 and 12, which is
// why a token read with the first field alone says a game's oC0 is r0 with no components written
UINT register_type(DWORD token) {
  return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) | ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}

Param destination(DWORD token) {
  Param p;
  p.regnum = token & D3DSP_REGNUM_MASK;
  p.regtype = register_type(token);
  p.mask = (token & D3DSP_WRITEMASK_ALL) >> 16;
  p.modifier = (token & D3DSP_DSTMOD_MASK) >> D3DSP_DSTMOD_SHIFT;
  p.shift = (token & D3DSP_DSTSHIFT_MASK) >> D3DSP_DSTSHIFT_SHIFT;
  return p;
}

Param source(DWORD token) {
  Param p;
  p.regnum = token & D3DSP_REGNUM_MASK;
  p.regtype = register_type(token);
  p.swizzle = (token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
  p.modifier = (token & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT;
  p.relative = (token & D3DSHADER_ADDRESSMODE_MASK) == D3DSHADER_ADDRMODE_RELATIVE;
  return p;
}

// a source token and, when it names a relative address, the register it addresses. the token form is the shader
// model's: vs_1_x addresses c[a0.x + regnum] inline, a0 implicit with no token of its own; vs_2_0 and ps_3_0 carry
// the address register in the token that follows (wined3d's shader_sm1.c shader_get_param; Microsoft Learn,
// relative addressing). a shader model 1 pixel shader has no relative addressing, so the version alone decides
bool take_source(const DWORD *params, UINT &index, UINT length, Param &out, UINT major) {
  if (index >= length)
    return false;
  out = source(params[index++]);
  if (out.relative) {
    if (major < 2) {
      out.address_reg = 0; // a0.x, the one address register a shader model 1 program addresses through
      return true;
    }
    if (index >= length)
      return false;
    Param addr = source(params[index++]);
    out.address_reg = addr.regnum;
  }
  return true;
}

// shader model 1 has no length in the instruction token (that field is shader model 2's). the count is the opcode's
// own, and a ps_1_4 `texld`/`texcrd` takes the coordinate as a second parameter where ps_1_1's `tex`/`texcoord` does not
// (Microsoft Learn, tex - ps, texld - ps_1_4)
UINT sm1_token_count(UINT opcode, UINT minor) {
  switch (opcode) {
  case D3DSIO_NOP:
  case D3DSIO_PHASE:
    return 0;
  case D3DSIO_MOV:
  case D3DSIO_RCP:
  case D3DSIO_RSQ:
  case D3DSIO_EXP:
  case D3DSIO_LOG:
  case D3DSIO_EXPP:
  case D3DSIO_LOGP:
  case D3DSIO_FRC:
  case D3DSIO_ABS:
  case D3DSIO_MOVA:
  case D3DSIO_DSX:
  case D3DSIO_DSY:
    return 2;
  case D3DSIO_ADD:
  case D3DSIO_SUB:
  case D3DSIO_MUL:
  case D3DSIO_DP3:
  case D3DSIO_DP4:
  case D3DSIO_MIN:
  case D3DSIO_MAX:
  case D3DSIO_SLT:
  case D3DSIO_SGE:
  case D3DSIO_DST:
  case D3DSIO_CRS:
  case D3DSIO_POW:
  case D3DSIO_NRM:
    return 3;
  case D3DSIO_MAD:
  case D3DSIO_LRP:
  case D3DSIO_CND:
  case D3DSIO_CMP:
  case D3DSIO_DP2ADD:
    return 4;
  case D3DSIO_SINCOS:
    return 4;
  case D3DSIO_DEF:
    return 5;
  case D3DSIO_DCL:
    // vs_1_1 / ps_1_x: dcl_usage + register (Wine device.c test_null_stream)
    return 2;
  case D3DSIO_TEXCOORD:
  case D3DSIO_TEX:
    return minor >= 4 ? 2 : 1;
  case D3DSIO_BEM:
    return 3;
  case D3DSIO_TEXKILL:
  case D3DSIO_TEXBEM:
  case D3DSIO_TEXBEML:
  case D3DSIO_TEXREG2AR:
  case D3DSIO_TEXREG2GB:
  case D3DSIO_TEXREG2RGB:
  case D3DSIO_TEXM3x2PAD:
  case D3DSIO_TEXM3x2TEX:
  case D3DSIO_TEXM3x3PAD:
  case D3DSIO_TEXM3x3TEX:
  case D3DSIO_TEXM3x3DIFF:
  case D3DSIO_TEXM3x3VSPEC:
  case D3DSIO_TEXDP3TEX:
  case D3DSIO_TEXDP3:
  case D3DSIO_TEXM3x2DEPTH:
  case D3DSIO_TEXM3x3:
  case D3DSIO_TEXDEPTH:
    return 1;
  case D3DSIO_TEXM3x3SPEC:
    return 2;
  default:
    return 0;
  }
}

const char *usage_name_of(UINT usage) {
  switch (usage) {
  case D3DDECLUSAGE_POSITION:
    return "POSITION";
  case D3DDECLUSAGE_BLENDWEIGHT:
    return "BLENDWEIGHT";
  case D3DDECLUSAGE_BLENDINDICES:
    return "BLENDINDICES";
  case D3DDECLUSAGE_NORMAL:
    return "NORMAL";
  case D3DDECLUSAGE_PSIZE:
    return "PSIZE";
  case D3DDECLUSAGE_TEXCOORD:
    return "TEXCOORD";
  case D3DDECLUSAGE_TANGENT:
    return "TANGENT";
  case D3DDECLUSAGE_BINORMAL:
    return "BINORMAL";
  case D3DDECLUSAGE_TESSFACTOR:
    return "TESSFACTOR";
  case D3DDECLUSAGE_POSITIONT:
    return "POSITIONT";
  case D3DDECLUSAGE_COLOR:
    return "COLOR";
  case D3DDECLUSAGE_FOG:
    return "FOG";
  case D3DDECLUSAGE_DEPTH:
    return "DEPTH";
  case D3DDECLUSAGE_SAMPLE:
    return "SAMPLE";
  default:
    return "TEXCOORD";
  }
}

// the components an element of `type` carries, and 0 for a type this build has no input layout format for
UINT mask_of_type(UINT type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return 0x1;
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_FLOAT16_2:
    return 0x3;
  case D3DDECLTYPE_FLOAT3:
    return 0x7;
  case D3DDECLTYPE_FLOAT4:
  case D3DDECLTYPE_D3DCOLOR:
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
  case D3DDECLTYPE_FLOAT16_4:
    return 0xF;
  default:
    return 0;
  }
}

// the class that translates one program
class Translator {
public:
  Translator(
      const ShaderCode &code, const AlphaTest *alpha, const FogState *fog, const TexProjectState *project,
      const BumpEnvState *bump, bool clip_planes, UINT texcoord_slot, bool flat, UINT shadow_mask
  )
      : code_(code), alpha_(alpha), fog_(fog), project_(project), bump_(bump),
        clip_planes_(clip_planes && !code.pixel), texcoord_slot_(texcoord_slot), flat_(flat),
        shadow_mask_(code.pixel ? shadow_mask : 0) {}

  bool run(
      const std::vector<VertexInput> *vertex_inputs, const std::vector<dxbc::SignatureElement> *pixel_inputs,
      TranslatedProgram &out
  ) {
    vertex_inputs_ = vertex_inputs;
    pixel_inputs_of_stage_ = pixel_inputs;
    // the version: what the first token names, and the stages this slice reads. a pixel shader of model 1.x is read
    // here too: its colour is r0, and `tex tn` samples stage n (Microsoft Learn, ps_1_1, tex - ps)
    ps1_ = code_.pixel && code_.major < 2;
    // a vertex program whose declaration carries D3D9's transformed position (an FVF's D3DFVF_XYZRHW is
    // D3DDECLUSAGE_POSITIONT) reads screen-space vertices, which the position write maps through the viewport
    transformed_input_ = vertex_inputs &&
                         std::any_of(vertex_inputs->begin(), vertex_inputs->end(), [](const VertexInput &input) {
                           return input.usage == D3DDECLUSAGE_POSITIONT;
                         });
    tex_temp_.fill(UINT_MAX);
    if (code_.pixel) {
      if (code_.major < 1 || code_.major > 3 || (code_.major == 1 && (code_.minor < 1 || code_.minor > 4))) {
        reason_ = "a pixel shader of a shader model this build does not translate";
        return false;
      }
    } else {
      if (code_.major > 3) {
        reason_ = "a vertex shader of a shader model past 3.0 is not translated";
        return false;
      }
      if (!vertex_inputs_) {
        reason_ = "a vertex shader needs the vertex declaration it draws with, and the draw has none";
        return false;
      }
    }

    // D3D9's alpha test is a pixel shader's: its comparison is written into the program's end (emit_body's epilogue), and
    // a comparison that always passes (D3DCMP_ALWAYS) writes nothing
    alpha_test_ = code_.pixel && alpha_ && alpha_->enabled;
    alpha_compare_ = alpha_test_ && alpha_->func != D3DCMP_ALWAYS;
    fog_enabled_ = code_.pixel && fog_ && fog_->enabled();
    project_div_ = code_.pixel && project_ && project_->divide_component >= 1 && project_->divide_component <= 3
                       ? project_->divide_component
                       : 0;
    force_alpha_one_ = code_.pixel && project_ && project_->force_alpha_one;

    writer_.reset(new dxbc::Writer(code_.pixel ? dxbc::Stage::Pixel : dxbc::Stage::Vertex));
    if (!scan())
      return false;
    if (fog_enabled_) {
      // table/vertex-Z fog need depth; fog-coord needs the FOG interpolator a foggy VS writes; Full needs neither
      if (fog_->source == FogSource::Table || fog_->source == FogSource::VertexZ) {
        if (!register_input(kSlotPosition, "SV_Position", 0)) {
          reason_ = "fog needs SV_Position and that interpolator is already taken";
          return false;
        }
        pixel_inputs_[kSlotPosition].system_value = D3D_NAME_POSITION;
        fog_position_reg_ = kSlotPosition;
      }
      if (fog_->source == FogSource::FogCoord) {
        if (!register_input(kSlotFog, "FOG", 0)) {
          reason_ = "fog needs the FOG interpolator and that register is already taken";
          return false;
        }
        fog_coord_reg_ = kSlotFog;
      }
    }
    if (!declare())
      return false;
    if (!emit_body())
      return false;

    out.dxbc = writer_->container();
    out.inputs = inputs_;
    out.outputs = outputs_;
    out.writes_fog = writes_fog_;
    out.samples = sampled_ != 0;
    return true;
  }

  // what a refused translation says about the program
  const std::string &reason() const { return reason_; }

private:
  // ---- the first pass: what the program declares, defines and uses -----------------------------------------------
  bool scan() {
    const DWORD *tokens = code_.tokens.data();
    const UINT count = (UINT)code_.tokens.size();
    for (UINT at = 1; at < count;) {
      const DWORD head = tokens[at];
      const UINT opcode = head & 0xFFFF;
      if (opcode == D3DSIO_END)
        break;
      if (opcode == D3DSIO_COMMENT) {
        at += 1 + ((head >> 16) & 0x7FFF);
        continue;
      }
      const UINT length = instruction_length(head);
      if (length > count - at - 1) {
        reason_ = "an instruction of the program runs past its end";
        return false;
      }
      const DWORD *params = tokens + at + 1;
      if (opcode == D3DSIO_DCL) {
        if (length < 2) {
          reason_ = "a dcl without a register";
          return false;
        }
        if (!scan_declaration(params[0], params[1]))
          return false;
      } else if (opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB) {
        if (!scan_definition(opcode, params, length))
          return false;
      } else {
        if (!scan_registers(opcode, params, length))
          return false;
      }
      at += 1 + length;
    }
    // a ps_1.x texture register that `tex` or `texcoord` writes is a temporary from then on, claimed after every other
    // temporary the program names so the two do not share a register
    if (ps1_) {
      for (UINT reg = 0; reg < kSamplers; reg++) {
        if (!(ps1_tex_written_ & (1u << reg)))
          continue;
        tex_temp_[reg] = max_temp_ + 1;
        max_temp_ = tex_temp_[reg];
      }
      uses_colour_out_[0] = true;
    }
    return true;
  }

  // the parameter count of one instruction. shader model 2 stores it in the token; shader model 1 does not, so a token
  // whose length field is 0 is the opcode's own count (a hand-written stream that does store the count is read as that)
  UINT instruction_length(DWORD head) const {
    const UINT opcode = head & 0xFFFF;
    if (opcode == D3DSIO_COMMENT)
      return (head >> 16) & 0x7FFF;
    const UINT encoded = (head >> 24) & 0xF;
    // SM1 vertex and pixel shaders omit the length nibble; only SM2+ stores it (vs_1_1 dcl is two params).
    if (code_.major < 2 && encoded == 0 && opcode != D3DSIO_END)
      return sm1_token_count(opcode, code_.minor);
    return encoded;
  }

  bool scan_declaration(DWORD usage_token, DWORD register_token) {
    const Param register_param = destination(register_token);
    // `dcl_2d s#` (and cube and volume): a vertex shader's texture fetch declares the same sampler a pixel shader does
    // (Microsoft Learn, texldl - vs). the stage number is both the texture and the sampler
    if (register_param.regtype == D3DSPR_SAMPLER) {
      const UINT dimension = usage_token & D3DSP_TEXTURETYPE_MASK;
      if (dimension != D3DSTT_2D && dimension != D3DSTT_CUBE && dimension != D3DSTT_VOLUME) {
        reason_ = "a sampler of a texture dimension this build does not sample yet (dcl_2d, dcl_cube and dcl_volume "
                  "are the ones it reads).";
        return false;
      }
      const UINT sampler_limit = code_.pixel ? kSamplers : kVertexSamplers;
      if (register_param.regnum >= sampler_limit) {
        reason_ = code_.pixel ? "a sampler past s" + std::to_string(kSamplers - 1) + ": this device has " +
                                    std::to_string(kSamplers) + " texture stages (MaxSimultaneousTextures)"
                              : "a vertex sampler past s" + std::to_string(kVertexSamplers - 1) +
                                    ": a vs_3_0 samples through four vertex texture samplers";
        return false;
      }
      declared_samplers_ |= 1u << register_param.regnum;
      if (dimension == D3DSTT_CUBE)
        sampler_cube_ |= 1u << register_param.regnum;
      if (dimension == D3DSTT_VOLUME)
        sampler_volume_ |= 1u << register_param.regnum;
      return true;
    }
    if (!code_.pixel) {
      // a vertex shader declares its inputs (`dcl_usage v#`) and, in shader model 3, its outputs (`dcl_usage o#`)
      const UINT usage = usage_token & 0xF;
      const UINT usage_index = (usage_token >> 16) & 0xF;
      if (register_param.regtype == D3DSPR_OUTPUT) {
        // one call: an output of a usage the pixel stage reads none of claims a free slot as a side effect, so a
        // second call would claim a second slot and a third would store a third (the declarations and the moves
        // would land apart and the interpolators would run out)
        const UINT slot = output_slot(usage, usage_index, register_param.regnum);
        if (slot == UINT_MAX) {
          reason_ = "a vertex shader output this build has no slot for";
          return false;
        }
        vs_output_declared_[register_param.regnum] = slot;
        vs_output_usage_[register_param.regnum].usage = usage;
        vs_output_usage_[register_param.regnum].usage_index = usage_index;
      } else if (register_param.regtype == D3DSPR_INPUT) {
        // a declared input is drawn from what the declaration supplies at its register: a dcl whose usage the
        // declaration names no element for (an FVF's D3DFVF_XYZRHW is D3DDECLUSAGE_POSITIONT, which no vertex
        // shader's POSITION dcl reads) cannot be fed, so the draw it carries is refused
        const VertexInput *supplied = nullptr;
        for (const auto &input : *vertex_inputs_)
          if (input.reg == register_param.regnum) {
            supplied = &input;
            break;
          }
        if (!supplied || supplied->usage != usage || supplied->usage_index != usage_index) {
          reason_ = "a dcl of an input the declaration does not supply (its position element is not that usage)";
          return false;
        }
      }
      return true;
    }
    // a pixel shader declares its inputs: a colour register or a texture coordinate one. ps_2_0 names the texture
    // coordinates in the texture register file (`dcl t0`), but ps_3_0 puts both in the input register file
    // (`dcl_color0 v0`, `dcl_texcoord0 v0`) and tells them apart by the declaration's usage, not its register number;
    // ps_3_0 may also name any other usage the vertex declaration carries (`dcl_blendweight0 v0`), which reads the
    // interpolator whose semantic that usage spells (Microsoft Learn, "Declaration Types and Usage Flags"; Wine
    // visual.c pretransformed_varying_test)
    if (register_param.regtype == D3DSPR_INPUT) {
      const UINT usage = usage_token & 0xF;
      const UINT usage_index = (usage_token >> 16) & 0xF;
      UINT slot;
      // ps_3_0's dcl names the usage the interpolator carries (dcl_blendweight0 v0 reads the declaration's
      // BLENDWEIGHT). a ps_2_0's dcl of an input register does not: its v# file is the colours, whatever usage
      // token the dcl names (Wine fog_with_shader_test's ps_2_0 dcl v0 reads oD0)
      if (code_.major == 3 && usage == D3DDECLUSAGE_TEXCOORD) {
        if (usage_index >= kTexcoordSlots) {
          reason_ = "a pixel shader texture coordinate past " + std::to_string(kTexcoordSlots) + " stages";
          return false;
        }
        slot = texcoord_slot_ + usage_index;
      } else if (code_.major == 3 && usage == D3DDECLUSAGE_COLOR) {
        // a pixel shader reads the diffuse and specular colours the declaration supplies, at the usage index the dcl
        // named, not the register it reads (Wine's own dcl_color1 v0 reads COLOR1)
        if (usage_index > 1) {
          reason_ = "a pixel shader colour input past COLOR1, which a vertex declaration supplies two of";
          return false;
        }
        slot = kSlotColour + usage_index;
      } else if (code_.major == 3) {
        slot = declaration_input_slot(usage, usage_index);
        if (slot == UINT_MAX) {
          reason_ = "a pixel shader input at a register past the interpolators this build packs";
          return false;
        }
      } else {
        // a ps_2_0's v# is the colour the register names (the coordinates live in the texture register file)
        slot = kSlotColour + std::min<UINT>(register_param.regnum, 1);
      }
      if (!register_input(slot, usage == D3DDECLUSAGE_COLOR ? "COLOR" : usage_name_of(usage),
                          code_.major == 3 ? usage_index : register_param.regnum)) {
        reason_ = "a pixel shader input this build has no place for";
        return false;
      }
      declared_pixel_input_[register_param.regnum] = slot;
      if (usage == D3DDECLUSAGE_TEXCOORD)
        declared_texcoord_input_[register_param.regnum] = slot;
    } else if (register_param.regtype == D3DSPR_TEXTURE) {
      if (!register_input(texcoord_slot_ + register_param.regnum, "TEXCOORD", register_param.regnum)) {
        reason_ = "a pixel shader texture coordinate register this build has no place for";
        return false;
      }
    }
    return true;
  }

  bool scan_definition(UINT opcode, const DWORD *params, UINT length) {
    if (opcode == D3DSIO_DEF && length >= 5) {
      const Param register_param = destination(params[0]);
      if (register_param.regtype != D3DSPR_CONST || register_param.regnum >= kFloatConstants) {
        reason_ = "a def of a register that is not a float constant";
        return false;
      }
      memcpy(defined_[register_param.regnum], params + 1, sizeof(float) * 4);
      defined_valid_[register_param.regnum] = true;
      return true;
    }
    if (opcode == D3DSIO_DEFI && length >= 5) {
      const Param register_param = destination(params[0]);
      if (register_param.regtype != D3DSPR_CONSTINT || register_param.regnum >= kIntConstants) {
        reason_ = "a defi of a register that is not an integer constant";
        return false;
      }
      memcpy(defined_int_[register_param.regnum], params + 1, sizeof(int) * 4);
      defined_int_valid_[register_param.regnum] = true;
      return true;
    }
    if (opcode == D3DSIO_DEFB && length >= 2) {
      const Param register_param = destination(params[0]);
      if (register_param.regtype != D3DSPR_CONSTBOOL || register_param.regnum >= kBoolConstants) {
        reason_ = "a defb of a register that is not a boolean constant";
        return false;
      }
      defined_bool_[register_param.regnum] = params[1] != 0;
      defined_bool_valid_[register_param.regnum] = true;
      return true;
    }
    return true;
  }

  bool scan_registers(UINT opcode, const DWORD *params, UINT length) {
    switch (opcode) {
    case D3DSIO_ELSE:
    case D3DSIO_ENDIF:
    case D3DSIO_ENDLOOP:
    case D3DSIO_ENDREP:
    case D3DSIO_BREAK:
    case D3DSIO_RET:
    case D3DSIO_NOP:
    // ps_1_4's phase takes no parameters and marks the boundary between the texture and colour phases (Microsoft
    // Learn, phase - ps). A program that has one is valid, and texdepth must follow it.
    case D3DSIO_PHASE:
    // a definition's parameters are its destination and its immediate values, which scan_definition reads; they are
    // not registers of the program
    case D3DSIO_DEF:
    case D3DSIO_DEFI:
    case D3DSIO_DEFB:
      return true;
    default:
      break;
    }
    if (length < 1) {
      reason_ = "an instruction with no parameters";
      return false;
    }
    // the destination: every instruction but the declarations and the branches writes one
    switch (opcode) {
    case D3DSIO_IF:
    case D3DSIO_IFC:
    case D3DSIO_ELSE:
    case D3DSIO_ENDIF:
    case D3DSIO_LOOP:
    case D3DSIO_ENDLOOP:
    case D3DSIO_BREAK:
    case D3DSIO_BREAKC:
    case D3DSIO_CALL:
    case D3DSIO_CALLNZ:
    case D3DSIO_LABEL:
    case D3DSIO_RET:
    case D3DSIO_REP:
    case D3DSIO_ENDREP:
    case D3DSIO_NOP:
      break;
    default: {
      if (length < 1)
        break;
      const Param dest_param = destination(params[0]);
      if (dest_param.regtype == D3DSPR_TEMP)
        max_temp_ = std::max(max_temp_, dest_param.regnum);
      if (!code_.pixel && (dest_param.regtype == D3DSPR_RASTOUT || dest_param.regtype == D3DSPR_ATTROUT ||
                           dest_param.regtype == D3DSPR_TEXCRDOUT || dest_param.regtype == D3DSPR_OUTPUT)) {
        if (!note_vertex_output(dest_param)) {
          reason_ = "a vertex shader output register this build has no slot for";
          return false;
        }
      }
      if (code_.pixel && dest_param.regtype == D3DSPR_COLOROUT && dest_param.regnum < 4)
        uses_colour_out_[dest_param.regnum] = true;
      // a texkill reads the register its token names (D3D9 ignores the write mask it carries), so a pixel shader that
      // kills on one of its inputs reads that input
      if (code_.pixel && opcode == D3DSIO_TEXKILL) {
        if (dest_param.regtype == D3DSPR_TEXTURE &&
            !register_input(texcoord_slot_ + dest_param.regnum, "TEXCOORD", dest_param.regnum)) {
          reason_ = "a texkill of a texture coordinate register this build has no place for";
          return false;
        }
        if (dest_param.regtype == D3DSPR_INPUT && code_.major < 3 && dest_param.regnum > 1) {
          reason_ = "a texkill of a pixel shader colour input past v1 is not D3D9's, and this build refuses it";
          return false;
        }
        if (dest_param.regtype == D3DSPR_INPUT && !declared_pixel_input_.count(dest_param.regnum) &&
            !declared_texcoord_input_.count(dest_param.regnum) &&
            !register_input(kSlotColour + std::min<UINT>(dest_param.regnum, 1), "COLOR", dest_param.regnum)) {
          reason_ = "a texkill of a pixel shader colour input this build has no place for";
          return false;
        }
      }
      // a texld samples the sampler its third parameter names, which is the stage whose texture it reads. a ps_1.x
      // `tex tn` names no sampler: the stage is the destination register (Microsoft Learn, tex - ps). a vertex shader's
      // texldl names one the same way a pixel shader's does (Microsoft Learn, texldl - vs)
      // texbem and texbeml may carry the source whose red and green the bump matrix multiplies (texbem t1, t0), which is
      // a second parameter; a ps_1_4 texld is TEX with three and names its sampler in the third, which the sampler branch
      // below reads instead
      const bool bump_texture_op = opcode == D3DSIO_TEXBEM || opcode == D3DSIO_TEXBEML;
      if (ps1_ &&
          (opcode == D3DSIO_TEX || opcode == D3DSIO_TEXCOORD || bump_texture_op ||
           opcode == D3DSIO_TEXM3x2TEX || opcode == D3DSIO_TEXM3x3TEX || opcode == D3DSIO_TEXDP3TEX) &&
          length >= 1 && length < 3) {
        const UINT stage = dest_param.regnum;
        // a bump op samples its own stage's interpolator; the source token is the bump offset's source, not a coordinate
        const UINT coord = bump_texture_op ? stage : (length >= 2 ? source(params[1]).regnum : stage);
        if (stage >= kSamplers || coord >= kSamplers) {
          reason_ = "a ps_1.x texture register past the stages this device has";
          return false;
        }
        ps1_tex_written_ |= 1u << stage;
        if (!register_input(texcoord_slot_ + coord, "TEXCOORD", coord)) {
          reason_ = "a ps_1.x texture coordinate this build has no place for";
          return false;
        }
        if (opcode != D3DSIO_TEXCOORD) {
          sampled_ |= 1u << stage;
          declared_samplers_ |= 1u << stage;
        }
      } else if (opcode == D3DSIO_TEX || opcode == D3DSIO_TEXLDL || opcode == D3DSIO_TEXLDD) {
        if (length < 3 || source(params[2]).regtype != D3DSPR_SAMPLER) {
          reason_ = "a texld without a sampler (the shader model 2 and 3 form names one: texld r#, t#, s#)";
          return false;
        }
        const UINT sampler_limit = code_.pixel ? kSamplers : kVertexSamplers;
        if (source(params[2]).regnum >= sampler_limit) {
          reason_ = code_.pixel ? "a texld of a sampler past s" + std::to_string(kSamplers - 1) + ": this device has " +
                                      std::to_string(kSamplers) + " texture stages (MaxSimultaneousTextures)"
                                : "a vertex texldl of a sampler past s" + std::to_string(kVertexSamplers - 1) +
                                      ": a vs_3_0 samples through four vertex texture samplers";
          return false;
        }
        sampled_ |= 1u << source(params[2]).regnum;
      }
      if (!code_.pixel && dest_param.regtype == D3DSPR_ADDR)
        uses_a0_ = true;
      break;
    }
    }
    // the sources: every parameter after the destination, except for the branches, whose first token is a source
    UINT at = 1;
    switch (opcode) {
    case D3DSIO_IF:
    case D3DSIO_IFC:
    case D3DSIO_CALL:
    case D3DSIO_CALLNZ:
    case D3DSIO_REP:
    case D3DSIO_LOOP:
    case D3DSIO_BREAKC:
      at = 0;
      break;
    default:
      break;
    }
    while (at < length) {
      Param src;
      if (!take_source(params, at, length, src, code_.major)) {
        reason_ = "a relative address without an address register";
        return false;
      }
      if (src.relative)
        uses_a0_ = true;
      if (src.regtype == D3DSPR_TEMP)
        max_temp_ = std::max(max_temp_, src.regnum);
      if (src.regtype == D3DSPR_CONST2 || src.regtype == D3DSPR_CONST3 || src.regtype == D3DSPR_CONST4) {
        if (!src.relative) {
          reason_ = "a constant bank past c255 is not this device's MaxVertexShaderConst";
          return false;
        }
      }
      if (!code_.pixel && src.regtype == D3DSPR_ADDR)
        uses_a0_ = true;
      if (code_.pixel && src.regtype == D3DSPR_INPUT && code_.major < 3 && src.regnum > 1) {
        reason_ = "a pixel shader colour input past v1 is not D3D9's, and this build refuses it";
        return false;
      }
      if (code_.pixel && src.regtype == D3DSPR_TEXTURE) {
        if (!register_input(texcoord_slot_ + src.regnum, "TEXCOORD", src.regnum)) {
          reason_ = "a pixel shader texture coordinate register this build has no place for";
          return false;
        }
      }
      if (code_.pixel && src.regtype == D3DSPR_INPUT && !declared_pixel_input_.count(src.regnum) &&
          !declared_texcoord_input_.count(src.regnum) &&
          !register_input(kSlotColour + std::min<UINT>(src.regnum, 1), "COLOR", src.regnum)) {
        reason_ = "a pixel shader colour input this build has no place for";
        return false;
      }
    }
    return true;
  }

  // remembers an input register the pixel shader reads and gives it its semantic
  bool register_input(UINT sm5_reg, const char *semantic, UINT semantic_index) {
    if (sm5_reg >= kMaxOutputRegisters) {
      reason_ = "a pixel shader input at a register past the interpolators this build packs";
      return false;
    }
    if (pixel_inputs_.count(sm5_reg))
      return true;
    dxbc::SignatureElement element;
    element.semantic = semantic;
    element.index = semantic_index;
    element.reg = sm5_reg;
    element.mask = 0xF;
    pixel_inputs_[sm5_reg] = element;
    return true;
  }

  // the interpolator a pixel shader's input register reads: an SM3 dcl decided it when it was scanned (any usage, at
  // the register the dcl named); a ps_1.x/2_0 register reads the coordinate slot that declaration named, and every
  // other input register a colour
  UINT pixel_input_slot(UINT regnum) const {
    const auto declared = declared_pixel_input_.find(regnum);
    if (declared != declared_pixel_input_.end())
      return declared->second;
    const auto texcoord = declared_texcoord_input_.find(regnum);
    return texcoord != declared_texcoord_input_.end() ? texcoord->second : kSlotColour + std::min<UINT>(regnum, 1);
  }

  // the slot a ps_3_0 input of a usage outside the two colours and the texture coordinates reads: the semantic that
  // usage spells, at the slot this translation allocated it first (the same semantic reads the same interpolator,
  // whatever registers the program names it at)
  UINT declaration_input_slot(UINT usage, UINT usage_index) {
    const UINT key = usage | (usage_index << 16);
    const auto allocated = declaration_input_slots_.find(key);
    if (allocated != declaration_input_slots_.end())
      return allocated->second;
    while (next_declaration_slot_ < kMaxOutputRegisters &&
           pixel_inputs_.count(next_declaration_slot_))
      next_declaration_slot_++;
    if (next_declaration_slot_ >= kMaxOutputRegisters)
      return UINT_MAX;
    declaration_input_slots_[key] = next_declaration_slot_;
    return next_declaration_slot_++;
  }

  // the register a vertex shader writes an output of `name` and `index` to: the one the pixel shader it draws with
  // reads it at, and `fallback` when that shader reads none of it
  UINT read_slot(const char *name, UINT index, UINT fallback) {    if (pixel_inputs_of_stage_) {
      for (const auto &element : *pixel_inputs_of_stage_) {
        if (element.semantic == name && element.index == index)
          return element.reg;
      }
    }
    return fallback;
  }

  UINT output_slot(UINT usage, UINT usage_index, UINT regnum) {
    switch (usage) {
    case D3DDECLUSAGE_POSITION:
    case D3DDECLUSAGE_POSITIONT:
      return read_slot("SV_Position", 0, kSlotPosition);
    case D3DDECLUSAGE_COLOR:
      if (usage_index > 1)
        return UINT_MAX;
      return read_slot("COLOR", usage_index, kSlotColour + usage_index);
    case D3DDECLUSAGE_TEXCOORD:
      if (usage_index >= kTexcoordSlots)
        return UINT_MAX;
      return read_slot("TEXCOORD", usage_index, texcoord_slot_ + usage_index);
    case D3DDECLUSAGE_FOG:
      // oFog is a scalar interpolator the pixel fog epilogue reads when FOGTABLEMODE is NONE (Wine fog_with_shader_test)
      return read_slot("FOG", 0, kSlotFog);
    case D3DDECLUSAGE_PSIZE:
      // point size is not a Metal vertex output this build makes yet
      reason_ = "the point-size output is not translated yet";
      return UINT_MAX;
    default: {
      // a vs_3_0 may write any usage a vertex declaration carries as an output: the link is the semantic its dcl
      // named (Microsoft Learn, "Declaration Types and Usage Flags"), which the pixel stage reads where it declared
      // the same semantic. one it reads none of is still written, at a free slot, so a program that writes what its
      // pixel shader ignores still draws. the claim is recorded here, at the dcl that made it: every output dcl is
      // scanned before any instruction is written, so two dcls of different usages would otherwise be given the same
      // free slot and the last one written would decide the semantic
      if (usage <= D3DDECLUSAGE_SAMPLE) {
        const UINT slot = read_slot(usage_name_of(usage), usage_index, UINT_MAX);
        if (slot != UINT_MAX) {
          claimed_outputs_.insert(slot);
          return slot;
        }
        const UINT free = free_output_slot();
        if (free != UINT_MAX)
          claimed_outputs_.insert(free);
        return free;
      }
      (void)regnum;
      return UINT_MAX;
    }
    }
  }

  // the lowest output register neither the pixel stage's inputs nor a written output holds: the slot an output of a
  // usage the pixel shader reads none of is written at, so the program still draws what it writes
  UINT free_output_slot() {
    for (UINT slot = kSlotDeclaration; slot < kMaxOutputRegisters; slot++) {
      if (pixel_inputs_of_stage_ &&
          std::any_of(pixel_inputs_of_stage_->begin(), pixel_inputs_of_stage_->end(),
                      [slot](const dxbc::SignatureElement &element) { return element.reg == slot; }))
        continue;
      if (written_outputs_.count(slot) || claimed_outputs_.count(slot))
        continue;
      return slot;
    }
    reason_ = "a vertex shader output at a register past the interpolators this build packs";
    return UINT_MAX;
  }

  // a source's register read at another swizzle: the conversion reads the position's components one at a time, and
  // the translation may hold the input in a shadow temporary it already padded
  std::vector<DWORD> swizzled_source(const std::vector<DWORD> &source, DWORD swizzle) const {
    if (source.empty())
      return source;
    const DWORD type = DECODE_D3D10_SB_OPERAND_TYPE(source[0]);
    if (type == D3D10_SB_OPERAND_TYPE_INPUT)
      return dxbc::src_input(source[1], swizzle);
    if (type == D3D10_SB_OPERAND_TYPE_TEMP)
      return dxbc::src_temp(source[1], swizzle);
    return source;
  }

  // D3D9's rasterizer takes screen-space positions: a vertex the declaration carries as POSITIONT is already in the
  // viewport's pixels, with rhw in its w, and the position a pixel stage is fed is the viewport mapping of it, as the
  // fixed-function pre-transformed vertex shader writes it (fixed_function_shaders.cpp):
  //   w = 1 / rhw; out = ((x - vp.x) * 2 / vw - 1) * w, (1 - (y - vp.y) * 2 / vh) * w, z * w, w
  // the viewport is b4's x, y, width, height (the draw binds it). the temporaries are the ones one instruction
  // borrows, which no instruction's own expansion is using once the source operand is built
  void emit_position_conversion(const std::vector<DWORD> &dest, const std::vector<DWORD> &first) {
    const UINT hold = mod_base_, scratch = mod_base_ + 1, factor = mod_base_ + 2, out = mod_base_ + 3;
    const auto vp = [&](DWORD swizzle) { return dxbc::src_constant(kViewportSlot, 0, swizzle); };
    auto negated = [](std::vector<DWORD> source) {
      negate_operand(source);
      return source;
    };
    // w = 1 / rhw, held in all four of hold's components
    writer_->instruction(D3D11_SB_OPCODE_RCP, false,
                         {dxbc::dest_temp(scratch, 0x1), swizzled_source(first, 0xFF)});
    writer_->mov(dxbc::dest_temp(hold, 0xF), dxbc::src_temp(scratch, 0x00));
    // 1/vw and 1/vh, then 2/vw and 2/vh
    writer_->instruction(D3D11_SB_OPCODE_RCP, false, {dxbc::dest_temp(scratch, 0x1), vp(0xAA)});
    writer_->instruction(D3D10_SB_OPCODE_MAD, false,
                         {dxbc::dest_temp(scratch, 0x1), dxbc::src_temp(scratch, 0x00),
                          dxbc::src_immediate_value(2.0f), dxbc::src_immediate_value(0.0f)});
    writer_->mov(dxbc::dest_temp(factor, 0x1), dxbc::src_temp(scratch, 0x00)); // 2/vw
    writer_->instruction(D3D11_SB_OPCODE_RCP, false, {dxbc::dest_temp(scratch, 0x1), vp(0xFF)});
    writer_->instruction(D3D10_SB_OPCODE_MAD, false,
                         {dxbc::dest_temp(scratch, 0x1), dxbc::src_temp(scratch, 0x00),
                          dxbc::src_immediate_value(2.0f), dxbc::src_immediate_value(0.0f)});
    writer_->mov(dxbc::dest_temp(factor, 0x2), dxbc::src_temp(scratch, 0x00)); // 2/vh
    // x: ((x - vp.x) * 2/vw) * w - w
    writer_->instruction(D3D10_SB_OPCODE_ADD, false,
                         {dxbc::dest_temp(out, 0x1), swizzled_source(first, 0x00), negated(vp(0x00))});
    writer_->instruction(D3D10_SB_OPCODE_MUL, false,
                         {dxbc::dest_temp(out, 0x1), dxbc::src_temp(out, 0x00),
                          dxbc::src_temp(factor, 0x00)});
    writer_->instruction(D3D10_SB_OPCODE_MAD, false,
                         {dxbc::dest_temp(out, 0x1), dxbc::src_temp(out, 0x00), dxbc::src_temp(hold, 0x00),
                          negated(dxbc::src_temp(hold, 0x00))});
    // y: w - ((y - vp.y) * 2/vh) * w
    writer_->instruction(D3D10_SB_OPCODE_ADD, false,
                         {dxbc::dest_temp(out, 0x2), swizzled_source(first, 0x55), negated(vp(0x55))});
    writer_->instruction(D3D10_SB_OPCODE_MUL, false,
                         {dxbc::dest_temp(out, 0x2), dxbc::src_temp(out, 0x55),
                          dxbc::src_temp(factor, 0x55)});
    writer_->instruction(D3D10_SB_OPCODE_MUL, false,
                         {dxbc::dest_temp(out, 0x2), dxbc::src_temp(out, 0x55), dxbc::src_temp(hold, 0x55)});
    writer_->instruction(D3D10_SB_OPCODE_ADD, false,
                         {dxbc::dest_temp(out, 0x2), dxbc::src_temp(hold, 0x55),
                          negated(dxbc::src_temp(out, 0x55))});
    // z: z * w, and w: w
    writer_->instruction(D3D10_SB_OPCODE_MUL, false,
                         {dxbc::dest_temp(out, 0x4), swizzled_source(first, 0xAA), dxbc::src_temp(hold, 0xAA)});
    writer_->mov(dxbc::dest_temp(out, 0x8), dxbc::src_temp(hold, 0xAA));
    writer_->mov(dest, dxbc::src_temp(out, 0xE4));
  }

  // the slot a vertex shader's output register is written to, and whether it is one this build has
  bool note_vertex_output(const Param &dest_param) {
    UINT slot = UINT_MAX;
    UINT usage = D3DDECLUSAGE_TEXCOORD;
    UINT usage_index = 0;
    if (dest_param.regtype == D3DSPR_RASTOUT) {
      if (dest_param.regnum == 0) {
        slot = kSlotPosition;
        usage = D3DDECLUSAGE_POSITION;
      } else if (dest_param.regnum == 1) {
        usage = D3DDECLUSAGE_FOG;
        slot = output_slot(usage, 0, dest_param.regnum);
      } else if (dest_param.regnum == 2) {
        usage = D3DDECLUSAGE_PSIZE;
        slot = output_slot(usage, 0, dest_param.regnum);
      }
    } else if (dest_param.regtype == D3DSPR_ATTROUT) {
      if (dest_param.regnum < 12) {
        slot = output_slot(D3DDECLUSAGE_COLOR, dest_param.regnum, dest_param.regnum);
        usage = D3DDECLUSAGE_COLOR;
        usage_index = dest_param.regnum;
      }
    } else if (dest_param.regtype == D3DSPR_TEXCRDOUT) {
      if (code_.major == 3) {
        // in shader model 3 the output registers are declared, and the declaration named the slot
        auto declared = vs_output_declared_.find(dest_param.regnum);
        if (declared != vs_output_declared_.end()) {
          slot = declared->second;
          auto named = vs_output_usage_.find(dest_param.regnum);
          if (named != vs_output_usage_.end()) {
            usage = named->second.usage;
            usage_index = named->second.usage_index;
          }
        } else if (dest_param.regnum < 8) {
          slot = output_slot(D3DDECLUSAGE_TEXCOORD, dest_param.regnum, texcoord_slot_ + dest_param.regnum);
          usage_index = dest_param.regnum;
        }
      } else if (dest_param.regnum < 8) {
        // the register the pixel stage reads this coordinate at when it named one, and the family's own slot otherwise:
        // a game's vs_1_1 drawn over the fixed-function pixel stage has to write oT0 where that shader reads TEXCOORD0
        slot = output_slot(D3DDECLUSAGE_TEXCOORD, dest_param.regnum, texcoord_slot_ + dest_param.regnum);
        usage_index = dest_param.regnum;
      }
    } else if (dest_param.regtype == D3DSPR_OUTPUT) {
      auto declared = vs_output_declared_.find(dest_param.regnum);
      if (declared != vs_output_declared_.end()) {
        slot = declared->second;
        auto named = vs_output_usage_.find(dest_param.regnum);
        if (named != vs_output_usage_.end()) {
          usage = named->second.usage;
          usage_index = named->second.usage_index;
        }
      }
    }
    if (slot == UINT_MAX)
      return false;
    vs_output_slot_[dest_param.regtype * 256 + dest_param.regnum] = slot;
    written_outputs_[slot].usage = usage;
    written_outputs_[slot].usage_index = usage_index;
    if (usage == D3DDECLUSAGE_FOG)
      writes_fog_ = true;
    return true;
  }

  // ---- the declarations ------------------------------------------------------------------------------------------
  // ps_1_4's texdepth writes the pixel's depth instead of a colour, so its program declares an SV_Depth output. The
  // scan only records colour outputs, so the program is walked for the instruction here (its tokens are already
  // validated by scan(), and a texdepth is not one of the instructions that changes the parse)
  bool program_writes_depth() const {
    const DWORD *tokens = code_.tokens.data();
    const UINT count = (UINT)code_.tokens.size();
    for (UINT at = 1; at < count;) {
      const DWORD head = tokens[at];
      const UINT opcode = head & 0xFFFF;
      if (opcode == D3DSIO_END)
        break;
      if (opcode == D3DSIO_COMMENT) {
        at += 1 + ((head >> 16) & 0x7FFF);
        continue;
      }
      if (opcode == D3DSIO_TEXDEPTH)
        return true;
      at += 1 + instruction_length(head);
    }
    return false;
  }

  bool declare() {
    // the outputs whose unwritten components take one of D3D9's defaults: a register a program writes in part keeps
    // the default in the components it does not write, and a varying the pixel shader reads that this program never
    // writes is synthesized into the signature with it
    std::vector<std::pair<UINT, std::array<float, 4>>> output_defaults;
    // the signatures
    if (code_.pixel) {
      for (const auto &entry : pixel_inputs_) {
        inputs_.push_back(entry.second);
      }
      // each colour output the program writes is a target; a program that writes none still links against the first,
      // as D3D9's pixel shader does
      bool any = false;
      for (UINT at = 0; at < 4; at++) {
        if (!uses_colour_out_[at])
          continue;
        any = true;
        dxbc::SignatureElement element;
        element.semantic = "SV_Target";
        element.index = at;
        element.system_value = D3D_NAME_TARGET;
        element.reg = at;
        element.mask = 0xF;
        outputs_.push_back(element);
      }
      if (!any) {
        dxbc::SignatureElement element;
        element.semantic = "SV_Target";
        element.index = 0;
        element.system_value = D3D_NAME_TARGET;
        element.reg = 0;
        element.mask = 0xF;
        outputs_.push_back(element);
      }
      // ps_1_4 texdepth writes the pixel's depth: the depth output sits past the four colour targets
      if (program_writes_depth()) {
        dxbc::SignatureElement element;
        element.semantic = "SV_Depth";
        element.index = 0;
        element.system_value = D3D_NAME_DEPTH;
        element.reg = 0;
        element.mask = 0x1;
        outputs_.push_back(element);
      }
    } else {
      for (UINT at = 0; at < vertex_inputs_->size(); at++) {
        const VertexInput &input = (*vertex_inputs_)[at];
        dxbc::SignatureElement element;
        element.semantic = usage_name_of(input.usage);
        element.index = input.usage_index;
        element.reg = at;
        // the components the vertex carries: the input signature declares exactly the ones the shader reads, as
        // Microsoft's compiler does for the same declaration, and the ones the vertex does not carry are completed in
        // a copy the translation makes (below)
        element.mask = input.mask;
        inputs_.push_back(element);
      }
      // the position is what a vertex shader has to write, whatever else it writes
      written_outputs_[kSlotPosition].usage = D3DDECLUSAGE_POSITION;
      written_outputs_[kSlotPosition].usage_index = 0;
      for (const auto &entry : written_outputs_) {
        const UINT slot = entry.first;
        dxbc::SignatureElement element;
        element.reg = slot;
        element.mask = 0xF;
        if (entry.second.usage == D3DDECLUSAGE_POSITION) {
          // the rasteriser reads the position from the system value, whatever the program's own usage token said
          element.semantic = "SV_Position";
          element.index = 0;
          element.system_value = D3D_NAME_POSITION;
        } else {
          // every other output is a value the pixel shader finds under the semantic the vertex shader declared it with
          element.semantic = usage_name_of(entry.second.usage);
          element.index = entry.second.usage_index;
        }
        outputs_.push_back(element);
      }
      // a partially written register's unwritten components read as D3D9's default: the diffuse of a shader-model 1
      // or 2 program is white, and every other register starts transparent black with w one (Wine visual.c
      // test_uninitialized_varyings records the same values). a vs_3_0 declared output starts zeroed instead, which is
      // the "partially initialized varying" the same test expects (o2.x written reads 0.5, 0, 0, 0)
      auto written_default = [this](const OutputUsage &usage) -> std::array<float, 4> {
        if (usage.usage == D3DDECLUSAGE_COLOR && usage.usage_index == 0 && code_.major < 3)
          return {1.0f, 1.0f, 1.0f, 1.0f};
        if (usage.usage == D3DDECLUSAGE_COLOR && code_.major >= 3)
          return {0.0f, 0.0f, 0.0f, 0.0f};
        return {0.0f, 0.0f, 0.0f, 1.0f};
      };
      for (const auto &entry : written_outputs_) {
        if (entry.second.usage == D3DDECLUSAGE_POSITION || entry.second.usage == D3DDECLUSAGE_POSITIONT)
          continue;
        output_defaults.emplace_back(entry.first, written_default(entry.second));
      }
      // every varying the pixel stage reads has to be in this signature, even when the program writes none of it:
      // otherwise the two signatures disagree, the pipeline link fails and the draw leaves the target untouched
      // instead of giving the pixel shader the default D3D9 defines for the uninitialized varying
      if (pixel_inputs_of_stage_) {
        std::map<UINT, bool> synthesized;
        // a signature holds one entry per semantic and index: a varying the program already writes is the one the
        // pixel stage reads, whatever register this build would have put a synthesized one at (a game's vs_1_1 writes
        // oT0 at its own slot, and asking for a TEXCOORD0 at another register must not add a second one)
        auto already_declared = [this](const dxbc::SignatureElement &element) {
          for (const auto &one : outputs_) {
            if (one.index == element.index && one.semantic == element.semantic)
              return true;
          }
          return false;
        };
        for (const auto &element : *pixel_inputs_of_stage_) {
          if (element.system_value == D3D_NAME_POSITION || written_outputs_.count(element.reg) ||
              synthesized.count(element.reg) || already_declared(element))
            continue;
          dxbc::SignatureElement added = element;
          added.mask = 0xF;
          outputs_.push_back(added);
          synthesized[element.reg] = true;
          // the diffuse colour D3D9 lights with is white; a varying the declaration supplies keeps the register
          // default's alpha one, and a usage the declaration carries no element of reads zero all across (Wine
          // pretransformed_varying_test's texcoord1, whose alpha the declaration has no element for, reads 0)
          const bool diffuse = element.semantic == "COLOR" && element.index == 0;
          bool supplied = !vertex_inputs_;
          if (vertex_inputs_) {
            for (const auto &input : *vertex_inputs_) {
              if (usage_name_of(input.usage) == element.semantic && input.usage_index == element.index) {
                supplied = true;
                break;
              }
            }
          }
          output_defaults.emplace_back(
              element.reg, diffuse ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                                   : std::array<float, 4>{0.0f, 0.0f, 0.0f, supplied ? 1.0f : 0.0f}
          );
        }
      }
      // the six clip distances, three to a register, after every output the pixel shader reads
      if (clip_planes_) {
        clip_slot_ = written_outputs_.empty() ? 1 : written_outputs_.rbegin()->first + 1;
        for (UINT half = 0; half < 2; half++) {
          dxbc::SignatureElement element;
          element.semantic = "SV_ClipDistance";
          element.index = half;
          element.reg = clip_slot_ + half;
          element.mask = 0x7;
          element.system_value = D3D_NAME_CLIP_DISTANCE;
          outputs_.push_back(element);
        }
      }
    }

    // the signature's registers have to be nondecreasing: the reader that turns this container into a shader refuses
    // one that is not (DXBC's own rule). A varying the pixel stage reads but the program never writes is synthesized
    // after the written ones, and its register can be below theirs (a vs_1_1 that writes oT0 but not oD0, drawn over
    // the fixed-function stage, synthesizes COLOR0 at output 1 after TEXCOORD0 at output 3), so the outputs are put in
    // register order before they are written.
    std::stable_sort(outputs_.begin(), outputs_.end(),
                     [](const dxbc::SignatureElement &a, const dxbc::SignatureElement &b) { return a.reg < b.reg; });

    for (const auto &element : inputs_)
      writer_->input(element);
    for (const auto &element : outputs_)
      writer_->output(element);

    // the D3D9 constant tables, as the three constant buffers a draw binds
    writer_->declare_constant_buffer(kFloatSlot, kFloatConstants);
    writer_->declare_constant_buffer(kIntSlot, kIntConstants);
    writer_->declare_constant_buffer(kBoolSlot, kBoolConstants);
    // the viewport a transformed vertex's position is mapped through
    if (transformed_input_)
      writer_->declare_constant_buffer(kViewportSlot, 1);

    // what a texture sample reads: for each stage it samples, the sampler the program declared (`dcl_2d s#`) and the
    // texture that stage holds. a program that samples without declaring one is a program D3D9 refuses, so this refuses
    // to translate it
    if (sampled_ & ~declared_samplers_) {
      reason_ = "a texld of a sampler the program does not declare (a pixel shader declares dcl_2d s# first)";
      return false;
    }
    const UINT sampler_limit = code_.pixel ? kSamplers : kVertexSamplers;
    for (UINT stage = 0; stage < sampler_limit; stage++) {
      if (!(sampled_ & (1u << stage)))
        continue;
      writer_->declare_sampler(stage);
      if (sampler_cube_ & (1u << stage))
        writer_->declare_texturecube(stage);
      else if (sampler_volume_ & (1u << stage))
        writer_->declare_texture3d(stage);
      else
        writer_->declare_texture2d(stage);
    }

    // the temporary registers the program reads and writes, and two the translation of one instruction into several
    // uses (a matrix multiply writes components in turn, and a comparison is followed by a mask)
    scratch_ = max_temp_ + 1;

    // a vertex shader's input register is four components whatever the declaration carries, and the components the
    // vertex does not carry read as D3D9 says they do: one for w, zero for the rest. the input assemblies of this
    // build fill an input the declaration leaves out with zero, so each such register is read through a copy of it that
    // this translation makes and completes (source_operand)
    input_shadow_.assign(inputs_.size(), UINT_MAX);
    if (!code_.pixel) {
      for (UINT at = 0; at < (UINT)inputs_.size(); at++) {
        if ((*vertex_inputs_)[at].mask != 0xF)
          input_shadow_[at] = scratch_++;
      }
    }

    // a pixel program that carries D3D9's alpha test writes its first target to a copy of its own: the test compares the
    // alpha the program ends with, and a shader model 5 output register is written, not read, so the value is held here
    // until the program's end has tested it (emit_body)
    colour_shadow_ = UINT_MAX;
    // a ps_1.x program's colour is r0, so the alpha test and fog read r0 and the epilogue copies it to oC0. a later
    // model writes oC0, which a shader model 5 output cannot be read back from, so that colour is held in its own
    // temporary
    if (ps1_) {
      if (alpha_compare_ || fog_enabled_)
        colour_shadow_ = 0;
    } else if ((alpha_test_ || fog_enabled_) && uses_colour_out_[0]) {
      colour_shadow_ = scratch_++;
    }
    if (uses_a0_)
      a0_temp_ = scratch_++;
    // the comparison sample of a depth texture (ShadowState): one temporary holds the reference, another the sample
    if (code_.pixel && shadow_mask_) {
      shadow_temp_ = scratch_++;
      shadow_ref_temp_ = scratch_++;
    }
    loop_temps_base_ = scratch_;
    scratch_ += 8;
    if (alpha_compare_ || fog_enabled_)
      writer_->declare_constant_buffer(kAlphaSlot, fog_enabled_ ? 3 : 1);
    if (clip_planes_) {
      position_shadow_ = scratch_++;
      writer_->declare_constant_buffer(kClipSlot, kClipPlanes);
    }

    // every declaration before the first instruction that uses what it declares, as a program's container has them.
    // the four past scratch_ are the source-modifier temporaries one instruction borrows (bias, sign, complement, x2)
    mod_base_ = scratch_ + 1;
    writer_->temps(mod_base_ + 10);
    for (const auto &element : inputs_) {
      if (code_.pixel && element.system_value == D3D_NAME_POSITION)
        writer_->declare_input_ps_siv(element.reg, D3D_NAME_POSITION, element.mask);
      else if (code_.pixel && flat_ && element.semantic == "COLOR")
        // D3DSHADE_FLAT: the colour reaches every pixel of the triangle from its first vertex, which is the provoking
        // vertex D3D11's constant interpolation reads (Wine visual.c test_shademode's ps_1_1 and ps_2_0 cases)
        writer_->declare_input(element.reg, element.mask, D3D10_SB_INTERPOLATION_CONSTANT);
      else
        writer_->declare_input(element.reg, element.mask);
    }
    for (const auto &element : outputs_) {
      // a vertex shader's position is a system value, and the program's own declarations have to say so (a Metal vertex
      // function's `[[position]]` comes from this declaration, not from the signature chunk). a pixel shader's targets
      // are plain outputs, as a compiler makes them
      if (element.system_value == D3D_NAME_DEPTH)
        writer_->declare_output_depth();
      else if (element.system_value != 0 && element.system_value != D3D_NAME_TARGET)
        // the position and the clip distances are system values a vertex shader declares; a pixel shader's targets
        // are plain outputs
        writer_->declare_output_siv(element.reg, element.system_value, element.mask);
      else
        writer_->declare_output(element.reg, element.mask);
    }

    // and then the prologue's own instructions
    for (UINT at = 0; at < (UINT)input_shadow_.size(); at++) {
      const UINT shadow = input_shadow_[at];
      if (shadow == UINT_MAX)
        continue;
      const UINT declared = (*vertex_inputs_)[at].mask;
      // the components the declaration leaves out read as the first one (a component the shader did not declare is not
      // one it may swizzle to), and are then filled with D3D9's value for them
      UINT swizzle = 0;
      for (UINT component = 0; component < 4; component++)
        swizzle |= ((declared & (1 << component)) ? component : 0) << (2 * component);
      writer_->mov(dxbc::dest_temp(shadow, declared), dxbc::src_input(at, swizzle));
      for (UINT component = 0; component < 4; component++) {
        if (declared & (1 << component))
          continue;
        writer_->mov(
            dxbc::dest_temp(shadow, 1 << component),
            dxbc::src_immediate_value(component == 3 ? 1.0f : 0.0f)
        );
      }
    }
    // and the defaults the outputs start with, before the program writes any component of them
    for (const auto &entry : output_defaults) {
      writer_->mov(
          dxbc::dest_output(entry.first, 0xF),
          dxbc::src_immediate(entry.second[0], entry.second[1], entry.second[2], entry.second[3])
      );
    }
    return true;
  }

  // ---- the body --------------------------------------------------------------------------------------------------
  bool emit_body() {
    const DWORD *tokens = code_.tokens.data();
    const UINT count = (UINT)code_.tokens.size();
    for (UINT at = 1; at < count;) {
      const DWORD head = tokens[at];
      const UINT opcode = head & 0xFFFF;
      if (opcode == D3DSIO_END)
        return finish();
      if (opcode == D3DSIO_COMMENT) {
        at += 1 + ((head >> 16) & 0x7FFF);
        continue;
      }
      const UINT length = instruction_length(head);
      mod_cursor_ = 0;
      if (!emit_instruction(head, tokens + at + 1, length))
        return false;
      if (scale_after_ && !apply_dest_scale())
        return false;
      scale_after_ = false;
      at += 1 + length;
    }
    return finish();
  }

  // the end of a pixel program that carries D3D9's alpha test: its first target's alpha, in the 0-to-255 units
  // D3DRS_ALPHAREF is, compared with the reference the draw uploads (b3), the pixel discarded when the comparison fails,
  // and the value written to the target afterwards. D3D9's comparison is on the alpha the program produced; the render
  // target's own eight-bit quantization happens after the test in D3D9 too, so the floats are what is compared, and an
  // alpha within a step of the reference is the only place that can differ from an integer comparison
  bool finish() {
    if (position_shadow_ != UINT_MAX) {
      // the position the program wrote, and each plane's distance from it: D3D9's vertex-shader clip planes are in clip
      // space, so a plane dotted with the clip position is the distance the rasterizer clips where it is negative
      writer_->mov(dxbc::dest_output(position_slot_, 0xF), dxbc::src_temp(position_shadow_, 0xE4));
      for (UINT plane = 0; plane < kClipPlanes; plane++)
        writer_->instruction(
            D3D10_SB_OPCODE_DP4, false,
            {dxbc::dest_output(clip_slot_ + plane / 3, 1u << (plane % 3)), dxbc::src_temp(position_shadow_, 0xE4),
             dxbc::src_constant(kClipSlot, plane, 0xE4)}
        );
    }
    // a ps_1.x program writes its colour to r0 and has no oC0 of its own (Microsoft Learn, ps_1_1). the epilogue copies
    // r0 to the target, after the alpha test when the draw asked for one
    if (ps1_ && colour_shadow_ == UINT_MAX) {
      writer_->mov(dxbc::dest_output(0, 0xF), dxbc::src_temp(0, 0xE4));
      return true;
    }
    if (colour_shadow_ == UINT_MAX)
      return true;
    if (alpha_compare_) {
      writer_->instruction(
          D3D10_SB_OPCODE_MUL, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(colour_shadow_, 0xFFFF), dxbc::src_immediate_value(255.0f)}
      );
      // the comparison that fails: D3D9's test passes when ALPHAFUNC compares as true, so the program discards when the
      // opposite comparison is true. shader model 5 has no greater-than or less-equal instruction (only eq, ne, lt and
      // ge), so those two are the same comparisons with the operands the other way round
      D3D10_SB_OPCODE_TYPE failure = D3D10_SB_OPCODE_EQ;
      bool swapped = false;
      switch (alpha_->func) {
      case D3DCMP_LESS:
        failure = D3D10_SB_OPCODE_GE;
        break;
      case D3DCMP_EQUAL:
        failure = D3D10_SB_OPCODE_NE;
        break;
      case D3DCMP_LESSEQUAL:
        failure = D3D10_SB_OPCODE_LT;
        swapped = true;
        break;
      case D3DCMP_GREATER:
        failure = D3D10_SB_OPCODE_GE;
        swapped = true;
        break;
      case D3DCMP_NOTEQUAL:
        failure = D3D10_SB_OPCODE_EQ;
        break;
      default: // D3DCMP_GREATEREQUAL
        failure = D3D10_SB_OPCODE_LT;
        break;
      }
      const std::vector<DWORD> reference = dxbc::src_constant(kAlphaSlot, 0, 0x0000);
      const std::vector<DWORD> alpha_value = dxbc::src_temp(scratch_, 0x0000);
      if (alpha_->func == D3DCMP_NEVER) {
        // never passes: every pixel is discarded, whatever its alpha
        writer_->discard_if_nonzero(dxbc::src_immediate_value(1.0f));
      } else {
        writer_->instruction(
            failure, false,
            {dxbc::dest_temp(scratch_, 0x2), swapped ? reference : alpha_value, swapped ? alpha_value : reference}
        );
        writer_->discard_if_nonzero(dxbc::src_temp(scratch_, 0x5555));
      }
    }
    if (fog_enabled_ && !apply_fog())
      return false;
    writer_->mov(dxbc::dest_output(0, 0xF), dxbc::src_temp(colour_shadow_, 0xE4));
    return true;
  }

  // blend fog colour into colour_shadow_.rgb (factor in scratch_.x; diff in scratch_+1)
  bool apply_fog() {
    const UINT factor_tmp = scratch_;
    const UINT work_tmp = scratch_ + 1;
    if (fog_->source == FogSource::Full) {
      writer_->mov(dxbc::dest_temp(factor_tmp, 0x1), dxbc::src_immediate_value(0.0f));
    } else if (fog_->source == FogSource::FogCoord) {
      writer_->instruction(
          D3D10_SB_OPCODE_MOV, true,
          {dxbc::dest_temp(factor_tmp, 0x1), dxbc::src_input(fog_coord_reg_, 0x0000)}
      );
    } else {
      // depth = SV_Position.z into work_tmp.x; zzzz swizzle is 0xAA
      writer_->mov(
          dxbc::dest_temp(work_tmp, 0x1), dxbc::src_input(fog_position_reg_, 0xAA)
      );
      const DWORD mode = fog_->mode;
      if (mode == D3DFOG_EXP || mode == D3DFOG_EXP2) {
        // e^(-density*d) via EXP(base2): 2^(-density*d*log2(e)); density is c1.z
        writer_->instruction(
            D3D10_SB_OPCODE_MUL, false,
            {dxbc::dest_temp(work_tmp, 0x1), dxbc::src_temp(work_tmp, 0x0000),
             dxbc::src_constant(kAlphaSlot, kFogParamsReg, 0xAAAA)}
        );
        if (mode == D3DFOG_EXP2) {
          writer_->instruction(
              D3D10_SB_OPCODE_MUL, false,
              {dxbc::dest_temp(work_tmp, 0x1), dxbc::src_temp(work_tmp, 0x0000), dxbc::src_temp(work_tmp, 0x0000)}
          );
        }
        writer_->instruction(
            D3D10_SB_OPCODE_MUL, false,
            {dxbc::dest_temp(work_tmp, 0x1), dxbc::src_temp(work_tmp, 0x0000),
             dxbc::src_immediate(kLog2e, kLog2e, kLog2e, kLog2e)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_EXP, false,
            {dxbc::dest_temp(factor_tmp, 0x1),
             dxbc::src_temp(work_tmp, 0x0000, D3D10_SB_OPERAND_MODIFIER_NEG)}
        );
      } else {
        // LINEAR: saturate((end - d) / max(end - start, 1e-5)); c1.xy = start, end
        writer_->instruction(
            D3D10_SB_OPCODE_ADD, false,
            {dxbc::dest_temp(work_tmp, 0x2), dxbc::src_constant(kAlphaSlot, kFogParamsReg, 0x5555),
             dxbc::src_temp(work_tmp, 0x0000, D3D10_SB_OPERAND_MODIFIER_NEG)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_ADD, false,
            {dxbc::dest_temp(work_tmp, 0x4), dxbc::src_constant(kAlphaSlot, kFogParamsReg, 0x5555),
             dxbc::src_constant(kAlphaSlot, kFogParamsReg, 0x0000, D3D10_SB_OPERAND_MODIFIER_NEG)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MAX, false,
            {dxbc::dest_temp(work_tmp, 0x4), dxbc::src_temp(work_tmp, 0xAAAA),
             dxbc::src_immediate(1e-5f, 1e-5f, 1e-5f, 1e-5f)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_DIV, true,
            {dxbc::dest_temp(factor_tmp, 0x1), dxbc::src_temp(work_tmp, 0x5555), dxbc::src_temp(work_tmp, 0xAAAA)}
        );
      }
    }
    // colour.rgb = lerp(fog_color, colour, factor) = fog + factor * (colour - fog)
    writer_->instruction(
        D3D10_SB_OPCODE_ADD, false,
        {dxbc::dest_temp(work_tmp, 0x7), dxbc::src_temp(colour_shadow_, 0xE4),
         dxbc::src_constant(kAlphaSlot, kFogColorReg, 0xE4, D3D10_SB_OPERAND_MODIFIER_NEG)}
    );
    writer_->instruction(
        D3D10_SB_OPCODE_MAD, false,
        {dxbc::dest_temp(colour_shadow_, 0x7), dxbc::src_temp(factor_tmp, 0x0000), dxbc::src_temp(work_tmp, 0xE4),
         dxbc::src_constant(kAlphaSlot, kFogColorReg, 0xE4)}
    );
    return true;
  }

  bool emit_instruction(DWORD head, const DWORD *params, UINT length) {
    const UINT opcode = head & 0xFFFF;
    if (opcode == D3DSIO_NOP || opcode == D3DSIO_PHASE || opcode == D3DSIO_DCL || opcode == D3DSIO_DEF ||
        opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB)
      return true;
    if (opcode == D3DSIO_ELSE) {
      writer_->else_();
      return true;
    }
    if (opcode == D3DSIO_ENDIF) {
      writer_->endif();
      return true;
    }
    if (opcode == D3DSIO_ENDLOOP || opcode == D3DSIO_ENDREP) {
      if (loop_stack_.empty()) {
        reason_ = "an endloop/endrep without a matching loop/rep";
        return false;
      }
      const UINT slot = loop_stack_.back();
      writer_->instruction(
          D3D10_SB_OPCODE_IADD, false,
          {dxbc::dest_temp(slot, 0x1), dxbc::src_temp(slot, 0x0000), dxbc::src_immediate_int(-1)}
      );
      if (opcode == D3DSIO_ENDLOOP) {
        writer_->instruction(
            D3D10_SB_OPCODE_IADD, false,
            {dxbc::dest_temp(slot, 0x2), dxbc::src_temp(slot, 0x5555), dxbc::src_temp(slot, 0xAAAA)}
        );
      }
      writer_->endloop();
      loop_stack_.pop_back();
      return true;
    }
    if (opcode == D3DSIO_BREAK) {
      writer_->break_();
      return true;
    }
    if (opcode == D3DSIO_RET) {
      writer_->instruction(D3D10_SB_OPCODE_RET, false, {});
      return true;
    }
    if (opcode == D3DSIO_LABEL) {
      if (length < 1) {
        reason_ = "a label without a number";
        return false;
      }
      writer_->label(destination(params[0]).regnum);
      return true;
    }
    if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ) {
      UINT cursor = 0;
      Param label;
      if (!take_source(params, cursor, length, label, code_.major)) {
        reason_ = "a call without a label";
        return false;
      }
      if (opcode == D3DSIO_CALLNZ) {
        Param predicate;
        if (!take_source(params, cursor, length, predicate, code_.major)) {
          reason_ = "a callnz without a predicate";
          return false;
        }
        std::vector<DWORD> pred;
        if (!source_operand(predicate, pred))
          return false;
        writer_->if_nz(pred);
        writer_->call(label.regnum);
        writer_->endif();
        return true;
      }
      writer_->call(label.regnum);
      return true;
    }
    if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
      UINT cursor = 0;
      Param first;
      if (!take_source(params, cursor, length, first, code_.major)) {
        reason_ = "a loop/rep without an integer count";
        return false;
      }
      Param count = first;
      if (first.regtype == D3DSPR_LOOP && !take_source(params, cursor, length, count, code_.major)) {
        reason_ = "a loop without an integer count";
        return false;
      }
      if (loop_stack_.size() >= 4) {
        reason_ = "more than four nested loop/rep instructions";
        return false;
      }
      const UINT slot = loop_temps_base_ + (UINT)loop_stack_.size() * 2;
      loop_stack_.push_back(slot);
      std::vector<DWORD> integer;
      if (!source_operand(count, integer))
        return false;
      writer_->instruction(
          D3D10_SB_OPCODE_IADD, false,
          {dxbc::dest_temp(slot, 0x1), integer, dxbc::src_immediate_int(0)}
      );
      if (opcode == D3DSIO_LOOP) {
        Param start = count;
        start.swizzle = 0x55;
        Param step = count;
        step.swizzle = 0xAA;
        std::vector<DWORD> start_op, step_op;
        if (!source_operand(start, start_op) || !source_operand(step, step_op))
          return false;
        writer_->instruction(
            D3D10_SB_OPCODE_IADD, false, {dxbc::dest_temp(slot, 0x2), start_op, dxbc::src_immediate_int(0)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_IADD, false, {dxbc::dest_temp(slot, 0x4), step_op, dxbc::src_immediate_int(0)}
        );
      }
      writer_->loop();
      writer_->instruction(
          D3D10_SB_OPCODE_IGE, false,
          {dxbc::dest_temp(slot, 0x8), dxbc::src_immediate_int(0), dxbc::src_temp(slot, 0x0000)}
      );
      writer_->breakc_nz(dxbc::src_temp(slot, 0xFFFF));
      return true;
    }
    if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC) {
      UINT cursor = 0;
      Param first;
      if (!take_source(params, cursor, length, first, code_.major)) {
        reason_ = "an if without a source";
        return false;
      }
      std::vector<DWORD> predicate;
      if (opcode == D3DSIO_IF) {
        if (!source_operand(first, predicate))
          return false;
        writer_->if_nz(predicate);
        return true;
      }
      Param second;
      if (!take_source(params, cursor, length, second, code_.major)) {
        reason_ = "an ifc without two sources";
        return false;
      }
      std::vector<DWORD> left, right;
      if (!source_operand(first, left) || !source_operand(second, right))
        return false;
      const UINT cmp = (head >> 16) & 7;
      D3D10_SB_OPCODE_TYPE sm5 = D3D10_SB_OPCODE_EQ;
      bool swapped = false;
      switch (cmp) {
      case D3DSPC_GT:
        sm5 = D3D10_SB_OPCODE_LT;
        swapped = true;
        break;
      case D3DSPC_EQ:
        sm5 = D3D10_SB_OPCODE_EQ;
        break;
      case D3DSPC_GE:
        sm5 = D3D10_SB_OPCODE_GE;
        break;
      case D3DSPC_LT:
        sm5 = D3D10_SB_OPCODE_LT;
        break;
      case D3DSPC_NE:
        sm5 = D3D10_SB_OPCODE_NE;
        break;
      case D3DSPC_LE:
        sm5 = D3D10_SB_OPCODE_GE;
        swapped = true;
        break;
      default:
        reason_ = "an ifc comparison this translator does not read";
        return false;
      }
      writer_->instruction(sm5, false, {dxbc::dest_temp(scratch_, 0x1), swapped ? right : left, swapped ? left : right});
      writer_->if_nz(dxbc::src_temp(scratch_, 0x0000));
      return true;
    }
    if (length < 2 && opcode != D3DSIO_DEFI && opcode != D3DSIO_DEFB && opcode != D3DSIO_TEXKILL) {
      // every instruction this slice reads takes a destination and at least one source
      switch (opcode) {
      case D3DSIO_MOV:
      case D3DSIO_ADD:
      case D3DSIO_SUB:
      case D3DSIO_MUL:
      case D3DSIO_MAD:
        reason_ = "an instruction with too few parameters";
        return false;
      default:
        break;
      }
    }

    const Param dest_param = destination(params[0]);
    bool saturate = (dest_param.modifier & 1) != 0; // D3DSPDM_SATURATE
    // ps_1.x clamps only on the explicit _sat modifier, as the destination modifier itself asks (wined3d's GLSL
    // backend writes clamp() for _SAT alone, and Wine visual.c cnd_test's comment builds 2.0 in a register with
    // "adding c0.r(=1.0) to c0.r into r1.r" and passes it to dp3: no implicit clamp of a shader model 1 write)
    std::vector<DWORD> dest;
    // texkill's parameter is the register it reads, which D3D9 encodes where a destination would be: it is not written, so
    // it is not a destination this build needs a slot for. setp's is a predicate this translation keeps in a temporary
    // of its own, and breakp's first parameter is the predicate it reads rather than one it writes
    const bool predicate_op = opcode == D3DSIO_SETP || opcode == D3DSIO_BREAKP;
    if (opcode != D3DSIO_TEXKILL && !predicate_op && !destination_operand(dest_param, dest)) {
      reason_ = "an output register this build writes no slot for";
      return false;
    }
    // a destination shift (ps_1.x _x2, _x4, _d2, ...) scales the result, and the clamp follows the scale. the instruction
    // writes a temporary and the epilogue of this instruction copies it across
    if (opcode != D3DSIO_TEXKILL && dest_param.shift) {
      int shift = (int)(dest_param.shift & 0xF);
      if (shift & 8)
        shift -= 16;
      if (shift != 0) {
        scale_after_ = true;
        scale_shift_ = shift;
        scale_sat_ = saturate;
        scale_dest_ = dest;
        scale_temp_ = mod_base_ + 9;
        dest = dxbc::dest_temp(scale_temp_, dest_param.mask ? dest_param.mask : 0xF);
        saturate = false;
      }
    }

    UINT cursor = 1;
    std::vector<Param> sources;
    while (cursor < length) {
      Param s;
      if (!take_source(params, cursor, length, s, code_.major)) {
        reason_ = "a relative address without an address register";
        return false;
      }
      sources.push_back(s);
    }
    auto source_at = [&](UINT index, std::vector<DWORD> &out) {
      if (index < 1 || index > sources.size())
        return false;
      return source_operand(sources[index - 1], out);
    };

    switch (opcode) {
    case D3DSIO_MOV:
    case D3DSIO_MOVA:
    case D3DSIO_ADD:
    case D3DSIO_MUL:
    case D3DSIO_MIN:
    case D3DSIO_MAX:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
    case D3DSIO_FRC:
    case D3DSIO_RCP:
    case D3DSIO_RSQ:
    case D3DSIO_EXP:
    case D3DSIO_EXPP:
    case D3DSIO_LOG:
    case D3DSIO_LOGP: {
      std::vector<DWORD> first;
      if (!source_at(1, first))
        return false;
      if ((opcode == D3DSIO_MOV || opcode == D3DSIO_MOVA) && dest_param.regtype == D3DSPR_ADDR) {
        // a0 is an integer: MOVA rounds to nearest, and a shader model 1 MOV to a0.x floors the value it carries
        // (Wine visual.c test_mova's table: c7 of -2.4 gives a0 -2 through mova, -3 through vs_1_x mov)
        if (opcode == D3DSIO_MOVA || (opcode == D3DSIO_MOV && code_.major < 2)) {
          writer_->instruction(opcode == D3DSIO_MOVA ? D3D10_SB_OPCODE_ROUND_NE : D3D10_SB_OPCODE_ROUND_NI, false,
                               {dxbc::dest_temp(scratch_), first});
          first = dxbc::src_temp(scratch_);
        }
        writer_->instruction(D3D10_SB_OPCODE_FTOI, false, {dest, first});
        return true;
      }
      D3D10_SB_OPCODE_TYPE sm5 = D3D10_SB_OPCODE_MOV;
      switch (opcode) {
      case D3DSIO_MOV:
      case D3DSIO_MOVA:
        sm5 = D3D10_SB_OPCODE_MOV;
        break;
      case D3DSIO_ADD:
        sm5 = D3D10_SB_OPCODE_ADD;
        break;
      case D3DSIO_MUL:
        sm5 = D3D10_SB_OPCODE_MUL;
        break;
      case D3DSIO_MIN:
        sm5 = D3D10_SB_OPCODE_MIN;
        break;
      case D3DSIO_MAX:
        sm5 = D3D10_SB_OPCODE_MAX;
        break;
      case D3DSIO_DP3:
        sm5 = D3D10_SB_OPCODE_DP3;
        break;
      case D3DSIO_DP4:
        sm5 = D3D10_SB_OPCODE_DP4;
        break;
      case D3DSIO_FRC:
        sm5 = D3D10_SB_OPCODE_FRC;
        break;
      case D3DSIO_RCP:
        sm5 = D3D11_SB_OPCODE_RCP;
        break;
      case D3DSIO_RSQ:
        sm5 = D3D10_SB_OPCODE_RSQ;
        break;
      case D3DSIO_EXP:
      case D3DSIO_EXPP:
        sm5 = D3D10_SB_OPCODE_EXP;
        break;
      default:
        sm5 = D3D10_SB_OPCODE_LOG;
        break;
      }
      if (opcode == D3DSIO_MOV || opcode == D3DSIO_MOVA || opcode == D3DSIO_FRC || opcode == D3DSIO_RCP ||
          opcode == D3DSIO_RSQ || opcode == D3DSIO_EXP || opcode == D3DSIO_EXPP || opcode == D3DSIO_LOG ||
          opcode == D3DSIO_LOGP) {
        // a draw whose declaration carries D3D9's transformed position maps the screen-space vertex the write reads
        // through the viewport (D3D9's rasterizer takes the pixels the fixed-function pipeline maps it to)
        if (opcode == D3DSIO_MOV && transformed_input_ && code_.major <= 3 &&
            (dest_param.regtype == D3DSPR_RASTOUT || dest_param.regtype == D3DSPR_OUTPUT)) {
          const auto written = vs_output_slot_.find(dest_param.regtype * 256 + dest_param.regnum);
          if (written != vs_output_slot_.end()) {
            const auto usage = written_outputs_.find(written->second);
            if (usage != written_outputs_.end() && (usage->second.usage == D3DDECLUSAGE_POSITION ||
                                                    usage->second.usage == D3DDECLUSAGE_POSITIONT)) {
              emit_position_conversion(dest, first);
              return true;
            }
          }
        }
        writer_->instruction(sm5, saturate, {dest, first});
        return true;
      }
      std::vector<DWORD> second;
      if (!source_at(2, second))
        return false;
      writer_->instruction(sm5, saturate, {dest, first, second});
      return true;
    }
    case D3DSIO_SUB: {
      std::vector<DWORD> first, second;
      if (!source_at(1, first) || !source_at(2, second))
        return false;
      // the second source negated is how add subtracts
      negate_operand(second);
      writer_->instruction(D3D10_SB_OPCODE_ADD, saturate, {dest, first, second});
      return true;
    }
    case D3DSIO_MAD: {
      std::vector<DWORD> first, second, third;
      if (!source_at(1, first) || !source_at(2, second) || !source_at(3, third))
        return false;
      writer_->instruction(D3D10_SB_OPCODE_MAD, saturate, {dest, first, second, third});
      return true;
    }
    case D3DSIO_ABS: {
      std::vector<DWORD> value;
      if (!source_at(1, value))
        return false;
      std::vector<DWORD> negated = value;
      negate_operand(negated);
      writer_->instruction(D3D10_SB_OPCODE_MAX, saturate, {dest, value, negated});
      return true;
    }
    case D3DSIO_SLT:
    case D3DSIO_SGE: {
      std::vector<DWORD> first, second;
      if (!source_at(1, first) || !source_at(2, second))
        return false;
      // the comparison writes all ones or none; masking it with 1.0 makes it the 1.0 or 0.0 D3D9's slt and sge write
      const std::vector<DWORD> compared = dest;
      writer_->instruction(
          opcode == D3DSIO_SLT ? D3D10_SB_OPCODE_LT : D3D10_SB_OPCODE_GE, false, {compared, first, second}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_AND, saturate, {dest, compared, dxbc::src_immediate(1.0f, 1.0f, 1.0f, 1.0f)}
      );
      return true;
    }
    case D3DSIO_SGN: {
      // dst = +1 for a positive component, 0 for zero and -1 for a negative one (Microsoft Learn, sgn). a comparison
      // writes all ones or none, which the mask with 1.0 turns into the 1.0 or 0.0 the subtraction needs
      std::vector<DWORD> value;
      if (!source_at(1, value))
        return false;
      writer_->instruction(
          D3D10_SB_OPCODE_LT, false, {dxbc::dest_temp(scratch_, 0xF), value, dxbc::src_immediate(0, 0, 0, 0)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_AND, false,
          {dxbc::dest_temp(scratch_, 0xF), dxbc::src_temp(scratch_), dxbc::src_immediate(1, 1, 1, 1)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_LT, false, {dxbc::dest_temp(scratch_ + 1, 0xF), dxbc::src_immediate(0, 0, 0, 0), value}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_AND, false,
          {dxbc::dest_temp(scratch_ + 1, 0xF), dxbc::src_temp(scratch_ + 1), dxbc::src_immediate(1, 1, 1, 1)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_ADD, saturate,
          {dest, dxbc::src_temp(scratch_ + 1), dxbc::src_temp(scratch_, 0xE4, D3D10_SB_OPERAND_MODIFIER_NEG)}
      );
      return true;
    }
    case D3DSIO_LIT: {
      // dst = (1, max(src.x, 0), src.x > 0 ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0, 1), and a zero exponent
      // leaves dst.z at 0 rather than pow(0, 0)'s NaN (Microsoft Learn, lit; Wine shader_glsl_lit's same guard)
      std::vector<DWORD> value;
      if (!source_at(1, value))
        return false;
      Param w_param = source(params[1]);
      w_param.swizzle = 0xFF; // .wwww
      Param x_param = source(params[1]);
      x_param.swizzle = 0x0000; // .xxxx
      std::vector<DWORD> w_value, x_value;
      if (!source_operand(w_param, w_value) || !source_operand(x_param, x_value))
        return false;
      const DWORD mask = dest_param.mask ? dest_param.mask : 0xF;
      auto component = [&](DWORD bit) {
        std::vector<DWORD> out = dest;
        out[0] &= ~D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK;
        out[0] |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(bit << 4);
        return out;
      };
      const UINT xy = scratch_, power = scratch_ + 1, work = scratch_ + 2;
      writer_->instruction(
          D3D10_SB_OPCODE_MAX, false, {dxbc::dest_temp(xy, 0x3), value, dxbc::src_immediate(0, 0, 0, 0)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MAX, false,
          {dxbc::dest_temp(power, 0x1), w_value, dxbc::src_immediate(-128, -128, -128, -128)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MIN, false,
          {dxbc::dest_temp(power, 0x1), dxbc::src_temp(power, 0x0000), dxbc::src_immediate(128, 128, 128, 128)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_LOG, false, {dxbc::dest_temp(work, 0x1), dxbc::src_temp(xy, 0x5555)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MUL, false,
          {dxbc::dest_temp(power, 0x1), dxbc::src_temp(work, 0x0000), dxbc::src_temp(power, 0x0000)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_EXP, false, {dxbc::dest_temp(power, 0x1), dxbc::src_temp(power, 0x0000)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_EQ, false,
          {dxbc::dest_temp(work, 0x2), w_value, dxbc::src_immediate(0, 0, 0, 0)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MOVC, false,
          {dxbc::dest_temp(power, 0x4), dxbc::src_temp(work, 0x5555), dxbc::src_immediate(0, 0, 0, 0),
           dxbc::src_temp(power, 0x0000)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_LT, false,
          {dxbc::dest_temp(work, 0x4), dxbc::src_immediate(0, 0, 0, 0), x_value}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MOVC, false,
          {dxbc::dest_temp(work, 0x8), dxbc::src_temp(work, 0xAAAA), dxbc::src_temp(power, 0xAAAA),
           dxbc::src_immediate(0, 0, 0, 0)}
      );
      if (mask & 0x1)
        writer_->instruction(
            D3D10_SB_OPCODE_MOV, false, {component(0x1), dxbc::src_immediate(1, 1, 1, 1)}
        );
      if (mask & 0x2)
        writer_->instruction(
            D3D10_SB_OPCODE_MOV, false, {component(0x2), dxbc::src_temp(xy, 0x0000)}
        );
      if (mask & 0x4)
        writer_->instruction(
            D3D10_SB_OPCODE_MOV, false, {component(0x4), dxbc::src_temp(work, 0xFFFF)}
        );
      if (mask & 0x8)
        writer_->instruction(
            D3D10_SB_OPCODE_MOV, false, {component(0x8), dxbc::src_immediate(1, 1, 1, 1)}
        );
      return true;
    }
    case D3DSIO_SETP: {
      // setp_comp p0, src0, src1: the comparison is kept for the if, breakp or callnz that reads it. D3D9 has one
      // predicate register, and its truth is every compared component of the destination's write mask
      const Param target = destination(params[0]);
      if (target.regtype != D3DSPR_PREDICATE || target.regnum != 0) {
        reason_ = "a setp of a predicate register this build does not keep";
        return false;
      }
      std::vector<DWORD> first, second;
      if (!source_at(1, first) || !source_at(2, second))
        return false;
      if (predicate_temp_ == UINT_MAX)
        predicate_temp_ = scratch_++;
      const UINT cmp = (head >> 16) & 7;
      D3D10_SB_OPCODE_TYPE sm5 = D3D10_SB_OPCODE_EQ;
      bool swapped = false;
      switch (cmp) {
      case D3DSPC_GT:
        sm5 = D3D10_SB_OPCODE_LT;
        swapped = true;
        break;
      case D3DSPC_EQ:
        sm5 = D3D10_SB_OPCODE_EQ;
        break;
      case D3DSPC_GE:
        sm5 = D3D10_SB_OPCODE_GE;
        break;
      case D3DSPC_LT:
        sm5 = D3D10_SB_OPCODE_LT;
        break;
      case D3DSPC_NE:
        sm5 = D3D10_SB_OPCODE_NE;
        break;
      case D3DSPC_LE:
        sm5 = D3D10_SB_OPCODE_GE;
        swapped = true;
        break;
      default:
        reason_ = "a setp comparison this translator does not read";
        return false;
      }
      const DWORD mask = target.mask ? target.mask : 0xF;
      writer_->instruction(
          sm5, false,
          {dxbc::dest_temp(predicate_temp_, mask), swapped ? second : first, swapped ? first : second}
      );
      for (UINT comp = 1; comp < 4; comp++) {
        if (!(mask & (1u << comp)))
          continue;
        writer_->instruction(
            D3D10_SB_OPCODE_AND, false,
            {dxbc::dest_temp(predicate_temp_, 0x1), dxbc::src_temp(predicate_temp_, 0x0000),
             dxbc::src_temp(predicate_temp_, 0x55u * comp)}
        );
      }
      predicate_valid_ = true;
      return true;
    }
    case D3DSIO_BREAKP: {
      if (length < 1) {
        reason_ = "a breakp without a predicate";
        return false;
      }
      const Param predicate = source(params[0]);
      if (predicate.regtype != D3DSPR_PREDICATE || predicate.regnum != 0 || !predicate_valid_) {
        reason_ = "a breakp of a predicate the program does not set";
        return false;
      }
      writer_->breakc_nz(dxbc::src_temp(predicate_temp_, 0x0000));
      return true;
    }
    case D3DSIO_LRP: {
      // dst = src1 * src2 + (1 - src1) * src3
      std::vector<DWORD> factor, first, second;
      if (!source_at(1, factor) || !source_at(2, first) || !source_at(3, second))
        return false;
      std::vector<DWORD> inverse = factor;
      negate_operand(inverse);
      writer_->instruction(
          D3D10_SB_OPCODE_ADD, false,
          {dxbc::dest_temp(scratch_), dxbc::src_immediate(1.0f, 1.0f, 1.0f, 1.0f), inverse}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MUL, false, {dxbc::dest_temp(scratch_), dxbc::src_temp(scratch_), second}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MAD, saturate, {dest, factor, first, dxbc::src_temp(scratch_)}
      );
      return true;
    }
    case D3DSIO_POW: {
      std::vector<DWORD> base, exponent;
      if (!source_at(1, base) || !source_at(2, exponent))
        return false;
      writer_->instruction(D3D10_SB_OPCODE_LOG, false, {dxbc::dest_temp(scratch_), base});
      writer_->instruction(
          D3D10_SB_OPCODE_MUL, false, {dxbc::dest_temp(scratch_), dxbc::src_temp(scratch_), exponent}
      );
      writer_->instruction(D3D10_SB_OPCODE_EXP, saturate, {dest, dxbc::src_temp(scratch_)});
      return true;
    }
    case D3DSIO_NRM: {
      std::vector<DWORD> value;
      if (!source_at(1, value))
        return false;
      writer_->instruction(D3D10_SB_OPCODE_DP3, false, {dxbc::dest_temp(scratch_), value, value});
      writer_->instruction(D3D10_SB_OPCODE_RSQ, false, {dxbc::dest_temp(scratch_), dxbc::src_temp(scratch_)});
      writer_->instruction(D3D10_SB_OPCODE_MUL, saturate, {dest, value, dxbc::src_temp(scratch_)});
      return true;
    }
    case D3DSIO_M4x4:
    case D3DSIO_M4x3:
    case D3DSIO_M3x4:
    case D3DSIO_M3x3:
    case D3DSIO_M3x2: {
      std::vector<DWORD> vector_operand;
      if (!source_at(1, vector_operand))
        return false;
      const Param matrix = source(params[2]);
      if (matrix.regtype != D3DSPR_CONST) {
        reason_ = "a matrix multiply whose matrix is not a float constant";
        return false;
      }
      const UINT rows = opcode == D3DSIO_M4x4 || opcode == D3DSIO_M3x4 ? 4 : opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x3 ? 3 : 2;
      const bool four = opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x4;
      for (UINT row = 0; row < rows; row++) {
        if (!(dest_param.mask & (1 << row)))
          continue;
        const UINT row_reg = matrix.regnum + row;
        if (row_reg >= kFloatConstants) {
          reason_ = "a matrix multiply past the float constants";
          return false;
        }
        std::vector<DWORD> output = dest;
        output[0] &= ~(D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK);
        output[0] |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK((1u << row) << 4);
        writer_->instruction(
            four ? D3D10_SB_OPCODE_DP4 : D3D10_SB_OPCODE_DP3, saturate,
            {output, vector_operand, dxbc::src_constant(kFloatSlot, row_reg)}
        );
      }
      return true;
    }
    case D3DSIO_CMP: {
      // dest = src0 >= 0 ? src1 : src2, per component (Microsoft Learn, cmp - ps)
      std::vector<DWORD> compared, first, second;
      if (!source_at(1, compared) || !source_at(2, first) || !source_at(3, second))
        return false;
      writer_->instruction(
          D3D10_SB_OPCODE_GE, false, {dxbc::dest_temp(scratch_), compared, dxbc::src_immediate(0, 0, 0, 0)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MOVC, saturate, {dest, dxbc::src_temp(scratch_), first, second}
      );
      return true;
    }
    case D3DSIO_CND: {
      // dest = src0 > 0.5 ? src1 : src2. ps_1_1 through ps_1_3 compare src0's alpha once and select it for every
      // component; ps_1_4 compares each component of src0 (Wine visual.c cnd_test: "All 3 components get set, based on
      // the .w comparison" and "Each component(r, g, b) is tested separately against 0.5"). the comparison is strict,
      // and Direct3D 10's LT reads "0.5 < src0", so a source of exactly 0.5 selects src2
      std::vector<DWORD> compared, first, second;
      if (!source_at(1, compared) || !source_at(2, first) || !source_at(3, second))
        return false;
      if (code_.minor < 4) {
        Param alpha = sources[0];
        alpha.swizzle = 0xFFFF; // .wwww
        alpha.modifier = kModNone;
        if (!source_operand(alpha, compared))
          return false;
      }
      // a coissued cnd selects its first source whatever the comparison held: the colour operation rides on the alpha
      // one it is paired with (Wine's cnd_test +cnd, and shader_glsl_cnd's coissue branch). the pair is a shader
      // model 1 form: ps_1_4 has no separate coissue behavior (Wine visual.c cnd_test, "ps_1_4 does not have a
      // different cnd behavior"), so its +cnd compares every component as a plain cnd does
      if ((head & 0x40000000u) != 0 && code_.minor < 4 && dest_param.mask != 0x8) {
        writer_->instruction(D3D10_SB_OPCODE_MOV, saturate, {dest, first});
        return true;
      }
      writer_->instruction(
          D3D10_SB_OPCODE_LT, false, {dxbc::dest_temp(scratch_), dxbc::src_immediate(0.5f, 0.5f, 0.5f, 0.5f), compared}
      );
      writer_->instruction(D3D10_SB_OPCODE_MOVC, saturate, {dest, dxbc::src_temp(scratch_), first, second});
      return true;
    }
    case D3DSIO_DP2ADD: {
      // dest = src0.x * src1.x + src0.y * src1.y + src2 (Microsoft Learn, dp2add)
      std::vector<DWORD> first, second, third;
      if (!source_at(1, first) || !source_at(2, second) || !source_at(3, third))
        return false;
      writer_->instruction(D3D10_SB_OPCODE_DP2, false, {dxbc::dest_temp(scratch_), first, second});
      writer_->instruction(D3D10_SB_OPCODE_ADD, saturate, {dest, dxbc::src_temp(scratch_), third});
      return true;
    }
    case D3DSIO_CRS: {
      // dest.xyz = src0 × src1 (Microsoft Learn, crs): i(yz-zy), j(zx-xz), k(xy-yx)
      Param a = source(params[1]), b = source(params[2]);
      Param a_yzx = a, a_zxy = a, b_yzx = b, b_zxy = b;
      auto rotate = [](Param &p, UINT x, UINT y, UINT z) {
        const UINT comps[4] = {UINT(p.swizzle & 3), UINT((p.swizzle >> 2) & 3), UINT((p.swizzle >> 4) & 3),
                               UINT((p.swizzle >> 6) & 3)};
        p.swizzle = comps[x] | (comps[y] << 2) | (comps[z] << 4) | (comps[x] << 6);
      };
      rotate(a_yzx, 1, 2, 0);
      rotate(a_zxy, 2, 0, 1);
      rotate(b_yzx, 1, 2, 0);
      rotate(b_zxy, 2, 0, 1);
      std::vector<DWORD> a_yzx_op, a_zxy_op, b_yzx_op, b_zxy_op;
      if (!source_operand(a_yzx, a_yzx_op) || !source_operand(a_zxy, a_zxy_op) || !source_operand(b_yzx, b_yzx_op) ||
          !source_operand(b_zxy, b_zxy_op))
        return false;
      writer_->instruction(D3D10_SB_OPCODE_MUL, false, {dxbc::dest_temp(scratch_), a_yzx_op, b_zxy_op});
      std::vector<DWORD> negated = dxbc::src_temp(scratch_);
      negate_operand(negated);
      writer_->instruction(D3D10_SB_OPCODE_MAD, saturate, {dest, a_zxy_op, b_yzx_op, negated});
      return true;
    }
    case D3DSIO_SINCOS: {
      // dest.x = cos(src), dest.y = sin(src) (Microsoft Learn, sincos). SM5 writes sin then cos.
      std::vector<DWORD> value;
      if (!source_at(1, value))
        return false;
      writer_->instruction(
          D3D10_SB_OPCODE_SINCOS, false,
          {dxbc::dest_temp(scratch_, 0x2), dxbc::dest_temp(scratch_, 0x1), value}
      );
      writer_->instruction(D3D10_SB_OPCODE_MOV, saturate, {dest, dxbc::src_temp(scratch_)});
      return true;
    }
    case D3DSIO_TEXCOORD: {
      // ps_1.1-1.3 `texcoord t#`: the texture coordinate, clamped to [0, 1], with w set to 1 (Microsoft Learn,
      // texcoord - ps). ps_1.4 `texcrd r#, t#` copies the coordinate to a temporary without clamping, dividing by .z
      // or .w when the source carries DZ or DW (Microsoft Learn, texcrd - ps; Wine shader_glsl_texcoord)
      if (!ps1_ || length < 1) {
        reason_ = "a texcoord this translator does not read";
        return false;
      }
      const bool texture_dest = dest_param.regtype == D3DSPR_TEXTURE;
      UINT dst_temp;
      if (texture_dest) {
        if (dest_param.regnum >= kSamplers || tex_temp_[dest_param.regnum] == UINT_MAX) {
          reason_ = "a texcoord this translator does not read";
          return false;
        }
        dst_temp = tex_temp_[dest_param.regnum];
      } else if (dest_param.regtype == D3DSPR_TEMP) {
        dst_temp = dest_param.regnum;
      } else {
        reason_ = "a texcoord of a register this build does not write";
        return false;
      }
      const Param coord_param = length >= 2 ? source(params[1]) : dest_param;
      if (coord_param.regnum >= kSamplers) {
        reason_ = "a texcoord of a texture coordinate register this build has no place for";
        return false;
      }
      const DWORD coord_mask = dest_param.mask ? dest_param.mask : 0xF;
      std::vector<DWORD> value = dxbc::src_input(texcoord_slot_ + coord_param.regnum, 0xE4);
      // ps_1.4's projected forms divide every written component by .z (DZ) or .w (DW) of the coordinate
      if (!texture_dest && (coord_param.modifier == kModDz || coord_param.modifier == kModDw)) {
        const DWORD divisor = coord_param.modifier == kModDz ? 0xAAu : 0xFFu;
        writer_->instruction(
            D3D11_SB_OPCODE_RCP, false,
            {dxbc::dest_temp(scratch_, 0x1), dxbc::src_input(texcoord_slot_ + coord_param.regnum, divisor)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MUL, false,
            {dxbc::dest_temp(scratch_, 0x3), value, dxbc::src_temp(scratch_, 0x0000)}
        );
        value = dxbc::src_temp(scratch_, 0xE4);
      }
      if (!texture_dest) {
        // ps_1.4's temporary is not clamped, and its w is the coordinate's own
        writer_->instruction(D3D10_SB_OPCODE_MOV, false, {dxbc::dest_temp(dst_temp, coord_mask), value});
        tex_ready_ |= 1u << dest_param.regnum;
        return true;
      }
      if (coord_mask & 0x7)
        writer_->instruction(
            D3D10_SB_OPCODE_MOV, true, {dxbc::dest_temp(dst_temp, coord_mask & 0x7), value}
        );
      if (coord_mask & 0x8)
        writer_->mov(dxbc::dest_temp(dst_temp, 0x8), dxbc::src_immediate(1, 1, 1, 1));
      tex_ready_ |= 1u << dest_param.regnum;
      return true;
    }
    case D3DSIO_TEX:
    case D3DSIO_TEXLDL: {
      // ps_1.1 `tex tn` samples stage n at interpolator n and replaces tn with the colour (Microsoft Learn, tex - ps).
      // ps_1_4 `texld` names the coordinate. neither form names a sampler: the stage is the destination register
      if (ps1_ && opcode == D3DSIO_TEX && length < 3) {
        const UINT stage = dest_param.regnum;
        const UINT coord = length >= 2 && !sources.empty() ? sources[0].regnum : stage;
        if (stage >= kSamplers || tex_temp_[stage] == UINT_MAX) {
          reason_ = "a ps_1.x tex of a stage this device does not have";
          return false;
        }
        std::vector<DWORD> uv = dxbc::src_input(texcoord_slot_ + coord, 0xE4);
        if (project_div_ != 0) {
          // D3DTSS PROJECTED: sample at texcoord.xy / texcoord.[div] (Microsoft Learn, D3DTTFF_PROJECTED)
          const DWORD div_swizzle = project_div_ == 1 ? 0x55u : project_div_ == 2 ? 0xAAu : 0xFFu;
          writer_->instruction(
              D3D11_SB_OPCODE_RCP, false,
              {dxbc::dest_temp(scratch_, 0x1), dxbc::src_input(texcoord_slot_ + coord, div_swizzle)}
          );
          writer_->instruction(
              D3D10_SB_OPCODE_MUL, false,
              {dxbc::dest_temp(scratch_, 0x3), uv, dxbc::src_temp(scratch_, 0x0000)}
          );
          uv = dxbc::src_temp(scratch_, 0xE4);
        }
        writer_->sample(dxbc::dest_temp(tex_temp_[stage], 0xF), uv, stage, stage);
        // D3D9 V8U8/V16U16: missing .ba are 1.0 (Wine visual.c test_signed_formats)
        if (force_alpha_one_)
          writer_->mov(dxbc::dest_temp(tex_temp_[stage], 0xC), dxbc::src_immediate_value(1.0f));
        tex_ready_ |= 1u << stage;
        return true;
      }
      // `texld r#, t#, s#`: the texels the texture bound to that sampler holds at the coordinate. texldp divides by
      // .w first (D3DSI_TEXLD_PROJECT). texldl samples at the explicit lod in the coordinate's .w (Microsoft Learn,
      // texld, texldp, texldl). a vertex shader has no derivatives, so a plain texld is sampled at lod 0 (sample_l),
      // which is the fetch D3D11's vertex stage can express
      std::vector<DWORD> coordinate;
      if (!source_at(1, coordinate))
        return false;
      bool divided = false;
      if (opcode == D3DSIO_TEX && (head & D3DSI_TEXLD_PROJECT)) {
        Param w = source(params[1]);
        w.swizzle = 0xFF; // .wwww
        std::vector<DWORD> divisor;
        if (!source_operand(w, divisor))
          return false;
        writer_->instruction(D3D11_SB_OPCODE_RCP, false, {dxbc::dest_temp(scratch_), divisor});
        writer_->instruction(
            D3D10_SB_OPCODE_MUL, false, {dxbc::dest_temp(scratch_), coordinate, dxbc::src_temp(scratch_)}
        );
        coordinate = dxbc::src_temp(scratch_);
        divided = true;
      }
      const UINT stage = source(params[2]).regnum;
      if (opcode == D3DSIO_TEXLDL || !code_.pixel) {
        std::vector<DWORD> lod_op;
        if (opcode == D3DSIO_TEXLDL) {
          Param lod = source(params[1]);
          lod.swizzle = 0xFF;
          if (!source_operand(lod, lod_op))
            return false;
        } else {
          lod_op = dxbc::src_immediate(0, 0, 0, 0);
        }
        writer_->sample_l(dest, coordinate, stage, stage, lod_op);
      } else if ((shadow_mask_ & (1u << stage)) && shadow_temp_ != UINT_MAX) {
        // a depth texture: the sample answers the comparison, not the texel (d3d9_shader.hpp ShadowState). the
        // reference is the coordinate's third component (divided by the fourth already, for a projected sample), the
        // LEQUAL test passes when it is at most the texel's depth, and every component the destination writes takes
        // the comparison's answer (Wine wined3d's sampler2DShadow: shadow_test's texld and texldp)
        std::vector<DWORD> reference;
        if (divided) {
          // the projected divide left the coordinate in the scratch register: its .z is the divided reference
          reference = dxbc::src_temp(scratch_, 0xAA);
        } else {
          Param z = source(params[1]);
          z.swizzle = 0xAA; // .zzzz
          z.modifier = kModNone;
          if (!source_operand(z, reference))
            return false;
        }
        // the reference is parked in its own temporary, the sample fills one of its own, and the comparison takes
        // both from there (a partial-write sample is one more thing for the backend to get wrong, so the sample
        // writes every component it can)
        writer_->mov(dxbc::dest_temp(shadow_ref_temp_, 0x2), reference);
        writer_->sample(dxbc::dest_temp(shadow_temp_), coordinate, stage, stage);
        // the comparison's answer as a number: GE's result feeds a MOVC (the same pair the comparisons of this
        // translation use), because the backend hands the comparison to the program as a raw integer-in-float whose
        // bit pattern is 0 or 1, not the 0.0 and 1.0 the Direct3D contract names (a MOVC reads it as its condition)
        writer_->instruction(
            D3D10_SB_OPCODE_GE, false,
            {dxbc::dest_temp(shadow_temp_, 0xF), dxbc::src_temp(shadow_temp_, 0x0000),
             dxbc::src_temp(shadow_ref_temp_, 0x5555)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MOVC, false,
            {dest, dxbc::src_temp(shadow_temp_, 0x0000), dxbc::src_immediate_value(1.0f),
             dxbc::src_immediate_value(0.0f)}
        );
      } else {
        writer_->sample(dest, coordinate, stage, stage);
      }
      return true;
    }
    case D3DSIO_TEXDEPTH: {
      // ps_1_4 texdepth makes the pixel's depth the register's .x divided by its .y (clamped to 1), itself clamped to
      // [0, 1]; the register is not modified and the depth is an implicit output (Microsoft Learn, texdepth - ps_1_4;
      // Wine's wined3d shader_glsl_texdepth has the same formula and clamping)
      if (length < 1) {
        reason_ = "a texdepth without a register";
        return false;
      }
      Param value = destination(params[0]);
      value.modifier = kModNone;
      value.swizzle = 0xE4;
      std::vector<DWORD> xy;
      if (!source_operand(value, xy))
        return false;
      writer_->instruction(
          D3D10_SB_OPCODE_MIN, false,
          {dxbc::dest_temp(scratch_, 0x2), xy, dxbc::src_immediate(1, 1, 1, 1)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_DIV, false,
          {dxbc::dest_temp(scratch_, 0x1), xy, dxbc::src_temp(scratch_, 0x55)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MAX, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(scratch_, 0x0), dxbc::src_immediate(0, 0, 0, 0)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MIN, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(scratch_, 0x0), dxbc::src_immediate(1, 1, 1, 1)}
      );
      writer_->mov(dxbc::dest_output_depth(0x1), dxbc::src_temp(scratch_, 0x0));
      return true;
    }
    case D3DSIO_TEXKILL: {
      // D3D9's texkill: the pixel is not written when a texture coordinate component is negative. ps_1_x compares the
      // first three (UVW) only, while ps_2_0 and up compare all four (Microsoft Learn, texkill - ps; Wine's
      // shader_glsl_texkill records the same split as tested by the d3d9 testsuite). Direct3D 11's discard tests a value
      // for zero, so the comparisons are made here, joined, and the pixel discarded when one of them failed. D3D9 kills
      // the whole pixel wherever the instruction is, which is also what a discard in shader model 5 does (the writes
      // around it are masked, not ordered)
      if (length < 1) {
        reason_ = "a texkill without a register";
        return false;
      }
      const Param killed = destination(params[0]);
      // the register's components are copied once, and each comparison selects its lane with the write mask: a source
      // swizzle on an input register is not a lane selector (Wine visual.c texkill_test kills on a negative z, which a
      // read that repeated x/y never saw)
      Param whole = killed;
      whole.modifier = kModNone;
      std::vector<DWORD> value;
      if (!source_operand(whole, value))
        return false;
      const UINT compared = code_.major >= 2 ? 4 : 3;
      writer_->mov(dxbc::dest_temp(scratch_, 0xF), value);
      for (UINT component = 0; component < compared; component++) {
        writer_->instruction(
            D3D10_SB_OPCODE_LT, false,
            {dxbc::dest_temp(scratch_, 1 << component), dxbc::src_temp(scratch_), dxbc::src_immediate_value(0.0f)}
        );
      }
      writer_->instruction(
          D3D10_SB_OPCODE_OR, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(scratch_, 0x0000), dxbc::src_temp(scratch_, 0x5555)}
      );
      if (compared == 4) {
        writer_->instruction(
            D3D10_SB_OPCODE_OR, false,
            {dxbc::dest_temp(scratch_, 0x4), dxbc::src_temp(scratch_, 0xAAAA), dxbc::src_temp(scratch_, 0xFFFF)}
        );
      }
      // .x holds x|y and .z holds z (ps_1_x) or z|w (ps_2_0 and up). reading .z here keeps every compared lane: a swizzle
      // of .x dropped them all (Wine visual.c texkill_test's negative z expectations)
      writer_->instruction(
          D3D10_SB_OPCODE_OR, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(scratch_, 0x0000), dxbc::src_temp(scratch_, 0xAAAA)}
      );
      writer_->discard_if_nonzero(dxbc::src_temp(scratch_, 0x0000));
      return true;
    }
    case D3DSIO_TEXLDD: {
      // texldd is sample_d, which a D3D11 vertex shader may use (it carries its own derivatives)
      std::vector<DWORD> coordinate, ddx, ddy;
      if (!source_at(1, coordinate) || !source_at(3, ddx) || !source_at(4, ddy))
        return false;
      if (sources.size() < 3 || sources[1].regtype != D3DSPR_SAMPLER) {
        reason_ = "a texldd without a sampler";
        return false;
      }
      writer_->sample_d(dest, coordinate, sources[1].regnum, sources[1].regnum, ddx, ddy);
      return true;
    }
    case D3DSIO_BEM: {
      // bem dst, src0, src1 (ps_1_2 and ps_1_3): dst.xy = src0.xy + the stage's bump-environment matrix times
      // src1.xy, and dst's other components are left alone (Microsoft Learn, bem; Wine shader_glsl_bem)
      if (!ps1_ || dest_param.regnum >= BumpEnvState::kStages || !bump_) {
        reason_ = "a bem of a stage this build has no bump-environment matrix for";
        return false;
      }
      std::vector<DWORD> first;
      if (!source_at(1, first))
        return false;
      Param second_x = source(params[2]);
      second_x.swizzle = 0x0000;
      Param second_y = source(params[2]);
      second_y.swizzle = 0x5555;
      std::vector<DWORD> source_x, source_y;
      if (!source_operand(second_x, source_x) || !source_operand(second_y, source_y))
        return false;
      const float *matrix = bump_->matrix[dest_param.regnum];
      writer_->instruction(D3D10_SB_OPCODE_MOV, false, {dxbc::dest_temp(scratch_, 0x3), first});
      writer_->instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(scratch_, 0x1), source_x, dxbc::src_immediate(matrix[0], matrix[0], matrix[0], matrix[0]),
           dxbc::src_temp(scratch_, 0x0000)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(scratch_, 0x1), source_y, dxbc::src_immediate(matrix[2], matrix[2], matrix[2], matrix[2]),
           dxbc::src_temp(scratch_, 0x0000)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(scratch_, 0x2), source_x, dxbc::src_immediate(matrix[1], matrix[1], matrix[1], matrix[1]),
           dxbc::src_temp(scratch_, 0x5555)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(scratch_, 0x2), source_y, dxbc::src_immediate(matrix[3], matrix[3], matrix[3], matrix[3]),
           dxbc::src_temp(scratch_, 0x5555)}
      );
      const DWORD written = dest_param.mask & 0x3;
      if (written) {
        std::vector<DWORD> out = dest;
        out[0] &= ~D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK;
        out[0] |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(written << 4);
        writer_->instruction(D3D10_SB_OPCODE_MOV, false, {out, dxbc::src_temp(scratch_, 0xE4)});
      }
      return true;
    }
    case D3DSIO_TEXBEM:
    case D3DSIO_TEXBEML:
    case D3DSIO_TEXM3x2TEX:
    case D3DSIO_TEXM3x3TEX:
    case D3DSIO_TEXDP3TEX: {
      // Microsoft Learn, texbem / texm3x2tex / texm3x3tex / texdp3tex (ps_1.x): each samples the destination stage.
      // texbem adds the bump-environment matrix times the source's red and green onto the stage's texture coordinates
      // before the sample; the matrix defaults to zero (D3DTSS_BUMPENVMAT*), so an unset stage samples at the
      // interpolator alone, which is also what a lone `texbem t0` in the caps probe does. texbeml then scales rgb by
      // the source's luminance through D3DTSS_BUMPENVLSCALE / LOFFSET (defaults 0 / 0 leave the sample). texm3x*tex
      // and texdp3tex build a texture coordinate from prior texm3x*pad / texture results; without those pads the
      // coordinate is the stage's interpolator, matching an incomplete stream that Create still accepts
      const UINT stage = dest_param.regnum;
      if (!ps1_ || stage >= kSamplers || tex_temp_[stage] == UINT_MAX) {
        reason_ = "a ps_1.x bump or matrix texture op of a stage this device does not have";
        return false;
      }
      std::vector<DWORD> coordinate = dxbc::src_input(texcoord_slot_ + stage, 0xE4);
      if ((opcode == D3DSIO_TEXBEM || opcode == D3DSIO_TEXBEML) && length >= 2 && !sources.empty()) {
        // texbem tn, tm: sample stage n at interpolator n plus the stage's bump matrix times tm.rg. the matrix comes
        // from D3DTSS_BUMPENVMAT* and is baked into this variant, which is keyed by it
        if (!bump_ || stage >= BumpEnvState::kStages) {
          reason_ = "a texbem without the bump-environment matrix its stage holds";
          return false;
        }
        Param value_x = sources[0];
        value_x.swizzle = 0x0000;
        Param value_y = sources[0];
        value_y.swizzle = 0x5555;
        std::vector<DWORD> source_x, source_y;
        if (!source_operand(value_x, source_x) || !source_operand(value_y, source_y))
          return false;
        const float *matrix = bump_->matrix[stage];
        writer_->instruction(D3D10_SB_OPCODE_MOV, false, {dxbc::dest_temp(scratch_, 0x3), coordinate});
        writer_->instruction(
            D3D10_SB_OPCODE_MAD, false,
            {dxbc::dest_temp(scratch_, 0x1), source_x,
             dxbc::src_immediate(matrix[0], matrix[0], matrix[0], matrix[0]), dxbc::src_temp(scratch_, 0x0000)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MAD, false,
            {dxbc::dest_temp(scratch_, 0x1), source_y,
             dxbc::src_immediate(matrix[2], matrix[2], matrix[2], matrix[2]), dxbc::src_temp(scratch_, 0x0000)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MAD, false,
            {dxbc::dest_temp(scratch_, 0x2), source_x,
             dxbc::src_immediate(matrix[1], matrix[1], matrix[1], matrix[1]), dxbc::src_temp(scratch_, 0x5555)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MAD, false,
            {dxbc::dest_temp(scratch_, 0x2), source_y,
             dxbc::src_immediate(matrix[3], matrix[3], matrix[3], matrix[3]), dxbc::src_temp(scratch_, 0x5555)}
        );
        coordinate = dxbc::src_temp(scratch_, 0xE4);
      }
      writer_->sample(
          dxbc::dest_temp(tex_temp_[stage], 0xF), coordinate, stage, stage
      );
      if (opcode == D3DSIO_TEXBEML && !sources.empty()) {
        // texbeml scales the sample's rgb by the source's blue times D3DTSS_BUMPENVLSCALE plus
        // D3DTSS_BUMPENVLOFFSET (Microsoft Learn, texbeml; Wine shader_glsl_texbem's luminance)
        Param value_z = sources[0];
        value_z.swizzle = 0xAAAA;
        std::vector<DWORD> source_z;
        if (!source_operand(value_z, source_z))
          return false;
        const float scale = bump_ ? bump_->lscale[stage] : 0.0f;
        const float offset = bump_ ? bump_->loffset[stage] : 0.0f;
        writer_->instruction(
            D3D10_SB_OPCODE_MAD, false,
            {dxbc::dest_temp(scratch_ + 1, 0x1), source_z, dxbc::src_immediate(scale, scale, scale, scale),
             dxbc::src_immediate(offset, offset, offset, offset)}
        );
        writer_->instruction(
            D3D10_SB_OPCODE_MUL, false,
            {dxbc::dest_temp(tex_temp_[stage], 0x7), dxbc::src_temp(tex_temp_[stage], 0x7),
             dxbc::src_temp(scratch_ + 1, 0x0000)}
        );
      }
      tex_ready_ |= 1u << stage;
      return true;
    }
    default:
      reason_ = "an instruction this translator does not read yet (d3dbc opcode " + std::to_string(opcode) + ")";
      return false;
    }
  }

  // ---- operands --------------------------------------------------------------------------------------------------
  // the scale a ps_1.x destination shift names (bits are a signed 4-bit power of two: 1 is _x2, 15 is _d2), applied
  // after the instruction and before the clamp
  bool apply_dest_scale() {
    const float scale = ldexpf(1.0f, scale_shift_);
    writer_->instruction(
        D3D10_SB_OPCODE_MUL, false,
        {dxbc::dest_temp(scale_temp_, 0xF), dxbc::src_temp(scale_temp_), dxbc::src_immediate(scale, scale, scale, scale)}
    );
    writer_->instruction(D3D10_SB_OPCODE_MOV, scale_sat_, {scale_dest_, dxbc::src_temp(scale_temp_)});
    return true;
  }

  bool apply_scalar_modifier(UINT modifier, float values[4], float divisor_z, float divisor_w) {
    for (UINT at = 0; at < 4; at++) {
      float &value = values[at];
      switch (modifier) {
      case kModNone:
        break;
      case kModNegate:
        value = -value;
        break;
      case kModBias:
        value -= 0.5f;
        break;
      case kModBiasNegate:
        value = -(value - 0.5f);
        break;
      case kModSign:
        value = value * 2.0f - 1.0f;
        break;
      case kModSignNegate:
        value = -(value * 2.0f - 1.0f);
        break;
      case kModComplement:
        value = 1.0f - value;
        break;
      case kModX2:
        value *= 2.0f;
        break;
      case kModX2Negate:
        value = -(value * 2.0f);
        break;
      case kModDz:
        value = divisor_z != 0.0f ? value / divisor_z : 0.0f;
        break;
      case kModDw:
        value = divisor_w != 0.0f ? value / divisor_w : 0.0f;
        break;
      case kModAbs:
        value = fabsf(value);
        break;
      case kModAbsNegate:
        value = -fabsf(value);
        break;
      default:
        reason_ = "a source modifier on a defined constant that this translator does not read";
        return false;
      }
    }
    return true;
  }

  // bias, sign, complement, x2, dz and dw are not operand modifiers in shader model 5, so the value is written to a
  // temporary and the instruction reads that. negate variants flip the read (Microsoft Learn, Source Register Modifiers)
  bool materialize_source_modifier(const Param &p, std::vector<DWORD> &out) {
    if (mod_cursor_ >= 4) {
      reason_ = "an instruction with more source modifiers than this translator keeps";
      return false;
    }
    Param plain = p;
    plain.modifier = kModNone;
    std::vector<DWORD> raw;
    if (!source_operand(plain, raw))
      return false;
    const UINT temp = mod_base_ + mod_cursor_++;
    const bool negate = p.modifier == kModBiasNegate || p.modifier == kModSignNegate || p.modifier == kModX2Negate;
    const UINT kind = negate && p.modifier == kModBiasNegate ? kModBias
                      : negate && p.modifier == kModSignNegate ? kModSign
                      : negate && p.modifier == kModX2Negate ? kModX2
                                                             : p.modifier;
    if (kind == kModBias) {
      writer_->instruction(
          D3D10_SB_OPCODE_ADD, false, {dxbc::dest_temp(temp), raw, dxbc::src_immediate(-0.5f, -0.5f, -0.5f, -0.5f)}
      );
    } else if (kind == kModSign) {
      writer_->instruction(
          D3D10_SB_OPCODE_MAD, false,
          {dxbc::dest_temp(temp), raw, dxbc::src_immediate(2, 2, 2, 2), dxbc::src_immediate(-1, -1, -1, -1)}
      );
    } else if (kind == kModComplement) {
      std::vector<DWORD> negated = raw;
      negate_operand(negated);
      writer_->instruction(
          D3D10_SB_OPCODE_ADD, false, {dxbc::dest_temp(temp), dxbc::src_immediate(1, 1, 1, 1), negated}
      );
    } else if (kind == kModX2) {
      writer_->instruction(
          D3D10_SB_OPCODE_MUL, false, {dxbc::dest_temp(temp), raw, dxbc::src_immediate(2, 2, 2, 2)}
      );
    } else if (kind == kModDz || kind == kModDw) {
      Param divisor = p;
      divisor.modifier = kModNone;
      divisor.swizzle = kind == kModDz ? 0xAA : 0xFF;
      std::vector<DWORD> divisor_op;
      if (!source_operand(divisor, divisor_op))
        return false;
      writer_->instruction(D3D11_SB_OPCODE_RCP, false, {dxbc::dest_temp(mod_base_ + 8), divisor_op});
      writer_->instruction(
          D3D10_SB_OPCODE_MUL, false, {dxbc::dest_temp(temp), raw, dxbc::src_temp(mod_base_ + 8, 0x00)}
      );
    } else if (kind == kModNot) {
      writer_->instruction(D3D10_SB_OPCODE_NOT, false, {dxbc::dest_temp(temp), raw});
    } else {
      reason_ = "a source modifier this translator does not read";
      return false;
    }
    out = dxbc::src_temp(temp, 0xE4, negate ? D3D10_SB_OPERAND_MODIFIER_NEG : D3D10_SB_OPERAND_MODIFIER_NONE);
    return true;
  }

  bool destination_operand(const Param &p, std::vector<DWORD> &out) {
    switch (p.regtype) {
    case D3DSPR_TEMP:
      out = dxbc::dest_temp(p.regnum, p.mask);
      return true;
    case D3DSPR_ADDR: // D3DSPR_TEXTURE is the same number: tn in a pixel shader, a0 in a vertex shader
      // a ps_1.x texture register is a temporary once `tex` or `texcoord` claims it (scan assigns tex_temp_)
      if (ps1_ && p.regnum < kSamplers && tex_temp_[p.regnum] != UINT_MAX) {
        out = dxbc::dest_temp(tex_temp_[p.regnum], p.mask ? p.mask : 0xF);
        return true;
      }
      if (code_.pixel || a0_temp_ == UINT_MAX)
        return false;
      out = dxbc::dest_temp(a0_temp_, p.mask ? p.mask : 0x1);
      return true;
    case D3DSPR_RASTOUT:
    case D3DSPR_ATTROUT:
    case D3DSPR_OUTPUT: {
      auto found = vs_output_slot_.find(p.regtype * 256 + p.regnum);
      if (found == vs_output_slot_.end())
        return false;
      // the clip planes' distances are measured from the position, so a program carrying them writes its copy
      if (position_shadow_ != UINT_MAX) {
        auto usage = written_outputs_.find(found->second);
        if (usage != written_outputs_.end() && (usage->second.usage == D3DDECLUSAGE_POSITION ||
                                                usage->second.usage == D3DDECLUSAGE_POSITIONT)) {
          position_slot_ = found->second;
          out = dxbc::dest_temp(position_shadow_, p.mask);
          return true;
        }
      }
      out = dxbc::dest_output(found->second, p.mask);
      return true;
    }
    case D3DSPR_COLOROUT:
      if (p.regnum >= 4)
        return false;
      // the first target is the alpha test's, so a program that carries one writes the copy it tests (declare)
      if (p.regnum == 0 && colour_shadow_ != UINT_MAX) {
        out = dxbc::dest_temp(colour_shadow_, p.mask);
        return true;
      }
      out = dxbc::dest_output(p.regnum, p.mask);
      return true;
    default:
      return false;
    }
  }

  // the SM5 operand a d3dbc source is read as, with its swizzle and its modifier
  bool source_operand(const Param &p, std::vector<DWORD> &out) {
    // a defi/defb constant is read as the immediate the program defined
    if (!p.relative && p.regtype == D3DSPR_CONSTINT && p.regnum < kIntConstants && defined_int_valid_[p.regnum]) {
      const int *values = defined_int_[p.regnum];
      out = dxbc::src_immediate_int(
          values[(p.swizzle >> 0) & 3], values[(p.swizzle >> 2) & 3], values[(p.swizzle >> 4) & 3],
          values[(p.swizzle >> 6) & 3]
      );
      return true;
    }
    if (!p.relative && p.regtype == D3DSPR_CONSTBOOL && p.regnum < kBoolConstants && defined_bool_valid_[p.regnum]) {
      const int value = defined_bool_[p.regnum] ? 1 : 0;
      out = dxbc::src_immediate_int(value, value, value, value);
      return true;
    }
    // a constant the program defined is read as the immediate it defined
    if (p.regtype == D3DSPR_CONST && !p.relative && p.regnum < kFloatConstants && defined_valid_[p.regnum]) {
      float values[4];
      for (UINT at = 0; at < 4; at++)
        values[at] = defined_[p.regnum][(p.swizzle >> (2 * at)) & 3];
      const float divisor_z = values[2];
      const float divisor_w = values[3];
      if (!apply_scalar_modifier(p.modifier, values, divisor_z, divisor_w))
        return false;
      out = dxbc::src_immediate(values[0], values[1], values[2], values[3]);
      return true;
    }

    D3D10_SB_OPERAND_MODIFIER modifier = D3D10_SB_OPERAND_MODIFIER_NONE;
    switch (p.modifier) {
    case kModNone:
      break;
    case kModNegate:
      modifier = D3D10_SB_OPERAND_MODIFIER_NEG;
      break;
    case kModAbs:
      modifier = D3D10_SB_OPERAND_MODIFIER_ABS;
      break;
    case kModAbsNegate:
      modifier = D3D10_SB_OPERAND_MODIFIER_ABSNEG;
      break;
    case kModBias:
    case kModBiasNegate:
    case kModSign:
    case kModSignNegate:
    case kModComplement:
    case kModX2:
    case kModX2Negate:
    case kModDz:
    case kModDw:
    case kModNot:
      return materialize_source_modifier(p, out);
    default:
      reason_ = "a source modifier this translator does not read";
      return false;
    }

    switch (p.regtype) {
    case D3DSPR_TEMP:
      out = dxbc::src_temp(p.regnum, p.swizzle, modifier);
      return true;
    case D3DSPR_INPUT: {
      // a vertex shader reads an input whose declaration carries fewer components than four through the copy of it
      // this translation made, which has the ones the vertex does not carry (d3d9_device_shader.cpp's prologue)
      const UINT shadow = p.regnum < input_shadow_.size() ? input_shadow_[p.regnum] : UINT_MAX;
      if (!code_.pixel && shadow != UINT_MAX) {
        out = dxbc::src_temp(shadow, p.swizzle, modifier);
        return true;
      }
      out = dxbc::src_input(code_.pixel ? pixel_input_slot(p.regnum) : p.regnum, p.swizzle, modifier);
      return true;
    }
    case D3DSPR_LOOP:
      if (loop_stack_.empty())
        return false;
      out = dxbc::src_temp(loop_stack_.back(), p.swizzle, modifier);
      return true;
    case D3DSPR_TEXTURE: // D3DSPR_ADDR in a vertex shader: a0
      if (!code_.pixel) {
        if (a0_temp_ == UINT_MAX)
          return false;
        out = dxbc::src_temp(a0_temp_, p.swizzle, modifier);
        return true;
      }
      // after `tex` or `texcoord`, tn is the colour that instruction wrote; before it, tn is the interpolator
      if (ps1_ && p.regnum < kSamplers && (tex_ready_ & (1u << p.regnum)) && tex_temp_[p.regnum] != UINT_MAX) {
        out = dxbc::src_temp(tex_temp_[p.regnum], p.swizzle, modifier);
        return true;
      }
      out = dxbc::src_input(texcoord_slot_ + p.regnum, p.swizzle, modifier);
      return true;
    case D3DSPR_CONST:
    case D3DSPR_CONST2:
    case D3DSPR_CONST3:
    case D3DSPR_CONST4: {
      UINT element = p.regnum;
      if (p.regtype == D3DSPR_CONST2)
        element += 2048;
      else if (p.regtype == D3DSPR_CONST3)
        element += 4096;
      else if (p.regtype == D3DSPR_CONST4)
        element += 6144;
      if (p.relative) {
        if (a0_temp_ == UINT_MAX)
          return false;
        out = dxbc::src_constant_relative(kFloatSlot, element, a0_temp_, p.swizzle, modifier);
        return true;
      }
      if (element >= kFloatConstants)
        return false;
      out = dxbc::src_constant(kFloatSlot, element, p.swizzle, modifier);
      return true;
    }
    case D3DSPR_CONSTINT:
      if (p.relative) {
        if (a0_temp_ == UINT_MAX)
          return false;
        out = dxbc::src_constant_relative(kIntSlot, p.regnum, a0_temp_, p.swizzle, modifier);
        return true;
      }
      if (p.regnum >= kIntConstants)
        return false;
      out = dxbc::src_constant(kIntSlot, p.regnum, p.swizzle, modifier);
      return true;
    case D3DSPR_CONSTBOOL:
      if (p.regnum >= kBoolConstants)
        return false;
      out = dxbc::src_constant(kBoolSlot, p.regnum, p.swizzle, modifier);
      return true;
    case D3DSPR_PREDICATE:
      if (p.regnum != 0 || predicate_temp_ == UINT_MAX || !predicate_valid_)
        return false;
      out = dxbc::src_temp(predicate_temp_, p.swizzle, modifier);
      return true;
    default:
      return false;
    }
  }

  // a negated operand: the extended operand token when it has none, the other sign when it has one.
  // Immediate32 cannot carry DXBC source modifiers (D3D11 Create*Shader rejects them), so fold the
  // negation into the literal bits (SUB/ABS of a `def` constant hit this path).
  static void negate_operand(std::vector<DWORD> &operand) {
    const DWORD type = DECODE_D3D10_SB_OPERAND_TYPE(operand[0]);
    if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
      const DWORD components = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(operand[0]);
      const UINT count = components == D3D10_SB_OPERAND_1_COMPONENT ? 1u
                         : components == D3D10_SB_OPERAND_4_COMPONENT ? 4u
                                                                     : 0u;
      for (UINT at = 0; at < count; at++) {
        float value = 0.0f;
        memcpy(&value, &operand[1 + at], sizeof(value));
        value = -value;
        memcpy(&operand[1 + at], &value, sizeof(value));
      }
      return;
    }
    if (operand[0] & D3D10_SB_OPERAND_EXTENDED_MASK) {
      // the modifier is the second token's only interest: flip it between none/neg and abs/absneg
      DWORD &extended = operand[1];
      const DWORD modifier = (extended & D3D10_SB_OPERAND_MODIFIER_MASK) >> D3D10_SB_OPERAND_MODIFIER_SHIFT;
      extended &= ~D3D10_SB_OPERAND_MODIFIER_MASK;
      extended |= ENCODE_D3D10_SB_EXTENDED_OPERAND_MODIFIER(
          modifier == D3D10_SB_OPERAND_MODIFIER_NEG      ? D3D10_SB_OPERAND_MODIFIER_NONE
          : modifier == D3D10_SB_OPERAND_MODIFIER_ABS    ? D3D10_SB_OPERAND_MODIFIER_ABSNEG
          : modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG ? D3D10_SB_OPERAND_MODIFIER_ABS
                                                         : D3D10_SB_OPERAND_MODIFIER_NEG
      );
      return;
    }
    operand[0] |= D3D10_SB_OPERAND_EXTENDED_MASK;
    operand.insert(operand.begin() + 1, ENCODE_D3D10_SB_EXTENDED_OPERAND_MODIFIER(D3D10_SB_OPERAND_MODIFIER_NEG));
  }

  const ShaderCode &code_;
  // D3D9's alpha test, which only a pixel shader carries, and the real it is: whether the first target is written to the
  // copy the test reads, and whether the comparison is one that can fail (D3DCMP_ALWAYS cannot)
  const AlphaTest *alpha_ = nullptr;
  const FogState *fog_ = nullptr;
  const TexProjectState *project_ = nullptr;
  const BumpEnvState *bump_ = nullptr;
  bool alpha_test_ = false;
  bool fog_enabled_ = false;
  UINT project_div_ = 0;
  bool force_alpha_one_ = false;
  bool writes_fog_ = false;
  UINT fog_position_reg_ = UINT_MAX;
  UINT fog_coord_reg_ = UINT_MAX;
  bool alpha_compare_ = false;
  UINT colour_shadow_ = UINT_MAX;
  // D3D9's user clip planes, which only a vertex shader carries: the position is written to a copy of its own (an SM5
  // output register is written, not read), and the program's end writes it out with the six planes' distances from it
  bool clip_planes_ = false;
  UINT position_slot_ = kSlotPosition;
  // where this translation's TEXCOORD0 starts: 2 for the unlit fixed-function textured vertex shaders, 3 for the
  // lit/eight-stage families and translated game vertex shaders (COLOR1 occupies 2 there)
  UINT texcoord_slot_ = kSlotTexcoord;
  // a ps_3_0's declared inputs, each register to the slot its dcl decided, and the slots the usages outside the
  // colours and the texture coordinates were allocated in this translation
  std::map<UINT, UINT> declared_pixel_input_;
  std::map<UINT, UINT> declaration_input_slots_;
  UINT next_declaration_slot_ = kSlotDeclaration;
  // the output slots a vs_3_0's dcl claimed before any instruction ran: every dcl is scanned first, so a free slot
  // one dcl picked must not be given to the next
  std::set<UINT> claimed_outputs_;
  // whether the pixel shader's colour inputs are read flat (D3DRS_SHADEMODE is D3DSHADE_FLAT)
  bool flat_ = false;
  // the pixel stages whose bound texture is a depth one (d3d9_shader.hpp ShadowState); their texld/texldp answer the
  // depth comparison rather than the texel
  UINT shadow_mask_ = 0;
  UINT shadow_temp_ = UINT_MAX;
  UINT shadow_ref_temp_ = UINT_MAX;
  UINT position_shadow_ = UINT_MAX;
  UINT clip_slot_ = 0;
  const std::vector<VertexInput> *vertex_inputs_ = nullptr;
  // the pixel shader's inputs, when a vertex shader is being translated against the shader it draws with
  const std::vector<dxbc::SignatureElement> *pixel_inputs_of_stage_ = nullptr;
  std::unique_ptr<dxbc::Writer> writer_;
  std::string reason_;
  UINT max_temp_ = 0;
  UINT scratch_ = 0;
  bool ps1_ = false;
  // a vertex program whose declaration reads D3D9's transformed (POSITIONT) position: the position write maps the
  // screen-space vertex through the viewport
  bool transformed_input_ = false;
  UINT ps1_tex_written_ = 0;
  UINT tex_ready_ = 0;
  std::array<UINT, kSamplers> tex_temp_{};
  UINT mod_base_ = 0;
  UINT mod_cursor_ = 0;
  bool scale_after_ = false;
  int scale_shift_ = 0;
  bool scale_sat_ = false;
  UINT scale_temp_ = 0;
  std::vector<DWORD> scale_dest_;
  // the temporary each input register is read through, when its declaration carries fewer than four components
  std::vector<UINT> input_shadow_;
  bool defined_valid_[kFloatConstants] = {};
  float defined_[kFloatConstants][4] = {};
  // defi and defb define an integer or boolean constant inside the program, read as the immediate it defined
  int defined_int_[kIntConstants][4] = {};
  bool defined_int_valid_[kIntConstants] = {};
  bool defined_bool_[kBoolConstants] = {};
  bool defined_bool_valid_[kBoolConstants] = {};
  // setp's p0: the comparison's truth, kept for the if, breakp and callnz that read it
  UINT predicate_temp_ = UINT_MAX;
  bool predicate_valid_ = false;
  // the stages the program samples and the samplers it declares, a bit each
  UINT sampled_ = 0;
  UINT declared_samplers_ = 0;
  UINT sampler_cube_ = 0;
  UINT sampler_volume_ = 0;
  bool uses_a0_ = false;
  UINT a0_temp_ = UINT_MAX;
  UINT loop_temps_base_ = 0;
  std::vector<UINT> loop_stack_;
  std::map<UINT, UINT> vs_output_slot_;
  std::map<UINT, UINT> vs_output_declared_;

  // the declaration element an output register carries, as the shader model 3 declaration named it
  struct OutputUsage {
    UINT usage = D3DDECLUSAGE_TEXCOORD;
    UINT usage_index = 0;
  };
  std::map<UINT, OutputUsage> vs_output_usage_;

  // every output slot the program writes, with the semantic it hands the pixel shader
  std::map<UINT, OutputUsage> written_outputs_;
  std::map<UINT, dxbc::SignatureElement> pixel_inputs_;
  // ps_3_0's input register file holds both colours and texture coordinates: the declarations record which of the
  // translator's slots each v# was declared as, and the instruction that reads v# looks it up
  std::map<UINT, UINT> declared_texcoord_input_;
  bool uses_colour_out_[4] = {};
  std::vector<dxbc::SignatureElement> inputs_;
  std::vector<dxbc::SignatureElement> outputs_;
};


} // namespace

// the parameter count a shader model 1 instruction's opcode carries (this file's own table), for callers that read a
// d3dbc program outside the translator's pass
UINT sm1_instruction_tokens(UINT opcode, UINT minor) {
  return sm1_token_count(opcode, minor);
}

const char *
usage_name(UINT usage) {
  return usage_name_of(usage);
}

UINT
declaration_type_mask(UINT type) {
  return mask_of_type(type);
}

// whether `opcode` is an instruction this translator reads. CreateVertexShader and CreatePixelShader refuse a program
// naming one it does not, where D3D9 does, rather than at the first draw that translates it. The list mirrors the read
// cases of emit_instruction; a new instruction is added here with it
bool
opcode_readable(UINT opcode) {
  switch (opcode) {
  case D3DSIO_NOP:
  case D3DSIO_MOV:
  case D3DSIO_MOVA:
  case D3DSIO_ADD:
  case D3DSIO_SUB:
  case D3DSIO_MAD:
  case D3DSIO_MUL:
  case D3DSIO_RCP:
  case D3DSIO_RSQ:
  case D3DSIO_EXP:
  case D3DSIO_LOG:
  case D3DSIO_EXPP:
  case D3DSIO_LOGP:
  case D3DSIO_LIT:
  case D3DSIO_DST:
  case D3DSIO_SGN:
  case D3DSIO_ABS:
  case D3DSIO_MIN:
  case D3DSIO_MAX:
  case D3DSIO_SLT:
  case D3DSIO_SGE:
  case D3DSIO_FRC:
  case D3DSIO_NRM:
  case D3DSIO_POW:
  case D3DSIO_CRS:
  case D3DSIO_SINCOS:
  case D3DSIO_LRP:
  case D3DSIO_CMP:
  case D3DSIO_CND:
  case D3DSIO_DP3:
  case D3DSIO_DP4:
  case D3DSIO_DP2ADD:
  case D3DSIO_M4x4:
  case D3DSIO_M4x3:
  case D3DSIO_M3x4:
  case D3DSIO_M3x3:
  case D3DSIO_M3x2:
  case D3DSIO_DCL:
  case D3DSIO_DEF:
  case D3DSIO_DEFI:
  case D3DSIO_DEFB:
  case D3DSIO_TEX:
  case D3DSIO_BEM:
  case D3DSIO_TEXBEM:
  case D3DSIO_TEXBEML:
  case D3DSIO_TEXCOORD:
  case D3DSIO_TEXKILL:
  case D3DSIO_TEXLDD:
  case D3DSIO_TEXLDL:
  case D3DSIO_TEXM3x2PAD:
  case D3DSIO_TEXM3x2TEX:
  case D3DSIO_TEXM3x3:
  case D3DSIO_TEXM3x3PAD:
  case D3DSIO_TEXM3x3TEX:
  case D3DSIO_TEXM3x3DIFF:
  case D3DSIO_TEXM3x3SPEC:
  case D3DSIO_TEXM3x3VSPEC:
  case D3DSIO_TEXM3x2DEPTH:
  case D3DSIO_TEXDP3:
  case D3DSIO_TEXDP3TEX:
  case D3DSIO_TEXREG2AR:
  case D3DSIO_TEXREG2GB:
  case D3DSIO_TEXREG2RGB:
  case D3DSIO_TEXDEPTH:
  case D3DSIO_DSX:
  case D3DSIO_DSY:
  case D3DSIO_SETP:
  case D3DSIO_BREAKP:
  case D3DSIO_BREAK:
  case D3DSIO_BREAKC:
  case D3DSIO_IF:
  case D3DSIO_IFC:
  case D3DSIO_ELSE:
  case D3DSIO_ENDIF:
  case D3DSIO_LOOP:
  case D3DSIO_ENDLOOP:
  case D3DSIO_REP:
  case D3DSIO_ENDREP:
  case D3DSIO_LABEL:
  case D3DSIO_CALL:
  case D3DSIO_CALLNZ:
  case D3DSIO_RET:
  case D3DSIO_PHASE:
  case D3DSIO_COMMENT:
  case D3DSIO_END:
    return true;
  default:
    return false;
  }
}

bool
translate_shader(
    const ShaderCode &code, const std::vector<VertexInput> *vertex_inputs,
    const std::vector<dxbc::SignatureElement> *pixel_inputs, const AlphaTest *alpha, const FogState *fog,
    const TexProjectState *project, const BumpEnvState *bump, bool clip_planes, TranslatedProgram &out,
    std::string &reason, UINT texcoord_slot, bool flat, UINT shadow_mask
) {
  Translator translator(code, alpha, fog, project, bump, clip_planes, texcoord_slot, flat, shadow_mask);
  if (translator.run(vertex_inputs, pixel_inputs, out))
    return true;
  reason = translator.reason();
  return false;
}

} // namespace mullion::d3d9
