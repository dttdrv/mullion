import Metal
import Foundation
// whether GPU memory a residency set keeps resident costs the first frame after the GPU idles (llama.cpp #17766), and
// whether requestResidency() while idle avoids it. args: <GiB of buffers> <idle seconds> <repeats>
let wall = CommandLine.arguments.count > 4 && CommandLine.arguments[4] == "wall"
let gib = Int(CommandLine.arguments[1])!, idle = Double(CommandLine.arguments[2])!, repeats = Int(CommandLine.arguments[3])!
let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!
let rsd = MTLResidencySetDescriptor(); rsd.initialCapacity = gib * 4
let set = try! device.makeResidencySet(descriptor: rsd)
var buffers: [MTLBuffer] = []
for _ in 0..<(gib * 4) { let b = device.makeBuffer(length: 256 << 20, options: .storageModePrivate)!; buffers.append(b); set.addAllocation(b) }
set.commit(); queue.addResidencySet(set); set.requestResidency()
let lib = try! device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
kernel void touch(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i * 4096] += 1; }
""", options: nil)
let pso = try! device.makeComputePipelineState(function: lib.makeFunction(name: "touch")!)
func frame() -> Double {
  let cb = queue.makeCommandBuffer()!; let e = cb.makeComputeCommandEncoder()!; e.setComputePipelineState(pso)
  for b in buffers { e.setBuffer(b, offset: 0, index: 0); e.dispatchThreads(MTLSize(width: (256 << 20) / 16384, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)) }
  e.endEncoding(); let start = Date(); cb.commit(); cb.waitUntilCompleted()
  return wall ? Date().timeIntervalSince(start) * 1000 : (cb.gpuEndTime - cb.gpuStartTime) * 1000
}
for _ in 0..<5 { _ = frame() }
func trial(_ heartbeat: Bool) -> Double {
  let end = Date().addingTimeInterval(idle)
  if heartbeat { while Date() < end { set.requestResidency(); Thread.sleep(forTimeInterval: 0.25) } } else { Thread.sleep(forTimeInterval: idle) }
  return frame()
}
var steady: [Double] = [], cold: [Double] = [], warm: [Double] = []
for _ in 0..<repeats { steady.append(frame()); cold.append(trial(false)); warm.append(trial(true)) }
func s(_ v: [Double]) -> String { String(format: "median %.2f ms, max %.2f ms", v.sorted()[v.count / 2], v.max()!) }
print("\(gib) GiB, \(idle) s idle, \(repeats) repeats on \(device.name), \(wall ? "wall-clock commit to completion" : "GPU time")")
print("  back-to-back frame:         " + s(steady))
print("  first frame after idle:     " + s(cold))
print("  same, heartbeat while idle: " + s(warm))
