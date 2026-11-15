#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_pipeline.hpp"
#include "airconv_ray.h"
#include "mullion_command_constants.hpp"
#include "mullion_shader_cache.hpp"
#include "com/com_pointer.hpp"
#include "util_md5.hpp"
#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// DXR state objects (DXR spec, "State objects"). a ray tracing shader is a visible function of Metal at a table slot
// the device gave it, and a shader identifier holds its shaders' slots (airconv_ray.h): a pipeline is the kernel
// that dispatches rays, linked with the functions of every export, and a function table with each at its slot. the
// pipeline's runtime calls its shaders directly where it can, through dispatchers the pipeline makes (RayDispatchers)

namespace mullion {

namespace {

// the name a library's function exports under: "name" of the C++ mangling "\x01?name@@..."
std::wstring
ExportName(std::string_view function) {
  if (function.starts_with("\x01?"))
    function = function.substr(2, function.find("@@") - 2);
  return std::wstring(function.begin(), function.end());
}

// a DXIL library. its conversion state is made when a conversion first asks for it: a state object whose shaders are
// all stored parses no library
class RayLibrary {
  mullion::mutex mutex_;
  // the container, until the state is made of it
  std::string bytecode_;
  sm50_shader_t shader_ = nullptr;

public:
  // of its container (ShaderDigest)
  Sha1Digest digest{};

  RayLibrary(const D3D12_SHADER_BYTECODE &container) :
      bytecode_((const char *)container.pShaderBytecode, container.BytecodeLength),
      digest(ShaderDigest(container.pShaderBytecode, container.BytecodeLength)) {}

  // null, with `error`, for a container that is no usable library
  sm50_shader_t
  Shader(SM50Error &error) {
    std::lock_guard lock(mutex_);
    if (!shader_ && !SM50Initialize(bytecode_.data(), bytecode_.size(), &shader_, nullptr, &error))
      bytecode_ = {};
    return shader_;
  }

  ~RayLibrary() {
    if (shader_)
      SM50Destroy(shader_);
  }
};

// a library's ray tracing shaders, as SM50GetRayShader gives them
using RayShaderList = std::vector<std::pair<std::string, SM50_RAY_SHADER_INFO>>;

// a stored list: this, each shader's information, its name's length and its name, and the SHA-1 of it all. the size
// tells a record of another layout from this one, and the digest a damaged one
struct RayShaderListRecord {
  char magic[8];
  uint32_t information_size;
  uint32_t shaders;
};
constexpr char kRayShaderListMagic[] = "SM50RAYS";
static_assert(sizeof(kRayShaderListMagic) - 1 == sizeof(RayShaderListRecord::magic));

Sha1Digest
RayShaderListKey(const Sha1Digest &library) {
  Sha1HashState h;
  h.update(kRayShaderListMagic, sizeof(kRayShaderListMagic));
  h.update(library);
  return h.final();
}

// the list of the library with this digest, as an earlier state object or an earlier process stored it in the shader
// cache, whose table belongs to one AIRCONV_VERSION. false when there is none, or no whole one
bool
StoredRayShaders(ShaderCache &cache, const Sha1Digest &library, RayShaderList &shaders) {
  WMT::Reference<WMT::DispatchData> stored;
  if (auto reader = cache.getReader())
    stored = reader.get(RayShaderListKey(library));
  if (!stored)
    return false;
  std::string bytes(stored.getBytes(nullptr, 0), '\0');
  stored.getBytes(bytes.data(), bytes.size());
  RayShaderListRecord record;
  if (bytes.size() < sizeof(record) + sizeof(Sha1Digest))
    return false;
  size_t body = bytes.size() - sizeof(Sha1Digest);
  memcpy(&record, bytes.data(), sizeof(record));
  if (memcmp(record.magic, kRayShaderListMagic, sizeof(record.magic)) ||
      record.information_size != sizeof(SM50_RAY_SHADER_INFO) ||
      memcmp(Sha1HashState::compute(bytes.data(), body).data, bytes.data() + body, sizeof(Sha1Digest)))
    return false;
  RayShaderList read;
  size_t at = sizeof(record);
  for (uint32_t i = 0; i < record.shaders; i++) {
    SM50_RAY_SHADER_INFO info;
    uint32_t length;
    if (body - at < sizeof(info) + sizeof(length))
      return false;
    memcpy(&info, bytes.data() + at, sizeof(info));
    memcpy(&length, bytes.data() + at + sizeof(info), sizeof(length));
    at += sizeof(info) + sizeof(length);
    if (body - at < length)
      return false;
    read.emplace_back(bytes.substr(at, length), info);
    at += length;
  }
  if (at != body)
    return false;
  shaders = std::move(read);
  return true;
}

void
StoreRayShaders(ShaderCache &cache, const Sha1Digest &library, const RayShaderList &shaders) {
  auto writer = cache.getWriter();
  if (!writer)
    return;
  RayShaderListRecord record{{}, sizeof(SM50_RAY_SHADER_INFO), uint32_t(shaders.size())};
  memcpy(record.magic, kRayShaderListMagic, sizeof(record.magic));
  std::string bytes((const char *)&record, sizeof(record));
  for (auto &[name, info] : shaders) {
    uint32_t length = name.size();
    bytes.append((const char *)&info, sizeof(info));
    bytes.append((const char *)&length, sizeof(length));
    bytes.append(name);
  }
  auto digest = Sha1HashState::compute(bytes.data(), bytes.size());
  bytes.append((const char *)digest.data, sizeof(digest));
  writer.set(RayShaderListKey(library), WMT::MakeDispatchData(bytes.data(), bytes.size()));
}

// a function of a library that is a ray tracing shader
struct RayShader {
  std::shared_ptr<RayLibrary> library;
  std::string function;
  SM50_RAY_SHADER_INFO info;
  // the collection it came from, which is a scope of its own
  const void *collection = nullptr;
};

} // namespace

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
  if (data.size() < 8 || word(data, 4) > (data.size() - 8) / sizeof(uint32_t))
    return {};
  for (uint32_t i = 0, count = word(data, 4); i < count; i++) {
    uint64_t at = word(data, 8 + (uint64_t)sizeof(uint32_t) * i);
    if (at < 8 + (uint64_t)sizeof(uint32_t) * count || at > data.size() - 8)
      return {};
    uint64_t type = word(data, at), size = word(data, at + 4);
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
  if (parts[Subobjects].size() < 8 || stride < 2 * sizeof(uint32_t) ||
      word(parts[Subobjects], 0) > (parts[Subobjects].size() - 8) / stride)
    return {};
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
      if ((uint64_t)field(3) * sizeof(uint32_t) + sizeof(uint32_t) > parts[Indices].size() ||
          word(parts[Indices], (uint64_t)field(3) * sizeof(uint32_t)) >
              (parts[Indices].size() / sizeof(uint32_t)) - field(3) - 1)
        return {};
      for (uint32_t n = 0, row = field(3), length = word(parts[Indices], (uint64_t)row * 4); n < length; n++)
        subobject.names.push_back(string(word(parts[Indices], ((uint64_t)row + 1 + n) * 4)));
      break;
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
      subobject.values[1] = field(3);
      [[fallthrough]];
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

namespace {

// a hit group: its name, and the names of its closest hit, any hit and intersection shaders, empty for none
struct HitGroup {
  std::wstring name, imports[3];
};

// how an association of a subobject with exports was made, the weakest first ("Subobject association behavior"):
// what the state object says overrides what its libraries say, and of each, an association that names exports comes
// before one that names none, and that before a subobject no association of its scope gives exports
enum Strength { LibraryDeclared, LibraryDefault, LibraryExplicit, Declared, Default, Explicit };

enum SubobjectKind { Global, Local, ShaderConfig, PipelineConfig, SubobjectKinds };

struct Association {
  SubobjectKind kind;
  ID3D12RootSignature *root;
  Strength strength;
  // the export, or none for every export of the scope
  std::wstring name;
  // the scopes of one without an export: the library whose subobjects made it, and the collection it was made in
  const RayLibrary *library;
  const void *collection;
  // an unresolved library subobject (DXR "Explicit associations": the subobject need not "be visible yet")
  std::wstring subobject;
  std::pair<uint32_t, uint32_t> values{};
};

// a shader compiled with its root signatures: the device's one of its key (MTLD3D12Device::ray_functions), whose
// Metal function the key names. the slot is this process's, and nothing converted holds it
struct RayFunction {
  MTLD3D12Device *device;
  Sha1Digest key;
  uint32_t slot;
  Com<MTLD3D12RootSignature, false> global, local;
  WMT::Reference<WMT::Function> function;
  uint32_t calls = 0; // SM50_RAY_SHADER_CALLS

  std::string
  Name() const {
    return "mullion_ray_shader_" + key.string();
  }

  ~RayFunction() {
    {
      std::lock_guard lock(device->ray_functions.mutex);
      // another of the key may have been made since the last reference to this one went
      if (auto found = device->ray_functions.made.find(key);
          found != device->ray_functions.made.end() && found->second.expired())
        device->ray_functions.made.erase(found);
    }
    device->FreeRayFunction(slot);
  }
};

// what a shader identifier names: a ray generation, miss or callable shader, or a hit group's closest hit, any hit
// and intersection shaders, in the identifier's order
struct RayExport {
  std::shared_ptr<SM50_RAY_SHADER_IDENTIFIER> identifier = std::make_shared<SM50_RAY_SHADER_IDENTIFIER>();
  std::shared_ptr<RayFunction> functions[3];
  uint32_t kinds[3]{}; // SM50_RAY_SHADER_KIND
};

// MULLION_D3D12_SPECIALIZED_RAYS=0: pipelines call every shader through the function table, and their traces do not
// know which kinds of shaders the pipeline has
bool
SpecializedRays() {
  static const bool enabled = env::getEnvVar("MULLION_D3D12_SPECIALIZED_RAYS") != "0";
  return enabled;
}

// the most shaders of one kind a dispatcher calls directly; it calls any more through the table
constexpr size_t kMaxDirectRayCalls = 256;

// a pipeline's dispatchers (mullion_command.metal, kRayFCIndex_DirectCalls), one for each kind of shader the runtime
// calls: a switch of direct calls of the shaders of that kind, by their slots (SM50CompileRayDispatcher), each a
// library of its one function. a closest hit or miss shader that may trace, and a callable shader that may trace or
// call one, is called through the table: a direct call would make the dispatcher reach itself. a dispatcher of no
// such shader calls nothing for another slot, which names none of the pipeline's shaders of its kind
bool
RayDispatchers(
    MTLD3D12Device *device, const std::vector<std::pair<uint32_t, const RayFunction *>> &shaders,
    std::vector<WMT::Reference<WMT::Function>> &dispatchers, std::string &error
) {
  static const char *const names[SM50_RAY_DISPATCHER_COUNT] = {
      "mullion_ray_generation_shader", "mullion_ray_closest_shader", "mullion_ray_any_hit_shader",
      "mullion_ray_intersection_shader", "mullion_ray_callable_shader",
  };
  for (uint32_t dispatcher = 0; dispatcher < SM50_RAY_DISPATCHER_COUNT; dispatcher++) {
    std::vector<SM50_RAY_DISPATCHED_SHADER> direct;
    bool table = false;
    for (auto [kind, function] : shaders) {
      uint32_t of = kind == SM50_RAY_SHADER_RAY_GENERATION                              ? SM50_RAY_DISPATCHER_GENERATION
                    : kind == SM50_RAY_SHADER_CLOSEST_HIT || kind == SM50_RAY_SHADER_MISS ? SM50_RAY_DISPATCHER_CLOSEST
                    : kind == SM50_RAY_SHADER_ANY_HIT                                    ? SM50_RAY_DISPATCHER_ANY_HIT
                    : kind == SM50_RAY_SHADER_INTERSECTION ? SM50_RAY_DISPATCHER_INTERSECTION
                                                           : SM50_RAY_DISPATCHER_CALLABLE;
      if (of != dispatcher ||
          std::any_of(direct.begin(), direct.end(), [&](auto &other) { return other.slot == function->slot; }))
        continue;
      uint32_t reaching = of == SM50_RAY_DISPATCHER_CLOSEST ? SM50_RAY_SHADER_CALLS_TRACE
                          : of == SM50_RAY_DISPATCHER_CALLABLE
                              ? SM50_RAY_SHADER_CALLS_TRACE | SM50_RAY_SHADER_CALLS_CALLABLE
                              : 0;
      auto name = function->Name();
      if ((function->calls & reaching) || direct.size() >= kMaxDirectRayCalls ||
          name.size() >= sizeof(SM50_RAY_DISPATCHED_SHADER::name)) {
        table = true;
        continue;
      }
      auto &shader = direct.emplace_back(SM50_RAY_DISPATCHED_SHADER{function->slot});
      name.copy(shader.name, sizeof(shader.name) - 1);
    }
    SM50ShaderBitcode bitcode;
    SM50Error conversion_error;
    if (SM50CompileRayDispatcher(
            SM50_RAY_DISPATCHER(dispatcher), direct.data(), direct.size(), table, device->GetMetalVersion(), &bitcode,
            &conversion_error
        )) {
      error = SM50GetErrorMessageString(conversion_error);
      return false;
    }
    SM50_COMPILED_BITCODE compiled;
    SM50GetCompiledBitcode(bitcode, &compiled);
    WMT::Reference<WMT::Error> metal_error;
    auto library = device->GetMTLDevice().newLibrary(WMT::MakeDispatchData(compiled.Data, compiled.Size), metal_error);
    auto function = library ? library.newFunction(names[dispatcher]) : WMT::Reference<WMT::Function>{};
    if (!function) {
      error = metal_error ? metal_error.description().getUTF8String() : "the library has no dispatcher";
      return false;
    }
    dispatchers.push_back(std::move(function));
  }
  return true;
}

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
  uint32_t flags_ = 0;
  // every shader the state object has seen, by name, for the hit groups that import them
  std::unordered_map<std::wstring, RayShader> shaders_;
  std::unordered_map<std::wstring, RayExport> exports_;
  std::vector<Com<MTLD3D12RootSignature, false>> root_signatures_;
  D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline_config_{};
  std::optional<std::pair<uint32_t, uint32_t>> configurations_[SubobjectKinds - ShaderConfig];
  uint64_t stack_size_ = 0;
  uint64_t max_call_depth_ = UINT32_MAX;
  // the libraries of this state object's own description, whose functions its shaders may call
  std::vector<std::shared_ptr<RayLibrary>> libraries_;
  // collections retain associations and uncompiled hit groups for the objects that use their exports
  bool deferred_ = false;
  std::vector<HitGroup> deferred_hit_groups_;
  std::vector<Association> deferred_associations_;
  // the libraries' definitions by their subobjects' names, and the names of all their subobjects: an association
  // may name one of another scope
  std::unordered_map<std::wstring, Association> library_definitions_;
  std::unordered_set<std::wstring> library_names_;

  struct Pipeline {
    WMT::Reference<WMT::ComputePipelineState> pipeline;
    WMT::Reference<WMT::VisibleFunctionTable> table;
  };
  // by call depth, made when the state object is and when its stack size changes: a dispatch only binds one, and
  // commands in flight keep using the one they were recorded with. one Metal could not make has no pipeline
  mullion::mutex pipeline_lock_;
  std::map<uint32_t, Pipeline> pipelines_;

  // the call depth of the pipeline the stack size asks for
  uint32_t
  CallDepth() const {
    return std::max<uint64_t>(std::min(stack_size_, max_call_depth_), 1);
  }

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
    if (!ShaderContainerHolds(desc.DXILLibrary.pShaderBytecode, desc.DXILLibrary.BytecodeLength) ||
        !ShaderFeaturesSupported(device_, desc.DXILLibrary.pShaderBytecode, desc.DXILLibrary.BytecodeLength))
      return FAIL(E_INVALIDARG);
    auto library = std::make_shared<RayLibrary>(desc.DXILLibrary);
    // its shaders, which a library that was parsed before has stored: such a library was usable
    auto &cache = ShaderCache::getInstance(WMTMetalVersion(device_->GetMetalVersion()));
    RayShaderList listed;
    if (!StoredRayShaders(cache, library->digest, listed)) {
      SM50Error error;
      auto shader = library->Shader(error);
      if (!shader) {
        ERR("CreateStateObject: a DXIL library is not usable: ", SM50GetErrorMessageString(error));
        return E_INVALIDARG;
      }
      char name[1024];
      SM50_RAY_SHADER_INFO info;
      for (uint32_t i = 0; SM50GetRayShader(shader, i, name, sizeof(name), &info); i++)
        listed.emplace_back(name, info);
      StoreRayShaders(cache, library->digest, listed);
    }
    std::unordered_map<std::wstring, RayShader> shaders;
    for (auto &[name, info] : listed)
      shaders.insert({ExportName(name), {library, name, info}});
    Export(shaders, {desc.pExports, desc.NumExports}, shaders_);
    libraries_.push_back(library);
    if (device_->NamesPasses())
      this->name += library->digest.string() + " ";
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

  // the shader converted for its root signatures, as the device has it, the shader cache stores it, or a conversion
  // now makes it. `libraries`: those whose functions the shader may call, this state object's
  std::shared_ptr<RayFunction>
  Compile(
      const RayShader &shader, ID3D12RootSignature *pGlobal, ID3D12RootSignature *pLocal,
      std::span<const std::shared_ptr<RayLibrary>> libraries, bool &unresolved
  ) {
    auto global_root = static_cast<MTLD3D12RootSignature *>(pGlobal),
         local_root = static_cast<MTLD3D12RootSignature *>(pLocal);
    // the address of the local root signature's static samplers is in the converted code
    uint64_t local_samplers = local_root ? local_root->StaticSamplersAddress : 0;
    bool links = shader.info.calls & SM50_RAY_SHADER_CALLS_LIBRARY;

    // everything the conversion reads (convert_dxil_ray_shader). the function's name is the key's, so that what is
    // stored is the same whatever slot the shader has
    Sha1HashState h;
    auto hash = [&](const auto &...values) { (h.update(values), ...); };
    hash("d3d12 ray", uint32_t(AIRCONV_VERSION), device_->GetMTLDevice().registryID(), shader.library->digest);
    hash(uint64_t(shader.function.size()));
    h.update(shader.function.data(), shader.function.size());
    hash(uint32_t(device_->GetMetalVersion()), uint32_t(device_->GetShaderFlags()), uint32_t(device_->GetSIMDWidth()));
    hash(bool(global_root), bool(local_root));
    if (global_root)
      hash(global_root->Digest);
    if (local_root)
      hash(local_root->Digest, local_samplers);
    // the libraries in the order a function is looked for in them
    if (links)
      for (auto &library : libraries)
        hash(library->digest);
    auto key = h.final();

    auto &shared = device_->ray_functions;
    auto made = [&]() -> std::shared_ptr<RayFunction> {
      std::lock_guard lock(shared.mutex);
      auto found = shared.made.find(key);
      return found == shared.made.end() ? nullptr : std::static_pointer_cast<RayFunction>(found->second.lock());
    };
    if (auto function = made())
      return function;

    auto function = std::make_shared<RayFunction>(device_, key, device_->AllocateRayFunction());
    function->global = global_root;
    function->local = local_root;
    function->calls = shader.info.calls;
    auto name = function->Name();
    auto load = [&](WMT::Reference<WMT::DispatchData> data, WMT::Reference<WMT::Error> &error) {
      auto library = device_->GetMTLDevice().newLibrary(data, error);
      return library ? library.newFunction(name.c_str()) : WMT::Reference<WMT::Function>{};
    };
    // an address in the code is this process's, so such a shader is not stored
    auto &cache = ShaderCache::getInstance(WMTMetalVersion(device_->GetMetalVersion()));
    auto reader = local_samplers ? WMT::CacheReader{} : cache.getReader();
    auto writer = local_samplers ? WMT::CacheWriter{} : cache.getWriter();
    WMT::Reference<WMT::Error> error;
    if (reader)
      if (auto stored = reader.get(key)) {
        if (!(function->function = load(stored, error)) && writer)
          writer.set(key, {});
      }
    if (!function->function) {
      auto blob = [](MTLD3D12RootSignature *root, SM50_SHADER_COMPILATION_ARGUMENT_TYPE type, void *next) {
        SM50_SHADER_ROOT_SIGNATURE_DATA data{next, type};
        data.bytecode_length = root->GetBlob(&data.bytecode);
        data.static_samplers = root->StaticSamplersAddress;
        return data;
      };
      SM50Error conversion_error;
      auto unusable = [&](const RayLibrary &library) {
        ERR("CreateStateObject: the DXIL library ", library.digest.string(),
            " is not usable: ", SM50GetErrorMessageString(conversion_error));
        return nullptr;
      };
      auto converted = shader.library->Shader(conversion_error);
      if (!converted)
        return unusable(*shader.library);
      std::vector<sm50_shader_t> linkable;
      if (links)
        for (auto &library : libraries) {
          linkable.push_back(library->Shader(conversion_error));
          if (!linkable.back())
            return unusable(*library);
        }
      SM50_SHADER_LIBRARIES_DATA linked{nullptr, SM50_SHADER_LIBRARIES, linkable.data(), (uint32_t)linkable.size()};
      SM50_SHADER_RAY_SHADER_DATA ray{&linked, SM50_SHADER_RAY_SHADER, shader.function.c_str()};
      void *arguments = &ray;
      SM50_SHADER_ROOT_SIGNATURE_DATA local, global;
      if (local_root)
        arguments = &(local = blob(local_root, SM50_SHADER_ROOT_SIGNATURE2, arguments));
      if (global_root)
        arguments = &(global = blob(global_root, SM50_SHADER_ROOT_SIGNATURE, arguments));
      SM50_SHADER_COMMON_DATA common{arguments, SM50_SHADER_COMMON, device_->GetMetalVersion()};
      common.flags = device_->GetShaderFlags();
      common.simd_width = device_->GetSIMDWidth();

      SM50ShaderBitcode bitcode;
      if (auto result = SM50Compile(
              converted, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, name.c_str(), &bitcode, &conversion_error
          )) {
        unresolved = result == SM50_COMPILE_UNRESOLVED;
        if (!unresolved)
          ERR("CreateStateObject: failed to compile ", shader.function, ": ",
              SM50GetErrorMessageString(conversion_error));
        return nullptr;
      }
      SM50_COMPILED_BITCODE compiled;
      SM50GetCompiledBitcode(bitcode, &compiled);
      auto data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
      if (!(function->function = load(data, error))) {
        ERR("CreateStateObject: no Metal function for ", shader.function, ": ",
            error ? error.description().getUTF8String() : "the library has none of its name");
        return nullptr;
      }
      if (writer)
        writer.set(key, data);
    }
    // another thread may have made the same meanwhile: a pipeline links one function of the name
    std::shared_ptr<RayFunction> other;
    {
      std::lock_guard lock(shared.mutex);
      auto &kept = shared.made[key];
      if (!(other = std::static_pointer_cast<RayFunction>(kept.lock())))
        kept = function;
    }
    return other ? other : function;
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
      if (type_ != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE || pParent->type_ != type_)
        return FAIL(E_INVALIDARG);
      if (device_->NamesPasses())
        name = pParent->name;
      shaders_ = pParent->shaders_;
      exports_ = pParent->exports_;
      root_signatures_ = pParent->root_signatures_;
      pipeline_config_ = pParent->pipeline_config_;
      std::copy(std::begin(pParent->configurations_), std::end(pParent->configurations_), configurations_);
    }
    std::span subobjects(pDesc->pSubobjects, pDesc->NumSubobjects);

    using Subobject = const D3D12_STATE_SUBOBJECT *;
    auto kind = [](D3D12_STATE_SUBOBJECT_TYPE type) {
      return type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE      ? (int)Global
             : type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE     ? (int)Local
             : type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG ? (int)ShaderConfig
             : type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG ||
                     type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1
                 ? (int)PipelineConfig
                 : -1;
    };
    auto definition = [&](Subobject subobject) {
      Association made{(SubobjectKind)kind(subobject->Type)};
      if (made.kind == Global || made.kind == Local)
        made.root = ((const D3D12_GLOBAL_ROOT_SIGNATURE *)subobject->pDesc)->pGlobalRootSignature;
      else if (made.kind == ShaderConfig) {
        auto &config = *(const D3D12_RAYTRACING_SHADER_CONFIG *)subobject->pDesc;
        made.values = {config.MaxPayloadSizeInBytes, config.MaxAttributeSizeInBytes};
      } else if (made.kind == PipelineConfig) {
        made.values.first = ((const D3D12_RAYTRACING_PIPELINE_CONFIG *)subobject->pDesc)->MaxTraceRecursionDepth;
        if (subobject->Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1)
          made.values.second = ((const D3D12_RAYTRACING_PIPELINE_CONFIG1 *)subobject->pDesc)->Flags;
      }
      return made;
    };
    auto same = [](const Association &a, const Association &b) {
      if (a.kind >= ShaderConfig)
        return a.values == b.values;
      if (a.root == b.root)
        return true;
      if (!a.root || !b.root)
        return false;
      const void *first, *second;
      static_cast<MTLD3D12RootSignature *>(a.root)->GetBlob(&first);
      static_cast<MTLD3D12RootSignature *>(b.root)->GetBlob(&second);
      UINT size, other_size;
      return SUCCEEDED(microsoft::DXBCGetRootSignature(first, &first, &size)) &&
             SUCCEEDED(microsoft::DXBCGetRootSignature(second, &second, &other_size)) && size == other_size &&
             !memcmp(first, second, size);
    };
    bool failed = false;
    auto configuration = [&](SubobjectKind of, const std::pair<uint32_t, uint32_t> &values) {
      auto &seen = configurations_[of - ShaderConfig];
      if (seen && *seen != values) {
        ERR("CreateStateObject: ray tracing configurations do not match across exports");
        failed = true;
      }
      seen = values;
    };
    std::vector<Association> associations;
    std::unordered_set<const RayLibrary *> configured;
    if (pParent)
      for (auto &[name, shader] : pParent->shaders_)
        configured.insert(shader.library.get());
    // the state object's subobjects that an association of its own gives exports
    std::unordered_set<Subobject> associated;
    std::vector<HitGroup> hit_groups;
    std::vector<std::pair<const RayLibrary *, LibrarySubobject>> library_subobjects;
    for (auto &subobject : subobjects) {
      switch (subobject.Type) {
      case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        flags_ |= ((const D3D12_STATE_OBJECT_CONFIG *)subobject.pDesc)->Flags;
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
      case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        root_signatures_.push_back(static_cast<MTLD3D12RootSignature *>(definition(&subobject).root));
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
        if (!collection->deferred_) {
          for (auto &[name, shader] : collection->shaders_)
            configured.insert(shader.library.get());
          for (unsigned of = ShaderConfig; of < SubobjectKinds; of++)
            if (auto &config = collection->configurations_[of - ShaderConfig]; config)
              configuration((SubobjectKind)of, *config);
        }
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
        library_definitions_.insert(collection->library_definitions_.begin(), collection->library_definitions_.end());
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
        auto made = definition(association.pSubobjectToAssociate);
        // without exports it declares the default ("Declaring a default association")
        made.strength = Default;
        if (!association.NumExports)
          associations.push_back(made);
        else
          associated.insert(association.pSubobjectToAssociate);
        for (auto name : std::span(association.pExports, association.NumExports)) {
          made.strength = Explicit;
          made.name = name;
          associations.push_back(made);
        }
        break;
      }
      default:
        break;
      }
    }
    // as does a subobject that no association gives exports
    for (auto &subobject : subobjects)
      if (auto of = kind(subobject.Type); of >= 0 && !associated.count(&subobject)) {
        auto made = definition(&subobject);
        made.strength = Declared;
        associations.push_back(made);
      }

    // the libraries' subobjects ("Subobjects in DXIL libraries"): a library is a scope of its own
    auto &library_definitions = library_definitions_;
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
        root_signatures_.push_back(static_cast<MTLD3D12RootSignature *>(root.ptr()));
        library_definitions[subobject.name] = {
            (SubobjectKind)kind(subobject.type), root.ptr(), LibraryDeclared, {}, library};
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        flags_ |= subobject.values[0];
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
        library_definitions[subobject.name] = {
            (SubobjectKind)kind(subobject.type),       nullptr, LibraryDeclared, {}, library, nullptr, {},
            {subobject.values[0], subobject.values[1]}};
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
    // subobject no library defines here waits under its name
    auto associate = [&](const RayLibrary *library, const std::wstring &name, std::span<const std::wstring> exports) {
      auto found = library_definitions.find(name);
      if (found == library_definitions.end() && library_names_.count(name))
        return;
      auto made = found == library_definitions.end() ? Association{SubobjectKinds} : found->second;
      made.library = library;
      if (found == library_definitions.end())
        made.subobject = name;
      made.strength = library ? LibraryDefault : Default;
      if (exports.empty())
        associations.push_back(made);
      for (auto &e : exports) {
        made.strength = library ? LibraryExplicit : Explicit;
        made.name = e;
        associations.push_back(made);
      }
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
        associations.push_back(library_definitions[subobject.name]);
    for (auto &subobject : subobjects)
      if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        auto &association = *(const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *)subobject.pDesc;
        std::vector<std::wstring> exports(association.pExports, association.pExports + association.NumExports);
        associate(nullptr, association.SubobjectToAssociate, exports);
      }
    // DXR "Explicit associations": an executable state object needs both ends of an association resolved
    for (auto &association : associations) {
      if (association.subobject.empty())
        continue;
      if (auto found = library_definitions.find(association.subobject); found != library_definitions.end()) {
        association.kind = found->second.kind;
        association.root = found->second.root;
        association.values = found->second.values;
        association.subobject.clear();
      } else if (library_names_.count(association.subobject)) {
        association.subobject.clear();
      } else if (type_ == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
        ERR("CreateStateObject: an association names a subobject that no DXIL library defines");
        return E_INVALIDARG;
      }
    }

    if (pParent && (!(flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) ||
                    !(pParent->flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS)))
      return FAIL(E_INVALIDARG);
    deferred_ = type_ == D3D12_STATE_OBJECT_TYPE_COLLECTION &&
                (flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS);
    if (type_ == D3D12_STATE_OBJECT_TYPE_COLLECTION) {
      for (auto &[name, shader] : shaders_)
        shader.collection = this;
      for (auto &association : associations)
        association.collection = association.collection ? association.collection : this;
    }

    bool unresolved = false;
    // the subobject of a shader: the strongest association with the shader, by the name it exports under or,
    // from its own library, the name it has there, with its hit group ("Subobject associations for hit groups"), or
    // with every export of a scope the shader is in. two of one strength that name exports must agree
    // ("Conflicting subobject associations")
    // a shader that a collection left for this state object keeps what the collection associated with it: this
    // state object's associations "do not override any existing associations in contained collections". one of its
    // own that names the shader is a conflict with one of the collection's that does, and stands where the
    // collection gave the shader only a default
    auto resolve = [&](SubobjectKind of, const std::wstring &name, const RayShader &shader, const std::wstring *group) {
      auto source = ExportName(shader.function);
      const Association *best[2] = {};
      auto applies = [&](const Association &a) {
        return a.name.empty() ? (!a.library || a.library == shader.library.get()) &&
                                    (!a.collection || a.collection == shader.collection)
                              : a.name == name || (group && a.name == *group) ||
                                    (a.library == shader.library.get() && a.name == source);
      };
      bool ambiguous[std::size(best)] = {};
      for (auto &a : associations) {
        unsigned scope = shader.collection && a.collection == shader.collection;
        auto &strongest = best[scope];
        if (!applies(a))
          continue;
        unresolved |= !a.subobject.empty();
        if (a.kind != of)
          continue;
        if (!strongest || a.strength > strongest->strength) {
          strongest = &a;
          ambiguous[scope] = false;
        } else if (a.strength == strongest->strength)
          ambiguous[scope] |= !same(a, *strongest);
      }
      for (unsigned i = 0; i < std::size(best); i++)
        if (ambiguous[i]) {
          if (!best[i]->name.empty()) {
            ERR("CreateStateObject: conflicting explicit subobject associations");
            failed = true;
          }
          best[i] = nullptr;
        }
      auto [outer, inner] = best;
      if (!outer || !inner) {
        if (!outer && !inner && of >= ShaderConfig && (ambiguous[0] || ambiguous[1]))
          failed = true;
        return outer ? outer : inner;
      }
      if (!outer->name.empty() && !inner->name.empty() && !same(*outer, *inner)) {
        ERR("CreateStateObject: a subobject association conflicts with its collection's");
        failed = true;
      }
      return outer->name.empty() ? inner : outer;
    };
    auto validate = [&](const std::wstring &name, const RayShader &shader, const std::wstring *group, bool inherited) {
      // DXR "Subobject association requirements": shader and pipeline configs are required and matching for all exports
      for (unsigned of = ShaderConfig; of < SubobjectKinds; of++) {
        auto config = resolve((SubobjectKind)of, name, shader, group);
        if (!config) {
          if (inherited && configurations_[of - ShaderConfig])
            continue;
          unresolved = true;
          if (!deferred_) {
            ERR("CreateStateObject: a ray tracing shader has no unambiguous configuration");
            failed = true;
          }
          continue;
        }
        configuration((SubobjectKind)of, config->values);
      }
    };
    auto function = [&](RayExport &to, unsigned index, const std::wstring &name, const std::wstring *group) {
      auto shader = shaders_.find(name);
      if (shader == shaders_.end()) {
        if (!deferred_) {
          ERR("CreateStateObject: a hit group imports a shader that no library exports");
          failed = true;
        }
        return false;
      }
      unresolved = false;
      validate(name, shader->second, group, configured.count(shader->second.library.get()));
      if (failed)
        return false;
      auto global_association = resolve(Global, name, shader->second, group),
           local_association = resolve(Local, name, shader->second, group);
      auto global = global_association ? global_association->root : nullptr,
           local = local_association ? local_association->root : nullptr;
      if (failed || unresolved) {
        failed |= !deferred_;
        return false;
      }
      auto previous = to.functions[index];
      if (previous && same({Global, global}, {Global, previous->global.ptr()}) &&
          same({Local, local}, {Local, previous->local.ptr()}))
        return true;
      auto made = Compile(shader->second, global, local, libraries_, unresolved);
      if (!made) {
        failed |= !deferred_ || !unresolved;
        return false;
      }
      if (previous)
        to.identifier = std::make_shared<SM50_RAY_SHADER_IDENTIFIER>(*to.identifier);
      to.functions[index] = made;
      to.kinds[index] = shader->second.info.kind;
      to.identifier->function[index] = made->slot;
      return true;
    };
    for (auto &[name, shader] : shaders_) {
      auto of = shader.info.kind;
      bool named = of == SM50_RAY_SHADER_RAY_GENERATION || of == SM50_RAY_SHADER_MISS || of == SM50_RAY_SHADER_CALLABLE;
      if (named && (!pParent || !pParent->shaders_.count(name))) {
        RayExport to = exports_.count(name) ? exports_.at(name) : RayExport{};
        if (function(to, 0, name, nullptr))
          exports_.insert_or_assign(name, std::move(to));
      } else if ((!pParent || !pParent->shaders_.count(name)) &&
               std::none_of(hit_groups.begin(), hit_groups.end(), [&](const HitGroup &group) {
                 return std::find(std::begin(group.imports), std::end(group.imports), name) != std::end(group.imports);
               })) {
        if (configured.count(shader.library.get())) {
          unresolved = false;
          validate(name, shader, nullptr, configured.count(shader.library.get()));
          failed |= unresolved && !deferred_;
        } else {
          RayExport checked;
          function(checked, 0, name, nullptr);
        }
      }
    }
    for (auto &group : hit_groups) {
      if (exports_.count(group.name)) {
        ERR("CreateStateObject: two exports have a hit group's name");
        return E_INVALIDARG;
      }
      RayExport to;
      bool complete = true;
      for (unsigned i = 0; i < std::size(group.imports); i++)
        if (!group.imports[i].empty())
          complete &= function(to, i, group.imports[i], &group.name);
      if (complete)
        exports_.insert({group.name, std::move(to)});
      else if (deferred_)
        deferred_hit_groups_.push_back(group);
    }
    if (type_ == D3D12_STATE_OBJECT_TYPE_COLLECTION)
      deferred_associations_ = std::move(associations);
    if (failed)
      return FAIL(E_INVALIDARG);
    if (auto &config = configurations_[PipelineConfig - ShaderConfig]; config)
      pipeline_config_ = {config->first, (D3D12_RAYTRACING_PIPELINE_FLAGS)config->second};

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
      // without callable recursion, DXR's ray recursion limit bounds every native call chain, regardless of spare
      // bytes requested by SetPipelineStackSize (DXR spec, "Ray recursion limit"; Metal maxCallStackDepth). DXR
      // bounds no chain of callable shaders, and leaves the stack they need to the application: a size it sums
      // from GetShaderStackSize is a call depth, but one it took from elsewhere, a count of bytes, is not, and would
      // ask Metal for a stack that deep. the pipeline's depth stops at callable shaders nested as deep as DXR lets
      // traces be declared, the one recursion it does bound, where the default has them two deep
      max_call_depth_ =
          stack_size_ + most[SM50_RAY_SHADER_CALLABLE] * (D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH - 2);
      if (pParent)
        stack_size_ = pParent->stack_size_;
    }
    if (type_ != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
      return S_OK;
    // a state object that is made can dispatch rays: its pipeline is compiled here, where Direct3D 12 compiles one
    // and reports what cannot be, and not in the first DispatchRays
    if (!MakePipeline(CallDepth()).pipeline)
      return FAIL(E_FAIL);
    if (device_->NamesPasses())
      device_->PipelineMade(name, "ray", start);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);
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
    return MTLD3D12DeviceChild::QueryInterface(riid, ppvObject);
  }

  void *STDMETHODCALLTYPE
  GetShaderIdentifier(const WCHAR *pExportName) {
    auto found = exports_.find(pExportName);
    return found == exports_.end() ? nullptr : found->second.identifier.get();
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

  // the pipeline of the new call depth is made here, in the application's set-up, and not in the next DispatchRays
  // (the call is "not thread-safe" against a list's ray dispatches: ID3D12StateObjectProperties::SetPipelineStackSize)
  void STDMETHODCALLTYPE
  SetPipelineStackSize(UINT64 PipelineStackSizeInBytes) {
    if (PipelineStackSizeInBytes >= 0xffffffff)
      return;
    auto start = device_->NamesPasses() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    bool known;
    {
      std::unique_lock<mullion::mutex> lock(pipeline_lock_);
      stack_size_ = PipelineStackSizeInBytes;
      known = pipelines_.count(CallDepth());
    }
    if (type_ != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE || known)
      return;
    if (MakePipeline(CallDepth()).pipeline && device_->NamesPasses())
      device_->PipelineMade(name, "ray", start);
  }

  bool
  GetPipeline(WMT::ComputePipelineState &Pipeline, WMT::VisibleFunctionTable &Table, uint32_t &Flags) {
    Flags = pipeline_config_.Flags;
    auto made = MakePipeline(CallDepth());
    Pipeline = made.pipeline;
    Table = made.table;
    return made.pipeline;
  }

  // the pipeline of a call depth, compiled the first time it is asked for
  Pipeline
  MakePipeline(uint32_t depth) {
    std::unique_lock<mullion::mutex> lock(pipeline_lock_);
    auto [entry, first] = pipelines_.try_emplace(depth);
    auto &made = entry->second;
    if (first) {
      auto library = device_->GetLib().getLibrary();
      // the table has each function at its slot; the pipeline links them all
      std::vector<obj_handle_t> slots(SM50_RAY_FUNCTION_FIRST_SHADER), linked;
      std::vector<std::pair<uint32_t, const RayFunction *>> shaders;
      for (auto &[_, e] : exports_)
        for (unsigned i = 0; i < std::size(e.functions); i++)
          if (auto &function = e.functions[i]) {
            slots.resize(std::max<size_t>(slots.size(), function->slot + 1));
            slots[function->slot] = function->function;
            shaders.push_back({e.kinds[i], function.get()});
          }
      // the kinds of shaders the pipeline has, for its traces
      bool any_hit = false, intersection = false;
      for (auto [kind, _] : shaders) {
        any_hit |= kind == SM50_RAY_SHADER_ANY_HIT;
        intersection |= kind == SM50_RAY_SHADER_INTERSECTION;
      }
      // with the dispatchers, or without them where Metal cannot make the pipeline
      for (unsigned attempt = SpecializedRays() ? 0 : 1; attempt < 2; attempt++) {
        bool direct = !attempt;
        WMT::Reference<WMT::Error> err;
        std::vector<WMT::Reference<WMT::Function>> dispatching;
        std::string dispatchers_error;
        if (direct && !RayDispatchers(device_, shaders, dispatching, dispatchers_error)) {
          ERR("Failed to make a ray tracing pipeline's dispatchers: ", dispatchers_error);
          continue;
        }
        WMTFunctionConstant constants[] = {
            {{&direct}, WMTDataTypeBool, kRayFCIndex_DirectCalls},
            {{&any_hit}, WMTDataTypeBool, kRayFCIndex_AnyHitShaders},
            {{&intersection}, WMTDataTypeBool, kRayFCIndex_IntersectionShaders},
        };
        auto function = [&](const char *name) {
          return library.newFunctionWithConstants(name, constants, SpecializedRays() ? std::size(constants) : 1, err);
        };
        WMT::Reference<WMT::Function> runtime[] = {
            function("mullion_ray_trace"), function("mullion_ray_report_hit"), function("mullion_ray_call")
        };
        slots[SM50_RAY_FUNCTION_TRACE] = runtime[0];
        slots[SM50_RAY_FUNCTION_REPORT_HIT] = runtime[1];
        slots[SM50_RAY_FUNCTION_CALL] = runtime[2];
        linked.clear();
        for (auto function : slots)
          if (function)
            linked.push_back(function);
        for (auto &dispatcher : dispatching)
          linked.push_back(dispatcher);
        auto kernel = function("dispatch_rays");
        WMTLinkedComputePipelineInfo info{};
        info.compute_function = kernel;
        info.functions.set(linked.data());
        info.function_count = linked.size();
        info.max_call_stack_depth = depth;
        made.pipeline = device_->GetMTLDevice().newLinkedComputePipelineState(info, err);
        if (made.pipeline) {
          made.table = made.pipeline.newVisibleFunctionTable(slots.data(), slots.size());
          break;
        }
        ERR("Failed to create a ray tracing pipeline", direct ? " that calls its shaders directly" : "", ": ",
            err ? err.description().getUTF8String() : "");
      }
    }
    return made;
  }
};

HRESULT
CreateStateObject(
    MTLD3D12Device *pDevice, const D3D12_STATE_OBJECT_DESC *pDesc, ID3D12StateObject *pParent, REFIID riid,
    void **ppStateObject
) {
  InitReturnPtr(ppStateObject);
  if (!pDesc)
    return FAIL(E_INVALIDARG);
  auto state = Com(new MTLD3D12StateObjectImpl(pDevice));
  HRESULT hr = state->Initialize(pDesc, static_cast<MTLD3D12StateObjectImpl *>(pParent));
  if (FAILED(hr))
    return hr;
  return state->QueryInterface(riid, ppStateObject);
}

} // namespace mullion
