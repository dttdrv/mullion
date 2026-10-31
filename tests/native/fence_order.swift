// observation: on Metal 4, a fence that one compute encoder updates after its dispatches and the next waits for before
// its own orders the two, with what the first wrote visible to the second, as Mullion orders its passes
// (d3d12_command_queue.cpp NextSync; winemetal_unix.c updateFence/waitForFence on MTLStageDispatch | MTLStageBlit |
// MTLStageAccelerationStructure). each round fills a buffer with the round's number in one encoder and copies it out
// in the next; in order, every copied word is the round's. without anything between them the two may overlap, which
// shows the rounds are long enough to catch a missing order. Metal 3's MTLFence between compute encoders is the
// baseline
import Metal

let device = MTLCreateSystemDefaultDevice()!
let count = 1 << 22, length = count * 4, rounds = 64
let library = try device.makeLibrary(source: """
  #include <metal_stdlib>
  using namespace metal;
  kernel void fill(device uint *data [[buffer(0)]], constant uint &value [[buffer(1)]], uint at [[thread_position_in_grid]]) {
    uint v = value;
    for (uint i = 0; i < 8; i++) v = v * 1 + (data[at] & 0);
    data[at] = v;
  }
  kernel void copy(device const uint *data [[buffer(0)]], device uint *out [[buffer(1)]], uint at [[thread_position_in_grid]]) {
    out[at] = data[at];
  }
  kernel void read(texture2d<uint, access::read> pixels [[texture(0)]], device uint *out [[buffer(1)]],
                   uint at [[thread_position_in_grid]]) {
    out[at] = pixels.read(uint2(at % 2048, at / 2048)).x;
  }
  struct V { float4 position [[position]]; };
  vertex V cover(uint id [[vertex_id]]) { return {float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1)}; }
  fragment uint paint(constant uint &value [[buffer(1)]]) {
    uint v = value;
    for (uint i = 0; i < 64; i++) v = v * 1 + i * 0;
    return v;
  }
  """, options: nil)
let fill = try device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let copy = try device.makeComputePipelineState(function: library.makeFunction(name: "copy")!)
let read = try device.makeComputePipelineState(function: library.makeFunction(name: "read")!)
let renderDescriptor = MTLRenderPipelineDescriptor()
renderDescriptor.vertexFunction = library.makeFunction(name: "cover")
renderDescriptor.fragmentFunction = library.makeFunction(name: "paint")
renderDescriptor.colorAttachments[0].pixelFormat = .r32Uint
let paint = try device.makeRenderPipelineState(descriptor: renderDescriptor)
let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Uint, width: 2048, height: 2048, mipmapped: false)
textureDescriptor.usage = [.renderTarget, .shaderRead]
textureDescriptor.storageMode = .private
let pixels = device.makeTexture(descriptor: textureDescriptor)!
let data = device.makeBuffer(length: length, options: .storageModePrivate)!
let out = device.makeBuffer(length: length, options: .storageModeShared)!
let values = device.makeBuffer(length: rounds * 4, options: .storageModeShared)!
for round in 0 ..< rounds { values.contents().storeBytes(of: UInt32(round + 1), toByteOffset: round * 4, as: UInt32.self) }
let grid = MTLSize(width: count, height: 1, depth: 1), group = MTLSize(width: 256, height: 1, depth: 1)

func check(_ round: Int) -> Bool {
  let words = out.contents().bindMemory(to: UInt32.self, capacity: count)
  return (0 ..< count).allSatisfy { words[$0] == UInt32(round + 1) }
}

// Metal 3: a fence between two compute encoders of one command buffer
let queue = device.makeCommandQueue()!
var ordered = 0
for round in 0 ..< rounds {
  let commands = queue.makeCommandBuffer()!
  let fence = device.makeFence()!
  let first = commands.makeComputeCommandEncoder()!
  first.setComputePipelineState(fill)
  first.setBuffer(data, offset: 0, index: 0)
  first.setBuffer(values, offset: round * 4, index: 1)
  first.dispatchThreads(grid, threadsPerThreadgroup: group)
  first.updateFence(fence)
  first.endEncoding()
  let second = commands.makeComputeCommandEncoder()!
  second.waitForFence(fence)
  second.setComputePipelineState(copy)
  second.setBuffer(data, offset: 0, index: 0)
  second.setBuffer(out, offset: 0, index: 1)
  second.dispatchThreads(grid, threadsPerThreadgroup: group)
  second.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
  ordered += check(round) ? 1 : 0
}
print("Metal 3, fence: in order every time: \(ordered == rounds)")

guard #available(macOS 26.0, *), let queue4 = device.makeMTL4CommandQueue() else { exit(0) }
let residency = try device.makeResidencySet(descriptor: MTLResidencySetDescriptor())
[data, out, values].forEach { residency.addAllocation($0) }
residency.addAllocation(pixels)
residency.commit()
queue4.addResidencySet(residency)
let tableDescriptor = MTL4ArgumentTableDescriptor()
tableDescriptor.maxBufferBindCount = 2
tableDescriptor.maxTextureBindCount = 1
let tables = [try device.makeArgumentTable(descriptor: tableDescriptor), try device.makeArgumentTable(descriptor: tableDescriptor)]
let allocator = device.makeCommandAllocator()!
let done = device.makeSharedEvent()!
var signalled: UInt64 = 0
let stages: MTLStages = [.dispatch, .blit, .accelerationStructure]

enum Between: String, CaseIterable {
  case nothing = "nothing", fence = "fence", fenceBuffers = "fence, a command buffer each, committed together",
       barrier = "queue barrier"
}
for between in Between.allCases {
  var ordered = 0
  for round in 0 ..< rounds {
    let fence = device.makeFence()!
    allocator.reset()
    var buffers: [MTL4CommandBuffer] = []
    var commands = device.makeCommandBuffer()!
    commands.beginCommandBuffer(allocator: allocator)
    tables[0].setAddress(data.gpuAddress, index: 0)
    tables[0].setAddress(values.gpuAddress + UInt64(round * 4), index: 1)
    let first = commands.makeComputeCommandEncoder()!
    first.setComputePipelineState(fill)
    first.setArgumentTable(tables[0])
    first.dispatchThreads(threadsPerGrid: grid, threadsPerThreadgroup: group)
    if between == .fence || between == .fenceBuffers { first.updateFence(fence, afterEncoderStages: stages) }
    first.endEncoding()
    if between == .fenceBuffers {
      commands.endCommandBuffer()
      buffers.append(commands)
      commands = device.makeCommandBuffer()!
      commands.beginCommandBuffer(allocator: allocator)
    }
    tables[1].setAddress(data.gpuAddress, index: 0)
    tables[1].setAddress(out.gpuAddress, index: 1)
    let second = commands.makeComputeCommandEncoder()!
    if between == .fence || between == .fenceBuffers { second.waitForFence(fence, beforeEncoderStages: stages) }
    if between == .barrier {
      second.barrier(afterQueueStages: .dispatch, beforeStages: .dispatch, visibilityOptions: .device)
    }
    second.setComputePipelineState(copy)
    second.setArgumentTable(tables[1])
    second.dispatchThreads(threadsPerGrid: grid, threadsPerThreadgroup: group)
    second.endEncoding()
    commands.endCommandBuffer()
    buffers.append(commands)
    queue4.commit(buffers)
    signalled += 1
    queue4.signalEvent(done, value: signalled)
    while done.signaledValue < signalled { usleep(50) }
    ordered += check(round) ? 1 : 0
  }
  print("Metal 4, \(between.rawValue): in order every time: \(ordered == rounds)" + (between == .nothing ? "" : ""))
  if ordered != rounds { FileHandle.standardError.write("  \(between.rawValue): \(ordered) / \(rounds)\n".data(using: .utf8)!) }
}

// a render pass that paints a target, then a compute pass that reads it: the render encoder updates the fence after
// its fragments, as Mullion's do (d3d12_command_queue.cpp, updateFence(sync.update, WMTRenderStageFragment))
for fenced in [false, true] {
  var ordered = 0
  for round in 0 ..< rounds {
    let fence = device.makeFence()!
    allocator.reset()
    let commands = device.makeCommandBuffer()!
    commands.beginCommandBuffer(allocator: allocator)
    let pass = MTL4RenderPassDescriptor()
    pass.colorAttachments[0].texture = pixels
    pass.colorAttachments[0].loadAction = .dontCare
    pass.colorAttachments[0].storeAction = .store
    tables[0].setAddress(values.gpuAddress + UInt64(round * 4), index: 1)
    let render = commands.makeRenderCommandEncoder(descriptor: pass)!
    render.setRenderPipelineState(paint)
    render.setArgumentTable(tables[0], stages: .fragment)
    render.drawPrimitives(primitiveType: .triangle, vertexStart: 0, vertexCount: 3)
    if fenced { render.updateFence(fence, afterEncoderStages: .fragment) }
    render.endEncoding()
    tables[1].setTexture(pixels.gpuResourceID, index: 0)
    tables[1].setAddress(out.gpuAddress, index: 1)
    let second = commands.makeComputeCommandEncoder()!
    if fenced { second.waitForFence(fence, beforeEncoderStages: stages) }
    second.setComputePipelineState(read)
    second.setArgumentTable(tables[1])
    second.dispatchThreads(threadsPerGrid: grid, threadsPerThreadgroup: group)
    second.endEncoding()
    commands.endCommandBuffer()
    queue4.commit([commands])
    signalled += 1
    queue4.signalEvent(done, value: signalled)
    while done.signaledValue < signalled { usleep(50) }
    ordered += check(round) ? 1 : 0
  }
  print("Metal 4, render then compute, \(fenced ? "fence after fragments" : "nothing"): in order every time: \(ordered == rounds)")
  if ordered != rounds { FileHandle.standardError.write("  render \(fenced): \(ordered) / \(rounds)\n".data(using: .utf8)!) }
}
