// the texture stage a Direct3D 9 draw samples through: the textures the device makes, the one stage 0 has set, the
// sampler that stage samples with and the operation it performs (the texture objects are d3d9_texture.cpp's, and the
// draw that binds them d3d9_device_draw.cpp's).
#include "d3d9_device.hpp"
#include <algorithm>
#include <vector>

namespace mullion::d3d9 {

// a texture a draw samples: a D3D11 texture over the same device, whose bytes are the CPU's while its pool lets the
// CPU write them (a managed texture's are), with the levels asked for and the view a draw samples it through. a level
// of it is a surface of this device's, and locking one through that surface is the same lock as through the texture.
HRESULT
MTLD3D9Device::CreateTexture(
    UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture,
    HANDLE *shared_handle
) {
  if (!texture)
    return D3DERR_INVALIDCALL;
  *texture = nullptr;
  // Shared handles (including SYSTEMMEM user-memory) are D3D9Ex-only; classic CreateDevice returns E_NOTIMPL
  // (Wine device.c test_resource_sharing).
  if (shared_handle && !extended_)
    return E_NOTIMPL;
  // D3D9Ex's SYSTEMMEM shared handle names caller-owned pixels, not a GPU resource. Other pools' shared handles are
  // not a representation this build has.
  const bool user_memory = shared_handle && *shared_handle && pool == D3DPOOL_SYSTEMMEM;
  if (shared_handle && !user_memory)
    return D3DERR_INVALIDCALL;
  if (user_memory && levels != 1)
    return D3DERR_INVALIDCALL;
  const DXGI_FORMAT dxgi_color = to_dxgi_format(format);
  const DXGI_FORMAT dxgi_depth = to_dxgi_depth_format(format);
  const bool depth_format = dxgi_depth != DXGI_FORMAT_UNKNOWN;
  // A depth format is a depth texture whether or not D3DUSAGE_DEPTHSTENCIL is named (Wine test_resource_access
  // FORMAT_DEPTH with usage 0). Colour + DEPTHSTENCIL is refused.
  if ((usage & D3DUSAGE_DEPTHSTENCIL) && !depth_format)
    return D3DERR_INVALIDCALL;
  if (depth_format && (usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  // Depth textures are default-pool only; DYNAMIC|DEPTHSTENCIL is refused (Wine test_resource_access).
  if (depth_format && pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  if ((usage & D3DUSAGE_DEPTHSTENCIL) && (usage & D3DUSAGE_DYNAMIC))
    return D3DERR_INVALIDCALL;
  // YUY2/UYVY scratch textures are CPU-shadowed with an R8G8 staging stand-in (Wine test_surface_blocks).
  const bool cpu_only_yuv = (format == D3DFMT_YUY2 || format == D3DFMT_UYVY) && pool == D3DPOOL_SCRATCH;
  DXGI_FORMAT dxgi_format = depth_format ? dxgi_depth : dxgi_color;
  if (cpu_only_yuv)
    dxgi_format = DXGI_FORMAT_R8G8_UNORM;
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !width || !height || width > caps_.MaxTextureWidth ||
      height > caps_.MaxTextureHeight) {
    ERR_ONCE(
        "D3D9: no texture of this format (", format, ") or this size (", width, "x", height, "): the device makes up to ",
        caps_.MaxTextureWidth, "x", caps_.MaxTextureHeight, "."
    );
    return D3DERR_INVALIDCALL;
  }
  // a compressed texture is stored as four-by-four blocks, so D3D9 makes only ones whose size is whole blocks
  if (block_size(format) && ((width & 3) || (height & 3))) {
    ERR_ONCE(
        "D3D9: a compressed texture is made of whole four-by-four blocks, so its size is a multiple of four; ", width,
        "x", height, " is not."
    );
    return D3DERR_INVALIDCALL;
  }
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH &&
      !user_memory) {
    ERR_ONCE("D3D9: texture pool ", (unsigned)pool, " is not made.");
    return D3DERR_INVALIDCALL;
  }
  // D3D9Ex has no managed pool at all (Wine d3d9ex.c test_resource_access tests 14 to 16; d3d9 texture.c
  // d3d9_texture_init refuses D3DPOOL_MANAGED on an extended device)
  if (extended_ && pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  // a default-pool resource belongs to the device the frames failed on, which a Reset replaces: one made while the
  // device is lost would be a resource of the device that is gone (IDirect3DDevice9::Reset). A managed texture is the
  // CPU's and is made as it always is
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  const bool cpu_pool = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH || user_memory;
  // A render-target texture is one a frame is drawn into and later samples: it is a default-pool resource with a render
  // target view of each level. A dynamic texture is one the CPU streams into, locked with DISCARD or NOOVERWRITE: it is
  // a default-pool resource whose levels are mapped for writing. WRITEONLY is a buffer usage, not a texture's (Wine
  // device.c test_lockrect_invalid refuses CreateTexture(..., D3DUSAGE_WRITEONLY, ...)).
  // DEPTHSTENCIL marks a depth texture (Wine test_resource_access).
  if (usage & D3DUSAGE_WRITEONLY)
    return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP)) {
    ERR_ONCE(
        "D3D9: texture usage 0x", std::hex, usage, " is not honoured yet."
    );
    return D3DERR_INVALIDCALL;
  }
  if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) && pool != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: a used texture (render target, depth-stencil or dynamic) is a default-pool texture; pool ",
             (unsigned)pool, " is refused.");
    return D3DERR_INVALIDCALL;
  }
  if ((usage & D3DUSAGE_RENDERTARGET) && (usage & D3DUSAGE_DYNAMIC)) {
    ERR_ONCE("D3D9: a texture is a render target or dynamic, not both.");
    return D3DERR_INVALIDCALL;
  }
  // D3DUSAGE_AUTOGENMIPMAP stays only out of D3DPOOL_SYSTEMMEM (Wine texture.c d3d9_texture_init), and holds a
  // texture of a single level: the chain is generated from that one when the game asks, so level 1 and beyond are
  // refused to lock and the top level mapping is what a game reads (Wine device.c test_device_create_texture
  // creates one in MANAGED and reads GetLevelCount == 1)
  if ((usage & D3DUSAGE_AUTOGENMIPMAP) && pool == D3DPOOL_SYSTEMMEM && !user_memory)
    return D3DERR_INVALIDCALL;
  // and an explicit chain is refused: the texture is the one level however many the game names (Wine device.c: a
  // level count of 2 and of 6 with the flag are D3DERR_INVALIDCALL, none named and one of 1 are made)
  if ((usage & D3DUSAGE_AUTOGENMIPMAP) && levels > 1)
    return D3DERR_INVALIDCALL;

  // the levels a D3D9 texture is asked for: the ones it names, or (none named) the whole chain down to one texel
  UINT chain = 1;
  for (UINT size = std::max(width, height); size > 1; size >>= 1)
    chain++;
  if (levels > chain) {
    ERR_ONCE("D3D9: a texture asked for ", levels, " levels; this size has ", chain, " (the whole chain down to one "
                                                                                        "texel).");
    return D3DERR_INVALIDCALL;
  }
  UINT level_count = levels ? levels : chain;
  // a system-memory texture takes any chain the game names, the whole one and a part of it (Wine device.c
  // test_systemmem: a single level of a 128 texel texture is made, tests/d3d9/d3d9_texture.cpp,
  // tests/d3d9/d3d9_transfer.cpp)
  if (usage & D3DUSAGE_AUTOGENMIPMAP)
    level_count = 1;

  // a depth texture is made typeless so that the one resource is both the depth-stencil view a draw tests against and
  // the shader resource a shadow map samples (Wine visual.c shadow_test)
  const DXGI_FORMAT resource_format = depth_format ? to_typeless_depth_format(dxgi_format) : dxgi_format;
  const DXGI_FORMAT read_format = depth_format ? to_depth_read_format(dxgi_format) : dxgi_format;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = level_count;
  desc.ArraySize = 1;
  desc.Format = resource_format;
  desc.SampleDesc.Count = 1;
  // A dynamic texture's levels are the CPU's and are uploaded on unlock: D3D11 maps a texture with WRITE_DISCARD only,
  // which would throw away the pixels a NOOVERWRITE lock leaves alone, so the texture is the device's and the bytes
  // the application writes reach it through the upload (the same rows a map would hand back)
  desc.Usage = cpu_pool ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = cpu_pool ? (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE) : 0;
  if (!cpu_pool)
    desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
  if (usage & D3DUSAGE_RENDERTARGET)
    desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
  if (depth_format)
    desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
  // A managed texture's levels below the top one are made out of it only when the game asks (GenerateMipSubLevels),
  // which D3D11 does through a render target view of the whole chain; the chain itself is made whether or not this
  // format can be generated, so the levels the game authored survive and GetLevelCount is the count it asked for. A
  // compressed format's levels are always the game's own blocks (D3D11 makes no level of a compressed texture out of
  // another). Nothing reduces the chain to one level behind the game's back.
  bool can_generate_mips = false;
  if (!cpu_pool && level_count > 1 && !(usage & D3DUSAGE_DYNAMIC)) {
    UINT support = 0;
    if (SUCCEEDED(d3d11_device_->CheckFormatSupport(dxgi_format, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
      can_generate_mips = true;
    }
  }

  ComPtr<ID3D11Texture2D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr) && can_generate_mips) {
    // the format said it can generate mips but the device refuses the render-target binding that needs: the chain is
    // still the game's, it simply cannot be generated here. The requested level count is kept.
    desc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
    can_generate_mips = false;
    create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  }
  if (FAILED(create_hr)) {
    ERR("D3D9: no texture of ", width, "x", height, " with ", level_count, " levels: 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11ShaderResourceView> view;
  if (!cpu_pool) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = read_format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MostDetailedMip = 0;
    view_desc.Texture2D.MipLevels = level_count;
    if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), &view_desc, &view))) {
      ERR_ONCE("D3D9: the texture's view is not there, so the texture is not either.");
      return D3DERR_INVALIDCALL;
    }
  }

  ComPtr<MTLD3D9Texture> result;
  result.Attach(new MTLD3D9Texture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count, can_generate_mips,
      usage, user_memory ? reinterpret_cast<void *>(*shared_handle) : nullptr,
      user_memory ? (format == D3DFMT_L8 ? width : width * 4) : 0
  ));

  if (pool == D3DPOOL_DEFAULT) {
    uint64_t bytes = (uint64_t)width * height * 4;
    for (UINT level = 1; level < level_count; level++)
      bytes += bytes / 4;
    tracked_texture_bytes_ += bytes;
  }

  *texture = result.Detach();
  return S_OK;
}

// a cube texture: six square faces as a D3D11 TEXTURECUBE (Microsoft Learn, IDirect3DDevice9::CreateCubeTexture).
// each face is an array slice, so a lock of GetCubeMapSurface(face, level) is the same lock as LockRect(face, level).
HRESULT
MTLD3D9Device::CreateCubeTexture(
    UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **texture,
    HANDLE *shared_handle
) {
  if (!texture)
    return D3DERR_INVALIDCALL;
  if (shared_handle)
    return extended_ ? D3DERR_INVALIDCALL : E_NOTIMPL;
  *texture = nullptr;
  DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  const bool cpu_only_yuv = (format == D3DFMT_YUY2 || format == D3DFMT_UYVY) && pool == D3DPOOL_SCRATCH;
  if (cpu_only_yuv)
    dxgi_format = DXGI_FORMAT_R8G8_UNORM;
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !edge || edge > caps_.MaxTextureWidth) {
    ERR_ONCE(
        "D3D9: no cube texture of this format (", format, ") or this edge (", edge, "): the device makes up to ",
        caps_.MaxTextureWidth, "."
    );
    return D3DERR_INVALIDCALL;
  }
  if (block_size(format) && (edge & 3)) {
    ERR_ONCE("D3D9: a compressed cube texture's edge is a multiple of four; ", edge, " is not.");
    return D3DERR_INVALIDCALL;
  }
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) {
    ERR_ONCE("D3D9: cube texture pool ", (unsigned)pool, " is not made.");
    return D3DERR_INVALIDCALL;
  }
  // D3D9Ex has no managed pool (Wine d3d9ex.c test_resource_access test 14, CUBE)
  if (extended_ && pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  if (usage & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP)) {
    ERR_ONCE("D3D9: cube texture usage 0x", std::hex, usage, " is not honoured yet.");
    return D3DERR_INVALIDCALL;
  }
  // AUTOGENMIPMAP is allowed on MANAGED (Wine device.c test_cube_texture_mipmap_gen); RT/DYNAMIC need DEFAULT.
  if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) && pool != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: a render-target or dynamic cube texture is a default-pool texture; pool ", (unsigned)pool,
             " is refused.");
    return D3DERR_INVALIDCALL;
  }
  if ((usage & D3DUSAGE_RENDERTARGET) && (usage & D3DUSAGE_DYNAMIC)) {
    ERR_ONCE("D3D9: a cube texture is a render target or dynamic, not both.");
    return D3DERR_INVALIDCALL;
  }

  UINT chain = 1;
  for (UINT size = edge; size > 1; size >>= 1)
    chain++;
  if (levels > chain) {
    ERR_ONCE("D3D9: a cube texture asked for ", levels, " levels; this edge has ", chain, ".");
    return D3DERR_INVALIDCALL;
  }
  UINT level_count = levels ? levels : chain;

  const bool cpu_pool = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = edge;
  desc.Height = edge;
  desc.MipLevels = level_count;
  desc.ArraySize = 6;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = cpu_pool ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
  desc.BindFlags = cpu_pool ? 0u : D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = cpu_pool ? (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE) : 0u;
  desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  if (usage & D3DUSAGE_RENDERTARGET)
    desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
  bool can_generate_mips = false;
  if (!cpu_pool && level_count > 1 && !(usage & D3DUSAGE_DYNAMIC) &&
      ((usage & D3DUSAGE_AUTOGENMIPMAP) || !(usage & D3DUSAGE_RENDERTARGET))) {
    UINT support = 0;
    if (SUCCEEDED(d3d11_device_->CheckFormatSupport(dxgi_format, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
      can_generate_mips = true;
    }
  }
  if ((usage & D3DUSAGE_AUTOGENMIPMAP) && !cpu_pool) {
    desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    can_generate_mips = true;
  }

  ComPtr<ID3D11Texture2D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr) && can_generate_mips) {
    desc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
    can_generate_mips = false;
    create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  }
  if (FAILED(create_hr)) {
    ERR("D3D9: no cube texture of edge ", edge, " with ", level_count, " levels: 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11ShaderResourceView> view;
  if (!cpu_pool) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = dxgi_format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    view_desc.TextureCube.MostDetailedMip = 0;
    view_desc.TextureCube.MipLevels = level_count;
    if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), &view_desc, &view))) {
      ERR_ONCE("D3D9: the cube texture's view is not there, so the texture is not either.");
      return D3DERR_INVALIDCALL;
    }
  }

  ComPtr<MTLD3D9CubeTexture> result;
  result.Attach(new MTLD3D9CubeTexture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count,
      can_generate_mips, usage
  ));
  *texture = result.Detach();
  return S_OK;
}

// a volume texture: a D3D11 TEXTURE3D the CPU authors with LockBox and a draw samples through dcl_volume / tex3D
// (Microsoft Learn, IDirect3DDevice9::CreateVolumeTexture)
HRESULT
MTLD3D9Device::CreateVolumeTexture(
    UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DVolumeTexture9 **texture, HANDLE *shared_handle
) {
  if (!texture)
    return D3DERR_INVALIDCALL;
  if (shared_handle)
    return extended_ ? D3DERR_INVALIDCALL : E_NOTIMPL;
  *texture = nullptr;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  const bool cpu_pool = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
  if (!width || !height || !depth || width > caps_.MaxVolumeExtent || height > caps_.MaxVolumeExtent ||
      depth > caps_.MaxVolumeExtent) {
    ERR_ONCE(
        "D3D9: no volume texture of this size (", width, "x", height, "x", depth, "): the device makes up to ",
        caps_.MaxVolumeExtent, "."
    );
    return D3DERR_INVALIDCALL;
  }
  // GPU pools need a DXGI mapping; SCRATCH/SYSTEMMEM may be CPU-only (Wine test_volume_blocks YUY2).
  if (dxgi_format == DXGI_FORMAT_UNKNOWN && !cpu_pool) {
    ERR_ONCE("D3D9: no volume texture of format ", (unsigned)format, ".");
    return D3DERR_INVALIDCALL;
  }
  if (dxgi_format == DXGI_FORMAT_UNKNOWN && !format_row_pitch(format, width) && format != D3DFMT_YUY2 &&
      format != D3DFMT_UYVY) {
    ERR_ONCE("D3D9: volume format ", (unsigned)format, " has no CPU layout in this build.");
    return D3DERR_INVALIDCALL;
  }
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) {
    ERR_ONCE("D3D9: volume texture pool ", (unsigned)pool, " is not made.");
    return D3DERR_INVALIDCALL;
  }
  // D3D9Ex has no managed pool (Wine d3d9ex.c test_resource_access test 14, VOLUME)
  if (extended_ && pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  if (usage & ~(DWORD)D3DUSAGE_DYNAMIC) {
    ERR_ONCE("D3D9: volume texture usage 0x", std::hex, usage, " is not honoured yet (dynamic is).");
    return D3DERR_INVALIDCALL;
  }
  // DYNAMIC is DEFAULT or SYSTEMMEM only; MANAGED/SCRATCH + DYNAMIC is INVALIDCALL (Wine device.c volume pool table).
  if ((usage & D3DUSAGE_DYNAMIC) && pool != D3DPOOL_DEFAULT && pool != D3DPOOL_SYSTEMMEM) {
    ERR_ONCE("D3D9: a dynamic volume texture is default or system-memory; pool ", (unsigned)pool, " is refused.");
    return D3DERR_INVALIDCALL;
  }
  // DXT volumes enforce whole-block dimensions (Wine create_size_checked); YUY2 does not.
  if (block_size(format) && ((width & 3) || (height & 3))) {
    ERR_ONCE("D3D9: a compressed volume texture's width and height are multiples of four; ", width, "x", height,
             " is not.");
    return D3DERR_INVALIDCALL;
  }
  if (!format_row_pitch(format, width) && dxgi_format == DXGI_FORMAT_UNKNOWN) {
    ERR_ONCE("D3D9: volume format ", (unsigned)format, " has no CPU layout in this build.");
    return D3DERR_INVALIDCALL;
  }

  UINT longest = std::max(width, std::max(height, depth));
  UINT chain = 1;
  for (UINT size = longest; size > 1; size >>= 1)
    chain++;
  if (levels > chain) {
    ERR_ONCE("D3D9: a volume texture asked for ", levels, " levels; this size has ", chain, ".");
    return D3DERR_INVALIDCALL;
  }
  UINT level_count = levels ? levels : chain;

  // YUY2/UYVY and other non-DXGI volume formats: Wine only creates them without driver support in SCRATCH
  // (test_volume_blocks need_driver_support). SYSTEMMEM/DEFAULT/MANAGED require CheckDeviceFormat support.
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || format == D3DFMT_YUY2 || format == D3DFMT_UYVY) {
    if (pool != D3DPOOL_SCRATCH)
      return D3DERR_INVALIDCALL;
    ComPtr<MTLD3D9VolumeTexture> result;
    result.Attach(new MTLD3D9VolumeTexture(
        static_cast<IDirect3DDevice9 *>(this), format, pool, width, height, depth, level_count, usage
    ));
    *texture = result.Detach();
    return S_OK;
  }

  D3D11_TEXTURE3D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Depth = depth;
  desc.MipLevels = level_count;
  desc.Format = dxgi_format;
  desc.Usage = cpu_pool ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
  desc.BindFlags = cpu_pool ? 0u : D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = cpu_pool ? (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE) : 0u;

  ComPtr<ID3D11Texture3D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture3D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr) && cpu_pool) {
    ComPtr<MTLD3D9VolumeTexture> result;
    result.Attach(new MTLD3D9VolumeTexture(
        static_cast<IDirect3DDevice9 *>(this), format, pool, width, height, depth, level_count, usage
    ));
    *texture = result.Detach();
    return S_OK;
  }
  if (FAILED(create_hr)) {
    ERR("D3D9: no volume texture of ", width, "x", height, "x", depth, ": 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11ShaderResourceView> view;
  if (!cpu_pool) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = dxgi_format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    view_desc.Texture3D.MostDetailedMip = 0;
    view_desc.Texture3D.MipLevels = level_count;
    if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), &view_desc, &view))) {
      ERR_ONCE("D3D9: the volume texture's view is not there, so the texture is not either.");
      return D3DERR_INVALIDCALL;
    }
  }

  ComPtr<MTLD3D9VolumeTexture> result;
  result.Attach(new MTLD3D9VolumeTexture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count, usage
  ));
  *texture = result.Detach();
  return S_OK;
}

namespace {

// the public texture stage an API call names, as the device's compact slot: pixel stages 0..15 are themselves, and
// D3DVERTEXTEXTURESAMPLER0..3 (257..260) are slots 16..19. D3DDMAPSAMPLER (256) is a stage this build does not
// implement: displacement mapping is not advertised, so it is refused like any other stage the device does not have
bool texture_stage_slot(DWORD stage, UINT &slot) {
  if (stage < MTLD3D9Device::kTextureStages) {
    slot = stage;
    return true;
  }
  if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) {
    slot = MTLD3D9Device::kTextureStages + (stage - D3DVERTEXTEXTURESAMPLER0);
    return true;
  }
  return false;
}

} // namespace

// the textures a draw samples: the sixteen pixel stages, and the four vertex samplers the API names 257..260, each
// keeping the texture SetTexture set for it. The public id is what a state block records, so the two namespaces
// capture and restore independently
HRESULT
MTLD3D9Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) {
  UINT slot = 0;
  if (!texture_stage_slot(stage, slot)) {
    ERR_ONCE("D3D9: stage ", stage, " is not a pixel stage (0..15) or D3DVERTEXTEXTURESAMPLER0..3 (257..260).");
    return D3DERR_INVALIDCALL;
  }
  if (!texture) {
    if (texture_sources_[slot]) {
      texture_sources_[slot]->release_private_ref();
      texture_sources_[slot] = nullptr;
    }
    textures_[slot] = nullptr;
    if (recording_)
      record(StateKey{StateKey::Texture, stage, 0});
    return S_OK;
  }

  // look up without calling through the public vtbl (Wine device.c test_get_set_texture replaces lpVtbl with
  // 0xdeadbeef and still expects SetTexture to succeed)
  IMTLD3D9Texture *ours = lookup_base_texture(texture);
  if (!ours)
    return D3DERR_INVALIDCALL;
  if (texture_sources_[slot])
    texture_sources_[slot]->release_private_ref();
  ours->add_private_ref();
  textures_[slot] = texture;
  texture_sources_[slot] = ours;
  if (recording_)
    record(StateKey{StateKey::Texture, stage, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetTexture(DWORD stage, IDirect3DBaseTexture9 **texture) {
  UINT slot = 0;
  if (!texture || !texture_stage_slot(stage, slot))
    return D3DERR_INVALIDCALL;
  *texture = textures_[slot];
  if (*texture)
    (*texture)->AddRef();
  return S_OK;
}

// the texture a stage with none samples: D3D9's default texture is one opaque white texel, which a program that samples
// an empty stage reads the way it reads any other texture. it is made once and bound as a draw's input when the pixel
// shader samples and SetTexture has set nothing
HRESULT
MTLD3D9Device::ensure_default_texture() {
  if (default_texture_view_)
    return S_OK;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 1;
  desc.Height = 1;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const DWORD white = 0xFFFFFFFF;
  D3D11_SUBRESOURCE_DATA data{};
  data.pSysMem = &white;
  data.SysMemPitch = sizeof(white);
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, &data, &default_texture_)) ||
      FAILED(d3d11_device_->CreateShaderResourceView(default_texture_.Get(), nullptr, &default_texture_view_))) {
    default_texture_.Reset();
    default_texture_view_.Reset();
    ERR_ONCE("D3D9: the default texture (the white texel a stage with no texture samples) is not made.");
    return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

// a sampler is a stage's: D3D9's stage # samples through sampler #. Stages 0..15 are the pixel samplers and 16..19
// the vertex samplers, which are separate namespaces even though both number their shader registers from 0
HRESULT
MTLD3D9Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  UINT slot = 0;
  if (!texture_stage_slot(sampler, slot)) {
    ERR_ONCE("D3D9: sampler ", sampler, " is not a pixel sampler (0..15) or D3DVERTEXTEXTURESAMPLER0..3.");
    return D3DERR_INVALIDCALL;
  }
  const HRESULT hr = sampler_states_[slot].set(type, value);
  if (SUCCEEDED(hr) && recording_)
    record(StateKey{StateKey::Sampler, sampler, (UINT)type});
  return hr;
}

HRESULT
MTLD3D9Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) {
  UINT slot = 0;
  if (!value || !texture_stage_slot(sampler, slot))
    return D3DERR_INVALIDCALL;
  return sampler_states_[slot].get(type, value);
}

// the fixed-function blender has two stages (the caps' MaxTextureBlendStages): stage 0 and the extra the extended
// pixel shader blends after it
HRESULT
MTLD3D9Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage >= kMaxBlendStages) {
    ERR_ONCE("D3D9: this device's fixed-function blender has ", kMaxBlendStages,
             " stages (MaxTextureBlendStages); stage ", stage, "'s operations are refused.");
    return D3DERR_INVALIDCALL;
  }
  const HRESULT hr = texture_stages_[stage].set(type, value);
  if (SUCCEEDED(hr) && recording_)
    record(StateKey{StateKey::TextureStage, stage, (UINT)type});
  return hr;
}

HRESULT
MTLD3D9Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  if (stage >= kMaxBlendStages || !value)
    return D3DERR_INVALIDCALL;
  return texture_stages_[stage].get(type, value);
}

} // namespace mullion::d3d9
