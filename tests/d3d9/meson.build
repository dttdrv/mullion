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

# the render states and the surfaces a frame is drawn into: depth, cull, fill, blend and shade read back out of the
# pixels, and render targets, depth surfaces and the viewport read back the same way
test('d3d9_states', executable('d3d9_states', ['d3d9_states.cpp'], dependencies : d3d9_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d9_targets', executable('d3d9_targets', ['d3d9_targets.cpp'], dependencies : d3d9_test_deps),
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

# no test: the generator of the fixed-function transform's shader blobs (src/d3d9/d3d9_transform_shaders.hpp), run by
# hand under the tests' Wine when the transform's source changes
executable('fixed_function_shaders', ['fixed_function_shaders.cpp'],
           dependencies : d3d9_test_deps + [cpp.find_library('d3dcompiler_47')])
