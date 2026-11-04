// the depth/stencil oracle uses D3D11.3 17.9-17.13 and the API stencil
// operation definitions. "The conversion of z to Depth Buffer precision uses
// round to nearest (+0.5 and truncate)." (17.11). "If no Depth Buffer is bound,
// the depth test always passes." (17.11).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_stencil_op
#pragma once
#include <d3d11.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdlib>

namespace om {
struct DepthCase {
  D3D11_DEPTH_STENCIL_DESC state{};
  UINT reference = 0, flags = 0, shader = 1;
  float depth = 0.5f, min = 0, max = 1;
  bool absent = false;
};
inline DepthCase depth_baseline() {
  DepthCase c;
  c.state = {
      TRUE,  D3D11_DEPTH_WRITE_MASK_ALL,      D3D11_COMPARISON_ALWAYS,
      FALSE, D3D11_DEFAULT_STENCIL_READ_MASK, D3D11_DEFAULT_STENCIL_WRITE_MASK};
  c.state.FrontFace =
      c.state.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                          D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
  return c;
}
inline std::vector<DepthCase> depth_cases(bool stencil_reference = false) {
  std::vector<DepthCase> cases;
  const float depths[] = {0, 0.25f, 0.5f, 0.75f, 1};
  for (UINT fn = D3D11_COMPARISON_NEVER; fn <= D3D11_COMPARISON_ALWAYS; fn++)
    for (bool enable : {false, true})
      for (auto write :
           {D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_DEPTH_WRITE_MASK_ALL})
        for (float z : depths) {
          auto c = depth_baseline();
          c.state.DepthEnable = enable;
          c.state.DepthFunc = D3D11_COMPARISON_FUNC(fn);
          c.state.DepthWriteMask = write;
          c.depth = z;
          cases.push_back(c);
        }
  for (UINT op = D3D11_STENCIL_OP_KEEP; op <= D3D11_STENCIL_OP_DECR; op++)
    for (UINT event = 0; event < 3; event++)
      for (UINT8 mask : {UINT8(0), UINT8(D3D11_DEFAULT_STENCIL_WRITE_MASK),
                         UINT8(0x55), UINT8(0xaa)}) {
        auto c = depth_baseline();
        c.reference = 0xa6;
        c.state.StencilEnable = TRUE;
        c.state.StencilWriteMask = mask;
        c.state.DepthFunc =
            event == 1 ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_ALWAYS;
        c.state.FrontFace.StencilFunc = c.state.BackFace.StencilFunc =
            event == 0 ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_ALWAYS;
        // different face operations ensure a front/back mapping cannot pass by
        // using the same descriptor twice.
        auto front = D3D11_STENCIL_OP(op);
        auto back = D3D11_STENCIL_OP(D3D11_STENCIL_OP_DECR +
                                     D3D11_STENCIL_OP_KEEP - op);
        if (event == 0)
          c.state.FrontFace.StencilFailOp = front,
          c.state.BackFace.StencilFailOp = back;
        if (event == 1)
          c.state.FrontFace.StencilDepthFailOp = front,
          c.state.BackFace.StencilDepthFailOp = back;
        if (event == 2)
          c.state.FrontFace.StencilPassOp = front,
          c.state.BackFace.StencilPassOp = back;
        cases.push_back(c);
      }
  for (UINT fn = D3D11_COMPARISON_NEVER; fn <= D3D11_COMPARISON_ALWAYS; fn++)
    for (UINT ref = 0; ref <= UINT8_MAX; ref++) {
      auto c = depth_baseline();
      c.reference = ref;
      c.state.StencilEnable = TRUE;
      c.state.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
      c.state.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC(fn);
      c.state.BackFace.StencilFunc = D3D11_COMPARISON_FUNC(
          D3D11_COMPARISON_ALWAYS + D3D11_COMPARISON_NEVER - fn);
      cases.push_back(c);
    }
  for (UINT mask = 0; mask <= UINT8_MAX; mask++) {
    auto c = depth_baseline();
    c.reference = std::rotl(UINT8(mask), 1);
    c.state.StencilEnable = TRUE;
    c.state.StencilReadMask = UINT8(mask);
    c.state.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC(
        D3D11_COMPARISON_NEVER + mask % D3D11_COMPARISON_ALWAYS);
    c.state.BackFace.StencilFunc = D3D11_COMPARISON_FUNC(
        D3D11_COMPARISON_ALWAYS - mask % D3D11_COMPARISON_ALWAYS);
    cases.push_back(c);
  }
  for (UINT mask = 0; mask <= UINT8_MAX; mask++)
    for (auto op : {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INVERT,
                    D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR}) {
      auto c = depth_baseline();
      c.reference = UINT8(~mask);
      c.state.StencilEnable = TRUE;
      c.state.StencilWriteMask = UINT8(mask);
      c.state.FrontFace.StencilPassOp = op;
      c.state.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
      cases.push_back(c);
    }
  for (UINT ref : {0u, 1u, UINT(UINT8_MAX), UINT(UINT8_MAX) + 1u, ~0u}) {
    auto c = depth_baseline();
    c.reference = ref;
    c.state.StencilEnable = TRUE;
    c.state.FrontFace.StencilPassOp = c.state.BackFace.StencilPassOp =
        D3D11_STENCIL_OP_REPLACE;
    cases.push_back(c);
  }
  for (UINT flags = 0;
       flags <= UINT(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL);
       flags++) {
    auto c = depth_baseline();
    c.flags = flags;
    c.reference = UINT8_MAX;
    c.state.StencilEnable = TRUE;
    c.state.FrontFace.StencilPassOp = c.state.BackFace.StencilPassOp =
        D3D11_STENCIL_OP_REPLACE;
    c.depth = 0.25f;
    cases.push_back(c);
  }
  for (bool stencil : {false, true}) {
    auto c = depth_baseline();
    c.absent = true;
    c.state.DepthFunc = D3D11_COMPARISON_NEVER;
    c.state.StencilEnable = stencil;
    c.state.FrontFace.StencilFunc = c.state.BackFace.StencilFunc =
        D3D11_COMPARISON_NEVER;
    cases.push_back(c);
  }
  for (float z :
       {-INFINITY, -1.0f, -0.0f, 0.25f, 0.5f, 0.75f, 2.0f, INFINITY, NAN}) {
    auto c = depth_baseline();
    c.depth = z;
    c.min = 0.25f;
    c.max = 0.75f;
    cases.push_back(c);
  }
  for (UINT fn = D3D11_COMPARISON_NEVER; fn <= D3D11_COMPARISON_ALWAYS; fn++)
    for (UINT shader : {0u, 2u, 3u}) {
      auto c = depth_baseline();
      c.shader = shader;
      c.state.DepthFunc = D3D11_COMPARISON_FUNC(fn);
      c.depth = shader == 2 ? 0.25f : shader == 3 ? 0.75f : 0.5f;
      cases.push_back(c);
    }
  if (stencil_reference) {
    for (UINT fn = D3D11_COMPARISON_NEVER; fn <= D3D11_COMPARISON_ALWAYS; fn++)
      for (UINT ref = 0; ref <= UINT8_MAX; ref++) {
        auto c = depth_baseline();
        c.shader = 4;
        c.reference = ref;
        c.state.StencilEnable = TRUE;
        c.state.FrontFace = c.state.BackFace = {
            D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
            D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_FUNC(fn)};
        cases.push_back(c);
        c.reference = UINT8(std::rotl(UINT8(ref), 1));
        c.state.StencilReadMask = UINT8(ref);
        cases.push_back(c);
      }
    for (UINT mask = 0; mask <= UINT8_MAX; mask++) {
      auto c = depth_baseline();
      c.shader = 4;
      c.reference = UINT8_MAX;
      c.state.StencilEnable = TRUE;
      c.state.StencilWriteMask = UINT8(mask);
      c.state.FrontFace.StencilPassOp = c.state.BackFace.StencilPassOp =
          D3D11_STENCIL_OP_REPLACE;
      cases.push_back(c);
    }
  }
  return cases;
}
template <typename Number>
inline bool comparison(D3D11_COMPARISON_FUNC fn, Number a, Number b) {
  switch (fn) {
  case D3D11_COMPARISON_NEVER:
    return false;
  case D3D11_COMPARISON_LESS:
    return a < b;
  case D3D11_COMPARISON_EQUAL:
    return a == b;
  case D3D11_COMPARISON_LESS_EQUAL:
    return a <= b;
  case D3D11_COMPARISON_GREATER:
    return a > b;
  case D3D11_COMPARISON_NOT_EQUAL:
    return a != b;
  case D3D11_COMPARISON_GREATER_EQUAL:
    return a >= b;
  case D3D11_COMPARISON_ALWAYS:
    return true;
  }
  std::abort();
}
inline UINT8 stencil(D3D11_STENCIL_OP op, UINT8 old, UINT ref) {
  switch (op) {
  case D3D11_STENCIL_OP_KEEP:
    return old;
  case D3D11_STENCIL_OP_ZERO:
    return 0;
  case D3D11_STENCIL_OP_REPLACE:
    return UINT8(ref);
  case D3D11_STENCIL_OP_INCR_SAT:
    return UINT8(std::min(UINT(old) + 1, UINT(UINT8_MAX)));
  case D3D11_STENCIL_OP_DECR_SAT:
    return old ? old - 1 : 0;
  case D3D11_STENCIL_OP_INVERT:
    return UINT8(~old);
  case D3D11_STENCIL_OP_INCR:
    return UINT8(old + 1);
  case D3D11_STENCIL_OP_DECR:
    return UINT8(old - 1);
  }
  std::abort();
}
struct DepthFormat {
  DXGI_FORMAT value;
  UINT depth_bits, stencil_bits, stride;
  bool floating;
};
inline constexpr DepthFormat depth_formats[] = {
    {DXGI_FORMAT_D16_UNORM, 16, 0, sizeof(UINT16), false},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, 24, 8, sizeof(UINT), false},
    {DXGI_FORMAT_D32_FLOAT, 32, 0, sizeof(float), true},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 32, 8, sizeof(UINT64), true}};
inline constexpr UINT depth_width = UINT8_MAX + 1u + 2;
inline float initial_depth(UINT x) {
  const float values[] = {0, 0.25f, 0.5f, 0.75f, 1};
  return values[x % std::size(values)];
}
inline UINT depth_word(DepthFormat f, float z) {
  return f.floating
             ? std::bit_cast<UINT>(z)
             : UINT(std::floor(double(z) * ((uint64_t(1) << f.depth_bits) - 1) +
                               0.5));
}
inline double depth_value(DepthFormat f, UINT word) {
  return f.floating ? double(std::bit_cast<float>(word))
                    : double(word) / ((uint64_t(1) << f.depth_bits) - 1);
}
inline std::vector<unsigned char> depth_seed(DepthFormat f, UINT height) {
  std::vector<unsigned char> bytes(depth_width * height * f.stride);
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < depth_width; x++) {
      UINT word = depth_word(f, initial_depth(x));
      auto p = bytes.data() + (y * depth_width + x) * f.stride;
      memcpy(p, &word, f.depth_bits / CHAR_BIT);
      if (f.stencil_bits)
        p[f.depth_bits / CHAR_BIT] = UINT8(x - 1);
    }
  return bytes;
}
struct DepthResult {
  UINT depth, color;
  UINT8 stencil;
};
inline DepthResult depth_result(DepthFormat f, const DepthCase &k, UINT x,
                                bool back) {
  UINT old = depth_word(f, initial_depth(x));
  UINT8 saved = UINT8(x - 1);
  float clamped =
      std::fmin(k.max, std::fmax(k.min, k.shader == 0 ? 0.5f : k.depth));
  UINT incoming = depth_word(f, clamped);
  const auto &face = back ? k.state.BackFace : k.state.FrontFace;
  bool stenciled = k.state.StencilEnable && f.stencil_bits && !k.absent;
  UINT reference = k.shader == 4 ? UINT8(k.reference + x - 1) : k.reference;
  bool spass = !stenciled ||
               comparison(face.StencilFunc, reference & k.state.StencilReadMask,
                          UINT(saved & k.state.StencilReadMask));
  // Apple GPUs have no 24-bit depth: D24 is Depth32Float, and an incoming depth compares before any rounding
  const bool unrounded = f.depth_bits == 24;
  bool dpass = !k.state.DepthEnable || k.absent ||
               comparison(k.state.DepthFunc, unrounded ? double(clamped) : depth_value(f, incoming),
                          unrounded ? double(float(depth_value(f, old))) : depth_value(f, old));
  UINT8 updated = saved;
  if (stenciled && !(k.flags & D3D11_DSV_READ_ONLY_STENCIL)) {
    auto op = !spass   ? face.StencilFailOp
              : !dpass ? face.StencilDepthFailOp
                       : face.StencilPassOp;
    UINT8 value = stencil(op, saved, reference);
    updated = UINT8((saved & ~k.state.StencilWriteMask) |
                    (value & k.state.StencilWriteMask));
  }
  bool write = spass && dpass && !k.absent && k.state.DepthEnable &&
               k.state.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL &&
               !(k.flags & D3D11_DSV_READ_ONLY_DEPTH);
  return {write ? incoming : old, spass && dpass ? back ? 2u : 1u : 0u,
          updated};
}
inline constexpr char depth_hlsl[] = R"hlsl(
cbuffer C : register(b0) { float written_depth; uint reversed; uint stencil_reference; float unused; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  if (reversed) uv = uv.yx;
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
uint ps(bool front : SV_IsFrontFace) : SV_Target { return front ? 1 : 2; }
uint ps_depth(bool front : SV_IsFrontFace, out float z : SV_Depth) : SV_Target { z=written_depth; return front ? 1 : 2; }
#ifdef CONSERVATIVE
uint ps_le(bool front : SV_IsFrontFace, out float z : SV_DepthLessEqual) : SV_Target { z=written_depth; return front ? 1 : 2; }
uint ps_ge(bool front : SV_IsFrontFace, out float z : SV_DepthGreaterEqual) : SV_Target { z=written_depth; return front ? 1 : 2; }
#endif
#ifdef STENCIL_REFERENCE
uint ps_stencil(float4 pos : SV_Position, bool front : SV_IsFrontFace,
                out float z : SV_Depth, out uint ref : SV_StencilRef) : SV_Target {
  z=written_depth; ref=(stencil_reference+uint(pos.x)-1)&STENCIL_MASK; return front ? 1 : 2;
}
#endif
)hlsl";
} // namespace om
