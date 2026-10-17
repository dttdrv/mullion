# each test checks one contract of the D3D11 API; see the comment at the top of each file
d3d11_test_deps = [cpp.find_library('d3d11'), cpp.find_library('dxgi'), cpp.find_library('d3dcompiler_47')]

# the sources a test has beside its own
d3d11_test_sources = { 'tessellator': files('../reference/reference.cpp') }
# the libraries a test links beside Direct3D's: the Metal bridge, for a test of the bridge itself
d3d11_test_links = { 'bridge_errors': [winemetal_dep] }
foreach name : ['clear_rectangles', 'copy_formats', 'resource_creation', 'view_creation', 'device_arguments', 'psout', 'device_teardown', 'default_updates', 'default_allocations', 'default_growth', 'bridge_errors', 'planar', 'feature_queries', 'shared_resource', 'subresource_surface', 'interface_eligibility', 'device_interfaces', 'shader_levels', 'swapchain_buffers', 'initial_data', 'input_layout', 'streamout_declarations', 'copy_region', 'staging_depth', 'timestamp', 'fullscreen', 'profile', 'queries', 'deferred_release', 'state_objects', 'buffer_loads', 'shared_fence', 'staging_order', 'commandlist_state', 'map_contract', 'state_transitions', 'occlusion_many', 'streamout', 'streamout_scratch', 'geometry_oversize', 'geometry_parts', 'drawauto', 'assembly', 'primitiveid', 'tessellator', 'predication', 'staging', 'state', 'viewports', 'rectangles_many', 'surface', 'indices', 'device', 'constants', 'minlod', 'formats', 'packed', 'garbled', 'null_indices', 'null_stage_indices', 'dynamic_generations']
  # without the shader cache, so a test meets the shaders this build makes
  test('d3d11_' + name,
       executable('d3d11_' + name, ['d3d11_' + name + '.cpp'] + d3d11_test_sources.get(name, []),
                  dependencies : d3d11_test_deps + d3d11_test_links.get(name, [])),
       env : ['MULLION_SHADER_CACHE=0'], timeout : 600)
endforeach
# no test: the runner's way to see where the threads of a test that outlived its time are
executable('where', ['where.cpp'], dependencies : [cpp.find_library('dbghelp')])
# 'window' tests present to a real window
test('d3d11_xrbias', executable('d3d11_xrbias', ['d3d11_xrbias.cpp'],
     dependencies : d3d11_test_deps + [cpp.find_library('gdi32')]),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 600)
test('d3d11_swapchain_formats', executable('d3d11_swapchain_formats', ['d3d11_swapchain_formats.cpp'], dependencies : d3d11_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d11_swapchain', executable('d3d11_swapchain', ['d3d11_swapchain.cpp'], dependencies : d3d11_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
