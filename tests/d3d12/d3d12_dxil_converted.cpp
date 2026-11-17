// contract: the DXIL operations that only a DXBC shader converted to DXIL has give what DXIL.rst defines for them.
// HLSL compiled to DXIL multiplies, divides and casts bits with LLVM's instructions; Microsoft's DXBC to DXIL
// converter keeps DXBC's imul, umul, udiv, uaddc and usubb as operations with two results, and casts its untyped
// registers with bit cast operations (DXIL.rst, "Operations": BinaryWithTwoOuts, BinaryWithCarryOrBorrow and the
// Bitcast classes):
// - IMul, UMul: "multiply of 32-bit operands src0 and src1 (note they are signed)" or unsigned, "producing the
//   correct full 64-bit result. The low 32 bits are placed in destLO. The high 32 bits are placed in destHI", and
//   "IMul(src0, src1) = destHi, destLo";
// - UDiv: "destQUOT, destREM = UDiv(src0, src1)", and "Divide by zero returns 0xffffffff for both quotient and
//   remainder";
// - UAddc: "dest0, dest1 = UAddc(src0, src1)", an "unsigned add of 32-bit operands src0 and src1, placing the LSB
//   part of the 32-bit result in dest0. dest1 is written with: 1 if a carry is produced, 0 otherwise";
// - USubb: "Unsigned subtract of 32-bit operands src1 from src0 ... dest1 is written with: 1 if a borrow is
//   produced, 0 otherwise";
// - BitcastI32toF32 and BitcastF32toI32, and BitcastI16toF16 and BitcastF16toI16 where the device has 16-bit
//   operations, "bitcast between different sizes": the bits unchanged. the test casts to a float, takes FAbs and
//   casts back, which clears the sign of a float that is a number; a denormal may come back as a zero (D3D11.3
//   3.1.3.2) and a NaN as any NaN (3.1.3.1), so neither is held.
// DXC compiles a shader that marks where each operation goes with UMax and UMin of the same operands and with
// LLVM's bitcast; its disassembly has those replaced, and is assembled and validated before a pipeline is made of it.
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst
#include "dxil_assembly.hpp"
#include <array>
#include <regex>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint2> inputs : register(t0);
RWStructuredBuffer<uint4> output : register(u0);
[numthreads(GROUP, 1, 1)] void cs(uint t : SV_DispatchThreadID) {
  uint a = inputs[t].x, b = inputs[t].y;
#ifdef HALF
  output[t] = uint4(max(a, b), min(a, b), asuint16(abs(asfloat16(uint16_t(a)))), b);
#else
  output[t] = uint4(max(a, b), min(a, b), asuint(abs(asfloat(a))), b);
#endif
}
)hlsl";

// DxilConstants.h, OpCode
enum Op : UINT { IMul = 41, UMul = 42, UDiv = 43, UAddc = 44, USubb = 45 };

// the seed's disassembly with UMax(a, b) replaced by the first result of `op` on a and b, UMin(a, b) by its second,
// and each bit cast by the DXIL operation of its types
static std::string
converted(const Compiler &compiler, const std::string &seed, Op op, const char *name) {
  auto ir = dxil_disassemble(compiler, seed);
  const bool carries = op == UAddc || op == USubb;
  const std::string type = carries ? "%dx.types.i32c" : "%dx.types.twoi32";
  const std::string function = carries ? "@dx.op.binaryWithCarryOrBorrow.i32" : "@dx.op.binaryWithTwoOuts.i32";
  const std::regex marker(R"(  %(\d+) = call i32 @dx\.op\.binary\.i32\(i32 (39|40), i32 (%\d+), i32 (%\d+)\))");
  std::string out;
  UINT markers = 0;
  auto at = ir.cbegin();
  for (std::sregex_iterator match(ir.begin(), ir.end(), marker), end; match != end; ++match, markers++) {
    const auto &m = *match;
    const std::string n = m[1], result = "%two." + n;
    const bool second = m[2] == "40";
    out.append(at, m[0].first);
    out += "  " + result + " = call " + type + " " + function + "(i32 " + std::to_string(op) + ", i32 " + m[3].str() +
           ", i32 " + m[4].str() + ")\n";
    if (carries && second)
      out += "  %bit." + n + " = extractvalue " + type + " " + result + ", 1\n  %" + n + " = zext i1 %bit." + n +
             " to i32";
    else
      out += "  %" + n + " = extractvalue " + type + " " + result + ", " + (second ? "1" : "0");
    at = m[0].second;
  }
  out.append(at, ir.cend());
  if (!expect(markers == 2, "%s: the seed has %u UMax and UMin calls, not one of each", name, markers))
    return {};
  struct Rewrite {
    const char *from, *to;
  };
  const Rewrite rewrites[] = {
      {R"(%(\d+) = bitcast i32 (%\d+) to float)", "%$1 = call float @dx.op.bitcastI32toF32(i32 126, i32 $2)"},
      {R"(%(\d+) = bitcast float (%\d+) to i32)", "%$1 = call i32 @dx.op.bitcastF32toI32(i32 127, float $2)"},
      {R"(%(\d+) = bitcast i16 (%\d+) to half)", "%$1 = call half @dx.op.bitcastI16toF16(i32 124, i16 $2)"},
      {R"(%(\d+) = bitcast half (%\d+) to i16)", "%$1 = call i16 @dx.op.bitcastF16toI16(i32 125, half $2)"},
      {R"(%dx\.types\.Handle = type \{ i8\* \})",
       "%dx.types.Handle = type { i8* }\n%dx.types.twoi32 = type { i32, i32 }\n%dx.types.i32c = type { i32, i1 }"},
  };
  for (auto &rewrite : rewrites)
    out = std::regex_replace(out, std::regex(rewrite.from), rewrite.to);
  // the declaration of the marker's function, which nothing calls now, becomes those of the operations
  const std::string declared = "declare i32 @dx.op.binary.i32(i32, i32, i32) #0";
  const bool half = out.find("@dx.op.bitcastI16toF16") != std::string::npos;
  auto declaration = out.find(declared);
  if (!expect(declaration != std::string::npos, "%s: the seed does not declare its marker's function", name) ||
      !expect(
          out.find(half ? "@dx.op.bitcastF16toI16" : "@dx.op.bitcastF32toI32") != std::string::npos &&
              (half || out.find("@dx.op.bitcastI32toF32") != std::string::npos),
          "%s: the seed has no bit cast each way", name
      ))
    return {};
  out.replace(
      declaration, declared.size(),
      "declare " + type + " " + function + "(i32, i32, i32) #0\n" +
          (half ? "declare half @dx.op.bitcastI16toF16(i32, i16) #0\ndeclare i16 @dx.op.bitcastF16toI16(i32, half) #0"
                : "declare float @dx.op.bitcastI32toF32(i32, i32) #0\ndeclare i32 @dx.op.bitcastF32toI32(i32, float) "
                  "#0")
  );
  return dxil_assemble(out, name);
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: the operations are DXIL's, which needs DXC\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
  const bool native16 =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4))) &&
      options4.Native16BitShaderOpsSupported;

  // every pair of values at the ends of each operation's ranges, and floats of each class
  const UINT values[] = {0,          1,          2,          3,          7,          0xffff,     0x10000,
                         0x12345678, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 0x3f800000,
                         0xbf800000, 0x7f800000, 0xff800000, 0x00800000, 0xc2c80000, 0x0000bc00, 0x0000fc00};
  const UINT group = 64;
  using Pair = std::array<UINT, 2>;
  using Result = std::array<UINT, 4>;
  std::vector<Pair> inputs;
  for (UINT a : values)
    for (UINT b : values)
      inputs.push_back({a, b});
  const UINT cases = UINT(inputs.size());
  inputs.resize((inputs.size() + group - 1) / group * group, Pair{1, 1});
  const UINT total = UINT(inputs.size());
  const UINT64 input_bytes = UINT64(total) * sizeof(Pair), bytes = UINT64(total) * sizeof(Result);

  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(root && input && initial && output && readback, "the root signature and the buffers"))
    return verdict();
  void *mapped;
  CHECK(input->Map(0, nullptr, &mapped));
  memcpy(mapped, inputs.data(), input_bytes);
  input->Unmap(0, nullptr);
  // no operation gives a result of all ones in every component: a store that did not happen shows
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  initial->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  struct Variant {
    Op op;
    const char *name;
    bool half;
  };
  const Variant variants[] = {{IMul, "IMul", false},   {UMul, "UMul", false},   {UDiv, "UDiv", false},
                              {UAddc, "UAddc", false}, {USubb, "USubb", false}, {UMul, "UMul with 16-bit casts", true}};
  for (auto &variant : variants) {
    if (variant.half && !native16) {
      printf("not run: %s, the device has no 16-bit shader operations\n", variant.name);
      continue;
    }
    step("%s", variant.name);
    std::vector<std::string> defines = {"GROUP=" + std::to_string(group)};
    if (variant.half)
      defines.push_back("HALF=1");
    auto seed = compiler.compile(
        hlsl, "cs", variant.half ? "cs_6_2" : "cs_6_0", defines,
        variant.half ? std::vector<std::wstring>{L"-enable-16bit-types"} : std::vector<std::wstring>{}
    );
    if (!expect(!seed.empty(), "%s: the seed does not compile", variant.name))
      continue;
    auto code = converted(compiler, seed, variant.op, variant.name);
    if (code.empty())
      continue;
    ComPtr<ID3D12PipelineState> pipeline;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{root.Get(), bytecode(code)};
    const HRESULT created = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
    if (!expect(SUCCEEDED(created), "%s: the pipeline gives %#lx", variant.name, created))
      continue;
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipeline.Get()));
    list->CopyResource(output.Get(), initial.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->Dispatch(total / group, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    Result *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT mismatches = 0;
    for (UINT i = 0; i < cases; i++) {
      const UINT a = inputs[i][0], b = inputs[i][1];
      UINT first = 0, second = 0;
      switch (variant.op) {
      case IMul: {
        const INT64 product = INT64(INT(a)) * INT64(INT(b));
        first = UINT(UINT64(product) >> 32), second = UINT(product);
        break;
      }
      case UMul: {
        const UINT64 product = UINT64(a) * b;
        first = UINT(product >> 32), second = UINT(product);
        break;
      }
      case UDiv:
        first = b ? a / b : 0xffffffff, second = b ? a % b : 0xffffffff;
        break;
      case UAddc:
        first = a + b, second = first < a;
        break;
      case USubb:
        first = a - b, second = a < b;
        break;
      }
      // the float's bits without its sign, where the float is a zero, a normal number or an infinity
      const UINT width = variant.half ? 16 : 32, fraction_bits = variant.half ? 10 : 23;
      const UINT bits = variant.half ? a & 0xffff : a, sign = 1u << (width - 1);
      const UINT exponent_mask = (1u << (width - 1 - fraction_bits)) - 1, fraction_mask = (1u << fraction_bits) - 1;
      const UINT exponent = bits >> fraction_bits & exponent_mask, fraction = bits & fraction_mask;
      const bool held = !fraction || (exponent != 0 && exponent != exponent_mask);
      if ((got[i][0] != first || got[i][1] != second || (held && got[i][2] != (bits & ~sign)) || got[i][3] != b) &&
          mismatches++ < 8)
        expect(
            false, "%s of %#x and %#x: %#x, %#x, cast %#x, last %#x, expected %#x, %#x, cast %#x%s, last %#x",
            variant.name, a, b, got[i][0], got[i][1], got[i][2], got[i][3], first, second, bits & ~sign,
            held ? "" : " (not held)", b
        );
    }
    expect(!mismatches, "%s: %u of %u results are wrong", variant.name, mismatches, cases);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
