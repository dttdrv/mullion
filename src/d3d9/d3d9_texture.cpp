#include "d3d9_texture.hpp"
#include <algorithm>
#include <cfloat>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9Texture = {0x8b1d0f27, 0x6a4c, 0x4e35, {0x8f, 0x71, 0x2c, 0x44, 0x9a, 0x0e, 0x63, 0xb8}};

namespace {

// D3D9's three sampler filters as the one D3D11 filter that samples the way they say. D3D11 names one filter for all
// three picks (minification, magnification and mip level) and the two do not always meet: D3D11's anisotropic filter
// is both picks at once with a linearly filtered mip chain, and its point/linear matrix has no anisotropic entry. a
// combination D3D11 cannot name exactly samples through the filter that honours it as far as it can -- the anisotropic
// filter where both picks ask for it and the mip chain is linear or off, and otherwise the point/linear filter, whose
// answer to an anisotropic pick is the linear one that pick is a superset of.
bool
filter_of(
    D3DTEXTUREFILTERTYPE min_filter, D3DTEXTUREFILTERTYPE mag_filter, D3DTEXTUREFILTERTYPE mip_filter,
    D3D11_FILTER &filter, float &max_lod
) {
  // no mipmapping: level 0 is the only level sampled, whatever the mip pick says
  max_lod = mip_filter == D3DTEXF_NONE ? 0.0f : FLT_MAX;
  if (min_filter == D3DTEXF_ANISOTROPIC && mag_filter == D3DTEXF_ANISOTROPIC && max_lod != 0.0f &&
      mip_filter == D3DTEXF_LINEAR) {
    filter = D3D11_FILTER_ANISOTROPIC;
    return true;
  }

  const D3DTEXTUREFILTERTYPE min = min_filter == D3DTEXF_ANISOTROPIC ? D3DTEXF_LINEAR : min_filter;
  const D3DTEXTUREFILTERTYPE mag = mag_filter == D3DTEXF_ANISOTROPIC ? D3DTEXF_LINEAR : mag_filter;
  const D3DTEXTUREFILTERTYPE mip = max_lod == 0.0f ? D3DTEXF_POINT : mip_filter;
  const bool min_point = min == D3DTEXF_POINT, mag_point = mag == D3DTEXF_POINT;
  const bool mip_point = mip == D3DTEXF_POINT;
  if (min_point && mag_point && mip_point)
    filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  else if (min_point && mag_point)
    filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
  else if (min_point && mip_point)
    filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
  else if (min_point)
    filter = D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
  else if (mag_point && mip_point)
    filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
  else if (mag_point)
    filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
  else if (mip_point)
    filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  else
    filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  return true;
}

D3D11_TEXTURE_ADDRESS_MODE
address_of(D3DTEXTUREADDRESS address) {
  switch (address) {
  case D3DTADDRESS_MIRROR:
    return D3D11_TEXTURE_ADDRESS_MIRROR;
  case D3DTADDRESS_CLAMP:
    return D3D11_TEXTURE_ADDRESS_CLAMP;
  case D3DTADDRESS_MIRRORONCE:
    return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
  default:
    return D3D11_TEXTURE_ADDRESS_WRAP;
  }
}

// the operations the textured pixel shader carries (d3d9_fixed_function_shaders.hpp), and the arguments they take
UINT32
operation_of(DWORD op) {
  switch (op) {
  case D3DTOP_DISABLE:
    return 0;
  case D3DTOP_SELECTARG1:
    return 1;
  case D3DTOP_SELECTARG2:
    return 2;
  case D3DTOP_MODULATE:
    return 3;
  case D3DTOP_ADD:
    return 4;
  case D3DTOP_SUBTRACT:
    return 5;
  default:
    return 0;
  }
}

UINT32
argument_of(DWORD argument) {
  switch (argument & D3DTA_SELECTMASK) {
  case D3DTA_DIFFUSE:
  case D3DTA_CURRENT: // the vertex colour at stage 0, which is the only stage this build has
    return 1;
  case D3DTA_TFACTOR:
    return 2;
  default:
    return 0; // D3DTA_TEXTURE
  }
}

} // namespace

Texture9Storage::Texture9Storage(
    ComPtr<ID3D11Texture2D> texture, const ComPtr<ID3D11ShaderResourceView> &view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, bool lockable, bool generate_mips
)
    : texture_(std::move(texture)), view_(view), context_(context), pixel_bytes_(pixel_size(format)),
      block_bytes_(block_size(format)), generate_mips_(generate_mips) {
  D3D11_TEXTURE2D_DESC desc{};
  texture_->GetDesc(&desc);
  // a texture whose bytes are not the CPU's has none to keep: a lock of it is refused rather than answered with bytes
  // that would never reach the texture
  if (!lockable)
    return;
  levels_.resize(desc.MipLevels);
  for (UINT level = 0; level < desc.MipLevels; level++) {
    Level &one = levels_[level];
    one.width = std::max(1u, desc.Width >> level);
    one.height = std::max(1u, desc.Height >> level);
    if (block_bytes_) {
      // a compressed level is a grid of four-by-four blocks, as many as its size rounds up to, and its rows are the
      // blocks' rows: the pitch a D3D9 lock of a compressed level reports is the one it is stored with
      one.pitch = ((one.width + 3) / 4) * block_bytes_;
      one.bytes.resize((size_t)one.pitch * ((one.height + 3) / 4));
      continue;
    }
    // the pitch a D3D9 texture's rows have here: as many bytes as the level's width takes, the way D3D9's own
    // surfaces of a managed texture are laid out
    one.pitch = one.width * pixel_bytes_;
    one.bytes.resize((size_t)one.pitch * one.height);
  }
}

// a managed texture's lock, as D3D9's contract has it: one level at a time, a rect inside the level, and no discard or
// no-overwrite flag (those are a dynamic buffer's), READONLY an allowed one
HRESULT
Texture9Storage::lock(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  locked->Pitch = 0;
  locked->pBits = nullptr;
  if (level >= levels_.size() || locked_level_ != kNoLevel)
    return D3DERR_INVALIDCALL;
  if (flags & ~(DWORD)(D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK))
    return D3DERR_INVALIDCALL;

  Level &one = levels_[level];
  LONG left = 0, top = 0;
  if (rect) {
    if (rect->left < 0 || rect->top < 0 || rect->right > (LONG)one.width || rect->bottom > (LONG)one.height ||
        rect->right <= rect->left || rect->bottom <= rect->top)
      return D3DERR_INVALIDCALL;
    // a compressed level is locked by whole blocks: D3D9 refuses a rectangle that starts inside one
    if (block_bytes_ && ((rect->left & 3) || (rect->top & 3)))
      return D3DERR_INVALIDCALL;
    left = rect->left;
    top = rect->top;
  }

  locked->Pitch = (INT)one.pitch;
  locked->pBits = one.bytes.data() + (size_t)(block_bytes_ ? top / 4 : top) * one.pitch +
                  (size_t)(block_bytes_ ? left / 4 : left) * (block_bytes_ ? block_bytes_ : pixel_bytes_);
  locked_level_ = level;
  read_only_ = (flags & D3DLOCK_READONLY) != 0;
  return S_OK;
}

// what a lock's bytes become: the level's contents, uploaded into the texture, and the levels below the top one made
// out of it when it was the top one that was written (D3D9's managed textures mipmap themselves)
HRESULT
Texture9Storage::unlock() {
  if (locked_level_ == kNoLevel)
    return D3DERR_INVALIDCALL;
  const UINT level = locked_level_;
  const bool read_only = read_only_;
  locked_level_ = kNoLevel;
  read_only_ = false;

  const Level &one = levels_[level];
  if (!read_only)
    context_->UpdateSubresource(texture_.Get(), level, nullptr, one.bytes.data(), one.pitch, 0);
  if (level == 0 && levels_.size() > 1 && generate_mips_)
    return generate_mips();
  return S_OK;
}

HRESULT
Texture9Storage::generate_mips() {
  if (!view_ || levels_.size() < 2)
    return S_OK;
  context_->GenerateMips(view_.Get());
  return S_OK;
}

MTLD3D9Texture::MTLD3D9Texture(
    IDirect3DDevice9 *device, ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, D3DPOOL pool, UINT levels, bool generate_mips
)
    : device_(device), texture_(std::move(texture)), view_(std::move(view)),
      storage_(texture_, view_, context, format, pool == D3DPOOL_MANAGED, generate_mips), format_(format),
      pool_(pool), levels_count_(levels) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9) ||
      riid == __uuidof(IDirect3DTexture9)) {
    *object = static_cast<IDirect3DTexture9 *>(this);
  } else if (riid == IID_IMTLD3D9Texture) {
    *object = static_cast<IMTLD3D9Texture *>(this);
  } else if (riid == IID_IMTLD3D9LockTarget) {
    *object = static_cast<IMTLD3D9LockTarget *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (device_)
    device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetPrivateData(REFGUID, const void *, DWORD, DWORD) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetPrivateData(REFGUID, void *, DWORD *) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::FreePrivateData(REFGUID) {
  return D3DERR_INVALIDCALL;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::SetPriority(DWORD) {
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetPriority() {
  return 0;
}

void STDMETHODCALLTYPE
MTLD3D9Texture::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetType() {
  return D3DRTYPE_TEXTURE;
}

// the most detailed level is the top one: the levels below it are made out of it by the device, so a texture that asks
// for another most-detailed level keeps this one and the call answers with the level the texture is on
DWORD STDMETHODCALLTYPE
MTLD3D9Texture::SetLOD(DWORD lod) {
  if (lod != 0) {
    ERR_ONCE("D3D9: only the top level is a texture's most detailed one yet.");
  }
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetLOD() {
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelCount() {
  return levels_count_;
}

// the automatic mip filter is a texture's own (D3DUSAGE_AUTOGENMIPMAP, a default-pool usage this build has none of):
// a managed texture's levels are the device's
HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) {
  ERR_ONCE("D3D9: the automatic mip filter is a D3DUSAGE_AUTOGENMIPMAP texture's, which this build has none of.");
  return D3DERR_INVALIDCALL;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetAutoGenFilterType() {
  return D3DTEXF_LINEAR;
}

void STDMETHODCALLTYPE
MTLD3D9Texture::GenerateMipSubLevels() {
  if (!managed()) {
    ERR_ONCE("D3D9: the mip levels of a texture are its device's, which this build makes for a managed texture.");
    return;
  }
  storage_.generate_mips();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) {
  if (level >= levels_.size() || !desc)
    return D3DERR_INVALIDCALL;
  return levels_[level]->GetDesc(desc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface_level) {
  if (level >= levels_.size() || !surface_level)
    return D3DERR_INVALIDCALL;
  *surface_level = levels_[level].Get();
  (*surface_level)->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!managed())
    return D3DERR_INVALIDCALL;
  return storage_.lock(level, rect, locked, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::UnlockRect(UINT level) {
  if (!managed())
    return D3DERR_INVALIDCALL;
  if (!storage_.locked() || level != storage_.locked_level())
    return D3DERR_INVALIDCALL;
  return storage_.unlock();
}

HRESULT
MTLD3D9Texture::lock_level(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) {
  return LockRect(level, locked, rect, flags);
}

HRESULT
MTLD3D9Texture::unlock_level(UINT level) {
  return UnlockRect(level);
}

// a managed texture's bytes are the same bytes wherever they are written, and a lock of one uploads the level it
// wrote, so the region it names is covered already
HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::AddDirtyRect(const RECT *) {
  return S_OK;
}

bool
SamplerState9::supported(D3DSAMPLERSTATETYPE type) {
  switch (type) {
  case D3DSAMP_MINFILTER:
  case D3DSAMP_MAGFILTER:
  case D3DSAMP_MIPFILTER:
  case D3DSAMP_ADDRESSU:
  case D3DSAMP_ADDRESSV:
  case D3DSAMP_ADDRESSW:
  case D3DSAMP_MAXANISOTROPY:
  case D3DSAMP_MIPMAPLODBIAS:
  case D3DSAMP_MAXMIPLEVEL:
  case D3DSAMP_BORDERCOLOR:
  case D3DSAMP_SRGBTEXTURE:
  case D3DSAMP_ELEMENTINDEX:
  case D3DSAMP_DMAPOFFSET:
    return true;
  default:
    return false;
  }
}

bool
SamplerState9::usable_value(D3DSAMPLERSTATETYPE type, DWORD value) {
  switch (type) {
  case D3DSAMP_MINFILTER:
  case D3DSAMP_MAGFILTER:
    return value == D3DTEXF_POINT || value == D3DTEXF_LINEAR || value == D3DTEXF_ANISOTROPIC;
  case D3DSAMP_MIPFILTER:
    return value == D3DTEXF_NONE || value == D3DTEXF_POINT || value == D3DTEXF_LINEAR;
  case D3DSAMP_ADDRESSU:
  case D3DSAMP_ADDRESSV:
  case D3DSAMP_ADDRESSW:
    // a border needs a colour the sampler is given and a mode this build has none of
    return value == D3DTADDRESS_WRAP || value == D3DTADDRESS_MIRROR || value == D3DTADDRESS_CLAMP ||
           value == D3DTADDRESS_MIRRORONCE;
  case D3DSAMP_MAXANISOTROPY:
    return value >= 1 && value <= 16;
  case D3DSAMP_MIPMAPLODBIAS:
  case D3DSAMP_MAXMIPLEVEL:
  case D3DSAMP_BORDERCOLOR:
  case D3DSAMP_SRGBTEXTURE:
  case D3DSAMP_ELEMENTINDEX:
  case D3DSAMP_DMAPOFFSET:
    // a value this build's sampler already has the meaning of: a bias of none, the top level, a border nobody reads,
    // textures sampled as they are stored, the first element, no displacement map
    return value == 0;
  default:
    return false;
  }
}

HRESULT
SamplerState9::set(D3DSAMPLERSTATETYPE type, DWORD value) {
  if (!supported(type) || !usable_value(type, value)) {
    ERR_ONCE("D3D9: this sampler state or value is not honoured yet; the sampler keeps the value it had.");
    return D3DERR_INVALIDCALL;
  }

  switch (type) {
  case D3DSAMP_MINFILTER:
    min_filter_ = (D3DTEXTUREFILTERTYPE)value;
    break;
  case D3DSAMP_MAGFILTER:
    mag_filter_ = (D3DTEXTUREFILTERTYPE)value;
    break;
  case D3DSAMP_MIPFILTER:
    mip_filter_ = (D3DTEXTUREFILTERTYPE)value;
    break;
  case D3DSAMP_ADDRESSU:
    address_u_ = (D3DTEXTUREADDRESS)value;
    break;
  case D3DSAMP_ADDRESSV:
    address_v_ = (D3DTEXTUREADDRESS)value;
    break;
  case D3DSAMP_ADDRESSW:
    address_w_ = (D3DTEXTUREADDRESS)value;
    break;
  case D3DSAMP_MAXANISOTROPY:
    max_anisotropy_ = value;
    break;
  default:
    return S_OK; // a state whose only usable value is the one this sampler has
  }
  current_ = false;
  return S_OK;
}

HRESULT
SamplerState9::get(D3DSAMPLERSTATETYPE type, DWORD *value) const {
  if (!value || !supported(type))
    return D3DERR_INVALIDCALL;
  switch (type) {
  case D3DSAMP_MINFILTER:
    *value = min_filter_;
    break;
  case D3DSAMP_MAGFILTER:
    *value = mag_filter_;
    break;
  case D3DSAMP_MIPFILTER:
    *value = mip_filter_;
    break;
  case D3DSAMP_ADDRESSU:
    *value = address_u_;
    break;
  case D3DSAMP_ADDRESSV:
    *value = address_v_;
    break;
  case D3DSAMP_ADDRESSW:
    *value = address_w_;
    break;
  case D3DSAMP_MAXANISOTROPY:
    *value = max_anisotropy_;
    break;
  default:
    *value = 0;
    break;
  }
  return S_OK;
}

ID3D11SamplerState *
SamplerState9::state(ID3D11Device *device) {
  if (current_)
    return sampler_.Get();

  D3D11_FILTER filter;
  float max_lod;
  if (!filter_of(min_filter_, mag_filter_, mip_filter_, filter, max_lod))
    return nullptr;

  D3D11_SAMPLER_DESC desc{};
  desc.Filter = filter;
  desc.AddressU = address_of(address_u_);
  desc.AddressV = address_of(address_v_);
  desc.AddressW = address_of(address_w_);
  desc.MipLODBias = 0.0f;
  // the anisotropy a point or linear filter reads is not its own, and D3D11 refuses a value above 1 for those
  desc.MaxAnisotropy = filter == D3D11_FILTER_ANISOTROPIC ? max_anisotropy_ : 1;
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  desc.MinLOD = 0.0f;
  desc.MaxLOD = max_lod;
  sampler_.Reset();
  if (FAILED(device->CreateSamplerState(&desc, &sampler_)))
    return nullptr;
  current_ = true;
  return sampler_.Get();
}

bool
TextureStageState9::supported(D3DTEXTURESTAGESTATETYPE type) {
  switch (type) {
  case D3DTSS_COLOROP:
  case D3DTSS_COLORARG1:
  case D3DTSS_COLORARG2:
  case D3DTSS_ALPHAOP:
  case D3DTSS_ALPHAARG1:
  case D3DTSS_ALPHAARG2:
  case D3DTSS_TEXCOORDINDEX:
  case D3DTSS_TEXTURETRANSFORMFLAGS:
  case D3DTSS_RESULTARG:
    return true;
  default:
    return false;
  }
}

bool
TextureStageState9::usable_value(D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  switch (type) {
  case D3DTSS_COLOROP:
  case D3DTSS_ALPHAOP:
    switch (value) {
    case D3DTOP_DISABLE:
    case D3DTOP_SELECTARG1:
    case D3DTOP_SELECTARG2:
    case D3DTOP_MODULATE:
    case D3DTOP_ADD:
    case D3DTOP_SUBTRACT:
      return true;
    default:
      return false;
    }
  case D3DTSS_COLORARG1:
  case D3DTSS_COLORARG2:
  case D3DTSS_ALPHAARG1:
  case D3DTSS_ALPHAARG2:
    // an argument with a modifier (a complement or an alpha replication) is another argument than the one it names
    if (value & (D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE))
      return false;
    switch (value & D3DTA_SELECTMASK) {
    case D3DTA_TEXTURE:
    case D3DTA_DIFFUSE:
    case D3DTA_CURRENT:
    case D3DTA_TFACTOR:
      return true;
    default:
      return false;
    }
  case D3DTSS_TEXCOORDINDEX:
    // the first set of texture coordinates, and no generated one: a texture transform or a camera-space map is another
    // stage's texture coordinates
    return value == 0;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    return value == D3DTTFF_DISABLE;
  case D3DTSS_RESULTARG:
    return value == D3DTA_CURRENT;
  default:
    return false;
  }
}

HRESULT
TextureStageState9::set(D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (!supported(type) || !usable_value(type, value)) {
    ERR_ONCE("D3D9: this texture stage state or value is not honoured yet; the stage keeps the value it had.");
    return D3DERR_INVALIDCALL;
  }
  switch (type) {
  case D3DTSS_COLOROP:
    colour_op_ = value;
    break;
  case D3DTSS_COLORARG1:
    colour_arg1_ = value;
    break;
  case D3DTSS_COLORARG2:
    colour_arg2_ = value;
    break;
  case D3DTSS_ALPHAOP:
    alpha_op_ = value;
    break;
  case D3DTSS_ALPHAARG1:
    alpha_arg1_ = value;
    break;
  case D3DTSS_ALPHAARG2:
    alpha_arg2_ = value;
    break;
  default:
    return S_OK; // a state whose only usable value is the one this stage has (the coordinate set, the transform, the
                 // result register)
  }
  return S_OK;
}

HRESULT
TextureStageState9::get(D3DTEXTURESTAGESTATETYPE type, DWORD *value) const {
  if (!value || !supported(type))
    return D3DERR_INVALIDCALL;
  switch (type) {
  case D3DTSS_COLOROP:
    *value = colour_op_;
    break;
  case D3DTSS_COLORARG1:
    *value = colour_arg1_;
    break;
  case D3DTSS_COLORARG2:
    *value = colour_arg2_;
    break;
  case D3DTSS_ALPHAOP:
    *value = alpha_op_;
    break;
  case D3DTSS_ALPHAARG1:
    *value = alpha_arg1_;
    break;
  case D3DTSS_ALPHAARG2:
    *value = alpha_arg2_;
    break;
  case D3DTSS_TEXCOORDINDEX:
    *value = 0;
    break;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    *value = D3DTTFF_DISABLE;
    break;
  default:
    *value = D3DTA_CURRENT;
    break;
  }
  return S_OK;
}

void
TextureStageState9::constants(UINT32 ops[4], UINT32 args[4]) const {
  ops[0] = operation_of(colour_op_);
  ops[1] = argument_of(colour_arg1_);
  ops[2] = argument_of(colour_arg2_);
  ops[3] = operation_of(alpha_op_);
  args[0] = argument_of(alpha_arg1_);
  args[1] = argument_of(alpha_arg2_);
  args[2] = 0;
  args[3] = 0;
}

} // namespace mullion::d3d9
