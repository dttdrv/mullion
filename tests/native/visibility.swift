// what Metal does with visibility counts, which the occlusion queries of both libraries rest on
// (src/d3d12/d3d12_command_allocator.hpp kVisibilitySegments). Apple documents one limit, the offset ("Maximum
// visibility query offset", Metal feature set tables: 256 KB from Apple7); this measures that one and the one Apple
// does not document: how many counting segments of one render pass keep their count. every segment counts at its
// own offset of a buffer filled with a sentinel, and draws `samples` samples.
import Metal

let device = MTLCreateSystemDefaultDevice()!
FileHandle.standardError.write("\(device.name)\n".data(using: .utf8)!)
let library = try! device.makeLibrary(source: """
  #include <metal_stdlib>
  using namespace metal;
  [[vertex]] float4 vs(uint id [[vertex_id]]) { return float4(float((id << 1) & 2) * 2 - 1, 1 - float(id & 2) * 2, 0, 1); }
  [[fragment]] float4 fs() { return float4(1); }
  """, options: nil)
let pipelineDescriptor = MTLRenderPipelineDescriptor()
pipelineDescriptor.vertexFunction = library.makeFunction(name: "vs")
pipelineDescriptor.fragmentFunction = library.makeFunction(name: "fs")
pipelineDescriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
let pipeline = try! device.makeRenderPipelineState(descriptor: pipelineDescriptor)
let side = 8, samples = UInt64(side * side)
let targetDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: side, height: side, mipmapped: false)
targetDescriptor.usage = .renderTarget
let target = device.makeTexture(descriptor: targetDescriptor)!
let sentinel: UInt64 = 0xAAAA_AAAA_AAAA_AAAA, words = 1 << 17
let buffer = device.makeBuffer(length: words * 8, options: .storageModeShared)!
let word = buffer.contents().bindMemory(to: UInt64.self, capacity: words)
let queue = device.makeCommandQueue()!

// one render pass that counts at each offset in turn; drawn: whether that segment draws
func pass(_ offsets: [Int], drawn: (Int) -> Bool = { _ in true }, off: Bool = false) {
  let descriptor = MTLRenderPassDescriptor()
  descriptor.colorAttachments[0].texture = target
  descriptor.colorAttachments[0].loadAction = .clear
  descriptor.visibilityResultBuffer = buffer
  let commands = queue.makeCommandBuffer()!
  let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
  encoder.setRenderPipelineState(pipeline)
  for (segment, offset) in offsets.enumerated() {
    if off { encoder.setVisibilityResultMode(.disabled, offset: 0) }
    encoder.setVisibilityResultMode(.counting, offset: offset)
    if drawn(segment) { encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3) }
  }
  encoder.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
}
func fill() { for i in 0 ..< words { word[i] = sentinel } }

print("a count at one offset of a pass lands at:")
for offset in [8, 65528, 65536, 262136, 262144, 524288] {
  fill()
  pass([offset])
  print("  \(offset): \((0 ..< words).filter { word[$0] == samples }.map { String($0 * 8) })")
}

// the runs of segments by what their word holds after `runs` passes over the same memory (of the segments that
// draw: one without a draw has nothing to tell them apart by)
func kinds(_ name: String, count: Int, spacing: Int = 8, every: Int = 1, off: Bool = false, runs: UInt64 = 1) {
  let offsets = (0 ..< count).map { 8 + $0 * spacing }
  fill()
  for _ in 0 ..< runs { pass(offsets, drawn: { $0 % every == 0 }, off: off) }
  var found: [(kind: String, first: Int, last: Int)] = []
  for segment in stride(from: 0, to: count, by: every) {
    let got = word[offsets[segment] / 8]
    let kind = got == samples ? "stored" : got == sentinel &+ runs &* samples ? "added" : got == sentinel ? "untouched" : got == 0 ? "zero" : "other"
    if found.last?.kind == kind { found[found.count - 1].last = segment } else { found.append((kind, segment, segment)) }
  }
  print("  \(name): " + found.map { "\($0.first)...\($0.last) \($0.kind)" }.joined(separator: ", "))
}
print("the segments of one pass, by what is at their offset afterwards:")
kinds("20000 segments", count: 20000)
kinds("the same pass twice", count: 20000, runs: 2)
kinds("offsets 16 bytes apart", count: 16000, spacing: 16)
kinds("counting off at offset 0 before each, as the library encodes a slot", count: 20000, off: true)
kinds("the same twice", count: 20000, off: true, runs: 2)
kinds("the same, a draw in every second segment only (of those)", count: 20000, every: 2, off: true)
kinds("4096 segments twice", count: 4096, runs: 2)
kinds("4097 segments twice", count: 4097, runs: 2)
