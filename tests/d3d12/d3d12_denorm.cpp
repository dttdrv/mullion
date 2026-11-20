// contract: SM 6.2 basic fp32 arithmetic obeys its function's denormal mode: "denorm input/output should be
// preserved" for preserve, flushed for ftz, and either for any (DXC, Denorm Mode,
// https://github.com/microsoft/DirectXShaderCompiler/wiki/Denorm-Mode); fp16 preserves them in every mode.
// DXBC and older DXIL flush to signed zero:
// "Denorms MUST be flushed to sign-preserved zero on input and output of any floating point mathematical
// operation" (D3D11.3 3.1.3.2). its data movement exception requires copies to keep their bits.
// "32-bit floating point operations must produce a result that is within 0.5 ULP of the infinitely preceise
// result" (3.1.3.1). neither rule orders flushing against rounding: a product tiny before rounding that rounds
// to the smallest normal can be that normal or same-sign zero. Arm ARM A1.5.6.2 (FPCR.AH=0) flushes before
// rounding; Intel SDM 4.9.1.5 and 10.2.3.3 (MXCSR.FTZ) detect tininess after rounding with unbounded exponent.
// IEEE binary32 arithmetic on the host is the oracle. pairs cover every exponent, both signs, zeros, infinities,
// NaNs, halfway rounding, cancellation and generated significands; the log gives the seed, mode and pair.
// precise DXIL FMad is not fused (DXIL.rst, FMad); precise FDiv cannot use reciprocal form, which requires fast
// math (DXIL.rst, FDiv). "Fused operations (such as mad, dp3) must produce results that are no less accurate
// than the worst possible serial ordering of evaluation of the unfused expansion of the operation" (D3D11.3
// 3.1.3.2). precise mad (22.10.9) allows fused or serial evaluation, with flushed, rounded intermediates, but
// "the result must be consistent for the same operation to occur in multiple shaders or different parts of a
// shader" (Microsoft Learn, mad function). one evaluation must match every pair of the shader. linked noinline
// functions retain their own modes (DXIL.rst, Functions), including opposing caller and callee modes.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <random>

static const char hlsl[] = R"hlsl(
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
#if LINKED
float denorm_sum(float x, float y);
[shader("compute")]
#endif
[numthreads(GROUP, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
  if (id.x >= COUNT) return;
  uint2 bits = src.Load2(id.x * 2 * WORD);
  float x = asfloat(bits.x), y = asfloat(bits.y);
  precise float a = x + y, s = x - y, m = x * y, d = x / y;
  uint at = id.x * FIELDS * WORD;
  dst.Store4(at, uint4(asuint(a), asuint(s), asuint(m), asuint(d)));
  dst.Store2(at + 4 * WORD, bits);
  float hx = f16tof32(bits.x & HALF_MASK), hy = f16tof32(bits.y & HALF_MASK);
#if HALF
  precise float16_t h = (float16_t)hx + (float16_t)hy;
#else
  precise float h = hx + hy;
#endif
  dst.Store(at + 6 * WORD, f32tof16((float)h));
  float z = asfloat(src.Load((COUNT * 2 + id.x) * WORD));
  precise float f = mad(x, z, y);
  dst.Store(at + 7 * WORD, asuint(f));
#if LINKED
  dst.Store(at + 8 * WORD, asuint(denorm_sum(x, y)));
#endif
}
)hlsl";

static const char callee_hlsl[] = R"hlsl(
[noinline] export float denorm_sum(float x, float y) {
  precise float sum = x + y;
  return sum;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const uint32_t seed = argc > 2 ? strtoull(argv[2], nullptr, 0) : 1;
  std::mt19937 next(seed);
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  const UINT count = 2 * group + 1, fields = 9;
  const UINT fraction = std::numeric_limits<float>::digits - 1;
  const UINT sign = UINT(1) << (sizeof(float) * CHAR_BIT - 1), normal = UINT(1) << fraction;
  const UINT infinity = std::bit_cast<UINT>(std::numeric_limits<float>::infinity());
  // binary16's fields (D3D11.3 3.1.5) select signed half denormals or zero
  const UINT half_fraction = 10, half_sign = UINT(1) << (sizeof(uint16_t) * CHAR_BIT - 1);
  const int half_min_exponent = -14;
  const UINT half_mask = half_sign | ((UINT(1) << half_fraction) - 1);
  std::vector<std::array<UINT, 2>> input(count);
  for (UINT i = 0; i < count; i++) {
    UINT exponent = (i % (infinity / normal + 1)) * normal;
    UINT value = exponent | (next() & (normal - 1)) | (i & 1 ? sign : 0);
    input[i] = {value, next()};
    switch ((i / (infinity / normal + 1) + i) % 8) {
    case 0: input[i] = {value, value}; break;
    case 1: input[i] = {value, value ^ sign}; break;
    case 2: input[i][1] = std::bit_cast<UINT>(0.5f); break;
    case 3: input[i][1] = std::bit_cast<UINT>(2.0f); break;
    case 4: input[i] = {normal + (i & (normal - 1)), normal}; break;
    case 5: input[i] = {i & (normal - 1), normal - (i & (normal - 1))}; break;
    case 6: input[i][1] = i & 1 ? sign : 0; break;
    }
  }
  const UINT edge[] = {0, sign, 1, sign | 1, normal - 1, sign | (normal - 1), normal,
                       sign | normal, infinity - 1, infinity, sign | infinity, infinity | (normal >> 1),
                       std::bit_cast<UINT>(0.5f), std::bit_cast<UINT>(1.0f), std::bit_cast<UINT>(2.0f),
                       std::bit_cast<UINT>(std::ldexp(1.0f, -std::numeric_limits<float>::digits)),
                       std::bit_cast<UINT>(std::nextafter(1.0f, 2.0f)), normal + 1, sign | (normal + 1),
                       std::bit_cast<UINT>(std::nextafter(1.0f, 0.0f)),
                       std::bit_cast<UINT>(std::nextafter(std::nextafter(1.0f, 0.0f), 0.0f))};
  for (UINT i = 0; i < std::size(edge); i++)
    for (UINT j = 0; j < std::size(edge); j++)
      input[i * std::size(edge) + j] = {edge[i], edge[j]};
  input[0] = {2, 3};
  input[1] = {10, 10};
  input[2] = {10, std::bit_cast<UINT>(2.0f)};
  std::vector<UINT> multiplier(count);
  for (auto &value : multiplier) value = next();
  multiplier[0] = std::bit_cast<UINT>(1.0f);
  const char *modes[] = {"default", "preserve", "ftz", "any"};
  struct Shader {
    std::string code;
    const char *mode, *callee_mode;
  };
  std::vector<Shader> codes;
  for (bool linked : {false, true})
    for (auto mode : modes) {
      if (!compiler.dxc && strcmp(mode, "default"))
        continue;
      if (linked && (!compiler.dxc || !strcmp(mode, "default") || !strcmp(mode, "any")))
        continue;
      std::vector<std::wstring> flags;
      bool native_half = compiler.dxc && strcmp(mode, "default");
      if (native_half)
        flags = {L"-enable-16bit-types", L"-denorm", std::wstring(mode, mode + strlen(mode))};
      step("compile seed=%u mode=%s linked=%d", seed, mode, linked);
      auto code = compiler.compile(
          hlsl, "cs",
          linked        ? "lib_6_3"
          : native_half ? "cs_6_2"
                        : "cs",
          {"GROUP=" + std::to_string(group), "COUNT=" + std::to_string(count), "FIELDS=" + std::to_string(fields),
           "WORD=" + std::to_string(sizeof(UINT)), "HALF=" + std::to_string(native_half),
           "HALF_MASK=" + std::to_string(half_mask), "LINKED=" + std::to_string(linked)},
          flags
      );
      if (code.empty()) {
        printf("failed: HLSL did not compile\n");
        return 1;
      }
      const char *callee_mode = !strcmp(mode, "preserve") ? "ftz" : "preserve";
      if (linked) {
        auto callee = compiler.compile(
            callee_hlsl, "", "lib_6_3", {}, {L"-denorm", std::wstring(callee_mode, callee_mode + strlen(callee_mode))}
        );
        if (callee.empty()) {
          printf("failed: HLSL did not compile\n");
          return 1;
        }
        auto create = (DxcCreateInstanceProc)GetProcAddress(GetModuleHandleA("dxcompiler.dll"), "DxcCreateInstance");
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcLinker> linker;
        CHECK(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
        CHECK(create(CLSID_DxcLinker, IID_PPV_ARGS(&linker)));
        const wchar_t *names[] = {L"entry", L"callee"}, *args[] = {L"-Od"};
        const std::string *libraries[] = {&code, &callee};
        for (UINT i = 0; i < std::size(libraries); i++) {
          ComPtr<IDxcBlobEncoding> blob;
          CHECK(utils->CreateBlob(libraries[i]->data(), libraries[i]->size(), DXC_CP_ACP, &blob));
          CHECK(linker->RegisterLibrary(names[i], blob.Get()));
        }
        ComPtr<IDxcOperationResult> result;
        CHECK(linker->Link(L"cs", L"cs_6_3", names, std::size(names), args, std::size(args), &result));
        HRESULT status;
        CHECK(result->GetStatus(&status));
        if (FAILED(status)) {
          ComPtr<IDxcBlobEncoding> errors;
          if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors)
            printf("link: %.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
          printf("failed: HLSL did not link\n");
          return 1;
        }
        ComPtr<IDxcBlob> blob;
        CHECK(result->GetResult(&blob));
        code.assign((const char *)blob->GetBufferPointer(), blob->GetBufferSize());
        dxbc::sign(code);
        code = kept(code, "cs_linked");
      }
      codes.push_back({code, mode, linked ? callee_mode : nullptr});
    }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto rs = root_signature(device.Get(), {std::size(params), params});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  const UINT64 bytes = UINT64(count) * fields * sizeof(UINT);
  auto src = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto dst = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(src && dst && readback, "buffers")) return verdict();
  void *mapped;
  CHECK(src->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  memcpy(mapped, input.data(), input.size() * sizeof(input[0]));
  memcpy((char *)mapped + input.size() * sizeof(input[0]), multiplier.data(), multiplier.size() * sizeof(UINT));
  src->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  auto flush = [&](UINT bits) { return (bits & ~sign) < normal ? bits & sign : bits; };
  for (auto &shader : codes) {
    step("dispatch seed=%u mode=%s callee=%s", seed, shader.mode, shader.callee_mode ? shader.callee_mode : "none");
    bool preserve = !strcmp(shader.mode, "preserve"), any = !strcmp(shader.mode, "any");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(shader.code)};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)
    ));
    CHECK(forget(readback.Get()));
    transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(dst.Get(), 0, src.Get(), 0, bytes);
    transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootShaderResourceView(0, src->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
    list->Dispatch((count + group - 1) / group, 1, 1);
    transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), dst.Get());
    transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    bool serial_mad = true, fused_mad = !compiler.dxc;
    for (UINT i = 0; i < count; i++) {
      auto reference = [&](bool flush_in, bool flush_out, bool fused = false) {
        double x = std::bit_cast<float>(flush_in ? flush(input[i][0]) : input[i][0]);
        double y = std::bit_cast<float>(flush_in ? flush(input[i][1]) : input[i][1]);
        double z = std::bit_cast<float>(flush_in ? flush(multiplier[i]) : multiplier[i]);
        UINT product = std::bit_cast<UINT>((float)(x * z));
        if (flush_out) product = flush(product);
        std::array<UINT, 5> out;
        double values[] = {
            x + y, x - y, x * y, x / y,
            fused ? std::fma((float)x, (float)z, (float)y) : double(std::bit_cast<float>(product)) + y
        };
        for (UINT c = 0; c < out.size(); c++) {
          out[c] = std::bit_cast<UINT>((float)values[c]);
          if (flush_out) out[c] = flush(out[c]);
        }
        return out;
      };
      auto preserved = reference(false, false), flushed = reference(true, true);
      for (UINT c = 0; c < preserved.size(); c++) {
        UINT value = got[i * fields + (c == 4 ? 7 : c)], want = preserve ? preserved[c] : flushed[c];
        auto matches = [&](UINT expected) {
          return value == expected || ((expected & ~sign) > infinity && (value & ~sign) > infinity);
        };
        bool valid = matches(want), valid_fused = false;
        for (bool flush_in : {false, true})
          for (bool flush_out : {false, true}) {
            if (!any && (flush_in != (!preserve) || flush_out != (!preserve))) continue;
            valid |= matches(reference(flush_in, flush_out)[c]);
            if (c == 2 && flush_out) {
              double x = std::bit_cast<float>(flush_in ? flush(input[i][0]) : input[i][0]);
              double y = std::bit_cast<float>(flush_in ? flush(input[i][1]) : input[i][1]);
              UINT rounded = reference(flush_in, false)[c];
              if (std::fabs(x * y) < std::numeric_limits<float>::min() && (rounded & ~sign) == normal)
                valid |= matches(rounded & sign);
            }
            if (c == 4 && !compiler.dxc)
              valid_fused |= matches(reference(flush_in, flush_out, true)[c]);
            if (c != 3 || compiler.dxc) continue;
            double x = std::bit_cast<float>(flush_in ? flush(input[i][0]) : input[i][0]);
            double y = std::bit_cast<float>(flush_in ? flush(input[i][1]) : input[i][1]);
            double inverse = 1.0 / y;
            UINT reciprocal = std::bit_cast<UINT>((float)inverse);
            if (flush_out) reciprocal = flush(reciprocal);
            UINT serial = std::bit_cast<UINT>((float)(x * std::bit_cast<float>(reciprocal)));
            if (flush_out) serial = flush(serial);
            valid |= matches(serial);
            // divide may use a reciprocal within 1 ULP then a multiply within 0.5 ULP (D3D11.3 3.1.3.2, 22.10.2)
            if (std::isfinite(x) && std::isfinite(y) && y && std::isfinite((float)inverse)) {
              double ulp = std::ldexp(1.0, std::max(std::ilogb(std::fabs(inverse)),
                                                  std::numeric_limits<float>::min_exponent - 1) - int(fraction));
              double a = x * (inverse - ulp), b = x * (inverse + ulp);
              UINT lo = std::bit_cast<UINT>((float)std::min(a, b)), hi = std::bit_cast<UINT>((float)std::max(a, b));
              if (flush_out) lo = flush(lo), hi = flush(hi);
              float v = std::bit_cast<float>(value);
              valid |= v >= std::bit_cast<float>(lo) && v <= std::bit_cast<float>(hi) &&
                       (value & sign) == (reference(flush_in, flush_out)[c] & sign);
            }
          }
        if (c == 4) {
          serial_mad &= valid;
          fused_mad &= valid_fused;
        }
        expect(valid || valid_fused,
               "seed=%u mode=%s pair=%u (%#x,%#x,%#x) operation=%u got=%#x want=%#x", seed, shader.mode, i,
               input[i][0], input[i][1], multiplier[i], c, value, want);
      }
      if (shader.callee_mode) {
        UINT value = got[i * fields + 8], want = !strcmp(shader.callee_mode, "preserve") ? preserved[0] : flushed[0];
        expect(value == want || ((want & ~sign) > infinity && (value & ~sign) > infinity),
               "seed=%u mode=%s callee=%s pair=%u (%#x,%#x) got=%#x want=%#x", seed, shader.mode,
               shader.callee_mode, i, input[i][0], input[i][1], value, want);
      }
      for (UINT c = 0; c < input[i].size(); c++)
        expect(got[i * fields + 4 + c] == input[i][c], "seed=%u mode=%s pair=%u copy=%u", seed, shader.mode, i, c);
      auto half = [&](UINT bits) {
        float value = std::ldexp(float(bits & (half_mask & ~half_sign)), half_min_exponent - int(half_fraction));
        return std::copysign(value, bits & half_sign ? -1.0f : 1.0f);
      };
      UINT want = std::bit_cast<uint16_t>((_Float16)(half(input[i][0]) + half(input[i][1])));
      expect(got[i * fields + 6] == want, "seed=%u mode=%s pair=%u half got=%#x want=%#x", seed, shader.mode, i,
             got[i * fields + 6], want);
    }
    expect(serial_mad || fused_mad, "seed=%u mode=%s mad evaluation is consistent across all pairs", seed, shader.mode);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
