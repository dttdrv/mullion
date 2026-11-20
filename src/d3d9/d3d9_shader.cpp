// the d3dbc programs a game makes and the objects it holds them in: D3D9's shader binary is its own token stream
// (not DXBC), so this reads the version and the instruction stream here, and the draw that uses one translates it to
// the DXBC an airconv pipeline takes (d3d9_shader_translator.hpp).
#include "d3d9_shader.hpp"
#include "d3d9_device.hpp"
#include "d3d9_shader_translator.hpp"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9VertexShader = {0x2b6f1c94, 0x4a3d, 0x4f8e, {0x8c, 0x51, 0x9e, 0x62, 0x3a, 0x77, 0x0d, 0x41}};
const IID IID_IMTLD3D9PixelShader = {0x5d1b8e42, 0x7c96, 0x4a0f, {0x8b, 0x24, 0x6d, 0x51, 0x0c, 0x9f, 0x3e, 0x72}};

namespace {

std::mutex g_vs_registry_lock;
std::unordered_map<IDirect3DVertexShader9 *, IMTLD3D9VertexShader *> g_vs_registry;
std::mutex g_ps_registry_lock;
std::unordered_map<IDirect3DPixelShader9 *, IMTLD3D9PixelShader *> g_ps_registry;

} // namespace

void
register_vertex_shader(IDirect3DVertexShader9 *iface, IMTLD3D9VertexShader *impl) {
  if (!iface || !impl)
    return;
  std::lock_guard<std::mutex> lock(g_vs_registry_lock);
  g_vs_registry[iface] = impl;
}

void
unregister_vertex_shader(IDirect3DVertexShader9 *iface) {
  if (!iface)
    return;
  std::lock_guard<std::mutex> lock(g_vs_registry_lock);
  g_vs_registry.erase(iface);
}

IMTLD3D9VertexShader *
lookup_vertex_shader(IDirect3DVertexShader9 *iface) {
  if (!iface)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_vs_registry_lock);
  auto found = g_vs_registry.find(iface);
  return found == g_vs_registry.end() ? nullptr : found->second;
}

void
register_pixel_shader(IDirect3DPixelShader9 *iface, IMTLD3D9PixelShader *impl) {
  if (!iface || !impl)
    return;
  std::lock_guard<std::mutex> lock(g_ps_registry_lock);
  g_ps_registry[iface] = impl;
}

void
unregister_pixel_shader(IDirect3DPixelShader9 *iface) {
  if (!iface)
    return;
  std::lock_guard<std::mutex> lock(g_ps_registry_lock);
  g_ps_registry.erase(iface);
}

IMTLD3D9PixelShader *
lookup_pixel_shader(IDirect3DPixelShader9 *iface) {
  if (!iface)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_ps_registry_lock);
  auto found = g_ps_registry.find(iface);
  return found == g_ps_registry.end() ? nullptr : found->second;
}

namespace {

// D3D9's program versions: the two high halves of the first token, and the majors and minors this build reads
constexpr DWORD kVertexVersion = 0xFFFE0000;
constexpr DWORD kPixelVersion = 0xFFFF0000;
constexpr DWORD kShaderVersionMask = 0xFFFF0000;

// the end of a program and a comment, from d3d9types.h (D3DSIO_END, D3DSIO_COMMENT)
constexpr DWORD kOpcodeEnd = 0xFFFF;
constexpr DWORD kOpcodeComment = 0xFFFE;

// the versions this build makes a shader of, which are the ones its caps name: a vertex shader of 1.1, 2.0 or 3.0, and a
// pixel shader of 1.1 through 1.4, 2.0 or 3.0. a program of a version above them is refused here rather than made and
// refused at the draw, which is what a device whose caps say 3.0 does (a device that made a higher shader and then
// could not draw with it would lie to the game twice). a pixel shader of 1.x is lifted onto the same translator as 2.0
// (d3d9_shader_translator.cpp): r0 is its colour, and `tex` samples the stage of that number
bool supported(bool pixel, UINT major, UINT minor) {
  if (major == 3)
    return minor == 0;
  if (pixel && major == 1)
    return minor >= 1 && minor <= 4;
  if (pixel)
    return major == 2 && minor == 0;
  if (major == 1)
    return minor == 1;
  return major == 2 && minor == 0;
}

} // namespace

// how many tokens a program is, 0 for one this build does not read. D3D9 hands CreateVertexShader and CreatePixelShader
// a pointer and no length, so the stream's own END token is where it ends; the walk is bounded, so a corrupt stream
// without one is refused rather than followed
UINT
program_tokens(const DWORD *code) {
  constexpr UINT kMost = 1u << 16;
  if (!code)
    return 0;
  const DWORD version = code[0];
  const DWORD kind = version & kShaderVersionMask;
  if (kind != kVertexVersion && kind != kPixelVersion)
    return 0;
  const bool pixel = kind == kPixelVersion;
  const UINT major = (version >> 8) & 0xFF;
  const UINT minor = version & 0xFF;
  if (!supported(pixel, major, minor))
    return 0;
  if (major >= 2) {
    for (UINT at = 1; at < kMost;) {
      const DWORD token = code[at];
      const DWORD opcode = token & 0xFFFF;
      if (opcode == kOpcodeEnd)
        return at + 1;
      const UINT length = opcode == kOpcodeComment ? (token >> 16) & 0x7FFF : (token >> 24) & 0xF;
      if (at + 1 + length > kMost)
        return 0;
      at += 1 + length;
    }
    return 0;
  }
  for (UINT at = 1; at < kMost; at++)
    if ((code[at] & 0xFFFF) == kOpcodeEnd && !(code[at] & 0x80000000))
      return at + 1;
  return 0;
}

namespace {

UINT
shader_register_type(DWORD token) {
  return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) | ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}

// Profile constant limits Create*Shader enforces (Wine device.c test_shader_inputs / constant index fixtures).
bool
constants_within_profile(const ShaderCode &code) {
  const UINT float_limit = !code.pixel ? 256u : code.major >= 3 ? 224u : code.major >= 2 ? 32u : 8u;
  const bool allow_int_bool = !code.pixel || code.major >= 3;
  const DWORD *tokens = code.tokens.data();
  const UINT count = (UINT)code.tokens.size();
  auto check_reg = [&](DWORD token) -> bool {
    const UINT type = shader_register_type(token);
    const UINT num = token & D3DSP_REGNUM_MASK;
    if (type == D3DSPR_CONST || type == D3DSPR_CONST2 || type == D3DSPR_CONST3 || type == D3DSPR_CONST4)
      return num < float_limit;
    if (type == D3DSPR_CONSTINT || type == D3DSPR_CONSTBOOL)
      return allow_int_bool && num < 16u;
    return true;
  };
  if (code.major >= 2) {
    for (UINT at = 1; at < count;) {
      const DWORD token = tokens[at];
      const DWORD opcode = token & 0xFFFF;
      if (opcode == kOpcodeEnd)
        break;
      const UINT length = opcode == kOpcodeComment ? (token >> 16) & 0x7FFF : (token >> 24) & 0xF;
      if (at + 1 + length > count)
        return false;
      if (opcode != kOpcodeComment) {
        // def/defi/defb: only the destination is a register; the following words are the immediate value
        // (Wine visual.c sincos_test embeds D3DSIO_DEF with float payloads that must not be read as registers)
        const UINT reg_words = (opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB) ? 1u : length;
        for (UINT i = 0; i < reg_words && i < length; i++) {
          if (!check_reg(tokens[at + 1 + i]))
            return false;
        }
      }
      at += 1 + length;
    }
    return true;
  }
  for (UINT at = 1; at < count;) {
    const DWORD token = tokens[at];
    if ((token & 0xFFFF) == kOpcodeEnd && !(token & 0x80000000))
      break;
    // Skip comment chunks (e.g. the CTAB d3dcompiler embeds): their body words are arbitrary data
    // that must not be scanned as register or instruction tokens.
    if ((token & 0xFFFF) == kOpcodeComment) {
      at += 1 + ((token >> 16) & 0x7FFF);
      continue;
    }
    if ((token & 0x80000000) && !check_reg(token))
      return false;
    at++;
  }
  return true;
}

// a sampler register a program names: a pixel shader samples s0..s15, a vs_3_0 its four vertex samplers s0..s3. the caps'
// MaxSimultaneousTextures is the pixel count and does not extend a vertex shader's registers, so the two limits are told
// apart here, where the program's profile is known, rather than at the first draw
bool
sampler_within_profile(const ShaderCode &code, DWORD token) {
  if (shader_register_type(token) != D3DSPR_SAMPLER)
    return true;
  const UINT limit = code.pixel ? 16 : 4;
  return (token & D3DSP_REGNUM_MASK) < limit;
}

// whether every instruction in the program is one the translator reads. D3D9 refuses a shader naming an opcode it does
// not know when the shader is created, and this build does the same rather than at the first draw that translates it
bool
instructions_readable(const ShaderCode &code) {
  const DWORD *tokens = code.tokens.data();
  const UINT count = (UINT)code.tokens.size();
  if (code.major >= 2) {
    for (UINT at = 1; at < count;) {
      const DWORD token = tokens[at];
      const DWORD opcode = token & 0xFFFF;
      if (opcode == kOpcodeEnd)
        return true;
      const UINT length = opcode == kOpcodeComment ? (token >> 16) & 0x7FFF : (token >> 24) & 0xF;
      if (at + 1 + length > count)
        return false;
      if (opcode != kOpcodeComment && !mullion::d3d9::opcode_readable(opcode)) {
        ERR_ONCE("D3D9: a shader naming instruction ", opcode, " was refused.");
        return false;
      }
      if (opcode == D3DSIO_DCL && length >= 2 && !sampler_within_profile(code, tokens[at + 2])) {
        ERR_ONCE("D3D9: a shader declaring a sampler past its profile's registers was refused.");
        return false;
      }
      if ((opcode == D3DSIO_TEX || opcode == D3DSIO_TEXLDL || opcode == D3DSIO_TEXLDD) && length >= 3 &&
          !sampler_within_profile(code, tokens[at + 3])) {
        ERR_ONCE("D3D9: a shader sampling a sampler past its profile's registers was refused.");
        return false;
      }
      at += 1 + length;
    }
    return false;
  }
  for (UINT at = 1; at < count; at++) {
    const DWORD token = tokens[at];
    if ((token & 0xFFFF) == kOpcodeEnd && !(token & 0x80000000))
      return true;
    // shader model 1 has no instruction length in its tokens: an instruction is a word without the register bit, and a
    // comment's payload is skipped by the length field its own token carries
    if (token & 0x80000000)
      continue;
    if ((token & 0xFFFF) == kOpcodeComment) {
      at += (token >> 16) & 0x7FFF;
      continue;
    }
    if (!mullion::d3d9::opcode_readable(token & 0xFFFF))
      return false;
  }
  return false;
}

bool
shader_writes_fog(const ShaderCode &code) {
  if (code.pixel)
    return false;
  const DWORD *tokens = code.tokens.data();
  const UINT count = (UINT)code.tokens.size();
  if (code.major >= 2) {
    for (UINT at = 1; at < count;) {
      const DWORD token = tokens[at];
      const DWORD opcode = token & 0xFFFF;
      if (opcode == kOpcodeEnd)
        break;
      const UINT length = opcode == kOpcodeComment ? (token >> 16) & 0x7FFF : (token >> 24) & 0xF;
      if (opcode != kOpcodeComment && length >= 1) {
        const DWORD dest = tokens[at + 1];
        if (shader_register_type(dest) == D3DSPR_RASTOUT && (dest & D3DSP_REGNUM_MASK) == 1)
          return true;
        // shader model 3 declares its outputs: dcl_fog o# names the fog interpolator, which is not RASTOUT there
        // (Wine visual.c fog_with_shader_test's vs_3_0).
        if (opcode == D3DSIO_DCL && length >= 2 && (tokens[at + 1] & D3DSP_DCL_USAGE_MASK) == D3DDECLUSAGE_FOG) {
          const UINT type = shader_register_type(tokens[at + 2]);
          if (type == D3DSPR_OUTPUT || type == D3DSPR_RASTOUT)
            return true;
        }
      }
      at += 1 + length;
    }
    return false;
  }
  for (UINT at = 1; at < count;) {
    const DWORD token = tokens[at];
    if ((token & 0xFFFF) == kOpcodeEnd && !(token & 0x80000000))
      break;
    // Skip comment chunks (e.g. the CTAB d3dcompiler embeds): their body words are arbitrary data.
    if ((token & 0xFFFF) == kOpcodeComment) {
      at += 1 + ((token >> 16) & 0x7FFF);
      continue;
    }
    if ((token & 0x80000000) && shader_register_type(token) == D3DSPR_RASTOUT && (token & D3DSP_REGNUM_MASK) == 1)
      return true;
    at++;
  }
  return false;
}

} // namespace

bool read_shader_code(const DWORD *code, ShaderCode &out) {
  const UINT count = program_tokens(code);
  if (!count)
    return false;
  const DWORD version = code[0];
  const bool pixel = (version & kShaderVersionMask) == kPixelVersion;

  out.tokens.assign(code, code + count);
  out.pixel = pixel;
  out.major = (version >> 8) & 0xFF;
  out.minor = version & 0xFF;
  if (!constants_within_profile(out))
    return false;
  if (!instructions_readable(out)) {
    ERR_ONCE("D3D9: a shader naming an instruction this build does not read was refused.");
    return false;
  }
  return true;
}

MTLD3D9VertexShader::MTLD3D9VertexShader(IDirect3DDevice9 *device, ShaderCode code, bool counted_device)
    : device_(device), device_ref_(device, counted_device),
      code_(std::move(code)), counted_device_(counted_device) {
  // oFog is RASTOUT register 1 (Microsoft Learn, vs_1_1 / vs_2_0). Detect before the first draw so fog source
  // selection does not wait on a translated variant.
  writes_fog_ = shader_writes_fog(code_);
  register_vertex_shader(static_cast<IDirect3DVertexShader9 *>(this), this);
}

HRESULT
MTLD3D9VertexShader::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DVertexShader9)) {
    *object = static_cast<IDirect3DVertexShader9 *>(this);
  } else if (riid == IID_IMTLD3D9VertexShader) {
    *object = static_cast<IMTLD3D9VertexShader *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

// the D3D11 vertex shader and input layout this program draws with through `declaration`: the program is translated
// against the declaration the first time a draw uses the pair, and the pair is kept for every draw after it
HRESULT
MTLD3D9VertexShader::compiled(
    ID3D11Device *device, const std::vector<D3DVERTEXELEMENT9> &declaration, const std::vector<VertexInput> &inputs,
    const std::vector<PixelInputSlot> &pixel_inputs, const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout,
    DWORD instanced_streams, bool clip_planes, ID3D11VertexShader **shader, ID3D11InputLayout **input_layout
) {
  if (!device || !shader || !input_layout)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  *input_layout = nullptr;
  auto same_slot = [](const PixelInputSlot &a, const PixelInputSlot &b) {
    return a.index == b.index && a.reg == b.reg && a.semantic == b.semantic;
  };
  for (auto &variant : variants_) {
    if (variant.clip_planes == clip_planes && variant.instanced_streams == instanced_streams &&
        variant.declaration.size() == declaration.size() &&
        memcmp(variant.declaration.data(), declaration.data(), declaration.size() * sizeof(D3DVERTEXELEMENT9)) == 0 &&
        variant.pixel_inputs.size() == pixel_inputs.size() &&
        std::equal(variant.pixel_inputs.begin(), variant.pixel_inputs.end(), pixel_inputs.begin(), same_slot)) {
      *shader = variant.shader.Get();
      (*shader)->AddRef();
      *input_layout = variant.layout.Get();
      (*input_layout)->AddRef();
      return S_OK;
    }
  }

  // the pixel shader's inputs, as the registers the two stages link through
  std::vector<dxbc::SignatureElement> slots;
  slots.reserve(pixel_inputs.size());
  for (const auto &input : pixel_inputs) {
    dxbc::SignatureElement element;
    element.semantic = input.semantic;
    element.index = input.index;
    element.reg = input.reg;
    slots.push_back(element);
  }

  TranslatedProgram program;
  std::string reason;
  if (!translate_shader(code_, &inputs, &slots, nullptr, nullptr, nullptr, nullptr, clip_planes, program, reason)) {
    ERR_ONCE("D3D9: a draw with this vertex shader is refused: ", reason);
    return D3DERR_INVALIDCALL;
  }

  if (const char *dump = getenv("MULLION_DXBC_DUMP")) {
    static unsigned dumped_vs = 0;
    std::string path = std::string(dump) + "\\vs" + std::to_string(++dumped_vs) + ".bin";
    if (FILE *file = fopen(path.c_str(), "wb")) {
      fwrite(program.dxbc.data(), 1, program.dxbc.size() * sizeof(DWORD), file);
      fclose(file);
    }
  }

  samples_ = samples_ || program.samples;
  writes_fog_ = writes_fog_ || program.writes_fog;

  Variant variant;
  variant.declaration = declaration;
  variant.pixel_inputs = pixel_inputs;
  variant.clip_planes = clip_planes;
  variant.instanced_streams = instanced_streams;
  HRESULT hr = device->CreateVertexShader(
      program.dxbc.data(), program.dxbc.size() * sizeof(DWORD), nullptr, &variant.shader
  );
  // a stream D3D9's SetStreamSourceFreq marks D3DSTREAMSOURCE_INSTANCEDATA advances once per instance, not once per
  // vertex: Direct3D 11 says so on the input layout's slot class, so the layout the declaration made is staged with
  // the class each of its streams has now (Wine visual.c stream_test draws the same declaration with the per-instance
  // data on a different stream from case to case)
  std::vector<D3D11_INPUT_ELEMENT_DESC> staged = layout;
  for (auto &element : staged) {
    if (element.InputSlot < 32 && (instanced_streams & (1u << element.InputSlot))) {
      element.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
      element.InstanceDataStepRate = 1;
    }
  }
  if (SUCCEEDED(hr) && !staged.empty())
    hr = device->CreateInputLayout(
        staged.data(), (UINT)staged.size(), program.dxbc.data(), program.dxbc.size() * sizeof(DWORD), &variant.layout
    );
  if (FAILED(hr)) {
    ERR_ONCE("D3D9: the D3D11 device did not take the translated vertex shader: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }

  *shader = variant.shader.Get();
  (*shader)->AddRef();
  *input_layout = variant.layout.Get();
  if (*input_layout)
    (*input_layout)->AddRef();
  variants_.push_back(std::move(variant));
  return S_OK;
}

HRESULT
MTLD3D9VertexShader::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (*device)
    (*device)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9VertexShader::GetFunction(void *data, UINT *size) {
  if (!size)
    return D3DERR_INVALIDCALL;
  const UINT bytes = (UINT)code_.tokens.size() * 4;
  if (!data) {
    *size = bytes;
    return S_OK;
  }
  if (*size < bytes)
    return D3DERR_INVALIDCALL;
  memcpy(data, code_.tokens.data(), bytes);
  *size = bytes;
  return S_OK;
}

HRESULT
MTLD3D9PixelShader::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DPixelShader9)) {
    *object = static_cast<IDirect3DPixelShader9 *>(this);
  } else if (riid == IID_IMTLD3D9PixelShader) {
    *object = static_cast<IMTLD3D9PixelShader *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

// the D3D11 pixel shader this program draws through with `alpha` and `fog`, translated the first time a draw uses that
// pair (a test that always passes is no test at all, so it is the same program as one translated with none)
HRESULT
MTLD3D9PixelShader::compiled(
    ID3D11Device *device, const AlphaTest &alpha, const FogState &fog, const TexProjectState &project,
    const BumpEnvState &bump, const ShadowState &shadow, UINT texcoord_slot, bool flat, ID3D11PixelShader **shader
) {
  if (!device || !shader)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  // D3DCMP_ALWAYS never fails, so a test with it is the program without one
  AlphaTest wanted = alpha;
  if (wanted.func == D3DCMP_ALWAYS)
    wanted.enabled = false;
  FogState wanted_fog = fog;
  TexProjectState wanted_project = project;
  const BumpEnvState &wanted_bump = bump;
  const ShadowState wanted_shadow = shadow;
  for (const Variant &variant : variants_) {
    if (variant.alpha == wanted && variant.fog == wanted_fog && variant.project == wanted_project &&
        variant.bump == wanted_bump && variant.shadow == wanted_shadow && variant.texcoord_slot == texcoord_slot &&
        variant.flat == flat) {
      *shader = variant.shader.Get();
      (*shader)->AddRef();
      // a game vertex shader translated against this variant writes the inputs where this variant reads them
      pixel_inputs_ = variant.inputs;
      return S_OK;
    }
  }

  TranslatedProgram program;
  std::string reason;
  if (!translate_shader(
          code_, nullptr, nullptr, &wanted, &wanted_fog, &wanted_project, &wanted_bump, false, program, reason,
          texcoord_slot, flat, wanted_shadow.mask
      )) {
    ERR_ONCE("D3D9: a draw with this pixel shader is refused: ", reason);
    return D3DERR_INVALIDCALL;
  }
  if (const char *dump = getenv("MULLION_DXBC_DUMP")) {
    static unsigned dumped_ps = 0;
    std::string path = std::string(dump) + "\\ps" + std::to_string(++dumped_ps) + ".bin";
    if (FILE *file = fopen(path.c_str(), "wb")) {
      fwrite(program.dxbc.data(), 1, program.dxbc.size() * sizeof(DWORD), file);
      fclose(file);
    }
  }
  samples_ = program.samples;
  alpha_tested_ = wanted.enabled;
  fog_tested_ = wanted_fog.enabled();
  pixel_inputs_.clear();
  pixel_inputs_.reserve(program.inputs.size());
  for (const auto &element : program.inputs) {
    PixelInputSlot slot;
    slot.semantic = element.semantic;
    slot.index = element.index;
    slot.reg = element.reg;
    pixel_inputs_.push_back(std::move(slot));
  }
  Variant variant;
  variant.alpha = wanted;
  variant.fog = wanted_fog;
  variant.project = wanted_project;
  variant.bump = wanted_bump;
  variant.shadow = wanted_shadow;
  variant.texcoord_slot = texcoord_slot;
  variant.flat = flat;
  variant.inputs = pixel_inputs_;
  HRESULT hr = device->CreatePixelShader(
      program.dxbc.data(), program.dxbc.size() * sizeof(DWORD), nullptr, &variant.shader
  );
  if (FAILED(hr)) {
    ERR_ONCE("D3D9: the D3D11 device did not take the translated pixel shader: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }
  *shader = variant.shader.Get();
  (*shader)->AddRef();
  variants_.push_back(std::move(variant));
  return S_OK;
}

const std::vector<PixelInputSlot> &
MTLD3D9PixelShader::pixel_inputs() {
  return pixel_inputs_;
}

HRESULT
MTLD3D9PixelShader::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (*device)
    (*device)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9PixelShader::GetFunction(void *data, UINT *size) {
  if (!size)
    return D3DERR_INVALIDCALL;
  const UINT bytes = (UINT)code_.tokens.size() * 4;
  if (!data) {
    *size = bytes;
    return S_OK;
  }
  if (*size < bytes)
    return D3DERR_INVALIDCALL;
  memcpy(data, code_.tokens.data(), bytes);
  *size = bytes;
  return S_OK;
}

// a game's shader is made from the program it is given, as D3D9 makes one: the bytecode is the shader's (a draw
// translates it), and a program of a stage or version this build does not read is refused
HRESULT
MTLD3D9Device::create_vertex_shader(const DWORD *byte_code, IDirect3DVertexShader9 **shader, bool counted_device) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  ShaderCode code;
  if (!read_shader_code(byte_code, code) || code.pixel) {
    ERR_ONCE("D3D9: a vertex shader whose program this build does not read was refused: it reads vs_1_1, vs_2_0 and "
             "vs_3_0 (the versions its caps name).");
    return D3DERR_INVALIDCALL;
  }
  ComPtr<MTLD3D9VertexShader> result;
  result.Attach(new MTLD3D9VertexShader(static_cast<IDirect3DDevice9 *>(this), std::move(code), counted_device));
  *shader = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateVertexShader(const DWORD *byte_code, IDirect3DVertexShader9 **shader) {
  return create_vertex_shader(byte_code, shader, true);
}

HRESULT
MTLD3D9Device::SetVertexShader(IDirect3DVertexShader9 *shader) {
  // Public refcount is unchanged (Wine device.c). Binding holds a private reference so a public Release that leaves
  // the shader set does not free it before the next draw (Wine visual.c alphatest_test).
  if (shader != vertex_shader_) {
    if (IMTLD3D9VertexShader *previous = lookup_vertex_shader(vertex_shader_))
      previous->release_private_ref();
    vertex_shader_ = shader;
    if (IMTLD3D9VertexShader *ours = lookup_vertex_shader(shader)) {
      ours->add_private_ref();
      // the def instructions a program carries set the constant registers when it is bound, which an application
      // can overwrite with a later SetVertexShaderConstantF (wined3d's shader_apply_def_constants). the translated
      // program reads a def'd constant directly as the immediate, but a relative address reads the constant buffer,
      // which only has the registers' contents (Wine visual.c test_mova's c[a0.x + 3] over the def'd c0 to c6)
      if (ours->apply_def_constants(vertex_constants_f_.data(), vertex_constants_i_.data(),
                                    vertex_constants_b_.data()))
        vs_f_dirty_ = vs_i_dirty_ = vs_b_dirty_ = true;
    }
  }
  if (recording_)
    record(StateKey{StateKey::VertexShader, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetVertexShader(IDirect3DVertexShader9 **shader) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = vertex_shader_;
  if (*shader)
    (*shader)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetVertexShaderConstantF(UINT start, const float *data, UINT count) {
  if (count > 256 || start > 256 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(&vertex_constants_f_[start * 4], data, (size_t)count * 4 * sizeof(float));
  if (count)
    vs_f_dirty_ = true;
  if (count && recording_)
    record(StateKey{StateKey::VertexConstantF, start, count});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetVertexShaderConstantF(UINT start, float *data, UINT count) {
  if (count > 256 || start > 256 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(data, &vertex_constants_f_[start * 4], (size_t)count * 4 * sizeof(float));
  return S_OK;
}

HRESULT
MTLD3D9Device::SetVertexShaderConstantI(UINT start, const int *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(&vertex_constants_i_[start * 4], data, (size_t)count * 4 * sizeof(int));
  if (count)
    vs_i_dirty_ = true;
  if (count && recording_)
    record(StateKey{StateKey::VertexConstantI, start, count});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetVertexShaderConstantI(UINT start, int *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(data, &vertex_constants_i_[start * 4], (size_t)count * 4 * sizeof(int));
  return S_OK;
}

HRESULT
MTLD3D9Device::SetVertexShaderConstantB(UINT start, const WINBOOL *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  for (UINT at = 0; at < count; at++)
    vertex_constants_b_[start + at] = data[at] ? TRUE : FALSE;
  if (count)
    vs_b_dirty_ = true;
  if (count && recording_)
    record(StateKey{StateKey::VertexConstantB, start, count});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetVertexShaderConstantB(UINT start, WINBOOL *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  for (UINT at = 0; at < count; at++)
    data[at] = vertex_constants_b_[start + at];
  return S_OK;
}

HRESULT
MTLD3D9Device::create_pixel_shader(const DWORD *byte_code, IDirect3DPixelShader9 **shader, bool counted_device) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  ShaderCode code;
  if (!read_shader_code(byte_code, code) || !code.pixel) {
    ERR_ONCE("D3D9: a pixel shader whose program this build does not read was refused: it reads ps_1_1 through ps_1_4, "
             "ps_2_0 and ps_3_0 (the versions its caps name).");
    return D3DERR_INVALIDCALL;
  }
  ComPtr<MTLD3D9PixelShader> result;
  result.Attach(new MTLD3D9PixelShader(static_cast<IDirect3DDevice9 *>(this), std::move(code), counted_device));
  *shader = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreatePixelShader(const DWORD *byte_code, IDirect3DPixelShader9 **shader) {
  return create_pixel_shader(byte_code, shader, true);
}

HRESULT
MTLD3D9Device::SetPixelShader(IDirect3DPixelShader9 *shader) {
  // Public refcount is unchanged (Wine device.c). Binding holds a private reference so a public Release that leaves
  // the shader set does not free it before the next draw (Wine visual.c alphatest_test).
  if (shader != pixel_shader_) {
    if (IMTLD3D9PixelShader *previous = lookup_pixel_shader(pixel_shader_))
      previous->release_private_ref();
    pixel_shader_ = shader;
    if (IMTLD3D9PixelShader *ours = lookup_pixel_shader(shader))
      ours->add_private_ref();
  }
  if (recording_)
    record(StateKey{StateKey::PixelShader, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetPixelShader(IDirect3DPixelShader9 **shader) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = pixel_shader_;
  if (*shader)
    (*shader)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetPixelShaderConstantF(UINT start, const float *data, UINT count) {
  if (count > 256 || start > 256 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(&pixel_constants_f_[start * 4], data, (size_t)count * 4 * sizeof(float));
  if (count)
    ps_f_dirty_ = true;
  if (count && recording_)
    record(StateKey{StateKey::PixelConstantF, start, count});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetPixelShaderConstantF(UINT start, float *data, UINT count) {
  if (count > 256 || start > 256 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(data, &pixel_constants_f_[start * 4], (size_t)count * 4 * sizeof(float));
  return S_OK;
}

HRESULT
MTLD3D9Device::SetPixelShaderConstantI(UINT start, const int *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(&pixel_constants_i_[start * 4], data, (size_t)count * 4 * sizeof(int));
  if (count)
    ps_i_dirty_ = true;
  if (count && recording_)
    record(StateKey{StateKey::PixelConstantI, start, count});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetPixelShaderConstantI(UINT start, int *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  if (count)
    memcpy(data, &pixel_constants_i_[start * 4], (size_t)count * 4 * sizeof(int));
  return S_OK;
}

HRESULT
MTLD3D9Device::SetPixelShaderConstantB(UINT start, const WINBOOL *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  for (UINT at = 0; at < count; at++)
    pixel_constants_b_[start + at] = data[at] ? TRUE : FALSE;
  if (count)
    ps_b_dirty_ = true;
  if (count && recording_)
    record(StateKey{StateKey::PixelConstantB, start, count});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetPixelShaderConstantB(UINT start, WINBOOL *data, UINT count) {
  if (count > 16 || start > 16 - count || (count && !data))
    return D3DERR_INVALIDCALL;
  for (UINT at = 0; at < count; at++)
    data[at] = pixel_constants_b_[start + at];
  return S_OK;
}

} // namespace mullion::d3d9
