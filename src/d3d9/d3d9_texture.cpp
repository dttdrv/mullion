#include "d3d9_texture.hpp"
#include <algorithm>
#include <cfloat>
#include <mutex>
#include <unordered_map>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9Texture = {0x8b1d0f27, 0x6a4c, 0x4e35, {0x8f, 0x71, 0x2c, 0x44, 0x9a, 0x0e, 0x63, 0xb8}};

namespace {

std::mutex g_texture_registry_lock;
std::unordered_map<IDirect3DBaseTexture9 *, IMTLD3D9Texture *> g_texture_registry;

} // namespace

void
register_base_texture(IDirect3DBaseTexture9 *iface, IMTLD3D9Texture *impl) {
  if (!iface || !impl)
    return;
  std::lock_guard<std::mutex> lock(g_texture_registry_lock);
  g_texture_registry[iface] = impl;
}

void
unregister_base_texture(IDirect3DBaseTexture9 *iface) {
  if (!iface)
    return;
  std::lock_guard<std::mutex> lock(g_texture_registry_lock);
  g_texture_registry.erase(iface);
}

IMTLD3D9Texture *
lookup_base_texture(IDirect3DBaseTexture9 *iface) {
  if (!iface)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_texture_registry_lock);
  auto found = g_texture_registry.find(iface);
  return found == g_texture_registry.end() ? nullptr : found->second;
}

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
      format_(format), pixel_bytes_(pixel_size(format)), block_bytes_(block_size(format)),
      can_generate_mips_(can_generate_mips) {
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
  // Face-major packing with 16-byte padding between cube faces (Wine device.c cube LockRect pBits layout).
  size_t offset = 0;
  const UINT mips = desc.MipLevels ? desc.MipLevels : 1;
  const UINT faces = desc.ArraySize ? desc.ArraySize : 1;
  for (UINT face = 0; face < faces; face++) {
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT sub = face * mips + mip;
      Level &one = levels_[sub];
      one.width = std::max(1u, desc.Width >> mip);
      one.height = std::max(1u, desc.Height >> mip);
      if (block_bytes_) {
        one.pitch = ((one.width + 3) / 4) * block_bytes_;
        one.size = (size_t)one.pitch * ((one.height + 3) / 4);
      } else {
        one.pitch = format_row_pitch(format_, one.width);
        if (!one.pitch)
          one.pitch = one.width * std::max(1u, pixel_bytes_);
        const UINT rows = format_height_blocks(format_, one.height);
        one.size = (size_t)one.pitch * (rows ? rows : one.height);
      }
      one.offset = offset;
      offset += one.size;
    }
    if (faces > 1)
      offset = (offset + 15u) & ~size_t{15};
  }
  blob_.assign(offset, 0);
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
  // GetDC holds the whole texture; another LockRect of this level is refused without wiping outputs.
  if (dc_held_ || locked_levels_.count(level))
    return D3DERR_INVALIDCALL;
  locked->Pitch = 0;
  locked->pBits = nullptr;
  // Non-lockable DEFAULT textures keep no CPU levels: refuse before indexing levels_ (Wine test_resource_access
  // locks GetCubeMapSurface of a default cube; the surface path must not crash on an empty shadow).
  if (!lockable_ || level >= level_count_ || level >= levels_.size())
    return D3DERR_INVALIDCALL;
  // Managed/SYSTEMMEM/SCRATCH accept DISCARD as a no-op flag (Wine visual.c locks managed textures with DISCARD).
  const DWORD dynamic_allowed = D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK;
  const DWORD managed_allowed = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_DISCARD;
  const DWORD allowed = dynamic_ ? dynamic_allowed : managed_allowed;
  if (flags & ~allowed)
    return D3DERR_INVALIDCALL;
  if (dynamic_ && (flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE))
    return D3DERR_INVALIDCALL;

  Level &one = levels_[level];
  LONG left = 0, top = 0;
  if (rect) {
    const bool invalid = rect->left < 0 || rect->top < 0 || rect->right > (LONG)one.width ||
                         rect->bottom > (LONG)one.height || rect->right <= rect->left || rect->bottom <= rect->top;
    // DEFAULT DYNAMIC refuses mid-block starts; short edges are allowed only at the mip boundary
    // (Wine device.c mip-level half-block lock of a 4x4 DXT texture succeeds on level 1).
    const bool unaligned =
        (block_bytes_ &&
         ((rect->left & 3) || (rect->top & 3) ||
          ((rect->right & 3) && rect->right != (LONG)one.width) ||
          ((rect->bottom & 3) && rect->bottom != (LONG)one.height))) ||
        ((format_ == D3DFMT_YUY2 || format_ == D3DFMT_UYVY) &&
         ((rect->left & 1) || ((rect->right & 1) && rect->right != (LONG)one.width)));
    // MANAGED/SYSTEMMEM/SCRATCH accept invalid/partial-block locks (pools[].success TRUE).
    if ((invalid || unaligned) && dynamic_)
      return D3DERR_INVALIDCALL;
    left = rect->left;
    top = rect->top;
  }

  locked->Pitch = (INT)one.pitch;
  locked->pBits = level_bits(one) + (size_t)(block_bytes_ ? top / 4 : top) * one.pitch +
                  (size_t)(block_bytes_ ? left / 4 : left) * (block_bytes_ ? block_bytes_ : pixel_bytes_);
  locked_levels_.insert(level);
  lock_read_only_[level] = (flags & D3DLOCK_READONLY) != 0;
  return S_OK;
}

// What a lock's bytes become: the level's contents, uploaded into the texture. A managed texture's lower levels are the
// ones the game authored, not a chain made again from the top level: D3DPOOL_MANAGED is not D3DUSAGE_AUTOGENMIPMAP, and
// Wine's d3d9 uploads a managed texture's lock without generating mips (Microsoft Learn, D3DPOOL; Wine dlls/d3d9).
HRESULT
Texture9Storage::unlock() {
  if (!lockable_ || locked_levels_.empty())
    return D3DERR_INVALIDCALL;
  const UINT level = *locked_levels_.begin();
  return unlock_level(level);
}

HRESULT
Texture9Storage::peek_level(UINT level, D3DLOCKED_RECT *locked) const {
  if (!locked || !lockable_ || level >= levels_.size())
    return D3DERR_INVALIDCALL;
  const Level &one = levels_[level];
  locked->Pitch = (INT)one.pitch;
  locked->pBits = const_cast<char *>(level_bits(one));
  return S_OK;
}

// D3D9's L8 is a luminance texture: every colour channel samples the byte and alpha is opaque. DXGI's R8 samples
// (L, 0, 0, 1) instead, and a D3D11 shader resource view has no swizzle, so an L8 texture is sampled through a
// B8G8R8A8 twin whose three colour channels all hold the byte (Wine d3d9ex.c test_user_memory samples one and reads
// 0x007f7f7f for a byte of 0x7f).
bool
Texture9Storage::luminance_sampling() const {
  return format_ == D3DFMT_L8 && desc_.Format == DXGI_FORMAT_R8_UNORM &&
         (desc_.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

ID3D11ShaderResourceView *
Texture9Storage::view() {
  if (!luminance_sampling())
    return view_.Get();
  if (luminance_view_)
    return luminance_view_.Get();
  if (FAILED(build_luminance_twin()))
    return view_.Get(); // the texture still samples, as the red-only value an R8 view gives
  return luminance_view_.Get();
}

void
Texture9Storage::sampling_changed() {
  if (luminance_sampling())
    luminance_view_.Reset();
}

HRESULT
Texture9Storage::build_luminance_twin() {
  ComPtr<ID3D11Device> device;
  texture_->GetDevice(&device);
  if (!device)
    return D3DERR_INVALIDCALL;
  if (!luminance_texture_) {
    D3D11_TEXTURE2D_DESC expanded{};
    texture_->GetDesc(&expanded);
    expanded.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    expanded.Usage = D3D11_USAGE_DEFAULT;
    expanded.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    expanded.CPUAccessFlags = 0;
    // a cube's twin is a cube too; nothing else about the resource is sampled
    expanded.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(device->CreateTexture2D(&expanded, nullptr, &luminance_texture_))) {
      ERR_ONCE("D3D9: a luminance texture has no B8G8R8A8 twin to be sampled through.");
      return D3DERR_INVALIDCALL;
    }
  }

  // the texture's pixels as the device has them: a staging copy waits for the work that wrote them
  D3D11_TEXTURE2D_DESC source{};
  texture_->GetDesc(&source);
  D3D11_TEXTURE2D_DESC staging_desc = source;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    ERR_ONCE("D3D9: a luminance texture has no staging texture to be read back through.");
    return D3DERR_INVALIDCALL;
  }
  context_->CopyResource(staging.Get(), texture_.Get());

  const UINT mips = source.MipLevels ? source.MipLevels : 1;
  const UINT faces = source.ArraySize ? source.ArraySize : 1;
  std::vector<uint32_t> level;
  for (UINT face = 0; face < faces; face++) {
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT sub = face * mips + mip;
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(context_->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped)))
        return D3DERR_INVALIDCALL;
      const UINT width = std::max(1u, source.Width >> mip);
      const UINT height = std::max(1u, source.Height >> mip);
      level.resize((size_t)width * height);
      const auto *bytes = static_cast<const uint8_t *>(mapped.pData);
      for (UINT y = 0; y < height; y++) {
        const uint8_t *row = bytes + (size_t)y * mapped.RowPitch;
        uint32_t *out = level.data() + (size_t)y * width;
        for (UINT x = 0; x < width; x++) {
          const uint32_t value = row[x]; // B8G8R8A8 ascending bytes: B, G, R, A
          out[x] = value | (value << 8) | (value << 16) | 0xFF000000u;
        }
      }
      context_->Unmap(staging.Get(), sub);
      context_->UpdateSubresource(luminance_texture_.Get(), sub, nullptr, level.data(), width * 4, 0);
    }
  }
  if (FAILED(device->CreateShaderResourceView(luminance_texture_.Get(), nullptr, &luminance_view_))) {
    ERR_ONCE("D3D9: the B8G8R8A8 twin of a luminance texture has no view.");
    return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

HRESULT
Texture9Storage::unlock_level(UINT level) {
  if (!lockable_ || !locked_levels_.count(level))
    return D3DERR_INVALIDCALL;
  const bool read_only = lock_read_only_[level];
  locked_levels_.erase(level);
  lock_read_only_.erase(level);

  // SYSTEMMEM/SCRATCH are staging textures: UpdateSubresource is invalid on them and crashes the Metal backend.
  // Depth-stencil bindings similarly reject UpdateSubresource (winemetal unix call abort). The CPU shadow remains
  // the lock content for DYNAMIC depth textures; UpdateTexture/draws that need GPU depth use other paths.
  if (!read_only && desc_.Usage != D3D11_USAGE_STAGING && !(desc_.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    const Level &one = levels_[level];
    context_->UpdateSubresource(texture_.Get(), level, nullptr, level_bits(one), one.pitch, 0);
    sampling_changed();
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
    if (one.size)
      context_->UpdateSubresource(texture_.Get(), level, nullptr, level_bits(one), one.pitch, 0);
  }
  // the twin belongs to the device that went with the old texture
  luminance_texture_.Reset();
  luminance_view_.Reset();
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
    const size_t rows = one.pitch ? one.size / one.pitch : 0;
    for (size_t row = 0; row < rows; row++)
      memcpy(level_bits(one) + row * one.pitch, (const char *)mapped.pData + row * mapped.RowPitch, row_bytes);
    context_->Unmap(staging.Get(), level);
  }
  sampling_changed();
  return S_OK;
}

MTLD3D9Texture::MTLD3D9Texture(
    IDirect3DDevice9 *device, ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, D3DPOOL pool, UINT levels, bool can_generate_mips,
    DWORD usage, void *user_memory, UINT user_pitch
)
    : device_(device), device_ref_(device, true), texture_(std::move(texture)), view_(std::move(view)),
      storage_(
          texture_, view_, context, format,
          pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ||
              (usage & D3DUSAGE_DYNAMIC) != 0 || user_memory != nullptr,
          can_generate_mips, (usage & D3DUSAGE_DYNAMIC) != 0
      ),
      format_(format), pool_(pool), usage_(usage), levels_count_(levels), user_memory_(user_memory),
      user_pitch_(user_pitch) {
  // the managed pool is the one a Reset keeps, so a managed texture is the one the device rebuilds on the device it
  // makes; every other pool is the old device's (d3d9_device.cpp's Reset)
  if (pool_ == D3DPOOL_MANAGED)
    resources_.attach(device, this);
  register_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DTexture9 *>(this)), this);
}

MTLD3D9Texture::~MTLD3D9Texture() {
  unregister_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DTexture9 *>(this)));
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
  view_ = storage_.base_view();
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
MTLD3D9Texture::SetPriority(DWORD priority) {
  const DWORD previous = priority_;
  // Only D3DPOOL_MANAGED stores priority (Wine device.c test_resource_priority)
  if (pool_ == D3DPOOL_MANAGED)
    priority_ = priority;
  return previous;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetPriority() {
  return priority_;
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

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter_type) {
  // D3DTEXF_NONE is refused; POINT/LINEAR/ANISOTROPIC and the quad filters are accepted. A texture without
  // D3DUSAGE_AUTOGENMIPMAP still accepts the filter -- it has no effect on it -- rather than refusing the call:
  // Wine's test_generate_mipmap sets the filter on a render-target texture and expects D3D_OK.
  if (filter_type == D3DTEXF_NONE ||
      (filter_type != D3DTEXF_POINT && filter_type != D3DTEXF_LINEAR && filter_type != D3DTEXF_ANISOTROPIC &&
       filter_type != D3DTEXF_PYRAMIDALQUAD && filter_type != D3DTEXF_GAUSSIANQUAD))
    return D3DERR_INVALIDCALL;
  autogen_filter_ = filter_type;
  return S_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetAutoGenFilterType() {
  return autogen_filter_;
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

// A level's surface names this texture as its container and shares the texture's public reference count (Wine's
// test_refcount: GetSurfaceLevel AddRefs the texture, and AddRef/Release of the surface move that same count). The
// surface object is owned by the texture and lives until the texture is destroyed.
HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface_level) {
  if (level >= levels_count_ || !surface_level)
    return D3DERR_INVALIDCALL;
  *surface_level = nullptr;
  if (level_surfaces_.size() < levels_count_)
    level_surfaces_.resize(levels_count_);
  if (!level_surfaces_[level]) {
    // Only D3DUSAGE_DEPTHSTENCIL textures answer SetDepthStencilSurface (Wine test_resource_access checks Usage).
    const bool depth_bind = (usage_ & D3DUSAGE_DEPTHSTENCIL) != 0;
    auto surface = std::make_unique<MTLD3D9Surface>(
        device_, texture_, format_, pool_, false, storage_.context(), usage_, depth_bind,
        static_cast<IMTLD3D9LockTarget *>(this), level, static_cast<IDirect3DBaseTexture9 *>(this), 0, nullptr, 0, true,
        false
    );
    surface->set_share_parent(static_cast<IDirect3DBaseTexture9 *>(this));
    level_surfaces_[level] = std::move(surface);
  }
  *surface_level = level_surfaces_[level].get();
  (*surface_level)->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (user_memory_) {
    if (level != 0 || !locked)
      return D3DERR_INVALIDCALL;
    const LONG left = rect ? rect->left : 0;
    const LONG top = rect ? rect->top : 0;
    const LONG right = rect ? rect->right : (LONG)(user_pitch_ / 4);
    const LONG bottom = rect ? rect->bottom : (LONG)levels_count_;
    (void)right;
    (void)bottom;
    locked->Pitch = (INT)user_pitch_;
    // A8R8G8B8-family formats are four bytes per texel; the only caller of the user-memory path is the D3D9Ex
    // system-memory contract, whose row pitch is the texture width times four.
    locked->pBits = static_cast<char *>(user_memory_) + (size_t)top * user_pitch_ + (size_t)left * 4;
    return S_OK;
  }
  if (!storage_.lockable() || storage_.dc_held())
    return D3DERR_INVALIDCALL;
  return storage_.lock(level, rect, locked, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::UnlockRect(UINT level) {
  if (level >= levels_count_)
    return D3DERR_INVALIDCALL;
  // a level that is not locked still unlocks (Wine surface.c: the resource being unmapped when nothing is mapped
  // answers WINEDDERR_NOTLOCKED, which a texture's level's unlock replaces with D3D_OK; a plain surface keeps
  // D3DERR_INVALIDCALL)
  if (user_memory_)
    return level == 0 ? S_OK : D3DERR_INVALIDCALL;
  if (!storage_.lockable() || !storage_.level_locked(level))
    // any level that is not locked unlocks as D3D_OK, whether lockable or not (Wine surface.c: the resource being
    // unmapped when nothing is mapped answers WINEDDERR_NOTLOCKED, which a texture's level unlock replaces with
    // D3D_OK; the device.c test_resource_access unlocks twice and reads D3D_OK both times)
    return S_OK;
  return storage_.unlock_level(level);
}

HRESULT
MTLD3D9Texture::lock_level(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) {
  return LockRect(level, locked, rect, flags);
}

HRESULT
MTLD3D9Texture::unlock_level(UINT level) {
  if (!storage_.lockable())
    return D3DERR_INVALIDCALL;
  // GetSurfaceLevel surfaces share this path: a redundant UnlockRect is D3D_OK for D3DRTYPE_TEXTURE.
  if (!storage_.level_locked(level))
    return S_OK;
  return storage_.unlock_level(level);
}

void
MTLD3D9Texture::acquire_dc() {
  storage_.acquire_dc();
}

void
MTLD3D9Texture::release_dc() {
  storage_.release_dc();
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
    : device_(device), device_ref_(device, true), texture_(std::move(texture)), view_(std::move(view)),
      storage_(
          texture_, view_, context, format,
          pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ||
              (usage & D3DUSAGE_DYNAMIC) != 0,
          can_generate_mips, (usage & D3DUSAGE_DYNAMIC) != 0
      ),
      format_(format), pool_(pool), usage_(usage), levels_count_(levels) {
  if (pool_ == D3DPOOL_MANAGED)
    resources_.attach(device, this);
  register_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DCubeTexture9 *>(this)), this);
}

MTLD3D9CubeTexture::~MTLD3D9CubeTexture() {
  unregister_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DCubeTexture9 *>(this)));
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
  view_ = storage_.base_view();
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
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::SetPriority(DWORD priority) {
  const DWORD previous = priority_;
  if (pool_ == D3DPOOL_MANAGED)
    priority_ = priority;
  return previous;
}
DWORD STDMETHODCALLTYPE MTLD3D9CubeTexture::GetPriority() { return priority_; }
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
  const UINT slot = (UINT)face * levels_count_ + level;
  if (face_surfaces_.size() < 6u * levels_count_)
    face_surfaces_.resize(6u * levels_count_);
  if (!face_surfaces_[slot]) {
    auto surface = std::make_unique<MTLD3D9Surface>(
        device_, texture_, format_, pool_, false, storage_.context(), usage_, false,
        static_cast<IMTLD3D9LockTarget *>(this), level, static_cast<IDirect3DBaseTexture9 *>(this), (UINT)face, nullptr,
        0, true, false
    );
    surface->set_share_parent(static_cast<IDirect3DBaseTexture9 *>(this));
    face_surfaces_[slot] = std::move(surface);
  }
  *surface_level = face_surfaces_[slot].get();
  (*surface_level)->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if ((UINT)face > 5 || level >= levels_count_ || !storage_.lockable() || storage_.dc_held())
    return D3DERR_INVALIDCALL;
  return storage_.lock(subresource(face, level), rect, locked, flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level) {
  if ((UINT)face > 5 || !storage_.lockable())
    return D3DERR_INVALIDCALL;
  const UINT sub = subresource(face, level);
  if (!storage_.level_locked(sub))
    return storage_.dc_held() ? S_OK : D3DERR_INVALIDCALL;
  return storage_.unlock_level(sub);
}

HRESULT
MTLD3D9CubeTexture::lock_level(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) {
  // Same gate as LockRect: surface locks must not bypass the pool/usage lockability check.
  if (level >= 6u * levels_count_ || !storage_.lockable() || storage_.dc_held())
    return D3DERR_INVALIDCALL;
  return storage_.lock(level, rect, locked, flags);
}

HRESULT
MTLD3D9CubeTexture::unlock_level(UINT level) {
  if (!storage_.lockable())
    return D3DERR_INVALIDCALL;
  // Wine device.c test_getdc: UnlockRect of a face that is not locked while another face holds GetDC returns S_OK.
  if (!storage_.level_locked(level))
    return storage_.dc_held() ? S_OK : D3DERR_INVALIDCALL;
  return storage_.unlock_level(level);
}

void
MTLD3D9CubeTexture::acquire_dc() {
  storage_.acquire_dc();
}

void
MTLD3D9CubeTexture::release_dc() {
  storage_.release_dc();
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
    // stored for GetSamplerState / state blocks; Wine device.c sets D3DSAMP_SRGBTEXTURE TRUE on every sampler
    return true;
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
  case D3DSAMP_MIPMAPLODBIAS:
    mip_lod_bias_ = value;
    break;
  case D3DSAMP_MAXMIPLEVEL:
    max_mip_level_ = value;
    break;
  case D3DSAMP_BORDERCOLOR:
    border_color_ = value;
    break;
  case D3DSAMP_SRGBTEXTURE:
    srgb_texture_ = value;
    break;
  case D3DSAMP_ELEMENTINDEX:
    element_index_ = value;
    break;
  case D3DSAMP_DMAPOFFSET:
    dmap_offset_ = value;
    break;
  default:
    return S_OK;
  }
  // filter/address/anisotropy rebuild the D3D11 sampler; SRGB and friends are recorded for Get without remaking it
  if (type == D3DSAMP_MINFILTER || type == D3DSAMP_MAGFILTER || type == D3DSAMP_MIPFILTER ||
      type == D3DSAMP_ADDRESSU || type == D3DSAMP_ADDRESSV || type == D3DSAMP_ADDRESSW ||
      type == D3DSAMP_MAXANISOTROPY)
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
  case D3DSAMP_MIPMAPLODBIAS:
    *value = mip_lod_bias_;
    break;
  case D3DSAMP_MAXMIPLEVEL:
    *value = max_mip_level_;
    break;
  case D3DSAMP_BORDERCOLOR:
    *value = border_color_;
    break;
  case D3DSAMP_SRGBTEXTURE:
    *value = srgb_texture_;
    break;
  case D3DSAMP_ELEMENTINDEX:
    *value = element_index_;
    break;
  case D3DSAMP_DMAPOFFSET:
    *value = dmap_offset_;
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
  case D3DTSS_BUMPENVMAT00:
  case D3DTSS_BUMPENVMAT01:
  case D3DTSS_BUMPENVMAT10:
  case D3DTSS_BUMPENVMAT11:
  case D3DTSS_TEXCOORDINDEX:
  case D3DTSS_BUMPENVLSCALE:
  case D3DTSS_BUMPENVLOFFSET:
  case D3DTSS_TEXTURETRANSFORMFLAGS:
  case D3DTSS_COLORARG0:
  case D3DTSS_ALPHAARG0:
  case D3DTSS_RESULTARG:
  case D3DTSS_CONSTANT:
    return true;
  default:
    return false;
  }
}

const D3DTEXTURESTAGESTATETYPE *
TextureStageState9::all_types(UINT &count) {
  static const D3DTEXTURESTAGESTATETYPE types[] = {
      D3DTSS_COLOROP,       D3DTSS_COLORARG1,  D3DTSS_COLORARG2,          D3DTSS_ALPHAOP,   D3DTSS_ALPHAARG1,
      D3DTSS_ALPHAARG2,     D3DTSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT01,     D3DTSS_BUMPENVMAT10, D3DTSS_BUMPENVMAT11,
      D3DTSS_TEXCOORDINDEX, D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET, D3DTSS_TEXTURETRANSFORMFLAGS,
      D3DTSS_COLORARG0,     D3DTSS_ALPHAARG0,  D3DTSS_RESULTARG,          D3DTSS_CONSTANT,
  };
  count = (UINT)(sizeof(types) / sizeof(types[0]));
  return types;
}

bool
TextureStageState9::usable_value(D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  switch (type) {
  case D3DTSS_COLOROP:
  case D3DTSS_ALPHAOP: {
    // the operations the eight-stage combiner honours: every D3DTEXTUREOP but the four this build refuses, the
    // ones that would blend differently than the formula the pixel shader carries. PREMODULATE is not defined on
    // D3D9's feature levels and the two bump-environment maps are real effects their own maths, which would need
    // their BUMPENVMAT matrices read in the pixel shader; drawing them as a different operation would be worse.
    // D3DTOP_DOTPRODUCT3 is refused at set as an operation this build has not (tests/d3d9/d3d9_texture.cpp)
    switch (value) {
    case D3DTOP_DISABLE:
    case D3DTOP_SELECTARG1:
    case D3DTOP_SELECTARG2:
    case D3DTOP_MODULATE:
    case D3DTOP_MODULATE2X:
    case D3DTOP_MODULATE4X:
    case D3DTOP_ADD:
    case D3DTOP_ADDSIGNED:
    case D3DTOP_ADDSIGNED2X:
    case D3DTOP_SUBTRACT:
    case D3DTOP_ADDSMOOTH:
    case D3DTOP_BLENDDIFFUSEALPHA:
    case D3DTOP_BLENDTEXTUREALPHA:
    case D3DTOP_BLENDFACTORALPHA:
    case D3DTOP_BLENDTEXTUREALPHAPM:
    case D3DTOP_BLENDCURRENTALPHA:
    case D3DTOP_MODULATEALPHA_ADDCOLOR:
    case D3DTOP_MODULATECOLOR_ADDALPHA:
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
    case D3DTOP_MULTIPLYADD:
    case D3DTOP_LERP:
      return true;
    default:
      return false;
    }
  }
  case D3DTSS_COLORARG1:
  case D3DTSS_COLORARG2:
  case D3DTSS_ALPHAARG1:
  case D3DTSS_ALPHAARG2:
  case D3DTSS_COLORARG0:
  case D3DTSS_ALPHAARG0:
  case D3DTSS_RESULTARG: {
    // the modifiers are honoured by the combiner's argument decoder (D3DTA_COMPLEMENT reads 1 - value,
    // D3DTA_ALPHAREPLICATE replicates the value's alpha to all four channels, and both may combine); a complemented
    // texture argument is refused, as is anything past those two modifiers (tests/d3d9/d3d9_texture.cpp)
    if (value & ~(DWORD)(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE))
      return false;
    if ((value & D3DTA_COMPLEMENT) && (value & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
      return false;
    switch (value & D3DTA_SELECTMASK) {
    case D3DTA_TEXTURE:
    case D3DTA_DIFFUSE:
    case D3DTA_CURRENT:
    case D3DTA_TFACTOR:
    case D3DTA_SPECULAR:
    case D3DTA_TEMP:
    case D3DTA_CONSTANT:
      return true;
    default:
      return false;
    }
  }
  case D3DTSS_TEXCOORDINDEX:
    // stages default to their own index; generated coordinates and higher indices are stored for Get
    return true;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    // D3D9 stores the flags a game sets and the fixed-function pixel shader reads them (tests/d3d9/
    // fixed_function_shaders.cpp, texture_coordinate): D3DTTFF_COUNT1 passes the coordinate through, COUNT2 crops it,
    // a PROJECTED stage divides by the third coordinate for COUNT3 and by the fourth for every other combination, and
    // D3DTTFF_PROJECTED alone acts like COUNT4 (Microsoft Learn, D3DTEXTURETRANSFORMFLAGS; Wine visual.c's
    // projected_textures_test sets 0xffffffff and COUNT1 and expects the flags back from Get). A value this device
    // does not recognise is therefore stored, not refused.
    return true;
  case D3DTSS_BUMPENVMAT00:
  case D3DTSS_BUMPENVMAT01:
  case D3DTSS_BUMPENVMAT10:
  case D3DTSS_BUMPENVMAT11:
  case D3DTSS_BUMPENVLSCALE:
  case D3DTSS_BUMPENVLOFFSET:
  case D3DTSS_CONSTANT:
    return true;
  default:
    return false;
  }
}

void
TextureStageState9::bump_env(float matrix[4], float &lscale, float &loffset) const {
  const DWORD values[4] = {bump_env_mat00_, bump_env_mat01_, bump_env_mat10_, bump_env_mat11_};
  for (UINT at = 0; at < 4; at++)
    memcpy(&matrix[at], &values[at], sizeof(float));
  memcpy(&lscale, &bump_env_lscale_, sizeof(float));
  memcpy(&loffset, &bump_env_loffset_, sizeof(float));
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
  case D3DTSS_COLORARG0:
    colour_arg0_ = value;
    break;
  case D3DTSS_ALPHAARG0:
    alpha_arg0_ = value;
    break;
  case D3DTSS_RESULTARG:
    result_arg_ = value;
    break;
  case D3DTSS_CONSTANT:
    constant_ = value;
    break;
  case D3DTSS_BUMPENVMAT00:
    bump_env_mat00_ = value;
    break;
  case D3DTSS_BUMPENVMAT01:
    bump_env_mat01_ = value;
    break;
  case D3DTSS_BUMPENVMAT10:
    bump_env_mat10_ = value;
    break;
  case D3DTSS_BUMPENVMAT11:
    bump_env_mat11_ = value;
    break;
  case D3DTSS_BUMPENVLSCALE:
    bump_env_lscale_ = value;
    break;
  case D3DTSS_BUMPENVLOFFSET:
    bump_env_loffset_ = value;
    break;
  case D3DTSS_TEXCOORDINDEX:
    texcoord_index_ = value;
    break;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    texture_transform_flags_ = value;
    break;
  default:
    return S_OK;
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
  case D3DTSS_COLORARG0:
    *value = colour_arg0_;
    break;
  case D3DTSS_ALPHAARG0:
    *value = alpha_arg0_;
    break;
  case D3DTSS_RESULTARG:
    *value = result_arg_;
    break;
  case D3DTSS_CONSTANT:
    *value = constant_;
    break;
  case D3DTSS_BUMPENVMAT00:
    *value = bump_env_mat00_;
    break;
  case D3DTSS_BUMPENVMAT01:
    *value = bump_env_mat01_;
    break;
  case D3DTSS_BUMPENVMAT10:
    *value = bump_env_mat10_;
    break;
  case D3DTSS_BUMPENVMAT11:
    *value = bump_env_mat11_;
    break;
  case D3DTSS_BUMPENVLSCALE:
    *value = bump_env_lscale_;
    break;
  case D3DTSS_BUMPENVLOFFSET:
    *value = bump_env_loffset_;
    break;
  case D3DTSS_TEXCOORDINDEX:
    *value = texcoord_index_;
    break;
  case D3DTSS_TEXTURETRANSFORMFLAGS:
    *value = texture_transform_flags_;
    break;
  default:
    *value = 0;
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

void
TextureStageState9::eight_board(UINT32 ops[4], UINT32 args[4], UINT32 args0[4]) const {
  // the eight-stage combiner's board row: every value stays the raw D3D9 one it was set with, so the pixel shader
  // decodes the argument's modifiers itself and the raw D3DTOP values drive the operations (d3d9_texmemory.hpp's
  // instruction counts are the two-stage encodings, which this path does not use)
  ops[0] = (UINT32)colour_op_;
  ops[1] = (UINT32)colour_arg1_;
  ops[2] = (UINT32)colour_arg2_;
  ops[3] = (UINT32)alpha_op_;
  args[0] = (UINT32)alpha_arg1_;
  args[1] = (UINT32)alpha_arg2_;
  args[2] = (UINT32)result_arg_;
  args[3] = (UINT32)texture_transform_flags_;
  args0[0] = (UINT32)colour_arg0_;
  args0[1] = (UINT32)alpha_arg0_;
  // the coordinate pass-through index: the raw D3DTSS_TEXCOORDINDEX keeps the texture-coordinate generation flags in
  // its upper half, which this build does not generate coordinates for; such a stage's coordinate stays zero (Wine's
  // fixed-function vertex shader fills the varying the vertex does not carry with vec4(0))
  args0[2] = (texcoord_index_ & 0xFFFF0000u) ? 0xffffffffu : texcoord_index_;
  args0[3] = 0;
}

void
Volume9Storage::init_levels(UINT width, UINT height, UINT depth, UINT levels) {
  levels_.resize(levels);
  for (UINT mip = 0; mip < levels; mip++) {
    Level &one = levels_[mip];
    one.width = std::max(1u, width >> mip);
    one.height = std::max(1u, height >> mip);
    one.depth = std::max(1u, depth >> mip);
    one.pitch = format_row_pitch(format_, one.width);
    if (!one.pitch)
      one.pitch = one.width * (pixel_bytes_ ? pixel_bytes_ : 4);
    one.slice_pitch = one.pitch * format_height_blocks(format_, one.height);
    one.bytes.resize((size_t)one.slice_pitch * one.depth);
  }
}

Volume9Storage::Volume9Storage(
    ComPtr<ID3D11Texture3D> texture, const ComPtr<ID3D11ShaderResourceView> &view,
    const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, bool lockable, bool dynamic
)
    : texture_(std::move(texture)), view_(view), context_(context), lockable_(lockable), dynamic_(dynamic),
      format_(format), pixel_bytes_(pixel_size(format)) {
  if (texture_)
    texture_->GetDesc(&desc_);
  if (!lockable_)
    return;
  init_levels(desc_.Width, desc_.Height, desc_.Depth, desc_.MipLevels ? desc_.MipLevels : 1);
}

Volume9Storage::Volume9Storage(D3DFORMAT format, UINT width, UINT height, UINT depth, UINT levels, bool dynamic)
    : lockable_(true), dynamic_(dynamic), format_(format), pixel_bytes_(pixel_size(format)) {
  desc_.Width = width;
  desc_.Height = height;
  desc_.Depth = depth;
  desc_.MipLevels = levels;
  desc_.Format = DXGI_FORMAT_UNKNOWN;
  desc_.Usage = D3D11_USAGE_STAGING;
  init_levels(width, height, depth, levels);
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
  const DWORD managed_allowed = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_DISCARD;
  const DWORD allowed = dynamic_ ? dynamic_allowed : managed_allowed;
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
    // Compressed / YUV volumes refuse mid-block lock starts; edges may be short only at the mip boundary
    // (Wine device.c test_volume_blocks: level-0 partial blocks fail, level-1 half-block boxes succeed).
    if (block_size(format_)) {
      if ((box->Left & 3) || (box->Top & 3))
        return D3DERR_INVALIDCALL;
      if (((box->Right & 3) && box->Right != one.width) || ((box->Bottom & 3) && box->Bottom != one.height))
        return D3DERR_INVALIDCALL;
    } else if (format_ == D3DFMT_YUY2 || format_ == D3DFMT_UYVY) {
      if (box->Left & 1)
        return D3DERR_INVALIDCALL;
      if ((box->Right & 1) && box->Right != one.width)
        return D3DERR_INVALIDCALL;
    }
    left = box->Left;
    top = box->Top;
    front = box->Front;
  }
  if (dynamic_ && (flags & D3DLOCK_DISCARD))
    std::fill(one.bytes.begin(), one.bytes.end(), 0);

  size_t offset = (size_t)front * one.slice_pitch;
  if (block_size(format_)) {
    offset += (size_t)(top / 4) * one.pitch + (size_t)(left / 4) * block_size(format_);
  } else if (format_ == D3DFMT_YUY2 || format_ == D3DFMT_UYVY) {
    offset += (size_t)top * one.pitch + (size_t)left * 2;
  } else {
    offset += (size_t)top * one.pitch + (size_t)left * pixel_bytes_;
  }
  locked->RowPitch = (INT)one.pitch;
  locked->SlicePitch = (INT)one.slice_pitch;
  locked->pBits = one.bytes.data() + offset;
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
  if (!read_only && desc_.Usage != D3D11_USAGE_STAGING) {
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
    : device_(device), device_ref_(device, true), texture_(std::move(texture)), view_(std::move(view)),
      storage_(
          texture_, view_, context, format,
          pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ||
              (usage & D3DUSAGE_DYNAMIC) != 0,
          (usage & D3DUSAGE_DYNAMIC) != 0
      ),
      format_(format), width_(0), height_(0), depth_(0), pool_(pool), usage_(usage), levels_count_(levels) {
  if (texture_) {
    D3D11_TEXTURE3D_DESC desc{};
    texture_->GetDesc(&desc);
    width_ = desc.Width;
    height_ = desc.Height;
    depth_ = desc.Depth;
  }
  if (pool_ == D3DPOOL_MANAGED)
    resources_.attach(device, this);
  register_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DVolumeTexture9 *>(this)), this);
}

MTLD3D9VolumeTexture::MTLD3D9VolumeTexture(
    IDirect3DDevice9 *device, D3DFORMAT format, D3DPOOL pool, UINT width, UINT height, UINT depth, UINT levels,
    DWORD usage
)
    : device_(device), device_ref_(device, true),
      storage_(format, width, height, depth, levels, (usage & D3DUSAGE_DYNAMIC) != 0), format_(format), width_(width),
      height_(height), depth_(depth), pool_(pool), usage_(usage), levels_count_(levels) {
  register_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DVolumeTexture9 *>(this)), this);
}

MTLD3D9VolumeTexture::~MTLD3D9VolumeTexture() {
  unregister_base_texture(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DVolumeTexture9 *>(this)));
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
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::SetPriority(DWORD priority) {
  const DWORD previous = priority_;
  if (pool_ == D3DPOOL_MANAGED)
    priority_ = priority;
  return previous;
}
DWORD STDMETHODCALLTYPE MTLD3D9VolumeTexture::GetPriority() { return priority_; }
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
  *desc = {};
  desc->Format = format_;
  desc->Type = D3DRTYPE_VOLUME;
  desc->Usage = usage_;
  desc->Pool = pool_;
  desc->Width = std::max(1u, width_ >> level);
  desc->Height = std::max(1u, height_ >> level);
  desc->Depth = std::max(1u, depth_ >> level);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetVolumeLevel(UINT level, IDirect3DVolume9 **volume_level) {
  if (!volume_level || level >= levels_count_)
    return D3DERR_INVALIDCALL;
  if (volume_levels_.size() < levels_count_)
    volume_levels_.resize(levels_count_);
  if (!volume_levels_[level])
    volume_levels_[level] = std::make_unique<MTLD3D9Volume>(this, level);
  *volume_level = volume_levels_[level].get();
  (*volume_level)->AddRef();
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
    : texture_(parent), level_(level) {}

ULONG STDMETHODCALLTYPE
MTLD3D9Volume::AddRef() {
  return texture_ ? texture_->AddRef() : Object9::AddRef();
}

ULONG STDMETHODCALLTYPE
MTLD3D9Volume::Release() {
  return texture_ ? texture_->Release() : Object9::Release();
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
