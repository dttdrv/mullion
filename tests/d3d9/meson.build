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
