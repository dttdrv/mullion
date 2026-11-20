# metal-bench

Small Swift programs that measure a Metal behaviour a Mullion choice rests on, run directly on the Mac (no Wine):

```sh
swiftc -O compression.swift -o /tmp/compression && /tmp/compression
swiftc -O residency_idle.swift -o /tmp/residency_idle && /tmp/residency_idle 8 10 3 wall
```

- `compression.swift`: what a texture usage costs lossless compression. A full-screen pass fills a 4096² RGBA8
  texture, a second reads it 8 times per pixel; timed per usage. On an M6 (macOS 27.3) the read pass takes 2.8 ms
  with `PixelFormatView` and 0.8–1.2 ms without; `ShaderWrite` alone keeps compression. Why D3D11 textures ask for
  `PixelFormatView` only when one of their views needs it (src/d3d11/d3d11_resource_helper.cpp).
- `residency_idle.swift <GiB> <idle seconds> <repeats> [wall]`: the first submission after the GPU idles, with and
  without `requestResidency()` while idle. On an M6, 8 GiB, 10 s idle: 135 ms of wall time instead of 3, and 4.4 ms
  with the request. Why Mullion has a residency heartbeat (src/mullion/mullion_heartbeat.hpp).
