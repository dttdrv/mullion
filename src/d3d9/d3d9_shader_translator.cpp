// the d3dbc translator: a game's shader program, read as Direct3D 9's tokens and written as the shader model 5 program
// this build's D3D11 device compiles (d3d9_dxbc.hpp). the instructions this slice reads are the arithmetic and the
// matrix multiplies a vertex shader transforms with, and the same arithmetic for a pixel shader's colour; texture
// sampling, flow control and the shader model 1.x texture registers are refused with the reason the log carries, rather
// than translated into something the game did not write.
#include "d3d9_shader_translator.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <map>
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
// the texture stages, and so the samplers, a pixel shader's program may sample (D3D9's eight: the device's caps say
// MaxSimultaneousTextures is eight)
constexpr UINT kSamplers = 8;

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
constexpr UINT kMaxOutputRegisters = 16;

// a source modifier as d3d9types.h packs it (D3DSPSM_*, already shifted down here)
enum SourceModifier {
  kModNone = 0,
  kModNegate = 1,
  kModBias = 2,
  kModBiasNegate = 3,
  kModSign = 4,
  kModSignNegate = 5,
  kModComplement = 6,
  kModComplementNegate = 7,
  kModX2 = 8,
  kModX2Negate = 9,
  kModDz = 10,
  kModDzNegate = 11,
  kModDw = 12,
  kModDwNegate = 13,
  kModAbs = 14,
  kModAbsNegate = 15,
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

// a source token and, when it is relative, the address register that follows it (Microsoft Learn, relative addressing)
bool take_source(const DWORD *params, UINT &index, UINT length, Param &out) {
  if (index >= length)
    return false;
  out = source(params[index++]);
  if (out.relative) {
    if (index >= length)
      return false;
    Param addr = source(params[index++]);
    out.address_reg = addr.regnum;
  }
  return true;
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
  Translator(const ShaderCode &code, const AlphaTest *alpha) : code_(code), alpha_(alpha) {}

  bool run(
      const std::vector<VertexInput> *vertex_inputs, const std::vector<dxbc::SignatureElement> *pixel_inputs,
      TranslatedProgram &out
  ) {
    vertex_inputs_ = vertex_inputs;
    pixel_inputs_of_stage_ = pixel_inputs;
    // the version: what the first token names, and the stages this slice reads
    if (code_.pixel) {
      if (code_.major < 2) {
        reason_ = "a pixel shader of shader model 1.x is not translated yet (its texture registers and samplers are "
                  "the instruction's own)";
        return false;
      }
      if (code_.major > 3) {
        reason_ = "a pixel shader of a shader model past 3.0 is not translated";
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

    writer_.reset(new dxbc::Writer(code_.pixel ? dxbc::Stage::Pixel : dxbc::Stage::Vertex));
    if (!scan())
      return false;
    if (!declare())
      return false;
    if (!emit_body())
      return false;

    out.dxbc = writer_->container();
    out.inputs = inputs_;
    out.outputs = outputs_;
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
      const UINT length = (head >> 24) & 0xF;
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
    return true;
  }

  bool scan_declaration(DWORD usage_token, DWORD register_token) {
    const Param register_param = destination(register_token);
    if (!code_.pixel) {
      // a vertex shader declares its inputs (`dcl_usage v#`) and, in shader model 3, its outputs (`dcl_usage o#`)
      const UINT usage = usage_token & 0xF;
      const UINT usage_index = (usage_token >> 16) & 0xF;
      if (register_param.regtype == D3DSPR_OUTPUT) {
        if (output_slot(usage, usage_index, register_param.regnum) == UINT_MAX) {
          reason_ = "a vertex shader output this build has no slot for";
          return false;
        }
        vs_output_declared_[register_param.regnum] = output_slot(usage, usage_index, register_param.regnum);
        vs_output_usage_[register_param.regnum].usage = usage;
        vs_output_usage_[register_param.regnum].usage_index = usage_index;
      }
      return true;
    }
    // a pixel shader declares its inputs: a colour register or a texture coordinate one
    if (register_param.regtype == D3DSPR_INPUT) {
      if (!register_input(kSlotColour + std::min<UINT>(register_param.regnum, 1), "COLOR", register_param.regnum)) {
        reason_ = "a pixel shader input this build has no place for";
        return false;
      }
    } else if (register_param.regtype == D3DSPR_TEXTURE) {
      if (!register_input(kSlotTexcoord + register_param.regnum, "TEXCOORD", register_param.regnum)) {
        reason_ = "a pixel shader texture coordinate register this build has no place for";
        return false;
      }
    } else if (register_param.regtype == D3DSPR_SAMPLER) {
      // `dcl_2d s#`: the sampler a texld reads, and the dimension it reads its texture as. D3D9's stage # is both the
      // texture and the sampler of that number, and this device has the eight stages its caps name
      const UINT dimension = usage_token & D3DSP_TEXTURETYPE_MASK;
      if (dimension != D3DSTT_2D && dimension != D3DSTT_CUBE && dimension != D3DSTT_VOLUME) {
        reason_ = "a sampler of a texture dimension this build does not sample yet (dcl_2d, dcl_cube and dcl_volume "
                  "are the ones it reads).";
        return false;
      }
      if (register_param.regnum >= kSamplers) {
        reason_ = "a sampler past s" + std::to_string(kSamplers - 1) + ": this device has " +
                  std::to_string(kSamplers) + " texture stages (MaxSimultaneousTextures)";
        return false;
      }
      declared_samplers_ |= 1u << register_param.regnum;
      if (dimension == D3DSTT_CUBE)
        sampler_cube_ |= 1u << register_param.regnum;
      if (dimension == D3DSTT_VOLUME)
        sampler_volume_ |= 1u << register_param.regnum;
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
    // defi and defb are read when an instruction needs them, and this slice names none
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
            !register_input(kSlotTexcoord + dest_param.regnum, "TEXCOORD", dest_param.regnum)) {
          reason_ = "a texkill of a texture coordinate register this build has no place for";
          return false;
        }
        if (dest_param.regtype == D3DSPR_INPUT && dest_param.regnum > 1) {
          reason_ = "a texkill of a pixel shader colour input past v1 is not D3D9's, and this build refuses it";
          return false;
        }
        if (dest_param.regtype == D3DSPR_INPUT &&
            !register_input(kSlotColour + std::min<UINT>(dest_param.regnum, 1), "COLOR", dest_param.regnum)) {
          reason_ = "a texkill of a pixel shader colour input this build has no place for";
          return false;
        }
      }
      // a texld samples the sampler its third parameter names, which is the stage whose texture it reads
      if (code_.pixel && (opcode == D3DSIO_TEX || opcode == D3DSIO_TEXLDL || opcode == D3DSIO_TEXLDD)) {
        if (length < 3 || source(params[2]).regtype != D3DSPR_SAMPLER) {
          reason_ = "a texld without a sampler (the shader model 2 and 3 form names one: texld r#, t#, s#)";
          return false;
        }
        if (source(params[2]).regnum >= kSamplers) {
          reason_ = "a texld of a sampler past s" + std::to_string(kSamplers - 1) + ": this device has " +
                    std::to_string(kSamplers) + " texture stages (MaxSimultaneousTextures)";
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
      if (!take_source(params, at, length, src)) {
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
      if (code_.pixel && src.regtype == D3DSPR_INPUT && src.regnum > 1) {
        reason_ = "a pixel shader colour input past v1 is not D3D9's, and this build refuses it";
        return false;
      }
      if (code_.pixel && src.regtype == D3DSPR_TEXTURE) {
        if (!register_input(kSlotTexcoord + src.regnum, "TEXCOORD", src.regnum)) {
          reason_ = "a pixel shader texture coordinate register this build has no place for";
          return false;
        }
      }
      if (code_.pixel && src.regtype == D3DSPR_INPUT &&
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

  // the register a vertex shader writes an output of `name` and `index` to: the one the pixel shader it draws with
  // reads it at, and `fallback` when that shader reads none of it
  UINT read_slot(const char *name, UINT index, UINT fallback) {
    if (pixel_inputs_of_stage_) {
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
      return read_slot("TEXCOORD", usage_index, kSlotTexcoord + usage_index);
    case D3DDECLUSAGE_FOG:
    case D3DDECLUSAGE_PSIZE:
      // D3D9's fog and point size are not values a pixel shader reads, and Direct3D 11 has no output register for them
      // (fog is its own render state's work, and the point size a Metal vertex output this build does not make yet)
      reason_ = "the fog and point-size outputs are not translated yet";
      return UINT_MAX;
    default:
      (void)regnum;
      return UINT_MAX;
    }
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
          slot = kSlotTexcoord + dest_param.regnum;
          usage_index = dest_param.regnum;
        }
      } else if (dest_param.regnum < 8) {
        slot = kSlotTexcoord + dest_param.regnum;
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
    return true;
  }

  // ---- the declarations ------------------------------------------------------------------------------------------
  bool declare() {
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
    }

    for (const auto &element : inputs_)
      writer_->input(element);
    for (const auto &element : outputs_)
      writer_->output(element);

    // the D3D9 constant tables, as the three constant buffers a draw binds
    writer_->declare_constant_buffer(kFloatSlot, kFloatConstants);
    writer_->declare_constant_buffer(kIntSlot, kIntConstants);
    writer_->declare_constant_buffer(kBoolSlot, kBoolConstants);

    // what a texture sample reads: for each stage it samples, the sampler the program declared (`dcl_2d s#`) and the
    // texture that stage holds. a program that samples without declaring one is a program D3D9 refuses, so this refuses
    // to translate it
    if (sampled_ & ~declared_samplers_) {
      reason_ = "a texld of a sampler the program does not declare (a pixel shader declares dcl_2d s# first)";
      return false;
    }
    for (UINT stage = 0; stage < kSamplers; stage++) {
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
    if (alpha_test_ && uses_colour_out_[0])
      colour_shadow_ = scratch_++;
    if (uses_a0_)
      a0_temp_ = scratch_++;
    loop_temps_base_ = scratch_;
    scratch_ += 8;
    if (alpha_compare_)
      writer_->declare_constant_buffer(kAlphaSlot, 1);

    // every declaration before the first instruction that uses what it declares, as a program's container has them
    writer_->temps(scratch_ + 1);
    for (const auto &element : inputs_)
      writer_->declare_input(element.reg, element.mask);
    for (const auto &element : outputs_) {
      // a vertex shader's position is a system value, and the program's own declarations have to say so (a Metal vertex
      // function's `[[position]]` comes from this declaration, not from the signature chunk). a pixel shader's targets
      // are plain outputs, as a compiler makes them
      if (!code_.pixel && element.system_value != 0)
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
      const UINT length = (head >> 24) & 0xF;
      if (!emit_instruction(head, tokens + at + 1, length))
        return false;
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
    writer_->mov(dxbc::dest_output(0, 0xF), dxbc::src_temp(colour_shadow_, 0xE4));
    return true;
  }

  bool emit_instruction(DWORD head, const DWORD *params, UINT length) {
    const UINT opcode = head & 0xFFFF;
    if (opcode == D3DSIO_NOP || opcode == D3DSIO_DCL || opcode == D3DSIO_DEF)
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
      if (!take_source(params, cursor, length, label)) {
        reason_ = "a call without a label";
        return false;
      }
      if (opcode == D3DSIO_CALLNZ) {
        Param predicate;
        if (!take_source(params, cursor, length, predicate)) {
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
      if (!take_source(params, cursor, length, first)) {
        reason_ = "a loop/rep without an integer count";
        return false;
      }
      Param count = first;
      if (first.regtype == D3DSPR_LOOP && !take_source(params, cursor, length, count)) {
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
      if (!take_source(params, cursor, length, first)) {
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
      if (!take_source(params, cursor, length, second)) {
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
        sm5 = D3D10_SB_OPCODE_GE;
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
        sm5 = D3D10_SB_OPCODE_LT;
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
    if (dest_param.shift) {
      reason_ = "a destination register shifted by a multiply or a divide";
      return false;
    }
    std::vector<DWORD> dest;
    // texkill's parameter is the register it reads, which D3D9 encodes where a destination would be: it is not written, so
    // it is not a destination this build needs a slot for
    if (opcode != D3DSIO_TEXKILL && !destination_operand(dest_param, dest)) {
      reason_ = "an output register this build writes no slot for";
      return false;
    }

    UINT cursor = 1;
    std::vector<Param> sources;
    while (cursor < length) {
      Param s;
      if (!take_source(params, cursor, length, s)) {
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
        // a0 is an integer: MOVA rounds to nearest, MOV truncates (Microsoft Learn, mova)
        if (opcode == D3DSIO_MOVA) {
          writer_->instruction(D3D10_SB_OPCODE_ROUND_NE, false, {dxbc::dest_temp(scratch_), first});
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
    case D3DSIO_TEX:
    case D3DSIO_TEXLDL: {
      // `texld r#, t#, s#`: the texels the texture bound to that sampler holds at the coordinate. texldp divides by
      // .w first (D3DSI_TEXLD_PROJECT). texldl samples at the explicit lod in the coordinate's .w (Microsoft Learn,
      // texld, texldp, texldl)
      std::vector<DWORD> coordinate;
      if (!source_at(1, coordinate))
        return false;
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
      }
      const UINT stage = source(params[2]).regnum;
      if (opcode == D3DSIO_TEXLDL) {
        Param lod = source(params[1]);
        lod.swizzle = 0xFF;
        std::vector<DWORD> lod_op;
        if (!source_operand(lod, lod_op))
          return false;
        writer_->sample_l(dest, coordinate, stage, stage, lod_op);
      } else {
        writer_->sample(dest, coordinate, stage, stage);
      }
      return true;
    }
    case D3DSIO_TEXKILL: {
      // D3D9's texkill: the pixel is not written when any of the register's four components is negative (Microsoft Learn,
      // texkill). Direct3D 11's discard tests a value for zero, so the four comparisons are made here, joined, and the
      // pixel discarded when one of them failed. D3D9 kills the whole pixel wherever the instruction is, which is also
      // what a discard in shader model 5 does (the writes around it are masked, not ordered)
      if (length < 1) {
        reason_ = "a texkill without a register";
        return false;
      }
      const Param killed = destination(params[0]);
      for (UINT component = 0; component < 4; component++) {
        Param read = killed;
        read.swizzle = component * 0x55; // .xxxx, .yyyy, .zzzz, .wwww
        read.modifier = kModNone;
        std::vector<DWORD> value;
        if (!source_operand(read, value))
          return false;
        writer_->instruction(
            D3D10_SB_OPCODE_LT, false,
            {dxbc::dest_temp(scratch_, 1 << component), value, dxbc::src_immediate_value(0.0f)}
        );
      }
      writer_->instruction(
          D3D10_SB_OPCODE_OR, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(scratch_, 0x0000), dxbc::src_temp(scratch_, 0x5555)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_OR, false,
          {dxbc::dest_temp(scratch_, 0x4), dxbc::src_temp(scratch_, 0xAAAA), dxbc::src_temp(scratch_, 0xFFFF)}
      );
      writer_->instruction(
          D3D10_SB_OPCODE_OR, false,
          {dxbc::dest_temp(scratch_, 0x1), dxbc::src_temp(scratch_, 0x0000), dxbc::src_temp(scratch_, 0x4444)}
      );
      writer_->discard_if_nonzero(dxbc::src_temp(scratch_, 0x0000));
      return true;
    }
    case D3DSIO_TEXLDD: {
      if (!code_.pixel) {
        reason_ = "a vertex shader's texture sample (vertex texture fetch) is not translated yet";
        return false;
      }
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
    case D3DSIO_TEXBEM:
    case D3DSIO_TEXBEML:
    case D3DSIO_TEXM3x2TEX:
    case D3DSIO_TEXM3x3TEX:
    case D3DSIO_TEXDP3TEX:
      reason_ = "the texture instructions of shader model 1.x (texbem, texm3x*tex, texdp3tex) are not translated "
                "yet";
      return false;
    default:
      reason_ = "an instruction this translator does not read yet (d3dbc opcode " + std::to_string(opcode) + ")";
      return false;
    }
  }

  // ---- operands --------------------------------------------------------------------------------------------------
  bool destination_operand(const Param &p, std::vector<DWORD> &out) {
    switch (p.regtype) {
    case D3DSPR_TEMP:
      out = dxbc::dest_temp(p.regnum, p.mask);
      return true;
    case D3DSPR_ADDR:
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
    // a constant the program defined is read as the immediate it defined
    if (p.regtype == D3DSPR_CONST && !p.relative && p.regnum < kFloatConstants && defined_valid_[p.regnum]) {
      float values[4];
      for (UINT at = 0; at < 4; at++)
        values[at] = defined_[p.regnum][(p.swizzle >> (2 * at)) & 3];
      switch (p.modifier) {
      case kModNegate:
        for (float &value : values)
          value = -value;
        break;
      case kModAbs:
        for (float &value : values)
          value = value < 0 ? -value : value;
        break;
      case kModAbsNegate:
        for (float &value : values)
          value = value < 0 ? value : -value;
        break;
      case kModNone:
        break;
      default:
        reason_ = "a source modifier on a defined constant that is not negate or absolute";
        return false;
      }
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
    default:
      reason_ = "a source modifier (bias, sign, complement, dz, dw or x2) this translator does not read yet";
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
      out = dxbc::src_input(code_.pixel ? kSlotColour + std::min<UINT>(p.regnum, 1) : p.regnum, p.swizzle, modifier);
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
      out = dxbc::src_input(kSlotTexcoord + p.regnum, p.swizzle, modifier);
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
    default:
      return false;
    }
  }

  // a negated operand: the extended operand token when it has none, the other sign when it has one
  static void negate_operand(std::vector<DWORD> &operand) {
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
  bool alpha_test_ = false;
  bool alpha_compare_ = false;
  UINT colour_shadow_ = UINT_MAX;
  const std::vector<VertexInput> *vertex_inputs_ = nullptr;
  // the pixel shader's inputs, when a vertex shader is being translated against the shader it draws with
  const std::vector<dxbc::SignatureElement> *pixel_inputs_of_stage_ = nullptr;
  std::unique_ptr<dxbc::Writer> writer_;
  std::string reason_;
  UINT max_temp_ = 0;
  UINT scratch_ = 0;
  // the temporary each input register is read through, when its declaration carries fewer than four components
  std::vector<UINT> input_shadow_;
  bool defined_valid_[kFloatConstants] = {};
  float defined_[kFloatConstants][4] = {};
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
  bool uses_colour_out_[4] = {};
  std::vector<dxbc::SignatureElement> inputs_;
  std::vector<dxbc::SignatureElement> outputs_;
};

} // namespace

const char *
usage_name(UINT usage) {
  return usage_name_of(usage);
}

UINT
declaration_type_mask(UINT type) {
  return mask_of_type(type);
}

bool
translate_shader(
    const ShaderCode &code, const std::vector<VertexInput> *vertex_inputs,
    const std::vector<dxbc::SignatureElement> *pixel_inputs, const AlphaTest *alpha, TranslatedProgram &out,
    std::string &reason
) {
  Translator translator(code, alpha);
  if (translator.run(vertex_inputs, pixel_inputs, out))
    return true;
  reason = translator.reason();
  return false;
}

} // namespace mullion::d3d9
