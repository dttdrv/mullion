// a game's vertex or pixel shader: the d3dbc program it was made from and the version its first token names. the
// D3D11 shader it draws through is made at the draw, not here, because its input signature is the vertex format the
// draw reads (D3D9 gives a shader its inputs by register; which semantics those registers carry is the FVF's or the
// declaration's, which the device does not know when the shader is made).
#pragma once
#include "d3d9_common.hpp"
#include <vector>

namespace mullion::d3d9 {

// the parameter count a shader model 1 instruction's opcode carries (d3d9_shader_translator.cpp's own table)
UINT sm1_instruction_tokens(UINT opcode, UINT minor);

// a d3dbc program of D3D9's: its tokens, whether it is a pixel shader's, and the version that names it
struct ShaderCode {
  std::vector<DWORD> tokens;
  bool pixel = false;
  UINT major = 0;
  UINT minor = 0;

  bool operator==(const ShaderCode &other) const {
    return pixel == other.pixel && major == other.major && minor == other.minor && tokens == other.tokens;
  }
};

// reads the version a d3dbc program starts with and checks that the program is one this build reads: a version token
// of a vertex or pixel shader, and a stream whose instructions end at D3DSIO_END. false for a program that is not,
// which CreateVertexShader and CreatePixelShader refuse as D3D9 does
bool read_shader_code(const DWORD *code, ShaderCode &out);

// D3D9's alpha test as a pixel shader carries it: whether D3DRS_ALPHATESTENABLE is on, and the comparison
// D3DRS_ALPHAFUNC names. Direct3D 11 has no alpha test of its own, so a program translated with one enabled ends every
// pixel with the comparison and a discard; the reference (D3DRS_ALPHAREF) is a per-draw value, so the program reads it
// from a constant buffer of its own (register b3) rather than carrying it
struct AlphaTest {
  bool enabled = false;
  DWORD func = D3DCMP_ALWAYS;

  bool operator==(const AlphaTest &other) const {
    return enabled == other.enabled && (!enabled || func == other.func);
  }
};

// D3D9 fog applied after a pixel program (Microsoft Learn, D3DRS_FOGENABLE / FOGTABLEMODE / FOGVERTEXMODE). Metal and
// D3D11 have no fixed fog, so a translated pixel program with fog ends by blending the fog colour. Table fog and
// vertex-Z fog read SV_Position.z; a foggy vertex shader's oFog is the blend factor itself; FOGENABLE with no mode and
// no fog output fully fogs the pixel (Wine visual.c fog_with_shader_test)
enum class FogSource : UINT { None = 0, Table = 1, VertexZ = 2, FogCoord = 3, Full = 4 };

struct FogState {
  FogSource source = FogSource::None;
  DWORD mode = D3DFOG_NONE; // D3DFOG_EXP / EXP2 / LINEAR for Table and VertexZ

  bool operator==(const FogState &other) const {
    return source == other.source && (source == FogSource::None || source == FogSource::Full ||
                                      source == FogSource::FogCoord || mode == other.mode);
  }
  bool enabled() const { return source != FogSource::None; }
};

// D3DTSS_TEXTURETRANSFORMFLAGS PROJECTED: ps_1.x `tex` divides by the Nth texcoord component before sampling
// (COUNT2→.y, COUNT3→.z, COUNT4 / PROJECTED-alone / 0xffffffff→.w). 0 means no divide.
// force_alpha_one: Metal/D3D11 two-channel SNORM views return B=A=0; D3D9 V8U8/V16U16 sample with B=A=1.
struct TexProjectState {
  UINT divide_component = 0; // 0 = off, else 1..3 selecting y/z/w
  bool force_alpha_one = false;

  bool operator==(const TexProjectState &other) const {
    return divide_component == other.divide_component && force_alpha_one == other.force_alpha_one;
  }
};

// the bump-environment matrix of each texture stage (D3DTSS_BUMPENVMAT00/01/10/11) and the luminance scale and offset
// D3DTSS_BUMPENVLSCALE / LOFFSET carry. ps_1.x `bem` and `texbem` offset a coordinate by the matrix times a source's
// red and green, so a stage's values are part of what the translated program means and of its variant
struct BumpEnvState {
  static constexpr UINT kStages = 8;
  float matrix[kStages][4] = {}; // m00, m01, m10, m11
  float lscale[kStages] = {};
  float loffset[kStages] = {};

  bool operator==(const BumpEnvState &other) const {
    for (UINT stage = 0; stage < kStages; stage++) {
      for (UINT at = 0; at < 4; at++)
        if (matrix[stage][at] != other.matrix[stage][at])
          return false;
      if (lscale[stage] != other.lscale[stage] || loffset[stage] != other.loffset[stage])
        return false;
    }
    return true;
  }
};

// the pixel-shader stages whose bound texture is a depth-stencil one (D3DFMT_D16, D24S8, D32, ...): Direct3D 9
// comparison-samples a depth texture in a pixel shader of model 2.0 and above, and its texld/texldp answer the
// comparison, not the texel (Microsoft Learn, "Shadow maps and depth buffers"; Wine wined3d ps_compile_args sets the
// shadow bit for every sampler a depth format is bound to, and its GLSL backend samples it through sampler2DShadow,
// whose LEQUAL test answers 1.0 when the reference is at most the texel's depth). the reference is the coordinate's
// third component, divided by the fourth for a projected sample. ps_1.x's two-component `tex` carries no reference,
// so it keeps the plain sample
struct ShadowState {
  UINT mask = 0; // one bit per pixel stage

  bool operator==(const ShadowState &other) const {
    return mask == other.mask;
  }
};

// the register a pixel shader reads one of its inputs at: the compiled programs link the two stages by register (a
// varying is named after its register and component), so a vertex shader has to write each output where the pixel
// shader it draws with reads it
struct PixelInputSlot {
  std::string semantic;
  UINT index = 0;
  UINT reg = 0;
};

// one vertex declaration element, as the vertex shader sees it: the input register it feeds and what it means
struct VertexInput {
  UINT reg = 0;
  UINT usage = 0; // D3DDECLUSAGE_*
  UINT usage_index = 0;
  UINT mask = 0xF; // the components the element carries
};

// what the device asks a vertex shader of this build for: the D3D11 pipeline it draws with through a declaration, and
// with or without the user clip planes' distances (a draw under D3DRS_CLIPPLANEENABLE binds the planes at b3)
struct IMTLD3D9VertexShader {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual HRESULT compiled(
      ID3D11Device *device, const std::vector<D3DVERTEXELEMENT9> &declaration, const std::vector<VertexInput> &inputs,
      const std::vector<PixelInputSlot> &pixel_inputs, const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout,
      DWORD instanced_streams, bool clip_planes, ID3D11VertexShader **shader, ID3D11InputLayout **input_layout
  ) = 0;
  virtual bool samples() = 0;
  // whether the program writes oFog, which selects FogSource::FogCoord when FOGTABLEMODE is NONE
  virtual bool writes_fog() = 0;
  // the program's shader model: the vs_1_1 no-oFog reference quirk differs from vs_2_0 and above
  virtual UINT major() = 0;
  // SetVertexShader holds a private reference so the public count is unchanged (Wine device.c) while the object
  // survives a public Release that leaves it bound (Wine visual.c alphatest_test Releases then draws)
  virtual void add_private_ref() = 0;
  virtual void release_private_ref() = 0;
  // the def instructions the program carries, in the constant register file binding it sets (wined3d applies a
  // shader's def constants when it is bound, so a relative address reads them from the constant buffer it reads)
  virtual bool apply_def_constants(float *floats, int *ints, BOOL *bools) = 0;
};

// and what it asks a pixel shader for: the D3D11 pixel shader its program is translated to, for the alpha test and fog
// the draw is made under (each combination is a different program, and all are kept)
struct IMTLD3D9PixelShader {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual HRESULT compiled(
      ID3D11Device *device, const AlphaTest &alpha, const FogState &fog, const TexProjectState &project,
      const BumpEnvState &bump, const ShadowState &shadow, UINT texcoord_slot, bool flat,
      ID3D11PixelShader **shader
  ) = 0;
  // the registers this shader reads its inputs at, which the vertex shader it draws with writes to
  virtual const std::vector<PixelInputSlot> &pixel_inputs() = 0;
  // whether the program samples a texture: the draw binds the texture stage 0 holds (and its sampler) only when it
  // does, so a program that never samples never makes a render target it draws into a shader input too
  virtual bool samples() = 0;
  virtual void add_private_ref() = 0;
  virtual void release_private_ref() = 0;
};

// Set*Shader must recognise a Mullion shader without calling through its public vtbl (Wine device.c replaces lpVtbl
// with 0xdeadbeef and still expects SetPixelShader to succeed)
void register_vertex_shader(IDirect3DVertexShader9 *iface, IMTLD3D9VertexShader *impl);
void unregister_vertex_shader(IDirect3DVertexShader9 *iface);
IMTLD3D9VertexShader *lookup_vertex_shader(IDirect3DVertexShader9 *iface);
void register_pixel_shader(IDirect3DPixelShader9 *iface, IMTLD3D9PixelShader *impl);
void unregister_pixel_shader(IDirect3DPixelShader9 *iface);
IMTLD3D9PixelShader *lookup_pixel_shader(IDirect3DPixelShader9 *iface);

extern const IID IID_IMTLD3D9VertexShader;
extern const IID IID_IMTLD3D9PixelShader;

class MTLD3D9VertexShader final : private Object9, public IDirect3DVertexShader9, public IMTLD3D9VertexShader {
public:
  MTLD3D9VertexShader(IDirect3DDevice9 *device, ShaderCode code, bool counted_device = true);
  ~MTLD3D9VertexShader() override {
    unregister_vertex_shader(static_cast<IDirect3DVertexShader9 *>(this));
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  // Wine d3d9 shader: public 0→1 takes the device reference again; 1→0 gives it back while the object may still be
  // bound (wined3d / our private Set*Shader hold)
  ULONG STDMETHODCALLTYPE AddRef() override {
    const ULONG previous = public_references();
    const ULONG now = Object9::AddRef();
    if (previous == 0 && now == 1)
      device_ref_.attach(device_, counted_device_);
    return now;
  }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }
  void on_last_reference() override { device_ref_.detach(); }
  void add_private_ref() override { Object9::AddRefPrivate(); }
  void release_private_ref() override { Object9::ReleasePrivate(); }

  // IDirect3DVertexShader9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

  // IMTLD3D9VertexShader: the D3D11 vertex shader and input layout this program draws with through `declaration`,
  // translated the first time a draw uses the pair and kept for every draw after it
  HRESULT compiled(
      ID3D11Device *device, const std::vector<D3DVERTEXELEMENT9> &declaration, const std::vector<VertexInput> &inputs,
      const std::vector<PixelInputSlot> &pixel_inputs, const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout,
      DWORD instanced_streams, bool clip_planes, ID3D11VertexShader **shader, ID3D11InputLayout **input_layout
  ) override;
  bool samples() override {
    return samples_;
  }
  bool writes_fog() override {
    return writes_fog_;
  }
  UINT major() override {
    return code_.major;
  }
  // the def instructions the program carries, in the constant register file binding it sets (wined3d applies a
  // shader's def constants when it is bound, so a relative address reads them from the constant buffer it reads).
  // false when the program carries none, so the caller skips the dirty marking
  bool apply_def_constants(float *floats, int *ints, BOOL *bools) override {
    // D3DSIO_DEF, D3DSIO_DEFI, D3DSIO_DEFB, D3DSIO_END, D3DSIO_COMMENT (d3d9types.h)
    constexpr UINT kDef = 81, kDefi = 48, kDefb = 47, kEnd = 0xFFFF, kComment = 0xFFFE;
    bool any = false;
    const DWORD *tokens = code_.tokens.data();
    const UINT count = (UINT)code_.tokens.size();
    for (UINT at = 1; at < count;) {
      const DWORD token = tokens[at];
      const UINT opcode = token & 0xFFFF;
      if (opcode == kEnd)
        break;
      if (opcode == kComment) {
        at += 1 + ((token >> 16) & 0x7FFF);
        continue;
      }
      // shader model 1 counts no length: the opcode's own count is what its instruction carries
      const UINT length = (token >> 24) & 0xF ? (token >> 24) & 0xF : sm1_instruction_tokens(opcode, code_.minor);
      if (at + 1 + length > count)
        break;
      if (length >= 5) {
        const UINT reg = tokens[at + 1] & D3DSP_REGNUM_MASK;
        if (opcode == kDef) { // c# with the four floats the program carries
          memcpy(&floats[reg * 4], &tokens[at + 2], 16);
          any = true;
        } else if (opcode == kDefi && ints) {
          memcpy(&ints[reg * 4], &tokens[at + 2], 16);
          any = true;
        } else if (opcode == kDefb && bools) {
          bools[reg] = tokens[at + 2] ? TRUE : FALSE;
          any = true;
        }
      }
      at += 1 + (opcode == kComment ? 0 : length);
    }
    return any;
  }

private:
  // the device that made the shader is held through a private reference (DeviceChildRef), so a shader kept after the
  // application released its device still has one to answer GetDevice with
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ShaderCode code_;
  // false for the device's own passthrough shader: the device holds that object, so it must not count the device
  bool counted_device_ = true;

  // one D3D11 pipeline per vertex declaration a draw has used the shader with, clipped or not
  struct Variant {
    std::vector<D3DVERTEXELEMENT9> declaration;
    std::vector<PixelInputSlot> pixel_inputs;
    DWORD instanced_streams = 0;
    bool clip_planes = false;
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11InputLayout> layout;
  };
  std::vector<Variant> variants_;
  bool samples_ = false;
  bool writes_fog_ = false;
};

class MTLD3D9PixelShader final : private Object9, public IDirect3DPixelShader9, public IMTLD3D9PixelShader {
public:
  MTLD3D9PixelShader(IDirect3DDevice9 *device, ShaderCode code, bool counted_device = true)
      : device_(device), device_ref_(device, counted_device),
        code_(std::move(code)), counted_device_(counted_device) {
    register_pixel_shader(static_cast<IDirect3DPixelShader9 *>(this), this);
  }
  ~MTLD3D9PixelShader() override {
    unregister_pixel_shader(static_cast<IDirect3DPixelShader9 *>(this));
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override {
    const ULONG previous = public_references();
    const ULONG now = Object9::AddRef();
    if (previous == 0 && now == 1)
      device_ref_.attach(device_, counted_device_);
    return now;
  }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }
  void on_last_reference() override { device_ref_.detach(); }
  void add_private_ref() override { Object9::AddRefPrivate(); }
  void release_private_ref() override { Object9::ReleasePrivate(); }

  // IDirect3DPixelShader9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

  // IMTLD3D9PixelShader: the D3D11 pixel shader this program draws through, translated the first time a draw uses it
  // with that alpha test and fog source, and kept for every draw after it
  HRESULT compiled(
      ID3D11Device *device, const AlphaTest &alpha, const FogState &fog, const TexProjectState &project,
      const BumpEnvState &bump, const ShadowState &shadow, UINT texcoord_slot, bool flat,
      ID3D11PixelShader **shader
  ) override;
  const std::vector<PixelInputSlot> &pixel_inputs() override;
  bool samples() override {
    return samples_;
  }
  // whether a translation of this program carries the alpha test, which is what a draw binds the reference for
  bool alpha_tested() const {
    return alpha_tested_;
  }
  bool fog_tested() const {
    return fog_tested_;
  }

private:
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ShaderCode code_;
  // false for the device's own passthrough shader: the device holds that object, so it must not count the device
  bool counted_device_ = true;
  // one translated program per alpha test and fog source a draw has used it with
  struct Variant {
    AlphaTest alpha;
    FogState fog;
    TexProjectState project;
    BumpEnvState bump;
    ShadowState shadow;
    UINT texcoord_slot = 3;
    bool flat = false;
    std::vector<PixelInputSlot> inputs;
    ComPtr<ID3D11PixelShader> shader;
  };
  std::vector<Variant> variants_;
  std::vector<PixelInputSlot> pixel_inputs_;
  bool samples_ = false;
  bool alpha_tested_ = false;
  bool fog_tested_ = false;
};

} // namespace mullion::d3d9
