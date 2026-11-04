import Metal
import Foundation

// usage: rendercheck vs.metallib vs.refl ps.metallib ps.refl
// Constants: c[i] = (i + 0.5, i + 0.25, -(i + 0.75), i * 0.5 + 1), i[k] = (3 + k, k, 1, 0), b[k] = k even.
let device = MTLCreateSystemDefaultDevice()!
let args = CommandLine.arguments

struct Refl { var cb: [(slot: Int, offset: Int)] = []; var args: [(type: Int, slot: Int, offset: Int)] = []; var qwords = 0 }
func readRefl(_ path: String) -> Refl {
  var r = Refl()
  for line in (try! String(contentsOfFile: path)).split(separator: "\n") {
    let f = line.split(separator: " ").map { String($0) }
    if f[0] == "cb" { r.cb.append((Int(f[2])!, Int(f[3])!)) }
    if f[0] == "arg" { r.args.append((Int(f[1])!, Int(f[2])!, Int(f[3])!)) }
    if f[0] == "qwords" { r.qwords = Int(f[1])! }
  }
  return r
}

var floats = [Float](repeating: 0, count: 256 * 4)
for i in 0..<256 {
  floats[i * 4 + 0] = Float(i) + 0.5
  floats[i * 4 + 1] = Float(i) + 0.25
  floats[i * 4 + 2] = -(Float(i) + 0.75)
  floats[i * 4 + 3] = Float(i) * 0.5 + 1
}
var ints = [Int32](repeating: 0, count: 16 * 4)
for k in 0..<16 { ints[k * 4] = Int32(3 + k); ints[k * 4 + 1] = Int32(k); ints[k * 4 + 2] = 1 }
var bools = [UInt32](repeating: 0, count: 16)
for k in 0..<16 { bools[k] = k % 2 == 0 ? 1 : 0 }

let cbFloat = device.makeBuffer(bytes: floats, length: floats.count * 4, options: .storageModeShared)!
let cbInt = device.makeBuffer(bytes: ints, length: ints.count * 4, options: .storageModeShared)!
let cbBool = device.makeBuffer(bytes: bools, length: bools.count * 4, options: .storageModeShared)!
let cbRuntime = device.makeBuffer(length: 1024, options: .storageModeShared)!
let cbs = [cbFloat, cbInt, cbBool, cbRuntime]

let texDesc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: 2, height: 2, mipmapped: false)
let tex = device.makeTexture(descriptor: texDesc)!
let texels: [Float] = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6]
tex.replace(region: MTLRegionMake2D(0, 0, 2, 2), mipmapLevel: 0, withBytes: texels, bytesPerRow: 32)
let sd = MTLSamplerDescriptor()
sd.minFilter = .nearest
sd.magFilter = .nearest
sd.supportArgumentBuffers = true
let sampler = device.makeSamplerState(descriptor: sd)!

func tables(_ r: Refl) -> (MTLBuffer?, MTLBuffer?) {
  var cbTable: MTLBuffer? = nil
  if !r.cb.isEmpty {
    var entries = [UInt64](repeating: 0, count: (r.cb.map { $0.offset }.max() ?? 0) + 2)
    // a reference to a constant buffer is its address and its length, which reads are bounded by
    for e in r.cb {
      entries[e.offset] = cbs[e.slot].gpuAddress
      entries[e.offset + 1] = UInt64(cbs[e.slot].length)
    }
    cbTable = device.makeBuffer(bytes: entries, length: entries.count * 8, options: .storageModeShared)
  }
  var argTable: MTLBuffer? = nil
  if r.qwords > 0 {
    var entries = [UInt64](repeating: 0, count: r.qwords)
    for a in r.args {
      if a.type == 1 { entries[a.offset] = sampler.gpuResourceID._impl; entries[a.offset + 1] = sampler.gpuResourceID._impl }
      if a.type == 2 { entries[a.offset] = tex.gpuResourceID._impl }
    }
    argTable = device.makeBuffer(bytes: entries, length: entries.count * 8, options: .storageModeShared)
  }
  return (cbTable, argTable)
}

let vs = try! device.makeLibrary(URL: URL(fileURLWithPath: args[1])).makeFunction(name: "shader_main")!
let ps = try! device.makeLibrary(URL: URL(fileURLWithPath: args[3])).makeFunction(name: "shader_main")!
let desc = MTLRenderPipelineDescriptor()
desc.vertexFunction = vs
desc.fragmentFunction = ps
desc.colorAttachments[0].pixelFormat = .rgba32Float
let vd = MTLVertexDescriptor()
for a in vs.vertexAttributes ?? [] where a.isActive {
  vd.attributes[a.attributeIndex].format = .float4
  vd.attributes[a.attributeIndex].bufferIndex = 20
}
vd.layouts[20].stride = 16
desc.vertexDescriptor = vd
let pso = try! device.makeRenderPipelineState(descriptor: desc)

let rt = device.makeTexture(descriptor: {
  let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: 1, height: 1, mipmapped: false)
  d.usage = [.renderTarget]
  d.storageMode = .shared
  return d
}())!
let verts: [Float] = [-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1]
let vbuf = device.makeBuffer(bytes: verts, length: verts.count * 4, options: .storageModeShared)!

let queue = device.makeCommandQueue()!
let cmd = queue.makeCommandBuffer()!
let pass = MTLRenderPassDescriptor()
pass.colorAttachments[0].texture = rt
pass.colorAttachments[0].loadAction = .clear
pass.colorAttachments[0].clearColor = MTLClearColorMake(-7, -7, -7, -7)
pass.colorAttachments[0].storeAction = .store
let enc = cmd.makeRenderCommandEncoder(descriptor: pass)!
enc.setRenderPipelineState(pso)
enc.setVertexBuffer(vbuf, offset: 0, index: 20)
let (vcb, varg) = tables(readRefl(args[2]))
let (pcb, parg) = tables(readRefl(args[4]))
if let b = vcb { enc.setVertexBuffer(b, offset: 0, index: 0) }
if let b = varg { enc.setVertexBuffer(b, offset: 0, index: 1) }
if let b = pcb { enc.setFragmentBuffer(b, offset: 0, index: 0) }
if let b = parg { enc.setFragmentBuffer(b, offset: 0, index: 1) }
for b in cbs { enc.useResource(b, usage: .read, stages: [.vertex, .fragment]) }
enc.useResource(tex, usage: .read, stages: [.vertex, .fragment])
enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
enc.endEncoding()
cmd.commit()
cmd.waitUntilCompleted()
var out = [Float](repeating: 0, count: 4)
rt.getBytes(&out, bytesPerRow: 16, from: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0)
print(out.map { String(format: "%.5f", $0) }.joined(separator: " "))
