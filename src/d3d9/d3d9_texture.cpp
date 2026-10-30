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
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, bool lockable, bool can_generate_mips, bool dynamic
)
    : texture_(std::move(texture)), view_(view), context_(context), lockable_(lockable), dynamic_(dynamic),
      pixel_bytes_(pixel_size(format)), block_bytes_(block_size(format)), can_generate_mips_(can_generate_mips) {
  texture_->GetDesc(&desc_);
  const D3D11_TEXTURE2D_DESC &desc = desc_;
  // a cube (or 2D array) locks each face's mip as its own subresource: D3D11CalcSubresource(mip, face, MipLevels)
  level_count_ = desc.MipLevels * desc.ArraySize;
  // A texture whose bytes are not the CPU's has none to keep: a lock of it is refused rather than answered with bytes
  // that would never reach the texture. A dynamic texture's levels are the CPU's too: D3D11 maps a texture with
  // WRITE_DISCARD only, so the level is authored here and uploaded, which is what its NOOVERWRITE lock needs to keep
  // the pixels outside the rectangle it writes.
  if (!lockable)
    return;
  levels_.resize(level_count_);
  for (UINT sub = 0; sub < level_count_; sub++) {
    const UINT mip = desc.MipLevels ? sub % desc.MipLevels : 0;
    Level &one = levels_[sub];
    one.width = std::max(1u, desc.Width >> mip);
    one.height = std::max(1u, desc.Height >> mip);
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

// A texture's lock, as D3D9's contract has it: one level at a time and a rect inside the level. A managed texture's
// bytes are the CPU's copy, and READONLY is allowed on them. A dynamic texture's level is the application's to write:
// DISCARD hands back the level as scratch, NOOVERWRITE promises not to write pixels the GPU may still read (so the
// level's other bytes are kept), and READONLY is refused because the bytes written are the texture's (Microsoft Learn,
// D3D9 lock flags).
HRESULT
Texture9Storage::lock(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  locked->Pitch = 0;
  locked->pBits = nullptr;
  if (level >= level_count_ || locked_level_ != kNoLevel)
    return D3DERR_INVALIDCALL;
  const DWORD dynamic_allowed = D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK;
  const DWORD allowed = dynamic_ ? dynamic_allowed : (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
  if (flags & ~allowed)
    return D3DERR_INVALIDCALL;
  if (dynamic_ && (flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE))
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

// What a lock's bytes become: the level's contents, uploaded into the texture. A managed texture's lower levels are the
// ones the game authored, not a chain made again from the top level: D3DPOOL_MANAGED is not D3DUSAGE_AUTOGENMIPMAP, and
// Wine's d3d9 uploads a managed texture's lock without generating mips (Microsoft Learn, D3DPOOL; Wine dlls/d3d9).
HRESULT
Texture9Storage::unlock() {
  if (locked_level_ == kNoLevel)
    return D3DERR_INVALIDCALL;
  const UINT level = locked_level_;
  const bool read_only = read_only_;
  locked_level_ = kNoLevel;
  read_only_ = false;

  if (!read_only) {
    const Level &one = levels_[level];
    context_->UpdateSubresource(texture_.Get(), level, nullptr, one.bytes.data(), one.pitch, 0);
  }
  return S_OK;
}

// The texture again, on the device a Reset made: the same description, the same levels, and the CPU's copy of each of
// them uploaded, so a managed texture samples the pixels the game authored before the device was lost. A level that was
// locked when the device failed is uploaded as the bytes the lock holds.
HRESULT
Texture9Storage::reset_device(ID3D11Device *device, const ComPtr<ID3D11DeviceContext> &context) {
  if (!device)
    return D3DERR_INVALIDCALL;
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = device->CreateTexture2D(&desc_, nullptr, &texture);
  if (FAILED(hr)) {
    ERR("D3D9: a texture of the size and format it had is not made on the device a Reset made: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }
  ComPtr<ID3D11ShaderResourceView> view;
  hr = device->CreateShaderResourceView(texture.Get(), nullptr, &view);
  if (FAILED(hr)) {
    ERR("D3D9: the view of a texture a Reset rebuilt is not there: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }
  texture_ = texture;
  view_ = view;
  context_ = context;
  for (UINT level = 0; level < levels_.size(); level++) {
    const Level &one = levels_[level];
    if (!one.bytes.empty())
      context_->UpdateSubresource(texture_.Get(), level, nullptr, one.bytes.data(), one.pitch, 0);
  }
  return S_OK;
}

HRESULT
Texture9Storage::generate_mips() {
  if (!view_ || levels_.size() < 2)
    return S_OK;
  if (!can_generate_mips_)
    return D3DERR_NOTAVAILABLE;
  context_->GenerateMips(view_.Get());
  // the generated levels are the GPU's; a later lock reads the CPU's bytes, so the chain is copied back now
  return refresh_cpu_levels();
}

// The generated chain is read back through a staging texture: the copy waits for the device's work, so the bytes are the
// ones the draws that follow would sample. Only a managed texture has CPU bytes to refresh.
HRESULT
Texture9Storage::refresh_cpu_levels() {
  if (levels_.empty())
    return S_OK;

  ComPtr<ID3D11Device> device;
  texture_->GetDevice(&device);
  D3D11_TEXTURE2D_DESC desc{};
  texture_->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  if (!device || FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    ERR_ONCE("D3D9: the generated mip chain has no staging texture to read back through.");
    return D3DERR_INVALIDCALL;
  }
  context_->CopyResource(staging.Get(), texture_.Get());

  for (UINT level = 0; level < levels_.size(); level++) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), level, D3D11_MAP_READ, 0, &mapped))) {
      ERR_ONCE("D3D9: a generated mip level could not be mapped for read back.");
      return D3DERR_INVALIDCALL;
    }
    Level &one = levels_[level];
    const size_t row_bytes = std::min<size_t>(one.pitch, mapped.RowPitch);
    const size_t rows = one.pitch ? one.bytes.size() / one.pitch : 0;
    for (size_t row = 0; row < rows; row++)
      memcpy(one.bytes.data() + row * one.pitch, (const char *)mapped.pData + row * mapped.RowPitch, row_bytes);
    context_->Unmap(staging.Get(), level);
  }
  return S_OK;
}

MTLD3D9Texture::MTLD3D9Texture(
    IDirect3DDevice9 *device, ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, D3DPOOL pool, UINT levels, bool can_generate_mips,
    DWORD usage
)
    : device_(device), device_ref_(device), texture_(std::move(texture)), view_(std::move(view)),
      storage_(
          texture_, view_, context, format, pool == D3DPOOL_MANAGED || (usage & D3DUSAGE_DYNAMIC) != 0,
          can_generate_mips, (usage & D3DUSAGE_DYNAMIC) != 0
      ),
      format_(format), pool_(pool), usage_(usage), levels_count_(levels) {
  // the managed pool is the one a Reset keeps, so a managed texture is the one the device rebuilds on the device it
  // makes; every other pool is the old device's (d3d9_device.cpp's Reset)
  if (pool_ == D3DPOOL_MANAGED)
    resources_.attach(device, this);
}

void
MTLD3D9Texture::reset_device(ID3D11Device *device, const ComPtr<ID3D11DeviceContext> &context) {
  if (pool_ != D3DPOOL_MANAGED || !device)
    return;
  if (FAILED(storage_.reset_device(device, context))) {
    ERR_ONCE("D3D9: a managed texture did not survive the device a Reset made: a draw that samples it reads a texture "
             "the old device has.");
    return;
  }
  // the texture and the view a draw samples are the new device's, and so are the levels of a lock taken after the
  // Reset; a level surface the application took before it still says what the texture it came from says
  texture_ = storage_.texture();
  view_ = storage_.view();
}

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
MTLD3D9Texture::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  return private_data_.set(guid, data, size, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetPrivateData(REFGUID guid, void *data, DWORD *size) {
  return private_data_.get(guid, data, size);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::FreePrivateData(REFGUID guid) {
  return private_data_.free(guid);
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
  if (storage_.generate_mips() == D3DERR_NOTAVAILABLE) {
    ERR_ONCE("D3D9: this texture's format has no generated mip levels (its levels are the game's own blocks).");
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) {
  if (level >= levels_count_ || !desc)
    return D3DERR_INVALIDCALL;
  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_->GetDesc(&texture_desc);
  *desc = to_surface_desc(texture_desc, format_, pool_, level);
  desc->Usage = usage_;
  return S_OK;
}

// A level's surface names this texture as its container and holds a reference on it, so the surface can outlive the
// application's texture reference and its lock still reaches the texture's storage. The texture does not hold its
// surfaces (that would be the other half of a reference cycle); each call makes one with its own reference, as the
// application's owns the reference GetSurfaceLevel hands back.
HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface_level) {
  if (level >= levels_count_ || !surface_level)
    return D3DERR_INVALIDCALL;
  *surface_level = nullptr;
  ComPtr<MTLD3D9Surface> surface;
  surface.Attach(new MTLD3D9Surface(
      device_, texture_, format_, pool_, false, storage_.context(), usage_, false,
      static_cast<IMTLD3D9LockTarget *>(this), level, static_cast<IDirect3DBaseTexture9 *>(this)
  ));
  *surface_level = surface.Detach();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!storage_.lockable())
    return D3DERR_INVALIDCALL;
  return storage_.lock(level, rect, locked, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::UnlockRect(UINT level) {
  if (!storage_.lockable())
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

MTLD3D9CubeTexture::MTLD3D9CubeTexture(
    IDirect3DDevice9 *device, ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, D3DPOOL pool, UINT levels, bool can_generate_mips,
    DWORD usage
)
    : device_(device), device_ref_(device), texture_(std::move(texture)), view_(std::move(view)),
      storage_(
          texture_, view_, context, format, pool == D3DPOOL_MANAGED || (usage & D3DUSAGE_DYNAMIC) != 0,
          can_generate_mips, (usage & D3DUSAGE_DYNAMIC) != 0
      ),
      format_(format), pool_(pool), usage_(usage), levels_count_(levels) {
  if (pool_ == D3DPOOL_MANAGED)
    resources_.attach(device, this);
}

void
MTLD3D9CubeTexture::reset_device(ID3D11Device *device, const ComPtr<ID3D11DeviceContext> &context) {
  if (pool_ != D3DPOOL_MANAGED || !device)
    return;
  if (FAILED(storage_.reset_device(device, context))) {
    ERR_ONCE("D3D9: a managed cube texture did not survive the device a Reset made.");
    return;
  }
  texture_ = storage_.texture();
  view_ = storage_.view();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9) ||
      riid == __uuidof(IDirect3DCubeTexture9)) {
    *object = static_cast<IDirect3DCubeTexture9 *>(this);
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
MTLD3D9CubeTexture::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (device_)
    device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  return private_data_.set(guid, data, size, flags);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetPrivateData(REFGUID guid, void *data, DWORD *size) {
  return private_data_.get(guid, data, size);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::FreePrivateData(REFGUID guid) {
  return private_data_.free(guid);
}
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::GetPriority() { return 0; }
void STDMETHODCALLTYPE MTLD3D9CubeTexture::PreLoad() {}
D3DRESOURCETYPE STDMETHODCALLTYPE MTLD3D9CubeTexture::GetType() { return D3DRTYPE_CUBETEXTURE; }
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::SetLOD(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::GetLOD() { return 0; }
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::GetLevelCount() { return levels_count_; }
HRESULT STDMETHODCALLTYPE MTLD3D9CubeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) {
  return D3DERR_INVALIDCALL;
}
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE MTLD3D9CubeTexture::GetAutoGenFilterType() { return D3DTEXF_LINEAR; }
void STDMETHODCALLTYPE MTLD3D9CubeTexture::GenerateMipSubLevels() {
  if (managed())
    storage_.generate_mips();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) {
  if (level >= levels_count_ || !desc)
    return D3DERR_INVALIDCALL;
  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_->GetDesc(&texture_desc);
  *desc = to_surface_desc(texture_desc, format_, pool_, level);
  desc->Usage = usage_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface_level) {
  if ((UINT)face > 5 || level >= levels_count_ || !surface_level)
    return D3DERR_INVALIDCALL;
  *surface_level = nullptr;
  ComPtr<MTLD3D9Surface> surface;
  surface.Attach(new MTLD3D9Surface(
      device_, texture_, format_, pool_, false, storage_.context(), usage_, false,
      static_cast<IMTLD3D9LockTarget *>(this), level, static_cast<IDirect3DBaseTexture9 *>(this), (UINT)face
  ));
  *surface_level = surface.Detach();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if ((UINT)face > 5 || level >= levels_count_ || !storage_.lockable())
    return D3DERR_INVALIDCALL;
  return storage_.lock(subresource(face, level), rect, locked, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level) {
  if ((UINT)face > 5 || !storage_.lockable() || !storage_.locked() ||
      storage_.locked_level() != subresource(face, level))
    return D3DERR_INVALIDCALL;
  return storage_.unlock();
}

HRESULT
MTLD3D9CubeTexture::lock_level(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) {
  return storage_.lock(level, rect, locked, flags);
}

HRESULT
MTLD3D9CubeTexture::unlock_level(UINT) {
  return storage_.unlock();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES, const RECT *) {
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

const D3DSAMPLERSTATETYPE *
SamplerState9::all_types(UINT &count) {
  static const D3DSAMPLERSTATETYPE types[] = {
      D3DSAMP_MINFILTER,   D3DSAMP_MAGFILTER,  D3DSAMP_MIPFILTER,    D3DSAMP_ADDRESSU,    D3DSAMP_ADDRESSV,
      D3DSAMP_ADDRESSW,    D3DSAMP_MAXANISOTROPY, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_BORDERCOLOR,
      D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX, D3DSAMP_DMAPOFFSET,
  };
  count = (UINT)(sizeof(types) / sizeof(types[0]));
  return types;
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

const D3DTEXTURESTAGESTATETYPE *
TextureStageState9::all_types(UINT &count) {
  static const D3DTEXTURESTAGESTATETYPE types[] = {
      D3DTSS_COLOROP,       D3DTSS_COLORARG1,  D3DTSS_COLORARG2,          D3DTSS_ALPHAOP,   D3DTSS_ALPHAARG1,
      D3DTSS_ALPHAARG2,     D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_RESULTARG,
  };
  count = (UINT)(sizeof(types) / sizeof(types[0]));
  return types;
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
    // the first set of texture coordinates, and no generated one: a camera-space map is another stage's coordinates
    return value == 0;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    return value == D3DTTFF_DISABLE || value == D3DTTFF_COUNT2;
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
  case D3DTSS_TEXCOORDINDEX:
    texcoord_index_ = value;
    break;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    texture_transform_flags_ = value;
    break;
  default:
    return S_OK; // a state whose only usable value is the one this stage has (the result register)
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
    *value = texcoord_index_;
    break;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    *value = texture_transform_flags_;
    break;
  default:
    *value = D3DTA_CURRENT;
    break;
  }
  return S_OK;
}

void
TextureStageState9::constants(UINT32 ops[4], UINT32 args[4], bool current_as_previous) const {
  auto argument = [&](DWORD value) -> UINT32 {
    if (current_as_previous && (value & D3DTA_SELECTMASK) == D3DTA_CURRENT)
      return 3;
    return argument_of(value);
  };
  ops[0] = operation_of(colour_op_);
  ops[1] = argument(colour_arg1_);
  ops[2] = argument(colour_arg2_);
  ops[3] = operation_of(alpha_op_);
  args[0] = argument(alpha_arg1_);
  args[1] = argument(alpha_arg2_);
  args[2] = 0;
  args[3] = 0;
}

Volume9Storage::Volume9Storage(
    ComPtr<ID3D11Texture3D> texture, const ComPtr<ID3D11ShaderResourceView> &view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, bool lockable, bool dynamic
)
    : texture_(std::move(texture)), view_(view), context_(context), lockable_(lockable), dynamic_(dynamic),
      pixel_bytes_(pixel_size(format)) {
  texture_->GetDesc(&desc_);
  if (!lockable_)
    return;
  levels_.resize(desc_.MipLevels);
  for (UINT mip = 0; mip < desc_.MipLevels; mip++) {
    Level &one = levels_[mip];
    one.width = std::max(1u, desc_.Width >> mip);
    one.height = std::max(1u, desc_.Height >> mip);
    one.depth = std::max(1u, desc_.Depth >> mip);
    one.pitch = one.width * pixel_bytes_;
    one.slice_pitch = one.pitch * one.height;
    one.bytes.resize((size_t)one.slice_pitch * one.depth);
  }
}

HRESULT
Volume9Storage::lock(UINT level, const D3DBOX *box, D3DLOCKED_BOX *locked, DWORD flags) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  locked->RowPitch = 0;
  locked->SlicePitch = 0;
  locked->pBits = nullptr;
  if (!lockable_ || level >= levels_.size() || locked_level_ != kNoLevel)
    return D3DERR_INVALIDCALL;
  const DWORD dynamic_allowed = D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK;
  const DWORD allowed = dynamic_ ? dynamic_allowed : (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
  if (flags & ~allowed)
    return D3DERR_INVALIDCALL;
  if (dynamic_ && (flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE))
    return D3DERR_INVALIDCALL;
  if ((flags & D3DLOCK_DISCARD) && box)
    return D3DERR_INVALIDCALL;

  Level &one = levels_[level];
  UINT left = 0, top = 0, front = 0;
  if (box) {
    if (box->Left >= box->Right || box->Top >= box->Bottom || box->Front >= box->Back || box->Right > one.width ||
        box->Bottom > one.height || box->Back > one.depth)
      return D3DERR_INVALIDCALL;
    left = box->Left;
    top = box->Top;
    front = box->Front;
  }
  if (dynamic_ && (flags & D3DLOCK_DISCARD))
    std::fill(one.bytes.begin(), one.bytes.end(), 0);

  locked->RowPitch = (INT)one.pitch;
  locked->SlicePitch = (INT)one.slice_pitch;
  locked->pBits = one.bytes.data() + (size_t)front * one.slice_pitch + (size_t)top * one.pitch +
                  (size_t)left * pixel_bytes_;
  locked_level_ = level;
  read_only_ = (flags & D3DLOCK_READONLY) != 0;
  return S_OK;
}

HRESULT
Volume9Storage::unlock() {
  if (locked_level_ == kNoLevel)
    return D3DERR_INVALIDCALL;
  const UINT level = locked_level_;
  const bool read_only = read_only_;
  locked_level_ = kNoLevel;
  read_only_ = false;
  if (!read_only) {
    const Level &one = levels_[level];
    context_->UpdateSubresource(texture_.Get(), level, nullptr, one.bytes.data(), one.pitch, one.slice_pitch);
  }
  return S_OK;
}

HRESULT
Volume9Storage::reset_device(ID3D11Device *device, const ComPtr<ID3D11DeviceContext> &context) {
  if (!device)
    return D3DERR_INVALIDCALL;
  ComPtr<ID3D11Texture3D> texture;
  HRESULT hr = device->CreateTexture3D(&desc_, nullptr, &texture);
  if (FAILED(hr))
    return D3DERR_INVALIDCALL;
  ComPtr<ID3D11ShaderResourceView> view;
  hr = device->CreateShaderResourceView(texture.Get(), nullptr, &view);
  if (FAILED(hr))
    return D3DERR_INVALIDCALL;
  texture_ = texture;
  view_ = view;
  context_ = context;
  for (UINT level = 0; level < levels_.size(); level++) {
    const Level &one = levels_[level];
    if (!one.bytes.empty())
      context_->UpdateSubresource(texture_.Get(), level, nullptr, one.bytes.data(), one.pitch, one.slice_pitch);
  }
  return S_OK;
}

MTLD3D9VolumeTexture::MTLD3D9VolumeTexture(
    IDirect3DDevice9 *device, ComPtr<ID3D11Texture3D> texture, ComPtr<ID3D11ShaderResourceView> view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, D3DPOOL pool, UINT levels, DWORD usage
)
    : device_(device), device_ref_(device), texture_(std::move(texture)), view_(std::move(view)),
      storage_(
          texture_, view_, context, format, pool == D3DPOOL_MANAGED || (usage & D3DUSAGE_DYNAMIC) != 0,
          (usage & D3DUSAGE_DYNAMIC) != 0
      ),
      format_(format), pool_(pool), usage_(usage), levels_count_(levels) {
  if (pool_ == D3DPOOL_MANAGED)
    resources_.attach(device, this);
}

void
MTLD3D9VolumeTexture::reset_device(ID3D11Device *device, const ComPtr<ID3D11DeviceContext> &context) {
  if (pool_ != D3DPOOL_MANAGED || !device)
    return;
  if (FAILED(storage_.reset_device(device, context))) {
    ERR_ONCE("D3D9: a managed volume texture did not survive the device a Reset made.");
    return;
  }
  texture_ = storage_.texture();
  view_ = storage_.view();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9) ||
      riid == __uuidof(IDirect3DVolumeTexture9)) {
    *object = static_cast<IDirect3DVolumeTexture9 *>(this);
  } else if (riid == IID_IMTLD3D9Texture) {
    *object = static_cast<IMTLD3D9Texture *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (device_)
    device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  return private_data_.set(guid, data, size, flags);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetPrivateData(REFGUID guid, void *data, DWORD *size) {
  return private_data_.get(guid, data, size);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::FreePrivateData(REFGUID guid) {
  return private_data_.free(guid);
}
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::GetPriority() { return 0; }
void STDMETHODCALLTYPE MTLD3D9VolumeTexture::PreLoad() {}
D3DRESOURCETYPE STDMETHODCALLTYPE MTLD3D9VolumeTexture::GetType() { return D3DRTYPE_VOLUMETEXTURE; }
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::SetLOD(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::GetLOD() { return 0; }
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::GetLevelCount() { return levels_count_; }
HRESULT STDMETHODCALLTYPE MTLD3D9VolumeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) {
  return D3DERR_INVALIDCALL;
}
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE MTLD3D9VolumeTexture::GetAutoGenFilterType() { return D3DTEXF_LINEAR; }
void STDMETHODCALLTYPE MTLD3D9VolumeTexture::GenerateMipSubLevels() {}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) {
  if (!desc || level >= levels_count_)
    return D3DERR_INVALIDCALL;
  D3D11_TEXTURE3D_DESC texture{};
  texture_->GetDesc(&texture);
  *desc = {};
  desc->Format = format_;
  desc->Type = D3DRTYPE_VOLUME;
  desc->Usage = usage_;
  desc->Pool = pool_;
  desc->Width = std::max(1u, texture.Width >> level);
  desc->Height = std::max(1u, texture.Height >> level);
  desc->Depth = std::max(1u, texture.Depth >> level);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetVolumeLevel(UINT level, IDirect3DVolume9 **volume_level) {
  if (!volume_level || level >= levels_count_)
    return D3DERR_INVALIDCALL;
  ComPtr<MTLD3D9Volume> volume;
  volume.Attach(new MTLD3D9Volume(this, level));
  *volume_level = volume.Detach();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) {
  return storage_.lock(level, box, locked, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::UnlockBox(UINT) {
  return storage_.unlock();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::AddDirtyBox(const D3DBOX *) {
  return S_OK;
}

MTLD3D9Volume::MTLD3D9Volume(MTLD3D9VolumeTexture *parent, UINT level)
    : texture_(parent), level_(level) {
  parent_.Attach(parent);
  parent_->AddRef();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DVolume9)) {
    *object = static_cast<IDirect3DVolume9 *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetDevice(IDirect3DDevice9 **device) {
  return texture_->GetDevice(device);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  return private_data_.set(guid, data, size, flags);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetPrivateData(REFGUID guid, void *data, DWORD *size) {
  return private_data_.get(guid, data, size);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::FreePrivateData(REFGUID guid) {
  return private_data_.free(guid);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetContainer(REFIID riid, void **container) {
  return texture_->QueryInterface(riid, container);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetDesc(D3DVOLUME_DESC *desc) {
  return texture_->GetLevelDesc(level_, desc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::LockBox(D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) {
  return texture_->LockBox(level_, locked, box, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::UnlockBox() {
  return texture_->UnlockBox(level_);
}

} // namespace mullion::d3d9
