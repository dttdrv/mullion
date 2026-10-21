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
