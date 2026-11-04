// contract: D3D11.3 15.1: "Rasterizer state is encapsulated in a object, which once created can not be edited."
// RSGetViewports: "unused elements of the array contain 0." GetDesc[1/2] returns the supplied state description.
// its count output "contains the actual number of viewports that are bound to the rasterizer stage."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11rasterizerstate-getdesc
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-rsgetviewports
// enum domains come from d3d11.h; GetDesc[1/2] must retain every supplied field and getters overwrite spare slots.
// D3D11.3 15.6.1 requires range validation, "skipping the call if there is an error". origin-plus-extent must
// also be in range, and a bad entry must leave the entire preceding array intact.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#ViewportRange
// negative origins are invalid at feature levels 9_x: "the call to RSSetViewports for feature levels 9_x fails"
// (D3D11_VIEWPORT, Microsoft Learn).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_viewport
// ForcedSampleCount: "Valid values are 0, 1, 2, 4, 8, and optionally 16." (D3D11_RASTERIZER_DESC1, Microsoft Learn).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ns-d3d11_1-d3d11_rasterizer_desc1
#include "d3d11_test.hpp"
#include "../metal_samples.hpp"
#include <d3d11_3.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <limits>

int main() {
  auto references = [](IUnknown *object) {
    object->AddRef();
    return object->Release();
  };
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11Device3> device3;
  CHECK(device.As(&device3));
  step("every fill/cull/Boolean combination and signed bias fields");
  const INT biases[] = {INT_MIN, -1, 0, 1, INT_MAX};
  for (auto fill : {D3D11_FILL_WIREFRAME, D3D11_FILL_SOLID})
    for (auto cull : {D3D11_CULL_NONE, D3D11_CULL_FRONT, D3D11_CULL_BACK})
      for (unsigned bits = 0; bits < (1u << 5); bits++) {
        D3D11_RASTERIZER_DESC2 desc{};
        desc.FillMode = fill;
        desc.CullMode = cull;
        desc.FrontCounterClockwise = bits & 1;
        desc.DepthClipEnable = bits >> 1 & 1;
        desc.ScissorEnable = bits >> 2 & 1;
        desc.MultisampleEnable = bits >> 3 & 1;
        desc.AntialiasedLineEnable = bits >> 4 & 1;
        desc.DepthBias = biases[bits % std::size(biases)];
        desc.DepthBiasClamp = (bits & 1 ? -1 : 1) * 0.25f;
        desc.SlopeScaledDepthBias = (bits & 2 ? -1 : 1) * 0.5f;
        ComPtr<ID3D11RasterizerState2> state, again;
        CHECK(device3->CreateRasterizerState2(&desc, &state));
        CHECK(device3->CreateRasterizerState2(&desc, &again));
        expect(state.Get() == again.Get(), "equal rasterizer descriptions gave distinct objects");
        D3D11_RASTERIZER_DESC2 got{};
        state->GetDesc2(&got);
        expect(!memcmp(&got, &desc, sizeof(desc)), "GetDesc2 changed supplied fields for bits %#x", bits);
        D3D11_RASTERIZER_DESC base{};
        D3D11_RASTERIZER_DESC1 one{};
        state->GetDesc(&base);
        state->GetDesc1(&one);
        expect(!memcmp(&base, &desc, sizeof(base)) && !memcmp(&one, &desc, sizeof(one)),
               "GetDesc/1 changed common fields");
        ComPtr<ID3D11DeviceChild> child;
        ComPtr<IUnknown> a, b;
        CHECK(state.As(&child));
        CHECK(state.As(&a));
        CHECK(child.As(&b));
        expect(a.Get() == b.Get(), "raster and device-child IUnknown identities differ");
        for (auto context : {immediate.Get(), deferred.Get()}) {
          context->RSSetState(state.Get());
          ComPtr<ID3D11RasterizerState> bound;
          auto before = references(state.Get());
          context->RSGetState(&bound);
          expect(references(state.Get()) == before + 1, "RSGetState did not AddRef its output");
          expect(bound.Get() == state.Get(), "RSGetState changed object identity");
          bound.Reset();
          expect(references(state.Get()) == before, "RSGetState output Release did not balance AddRef");
          context->RSSetState(nullptr);
          context->RSGetState(bound.ReleaseAndGetAddressOf());
          expect(!bound, "default rasterizer state getter is nonnull");
        }
      }
  step("float state fields round-trip zero, subnormals, infinities and NaN");
  for (float value : {-0.0f, 0.0f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
                      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
    for (bool slope : {false, true}) {
      D3D11_RASTERIZER_DESC2 desc{};
      desc.FillMode = D3D11_FILL_SOLID;
      desc.CullMode = D3D11_CULL_NONE;
      if (slope)
        desc.SlopeScaledDepthBias = value;
      else
        desc.DepthBiasClamp = value;
      ComPtr<ID3D11RasterizerState2> state;
      CHECK(device3->CreateRasterizerState2(&desc, &state));
      D3D11_RASTERIZER_DESC2 got;
      state->GetDesc2(&got);
      auto equal = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
      expect(equal(got.SlopeScaledDepthBias, desc.SlopeScaledDepthBias) &&
                 equal(got.DepthBiasClamp, desc.DepthBiasClamp),
             "floating-point state values changed");
      got.SlopeScaledDepthBias = desc.SlopeScaledDepthBias;
      got.DepthBiasClamp = desc.DepthBiasClamp;
      expect(!memcmp(&got, &desc, sizeof(desc)), "non-floating state fields changed");
    }
  step("conservative rasterization creation matches the advertised tier");
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options)));
  for (auto fill : {D3D11_FILL_WIREFRAME, D3D11_FILL_SOLID}) {
    D3D11_RASTERIZER_DESC2 desc{};
    desc.FillMode = fill;
    desc.CullMode = D3D11_CULL_NONE;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;
    ComPtr<ID3D11RasterizerState2> state;
    HRESULT hr = device3->CreateRasterizerState2(&desc, &state);
    bool supported = options.ConservativeRasterizationTier != D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED &&
                     fill == D3D11_FILL_SOLID;
    expect(supported ? hr == S_OK && state : FAILED(hr) && !state, "conservative tier %u fill %u returned %#lx",
           options.ConservativeRasterizationTier, fill, hr);
    if (state) {
      D3D11_RASTERIZER_DESC2 got;
      state->GetDesc2(&got);
      expect(!memcmp(&got, &desc, sizeof(desc)), "GetDesc2 changed conservative ON fields");
    }
  }
  step("rasterizer enum validation");
  for (bool fill : {false, true})
    for (UINT invalid : {0u, fill ? UINT(D3D11_FILL_WIREFRAME) - 1 : UINT(D3D11_CULL_BACK) + 1,
                         UINT(D3D11_FILL_SOLID) + 1, UINT_MAX}) {
      D3D11_RASTERIZER_DESC2 desc{};
      desc.FillMode = D3D11_FILL_SOLID;
      desc.CullMode = D3D11_CULL_NONE;
      if (fill)
        desc.FillMode = D3D11_FILL_MODE(invalid);
      else
        desc.CullMode = D3D11_CULL_MODE(invalid);
      ComPtr<ID3D11RasterizerState2> state;
      HRESULT hr = device3->CreateRasterizerState2(&desc, &state);
      expect(hr == E_INVALIDARG && !state, "invalid %s %#x returned %#lx and %p", fill ? "fill" : "cull", invalid, hr,
             state.Get());
    }
  for (auto invalid : {D3D11_CONSERVATIVE_RASTERIZATION_MODE(UINT(D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON) + 1),
                       D3D11_CONSERVATIVE_RASTERIZATION_MODE(UINT_MAX)}) {
    D3D11_RASTERIZER_DESC2 desc{};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.ConservativeRaster = invalid;
    ComPtr<ID3D11RasterizerState2> state;
    HRESULT hr = device3->CreateRasterizerState2(&desc, &state);
    expect(hr == E_INVALIDARG && !state, "invalid conservative mode %#x returned %#lx", invalid, hr);
  }
  step("invalid viewport fields preserve the preceding valid viewport");
  D3D11_VIEWPORT valid{0, 0, 16, 16, 0, 1};
  std::vector<D3D11_VIEWPORT> invalid;
  for (float D3D11_VIEWPORT::*field : {&D3D11_VIEWPORT::Width, &D3D11_VIEWPORT::Height}) {
    auto v = valid;
    v.*field = -1;
    invalid.push_back(v);
  }
  for (float D3D11_VIEWPORT::*field : {&D3D11_VIEWPORT::TopLeftX, &D3D11_VIEWPORT::TopLeftY})
    for (float value : {std::nextafter(float(D3D11_VIEWPORT_BOUNDS_MIN), -std::numeric_limits<float>::infinity()),
                        float(D3D11_VIEWPORT_BOUNDS_MAX)}) {
      auto v = valid;
      v.*field = value;
      invalid.push_back(v);
    }
  for (float D3D11_VIEWPORT::*field : {&D3D11_VIEWPORT::MinDepth, &D3D11_VIEWPORT::MaxDepth})
    for (float value : {-std::numeric_limits<float>::epsilon(), std::nextafter(1.0f, 2.0f)}) {
      auto v = valid;
      v.*field = value;
      invalid.push_back(v);
    }
  auto reversed = valid;
  reversed.MinDepth = 1;
  reversed.MaxDepth = 0;
  invalid.push_back(reversed);
  for (bool horizontal : {false, true}) {
    auto v = valid;
    if (horizontal) {
      v.TopLeftX = std::nextafter(float(D3D11_VIEWPORT_BOUNDS_MAX) - v.Width,
                                std::numeric_limits<float>::infinity());
    } else {
      v.TopLeftY = std::nextafter(float(D3D11_VIEWPORT_BOUNDS_MAX) - v.Height,
                                std::numeric_limits<float>::infinity());
    }
    invalid.push_back(v);
  }
  for (float D3D11_VIEWPORT::*field : {&D3D11_VIEWPORT::TopLeftX, &D3D11_VIEWPORT::TopLeftY, &D3D11_VIEWPORT::Width,
                                       &D3D11_VIEWPORT::Height, &D3D11_VIEWPORT::MinDepth, &D3D11_VIEWPORT::MaxDepth})
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
      auto v = valid;
      v.*field = value;
      invalid.push_back(v);
    }
  for (auto context : {immediate.Get(), deferred.Get()})
    for (const auto &v : invalid) {
      context->RSSetViewports(1, &valid);
      context->RSSetViewports(1, &v);
      D3D11_VIEWPORT got;
      UINT count = 1;
      context->RSGetViewports(&count, &got);
      expect(count == 1 && !memcmp(&got, &valid, sizeof(valid)), "invalid viewport replaced valid state");
      std::array<D3D11_VIEWPORT, 2> preceding{valid, valid}, replacement{valid, v}, result;
      replacement[0].Width /= 2;
      context->RSSetViewports(preceding.size(), preceding.data());
      context->RSSetViewports(replacement.size(), replacement.data());
      count = result.size();
      context->RSGetViewports(&count, result.data());
      expect(count == preceding.size() && !memcmp(result.data(), preceding.data(), sizeof(preceding)),
             "invalid trailing viewport partially replaced state");
    }
  step("inclusive viewport bounds and zero extents");
  for (auto context : {immediate.Get(), deferred.Get()})
    for (float origin : {float(D3D11_VIEWPORT_BOUNDS_MIN), float(D3D11_VIEWPORT_BOUNDS_MAX)})
      for (float depth : {0.0f, 1.0f}) {
        D3D11_VIEWPORT v{origin, origin, float(D3D11_VIEWPORT_BOUNDS_MAX) - origin,
                         float(D3D11_VIEWPORT_BOUNDS_MAX) - origin, depth, depth}, got;
        context->RSSetViewports(1, &v);
        UINT count = 1;
        context->RSGetViewports(&count, &got);
        expect(count == 1 && !memcmp(&got, &v, sizeof(v)), "valid viewport boundary was rejected");
      }
  step("feature levels 9_x reject negative viewport origins");
  for (auto level : {D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_3}) {
    ComPtr<ID3D11Device> downlevel;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &downlevel,
                            nullptr, &context));
    for (float D3D11_VIEWPORT::*field : {&D3D11_VIEWPORT::TopLeftX, &D3D11_VIEWPORT::TopLeftY}) {
      auto bad = valid;
      bad.*field = -1;
      context->RSSetViewports(1, &valid);
      context->RSSetViewports(1, &bad);
      D3D11_VIEWPORT got;
      UINT count = 1;
      context->RSGetViewports(&count, &got);
      expect(count == 1 && !memcmp(&got, &valid, sizeof(valid)), "negative origin accepted at level %#x", level);
    }
  }
  step("forced sample count validation");
  for (UINT count : {0u, 1u, 2u, 4u, 8u}) {
    if (count == 8 && !metal_has_8x_samples())
      continue;
    D3D11_RASTERIZER_DESC2 desc{};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.ForcedSampleCount = count;
    expect(device3->CreateRasterizerState2(&desc, nullptr) == S_FALSE, "valid forced count %u was rejected", count);
  }
  {
    D3D11_RASTERIZER_DESC2 desc{};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.ForcedSampleCount = 16;
    ComPtr<ID3D11RasterizerState2> state;
    HRESULT hr = device3->CreateRasterizerState2(&desc, &state);
    expect(hr == S_OK ? bool(state) : FAILED(hr) && !state, "optional forced count 16 returned %#lx", hr);
    if (state) {
      D3D11_RASTERIZER_DESC2 got;
      state->GetDesc2(&got);
      expect(!memcmp(&got, &desc, sizeof(desc)), "optional forced count 16 changed state fields");
      expect(device3->CreateRasterizerState2(&desc, nullptr) == S_FALSE, "forced count 16 validation-only call");
    }
  }
  for (UINT invalid : {3u, 5u, 7u, 9u, 15u, 17u, UINT(D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT), UINT_MAX}) {
    D3D11_RASTERIZER_DESC2 desc{};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.ForcedSampleCount = invalid;
    ComPtr<ID3D11RasterizerState2> state;
    HRESULT hr = device3->CreateRasterizerState2(&desc, &state);
    expect(hr == E_INVALIDARG && !state, "invalid forced count %u returned %#lx", invalid, hr);
  }
  step("all viewport/scissor getter capacities with nonzero spare entries");
  constexpr UINT capacity = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  std::array<D3D11_VIEWPORT, capacity> vps;
  std::array<D3D11_RECT, capacity> rects;
  for (UINT i = 0; i < capacity; i++) {
    vps[i] = {float(i), float(i), float(i + 1), float(i + 2), 0.25f, 0.75f};
    rects[i] = {LONG(i), LONG(i), LONG(i + 1), LONG(i + 2)};
  }
  for (auto context : {immediate.Get(), deferred.Get()})
    for (UINT bound : {0u, 1u, capacity - 1, capacity}) {
      context->RSSetViewports(bound, vps.data());
      context->RSSetScissorRects(bound, rects.data());
      UINT number = UINT_MAX;
      context->RSGetViewports(&number, nullptr);
      expect(number == bound, "viewport count query = %u, want %u", number, bound);
      number = UINT_MAX;
      context->RSGetScissorRects(&number, nullptr);
      expect(number == bound, "scissor count query = %u, want %u", number, bound);
      for (UINT requested : {0u, 1u, capacity - 1, capacity}) {
        std::array<D3D11_VIEWPORT, capacity + 1> got_vp;
        std::array<D3D11_RECT, capacity + 1> got_rect;
        memset(got_vp.data(), 0x7f, sizeof(got_vp));
        memset(got_rect.data(), 0x7f, sizeof(got_rect));
        auto before_vp = got_vp;
        auto before_rect = got_rect;
        number = requested;
        context->RSGetViewports(&number, got_vp.data());
        UINT rect_number = requested;
        context->RSGetScissorRects(&rect_number, got_rect.data());
        expect(number == bound && rect_number == bound, "getter returned wrong bound counts");
        for (UINT i = 0; i <= capacity; i++) {
          D3D11_VIEWPORT want_vp = i >= requested ? before_vp[i] : i < bound ? vps[i] : D3D11_VIEWPORT{};
          D3D11_RECT want_rect = i >= requested ? before_rect[i] : i < bound ? rects[i] : D3D11_RECT{};
          expect(!memcmp(&got_vp[i], &want_vp, sizeof(want_vp)), "viewport entry %u bound %u requested %u", i, bound,
                 requested);
          expect(!memcmp(&got_rect[i], &want_rect, sizeof(want_rect)), "scissor entry %u bound %u requested %u", i,
                 bound, requested);
        }
      }
    }
  step("oversized viewport/scissor setters leave preceding state intact");
  {
    std::array<D3D11_VIEWPORT, capacity + 1> oversized_vps{};
    std::array<D3D11_RECT, capacity + 1> oversized_rects{};
    std::copy(vps.begin(), vps.end(), oversized_vps.begin());
    std::copy(rects.begin(), rects.end(), oversized_rects.begin());
    for (auto context : {immediate.Get(), deferred.Get()}) {
      context->RSSetViewports(capacity, vps.data());
      context->RSSetScissorRects(capacity, rects.data());
      context->RSSetViewports(capacity + 1, oversized_vps.data());
      context->RSSetScissorRects(capacity + 1, oversized_rects.data());
      decltype(vps) got_vps;
      decltype(rects) got_rects;
      UINT count = capacity;
      context->RSGetViewports(&count, got_vps.data());
      expect(count == capacity && !memcmp(got_vps.data(), vps.data(), sizeof(vps)),
             "oversized viewport setter changed state");
      count = capacity;
      context->RSGetScissorRects(&count, got_rects.data());
      expect(count == capacity && !memcmp(got_rects.data(), rects.data(), sizeof(rects)),
             "oversized scissor setter changed state");
      D3D11_RASTERIZER_DESC desc{D3D11_FILL_WIREFRAME, D3D11_CULL_FRONT};
      ComPtr<ID3D11RasterizerState> state;
      CHECK(device->CreateRasterizerState(&desc, &state));
      context->RSSetState(state.Get());
      context->ClearState();
      context->RSGetState(state.ReleaseAndGetAddressOf());
      expect(!state, "ClearState retained raster state");
      count = UINT_MAX;
      context->RSGetViewports(&count, nullptr);
      expect(!count, "ClearState retained viewports");
      count = UINT_MAX;
      context->RSGetScissorRects(&count, nullptr);
      expect(!count, "ClearState retained scissors");
    }
  }
  step("live rasterizer object limit and capacity reuse");
  {
    ComPtr<ID3D11Device> limited;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &limited,
                            nullptr, nullptr));
    std::vector<ComPtr<ID3D11RasterizerState>> states(D3D11_REQ_RASTERIZER_OBJECT_COUNT_PER_DEVICE);
    D3D11_RASTERIZER_DESC desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    desc.DepthBias = -1;
    ComPtr<ID3D11RasterizerState> retired;
    CHECK(limited->CreateRasterizerState(&desc, &retired));
    retired.Reset();
    for (UINT i = 0; i < states.size(); i++) {
      desc.DepthBias = INT(i);
      CHECK(limited->CreateRasterizerState(&desc, &states[i]));
    }
    desc.DepthBias = INT(states.size());
    ComPtr<ID3D11RasterizerState> extra;
    HRESULT hr = limited->CreateRasterizerState(&desc, &extra);
    expect(hr == D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS && !extra, "object limit returned %#lx", hr);
    extra.Reset();
    desc.DepthBias = 0;
    CHECK(limited->CreateRasterizerState(&desc, &extra));
    expect(extra == states.front(), "duplicate state changed identity at the object limit");
    extra.Reset();
    desc.DepthBias = -1;
    hr = limited->CreateRasterizerState(&desc, &extra);
    expect(hr == D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS && !extra, "released cached state bypassed the limit");
    extra.Reset();
    ComPtr<ID3D11DeviceContext> context;
    CHECK(limited->CreateDeferredContext(0, &context));
    context->RSSetState(states.back().Get());
    states.back().Reset();
    desc.DepthBias = INT(states.size());
    hr = limited->CreateRasterizerState(&desc, &extra);
    expect(hr == D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS && !extra, "bound state did not occupy its slot");
    extra.Reset();
    context->RSSetState(nullptr);
    CHECK(limited->CreateRasterizerState(&desc, &extra));
  }
  step("validation-only base and versioned creation");
  D3D11_RASTERIZER_DESC base{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  base.DepthClipEnable = TRUE;
  D3D11_RASTERIZER_DESC1 one{};
  memcpy(&one, &base, sizeof(base));
  D3D11_RASTERIZER_DESC2 two{};
  memcpy(&two, &base, sizeof(base));
  expect(device3->CreateRasterizerState2(&two, nullptr) == S_FALSE, "CreateRasterizerState2 validation-only call");
  expect(SUCCEEDED(device3->CreateRasterizerState1(&one, nullptr)), "CreateRasterizerState1 optional output call");
  expect(SUCCEEDED(device->CreateRasterizerState(&base, nullptr)), "CreateRasterizerState optional output call");
  return verdict();
}
