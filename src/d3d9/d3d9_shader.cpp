// the d3dbc programs a game makes and the objects it holds them in: D3D9's shader binary is its own token stream
// (not DXBC), so this reads the version and the instruction stream here, and the draw that uses one translates it to
// the DXBC an airconv pipeline takes (d3d9_shader_translator.hpp).
#include "d3d9_shader.hpp"
#include "d3d9_device.hpp"
#include "d3d9_shader_translator.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9VertexShader = {0x2b6f1c94, 0x4a3d, 0x4f8e, {0x8c, 0x51, 0x9e, 0x62, 0x3a, 0x77, 0x0d, 0x41}};
const IID IID_IMTLD3D9PixelShader = {0x5d1b8e42, 0x7c96, 0x4a0f, {0x8b, 0x24, 0x6d, 0x51, 0x0c, 0x9f, 0x3e, 0x72}};

namespace {

// D3D9's program versions: the two high halves of the first token, and the majors and minors this build reads
constexpr DWORD kVertexVersion = 0xFFFE0000;
constexpr DWORD kPixelVersion = 0xFFFF0000;
constexpr DWORD kShaderVersionMask = 0xFFFF0000;

// the end of a program and a comment, from d3d9types.h (D3DSIO_END, D3DSIO_COMMENT)
constexpr DWORD kOpcodeEnd = 0xFFFF;
constexpr DWORD kOpcodeComment = 0xFFFE;

bool supported(bool pixel, UINT major, UINT minor) {
  if (major == 1)
    return pixel ? minor >= 1 && minor <= 4 : minor == 1;
  // 2.0 and 3.0 are the same tokens in both stages
  return (major == 2 || major == 3) && minor == 0;
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
  return true;
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
    ID3D11VertexShader **shader, ID3D11InputLayout **input_layout
) {
  if (!device || !shader || !input_layout)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  *input_layout = nullptr;
  auto same_slot = [](const PixelInputSlot &a, const PixelInputSlot &b) {
    return a.index == b.index && a.reg == b.reg && a.semantic == b.semantic;
  };
  for (auto &variant : variants_) {
    if (variant.declaration.size() == declaration.size() &&
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
  if (!translate_shader(code_, &inputs, &slots, program, reason)) {
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

  Variant variant;
  variant.declaration = declaration;
  variant.pixel_inputs = pixel_inputs;
  HRESULT hr = device->CreateVertexShader(
      program.dxbc.data(), program.dxbc.size() * sizeof(DWORD), nullptr, &variant.shader
  );
  if (SUCCEEDED(hr) && !layout.empty())
    hr = device->CreateInputLayout(
        layout.data(), (UINT)layout.size(), program.dxbc.data(), program.dxbc.size() * sizeof(DWORD), &variant.layout
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

// the D3D11 pixel shader this program draws through, translated the first time a draw uses it
HRESULT
MTLD3D9PixelShader::compiled(ID3D11Device *device, ID3D11PixelShader **shader) {
  if (!device || !shader)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  if (!compiled_) {
    TranslatedProgram program;
    std::string reason;
    if (!translate_shader(code_, nullptr, nullptr, program, reason)) {
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
    pixel_inputs_.clear();
    pixel_inputs_.reserve(program.inputs.size());
    for (const auto &element : program.inputs) {
      PixelInputSlot slot;
      slot.semantic = element.semantic;
      slot.index = element.index;
      slot.reg = element.reg;
      pixel_inputs_.push_back(std::move(slot));
    }
    HRESULT hr = device->CreatePixelShader(
        program.dxbc.data(), program.dxbc.size() * sizeof(DWORD), nullptr, &compiled_
    );
    if (FAILED(hr)) {
      ERR_ONCE("D3D9: the D3D11 device did not take the translated pixel shader: 0x", std::hex, hr);
      return D3DERR_INVALIDCALL;
    }
  }
  *shader = compiled_.Get();
  (*shader)->AddRef();
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
MTLD3D9Device::CreateVertexShader(const DWORD *byte_code, IDirect3DVertexShader9 **shader) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  ShaderCode code;
  if (!read_shader_code(byte_code, code) || code.pixel) {
    ERR_ONCE("D3D9: a vertex shader whose program this build does not read was refused.");
    return D3DERR_INVALIDCALL;
  }
  ComPtr<MTLD3D9VertexShader> result;
  result.Attach(new MTLD3D9VertexShader(static_cast<IDirect3DDevice9 *>(this), std::move(code)));
  *shader = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetVertexShader(IDirect3DVertexShader9 *shader) {
  vertex_shader_ = shader;
  if (recording_)
    record(StateKey{StateKey::VertexShader, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetVertexShader(IDirect3DVertexShader9 **shader) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = vertex_shader_.Get();
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
MTLD3D9Device::CreatePixelShader(const DWORD *byte_code, IDirect3DPixelShader9 **shader) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = nullptr;
  ShaderCode code;
  if (!read_shader_code(byte_code, code) || !code.pixel) {
    ERR_ONCE("D3D9: a pixel shader whose program this build does not read was refused.");
    return D3DERR_INVALIDCALL;
  }
  ComPtr<MTLD3D9PixelShader> result;
  result.Attach(new MTLD3D9PixelShader(static_cast<IDirect3DDevice9 *>(this), std::move(code)));
  *shader = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetPixelShader(IDirect3DPixelShader9 *shader) {
  pixel_shader_ = shader;
  if (recording_)
    record(StateKey{StateKey::PixelShader, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetPixelShader(IDirect3DPixelShader9 **shader) {
  if (!shader)
    return D3DERR_INVALIDCALL;
  *shader = pixel_shader_.Get();
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
