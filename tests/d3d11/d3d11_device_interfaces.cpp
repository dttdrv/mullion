// contract: D3D11-created devices reject D3D10 device interfaces until a context-state object is created;
// D3D10-created devices expose them immediately. children still expose their D3D10 interfaces, whose GetDevice
// returns null while the device interface is unavailable. Wine's Windows witnesses: test_device_interfaces,
// test_create_buffer, test_texture{1,2,3}d_interfaces and test_device_context_state in dlls/d3d11/tests/d3d11.c.
// Microsoft Learn, IUnknown::QueryInterface: "This method returns S_OK if the interface is supported, and
// E_NOINTERFACE otherwise." ID3D10DeviceChild::GetDevice: "Get a pointer to the device that created this interface."
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-queryinterface(refiid_void)
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10devicechild-getdevice
// ID3D11Device1::CreateDeviceContextState, FeatureLevels: "you must set FeatureLevels to greater than 0";
// invalid calls clear both outputs and leave availability alone. valid creation enables it for every emulated
// IID, before swapping and after release (Wine's test_device_context_state), at every supported feature level.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-createdevicecontextstate
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_1.h>

static void
check_interfaces(ID3D11Device *device, const std::vector<ComPtr<ID3D11DeviceChild>> &children, bool available) {
  ComPtr<IUnknown> identity;
  ComPtr<IDXGIDevice> dxgi;
  if (!expect(device->QueryInterface(IID_PPV_ARGS(&identity)) == S_OK &&
                  device->QueryInterface(IID_PPV_ARGS(&dxgi)) == S_OK && identity && dxgi,
              "device identity or DXGI interface is absent"))
    return;
  for (IUnknown *route : {static_cast<IUnknown *>(device), identity.Get(), static_cast<IUnknown *>(dxgi.Get())}) {
    for (const auto &iid : {__uuidof(ID3D10Device), __uuidof(ID3D10Device1)}) {
      expect(route->QueryInterface(iid, nullptr) == E_POINTER, "null QueryInterface output did not return E_POINTER");
      IUnknown *result = identity.Get();
      auto hr = route->QueryInterface(iid, reinterpret_cast<void **>(&result));
      expect(hr == (available ? S_OK : E_NOINTERFACE), "device QueryInterface returned %#lx", hr);
      expect(available ? result != nullptr : result == nullptr, "device interface output was not correct");
      if (SUCCEEDED(hr) && result) {
        ComPtr<IUnknown> returned_identity;
        expect(result->QueryInterface(IID_PPV_ARGS(&returned_identity)) == S_OK, "D3D10 device lost IUnknown");
        expect(returned_identity == identity, "D3D10 device has a different COM identity");
        result->Release();
      }
    }
  }
  for (size_t i = 0; i < children.size(); ++i) {
    ComPtr<ID3D10DeviceChild> child10;
    auto hr = children[i].As(&child10);
    if (!expect(hr == S_OK && child10, "child %zu lost its D3D10 interface: %#lx", i, hr))
      continue;
    ComPtr<ID3D11Device> parent11;
    children[i]->GetDevice(&parent11);
    expect(parent11.Get() == device, "child %zu returned a different D3D11 device", i);
    auto sentinel = reinterpret_cast<ID3D10Device *>(device);
    ID3D10Device *parent10 = sentinel;
    child10->GetDevice(&parent10);
    expect(available ? parent10 && parent10 != sentinel : parent10 == nullptr,
           "child %zu D3D10 GetDevice availability differs", i);
    if (parent10 && parent10 != sentinel) {
      ComPtr<IUnknown> parent_identity;
      expect(parent10->QueryInterface(IID_PPV_ARGS(&parent_identity)) == S_OK, "child %zu D3D10 device lost IUnknown",
             i);
      expect(parent_identity == identity, "child %zu returned a different D3D10 device", i);
      parent10->Release();
    }
    ComPtr<IUnknown> child_identity, child10_identity;
    auto identity11_hr = children[i].As(&child_identity);
    auto identity10_hr = child10.As(&child10_identity);
    expect(identity11_hr == S_OK && identity10_hr == S_OK && child_identity && child10_identity &&
               child_identity == child10_identity,
           "child %zu has no common COM identity: %#lx, %#lx", i, identity11_hr, identity10_hr);
  }
}

int
main() {
  const char hlsl[] = R"(
    struct Vertex { float4 position : SV_Position; };
    float4 vs(float4 position : POSITION) : SV_Position { return position; }
    [maxvertexcount(1)]
    void gs(point Vertex vertices[1], inout PointStream<Vertex> output_stream) {
      output_stream.Append(vertices[0]);
    }
    float4 ps() : SV_Target { return 1; }
  )";
  ComPtr<ID3DBlob> vs, gs, ps;
  for (auto stage : {"vs", "gs", "ps"}) {
    ComPtr<ID3DBlob> code, errors;
    auto profile = std::string(stage) + "_4_0";
    auto hr =
        D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, stage, profile.c_str(), 0, 0, &code, &errors);
    if (FAILED(hr)) {
      if (errors)
        printf("%s: %.*s\n", stage, int(errors->GetBufferSize()),
               static_cast<const char *>(errors->GetBufferPointer()));
      expect(false, "%s did not compile: %#lx", stage, hr);
      return verdict();
    }
    if (!strcmp(stage, "vs"))
      vs = code;
    if (!strcmp(stage, "gs"))
      gs = code;
    if (!strcmp(stage, "ps"))
      ps = code;
  }
  auto module = LoadLibraryW(L"d3d10_1.dll");
  if (!expect(module != nullptr, "d3d10_1.dll could not load: %lu", GetLastError()))
    return verdict();
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(GetProcAddress(module, "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "D3D10CreateDevice1 could not load")) {
    FreeLibrary(module);
    return verdict();
  }
  // the feature levels admitted by CreateDeviceContextState's pFeatureLevels contract, in descending order
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                      D3D_FEATURE_LEVEL_9_1};
  D3D_FEATURE_LEVEL maximum{};
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                          nullptr, &maximum, nullptr));
  for (bool created10 : {false, true}) {
    for (auto level : levels) {
      if (level > maximum || (created10 && level > D3D_FEATURE_LEVEL_10_1))
        continue;
      for (UINT flags : {0u, UINT(D3D11_CREATE_DEVICE_SINGLETHREADED)}) {
        for (UINT state_flags : {0u, UINT(D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED)}) {
          if (flags && !state_flags)
            continue;
          for (const auto &emulated :
               {__uuidof(ID3D10Device), __uuidof(ID3D10Device1), __uuidof(ID3D11Device), __uuidof(ID3D11Device1)}) {
            for (bool choose_level : {false, true}) {
              if (created10 && emulated != __uuidof(ID3D11Device1))
                continue;
              step("D3D%u level %#x device/state flags %#x/%#x IID %lu chosen output %u", created10 ? 10 : 11, level,
                   flags, state_flags, emulated.Data1, choose_level);
              ComPtr<ID3D11Device> device;
              if (created10) {
                ComPtr<ID3D10Device1> device10;
                CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr,
                               flags ? D3D10_CREATE_DEVICE_SINGLETHREADED : 0, static_cast<D3D10_FEATURE_LEVEL1>(level),
                               D3D10_1_SDK_VERSION, &device10));
                CHECK(device10.As(&device));
              } else {
                CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1, D3D11_SDK_VERSION,
                                        &device, nullptr, nullptr));
              }
              std::vector<ComPtr<ID3D11DeviceChild>> children;
              for (UINT dimension = D3D11_RESOURCE_DIMENSION_BUFFER; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D;
                   ++dimension) {
                if (level < D3D_FEATURE_LEVEL_10_0 && dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
                  continue;
                ComPtr<ID3D11Resource> resource;
                switch (dimension) {
                case D3D11_RESOURCE_DIMENSION_BUFFER: {
                  D3D11_BUFFER_DESC desc{sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
                  ComPtr<ID3D11Buffer> object;
                  CHECK(device->CreateBuffer(&desc, nullptr, &object));
                  CHECK(object.As(&resource));
                  break;
                }
                case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
                  D3D11_TEXTURE1D_DESC desc{1,
                                            1,
                                            1,
                                            DXGI_FORMAT_R8G8B8A8_UNORM,
                                            D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
                  ComPtr<ID3D11Texture1D> object;
                  CHECK(device->CreateTexture1D(&desc, nullptr, &object));
                  CHECK(object.As(&resource));
                  break;
                }
                case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
                  D3D11_TEXTURE2D_DESC desc{1,
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_R8G8B8A8_UNORM,
                                            {1, 0},
                                            D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
                  ComPtr<ID3D11Texture2D> object;
                  CHECK(device->CreateTexture2D(&desc, nullptr, &object));
                  CHECK(object.As(&resource));
                  break;
                }
                case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
                  D3D11_TEXTURE3D_DESC desc{1,
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_R8G8B8A8_UNORM,
                                            D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
                  ComPtr<ID3D11Texture3D> object;
                  CHECK(device->CreateTexture3D(&desc, nullptr, &object));
                  CHECK(object.As(&resource));
                  break;
                }
                }
                ComPtr<ID3D11DeviceChild> child;
                CHECK(resource.As(&child));
                children.push_back(child);
                if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
                  ComPtr<ID3D11ShaderResourceView> srv;
                  ComPtr<ID3D11RenderTargetView> rtv;
                  CHECK(device->CreateShaderResourceView(resource.Get(), nullptr, &srv));
                  CHECK(device->CreateRenderTargetView(resource.Get(), nullptr, &rtv));
                  children.push_back(srv);
                  children.push_back(rtv);
                }
              }
              if (level >= D3D_FEATURE_LEVEL_10_0) {
                D3D11_TEXTURE2D_DESC depth_desc{
                    1, 1, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
                ComPtr<ID3D11Texture2D> depth;
                ComPtr<ID3D11DepthStencilView> dsv;
                CHECK(device->CreateTexture2D(&depth_desc, nullptr, &depth));
                CHECK(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
                children.push_back(dsv);
                ComPtr<ID3D11VertexShader> vertex;
                ComPtr<ID3D11GeometryShader> geometry;
                ComPtr<ID3D11PixelShader> pixel;
                CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
                CHECK(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &geometry));
                CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
                children.push_back(vertex);
                children.push_back(geometry);
                children.push_back(pixel);
                D3D11_INPUT_ELEMENT_DESC element{
                    "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
                ComPtr<ID3D11InputLayout> layout;
                CHECK(device->CreateInputLayout(&element, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &layout));
                children.push_back(layout);
              }
              D3D11_BLEND_DESC blend_desc{};
              blend_desc.RenderTarget[0] = {
                  FALSE,           D3D11_BLEND_ONE,  D3D11_BLEND_ZERO,   D3D11_BLEND_OP_ADD,
                  D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
              ComPtr<ID3D11BlendState> blend;
              CHECK(device->CreateBlendState(&blend_desc, &blend));
              children.push_back(blend);
              D3D11_DEPTH_STENCIL_DESC stencil_desc{};
              stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
              stencil_desc.DepthFunc = D3D11_COMPARISON_LESS;
              stencil_desc.FrontFace = stencil_desc.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                                                D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
              ComPtr<ID3D11DepthStencilState> stencil;
              CHECK(device->CreateDepthStencilState(&stencil_desc, &stencil));
              children.push_back(stencil);
              D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
              ComPtr<ID3D11RasterizerState> raster;
              CHECK(device->CreateRasterizerState(&raster_desc, &raster));
              children.push_back(raster);
              D3D11_SAMPLER_DESC sampler_desc{};
              sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
              sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
              sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
              ComPtr<ID3D11SamplerState> sampler;
              CHECK(device->CreateSamplerState(&sampler_desc, &sampler));
              children.push_back(sampler);
              D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT};
              ComPtr<ID3D11Query> query;
              CHECK(device->CreateQuery(&query_desc, &query));
              children.push_back(query);
              if (level >= D3D_FEATURE_LEVEL_10_0) {
                query_desc.Query = D3D11_QUERY_OCCLUSION_PREDICATE;
                ComPtr<ID3D11Predicate> predicate;
                CHECK(device->CreatePredicate(&query_desc, &predicate));
                children.push_back(predicate);
              }
              check_interfaces(device.Get(), children, created10);
              ComPtr<ID3D11Device1> device1;
              CHECK(device.As(&device1));
              const D3D_FEATURE_LEVEL unsupported = D3D_FEATURE_LEVEL_12_0, invalid = D3D_FEATURE_LEVEL(0);
              struct {
                const D3D_FEATURE_LEVEL *levels;
                UINT count, flags, sdk;
                const GUID *iid;
              } invalid_calls[] = {
                  {&level, 0, state_flags, D3D11_SDK_VERSION, &emulated},
                  {nullptr, 0, state_flags, D3D11_SDK_VERSION, &emulated},
                  {nullptr, 1, state_flags, D3D11_SDK_VERSION, &emulated},
                  {&level, 1, ~UINT(D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED), D3D11_SDK_VERSION, &emulated},
                  {&level, 1, state_flags, D3D11_SDK_VERSION + 1, &emulated},
                  {&level, 1, state_flags, D3D11_SDK_VERSION, &__uuidof(IUnknown)},
                  {&unsupported, 1, state_flags, D3D11_SDK_VERSION, &emulated},
                  {&invalid, 1, state_flags, D3D11_SDK_VERSION, &emulated},
                  {&level, 1, 0, D3D11_SDK_VERSION, &emulated},
              };
              for (size_t variant = 0; variant < ARRAYSIZE(invalid_calls) - (flags == 0); ++variant) {
                const auto &call = invalid_calls[variant];
                for (bool output : {false, true}) {
                  for (bool chosen_output : {false, true}) {
                    step("D3D%u level %#x flags %#x/%#x IID %lu invalid variant %zu state/level outputs %u/%u",
                         created10 ? 10 : 11, level, flags, state_flags, emulated.Data1, variant, output,
                         chosen_output);
                    auto sentinel = reinterpret_cast<ID3DDeviceContextState *>(device.Get());
                    auto state = sentinel;
                    auto chosen = D3D_FEATURE_LEVEL(~0u);
                    auto hr =
                        device1->CreateDeviceContextState(call.flags, call.levels, call.count, call.sdk, *call.iid,
                                                          chosen_output ? &chosen : nullptr, output ? &state : nullptr);
                    expect(hr == E_INVALIDARG && (!chosen_output || chosen == 0) && (!output || state == nullptr),
                           "invalid creation returned %#lx, level %#x, state %p", hr, chosen, state);
                    if (SUCCEEDED(hr) && output && state && state != sentinel)
                      state->Release();
                    check_interfaces(device.Get(), children, created10);
                  }
                }
              }
              for (auto requested : {std::vector<D3D_FEATURE_LEVEL>(std::begin(levels), std::end(levels)),
                                     std::vector<D3D_FEATURE_LEVEL>{D3D_FEATURE_LEVEL_10_0, maximum}}) {
                step("D3D%u level %#x flags %#x/%#x IID %lu validates ordered levels beginning %#x",
                     created10 ? 10 : 11, level, flags, state_flags, emulated.Data1, requested[0]);
                D3D_FEATURE_LEVEL chosen{};
                expect(device1->CreateDeviceContextState(state_flags, requested.data(), requested.size(),
                                                         D3D11_SDK_VERSION, emulated, &chosen, nullptr) == S_FALSE &&
                           chosen == (requested[0] == D3D_FEATURE_LEVEL_10_0 ? requested[0] : maximum),
                       "validation-only context state did not select the first supported level: %#x", chosen);
                check_interfaces(device.Get(), children, created10);
              }
              expect(device1->CreateDeviceContextState(state_flags, &level, 1, D3D11_SDK_VERSION, emulated, nullptr,
                                                       nullptr) == S_FALSE,
                     "validation without either output did not return S_FALSE");
              check_interfaces(device.Get(), children, created10);
              ComPtr<ID3DDeviceContextState> state;
              step("D3D%u level %#x flags %#x/%#x IID %lu creates a context state, chosen output %u",
                   created10 ? 10 : 11, level, flags, state_flags, emulated.Data1, choose_level);
              D3D_FEATURE_LEVEL chosen{};
              CHECK(device1->CreateDeviceContextState(state_flags, &level, 1, D3D11_SDK_VERSION, emulated,
                                                      choose_level ? &chosen : nullptr, &state));
              expect(state && (!choose_level || chosen == level), "created state or chosen level is absent: %#x",
                     chosen);
              check_interfaces(device.Get(), children, true);
              state.Reset();
              step("D3D%u level %#x flags %#x/%#x IID %lu releases the context state and retains device interfaces",
                   created10 ? 10 : 11, level, flags, state_flags, emulated.Data1);
              check_interfaces(device.Get(), children, true);
            }
          }
        }
      }
    }
  }
  FreeLibrary(module);
  return verdict();
}
