# each test checks one contract of the D3D11 API; see the comment at the top of each file
d3d11_test_deps = [cpp.find_library('d3d11'), cpp.find_library('dxgi'), cpp.find_library('d3dcompiler_47')]

# the sources a test has beside its own
d3d11_test_sources = { 'tessellator': files('../reference/reference.cpp') }
foreach name : ['buffer_loads', 'swapchain_buffers', 'initial_data', 'input_layout', 'streamout_declarations', 'copy_region', 'staging_depth', 'profile', 'queries', 'state_objects', 'shared_fence', 'staging_order', 'commandlist_state', 'map_contract', 'state_transitions', 'occlusion_many', 'streamout', 'drawauto', 'assembly', 'primitiveid', 'tessellator', 'predication', 'staging', 'state', 'viewports', 'surface', 'indices', 'device', 'constants', 'minlod', 'formats', 'packed', 'garbled', 'null_indices', 'null_stage_indices', 'dynamic_generations']
  # without the shader cache, so a test meets the shaders this build makes
  test('d3d11_' + name,
       executable('d3d11_' + name, ['d3d11_' + name + '.cpp'] + d3d11_test_sources.get(name, []), dependencies : d3d11_test_deps),
       env : ['DXMT_SHADER_CACHE=0'], timeout : 600)
endforeach
# 'window' tests present to a real window
test('d3d11_swapchain', executable('d3d11_swapchain', ['d3d11_swapchain.cpp'], dependencies : d3d11_test_deps),
     env : ['DXMT_SHADER_CACHE=0'], suite : 'window', timeout : 300)
