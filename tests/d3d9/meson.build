# Direct3D 9's tests, built only when the build has Direct3D 9 (enable_d3d9), which is off unless a build asks
d3d9_test_deps = [cpp.find_library('d3d9'), cpp.find_library('d3d11'), cpp.find_library('dxgi')]

# 'present' presents to a real window, so it is a window test, as d3d11_swapchain is. two runs: one presents through
# GDI, whose window pixels the test reads, and one through the Metal view a game's window gets
d3d9_present = executable('d3d9_present', ['d3d9_present.cpp'], dependencies : d3d9_test_deps)
test('d3d9_present', d3d9_present, env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_present_metal_view', d3d9_present, args : ['--metal-view'], env : ['MULLION_SHADER_CACHE=0'],
     suite : 'window', timeout : 300)

# the fixed-function draw path: a triangle's pixels, and what the draw path refuses (both present to a window, so both
# are window tests, as d3d9_present is)
test('d3d9_triangle', executable('d3d9_triangle', ['d3d9_triangle.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_draw_contract', executable('d3d9_draw_contract', ['d3d9_draw_contract.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a fan keeps its first index as the centre of every triangle: nonidentity indices, a colour a corner, nonzero
# StartIndex and BaseVertexIndex, 32-bit indices past 65535, both windings, and scratch reuse
test('d3d9_fan', executable('d3d9_fan', ['d3d9_fan.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the render states and the surfaces a frame is drawn into: depth, cull, fill, blend and shade read back out of the
# pixels, and render targets, depth surfaces and the viewport read back the same way
test('d3d9_states', executable('d3d9_states', ['d3d9_states.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_targets', executable('d3d9_targets', ['d3d9_targets.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# every colour-write mask on every target, with all four channels preserved through sentinels and shader output,
# simultaneous different masks, sparse slots, state blocks, blending, Reset defaults and A8B8G8R8's channel order
test('d3d9_write_masks', executable('d3d9_write_masks', ['d3d9_write_masks.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the texture a textured draw samples and the stage that samples it: the texels a frame is made of, the stage's
# operations, arguments and alpha op, the sampler's addressing and filters, and what the device refuses
test('d3d9_texture', executable('d3d9_texture', ['d3d9_texture.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the levels a managed texture owns: the authored levels below level 0 survive a write and a read-only lock, an
# explicit GenerateMipSubLevels filters them from level 0, a DXT chain is the application's blocks, and the level
# count is the one the texture was made with
test('d3d9_mips', executable('d3d9_mips', ['d3d9_mips.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a game's own shader: the program it is made from and handed back, the stage that keeps it, its constant tables, the
# programs and constants the device refuses, and the draw it cannot make until the d3dbc translator lands
d3d9_shader_deps = d3d9_test_deps + [cpp.find_library('d3dcompiler_47')]
test('d3d9_shader', executable('d3d9_shader', ['d3d9_shader.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a game's own shaders drawing: a vertex declaration feeds a translated vertex shader, its program (hand-written, and
# Microsoft's compile of HLSL) and the pixel shader's draw a triangle whose pixels are read back
test('d3d9_shader_draw', executable('d3d9_shader_draw', ['d3d9_shader_draw.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a game's own shaders sampling a texture: the texels a textured quad draws, the sampler's filter and address modes at
# the pixels that tell them apart, a DXT1 texture's blocks, and the texel a stage with no texture samples
test('d3d9_texture_draw', executable('d3d9_texture_draw', ['d3d9_texture_draw.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the calls a title makes between its draws: DrawIndexedPrimitiveUP's arrays, the event query a frame waits on, and the
# state blocks a game saves and restores its state with
test('d3d9_device_calls', executable('d3d9_device_calls', ['d3d9_device_calls.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a resource kept after the application released its device: the child's reference keeps the device valid, and the
# device's own references do not keep the child and the device alive for good
test('d3d9_lifetime', executable('d3d9_lifetime', ['d3d9_lifetime.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a surface's container (its texture, its swap chain, or none) and a resource's private data
test('d3d9_containers', executable('d3d9_containers', ['d3d9_containers.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a window that is not one is refused, and Present after the window is destroyed does not crash (GDI present on i386
# copies into guest-addressable pages)
test('d3d9_hwnd', executable('d3d9_hwnd', ['d3d9_hwnd.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# ColorFill, StretchRect and UpdateSurface: the copies a game makes outside a draw, with the formats, pools and
# rectangles each takes and refuses
test('d3d9_transfer', executable('d3d9_transfer', ['d3d9_transfer.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a windowed chain resized and reformatted again: a live back buffer or depth surface refuses it, the state is lost,
# and the automatic depth-stencil surface follows the new size
test('d3d9_reset', executable('d3d9_reset', ['d3d9_reset.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# what a chain presents and a render target holds: GetFrontBufferData's frame shown, the FLIP and COPY swap effects, a
# multisampled DISCARD chain resolved by StretchRect and by Present, and a render target the CPU locks
test('d3d9_chain', executable('d3d9_chain', ['d3d9_chain.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the cooperative level a device reports, and what its draws and Present answer once its frames failed: the second run
# makes the run's first command buffer a failed one (MULLION_D3D11_FAULT_DEVICE), so the lost contract is exercised
# without a GPU fault
d3d9_lifecycle = executable('d3d9_lifecycle', ['d3d9_lifecycle.cpp'], dependencies : d3d9_test_deps)
test('d3d9_lifecycle', d3d9_lifecycle, env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_lifecycle_lost', d3d9_lifecycle, env : ['MULLION_SHADER_CACHE=0', 'MULLION_D3D11_FAULT_DEVICE=1'],
     suite : 'window', timeout : 300)
# the whole way back: lost, D3DERR_DEVICENOTRESET, Reset, and a frame drawn and presented again. The fault is the
# run's fourth completed command buffer, so the resources the recovery keeps (and the Present before the loss) are made
# on a device whose frames are live
test('d3d9_lifecycle_reset', d3d9_lifecycle,
     env : ['MULLION_SHADER_CACHE=0', 'MULLION_D3D11_FAULT_DEVICE=4'], args : ['--reset'], suite : 'window',
     timeout : 300)

# the pixels a program does not write: texkill's negative coordinate, D3DRS_ALPHATESTENABLE's comparison against
# D3DRS_ALPHAREF, and the fixed-function draw that asks for the test it has none of
test('d3d9_discard', executable('d3d9_discard', ['d3d9_discard.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the transform stage: a D3DFVF_XYZ vertex under the world, view and projection the device holds, the same pixels as
# the screen-space path under identity, an exactly measured translation, scale and rotation, the perspective divide, and
# the viewport's depth range
test('d3d9_transform', executable('d3d9_transform', ['d3d9_transform.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the lit vertex: global ambient, the material and a directional or point light's contribution against the documented
# equation, a point light's attenuation at two distances and its Range, the specular term D3DRS_SPECULARENABLE gates,
# LightEnable and an index past MaxActiveLights, and a state block carrying lights and material
test('d3d9_lighting', executable('d3d9_lighting', ['d3d9_lighting.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the caps a game plans its shaders from: the shader models the translator certifies, the counters the other tests
# certify, and the versions refused where a program above them is made
test('d3d9_caps', executable('d3d9_caps', ['d3d9_caps.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# cube textures, MSAA create/resolve, additional swap chains / streams / fog storage, and remaining vs_2_0 opcodes
test('d3d9_cube', executable('d3d9_cube', ['d3d9_cube.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_volume', executable('d3d9_volume', ['d3d9_volume.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_msaa', executable('d3d9_msaa', ['d3d9_msaa.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_extra', executable('d3d9_extra', ['d3d9_extra.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_ff_extra', executable('d3d9_ff_extra', ['d3d9_ff_extra.cpp'], dependencies : d3d9_shader_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_shader_opcodes', executable('d3d9_shader_opcodes', ['d3d9_shader_opcodes.cpp'],
     dependencies : d3d9_shader_deps), env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# D3DTSS_TEXTURETRANSFORMFLAGS: the projected coordinate's divisor (w, z, or the coordinate's own last component), a
# count the vertex cannot supply, and the wide declaration and stride a three- or four-component coordinate needs
test('d3d9_projected', executable('d3d9_projected', ['d3d9_projected.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the stream frequencies D3D9 accepts and the instanced draws they make: an INDEXEDDATA count on any stream, an
# INSTANCEDATA stream read at the instance's own vertices, and a non-indexed draw that is one instance
test('d3d9_streams', executable('d3d9_streams', ['d3d9_streams.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the topologies D3D9 has and D3D11 does not: a triangle fan through all four draws that take one, point fill's
# vertices as single pixels, ps_3_0's oC1 in the second of four render targets, and the additional chain's own back
# buffer, display mode, raster status and vertical blank
test('d3d9_topology', executable('d3d9_topology', ['d3d9_topology.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# the texture stages past the first two: a stage 2 that changes the colour, a stage 5 multiply, a complement
# argument, a D3DTA_TEMP result and D3DFVF_SPECULAR's colour, read back from the frame (d3d9_multi_stage.cpp). a
# fixed-function path that carries two stages only fails these
test('d3d9_multi_stage', executable('d3d9_multi_stage', ['d3d9_multi_stage.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# d3d9.dll's export table: every name a modern Windows d3d9.dll has resolves and is called safely (the PIX markers CS2 and
# Alien Swarm import, the validator, the PSGP and debug stubs, the 9On12 pair), and the create calls hand back live objects
test('d3d9_exports', executable('d3d9_exports', ['d3d9_exports.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# no test: the generator of the fixed-function shader headers. Regenerate only through
# tools/regen-fixed-function-shaders.sh, which rebuilds this executable from its current source before it runs and
# stamps the headers it writes; tools/check-fixed-function-shaders.sh fails on a header from a different source.
executable('fixed_function_shaders', ['fixed_function_shaders.cpp'],
           dependencies : d3d9_test_deps + [cpp.find_library('d3dcompiler_47')])

# a D3DFVF_XYZ vertex carries no colour: the fixed-function pipeline supplies D3D9's opaque white, a stage with no
# texture samples the white texel, and neither reads the next vertex's bytes as the colour
test('d3d9_position_colour', executable('d3d9_position_colour', ['d3d9_position_colour.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# every fixed-function stage samples the dimension its own texture is: a 2D texture at (u, v), a cube by a direction
# (so a +X direction reads +X and a +Y one reads +Y), a volume at (u, v, w) so a two-component coordinate reads slice
# 0, a stage with no texture the white texel, and mixed dimensions across stages of both the two-stage and eight-stage
# combiners, with a texture-type change between draws
test('d3d9_ff_sampling', executable('d3d9_ff_sampling', ['d3d9_ff_sampling.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# ps_1_4's texdepth writes the pixel's depth as clamp(x / min(y, 1), 0, 1): a draw with that pixel shader and no
# vertex shader has to be accepted, and the depth test has to see the value the register names
test('d3d9_texdepth', executable('d3d9_texdepth', ['d3d9_texdepth.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# ps_1_1's texkill discards a pixel when any component of t0 is negative: an FVF whose TEXCOORDSIZE4 names four
# coordinates has to deliver all four, so a negative z or w kills just as a negative x does
test('d3d9_texkill', executable('d3d9_texkill', ['d3d9_texkill.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a depth texture a pixel shader of model 2.0 and above samples answers the depth comparison, not the texel: the
# reference is the coordinate's third component (divided by the fourth for texldp), a point-filtered LEQUAL compare
# answers 1.0 when it is at most the texel's depth, and every written component takes the answer (Wine visual.c
# shadow_test, whose texld + texldp program reads a D16/D24S8 depth texture)
test('d3d9_shadow', executable('d3d9_shadow', ['d3d9_shadow.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# ps_1_x's cnd: ps_1_1 through 1_3 compare the alpha once, ps_1_4 each component of its source, and both select the
# first source only when the comparison is strictly above 0.5 (Wine visual.c cnd_test)
test('d3d9_shader_cnd', executable('d3d9_shader_cnd', ['d3d9_shader_cnd.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)

# a ps_3_0 may read any usage the vertex declaration carries, over a transformed (POSITIONT) vertex; the twelve
# draws are accepted and read the usage's value, as Wine's pretransformed_varying_test records from Windows
# (visual.c 8402 hung the device before)
test('d3d9_varyings', executable('d3d9_varyings', ['d3d9_varyings.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
