import Metal
// Metal compiler bugs in 32-bit integer code that tests/fuzz.py found through Mullion, each shown in plain Metal source
// with no Mullion in it, against what the CPU computes. on an M6 with macOS 27.3 each has given wrong results (the
// division for some divisors and program shapes only); a release that fixes one prints "ok" for it, and its
// workaround in Mullion may then go (docs/workarounds.md).
//
// 1. a value Metal can tell is all ones or zero (a sign-extended bool) is taken as 16 bits wide by clz and popcount:
//    clz(c == 0 ? ~0u : 0u) is 16 for c = 0, popcount 16. (airconv: clz and popcount of reversed bits)
// 2. and by min and max of two such values and a third: min(min(v1, v2), a) is 0xffff. (not worked around)
// 3. a constant divided by a variable is wrong for many divisors, and for a divisor in some threads and not others:
//    0xffffffff / 3 gave 0x55555561, 0xfffffffe / 3 0x55555555. checked here over 2^20 threads' divisors. (airconv: the
//    dividend through reverse_bits twice)
let device = MTLCreateSystemDefaultDevice()!
let source = """
#include <metal_stdlib>
using namespace metal;
kernel void k(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {
  uint c = in[0], a = in[1];
  uint v = c == 0 ? 0xffffffffu : 0u;
  uint v1 = c != 30 ? 0xffffffffu : 0u, v2 = c != 31 ? 0xffffffffu : 0u;
  if (i == 0)
    for (uint k = 0; k < 4; k++)
      out[k] = k == 0 ? clz(v) : k == 1 ? popcount(v) : k == 2 ? min(min(v1, v2), a) : max(v1, max(v2, a));
  uint divisor = in[3 + i];
  out[4 + i * 2] = 0xffffffffu / divisor;
  out[5 + i * 2] = 0xfffffffeu / divisor;
}
"""
let lib = try! device.makeLibrary(source: source, options: nil)
let pso = try! device.makeComputePipelineState(function: lib.makeFunction(name: "k")!)
let threads = 1 << 20
var input: [UInt32] = [0, 0x8000_0000, 0] + (0..<threads).map { UInt32($0 % 100_000 + 1) }
let inb = device.makeBuffer(bytes: &input, length: input.count * 4, options: .storageModeShared)!
let outb = device.makeBuffer(length: (4 + threads * 2) * 4, options: .storageModeShared)!
let q = device.makeCommandQueue()!, cb = q.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
e.setComputePipelineState(pso); e.setBuffer(inb, offset: 0, index: 0); e.setBuffer(outb, offset: 0, index: 1)
e.dispatchThreads(MTLSize(width: threads, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
let out = outb.contents().bindMemory(to: UInt32.self, capacity: 4 + threads * 2)
let checks: [(String, UInt32)] = [
  ("1. clz(sign-extended bool ~0)", 0), ("1. popcount(sign-extended bool ~0)", 32),
  ("2. min(min(~0, ~0), 0x80000000)", 0x8000_0000), ("2. max(~0, max(~0, 0x80000000))", 0xffff_ffff),
]
print(device.name)
for (n, (name, want)) in checks.enumerated() {
  let got = out[n]
  print(got == want ? "ok   " : "WRONG", name, "= 0x" + String(got, radix: 16), got == want ? "" : "(want 0x\(String(want, radix: 16)))")
}
for (k, dividend) in [UInt32(0xffff_ffff), 0xffff_fffe].enumerated() {
  var wrong = 0, first = ""
  for i in 0..<threads where out[4 + i * 2 + k] != dividend / input[3 + i] {
    if wrong == 0 { first = " (first: / \(input[3 + i]) = 0x\(String(out[4 + i * 2 + k], radix: 16)), want 0x\(String(dividend / input[3 + i], radix: 16)))" }
    wrong += 1
  }
  print(wrong == 0 ? "ok   " : "WRONG", "3. 0x\(String(dividend, radix: 16)) / divisor:", wrong, "of", threads, "wrong" + first)
}
