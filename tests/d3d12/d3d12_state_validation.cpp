// contract: each raytracing export has matching shader and pipeline configs and root bindings for its resources.
// "Required & matching for all exports" (DXR functional spec, "Subobject association requirements", shader and
// pipeline config rows); "must define all resource bindings declared by the shader" (D3D12_GLOBAL_ROOT_SIGNATURE).
// default candidates with different definitions leave no association; explicit associations select a definition
// (vkd3d-proton's Windows witnesses: test_raytracing_reject_duplicate_objects and
// test_raytracing_object_assignment_ignore_default). definitions with identical content are not conflicts.
// AddToStateObject: "The state object description must be fully self-contained" and "must both opt-in".
// the default stack is computed from the shader stack sizes and MaxTraceRecursionDepth ("Default pipeline stack size").
#include "d3d12_test.hpp"
#include <algorithm>
#include <iterator>

static const char hlsl[] = R"hlsl(
#if EMBEDDED
RaytracingShaderConfig shader_config = { PAYLOAD, ATTRIBUTE };
RaytracingPipelineConfig pipeline_config = { DEPTH };
RaytracingShaderConfig shader_other = { PAYLOAD + PAYLOAD, ATTRIBUTE };
RaytracingPipelineConfig pipeline_other = { DEPTH + 1 };
#endif
[shader("raygeneration")] void empty() {}
struct Payload { uint value; };
[shader("miss")] void missed(inout Payload p) { p.value = 1; }
#if !EMBEDDED
RWByteAddressBuffer destination : register(u0);
[shader("raygeneration")] void bound() { destination.Store(0, 1); }
[shader("closesthit")]
void closest(inout Payload p, in BuiltInTriangleIntersectionAttributes attributes) { p.value = 1; }
[shader("closesthit")]
void closest_bound(inout Payload p, in BuiltInTriangleIntersectionAttributes attributes) {
  destination.Store(0, p.value);
}
[shader("anyhit")]
void any_bound(inout Payload p, in BuiltInTriangleIntersectionAttributes attributes) { destination.Store(0, p.value); }
[shader("intersection")]
void intersection_bound() {
  destination.Store(0, 1);
  BuiltInTriangleIntersectionAttributes attributes = { float2(0, 0) };
  ReportHit(1, 0, attributes);
}
#endif
)hlsl";

int main(int argc, char **argv)
{
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: DXR libraries require DXC\n");
    return 77;
  }
  const UINT payload = sizeof(UINT), attribute = 2 * sizeof(float), depth = 1;
  auto library = compiler.compile(hlsl, "", "lib_6_3", {"EMBEDDED=0"});
  auto embedded = compiler.compile(hlsl, "", "lib_6_3",
                                   {"EMBEDDED=1", "PAYLOAD=" + std::to_string(payload),
                                    "ATTRIBUTE=" + std::to_string(attribute), "DEPTH=" + std::to_string(depth)});
  if (library.empty() || embedded.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
    printf("skipped: raytracing tier 1.1 is not supported\n");
    return 77;
  }
  D3D12_EXPORT_DESC empty{L"empty"}, bound{L"bound"};
  D3D12_DXIL_LIBRARY_DESC lib{bytecode(library), 1, &empty};
  D3D12_RAYTRACING_SHADER_CONFIG shader{payload, attribute}, shader_other{payload + sizeof(UINT), attribute},
      attribute_other{payload, attribute + sizeof(float)};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{depth}, pipeline_other{depth + 1};
  D3D12_RAYTRACING_PIPELINE_CONFIG1 flags_other{depth, D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES};
  auto create = [&](D3D12_STATE_OBJECT_TYPE type, const std::vector<D3D12_STATE_SUBOBJECT> &subobjects, HRESULT want,
                    ComPtr<ID3D12StateObject> *keep = nullptr) {
    D3D12_STATE_OBJECT_DESC desc{type, (UINT)subobjects.size(), subobjects.data()};
    ComPtr<ID3D12StateObject> state;
    HRESULT hr = device->CreateStateObject(&desc, IID_PPV_ARGS(&state));
    expect(hr == want && !!state == SUCCEEDED(want), "CreateStateObject: %08lx, expected %08lx", hr, want);
    if (keep)
      *keep = state;
  };
  const D3D12_STATE_SUBOBJECT configs[] = {{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
                                           {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline}};
  const D3D12_STATE_SUBOBJECT alternatives[][2] = {
      {{configs[0].Type, &shader_other}, {configs[1].Type, &pipeline_other}},
      {{configs[0].Type, &attribute_other}, {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &flags_other}}};
  D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline1{depth, D3D12_RAYTRACING_PIPELINE_FLAG_NONE};
  step("matching pipeline config and config1 definitions");
  create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
         {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
          configs[0],
          configs[1],
          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &pipeline1}},
         S_OK);
  D3D12_EXPORT_DESC closest{L"closest"};
  lib.pExports = &closest;
  step("an ungrouped closest hit export still needs configurations");
  create(D3D12_STATE_OBJECT_TYPE_COLLECTION, {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}}, E_INVALIDARG);
  step("an ungrouped closest hit export with configurations");
  ComPtr<ID3D12StateObject> hit_collection;
  create(D3D12_STATE_OBJECT_TYPE_COLLECTION, {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}, configs[0], configs[1]},
         S_OK, &hit_collection);
  if (hit_collection) {
    D3D12_EXISTING_COLLECTION_DESC existing{hit_collection.Get()};
    step("an imported ungrouped closest hit keeps its validated configurations");
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing}},
           S_OK);
  }
  D3D12_HIT_GROUP_DESC group{L"group", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, closest.Name};
  const wchar_t *group_name = group.HitGroupExport;
  std::vector<D3D12_STATE_SUBOBJECT> grouped{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                                             {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group},
                                             configs[0],
                                             configs[1],
                                             {},
                                             {}};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION group_associations[] = {{&grouped[2], 1, &group_name},
                                                                 {&grouped[3], 1, &group_name}};
  for (UINT i = 0; i < std::size(group_associations); i++)
    grouped[grouped.size() - std::size(group_associations) + i] = {
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &group_associations[i]};
  step("configurations explicitly associated with a hit group cover its component shader");
  create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, grouped, S_OK);
  auto stack = [&](ID3D12StateObject *state, UINT recursion) {
    ComPtr<ID3D12StateObjectProperties> properties;
    if (!expect(SUCCEEDED(state->QueryInterface(IID_PPV_ARGS(&properties))), "state object properties"))
      return;
    UINT64 want = properties->GetShaderStackSize(empty.Name) + properties->GetShaderStackSize(L"missed") * recursion;
    expect(properties->GetPipelineStackSize() == want, "pipeline stack %llu, expected %llu for recursion %u",
           (unsigned long long)properties->GetPipelineStackSize(), (unsigned long long)want, recursion);
  };
  D3D12_EXPORT_DESC stack_names[] = {empty, {L"missed"}};
  D3D12_DXIL_LIBRARY_DESC stack_lib{bytecode(library), (UINT)std::size(stack_names), stack_names};
  for (UINT recursion : {0u, depth, depth + 1, (UINT)D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH})
    for (bool config1 : {false, true}) {
      step("pipeline stack, recursion %u, config1 %u", recursion, config1);
      D3D12_RAYTRACING_PIPELINE_CONFIG selected{recursion};
      D3D12_RAYTRACING_PIPELINE_CONFIG1 selected1{recursion, D3D12_RAYTRACING_PIPELINE_FLAG_NONE};
      ComPtr<ID3D12StateObject> state;
      create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
             {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &stack_lib}, configs[0],
              {config1 ? D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1
                       : D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
               config1 ? (const void *)&selected1 : &selected}},
             S_OK, &state);
      if (state)
        stack(state.Get(), recursion);
    }
  lib.pExports = &empty;
  for (auto type : {D3D12_STATE_OBJECT_TYPE_COLLECTION, D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE}) {
    for (UINT omitted = 0; omitted <= std::size(configs); omitted++) {
      step("type %u, missing configuration %u", type, omitted);
      std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}};
      for (UINT i = 0; i < std::size(configs); i++)
        if (i != omitted)
          objects.push_back(configs[i]);
      create(type, objects, omitted == std::size(configs) ? S_OK : E_INVALIDARG);
    }
    for (UINT field = 0; field < std::size(alternatives); field++)
      for (UINT kind = 0; kind < std::size(configs); kind++)
        for (UINT count = 1; count <= std::size(configs) + 1; count++)
          for (bool conflict : {false, true})
            for (UINT selection = 0; selection < 3; selection++) {
              step("type %u, config %u, field %u, count %u, conflict %u, selection %u", type, kind, field, count,
                   conflict, selection);
              std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                                                         configs[1 - kind]};
              for (UINT i = 0; i < count; i++)
                objects.push_back(i && conflict ? alternatives[field][kind] : configs[kind]);
              const wchar_t *name = empty.Name;
              D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&objects[2], selection == 2 ? 1u : 0u, &name};
              if (selection)
                objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association});
              association.pSubobjectToAssociate = &objects[2];
              create(type, objects, count > 1 && conflict && !selection ? E_INVALIDARG : S_OK);
            }
  }
  for (UINT kind = 0; kind < std::size(configs); kind++)
    for (bool conflict : {false, true}) {
      step("two explicit associations, config %u, conflict %u", kind, conflict);
      const wchar_t *name = empty.Name;
      std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                                                 configs[1 - kind],
                                                 configs[kind],
                                                 conflict ? alternatives[0][kind] : configs[kind],
                                                 {},
                                                 {}};
      D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION associations[] = {{&objects[2], 1, &name}, {&objects[3], 1, &name}};
      for (UINT i = 0; i < std::size(associations); i++)
        objects[objects.size() - std::size(associations) + i] = {
            D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &associations[i]};
      create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, objects, conflict ? E_INVALIDARG : S_OK);
    }
  D3D12_EXPORT_DESC aliases[] = {empty, {L"added", empty.Name}};
  D3D12_DXIL_LIBRARY_DESC aliased{bytecode(library), (UINT)std::size(aliases), aliases};
  for (UINT kind = 0; kind < std::size(configs); kind++)
    for (bool conflict : {false, true}) {
      step("config %u explicitly assigned across exports, conflict %u", kind, conflict);
      std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &aliased},
                                                 configs[1 - kind],
                                                 configs[kind],
                                                 conflict ? alternatives[0][kind] : configs[kind],
                                                 {},
                                                 {}};
      D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION associations[] = {{&objects[2], 1, &aliases[0].Name},
                                                               {&objects[3], 1, &aliases[1].Name}};
      for (UINT i = 0; i < std::size(associations); i++)
        objects[objects.size() - std::size(associations) + i] = {
            D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &associations[i]};
      create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, objects, conflict ? E_INVALIDARG : S_OK);
    }

  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_UAV};
  for (bool local : {false, true}) {
    D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter, 0, nullptr,
                                        local ? D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE
                                              : D3D12_ROOT_SIGNATURE_FLAG_NONE};
    parameter.Descriptor.ShaderRegister = 0;
    auto first = root_signature(device.Get(), root_desc), identical = root_signature(device.Get(), root_desc);
    parameter.Descriptor.ShaderRegister = 1;
    auto other = root_signature(device.Get(), root_desc);
    D3D12_ROOT_PARAMETER parameters[] = {parameter, parameter};
    parameters[1].Descriptor.ShaderRegister = 0;
    root_desc.NumParameters = std::size(parameters);
    root_desc.pParameters = parameters;
    auto wider = root_signature(device.Get(), root_desc);
    if (!expect(first && identical && other && wider, "root signatures were not created"))
      return verdict();
    D3D12_EXPORT_DESC hit_names[] = {{L"closest_bound"}, {L"any_bound"}, {L"intersection_bound"}};
    for (auto &hit : hit_names)
      for (auto type : {D3D12_STATE_OBJECT_TYPE_COLLECTION, D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE})
        for (auto signature : {(ID3D12RootSignature *)nullptr, first.Get(), other.Get()}) {
          step("ungrouped %ls, type %u, root local %u, binding %u", hit.Name, type, local,
               signature ? signature == first.Get() ? 1u : 2u : 0u);
          lib.pExports = &hit;
          std::vector<D3D12_STATE_SUBOBJECT> objects{
              {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}, configs[0], configs[1]};
          D3D12_GLOBAL_ROOT_SIGNATURE definition{signature};
          if (signature)
            objects.push_back({local ? D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE
                                    : D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
                               &definition});
          create(type, objects, signature == first.Get() ? S_OK : E_INVALIDARG);
        }
    lib.pExports = &bound;
    D3D12_GLOBAL_ROOT_SIGNATURE wider_definition{wider.Get()};
    step("root local %u, a wider definition still binds u0", local);
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
           {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
            configs[0],
            configs[1],
            {local ? D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE : D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
             &wider_definition}},
           S_OK);
    UINT root_variant = 0;
    for (auto different : {other.Get(), wider.Get()}) {
      D3D12_GLOBAL_ROOT_SIGNATURE roots[] = {{first.Get()}, {identical.Get()}, {different}};
      for (UINT count = 0; count <= std::size(roots); count++)
        for (bool conflict : {false, true})
          for (bool reversed : {false, true})
            for (UINT selection = 0; selection < 3; selection++) {
              if (!count && selection)
                continue;
              step("root local %u, variant %u, count %u, conflict %u, reversed %u, selection %u", local, root_variant,
                   count, conflict, reversed, selection);
              std::vector<D3D12_STATE_SUBOBJECT> objects{
                  {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}, configs[0], configs[1]};
              for (UINT i = 0; i < count; i++)
                objects.push_back({local ? D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE
                                         : D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
                                   &roots[i && conflict ? 2 : i % 2]});
              if (reversed)
                std::reverse(objects.begin() + 3, objects.end());
              const wchar_t *name = bound.Name;
              D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{
                  count ? &objects[reversed ? objects.size() - 1 : 3] : nullptr, selection == 2 ? 1u : 0u, &name};
              UINT selected = reversed ? objects.size() - 1 : 3;
              if (selection) {
                objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association});
                association.pSubobjectToAssociate = &objects[selected];
              }
              create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, objects,
                     !count || (count > 1 && conflict && !selection) ? E_INVALIDARG : S_OK);
            }
      root_variant++;
    }
  }

  D3D12_EXPORT_DESC names[] = {{L"empty"}, {L"shader_config"}, {L"pipeline_config"}};
  D3D12_DXIL_LIBRARY_DESC embedded_lib{bytecode(embedded), (UINT)std::size(names), names};
  D3D12_EXPORT_DESC embedded_stack_names[] = {empty, {L"missed"}, names[1], names[2]};
  D3D12_DXIL_LIBRARY_DESC embedded_stack{
      bytecode(embedded), (UINT)std::size(embedded_stack_names), embedded_stack_names};
  ComPtr<ID3D12StateObject> embedded_pipeline;
  step("pipeline stack from embedded configurations");
  create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
         {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &embedded_stack}}, S_OK, &embedded_pipeline);
  if (embedded_pipeline)
    stack(embedded_pipeline.Get(), depth);
  for (UINT exported = 0; exported <= std::size(names); exported++) {
    step("embedded definitions, export count %u", exported);
    embedded_lib.NumExports = exported;
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &embedded_lib}},
           exported == std::size(names) ? S_OK : E_INVALIDARG);
  }
  embedded_lib.NumExports = 0;
  const wchar_t *name = empty.Name;
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION shader_named{names[1].Name, 1, &name},
      pipeline_named{names[2].Name, 1, &name};
  std::vector<D3D12_EXPORT_DESC> definitions{empty, names[1], names[2], {L"shader_other"}, {L"pipeline_other"}};
  embedded_lib.NumExports = definitions.size();
  embedded_lib.pExports = definitions.data();
  for (bool explicit_selection : {false, true}) {
    step("embedded conflicting defaults, explicit selection %u", explicit_selection);
    std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &embedded_lib}};
    if (explicit_selection) {
      objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &shader_named});
      objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &pipeline_named});
    }
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, objects, explicit_selection ? S_OK : E_INVALIDARG);
  }

  lib.pExports = &empty;
  ComPtr<ID3D12StateObject> collection;
  embedded_lib.NumExports = std::size(names);
  embedded_lib.pExports = names;
  step("a compiled collection retains its embedded shader and pipeline configurations");
  create(D3D12_STATE_OBJECT_TYPE_COLLECTION, {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &embedded_lib}}, S_OK,
         &collection);
  if (collection) {
    D3D12_EXISTING_COLLECTION_DESC existing{collection.Get()};
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
           {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing}, configs[0], configs[1]}, S_OK);
  }
  collection.Reset();
  create(D3D12_STATE_OBJECT_TYPE_COLLECTION, {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}, configs[0], configs[1]},
         S_OK, &collection);
  if (collection) {
    D3D12_EXPORT_DESC added{L"added", empty.Name};
    lib.pExports = &added;
    D3D12_EXISTING_COLLECTION_DESC existing{collection.Get()};
    for (UINT kind = 0; kind < std::size(configs); kind++)
      for (bool conflict : {false, true}) {
        step("compiled collection, config %u, conflict %u", kind, conflict);
        create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
               {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
                {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                configs[1 - kind],
                conflict ? alternatives[0][kind] : configs[kind]},
               conflict ? E_INVALIDARG : S_OK);
      }
  }
  lib.pExports = &empty;
  D3D12_STATE_OBJECT_CONFIG deferred{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS};
  collection.Reset();
  create(D3D12_STATE_OBJECT_TYPE_COLLECTION,
         {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib}, {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred}},
         S_OK, &collection);
  if (collection) {
    D3D12_EXISTING_COLLECTION_DESC existing{collection.Get()};
    step("deferred collection resolves configurations in its pipeline");
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
           {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing}, configs[0], configs[1]}, S_OK);
    step("deferred collection still needs configurations in its pipeline");
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing}},
           E_INVALIDARG);
  }
  D3D12_STATE_OBJECT_CONFIG additions{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
  ComPtr<ID3D12Device7> device7;
  CHECK(device.As(&device7));
  for (bool parent_enabled : {false, true}) {
    ComPtr<ID3D12StateObject> parent;
    step("a pipeline with embedded configurations, additions enabled %u", parent_enabled);
    std::vector<D3D12_STATE_SUBOBJECT> parent_objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &embedded_lib}};
    if (parent_enabled)
      parent_objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions});
    create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, parent_objects, S_OK, &parent);
    if (!parent)
      continue;
    D3D12_EXPORT_DESC added{L"added", empty.Name};
    lib.pExports = &added;
    for (UINT kind = 0; kind < std::size(configs); kind++)
      for (bool conflict : {false, true})
        for (bool addition_enabled : {false, true}) {
          step("AddToStateObject, parent %u, addition %u, config %u, conflict %u", parent_enabled, addition_enabled,
               kind, conflict);
          std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                                                     configs[1 - kind],
                                                     conflict ? alternatives[0][kind] : configs[kind]};
          if (addition_enabled)
            objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions});
          D3D12_STATE_OBJECT_DESC desc{
              D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)objects.size(), objects.data()};
          ComPtr<ID3D12StateObject> grown;
          HRESULT hr = device7->AddToStateObject(&desc, parent.Get(), IID_PPV_ARGS(&grown));
          HRESULT want = !parent_enabled || !addition_enabled || conflict ? E_INVALIDARG : S_OK;
          expect(hr == want && !!grown == SUCCEEDED(want), "AddToStateObject: %08lx, expected %08lx", hr, want);
          if (grown) {
            ComPtr<ID3D12StateObjectProperties> before, after;
            CHECK(parent.As(&before));
            CHECK(grown.As(&after));
            const void *first = before->GetShaderIdentifier(empty.Name),
                       *second = after->GetShaderIdentifier(empty.Name);
            expect(first && second && !memcmp(first, second, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) &&
                       !before->GetShaderIdentifier(added.Name) && after->GetShaderIdentifier(added.Name),
                   "the addition must keep old identifiers and export its new shader");
            expect(after->GetPipelineStackSize() == before->GetPipelineStackSize(),
                   "the addition keeps the stack size");
          }
        }
    if (!parent_enabled)
      continue;
    for (UINT defined = 0; defined < (1u << std::size(configs)); defined++) {
      step("AddToStateObject, named configuration mask in addition %u", defined);
      std::vector<D3D12_EXPORT_DESC> added_names{added};
      for (UINT i = 0; i < std::size(configs); i++)
        if (defined & (1u << i))
          added_names.push_back(names[i + 1]);
      D3D12_DXIL_LIBRARY_DESC added_lib{bytecode(embedded), (UINT)added_names.size(), added_names.data()};
      D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION associations[] = {{names[1].Name, 1, &added.Name},
                                                                   {names[2].Name, 1, &added.Name}};
      std::vector<D3D12_STATE_SUBOBJECT> objects{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &added_lib},
                                                 {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions}};
      for (auto &association : associations)
        objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association});
      HRESULT want = added_names.size() == std::size(names) ? S_OK : E_INVALIDARG;
      create(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, objects, want);
      D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)objects.size(), objects.data()};
      ComPtr<ID3D12StateObject> grown;
      HRESULT hr = device7->AddToStateObject(&desc, parent.Get(), IID_PPV_ARGS(&grown));
      expect(hr == want && !!grown == SUCCEEDED(want), "AddToStateObject: %08lx, expected %08lx", hr, want);
    }
  }
  return verdict();
}
