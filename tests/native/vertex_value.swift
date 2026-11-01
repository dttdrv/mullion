// whether a fragment function reads an input at each of its triangle's vertices (MSL 4's vertex_value<T>, which
// airconv builds GetAttributeAtVertex and a flat input beside it on, and D3D12_FEATURE_DATA_D3D12_OPTIONS3's
// BarycentricsSupported promises). one triangle whose vertices output 1, 3 and 5 (and 7, 8 and 9 as a uint) covers
// a pixel whose shader returns the three values it reads, and the first vertex's uint read the same way. the
// recorded result is the required one. macOS 27.2 (26B5112a) on an Apple M4 returns other values, the bits 1, 1 and 0
// (1e-45 1e-45 0.0) whatever the vertices hold, so winemetal draws the same triangle before the device reports
// barycentrics (vertex_values_work, winemetal_unix.c)
import Metal

let device = MTLCreateSystemDefaultDevice()!
FileHandle.standardError.write("\(device.name)\n".data(using: .utf8)!)
let source = """
  #include <metal_stdlib>
  using namespace metal;
  struct V { float4 p [[position]]; float a [[user(a), flat]]; uint f [[user(f), flat]]; };
  vertex V vs(uint id [[vertex_id]]) {
    float2 q = float2((id << 1) & 2, id & 2);
    V v;
    v.p = float4(q * float2(1, -1) + float2(-0.9, 0.9), 0.5, 1);
    v.a = 1 + id * 2;
    v.f = 7 + id;
    return v;
  }
  struct In { float4 p [[position]]; vertex_value<float> a [[user(a)]]; vertex_value<uint> f [[user(f)]]; };
  fragment float4 ps(In in [[stage_in]]) {
    return float4(in.a.get(vertex_index::first), in.a.get(vertex_index::second), in.a.get(vertex_index::third),
                  float(in.f.get(vertex_index::first)));
  }
  """
let options = MTLCompileOptions()
options.languageVersion = .version4_0
let library = try! device.makeLibrary(source: source, options: options)
let pipeline = MTLRenderPipelineDescriptor()
pipeline.vertexFunction = library.makeFunction(name: "vs")
pipeline.fragmentFunction = library.makeFunction(name: "ps")
pipeline.colorAttachments[0].pixelFormat = .rgba32Float
let state = try! device.makeRenderPipelineState(descriptor: pipeline)
let target = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: 8, height: 8, mipmapped: false)
target.usage = .renderTarget
target.storageMode = .shared
let texture = device.makeTexture(descriptor: target)!
let pass = MTLRenderPassDescriptor()
pass.colorAttachments[0].texture = texture
pass.colorAttachments[0].loadAction = .clear
pass.colorAttachments[0].storeAction = .store
let commands = device.makeCommandQueue()!.makeCommandBuffer()!
let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
encoder.setRenderPipelineState(state)
encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
encoder.endEncoding()
commands.commit()
commands.waitUntilCompleted()
var pixel = [Float](repeating: 0, count: 4)
texture.getBytes(&pixel, bytesPerRow: 8 * 16, from: MTLRegionMake2D(1, 1, 1, 1), mipmapLevel: 0)
print("the vertices' values read at pixel (1, 1): \(pixel.map { String($0) }.joined(separator: " "))")
