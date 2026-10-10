#include "DXBCParser/BlobContainer.h"
#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_pipeline.hpp"
#include "airconv_ray.h"
#include "com/com_pointer.hpp"
#include "util_md5.hpp"
#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

// DXR state objects (DXR spec, "State objects"). a ray tracing shader is a visible function of Metal at a table slot
// the device gave it, and a shader identifier holds its shaders' slots (airconv_ray.h): a pipeline is the kernel
// that dispatches rays, linked with the functions of every export, and a function table with each at its slot

namespace dxmt {

namespace {

// the name a library's function exports under: "name" of the C++ mangling "\x01?name@@..."
std::wstring
ExportName(std::string_view function) {
  if (function.starts_with("\x01?"))
    function = function.substr(2, function.find("@@") - 2);
  return std::wstring(function.begin(), function.end());
}

struct RayLibrary {
  sm50_shader_t shader = nullptr;
  ~RayLibrary() {
    if (shader)
      SM50Destroy(shader);
  }
};

// a function of a library that is a ray tracing shader
struct RayShader {
  std::shared_ptr<RayLibrary> library;
  std::string function;
  SM50_RAY_SHADER_INFO info;
  // the collection it came from uncompiled, which is a scope of its own
  const void *collection = nullptr;
};

// a subobject a DXIL library defines, of the kinds a state object takes from one: their numbers are D3D12's
// subobject types
struct LibrarySubobject {
  D3D12_STATE_SUBOBJECT_TYPE type;
  std::wstring name;
  // a root signature, serialized
  std::span<const uint8_t> bytes;
  // an association's subobject, then its exports; a hit group's any hit, closest hit and intersection shaders
  std::vector<std::wstring> names;
  // a pipeline config's recursion depth and flags; a state object config's flags
  uint32_t values[2];
};

// the subobjects in a library's runtime data, the container's RDAT part: a version and a count of parts, the parts'
// offsets, and each part its type, its size and its data (DXC's DxilRuntimeReflection.h). a record of the subobject
// table is DWORDs: its kind, its name, and what RDAT_SubobjectTypes.inl gives the kind
std::vector<LibrarySubobject>
ReadSubobjects(const D3D12_SHADER_BYTECODE &library) {
  std::vector<LibrarySubobject> out;
  microsoft::CDXBCParser container;
  if (FAILED(container.ReadDXBC(library.pShaderBytecode, library.BytecodeLength)))
    return out;
  auto blob = container.FindNextMatchingBlob((microsoft::DXBCFourCC)MAKEFOURCC('R', 'D', 'A', 'T'));
  if (blob == DXBC_BLOB_NOT_FOUND)
    return out;
  std::span data((const uint8_t *)container.GetBlob(blob), container.GetBlobSize(blob));
  auto word = [](std::span<const uint8_t> from, uint64_t at) {
    uint32_t value = 0;
    if (at + sizeof(value) <= from.size())
      memcpy(&value, from.data() + at, sizeof(value));
    return value;
  };
  enum { Strings = 1, Indices = 2, RawBytes = 5, Subobjects = 6, Parts };
  std::span<const uint8_t> parts[Parts];
  for (uint32_t i = 0, count = word(data, 4); i < count; i++) {
    uint64_t at = word(data, 8 + 4 * i), type = word(data, at), size = word(data, at + 4);
    if (type < Parts && at + 8 + size <= data.size())
      parts[type] = data.subspan(at + 8, size);
  }
  // a string is its offset in the string part
  auto string = [&](uint32_t offset) {
    std::wstring name;
    for (auto at = offset; at < parts[Strings].size() && parts[Strings][at]; at++)
      name.push_back(parts[Strings][at]);
    return name;
  };
  uint32_t stride = word(parts[Subobjects], 4);
  for (uint32_t i = 0, count = word(parts[Subobjects], 0); i < count; i++) {
    auto record = parts[Subobjects].subspan(std::min<size_t>(8 + (uint64_t)i * stride, parts[Subobjects].size()));
    auto field = [&](uint32_t index) { return index * 4 < stride ? word(record, index * 4) : 0; };
    LibrarySubobject subobject{(D3D12_STATE_SUBOBJECT_TYPE)field(0), string(field(1))};
    switch (subobject.type) {
    case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
    case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
      // an offset in the raw bytes part and a size
      if ((uint64_t)field(2) + field(3) <= parts[RawBytes].size())
        subobject.bytes = parts[RawBytes].subspan(field(2), field(3));
      break;
    case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
      subobject.names.push_back(string(field(2)));
      // the exports are a row of the index part: its length, then the strings
      for (uint32_t n = 0, row = field(3), length = word(parts[Indices], (uint64_t)row * 4); n < length; n++)
        subobject.names.push_back(string(word(parts[Indices], ((uint64_t)row + 1 + n) * 4)));
      break;
    case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
      subobject.values[0] = field(2);
      break;
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
      subobject.values[0] = field(2);
      subobject.values[1] = subobject.type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1 ? field(3) : 0;
      break;
    case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
      // after the group's type
      for (uint32_t n = 0; n < 3; n++)
        subobject.names.push_back(string(field(3 + n)));
      break;
    default:
      break;
    }
    out.push_back(std::move(subobject));
  }
  return out;
}

// a hit group: its name, and the names of its closest hit, any hit and intersection shaders, empty for none
struct HitGroup {
  std::wstring name, imports[3];
};

// how an association of a subobject with exports was made, the weakest first ("Subobject association behavior"):
// what the state object says overrides what its libraries say, and of each, an association that names exports comes
// before one that names none, and that before a subobject no association of its scope gives exports
enum Strength { LibraryDeclared, LibraryDefault, LibraryExplicit, Declared, Default, Explicit };

enum RootKind { Global, Local, RootKinds };

struct Association {
  RootKind kind;
  ID3D12RootSignature *root;
  Strength strength;
  // the export, or none for every export of the scope
  std::wstring name;
  // the scopes of one without an export: the library whose subobjects made it, and the collection it was made in,
  // if its shaders were left for the state object that takes the collection to compile
  const RayLibrary *library;
  const void *collection;
  // the library subobject it names, while no library the state object has seen defines it: it has no root until one
  // does ("Explicit associations": the subobject need not "be visible yet")
  std::wstring subobject;
};

// a shader compiled with its root signatures
struct RayFunction {
  MTLD3D12Device *device;
  uint32_t slot;
  WMT::Reference<WMT::Function> function;

  ~RayFunction() {
    device->FreeRayFunction(slot);
  }
};

// what a shader identifier names: a ray generation, miss or callable shader, or a hit group's closest hit, any hit
// and intersection shaders, in the identifier's order
struct RayExport {
  SM50_RAY_SHADER_IDENTIFIER identifier{};
  std::shared_ptr<RayFunction> functions[3];
  uint32_t kinds[3]{}; // SM50_RAY_SHADER_KIND
};

// stack sizes are counted in calls: a shader runs under the runtime function that calls it, but the ray generation
// shader, which the kernel calls. the pipeline stack size an application sums from them is then the call depth
// the pipeline needs ("Pipeline stack")
constexpr uint64_t
StackSize(uint32_t kind) {
  return kind == SM50_RAY_SHADER_RAY_GENERATION ? 1 : 2;
}

} // namespace

class MTLD3D12StateObjectImpl : public MTLD3D12DeviceChild<MTLD3D12StateObject, ID3D12StateObjectProperties> {
  D3D12_STATE_OBJECT_TYPE type_ = D3D12_STATE_OBJECT_TYPE_COLLECTION;
  // every shader the state object has seen, by name, for the hit groups that import them
  std::unordered_map<std::wstring, RayShader> shaders_;
  std::unordered_map<std::wstring, RayExport> exports_;
  std::vector<Com<ID3D12RootSignature>> root_signatures_;
  D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline_config_{};
  uint64_t stack_size_ = 0;
  // the libraries of this state object's own description, whose functions its shaders may call
  std::vector<std::shared_ptr<RayLibrary>> libraries_;
  // of a collection whose exports may depend on what the state object that takes it defines
  // (D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS): nothing is compiled, and its hit
  // groups and associations wait for that state object
  bool deferred_ = false;
  std::vector<HitGroup> deferred_hit_groups_;
  std::vector<Association> deferred_associations_;
  // the libraries' root signatures by their subobjects' names, and the names of all their subobjects: an association
  // may name one of another scope
  std::unordered_map<std::wstring, std::pair<RootKind, ID3D12RootSignature *>> library_roots_;
  std::unordered_set<std::wstring> library_names_;

  struct Pipeline {
    WMT::Reference<WMT::ComputePipelineState> pipeline;
    WMT::Reference<WMT::VisibleFunctionTable> table;
  };
  // by call depth; commands in flight keep using the one they were recorded with
  dxmt::mutex pipeline_lock_;
  std::map<uint32_t, Pipeline> pipelines_;

  // the shaders `pExports` takes from `all` under their new names, or all of them without a list
  template <typename Value>
  static void
  Export(
      const std::unordered_map<std::wstring, Value> &all, std::span<const D3D12_EXPORT_DESC> exports,
      std::unordered_map<std::wstring, Value> &out
  ) {
    if (exports.empty())
      out.insert(all.begin(), all.end());
    for (auto &desc : exports) {
      std::wstring name = desc.Name;
      // a mangled name exports as the function it mangles
      auto found = all.find(ExportName(std::string(name.begin(), name.end())));
      if (desc.ExportToRename) {
        std::wstring source = desc.ExportToRename;
        found = all.find(ExportName(std::string(source.begin(), source.end())));
      } else {
        name = found == all.end() ? name : found->first;
      }
      if (found != all.end())
        out.insert({name, found->second});
    }
  }

  // takes the library's shaders, and gives its subobjects: all of them, or those the exports name, under their
  // new names
  HRESULT
  AddLibrary(
      const D3D12_DXIL_LIBRARY_DESC &desc, std::vector<std::pair<const RayLibrary *, LibrarySubobject>> &subobjects
  ) {
    auto library = std::make_shared<RayLibrary>();
    if (!ShaderContainerHolds(desc.DXILLibrary.pShaderBytecode, desc.DXILLibrary.BytecodeLength))
      return E_INVALIDARG;
    SM50Error error;
    if (SM50Initialize(
            desc.DXILLibrary.pShaderBytecode, desc.DXILLibrary.BytecodeLength, &library->shader, nullptr, &error
        )) {
      ERR("CreateStateObject: a DXIL library is not usable: ", SM50GetErrorMessageString(error));
      return E_INVALIDARG;
    }
    std::unordered_map<std::wstring, RayShader> shaders;
    char name[1024];
    SM50_RAY_SHADER_INFO info;
    for (uint32_t i = 0; SM50GetRayShader(library->shader, i, name, sizeof(name), &info); i++)
      shaders.insert({ExportName(name), {library, name, info}});
    Export(shaders, {desc.pExports, desc.NumExports}, shaders_);
    libraries_.push_back(library);
    if (device_->NamesPasses())
      this->name +=
          Sha1HashState::compute(desc.DXILLibrary.pShaderBytecode, desc.DXILLibrary.BytecodeLength).string() + " ";
    for (auto &subobject : ReadSubobjects(desc.DXILLibrary)) {
      if (!desc.NumExports)
        subobjects.push_back({library.get(), subobject});
      for (auto &e : std::span(desc.pExports, desc.NumExports))
        if (subobject.name == (e.ExportToRename ? e.ExportToRename : e.Name)) {
          subobjects.push_back({library.get(), subobject});
          subobjects.back().second.name = e.Name;
        }
    }
    return S_OK;
  }

  // a library has its root signatures serialized without the container that root signature creation takes
  Com<ID3D12RootSignature>
  RootSignature(std::span<const uint8_t> serialized) {
    struct {
      microsoft::DXBCHeader header;
      uint32_t offset;
      microsoft::DXBCBlobHeader blob;
    } head{};
    std::vector<uint8_t> container(sizeof(head) + serialized.size());
    head.header = {microsoft::DXBC_FOURCC_NAME, {}, {1, 0}, (uint32_t)container.size(), 1};
    head.offset = offsetof(decltype(head), blob);
    head.blob = {microsoft::DXBC_RootSignature, (uint32_t)serialized.size()};
    memcpy(container.data(), &head, sizeof(head));
    memcpy(container.data() + sizeof(head), serialized.data(), serialized.size());
    auto hash = md5::hashDxbcBinary(container.data(), container.size());
    memcpy(container.data() + offsetof(microsoft::DXBCHeader, Hash), &hash, sizeof(hash));
    Com<ID3D12RootSignature> root;
    device_->CreateRootSignature(0, container.data(), container.size(), IID_PPV_ARGS(&root));
    return root;
  }

  // `libraries`: those whose functions the shader may call
  std::shared_ptr<RayFunction>
  Compile(
      const RayShader &shader, ID3D12RootSignature *pGlobal, ID3D12RootSignature *pLocal,
      std::span<const sm50_shader_t> libraries
  ) {
    auto blob = [](ID3D12RootSignature *root, SM50_SHADER_COMPILATION_ARGUMENT_TYPE type, void *next) {
      SM50_SHADER_ROOT_SIGNATURE_DATA data{next, type};
      data.bytecode_length = static_cast<MTLD3D12RootSignature *>(root)->GetBlob(&data.bytecode);
      data.static_samplers = static_cast<MTLD3D12RootSignature *>(root)->StaticSamplersAddress;
      return data;
    };
    auto function = std::make_shared<RayFunction>(device_, device_->AllocateRayFunction());
    auto name = "dxmt_ray_shader_" + std::to_string(function->slot);

    SM50_SHADER_LIBRARIES_DATA linkable{nullptr, SM50_SHADER_LIBRARIES, libraries.data(), (uint32_t)libraries.size()};
    SM50_SHADER_RAY_SHADER_DATA ray{&linkable, SM50_SHADER_RAY_SHADER, shader.function.c_str()};
    void *arguments = &ray;
    SM50_SHADER_ROOT_SIGNATURE_DATA local, global;
    if (pLocal)
      arguments = &(local = blob(pLocal, SM50_SHADER_ROOT_SIGNATURE2, arguments));
    if (pGlobal)
      arguments = &(global = blob(pGlobal, SM50_SHADER_ROOT_SIGNATURE, arguments));
    SM50_SHADER_COMMON_DATA common{arguments, SM50_SHADER_COMMON, device_->GetMetalVersion()};
    common.flags = {};
    common.simd_width = device_->GetSIMDWidth();

    SM50ShaderBitcode bitcode;
    SM50Error error;
    if (SM50Compile(
            shader.library->shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, name.c_str(), &bitcode, &error
        )) {
      ERR("CreateStateObject: failed to compile ", shader.function, ": ", SM50GetErrorMessageString(error));
      return nullptr;
    }
    SM50_COMPILED_BITCODE compiled;
    SM50GetCompiledBitcode(bitcode, &compiled);
    WMT::Reference<WMT::Error> err;
    auto library = device_->GetMTLDevice().newLibrary(WMT::MakeDispatchData(compiled.Data, compiled.Size), err);
    if (library)
      function->function = library.newFunction(name.c_str());
    if (!function->function) {
      ERR("CreateStateObject: no Metal function for ", shader.function, ": ",
          err ? err.description().getUTF8String() : "the library has none of its name");
      return nullptr;
    }
    return function;
  }

public:
  MTLD3D12StateObjectImpl(MTLD3D12Device *pDevice) :
      MTLD3D12DeviceChild<MTLD3D12StateObject, ID3D12StateObjectProperties>(pDevice) {}

  // `pParent` is the state object an addition grows from (AddToStateObject): it keeps its exports and their
  // identifiers
  HRESULT
  Initialize(const D3D12_STATE_OBJECT_DESC *pDesc, MTLD3D12StateObjectImpl *pParent) {
    auto start = device_->NamesPasses() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    type_ = pDesc->Type;
    libraries_.clear();
    if (pParent) {
      if (device_->NamesPasses())
        name = pParent->name;
      shaders_ = pParent->shaders_;
      exports_ = pParent->exports_;
      root_signatures_ = pParent->root_signatures_;
      library_roots_ = pParent->library_roots_;
      library_names_ = pParent->library_names_;
      pipeline_config_ = pParent->pipeline_config_;
    }
    std::span subobjects(pDesc->pSubobjects, pDesc->NumSubobjects);

    using Subobject = const D3D12_STATE_SUBOBJECT *;
    auto kind = [](D3D12_STATE_SUBOBJECT_TYPE type) {
      return type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE  ? (int)Global
             : type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE ? (int)Local
                                                                       : -1;
    };
    auto root_of = [](Subobject subobject) {
      return ((const D3D12_GLOBAL_ROOT_SIGNATURE *)subobject->pDesc)->pGlobalRootSignature;
    };
    std::vector<Association> associations;
    // the state object's root signatures that an association of its own gives exports
    std::unordered_set<Subobject> associated;
    std::vector<HitGroup> hit_groups;
    std::vector<std::pair<const RayLibrary *, LibrarySubobject>> library_subobjects;
    uint32_t flags = 0;
    for (auto &subobject : subobjects) {
      switch (subobject.Type) {
      case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        flags |= ((const D3D12_STATE_OBJECT_CONFIG *)subobject.pDesc)->Flags;
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        pipeline_config_ = {((const D3D12_RAYTRACING_PIPELINE_CONFIG *)subobject.pDesc)->MaxTraceRecursionDepth};
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
        pipeline_config_ = *(const D3D12_RAYTRACING_PIPELINE_CONFIG1 *)subobject.pDesc;
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
      case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        root_signatures_.push_back(root_of(&subobject));
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        if (HRESULT hr = AddLibrary(*(const D3D12_DXIL_LIBRARY_DESC *)subobject.pDesc, library_subobjects); FAILED(hr))
          return hr;
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
        auto &desc = *(const D3D12_EXISTING_COLLECTION_DESC *)subobject.pDesc;
        auto collection = static_cast<MTLD3D12StateObjectImpl *>(desc.pExistingCollection);
        if (device_->NamesPasses())
          name += collection->name;
        Export(collection->shaders_, {desc.pExports, desc.NumExports}, shaders_);
        Export(collection->exports_, {desc.pExports, desc.NumExports}, exports_);
        root_signatures_.insert(
            root_signatures_.end(), collection->root_signatures_.begin(), collection->root_signatures_.end()
        );
        // what it left for this state object: its hit groups, as the exports take them, and its associations
        std::unordered_map<std::wstring, HitGroup> groups, taken;
        for (auto &group : collection->deferred_hit_groups_)
          groups[group.name] = group;
        Export(groups, {desc.pExports, desc.NumExports}, taken);
        for (auto &[name, group] : taken)
          hit_groups.push_back({name, {group.imports[0], group.imports[1], group.imports[2]}});
        // an association follows its export to the name it is taken under
        for (auto &association : collection->deferred_associations_) {
          if (association.name.empty() || !desc.NumExports)
            associations.push_back(association);
          for (auto &taken_as : std::span(desc.pExports, desc.NumExports))
            if (!association.name.empty() &&
                association.name == (taken_as.ExportToRename ? taken_as.ExportToRename : taken_as.Name)) {
              associations.push_back(association);
              associations.back().name = taken_as.Name;
            }
        }
        library_roots_.insert(collection->library_roots_.begin(), collection->library_roots_.end());
        library_names_.insert(collection->library_names_.begin(), collection->library_names_.end());
        libraries_.insert(libraries_.end(), collection->libraries_.begin(), collection->libraries_.end());
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
        auto &group = *(const D3D12_HIT_GROUP_DESC *)subobject.pDesc;
        auto name = [](const WCHAR *import) { return std::wstring(import ? import : L""); };
        hit_groups.push_back(
            {group.HitGroupExport,
             {name(group.ClosestHitShaderImport), name(group.AnyHitShaderImport), name(group.IntersectionShaderImport)}}
        );
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
        auto &association = *(const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *)subobject.pDesc;
        auto of = kind(association.pSubobjectToAssociate->Type);
        if (of < 0)
          break;
        auto root = root_of(association.pSubobjectToAssociate);
        // without exports it declares the default ("Declaring a default association")
        if (!association.NumExports)
          associations.push_back({(RootKind)of, root, Default});
        else
          associated.insert(association.pSubobjectToAssociate);
        for (auto name : std::span(association.pExports, association.NumExports))
          associations.push_back({(RootKind)of, root, Explicit, name});
        break;
      }
      default:
        break;
      }
    }
    // as does a root signature that no association gives exports
    for (auto &subobject : subobjects)
      if (auto of = kind(subobject.Type); of >= 0 && !associated.count(&subobject))
        associations.push_back({(RootKind)of, root_of(&subobject), Declared});

    // the libraries' subobjects ("Subobjects in DXIL libraries"): a library is a scope of its own
    auto &library_roots = library_roots_;
    for (auto &[library, subobject] : library_subobjects) {
      library_names_.insert(subobject.name);
      switch (subobject.type) {
      case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
      case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
        auto root = RootSignature(subobject.bytes);
        if (!root) {
          ERR("CreateStateObject: a DXIL library's root signature is not usable");
          return E_INVALIDARG;
        }
        root_signatures_.push_back(root);
        library_roots[subobject.name] = {(RootKind)kind(subobject.type), root.ptr()};
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        flags |= subobject.values[0];
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
        pipeline_config_ = {subobject.values[0], (D3D12_RAYTRACING_PIPELINE_FLAGS)subobject.values[1]};
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
        // a library names any hit before closest hit
        hit_groups.push_back({subobject.name, {subobject.names[1], subobject.names[0], subobject.names[2]}});
        break;
      default:
        break;
      }
    }
    // an association of a library's subobject: one of a library's own (`library`), or the state object's. a
    // subobject no library defines here waits under its name; one that is no root signature has nothing to give
    auto associate = [&](const RayLibrary *library, const std::wstring &name, std::span<const std::wstring> exports) {
      auto found = library_roots.find(name);
      if (found == library_roots.end() && library_names_.count(name))
        return;
      auto [of, root] = found == library_roots.end() ? std::pair{RootKinds, (ID3D12RootSignature *)nullptr} : found->second;
      if (exports.empty())
        associations.push_back({of, root, library ? LibraryDefault : Default, {}, library, nullptr, root ? L"" : name});
      for (auto &e : exports)
        associations.push_back({of, root, library ? LibraryExplicit : Explicit, e, library, nullptr, root ? L"" : name});
    };
    std::unordered_set<std::wstring> library_associated;
    for (auto &[library, subobject] : library_subobjects)
      if (subobject.type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        associate(library, subobject.names[0], std::span(subobject.names).subspan(1));
        if (subobject.names.size() > 1)
          library_associated.insert(subobject.names[0]);
      }
    for (auto &[library, subobject] : library_subobjects)
      if (auto of = kind(subobject.type); of >= 0 && !library_associated.count(subobject.name))
        associations.push_back({(RootKind)of, library_roots[subobject.name].second, LibraryDeclared, {}, library});
    for (auto &subobject : subobjects)
      if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        auto &association = *(const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *)subobject.pDesc;
        std::vector<std::wstring> exports(association.pExports, association.pExports + association.NumExports);
        associate(nullptr, association.SubobjectToAssociate, exports);
      }
    // the subobjects that were not there when an association named them may be now; an executable state object
    // needs them all
    for (auto &association : associations) {
      if (association.subobject.empty())
        continue;
      if (auto found = library_roots.find(association.subobject); found != library_roots.end()) {
        std::tie(association.kind, association.root) = found->second;
        association.subobject.clear();
      } else if (type_ == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE && !library_names_.count(association.subobject)) {
        ERR("CreateStateObject: an association names a subobject that no DXIL library defines");
        return E_INVALIDARG;
      }
    }

    deferred_ = type_ == D3D12_STATE_OBJECT_TYPE_COLLECTION &&
                (flags & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS);
    if (deferred_) {
      for (auto &[name, shader] : shaders_)
        shader.collection = this;
      for (auto &association : associations)
        association.collection = association.collection ? association.collection : this;
      deferred_hit_groups_ = std::move(hit_groups);
      deferred_associations_ = std::move(associations);
      return S_OK;
    }

    std::vector<sm50_shader_t> linkable;
    for (auto &library : libraries_)
      linkable.push_back(library->shader);
    bool failed = false;
    // the root signature of a shader: the strongest association with the shader, by the name it exports under or,
    // from its own library, the name it has there, with its hit group ("Subobject associations for hit groups"), or
    // with every export of a scope the shader is in. two of one strength that name exports must agree
    // ("Conflicting subobject associations")
    // a shader that a collection left for this state object keeps what the collection associated with it: this
    // state object's associations "do not override any existing associations in contained collections". one of its
    // own that names the shader is a conflict with one of the collection's that does, and stands where the
    // collection gave the shader only a default
    auto root = [&](RootKind of, const std::wstring &name, const RayShader &shader, const std::wstring *group) {
      auto source = ExportName(shader.function);
      // the strongest of the shader's collection, and of everything else
      const Association *best[2] = {};
      for (auto &a : associations) {
        if (a.kind != of || !a.root)
          continue;
        bool applies = a.name.empty()
                           ? (!a.library || a.library == shader.library.get()) &&
                                 (!a.collection || a.collection == shader.collection)
                           : a.name == name || (group && a.name == *group) ||
                                 (a.library == shader.library.get() && a.name == source);
        auto &strongest = best[shader.collection && a.collection == shader.collection];
        if (!applies || (strongest && a.strength < strongest->strength))
          continue;
        if (strongest && a.strength == strongest->strength && !a.name.empty() && a.root != strongest->root) {
          ERR("CreateStateObject: two associations give a shader different root signatures");
          failed = true;
        }
        strongest = &a;
      }
      auto [outer, inner] = best;
      if (!outer || !inner)
        return outer ? outer->root : inner ? inner->root : nullptr;
      if (!outer->name.empty() && !inner->name.empty() && outer->root != inner->root) {
        ERR("CreateStateObject: an association gives a collection's shader another root signature than its collection");
        failed = true;
      }
      return (outer->name.empty() ? inner : outer)->root;
    };
    // a shader compiles once for each pair of root signatures it is used with
    std::map<std::tuple<sm50_shader_t, std::string, ID3D12RootSignature *, ID3D12RootSignature *>,
             std::shared_ptr<RayFunction>>
        compiled;
    auto function = [&](RayExport &to, unsigned index, const std::wstring &name, const std::wstring *group) {
      auto shader = shaders_.find(name);
      if (shader == shaders_.end()) {
        ERR("CreateStateObject: a hit group imports a shader that no library exports");
        failed = true;
        return;
      }
      auto global = root(Global, name, shader->second, group), local = root(Local, name, shader->second, group);
      auto &made = compiled[{shader->second.library->shader, shader->second.function, global, local}];
      if (!made && !(made = Compile(shader->second, global, local, linkable))) {
        failed = true;
        return;
      }
      to.functions[index] = made;
      to.kinds[index] = shader->second.info.kind;
      to.identifier.function[index] = made->slot;
    };
    for (auto &[name, shader] : shaders_) {
      auto of = shader.info.kind;
      bool named = of == SM50_RAY_SHADER_RAY_GENERATION || of == SM50_RAY_SHADER_MISS || of == SM50_RAY_SHADER_CALLABLE;
      if (named && !exports_.count(name))
        function(exports_[name], 0, name, nullptr);
    }
    for (auto &group : hit_groups) {
      if (exports_.count(group.name)) {
        ERR("CreateStateObject: two exports have a hit group's name");
        return E_INVALIDARG;
      }
      auto &to = exports_[group.name];
      for (unsigned i = 0; i < std::size(group.imports); i++)
        if (!group.imports[i].empty())
          function(to, i, group.imports[i], &group.name);
    }
    if (failed)
      return E_INVALIDARG;

    // "Default pipeline stack size": the deepest any chain of the shaders there are can get, with callable shaders
    // two deep
    if (type_ == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
      std::unordered_map<uint32_t, uint64_t> most;
      for (auto &[_, e] : exports_)
        for (auto of : e.kinds)
          if (of)
            most[of] = StackSize(of);
      uint64_t recursion = pipeline_config_.MaxTraceRecursionDepth;
      uint64_t hit = std::max(most[SM50_RAY_SHADER_CLOSEST_HIT], most[SM50_RAY_SHADER_MISS]);
      stack_size_ = most[SM50_RAY_SHADER_RAY_GENERATION] +
                    std::max(hit, most[SM50_RAY_SHADER_INTERSECTION] + most[SM50_RAY_SHADER_ANY_HIT]) *
                        std::min<uint64_t>(1, recursion) +
                    hit * (std::max<uint64_t>(recursion, 1) - 1) + 2 * most[SM50_RAY_SHADER_CALLABLE];
      if (pParent)
        stack_size_ = pParent->stack_size_;
    }
    if (device_->NamesPasses() && type_ == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
      device_->PipelineMade(name, "ray", start);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12StateObject)) {
      *ppvObject = ref(static_cast<MTLD3D12StateObject *>(this));
      return S_OK;
    }
    if (riid == __uuidof(ID3D12StateObjectProperties)) {
      *ppvObject = ref(static_cast<ID3D12StateObjectProperties *>(this));
      return S_OK;
    }
    if (logQueryInterfaceError(__uuidof(ID3D12StateObject), riid))
      WARN("D3D12StateObject: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  void *STDMETHODCALLTYPE
  GetShaderIdentifier(const WCHAR *pExportName) {
    auto found = exports_.find(pExportName);
    return found == exports_.end() ? nullptr : &found->second.identifier;
  }

  // a hit group's shaders are named "group::closesthit", "group::anyhit" and "group::intersection"; the group
  // itself has no size, nor has another shader a part
  UINT64 STDMETHODCALLTYPE
  GetShaderStackSize(const WCHAR *pExportName) {
    std::wstring name = pExportName;
    unsigned index = 0;
    auto at = name.find(L"::");
    bool part = at != std::wstring::npos;
    if (part) {
      const std::wstring_view parts[] = {L"closesthit", L"anyhit", L"intersection"};
      index = std::find(std::begin(parts), std::end(parts), std::wstring_view(name).substr(at + 2)) - parts;
      name.resize(at);
    }
    auto found = exports_.find(name);
    if (found == exports_.end() || index >= std::size(found->second.kinds))
      return 0xffffffff;
    auto kind = found->second.kinds[index];
    bool of_group = kind == SM50_RAY_SHADER_CLOSEST_HIT || kind == SM50_RAY_SHADER_ANY_HIT ||
                    kind == SM50_RAY_SHADER_INTERSECTION;
    return !kind || part != of_group ? 0xffffffff : StackSize(kind);
  }

  UINT64 STDMETHODCALLTYPE
  GetPipelineStackSize() {
    return stack_size_;
  }

  void STDMETHODCALLTYPE
  SetPipelineStackSize(UINT64 PipelineStackSizeInBytes) {
    if (PipelineStackSizeInBytes < 0xffffffff)
      stack_size_ = PipelineStackSizeInBytes;
  }

  bool
  GetPipeline(WMT::ComputePipelineState &Pipeline, WMT::VisibleFunctionTable &Table, uint32_t &Flags) {
    std::unique_lock<dxmt::mutex> lock(pipeline_lock_);
    Flags = pipeline_config_.Flags;
    uint32_t depth = std::max<uint64_t>(stack_size_, 1);
    auto &made = pipelines_[depth];
    if (!made.pipeline) {
      auto start = device_->NamesPasses() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
      auto library = device_->GetLib().getLibrary();
      WMT::Reference<WMT::Function> runtime[] = {
          library.newFunction("dxmt_ray_trace"), library.newFunction("dxmt_ray_report_hit"),
          library.newFunction("dxmt_ray_call")
      };
      // the table has each function at its slot; the pipeline links them all
      std::vector<obj_handle_t> slots(SM50_RAY_FUNCTION_FIRST_SHADER), linked;
      slots[SM50_RAY_FUNCTION_TRACE] = runtime[0];
      slots[SM50_RAY_FUNCTION_REPORT_HIT] = runtime[1];
      slots[SM50_RAY_FUNCTION_CALL] = runtime[2];
      for (auto &[_, e] : exports_)
        for (auto &function : e.functions)
          if (function) {
            slots.resize(std::max<size_t>(slots.size(), function->slot + 1));
            slots[function->slot] = function->function;
          }
      for (auto function : slots)
        if (function)
          linked.push_back(function);
      auto kernel = library.newFunction("dispatch_rays");
      WMTLinkedComputePipelineInfo info{};
      info.compute_function = kernel;
      info.functions.set(linked.data());
      info.function_count = linked.size();
      info.max_call_stack_depth = depth;
      WMT::Reference<WMT::Error> err;
      made.pipeline = device_->GetMTLDevice().newLinkedComputePipelineState(info, err);
      if (!made.pipeline) {
        ERR("Failed to create a ray tracing pipeline: ", err ? err.description().getUTF8String() : "");
        return false;
      }
      made.table = made.pipeline.newVisibleFunctionTable(slots.data(), slots.size());
      if (device_->NamesPasses())
        device_->PipelineMade(name, "ray", start);
    }
    Pipeline = made.pipeline;
    Table = made.table;
    return true;
  }
};

HRESULT
CreateStateObject(
    MTLD3D12Device *pDevice, const D3D12_STATE_OBJECT_DESC *pDesc, ID3D12StateObject *pParent, REFIID riid,
    void **ppStateObject
) {
  InitReturnPtr(ppStateObject);
  if (!pDesc)
    return E_INVALIDARG;
  auto state = Com(new MTLD3D12StateObjectImpl(pDevice));
  HRESULT hr = state->Initialize(pDesc, static_cast<MTLD3D12StateObjectImpl *>(pParent));
  if (FAILED(hr))
    return hr;
  return state->QueryInterface(riid, ppStateObject);
}

} // namespace dxmt
