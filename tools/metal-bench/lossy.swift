// what lossy compression saves and where Metal takes it, for mullion::memory::Lossy (src/mullion/mullion_memory.hpp).
// `lossy` prints the sizes of 4096x4096 textures lossless and lossy; `lossy <pixel format raw value> <case>` makes one
// lossy 256x256 texture and exits 0 when Metal takes it: run it with MTL_DEBUG_LAYER=1, which ends it on a refusal.
// cases: plain write view rt shared msaa array 3d cube heap mips
import Metal
let device = MTLCreateSystemDefaultDevice()!
if CommandLine.arguments.count < 3 { sizes(); exit(0) }
let f = MTLPixelFormat(rawValue: UInt(CommandLine.arguments[1])!)!
let c = CommandLine.arguments[2]
let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: 256, height: 256, mipmapped: c == "mips")
d.usage = .shaderRead; d.storageMode = .private; d.compressionType = .lossy
switch c {
case "write": d.usage = [.shaderRead, .shaderWrite]
case "view": d.usage = [.shaderRead, .pixelFormatView]
case "rt": d.usage = [.shaderRead, .renderTarget]
case "shared": d.storageMode = .shared
case "msaa": d.textureType = .type2DMultisample; d.sampleCount = 4; d.usage = [.renderTarget]
case "array": d.textureType = .type2DArray; d.arrayLength = 4
case "3d": d.textureType = .type3D; d.depth = 4
case "cube": d.textureType = .typeCube
case "heap":
  let hd = MTLHeapDescriptor(); hd.size = 16 << 20; hd.storageMode = .private; hd.type = .placement
  let t = device.makeHeap(descriptor: hd)!.makeTexture(descriptor: d, offset: 0); exit(t?.compressionType == .lossy ? 0 : 2)
default: break
}
let t = device.makeTexture(descriptor: d)
exit(t?.compressionType == .lossy ? 0 : 2)

func sizes() {
  for (name, f) in [("rgba8", MTLPixelFormat.rgba8Unorm), ("rgba16f", .rgba16Float), ("r32f", .r32Float)] {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: 4096, height: 4096, mipmapped: false)
    d.usage = [.shaderRead, .renderTarget]; d.storageMode = .private
    let lossless = device.heapTextureSizeAndAlign(descriptor: d).size
    d.compressionType = .lossy
    print(name, lossless >> 20, "MB lossless,", device.heapTextureSizeAndAlign(descriptor: d).size >> 20, "MB lossy")
  }
}
