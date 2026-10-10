// contract: ignored state-description fields have Windows' canonical values, and the same effective state
// returns the same object through Direct3D 10 and 11: "the same interface will be returned" (Microsoft Learn,
// ID3D11Device::CreateSamplerState, Remarks; CreateBlendState and CreateDepthStencilState likewise).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createsamplerstate
// D3D11.3 17.1: "If false, only use entry [0] below for all RenderTargets"; 22.3.34:
// "MaxAnisotropy (when Filter is Anisotropic)". 17.9 disables depth/stencil irrespective of other settings.
// the canonical GetDesc values are recorded by Wine's test_create_{blend,depthstencil,sampler}_state in
// dlls/d3d11/tests/d3d11.c and dlls/d3d10core/tests/d3d10core.c. depth and blend defaults below come from the
// Microsoft Learn D3D11_DEPTH_STENCIL_DESC and D3D11_BLEND_DESC tables, through the SDK's default constructors.
// enums, target counts and anisotropy bounds come from the API header. padding is deliberately initialized
// differently on equivalent descriptions and never compared as returned data. no shaders are needed.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <d3d10_1.h>
#include <cstddef>
#include <iterator>
#include <limits>

int
main() {
  ComPtr<ID3D11Device> device;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, nullptr));
  ComPtr<ID3D11Device1> device1;
  CHECK(device.As(&device1));
  ComPtr<ID3DDeviceContextState> compatibility;
  const D3D_FEATURE_LEVEL legacy_level = D3D_FEATURE_LEVEL_10_1;
  CHECK(device1->CreateDeviceContextState(0, &legacy_level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device1),
                                         nullptr, &compatibility));
  ComPtr<ID3D10Device1> device10;
  CHECK(device.As(&device10));
  const BOOL enables[] = {FALSE, TRUE, std::numeric_limits<BOOL>::min(), std::numeric_limits<BOOL>::max()};
  const UINT filter_types = D3D11_FILTER_TYPE_LINEAR - D3D11_FILTER_TYPE_POINT + 1;
  const UINT basic_filters = filter_types * filter_types * filter_types;
  const D3D11_TEXTURE_ADDRESS_MODE addresses[] = {
      D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_MIRROR};
  constexpr auto padding_begin = offsetof(D3D11_DEPTH_STENCIL_DESC, StencilWriteMask) + sizeof(UINT8);
  constexpr auto padding_end = offsetof(D3D11_DEPTH_STENCIL_DESC, FrontFace);
  unsigned variant = 0;

  for (BOOL depth : enables) {
    for (BOOL stencil : enables) {
      for (UINT compare = D3D11_COMPARISON_NEVER; compare <= D3D11_COMPARISON_ALWAYS; compare++) {
        for (UINT op = D3D11_STENCIL_OP_KEEP; op <= D3D11_STENCIL_OP_DECR; op++) {
          step("depth/stencil variant=%u depth=%d stencil=%d compare=%u op=%u", variant, depth, stencil, compare, op);
          D3D11_DEPTH_STENCIL_DESC desc;
          memset(&desc, variant++, sizeof(desc));
          desc.DepthEnable = depth;
          desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK(compare % (D3D11_DEPTH_WRITE_MASK_ALL + 1));
          desc.DepthFunc = D3D11_COMPARISON_FUNC(compare);
          desc.StencilEnable = stencil;
          desc.StencilReadMask = UINT8((compare - D3D11_COMPARISON_NEVER) * std::numeric_limits<UINT8>::max() /
                                      (D3D11_COMPARISON_ALWAYS - D3D11_COMPARISON_NEVER));
          desc.StencilWriteMask = UINT8(~desc.StencilReadMask);
          desc.FrontFace = {D3D11_STENCIL_OP(op), D3D11_STENCIL_OP(op), D3D11_STENCIL_OP(op),
                            D3D11_COMPARISON_FUNC(compare)};
          desc.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE,
                           D3D11_COMPARISON_ALWAYS};
          D3D11_DEPTH_STENCIL_DESC wanted = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
          wanted.DepthEnable = !!depth;
          wanted.StencilEnable = !!stencil;
          if (depth) {
            wanted.DepthWriteMask = desc.DepthWriteMask;
            wanted.DepthFunc = desc.DepthFunc;
          }
          if (stencil) {
            wanted.StencilReadMask = desc.StencilReadMask;
            wanted.StencilWriteMask = desc.StencilWriteMask;
            wanted.FrontFace = desc.FrontFace;
            wanted.BackFace = desc.BackFace;
          }
          memset(reinterpret_cast<char *>(&wanted) + padding_begin, ~(variant - 1), padding_end - padding_begin);
          ComPtr<ID3D11DepthStencilState> first, same, roundtrip;
          CHECK(device->CreateDepthStencilState(&desc, &first));
          D3D11_DEPTH_STENCIL_DESC got{};
          first->GetDesc(&got);
          expect(got.DepthEnable == wanted.DepthEnable && got.DepthWriteMask == wanted.DepthWriteMask &&
                     got.DepthFunc == wanted.DepthFunc && got.StencilEnable == wanted.StencilEnable &&
                     got.StencilReadMask == wanted.StencilReadMask && got.StencilWriteMask == wanted.StencilWriteMask &&
                     !memcmp(&got.FrontFace, &wanted.FrontFace, sizeof(got.FrontFace)) &&
                     !memcmp(&got.BackFace, &wanted.BackFace, sizeof(got.BackFace)), "canonical depth/stencil fields");
          CHECK(device->CreateDepthStencilState(&wanted, &same));
          expect(first == same, "equivalent depth/stencil descriptions differ");
          ComPtr<ID3D10DepthStencilState> legacy;
          CHECK(device10->CreateDepthStencilState(reinterpret_cast<const D3D10_DEPTH_STENCIL_DESC *>(&desc), &legacy));
          CHECK(legacy.As(&roundtrip));
          expect(first == roundtrip, "Direct3D 10 depth/stencil identity differs");
          D3D10_DEPTH_STENCIL_DESC got10{};
          legacy->GetDesc(&got10);
          expect(got10.DepthEnable == wanted.DepthEnable &&
                     got10.DepthWriteMask == D3D10_DEPTH_WRITE_MASK(wanted.DepthWriteMask) &&
                     got10.DepthFunc == D3D10_COMPARISON_FUNC(wanted.DepthFunc) &&
                     got10.StencilEnable == wanted.StencilEnable &&
                     got10.StencilReadMask == wanted.StencilReadMask &&
                     got10.StencilWriteMask == wanted.StencilWriteMask &&
                     !memcmp(&got10.FrontFace, &wanted.FrontFace, sizeof(got10.FrontFace)) &&
                     !memcmp(&got10.BackFace, &wanted.BackFace, sizeof(got10.BackFace)),
                 "Direct3D 10 depth/stencil fields");
          legacy.Reset();
          roundtrip.Reset();
          expect(same.Detach()->Release() == 1, "depth/stencil duplicate reference");
          expect(first.Detach()->Release() == 0, "depth/stencil last public reference");
        }
      }
    }
  }

  for (UINT reduction = D3D11_FILTER_REDUCTION_TYPE_STANDARD; reduction <= D3D11_FILTER_REDUCTION_TYPE_COMPARISON;
       reduction++) {
    for (UINT filtering = 0; filtering <= basic_filters; filtering++) {
      auto filter = D3D11_FILTER(filtering == basic_filters
                                   ? D3D11_ENCODE_ANISOTROPIC_FILTER(reduction)
                                   : D3D11_ENCODE_BASIC_FILTER(filtering / filter_types / filter_types,
                                                              filtering / filter_types % filter_types,
                                                              filtering % filter_types, reduction));
      for (UINT border = 0; border < (1u << std::size(addresses)); border++) {
        // D3D11.3 7.18.3 permits MaxAnisotropy from 0 through D3D11_MAX_MAXANISOTROPY
        for (UINT anisotropy = 0; anisotropy <= D3D11_MAX_MAXANISOTROPY; anisotropy++) {
          for (UINT compare = D3D11_COMPARISON_NEVER; compare <= D3D11_COMPARISON_ALWAYS; compare++) {
            step("sampler variant=%u filter=%u border=%u anisotropy=%u compare=%u", variant++, filter, border,
                 anisotropy, compare);
            D3D11_SAMPLER_DESC desc = CD3D11_SAMPLER_DESC(D3D11_DEFAULT);
            desc.Filter = filter;
            desc.AddressU = border & (1u << 0) ? D3D11_TEXTURE_ADDRESS_BORDER : addresses[0];
            desc.AddressV = border & (1u << 1) ? D3D11_TEXTURE_ADDRESS_BORDER : addresses[1];
            desc.AddressW = border & (1u << 2) ? D3D11_TEXTURE_ADDRESS_BORDER : addresses[2];
            desc.MipLODBias = 0.5f;
            desc.MinLOD = -1.0f;
            desc.MaxLOD = 1.0f;
            desc.MaxAnisotropy = anisotropy;
            desc.ComparisonFunc = D3D11_COMPARISON_FUNC(compare);
            auto wanted = desc;
            if (filtering != basic_filters)
              wanted.MaxAnisotropy = 0;
            if (reduction == D3D11_FILTER_REDUCTION_TYPE_STANDARD)
              wanted.ComparisonFunc = D3D11_COMPARISON_NEVER;
            if (!border)
              for (auto &component : wanted.BorderColor)
                component = 0;
            ComPtr<ID3D11SamplerState> first, same, roundtrip;
            CHECK(device->CreateSamplerState(&desc, &first));
            D3D11_SAMPLER_DESC got{};
            first->GetDesc(&got);
            expect(!memcmp(&got, &wanted, sizeof(got)), "canonical sampler fields");
            CHECK(device->CreateSamplerState(&wanted, &same));
            expect(first == same, "equivalent sampler descriptions differ");
            ComPtr<ID3D10SamplerState> legacy;
            CHECK(device10->CreateSamplerState(reinterpret_cast<const D3D10_SAMPLER_DESC *>(&desc), &legacy));
            CHECK(legacy.As(&roundtrip));
            expect(first == roundtrip, "Direct3D 10 sampler identity differs");
            D3D10_SAMPLER_DESC got10{};
            legacy->GetDesc(&got10);
            expect(!memcmp(&got10, &wanted, sizeof(got10)), "Direct3D 10 sampler fields");
            legacy.Reset();
            roundtrip.Reset();
            expect(same.Detach()->Release() == 1, "sampler duplicate reference");
            expect(first.Detach()->Release() == 0, "sampler last public reference");
          }
        }
      }
    }
  }

  for (BOOL independent : enables) {
    for (BOOL blend : enables) {
      for (BOOL logic : enables) {
        if (independent && logic)
          continue;
        for (UINT op = D3D11_BLEND_OP_ADD; op <= D3D11_BLEND_OP_MAX; op++) {
          step("blend variant=%u independent=%d blend=%d logic=%d op=%u", variant++, independent, blend, logic, op);
          D3D11_BLEND_DESC1 desc{};
          desc.AlphaToCoverageEnable = blend;
          desc.IndependentBlendEnable = independent;
          D3D11_BLEND_DESC1 wanted{};
          const auto defaults = CD3D11_BLEND_DESC(D3D11_DEFAULT);
          wanted.AlphaToCoverageEnable = !!blend;
          wanted.IndependentBlendEnable = !!independent;
          for (UINT i = 0; i < std::size(desc.RenderTarget); i++) {
            auto &rt = desc.RenderTarget[i];
            rt.BlendEnable = blend;
            rt.LogicOpEnable = logic;
            rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP(op);
            rt.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
            rt.DestBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
            rt.LogicOp = D3D11_LOGIC_OP_XOR;
            rt.RenderTargetWriteMask = UINT8(i & D3D11_COLOR_WRITE_ENABLE_ALL);
            auto &expected = wanted.RenderTarget[i];
            const auto &baseline = defaults.RenderTarget[i];
            expected = {FALSE, FALSE, baseline.SrcBlend, baseline.DestBlend, baseline.BlendOp,
                        baseline.SrcBlendAlpha, baseline.DestBlendAlpha, baseline.BlendOpAlpha,
                        D3D11_LOGIC_OP_NOOP, rt.RenderTargetWriteMask};
            if (blend) {
              expected = rt;
              expected.BlendEnable = TRUE;
            }
            expected.LogicOpEnable = !!logic;
            expected.LogicOp = logic ? rt.LogicOp : D3D11_LOGIC_OP_NOOP;
            expected.RenderTargetWriteMask = rt.RenderTargetWriteMask;
            if (i && !independent)
              expected = wanted.RenderTarget[0];
          }
          ComPtr<ID3D11BlendState1> first, same;
          HRESULT hr = device1->CreateBlendState1(&desc, &first);
          if (blend && logic) {
            expect(hr == E_INVALIDARG && !first, "blending and logic operations are mutually exclusive");
            continue;
          }
          if (!expect(hr == S_OK && first, "CreateBlendState1 returned %#lx", hr))
            return verdict();
          D3D11_BLEND_DESC1 got{};
          first->GetDesc1(&got);
          expect(got.AlphaToCoverageEnable == wanted.AlphaToCoverageEnable &&
                     got.IndependentBlendEnable == wanted.IndependentBlendEnable, "canonical blend booleans");
          for (UINT i = 0; i < std::size(got.RenderTarget); i++)
            expect(!memcmp(&got.RenderTarget[i], &wanted.RenderTarget[i],
                           offsetof(D3D11_RENDER_TARGET_BLEND_DESC1, RenderTargetWriteMask) + sizeof(UINT8)),
                   "canonical blend target %u", i);
          CHECK(device1->CreateBlendState1(&wanted, &same));
          expect(first == same, "equivalent blend descriptions differ");
          expect(same.Detach()->Release() == 1, "blend duplicate reference");
          expect(first.Detach()->Release() == 0, "blend last public reference");
        }
      }
    }
  }

  for (UINT target = 0; target < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; target++) {
    for (BOOL blend : enables) {
      for (bool uniform : {false, true}) {
        for (bool different_mask : {false, true}) {
          step("Direct3D 10 blend variant=%u target=%u blend=%d uniform=%d mask=%d", variant++, target, blend,
               uniform, different_mask);
          D3D10_BLEND_DESC desc{};
          desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ONE;
          desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ZERO;
          desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
          for (auto &mask : desc.RenderTargetWriteMask)
            mask = D3D10_COLOR_WRITE_ENABLE_ALL;
          if (uniform)
            for (UINT i = 0; i < std::size(desc.BlendEnable); i++)
              desc.BlendEnable[i] = blend ? enables[1 + i % (std::size(enables) - 1)] : FALSE;
          desc.BlendEnable[target] = blend;
          if (different_mask)
            desc.RenderTargetWriteMask[target] = D3D10_COLOR_WRITE_ENABLE_RED;
          ComPtr<ID3D10BlendState> legacy;
          CHECK(device10->CreateBlendState(&desc, &legacy));
          ComPtr<ID3D11BlendState> first, same;
          CHECK(legacy.As(&first));
          D3D11_BLEND_DESC got{};
          first->GetDesc(&got);
          expect(got.IndependentBlendEnable == ((!uniform && !!blend) || different_mask),
                 "Direct3D 10 independent enables and masks");
          D3D10_BLEND_DESC got10{};
          legacy->GetDesc(&got10);
          expect(got10.SrcBlend == desc.SrcBlend && got10.DestBlend == desc.DestBlend &&
                     got10.BlendOp == desc.BlendOp &&
                     got10.SrcBlendAlpha == desc.SrcBlendAlpha && got10.DestBlendAlpha == desc.DestBlendAlpha &&
                     got10.BlendOpAlpha == desc.BlendOpAlpha, "Direct3D 10 blend equations");
          for (UINT i = 0; i < std::size(desc.BlendEnable); i++)
            expect(got10.BlendEnable[i] == !!desc.BlendEnable[i] &&
                       got10.RenderTargetWriteMask[i] == desc.RenderTargetWriteMask[i],
                   "Direct3D 10 blend target %u", i);
          CHECK(device->CreateBlendState(&got, &same));
          expect(first == same, "Direct3D 10 blend roundtrip differs");
          ComPtr<ID3D10BlendState1> legacy1, same10;
          CHECK(legacy.As(&legacy1));
          D3D10_BLEND_DESC1 got1{};
          legacy1->GetDesc1(&got1);
          CHECK(device10->CreateBlendState1(&got1, &same10));
          expect(legacy1 == same10, "Direct3D 10.1 blend roundtrip differs");
        }
      }
    }
  }
  return verdict();
}
