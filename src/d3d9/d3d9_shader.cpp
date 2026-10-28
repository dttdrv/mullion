// the d3dbc programs a game makes and the objects it holds them in: D3D9's shader binary is its own token stream
// (not DXBC), so this reads the version and the instruction stream here, and the draw that uses one translates it to
// the DXBC an airconv pipeline takes (d3d9_shader_translator.hpp).
#include "d3d9_shader.hpp"
#include "d3d9_device.hpp"

namespace mullion::d3d9 {

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
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
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
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
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
