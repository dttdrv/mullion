// a DXBC container for the shaders this build makes out of a game's d3dbc program: the input and output signatures the
// two stages link through, the SHEX token stream this build emits (shader model 5, which is what the D3D11 device under
// this layer compiles through airconv), and the container that holds them. the token encodings are the format's own,
// from libs/DXBCParser/d3d12tokenizedprogramformat.hpp; a container needs no RDEF chunk for the shaders this build
// makes (a d3dbc program's resources and samplers are the instructions' own registers, and this build declares none).
//
// header only, so the writer can be proved on its own by a test that hands its output straight to a D3D11 device
// (tests/d3d9/d3d9_dxbc.cpp) before any d3dbc translation is involved.
#pragma once
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "d3d9_common.hpp"
#include "util_md5.hpp"
#include <cstring>
#include <string>
#include <vector>

namespace mullion::d3d9::dxbc {

// the token format's names, which the header keeps in its own namespace (as the Windows SDK does)
using namespace microsoft;

// the two stages a d3dbc program names, as D3D10_SB_TOKENIZED_PROGRAM_TYPE does
enum class Stage : UINT { Vertex = D3D10_SB_VERTEX_SHADER, Pixel = D3D10_SB_PIXEL_SHADER };

// one signature element: what a register of the shader means to the stage on the other side of it. `system_value` is a
// D3D_NAME (POSITION for the vertex shader's SV_Position, UNDEFINED for a name of its own)
struct SignatureElement {
  std::string semantic;
  UINT index = 0;
  UINT reg = 0;
  UINT mask = 0xF;
  UINT system_value = D3D_NAME_UNDEFINED;
};

namespace detail {

// the swizzle field d3dbc and DXBC share: two bits per component, x in the low two
inline DWORD swizzle_bits(DWORD swizzle) {
  return ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE(
      swizzle & 3, (swizzle >> 2) & 3, (swizzle >> 4) & 3, (swizzle >> 6) & 3
  );
}

// an operand token: four components, `selection` bits (a mask or a swizzle), its type, and how many indices follow
inline DWORD operand_token(UINT type, DWORD selection, UINT index_dimension) {
  return ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | selection |
         ENCODE_D3D10_SB_OPERAND_TYPE(type) |
         ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(index_dimension) |
         ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
         (index_dimension >= D3D10_SB_OPERAND_INDEX_2D
              ? ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
              : 0);
}

inline std::vector<DWORD> with_modifier(std::vector<DWORD> operand, D3D10_SB_OPERAND_MODIFIER modifier) {
  if (modifier == D3D10_SB_OPERAND_MODIFIER_NONE)
    return operand;
  operand[0] |= D3D10_SB_OPERAND_EXTENDED_MASK;
  operand.push_back(ENCODE_D3D10_SB_EXTENDED_OPERAND_MODIFIER(modifier));
  return operand;
}

} // namespace detail

// a destination: a write mask of the components the instruction writes
inline std::vector<DWORD> dest_temp(UINT reg, UINT mask = 0xF) {
  return {detail::operand_token(
              D3D10_SB_OPERAND_TYPE_TEMP,
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
                  ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask << 4),
              D3D10_SB_OPERAND_INDEX_1D
          ),
          reg};
}

inline std::vector<DWORD> dest_output(UINT reg, UINT mask = 0xF) {
  return {detail::operand_token(
              D3D10_SB_OPERAND_TYPE_OUTPUT,
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
                  ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask << 4),
              D3D10_SB_OPERAND_INDEX_1D
          ),
          reg};
}

// a source: the swizzle its components are read with, and the modifier (negate, absolute) it is read through
inline std::vector<DWORD> src_temp(UINT reg, DWORD swizzle = 0xE4, D3D10_SB_OPERAND_MODIFIER modifier = D3D10_SB_OPERAND_MODIFIER_NONE) {
  return detail::with_modifier(
      {detail::operand_token(
           D3D10_SB_OPERAND_TYPE_TEMP,
           ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
               detail::swizzle_bits(swizzle),
           D3D10_SB_OPERAND_INDEX_1D
       ),
       reg},
      modifier
  );
}

inline std::vector<DWORD> src_input(
    UINT reg, DWORD swizzle = 0xE4, D3D10_SB_OPERAND_MODIFIER modifier = D3D10_SB_OPERAND_MODIFIER_NONE
) {
  return detail::with_modifier(
      {detail::operand_token(
           D3D10_SB_OPERAND_TYPE_INPUT,
           ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
               detail::swizzle_bits(swizzle),
           D3D10_SB_OPERAND_INDEX_1D
       ),
       reg},
      modifier
  );
}

// a D3D9 constant register: `slot` is the constant buffer (0 float, 1 integer, 2 boolean), `element` the register
inline std::vector<DWORD> src_constant(
    UINT slot, UINT element, DWORD swizzle = 0xE4, D3D10_SB_OPERAND_MODIFIER modifier = D3D10_SB_OPERAND_MODIFIER_NONE
) {
  return detail::with_modifier(
      {detail::operand_token(
           D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER,
           ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
               detail::swizzle_bits(swizzle),
           D3D10_SB_OPERAND_INDEX_2D
       ),
       slot, element},
      modifier
  );
}

// one component of a value: the immediate an instruction's single component is written from. its token says one
// component, not four (a compiler's own `mov o0.w, l(1.000000)` has exactly this token), because the count is what
// tells a reader how many words the operand follows with: a token that says four over a single word is a program whose
// instructions after it are read from the wrong place
inline std::vector<DWORD> src_immediate_value(float value) {
  DWORD word = 0;
  memcpy(&word, &value, 4);
  return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT) |
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
              ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) |
              ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_0D),
          word};
}

inline std::vector<DWORD> src_immediate(float x, float y, float z, float w) {
  DWORD values[4];
  memcpy(values, &x, 4);
  memcpy(values + 1, &y, 4);
  memcpy(values + 2, &z, 4);
  memcpy(values + 3, &w, 4);
  return {detail::operand_token(
              D3D10_SB_OPERAND_TYPE_IMMEDIATE32,
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
                  ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(0xF0),
              D3D10_SB_OPERAND_INDEX_0D
          ),
          values[0], values[1], values[2], values[3]};
}

// the shader a stage draws with, as the tokens of its program and the signatures it links through
class Writer {
public:
  explicit Writer(Stage stage) : stage_(stage) {}

  // ---- signatures -----------------------------------------------------------------------------------------------
  void input(SignatureElement element) { inputs_.push_back(std::move(element)); }
  void output(SignatureElement element) { outputs_.push_back(std::move(element)); }

  // ---- instructions ---------------------------------------------------------------------------------------------
  // mov, which the prologue of a translation completes an input register with
  void mov(const std::vector<DWORD> &dest, const std::vector<DWORD> &src) { instruction(D3D10_SB_OPCODE_MOV, false, {dest, src}); }

  // ---- declarations ---------------------------------------------------------------------------------------------
  void temps(UINT count) {
    if (count < 1)
      count = 1;
    raw(D3D10_SB_OPCODE_DCL_TEMPS, {count});
  }
  // a stage's input: a vertex shader's is a declaration of the register the input signature fills, a pixel shader's
  // says how the value between the vertices is made, which for a game's d3dbc program is always linear interpolation of
  // the triangle's three values
  void declare_input(UINT reg, UINT mask = 0xF) {
    if (stage_ == Stage::Pixel) {
      program_.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT_PS) |
                         ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(3) |
                         ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_LINEAR));
      program_.push_back(detail::operand_token(
          D3D10_SB_OPERAND_TYPE_INPUT,
          ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask << 4),
          D3D10_SB_OPERAND_INDEX_1D
      ));
      program_.push_back(reg);
      return;
    }
    raw(D3D10_SB_OPCODE_DCL_INPUT, {detail::operand_token(
                                       D3D10_SB_OPERAND_TYPE_INPUT,
                                       ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(
                                           D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE
                                       ) |
                                           ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask << 4),
                                       D3D10_SB_OPERAND_INDEX_1D
                                   ),
                                   reg});
  }
  void declare_output(UINT reg, UINT mask = 0xF) {
    raw(D3D10_SB_OPCODE_DCL_OUTPUT, {detail::operand_token(
                                        D3D10_SB_OPERAND_TYPE_OUTPUT,
                                        ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(
                                            D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE
                                        ) |
                                            ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask << 4),
                                        D3D10_SB_OPERAND_INDEX_1D
                                    ),
                                    reg});
  }
  // an output that is a system value (the position), which the program has to say in its own declarations and not
  // only in the output signature: the shader the pipeline makes is read from them
  void declare_output_siv(UINT reg, UINT name, UINT mask = 0xF) {
    raw(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, {detail::operand_token(
                                            D3D10_SB_OPERAND_TYPE_OUTPUT,
                                            ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(
                                                D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE
                                            ) |
                                                ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask << 4),
                                            D3D10_SB_OPERAND_INDEX_1D
                                        ),
                                        reg,
                                        name});
  }
  // the constant buffer at `slot`, holding `count` four-component registers, read through immediate indices
  void declare_constant_buffer(UINT slot, UINT count) {
    raw(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {detail::operand_token(
                                                 D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER,
                                                 ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(
                                                     D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE
                                                 ) |
                                                     ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(0xF0),
                                                 D3D10_SB_OPERAND_INDEX_2D
                                             ),
                                             slot,
                                             count});
  }

  // ---- instructions ---------------------------------------------------------------------------------------------
  // one instruction: its opcode, whether it saturates its result, and the operands' tokens
  void instruction(
      D3D10_SB_OPCODE_TYPE opcode, bool saturate, std::initializer_list<std::vector<DWORD>> operands
  ) {
    std::vector<DWORD> tokens;
    for (const auto &operand : operands)
      tokens.insert(tokens.end(), operand.begin(), operand.end());
    const DWORD head = ENCODE_D3D10_SB_OPCODE_TYPE(opcode) |
                       ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((DWORD)(1 + tokens.size())) |
                       ENCODE_D3D10_SB_INSTRUCTION_SATURATE(saturate);
    program_.push_back(head);
    program_.insert(program_.end(), tokens.begin(), tokens.end());
  }

  // the container: the two signatures, then the program and its ret
  std::vector<DWORD> container() const {
    std::vector<DWORD> program = program_;
    program.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET) |
                      ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1));

    // the SHEX chunk: the program's version token (which stage it is of, and shader model 5.0), the length of what
    // follows in words (which counts the two, as a compiler's own container has it), then the instructions
    std::vector<DWORD> shex;
    shex.push_back(ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN((UINT)stage_, 5, 0));
    shex.push_back((DWORD)(2 + program.size()));
    shex.insert(shex.end(), program.begin(), program.end());

    const auto isgn = signature(inputs_, true);
    const auto osgn = signature(outputs_, false);

    struct Chunk {
      char fourcc[4];
      const std::vector<DWORD> *data;
    };
    const Chunk chunks[] = {{{'I', 'S', 'G', 'N'}, &isgn}, {{'O', 'S', 'G', 'N'}, &osgn}, {{'S', 'H', 'E', 'X'}, &shex}};

    // 'DXBC', the checksum (filled in below), the version, the container's size, and the offsets of the chunks
    constexpr UINT kHeaderWords = 4 + 1 + 1 + 1 + 1;
    std::vector<DWORD> container;
    container.reserve(kHeaderWords + 3 * 2 + 64);
    container.push_back(0x43425844); // 'DXBC', little endian
    container.push_back(0);          // checksum[0]
    container.push_back(0);          // checksum[1]
    container.push_back(0);          // checksum[2]
    container.push_back(0);          // checksum[3]
    container.push_back(1);          // version
    const UINT size_word = (UINT)container.size();
    container.push_back(0);          // the container's size, filled in below
    container.push_back(ARRAYSIZE(chunks));
    const UINT offsets_word = (UINT)container.size();
    container.resize(container.size() + ARRAYSIZE(chunks), 0);

    UINT word = (UINT)container.size();
    for (UINT at = 0; at < ARRAYSIZE(chunks); at++) {
      container[offsets_word + at] = word * 4;
      container.push_back(((DWORD)(BYTE)chunks[at].fourcc[0]) | ((DWORD)(BYTE)chunks[at].fourcc[1] << 8) |
                          ((DWORD)(BYTE)chunks[at].fourcc[2] << 16) | ((DWORD)(BYTE)chunks[at].fourcc[3] << 24));
      container.push_back((DWORD)chunks[at].data->size() * 4);
      container.insert(container.end(), chunks[at].data->begin(), chunks[at].data->end());
      word = (UINT)container.size();
    }
    container[size_word] = (DWORD)container.size() * 4;

    // the checksum Direct3D refuses a container without, which this build's own check (d3d11_pipeline_cache.cpp) asks
    // for as well: the hash of what follows the checksum, which is the container's bytes from the version on
    const auto hash = md5::hashDxbcBinary(container.data(), container.size() * sizeof(DWORD));
    memcpy(&container[1], hash.data.data(), sizeof(hash.data));
    return container;
  }

private:
  void raw(D3D10_SB_OPCODE_TYPE opcode, std::initializer_list<DWORD> following) {
    program_.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(opcode) |
                       ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((DWORD)(1 + following.size())));
    program_.insert(program_.end(), following.begin(), following.end());
  }

  // a signature chunk, as shader model 5 writes one (which is what a compiler's own container has: a signature of
  // chrome's, its count and the offset of the entries, then six words of each element, then the names): the elements,
  // each with the offset of its name, and the names themselves
  static std::vector<DWORD> signature(const std::vector<SignatureElement> &elements, bool input) {
    const UINT count = (UINT)elements.size();
    // the header is the count and the offset of the first element, which follows the two
    constexpr UINT kHeaderBytes = 2 * sizeof(DWORD);
    // an element is 24 bytes: the name's offset, its index, the system value, the component type, the register, and
    // the two mask bytes with the padding after them
    constexpr UINT kElementBytes = 6 * sizeof(DWORD);

    std::vector<DWORD> chunk;
    chunk.push_back(count);
    chunk.push_back(kHeaderBytes);

    std::vector<char> strings;
    for (UINT at = 0; at < count; at++) {
      const UINT name_offset = kHeaderBytes + kElementBytes * count + (UINT)strings.size();
      chunk.push_back(name_offset);
      chunk.push_back(elements[at].index);
      chunk.push_back(elements[at].system_value);
      chunk.push_back(elements[at].semantic.empty() ? D3D_REGISTER_COMPONENT_UNKNOWN : D3D_REGISTER_COMPONENT_FLOAT32);
      chunk.push_back(elements[at].reg);
      const BYTE mask = (BYTE)elements[at].mask;
      // an input's second mask byte says which components the shader always reads, an output's which it never writes
      const BYTE mask_word[4] = {mask, (BYTE)(input ? mask : 0), 0, 0};
      DWORD packed = 0;
      memcpy(&packed, mask_word, sizeof(packed));
      chunk.push_back(packed);
      strings.insert(strings.end(), elements[at].semantic.begin(), elements[at].semantic.end());
      strings.push_back('\0');
      // the names are padded to four bytes with the byte a compiler pads them with
      while (strings.size() % 4)
        strings.push_back((char)0xAB);
    }
    const UINT words = (UINT)(strings.size() / 4);
    for (UINT at = 0; at < words; at++) {
      DWORD word = 0;
      memcpy(&word, strings.data() + at * 4, 4);
      chunk.push_back(word);
    }
    return chunk;
  }

  Stage stage_;
  std::vector<SignatureElement> inputs_;
  std::vector<SignatureElement> outputs_;
  std::vector<DWORD> program_;
};

} // namespace mullion::d3d9::dxbc
