// observation: on Metal 3, a command buffer that encodes a shared event signal after all its work has that work done,
// and what it wrote visible to the CPU, once the CPU sees the event's value, before Metal calls the buffer completed,
// as Mullion's Metal 3 queue takes it (d3d12_command_queue.cpp EventWait, winemetal_unix.c
// MTLCommandBuffer_waitUntilSignaled). each round fills a private buffer with the round's number in a long compute
// pass and copies it into a shared buffer, clears a target to the number in a render pass and copies the target into
// another shared buffer, and signals; the CPU waits for the value and reads at once. a signal encoded before the work
// lets the CPU read before it is done, which shows the rounds are long enough to catch a read too early. the times,
// on stderr, are not part of the observation: a buffer with one clear, from commit to the event and to
// waitUntilCompleted's return; and what a signal at the end of each of 256 small command buffers adds to their GPU
// time (some 17 us a buffer on an M4), which is why Mullion signals only the buffers the CPU is likely to wait for
import Metal

let device = MTLCreateSystemDefaultDevice()!
let count = 1 << 22, length = count * 4, rounds = 64, side = 1024
let library = try device.makeLibrary(source: """
  #include <metal_stdlib>
  using namespace metal;
  kernel void fill(device uint *data [[buffer(0)]], constant uint &value [[buffer(1)]], uint at [[thread_position_in_grid]]) {
    uint v = value;
    for (uint i = 0; i < 64; i++) v = v * 1 + (data[(at * 7 + i * 4099) & \(count - 1)] & 0);
    data[at] = v;
  }
  """, options: nil)
let fill = try device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: side, height: side, mipmapped: false)
textureDescriptor.usage = [.renderTarget]
textureDescriptor.storageMode = .private
let target = device.makeTexture(descriptor: textureDescriptor)!
let data = device.makeBuffer(length: length, options: .storageModePrivate)!
let out = device.makeBuffer(length: length, options: .storageModeShared)!
let texels = device.makeBuffer(length: side * side * 4, options: .storageModeShared)!
let values = device.makeBuffer(length: rounds * 4, options: .storageModeShared)!
for round in 0 ..< rounds { values.contents().storeBytes(of: UInt32(round + 1), toByteOffset: round * 4, as: UInt32.self) }
let grid = MTLSize(width: count, height: 1, depth: 1), group = MTLSize(width: 256, height: 1, depth: 1)
let queue = device.makeCommandQueue()!
let event = device.makeSharedEvent()!
var value: UInt64 = 0

func check(_ round: Int) -> Bool {
  let words = out.contents().bindMemory(to: UInt32.self, capacity: count)
  let floats = texels.contents().bindMemory(to: Float.self, capacity: side * side)
  return stride(from: 0, to: count, by: 4099).allSatisfy { words[$0] == UInt32(round + 1) } &&
    words[count - 1] == UInt32(round + 1) &&
    stride(from: 0, to: side * side, by: 1031).allSatisfy { floats[$0] == Float(round + 1) }
}

for signalFirst in [false, true] {
  var ordered = 0, before = 0
  for round in 0 ..< rounds {
    let commands = queue.makeCommandBuffer()!
    value += 1
    if signalFirst { commands.encodeSignalEvent(event, value: value) }
    let compute = commands.makeComputeCommandEncoder()!
    compute.setComputePipelineState(fill)
    compute.setBuffer(data, offset: 0, index: 0)
    compute.setBuffer(values, offset: round * 4, index: 1)
    compute.dispatchThreads(grid, threadsPerThreadgroup: group)
    compute.endEncoding()
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = target
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColor(red: Double(round + 1), green: 0, blue: 0, alpha: 0)
    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
    let blit = commands.makeBlitCommandEncoder()!
    blit.copy(from: data, sourceOffset: 0, to: out, destinationOffset: 0, size: length)
    blit.copy(from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
              sourceSize: MTLSize(width: side, height: side, depth: 1), to: texels, destinationOffset: 0,
              destinationBytesPerRow: side * 4, destinationBytesPerImage: side * side * 4)
    blit.endEncoding()
    if !signalFirst { commands.encodeSignalEvent(event, value: value) }
    commands.commit()
    if !event.wait(untilSignaledValue: value, timeoutMS: 10000) { break }
    before += commands.status.rawValue < MTLCommandBufferStatus.completed.rawValue ? 1 : 0
    ordered += check(round) ? 1 : 0
    commands.waitUntilCompleted()
  }
  print("Metal 3, signal \(signalFirst ? "before" : "after") the work: read in full every time: \(ordered == rounds)")
  FileHandle.standardError.write("  signal \(signalFirst ? "first" : "last"): \(ordered) / \(rounds) in full, \(before) seen before Metal called the buffer completed\n".data(using: .utf8)!)
}

// times of a buffer with one clear: commit to the event, and commit to waitUntilCompleted, in separate buffers
func median(_ times: [Double]) -> Double { times.sorted()[times.count / 2] }
var toEvent: [Double] = [], toCompleted: [Double] = []
let small = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 64, height: 64, mipmapped: false)
small.usage = [.renderTarget]
small.storageMode = .private
let clearTarget = device.makeTexture(descriptor: small)!
for i in 0 ..< 400 {
  let commands = queue.makeCommandBuffer()!
  let pass = MTLRenderPassDescriptor()
  pass.colorAttachments[0].texture = clearTarget
  pass.colorAttachments[0].loadAction = .clear
  pass.colorAttachments[0].storeAction = .store
  commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
  value += 1
  commands.encodeSignalEvent(event, value: value)
  let start = DispatchTime.now().uptimeNanoseconds
  commands.commit()
  if i & 1 == 0 {
    _ = event.wait(untilSignaledValue: value, timeoutMS: 10000)
    toEvent.append(Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6)
    commands.waitUntilCompleted()
  } else {
    commands.waitUntilCompleted()
    toCompleted.append(Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6)
  }
}
FileHandle.standardError.write(String(format: "  one clear: commit to event %.3f ms, commit to waitUntilCompleted %.3f ms (medians of 200)\n",
                                      median(toEvent), median(toCompleted)).data(using: .utf8)!)

// the GPU time of 256 command buffers of one small dispatch each, without and with a signal at the end of each
var spans: [Bool: [Double]] = [false: [], true: []]
for _ in 0 ..< 5 {
  for signal in [false, true] {
    var buffers: [MTLCommandBuffer] = []
    for _ in 0 ..< 256 {
      let commands = queue.makeCommandBuffer()!
      let compute = commands.makeComputeCommandEncoder()!
      compute.setComputePipelineState(fill)
      compute.setBuffer(data, offset: 0, index: 0)
      compute.setBuffer(values, offset: 0, index: 1)
      compute.dispatchThreads(MTLSize(width: 4096, height: 1, depth: 1), threadsPerThreadgroup: group)
      compute.endEncoding()
      if signal { value += 1; commands.encodeSignalEvent(event, value: value) }
      commands.commit()
      buffers.append(commands)
    }
    buffers.last!.waitUntilCompleted()
    spans[signal]!.append((buffers.last!.gpuEndTime - buffers.first!.gpuStartTime) * 1000)
  }
}
FileHandle.standardError.write(String(format: "  256 small buffers: GPU %.3f ms without signals, %.3f ms with one each (medians of 5)\n",
                                      median(spans[false]!), median(spans[true]!)).data(using: .utf8)!)
