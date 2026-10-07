#pragma once

#include "airconv_public.h"
#include "com/com_guid.hpp"
#include "d3d11_device.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_input_layout.hpp"
#include "sha1/sha1_util.hpp"
#include "log/log.hpp"
#include <variant>

struct MTL_COMPILED_SHADER {
  /**
  NOTE: it's not retained by design
  */
  WMT::Function Function;
};

namespace dxmt {
class Shader;
};
typedef dxmt::Shader *ManagedShader;

DEFINE_COM_INTERFACE("e95ba1c7-e43f-49c3-a907-4ac669c9fb42", IMTLD3D11Shader)
    : public IUnknown {
  virtual ManagedShader GetManagedShader() = 0;
  // the context's hold on what is bound: without the application's count, which holds the device
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

namespace dxmt {

class NullD3D10Shader {
public:
  NullD3D10Shader(IUnknown *discard) {}
};

template <typename Interface, typename D3D10Interface = NullD3D10Shader>
class TShaderBase : public MTLD3D11DeviceChild<Interface, IMTLD3D11Shader> {
private:
  ManagedShader shader;
  D3D10Interface d3d10;

public:
  TShaderBase(MTLD3D11Device *device, ManagedShader shader)
      : MTLD3D11DeviceChild<Interface, IMTLD3D11Shader>(device),
        shader(shader),
        d3d10(static_cast<Interface *>(this)) {}

  ~TShaderBase() {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(Interface)) {
      *ppvObject = ref_and_cast<Interface>(this);
      return S_OK;
    }

    if constexpr (!std::is_same<D3D10Interface, NullD3D10Shader>::value) {
      if (riid == __uuidof(ID3D10DeviceChild)
          || riid == __uuidof(typename D3D10Interface::ImplementedInterface)) {
        *ppvObject = ref_and_cast<D3D10Interface>(&d3d10);
        return S_OK;
      }
    }

    if (riid == __uuidof(IMTLD3D11Shader)) {
      *ppvObject = ref_and_cast<IMTLD3D11Shader>(this);
      return S_OK;
    }

    // FIXME: should it really be here?
    if (riid == __uuidof(IMTLD3D11StreamOutputLayout)) {
      return E_NOINTERFACE;
    }

    if (logQueryInterfaceError(__uuidof(Interface), riid)) {
      WARN("D3D11Shader: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  ManagedShader GetManagedShader() { return shader; }
};

struct ShaderVariantVertex {
  using this_type = ShaderVariantVertex;
  ManagedInputLayout input_layout_handle;
  uint32_t gs_passthrough;
  bool rasterization_disabled;
  bool operator==(const this_type &rhs) const {
    return input_layout_handle == rhs.input_layout_handle &&
           gs_passthrough == rhs.gs_passthrough &&
           rasterization_disabled == rhs.rasterization_disabled;
  }
};

struct ShaderVariantPixel {
  using this_type = ShaderVariantPixel;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  bool operator==(const this_type &rhs) const {
    return sample_mask == rhs.sample_mask &&
           dual_source_blending == rhs.dual_source_blending &&
           disable_depth_output == rhs.disable_depth_output &&
           unorm_output_reg_mask == rhs.unorm_output_reg_mask;
  }
};

struct ShaderVariantTessellationVertexHull {
  using this_type = ShaderVariantTessellationVertexHull;
  ManagedInputLayout input_layout_handle;
  ManagedShader vertex_shader_handle;
  SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t max_potential_tess_factor;
  uint32_t mesh_vertex_size;
  // the geometry shader that runs after the domain shader, in its mesh stage (null for none), and the most
  // threadgroups a mesh grid may have (0 while not known)
  ManagedShader geometry_shader_handle;
  uint32_t max_mesh_threadgroups;
  bool operator==(const this_type &rhs) const = default;
};

struct ShaderVariantTessellationDomain {
  using this_type = ShaderVariantTessellationDomain;
  ManagedShader hull_shader_handle;
  uint32_t max_potential_tess_factor;
  uint32_t mesh_vertex_size;
  bool rasterization_disabled;
  ManagedShader geometry_shader_handle;
  uint32_t max_mesh_threadgroups;
  bool operator==(const this_type &rhs) const = default;
};

// what stream output adds to a geometry pipeline's two stages: its layout (null for none) and whether this is the
// pass that only counts. and what a pipeline without a geometry shader needs: the input primitive
// (D3D10_SB_PRIMITIVE), and whether its mesh stage passes the primitives on to the rasterizer
struct ShaderVariantStreamOutput {
  using this_type = ShaderVariantStreamOutput;
  IMTLD3D11StreamOutputLayout *layout;
  bool counting;
  uint32_t input_primitive;
  bool pass_through;
  bool operator==(const this_type &rhs) const = default;
};

struct ShaderVariantGeometryVertex {
  using this_type = ShaderVariantGeometryVertex;
  ManagedInputLayout input_layout_handle;
  ManagedShader geometry_shader_handle; // null for stream output alone
  SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  bool strip_topology;
  ShaderVariantStreamOutput stream_output;
  bool operator==(const this_type &rhs) const = default;
};

// a variant of the geometry shader, or of the vertex shader itself for stream output alone
struct ShaderVariantGeometry {
  using this_type = ShaderVariantGeometry;
  ManagedShader vertex_shader_handle;
  bool strip_topology;
  ShaderVariantStreamOutput stream_output;
  bool operator==(const this_type &rhs) const = default;
};

struct ShaderVariantDefault {
  using this_type = ShaderVariantDefault;
  bool operator==(const this_type &rhs) const { return true; }
};

using ShaderVariant =
    std::variant<ShaderVariantDefault, ShaderVariantVertex, ShaderVariantPixel,
                 ShaderVariantTessellationVertexHull,
                 ShaderVariantTessellationDomain,
                 ShaderVariantGeometryVertex, ShaderVariantGeometry>;

class ThreadpoolWork {
public:
  virtual ~ThreadpoolWork() {}
  virtual ThreadpoolWork *RunThreadpoolWork() = 0;
  virtual bool GetIsDone() = 0;
  virtual void SetIsDone(bool state) = 0;
};

class CompiledShader : public ThreadpoolWork {
public:
  virtual ~CompiledShader() {};
  /**
  return false if it's not ready
   */
  virtual bool GetShader(MTL_COMPILED_SHADER *pShaderData) = 0;
};

class Shader {
public:
  virtual ~Shader() {};
  /* FIXME: exposed implementation detail */
  virtual sm50_shader_t handle() = 0;
  /* FIXME: exposed implementation detail */
  virtual MTL_SHADER_REFLECTION &reflection() = 0;
  virtual MTL_SM50_SHADER_ARGUMENT *constant_buffers_info() = 0;
  virtual MTL_SM50_SHADER_ARGUMENT *arguments_info() = 0;
  virtual CompiledShader *get_shader(ShaderVariant variant) = 0;
  virtual const Sha1Digest& sha1() = 0;
  virtual void dump() = 0;

  virtual WMT::Reference<WMT::DispatchData> find_cached_variant(Sha1Digest &key) = 0;
  virtual void update_cached_variant(Sha1Digest &key, WMT::DispatchData data) = 0;
};

template <typename Variant>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *, ManagedShader, Variant);

} // namespace dxmt

namespace std {
template <> struct hash<dxmt::ShaderVariant> {
  size_t operator()(const dxmt::ShaderVariant &v) const noexcept {
    return v.index(); // FIXME:
  };
};
} // namespace std
