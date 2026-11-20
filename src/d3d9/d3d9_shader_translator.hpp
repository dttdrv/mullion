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
  // whether the program samples a texture, which is what tells a draw to bind the stage's texture and sampler
  bool samples = false;
  // whether a vertex program writes oFog (FogSource::FogCoord when table fog is off)
  bool writes_fog = false;
};

// translates `code`, whose vertex inputs are `vertex_inputs` for a vertex shader (null for a pixel shader, whose
// declarations carry its own). false leaves `reason` naming what the translator does not read yet
// `pixel_inputs` is the pixel shader's input signature when a vertex shader is translated against the shader it draws
// with: the two stages link by register (the compiled programs name a varying after its register and component), so a
// vertex shader's output is written where the pixel shader reads it. null for a pixel shader's own translation
// `alpha` is D3D9's alpha test, which only a pixel shader carries: null, or disabled, for a program that has none
// `fog` is D3D9 fog applied after a pixel program: null, or FogSource::None, for a program that has none
// `project` is D3DTSS_TEXTURETRANSFORMFLAGS PROJECTED for ps_1.x `tex`: null, or divide_component 0, for none
// `clip_planes` is whether a vertex shader writes D3D9's six user clip planes' distances (SV_ClipDistance0 and 1, after
// every other output) from the planes it reads at b3 in clip space; a pixel shader's translation ignores it
// `texcoord_slot` is where TEXCOORD0 starts: the unlit fixed-function textured vertex shaders write it at 2, while the
// lit/eight-stage families and translated game vertex shaders reserve 2 for COLOR1 and write it at 3 (the default)
// `shadow_mask` is the pixel stages whose bound texture is a depth-stencil one: Direct3D 9 comparison-samples a depth
// texture in a pixel shader of model 2.0 and above, so that stage's texld/texldp emit the comparison the sample answers
// (d3d9_shader.hpp ShadowState), not the texel. vertex shaders and ps_1.x keep the plain sample
// whether `opcode` is an instruction this build's translator reads: CreateVertexShader and CreatePixelShader refuse a
// program naming one it does not, as D3D9 refuses it, rather than at the first draw that translates it
bool opcode_readable(UINT opcode);

bool translate_shader(
    const ShaderCode &code, const std::vector<VertexInput> *vertex_inputs,
    const std::vector<dxbc::SignatureElement> *pixel_inputs, const AlphaTest *alpha, const FogState *fog,
    const TexProjectState *project, const BumpEnvState *bump, bool clip_planes, TranslatedProgram &out,
    std::string &reason, UINT texcoord_slot = 3, bool flat = false, UINT shadow_mask = 0
);

// the semantic name a D3D9 declaration usage is written as in a signature
const char *usage_name(UINT usage);

// how many components an element's mask has, from its D3DDECLTYPE (0 for a type this build has no layout format for)
UINT declaration_type_mask(UINT type);

} // namespace mullion::d3d9
