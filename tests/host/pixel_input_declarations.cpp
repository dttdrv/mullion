// Ignored pixel inputs must add neither a function argument nor a register initializer.
#include "dxbc_converter.hpp"
#include <cstdio>

using namespace microsoft;
using namespace mullion::dxbc;

int
main() {
  unsigned wrong = 0;
  for (auto opcode : {D3D10_SB_OPCODE_DCL_INPUT_PS_SIV, D3D10_SB_OPCODE_DCL_INPUT_PS_SGV}) {
    auto supported = opcode == D3D10_SB_OPCODE_DCL_INPUT_PS_SIV ? D3D10_SB_NAME_POSITION : D3D10_SB_NAME_IS_FRONT_FACE;
    for (auto name : {D3D10_SB_NAME_VERTEX_ID, supported}) {
      SM50ShaderInternal shader{};
      shader.shader_type = D3D10_SB_PIXEL_SHADER;
      CSignatureParser input;
      CSignatureParser5 output;
      D3D10ShaderBinary::CInstruction declaration(opcode);
      declaration.m_Operands[0].m_Index[0].m_RegIndex = 7;
      declaration.m_Operands[0].m_WriteMask = D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL;
      declaration.m_InputPSDeclSIV.Name = name;
      declaration.m_InputPSDeclSIV.InterpolationMode = D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE;
      handle_signature(input, output, declaration, &shader, 0);

      unsigned expected = name == supported;
      if (shader.signature_handlers.size() != expected || shader.func_signature.InputCount() != expected ||
          shader.max_input_register != 8 * expected) {
        printf("wrong: opcode %u, system value %u: handlers=%zu inputs=%u registers=%u\n",
               unsigned(opcode), unsigned(name), shader.signature_handlers.size(),
               unsigned(shader.func_signature.InputCount()), unsigned(shader.max_input_register));
        wrong++;
      }
    }
  }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
