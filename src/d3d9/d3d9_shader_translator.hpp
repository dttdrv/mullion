// a game's d3dbc program as the DXBC container this build's D3D11 device takes: the program's instructions are read as
// the tokens Direct3D 9 defines them (d3d9types.h's D3DSIO_* and D3DSP_*), and written out as the shader model 5
// instructions of d3d9_dxbc.hpp, which is what airconv compiles to Metal. what a program's registers mean is D3D9's:
// a vertex shader's inputs are the vertex declaration's elements, its outputs are oPos/oFog/oPts/oD#/oT#, and a pixel
// shader's are v# (the two colours) and t# (the texture coordinates), with oC# its targets.
//
// a translation is per draw, not per shader, because a vertex shader's input signature is the declaration the draw
// reads (d3d9_device_draw.cpp), so the compiled shaders are the device's cache, keyed by the program and the
// declaration it was translated with.
#pragma once
#include "d3d9_dxbc.hpp"
#include "d3d9_shader.hpp"
#include <string>
#include <vector>

namespace mullion::d3d9 {

// a translated program: the container, and the signatures the D3D11 input layout and the two stages link through
struct TranslatedProgram {
  std::vector<DWORD> dxbc;
  std::vector<dxbc::SignatureElement> inputs;
  std::vector<dxbc::SignatureElement> outputs;
};

// translates `code`, whose vertex inputs are `vertex_inputs` for a vertex shader (null for a pixel shader, whose
// declarations carry its own). false leaves `reason` naming what the translator does not read yet
// `pixel_inputs` is the pixel shader's input signature when a vertex shader is translated against the shader it draws
// with: the two stages link by register (the compiled programs name a varying after its register and component), so a
// vertex shader's output is written where the pixel shader reads it. null for a pixel shader's own translation
bool translate_shader(
    const ShaderCode &code, const std::vector<VertexInput> *vertex_inputs,
    const std::vector<dxbc::SignatureElement> *pixel_inputs, TranslatedProgram &out, std::string &reason
);

// the semantic name a D3D9 declaration usage is written as in a signature
const char *usage_name(UINT usage);

// how many components an element's mask has, from its D3DDECLTYPE (0 for a type this build has no layout format for)
UINT declaration_type_mask(UINT type);

} // namespace mullion::d3d9
