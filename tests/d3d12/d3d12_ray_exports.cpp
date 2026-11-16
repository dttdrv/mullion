// contract: imported and inherited shader identifiers keep their address, bytes and lifetime, and state objects
// retain root signatures without changing their public reference count. "State object holds a reference."
// (DXR functional spec, D3D12_GLOBAL_ROOT_SIGNATURE and D3D12_LOCAL_ROOT_SIGNATURE); "The data referenced by this
// pointer is valid as long as the state object it came from is valid." (GetShaderIdentifier, Microsoft Learn).
// vkd3d-proton's test_raytracing_collection_handle_invariance and test_raytracing witness pointer identity and
// private retention on Windows. generated aliases and hit groups exercise export selection, renaming, map growth,
// additions and destruction in both orders. a fence attached to each root signature observes its final release.
// failed creation releases them too: "E_INVALIDARG if one of the input parameters is invalid"
// (ID3D12Device5::CreateStateObject, Microsoft Learn); a library with a flipped validator hash is invalid (INF-0004).
// "If the shader isn’t fully resolved in the state object, the return value is nullptr." (DXR spec,
// GetShaderIdentifier, https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#getshaderidentifier).
// resolved exports compile even when others need an external function, root binding or subobject. imported and
// inherited ray generation, miss and callable identifiers execute, writing their payload to a readback buffer.
// the original and added objects "must both opt-in" (DXR spec, AddToStateObject).
// additions are executable state objects. oversized stack requests also execute, with and without a callable
// shader in the pipeline: "Stack size in bytes" (DXR spec, SetPipelineStackSize).
#include "d3d12_test.hpp"
#include <array>

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> results : register(u0);
RWStructuredBuffer<uint> external_result : register(u1);
struct Payload { uint value; };
void traced(inout Payload p) {
  RayDesc ray = { float3(0, 0, 0), 0, float3(0, 0, 1), 1 };
  TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 0, DispatchRaysIndex().x, ray, p);
}
[shader("raygeneration")] void generate() {
  Payload p = { DispatchRaysIndex().x }; traced(p); CallShader(0, p); results[DispatchRaysIndex().x] = p.value;
}
[shader("raygeneration")] void trace_generate() {
  Payload p = { DispatchRaysIndex().x }; traced(p); results[DispatchRaysIndex().x] = p.value;
}
[shader("miss")] void missed(inout Payload p) { p.value += 1; }
uint external_value(uint value);
[shader("miss")] void needs_function(inout Payload p) { p.value += external_value(p.value); }
[shader("miss")] void needs_root(inout Payload p) { external_result[0] = p.value; }
[shader("callable")] void called(inout Payload p) { p.value += 2; }
[shader("closesthit")]
void closest(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.value += results[0] + 3; }
[shader("anyhit")]
void accepted(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.value += 4; }
[shader("intersection")]
void intersected() { BuiltInTriangleIntersectionAttributes a = { float2(0, 0) }; ReportHit(1, 0, a); }
)hlsl";

static const char definition[] = R"hlsl(
uint external_value(uint value) { return value + 1; }
GlobalRootSignature external_signature = { "UAV(u0), SRV(t0), UAV(u1)" };
RaytracingShaderConfig external_config = { PAYLOAD_BYTES, ATTRIBUTE_BYTES };
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: ray tracing libraries need DXC\n");
    return 77;
  }
  step("compile the export library");
  auto library = compiler.compile(hlsl, "", "lib_6_3");
  auto definitions = compiler.compile(
      definition, "", "lib_6_3",
      {"PAYLOAD_BYTES=" + std::to_string(sizeof(UINT)), "ATTRIBUTE_BYTES=" + std::to_string(sizeof(float) * 2)},
      {L"-default-linkage", L"external"}
  );
  if (library.empty() || definitions.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device7> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3]{{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[2].Descriptor.ShaderRegister = 1;
  D3D12_RAYTRACING_SHADER_CONFIG shader{sizeof(UINT), sizeof(float) * 2};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{1};
  {
    auto root = root_signature(device.Get(), {2, parameters});
    auto complete = root_signature(device.Get(), {(UINT)std::size(parameters), parameters});
    if (!expect(root && complete, "dependency root signatures"))
      return verdict();
    D3D12_EXPORT_DESC selected[] = {
        {L"missed"}, {L"needs_root"}, {L"needs_function"}, {L"needs_association", L"missed"},
        {L"needs_configuration", L"missed"}
    };
    D3D12_DXIL_LIBRARY_DESC code{bytecode(library), (UINT)std::size(selected), selected};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()}, supplied{complete.Get()};
    D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS};
    const WCHAR *associated = L"needs_association";
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION unresolved{L"external_signature", 1, &associated};
    const WCHAR *configured = L"needs_configuration";
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION unresolved_config{L"external_config", 1, &configured};
    D3D12_HIT_GROUP_DESC group{L"needs_group", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"closest"};
    D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &code},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &unresolved},
        {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &unresolved_config},
    };
    for (UINT missing = 0; missing < 4; missing++) {
      step("dependency variant %u: resolved and unresolved exports in one collection", missing);
      std::vector<D3D12_STATE_SUBOBJECT> contents = {
          subobjects[0], subobjects[1], subobjects[4], subobjects[5], subobjects[6], subobjects[7]
      };
      if (!(missing & 1))
        contents.push_back(subobjects[2]);
      if (!(missing & 2))
        contents.push_back(subobjects[3]);
      D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, (UINT)contents.size(), contents.data()};
      ComPtr<ID3D12StateObject> collection, linked;
      config.Flags = D3D12_STATE_OBJECT_FLAG_NONE;
      auto hr = device->CreateStateObject(&desc, IID_PPV_ARGS(&collection));
      expect(hr == E_INVALIDARG && !collection, "unresolved collection without opt-in returned %08lx", hr);
      collection.Reset();
      config.Flags = D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS;
      CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&collection)));
      ComPtr<ID3D12StateObjectProperties> properties;
      CHECK(collection.As(&properties));
      auto identifier = properties->GetShaderIdentifier(L"missed");
      expect(!!identifier == !missing, "resolved export with missing config mask %u", missing);
      expect(!properties->GetShaderIdentifier(L"needs_root"), "unbound resource has an identifier");
      expect(!properties->GetShaderIdentifier(L"needs_function"), "undefined function has an identifier");
      expect(!properties->GetShaderIdentifier(L"needs_association"), "undefined subobject has an identifier");
      expect(!properties->GetShaderIdentifier(L"needs_configuration"), "undefined config has an identifier");
      expect(!properties->GetShaderIdentifier(group.HitGroupExport), "undefined hit shader has an identifier");
      D3D12_EXISTING_COLLECTION_DESC existing{collection.Get()};
      D3D12_DXIL_LIBRARY_DESC functions{bytecode(definitions)};
      D3D12_EXPORT_DESC hit_export{L"closest"};
      D3D12_DXIL_LIBRARY_DESC hit_code{bytecode(library), 1, &hit_export};
      const WCHAR *names[] = {L"needs_root", L"closest"};
      D3D12_STATE_SUBOBJECT imports[] = {
          {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
          {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &functions},
          {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &supplied},
          {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION},
          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
          {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &hit_code},
      };
      D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&imports[2], (UINT)std::size(names), names};
      imports[3].pDesc = &association;
      desc = {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(imports), imports};
      CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&linked)));
      CHECK(linked.As(&properties));
      for (auto &e : selected)
        expect(!!properties->GetShaderIdentifier(e.Name), "linked dependency variant %u", missing);
      expect(!!properties->GetShaderIdentifier(group.HitGroupExport), "linked hit group variant %u", missing);
      if (identifier)
        expect(properties->GetShaderIdentifier(L"missed") == identifier, "resolved export was recompiled");
    }
  }
  const UINT count = 129;
  for (UINT order = 0; order < 2; order++) {
    step("variant %u: retain global and local root signatures", order);
    ComPtr<ID3D12Fence> lifetime[2];
    ComPtr<ID3D12RootSignature> roots[2];
    for (UINT i = 0; i < std::size(roots); i++) {
      D3D12_ROOT_SIGNATURE_DESC desc{};
      if (!i) {
        desc.NumParameters = 2;
        desc.pParameters = parameters;
      }
      desc.Flags = i ? D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE : D3D12_ROOT_SIGNATURE_FLAG_NONE;
      roots[i] = root_signature(device.Get(), desc);
      if (!expect(!!roots[i], "root signature %u", i))
        return verdict();
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lifetime[i])));
      CHECK(roots[i]->SetPrivateDataInterface(__uuidof(ID3D12Fence), lifetime[i].Get()));
    }
    std::vector<std::wstring> names;
    for (UINT i = 0; i < count; i++)
      names.push_back(L"miss_" + std::to_wstring(i));
    names.insert(names.end(), {L"generate", L"called"});
    std::vector<D3D12_EXPORT_DESC> exports;
    for (UINT i = 0; i < names.size(); i++)
      exports.push_back({names[i].c_str(), i < count ? L"missed" : nullptr});
    for (auto name : {L"closest", L"accepted", L"intersected"})
      exports.push_back({name});
    D3D12_DXIL_LIBRARY_DESC code{bytecode(library), (UINT)exports.size(), exports.data()};
    D3D12_GLOBAL_ROOT_SIGNATURE global{roots[0].Get()};
    D3D12_LOCAL_ROOT_SIGNATURE local{roots[1].Get()};
    auto invalid = library;
    invalid[dxbc::hash_at] ^= 1;
    D3D12_DXIL_LIBRARY_DESC bad_code{bytecode(invalid)};
    D3D12_STATE_SUBOBJECT bad_subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &bad_code},
    };
    D3D12_STATE_OBJECT_DESC bad_desc{
        D3D12_STATE_OBJECT_TYPE_COLLECTION, (UINT)std::size(bad_subobjects), bad_subobjects
    };
    ComPtr<ID3D12StateObject> rejected;
    step("variant %u: reject an invalid library after retaining roots", order);
    auto hr = device->CreateStateObject(&bad_desc, IID_PPV_ARGS(&rejected));
    expect(hr == E_INVALIDARG && !rejected, "invalid library returned %08lx, object %u", hr, !!rejected);
    D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAGS(
        D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS |
        D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS |
        (order ? D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS
               : D3D12_STATE_OBJECT_FLAG_NONE)
    )};
    std::vector<std::wstring> group_names;
    for (UINT i = 0; i < count; i++)
      group_names.push_back(L"hit_" + std::to_wstring(i));
    std::vector<D3D12_HIT_GROUP_DESC> groups;
    for (UINT i = 0; i < group_names.size(); i++)
      groups.push_back({group_names[i].c_str(),
                        i % 2 ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE : D3D12_HIT_GROUP_TYPE_TRIANGLES,
                        L"accepted", L"closest", i % 2 ? L"intersected" : nullptr});
    std::vector<D3D12_STATE_SUBOBJECT> subobjects = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &code},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
    };
    for (auto &group : groups)
      subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group});
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, (UINT)subobjects.size(), subobjects.data()};
    ComPtr<ID3D12StateObject> collection, parent, child;
    step("variant %u: compile the collection", order);
    CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&collection)));
    ComPtr<ID3D12StateObjectProperties> original, imported, inherited;
    CHECK(collection.As(&original));
    names.insert(names.end(), group_names.begin(), group_names.end());
    std::vector<void *> pointers;
    std::vector<std::array<uint8_t, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES>> bytes(names.size());
    for (UINT i = 0; i < names.size(); i++) {
      auto pointer = original->GetShaderIdentifier(names[i].c_str());
      if (!expect(!!pointer, "identifier %u", i))
        return verdict();
      pointers.push_back(pointer);
      memcpy(bytes[i].data(), pointer, bytes[i].size());
    }
    std::vector<std::wstring> renamed;
    std::vector<D3D12_EXPORT_DESC> taken;
    for (auto &name : names)
      renamed.push_back(L"import_" + name);
    for (UINT i = 0; i < names.size(); i++)
      taken.push_back({renamed[i].c_str(), names[i].c_str()});
    D3D12_EXISTING_COLLECTION_DESC existing{collection.Get(), order ? (UINT)taken.size() : 0,
                                           order ? taken.data() : nullptr};
    D3D12_STATE_SUBOBJECT from[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
    };
    desc = {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(from), from};
    step("variant %u: import %zu identifiers, %s", order, names.size(), order ? "renamed" : "all exports");
    CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&parent)));
    CHECK(parent.As(&imported));
    D3D12_EXPORT_DESC added_export{L"added_generate", L"generate"};
    D3D12_DXIL_LIBRARY_DESC added_code{bytecode(library), 1, &added_export};
    D3D12_EXPORT_DESC added_shaders[] = {{L"added_closest", L"closest"}, {L"added_accepted", L"accepted"}};
    D3D12_EXISTING_COLLECTION_DESC added_collection{collection.Get(), (UINT)std::size(added_shaders), added_shaders};
    D3D12_HIT_GROUP_DESC added_group{L"added_hit", D3D12_HIT_GROUP_TYPE_TRIANGLES,
                                    L"added_accepted", L"added_closest"};
    D3D12_STATE_SUBOBJECT addition[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &added_code},
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &added_collection},
        {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &added_group},
        {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION},
    };
    const WCHAR *generated_name = added_export.Name;
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION added_association{&addition[4], 1, &generated_name};
    addition[std::size(addition) - 1].pDesc = &added_association;
    desc = {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(addition), addition};
    step("variant %u: reject additions without opt-in or with a collection type", order);
    auto flags = config.Flags;
    config.Flags = D3D12_STATE_OBJECT_FLAG_NONE;
    hr = device->AddToStateObject(&desc, parent.Get(), IID_PPV_ARGS(&rejected));
    expect(hr == E_INVALIDARG && !rejected, "addition without opt-in returned %08lx", hr);
    rejected.Reset();
    config.Flags = flags;
    desc.Type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
    hr = device->AddToStateObject(&desc, parent.Get(), IID_PPV_ARGS(&rejected));
    expect(hr == E_INVALIDARG && !rejected, "collection addition returned %08lx", hr);
    rejected.Reset();
    desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    hr = device->AddToStateObject(&desc, collection.Get(), IID_PPV_ARGS(&rejected));
    expect(hr == E_INVALIDARG && !rejected, "collection parent returned %08lx", hr);
    rejected.Reset();
    ComPtr<ID3D12StateObject> no_additions;
    D3D12_STATE_OBJECT_DESC parent_desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(from), from};
    config.Flags = D3D12_STATE_OBJECT_FLAG_NONE;
    CHECK(device->CreateStateObject(&parent_desc, IID_PPV_ARGS(&no_additions)));
    config.Flags = flags;
    hr = device->AddToStateObject(&desc, no_additions.Get(), IID_PPV_ARGS(&rejected));
    expect(hr == E_INVALIDARG && !rejected, "parent without opt-in returned %08lx", hr);
    rejected.Reset();
    no_additions.Reset();
    CHECK(device->AddToStateObject(&desc, parent.Get(), IID_PPV_ARGS(&child)));
    CHECK(child.As(&inherited));
    expect(!!inherited->GetShaderIdentifier(added_group.HitGroupExport), "addition has no hit group identifier");
    std::vector<D3D12_EXPORT_DESC> misses;
    for (UINT i = 0; i < count; i++)
      misses.push_back({names[i].c_str()});
    D3D12_EXISTING_COLLECTION_DESC only_misses{collection.Get(), (UINT)misses.size(), misses.data()};
    D3D12_EXPORT_DESC trace_export{L"trace_generate"};
    D3D12_DXIL_LIBRARY_DESC trace_code{bytecode(library), 1, &trace_export};
    D3D12_STATE_SUBOBJECT tracing[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &only_misses},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &trace_code},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
    };
    desc = {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(tracing), tracing};
    ComPtr<ID3D12StateObject> trace;
    ComPtr<ID3D12StateObjectProperties> trace_properties;
    CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&trace)));
    CHECK(trace.As(&trace_properties));
    auto default_stack = trace_properties->GetPipelineStackSize();
    trace_properties->SetPipelineStackSize(256); // vkd3d-proton test_raytracing's byte-size request
    expect(trace_properties->GetPipelineStackSize() == 256, "oversized stack size was not retained");
    auto bound_root = roots[0].Get();
    for (auto &root : roots) {
      auto refs = root.Detach()->Release();
      expect(refs == 0, "root has %lu public references after release", refs);
    }
    expect(!!inherited->GetShaderIdentifier(added_export.Name), "addition has no new identifier");
    expect(!imported->GetShaderIdentifier(L"not_exported"), "unknown export has an identifier");
    for (UINT i = 0; i < names.size(); i++) {
      auto name = (order ? renamed[i] : names[i]).c_str();
      expect(imported->GetShaderIdentifier(name) == pointers[i], "import changed identifier %u's address", i);
      expect(inherited->GetShaderIdentifier(name) == pointers[i], "addition changed identifier %u's address", i);
    }
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    for (UINT use = 0; use < 6; use++) {
      if (use == 2) {
        step("variant %u: release source objects before their last user", order);
        original.Reset();
        imported.Reset();
        if (order) {
          parent.Reset();
          collection.Reset();
        } else {
          collection.Reset();
          parent.Reset();
        }
      }
      step("variant %u: dispatch identifier variant %u", order, use);
      bool plain = use >= 4;
      auto properties = plain ? trace_properties.Get() : use ? inherited.Get() : imported.Get();
      auto object = plain ? trace.Get() : use ? child.Get() : parent.Get();
      auto exported_name = [&](const std::wstring &name) { return !plain && order ? L"import_" + name : name; };
      auto generated = plain ? L"trace_generate" : use == 3 ? L"added_generate" : exported_name(L"generate");
      if (use == 5)
        trace_properties->SetPipelineStackSize(default_stack);
      // a stack in bytes that no sum of the shaders' stack sizes gives, where a callable shader could use it
      if (use == 1) {
        properties->SetPipelineStackSize(64 * 1024);
        expect(properties->GetPipelineStackSize() == 64 * 1024, "a stack size in bytes was not retained");
      }
      const UINT stride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
      std::vector<uint8_t> table((count + 2) * stride);
      auto record = [&](UINT index, const std::wstring &name) {
        auto pointer = properties->GetShaderIdentifier(name.c_str());
        if (expect(!!pointer, "dispatch identifier %u", index))
          memcpy(table.data() + index * stride, pointer, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      };
      record(0, generated);
      for (UINT i = 0; i < count; i++)
        record(i + 1, exported_name(names[i]));
      if (!plain)
        record(count + 1, exported_name(L"called"));
      auto records = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, table.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * sizeof(UINT),
                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, count * sizeof(UINT),
                             D3D12_RESOURCE_STATE_COPY_DEST);
      auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, count * sizeof(UINT),
                            D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!expect(records && output && readback && initial, "dispatch buffers"))
        return verdict();
      void *mapped;
      CHECK(records->Map(0, nullptr, &mapped));
      memcpy(mapped, table.data(), table.size());
      records->Unmap(0, nullptr);
      CHECK(initial->Map(0, nullptr, &mapped));
      memset(mapped, 0xff, output->GetDesc().Width);
      initial->Unmap(0, nullptr);
      CHECK(forget(readback.Get()));
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList4> list;
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(
          0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)
      ));
      list->CopyResource(output.Get(), initial.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetComputeRootSignature(bound_root);
      list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      list->SetComputeRootShaderResourceView(1, 0);
      list->SetPipelineState1(object);
      D3D12_DISPATCH_RAYS_DESC rays{};
      auto address = records->GetGPUVirtualAddress();
      rays.RayGenerationShaderRecord = {address, stride};
      rays.MissShaderTable = {address + stride, count * stride, stride};
      if (!plain)
        rays.CallableShaderTable = {address + (count + 1) * stride, stride, stride};
      rays.Width = count;
      rays.Height = rays.Depth = 1;
      list->DispatchRays(&rays);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      for (UINT i = 0; i < count; i++)
        expect(((UINT *)mapped)[i] == i + 1 + (plain ? 0 : 2), "ray %u gave %u", i, ((UINT *)mapped)[i]);
      readback->Unmap(0, nullptr);
    }
    queue.Reset();
    for (UINT i = 0; i < names.size(); i++) {
      auto pointer = inherited->GetShaderIdentifier((order ? renamed[i] : names[i]).c_str());
      expect(pointer && !memcmp(pointer, bytes[i].data(), bytes[i].size()), "identifier %u's bytes changed", i);
    }
    for (auto &fence : lifetime) {
      fence->AddRef();
      auto refs = fence->Release();
      expect(refs == 2, "root signature's private data has %lu references before the last user is released", refs);
    }
    inherited.Reset();
    child.Reset();
    trace_properties.Reset();
    trace.Reset();
    for (auto &fence : lifetime) {
      fence->AddRef();
      auto refs = fence->Release();
      expect(refs == 1, "root signature's private data has %lu references after the last user is released", refs);
    }
  }
  return verdict();
}
