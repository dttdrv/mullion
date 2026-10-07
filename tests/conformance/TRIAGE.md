# Conformance triage

What Wine's Direct3D tests and vkd3d-proton's Direct3D 12 tests find in Mullion, by root cause. `tests/conformance.py` made the verdicts,
one test function a process, as Windows (`WINETEST_PLATFORM=windows`, `VKD3D_TEST_PLATFORM=windows`). The JSON files beside this one hold
each test's verdict, its first failing check and where it crashed. A root cause is CONFIRMED where its place in the source was read, or is
where Mullion's own message in the test's log comes from, and PLAUSIBLE where the place named is where the behaviour is decided and the
cause in it was not traced. Line numbers are those of the source this run was made from.

## Numbers

| suite | ran | passed | partly | failed | crashed | timeout | first baseline: passed + partly of ran |
|---|---|---|---|---|---|---|---|
| wine-d3d11 | 162 | 90 | 4 | 57 | 11 | 3 (not rerun) | 88 + 4 of 165 |
| wine-d3d10core | 109 | 61 | 1 | 40 | 7 | 2 (not rerun) | 59 + 1 of 111 |
| wine-dxgi | 38 | 16 | 2 | 20 | 0 | 3 (not rerun) | 12 + 1 of 41 |
| wine-d3d12 | 10 | 6 | 0 | 3 | 1 | 0 (not rerun) | 3 + 0 of 10 |
| vkd3d-proton | 565 | 333 | 81 | 135 | 16 | 10 (not rerun) | 333 + 81 of 575 |

'partly' is a test in which nothing failed and something was skipped (a feature the device does not report): no pass.
The timeouts were not run again: they ended in GPU timeouts, 'Insufficient Memory' or never ended, and are listed at the end.

## Root causes, crashes first, then by the number of tests each fails

### 1. a tessellation pipeline with stream output is refused ('stream output of a tessellated draw is not implemented yet'); the tests go on without a pipeline

- class: missing; 10 tests, 6 of them crash
- CONFIRMED; where: src/d3d12/d3d12_pipeline_graphics.cpp:646
- Windows: the pipeline is created
- tests:
  - `vkd3d-proton` `test_line_tessellation_dxbc` (failed, 55 checks): `test_line_tessellation_dxbc:1115: Test failed: Failed to create state, hr 0x80004001.`
  - `vkd3d-proton` `test_line_tessellation_dxil` (failed, 55 checks): `test_line_tessellation_dxil:1115: Test failed: Failed to create state, hr 0x80004001.`
  - `vkd3d-proton` `test_primitive_id_read_tess_geom` (failed, 1 checks): `test_primitive_id_read_tess_geom:5102: Test failed: Failed to create pipeline.`
  - `vkd3d-proton` `test_quad_tessellation_dxbc` (crashed, 2 checks): `test_quad_tessellation_dxbc:347: Test failed: Failed to create state, hr 0x80004001.`; crash at `d3d12.exe: test_quad_tessellation at d3d12_tessellation.c:382:5`
  - `vkd3d-proton` `test_quad_tessellation_dxil` (crashed, 2 checks): `test_quad_tessellation_dxil:347: Test failed: Failed to create state, hr 0x80004001.`; crash at `d3d12.exe: test_quad_tessellation at d3d12_tessellation.c:382:5`
  - `vkd3d-proton` `test_quad_tessellation_wrong_input_count_dxbc` (crashed, 2 checks): `test_quad_tessellation_wrong_input_count_dxbc:347: Test failed: Failed to create state, hr 0x80004001.`; crash at `d3d12.exe: test_quad_tessellation at d3d12_tessellation.c:382:5`
  - `vkd3d-proton` `test_quad_tessellation_wrong_input_count_dxil` (crashed, 2 checks): `test_quad_tessellation_wrong_input_count_dxil:347: Test failed: Failed to create state, hr 0x80004001.`; crash at `d3d12.exe: test_quad_tessellation at d3d12_tessellation.c:382:5`
  - `vkd3d-proton` `test_quad_tessellation_wrong_pso_topology_dxbc` (crashed, 2 checks): `test_quad_tessellation_wrong_pso_topology_dxbc:347: Test failed: Failed to create state, hr 0x80004001.`; crash at `d3d12.exe: test_quad_tessellation at d3d12_tessellation.c:382:5`
  - `vkd3d-proton` `test_quad_tessellation_wrong_pso_topology_dxil` (crashed, 2 checks): `test_quad_tessellation_wrong_pso_topology_dxil:347: Test failed: Failed to create state, hr 0x80004001.`; crash at `d3d12.exe: test_quad_tessellation at d3d12_tessellation.c:382:5`
  - `vkd3d-proton` `test_tessellation_read_tesslevel` (failed, 29 checks): `test_tessellation_read_tesslevel:948: Test failed: Failed to create state, hr 0x80004001.`

### 2. view creation fails with E_FAIL for descriptions Windows accepts: render target views of buffers and of 3D texture slices ('Unhandled rtv creation', 'tex3d rtv creation not properly handled'), depth stencil view test 20, shader resource view test 16, unordered access view test 24

- class: bug; 6 tests, 5 of them crash
- CONFIRMED; where: src/d3d11/d3d11_resource_view_helper.cpp:506 and the functions beside it
- Windows: the views are created
- tests:
  - `wine-d3d10core` `test_create_depthstencil_view` (crashed, 2 checks): `d3d10core.c:2708: Test failed: Got unexpected refcount 1, expected >= 2.`; crash at `d3d10core_test.exe: check_interface_ at d3d10core.c:176:10`
  - `wine-d3d10core` `test_create_shader_resource_view` (crashed, 4 checks): `d3d10core.c:3667: Test failed: Got unexpected refcount 1, expected >= 2.`; crash at `d3d10core_test.exe: check_interface_ at d3d10core.c:176:10`
  - `wine-d3d11` `test_create_depthstencil_view` (crashed, 3 checks): `d3d11.c:4059: Test failed: Got unexpected refcount 1, expected >= 2.`; crash at `d3d11_test.exe: check_interface_ at d3d11.c:276:10`
  - `wine-d3d11` `test_create_shader_resource_view` (crashed, 4 checks): `d3d11.c:4730: Test failed: Got unexpected refcount 1, expected >= 2.`; crash at `d3d11_test.exe: check_interface_ at d3d11.c:276:10`
  - `wine-d3d11` `test_create_unordered_access_view` (crashed, 6 checks): `d3d11.c:20061: Test failed: Got unexpected hr 0.`; crash at `d3d11_test.exe: test_create_unordered_access_view at d3d11.c:20200:9`
  - `wine-d3d11` `test_render_target_views` (failed, 37 checks): `d3d11.c:11745: Test failed: Test 0: Got unexpected hr 0x80004005.`

### 3. not implemented: root signatures from a library's subobject and of version 1.2, heaps with ALLOW_WRITE_WATCH, ID3D12DeviceConfiguration1, ID3DDestructionNotifier, OpenExistingHeapFromAddress, shared handles

- class: missing; 6 tests, 2 of them crash
- CONFIRMED; where: src/d3d12/d3d12_root_signature.cpp:305 and 514, src/d3d12/d3d12_resource_helper.cpp:376, src/d3d12/d3d12_device.cpp (E_NOTIMPL at 881-891, 1344)
- Windows: they exist
- tests:
  - `vkd3d-proton` `test_destruction_notifier_callback` (crashed, 1 checks): `test_destruction_notifier_callback:1835: Test failed: Failed to query destruction notifier from resource, hr 0x80004002.`; crash at `d3d12.exe: test_destruction_notifier_callback at d3d12_device.c:1846:10`
  - `vkd3d-proton` `test_device_configuration` (failed, 3 checks): `test_device_configuration:2548:ID3D12Device: Test failed: Failed to query ID3D12DeviceConfiguration1.`
  - `vkd3d-proton` `test_open_heap_from_address` (crashed, 1 checks): `test_open_heap_from_address:106: Test failed: Failed to open heap from address: hr #80004001.`; crash at `d3d12.exe: test_open_heap_from_address at d3d12_win32_exclusive.c:112:25`
  - `vkd3d-proton` `test_raytracing_root_signature_from_subobject` (failed, 3 checks): `test_raytracing_root_signature_from_subobject:3404: Test failed: Failed to create root signature, hr #80004005.`
  - `vkd3d-proton` `test_root_signature_byte_code2` (failed, 1 checks): `test_root_signature_byte_code2:1625: Test failed: Got unexpected hr 0x80070057.`
  - `vkd3d-proton` `test_write_watch` (failed, 1 checks): `test_write_watch:285: Test failed: Failed to ResetWriteWatch 0x57.`

### 4. a staging texture of a depth format (D24_UNORM_S8_UINT, and the depth format of the clear test) is not created: E_FAIL, with nothing logged; the tests then read a readback they were not given

- class: bug; 4 tests, 4 of them crash
- PLAUSIBLE; where: CreateStagingTexture2D in src/d3d11/d3d11_resource_staging.cpp, reached from src/d3d11/d3d11_device.cpp:732
- Windows: the texture is created: it is how depth is read back
- tests:
  - `wine-d3d10core` `test_clear_depth_stencil_view` (crashed); crash at `d3d10core_test.exe: release_resource_readback at d3d10core.c:0:0`
  - `wine-d3d10core` `test_depth_bias` (crashed); crash at `d3d10core_test.exe: test_depth_bias at d3d10core.c:16145:25`
  - `wine-d3d11` `test_clear_depth_stencil_view` (crashed); crash at `d3d11_test.exe: test_clear_depth_stencil_view at d3d11.c:16582:15`
  - `wine-d3d11` `test_depth_bias` (crashed); crash at `d3d11_test.exe: test_depth_bias at d3d11.c:28698:25`

### 5. ray tracing pipelines: Metal refuses one ('Exceeds maximum interface register allocation'), identifiers differ between a collection and its pipeline, structures through a descriptor table are not found, and state objects keep public references on root signatures

- class: bug; 4 tests, 1 of them crash
- PLAUSIBLE; where: src/d3d12/d3d12_state_object.cpp; not traced further
- Windows: the pipelines are created and trace
- tests:
  - `vkd3d-proton` `test_rayquery_root_table` (failed, 288 checks): `test_rayquery_root_table:2313:Test: Plain: Test failed: Ray color [0].x mismatch (1000.000000 != 1.000000).`
  - `vkd3d-proton` `test_raytracing` (failed, 823 checks): `test_raytracing:1577:Test: Plain: Test failed: Ref count 6 != 1.`
  - `vkd3d-proton` `test_raytracing_collection_handle_invariance` (failed, 2 checks): `test_raytracing_collection_handle_invariance:5198: Test failed: Pointer handles should match.`
  - `vkd3d-proton` `test_raytracing_mismatch_global_rs_link` (crashed, 1 checks): `test_raytracing_mismatch_global_rs_link:3919: Test failed: Failed to query miss handle from COLLECTION.`; crash at `d3d12.exe: test_raytracing_mismatch_global_rs_link at d3d12_raytracing.c:3947:12`

### 6. a Direct3D 10 or 11 flip swap chain has only its buffer 0 ('Non zero-index buffer is not supported')

- class: missing; 3 tests, 2 of them crash
- CONFIRMED; where: src/d3d11/d3d11_swapchain.cpp:264
- Windows: GetBuffer gives every buffer of a flip swap chain, read only past the first
- tests:
  - `wine-d3d10core` `test_swapchain_flip` (crashed, 3 checks): `d3d10core.c:11163: Test failed: Failed to get buffer, hr 0x887a0004.`; crash at `d3d10core_test.exe: test_swapchain_flip at d3d10core.c:11180:5`
  - `wine-d3d11` `test_swapchain_flip` (crashed, 3 checks): `d3d11.c:16117: Test failed: Got unexpected hr 0x887a0004.`; crash at `d3d11_test.exe: test_swapchain_flip at d3d11.c:16134:5`
  - `wine-dxgi` `test_swapchain_parameters` (failed, 280 checks): `dxgi.c:4887: Test failed: Got usage 20, expected 60, test 0.`

### 7. a buffer made with initial data whose pointer cannot be read ends the process: unix call 107 (MTLBuffer_updateContents, a memcpy) faults, and the thunk asserts

- class: crash; 2 tests, 2 of them crash
- CONFIRMED; where: src/winemetal/winemetal_thunks.c:949, src/winemetal/unix/winemetal_unix.c:2851, reached from src/d3d11/d3d11_buffer.cpp:108. no Metal error exists: the call faulted
- Windows: CreateBuffer fails with E_INVALIDARG
- tests:
  - `wine-d3d10core` `test_create_rendertarget_view` (crashed, 4 checks): `d3d10core.c:3015: Test failed: Got unexpected hr 0.`; crash at `d3d10core_test.exe: test_create_rendertarget_view at d3d10core.c:3050:5`
  - `wine-d3d11` `test_create_rendertarget_view` (crashed, 4 checks): `d3d11.c:4368: Test failed: Got unexpected hr 0.`; crash at `d3d11_test.exe: test_create_rendertarget_view at d3d11.c:4403:5`

### 8. pipeline statistics queries: CreateQueryHeap returns E_NOTIMPL, BeginQuery logs 'query type 3 is not implemented', and ResolveQueryData then ends the process in unix call 36 (MTLBlitCommandEncoder_encodeCommands)

- class: missing; 2 tests, 2 of them crash
- CONFIRMED; where: src/d3d12/d3d12_query_heap.cpp:53, src/d3d12/d3d12_command_list.cpp:2164, src/winemetal/winemetal_thunks.c:384
- Windows: the heap type needs no capability and always exists
- tests:
  - `vkd3d-proton` `test_create_query_heap` (crashed, 1 checks): `test_create_query_heap:54: Test failed: Failed to create query heap, type 2, hr 0x80004001.`; crash at `?+0x0`
  - `vkd3d-proton` `test_query_pipeline_statistics` (crashed, 7 checks): `test_query_pipeline_statistics:233: Test failed: Failed to create query heap, type 2, hr 0x80004001.`; crash at `d3d12.exe: test_query_pipeline_statistics at d3d12_query.c:303:5`

### 9. CreatePipelineLibrary returns DXGI_ERROR_UNSUPPORTED

- class: missing; 2 tests, 1 of them crash
- CONFIRMED; where: src/d3d12/d3d12_device.cpp:1062
- Windows: a library is made
- tests:
  - `vkd3d-proton` `test_destruction_notifier_interfaces` (failed, 13 checks): `test_destruction_notifier_interfaces:2013: Test failed: Failed to create pipeline library, hr 0x887a0004.`
  - `vkd3d-proton` `test_pipeline_library` (crashed, 2 checks): `test_pipeline_library:197: Test failed: Failed to create pipeline library, hr 0x887a0004.`; crash at `d3d12.exe: test_pipeline_library at d3d12_pso_blob.c:212:10`

### 10. ExtractStreamOutputElements faults on a stream output declaration Windows refuses

- class: crash; 1 tests, 1 of them crash
- CONFIRMED; where: src/dxmt/dxmt_stream_output.hpp:38 (the entry's stream has no signature to read)
- Windows: CreateGeometryShaderWithStreamOutput returns E_INVALIDARG
- tests:
  - `wine-d3d11` `test_stream_output` (crashed, 104 checks): `d3d11.c:27085: Test failed: Got unexpected hr 0.`; crash at `d3d11.dll: long dxmt::ExtractStreamOutputElements<D3D11_SO_DECLARATION_ENTRY>(void const*, std::__1::span<D3D11_SO_DECLARATION_ENTRY const, 18446744073709551615ull>, std::__1::vector<SM50_STREAM_OUTPUT_ELEMENT2, std::__1::allocator<SM50_STREAM_OUTPUT_ELEMENT2>>&) at ??:0:0`

### 11. an illegal instruction in SetVertexBuffers after ClearState

- class: crash; 1 tests, 1 of them crash
- PLAUSIBLE; where: src/d3d11/d3d11_context_impl.cpp, SetVertexBuffers (3899), right after CommandChunk::allocate_cpu_heap; the cause is not traced
- Windows: the calls succeed
- tests:
  - `wine-d3d11` `test_clear_state` (crashed, 1 checks): `d3d11.c:12562: Test failed: Got unexpected blend factor {0.00000000e+000, 0.00000000e+000, 0.00000000e+000, 0.00000000e+000}.`; crash at `d3d11.dll: dxmt::MTLD3D11DeviceContextImplBase<dxmt::ContextInternalState>::SetVertexBuffers(unsigned int, unsigned int, ID3D11Buffer* const*, unsigned int const*, unsigned int const*) at ??:0:0`

### 12. Map of a staging resource's subresource other than 0 fails assert(Subresource == 0)

- class: crash; 1 tests, 1 of them crash
- CONFIRMED; where: src/d3d11/d3d11_resource_staging.cpp:25
- Windows: Map returns E_INVALIDARG for a subresource the resource does not have, and maps one it has
- tests:
  - `wine-d3d11` `test_resource_map` (crashed, 4 checks): `d3d11.c:15344: Test failed: Got unexpected hr 0.`; crash at `kernelbase.dll+0x14dd3`

### 13. CreateInputLayout refuses (E_INVALIDARG) a layout with an element the shader takes as a system value ('ps: SV_InstanceID excluded'); the test then releases what it was not given

- class: bug; 1 tests, 1 of them crash
- PLAUSIBLE; where: CreateInputLayout, src/d3d11/d3d11_device.cpp:215
- Windows: S_OK
- tests:
  - `wine-d3d11` `test_create_input_layout` (crashed, 19 checks): `d3d11.c:19240: Test failed: Format 0x10: Got refcount 1, expected 2.`; crash at `d3d11_test.exe: check_layout_element_exclusion at d3d11.c:19061:13`

### 14. D3D12CreateDevice refuses an IDXCoreAdapter (E_INVALIDARG); the test then uses the device it was not given

- class: bug; 1 tests, 1 of them crash
- CONFIRMED; where: src/d3d12/d3d12.cpp:55-61
- Windows: the device is created from the DXCore adapter
- tests:
  - `wine-d3d12` `test_create_device` (crashed, 1 checks): `d3d12.c:971: Test failed: Got unexpected hr 0x80070057.`; crash at `d3d12_test.exe: func_d3d12 at d3d12.c:1577:38`

### 15. which interfaces Direct3D 10 and 11 objects answer for, and what GetDevice and private data return, differ from Windows (a Direct3D 11 texture answers for ID3D10Texture2D, a device pointer where the tests expect none)

- class: semantics; 17 tests
- PLAUSIBLE; where: the QueryInterface of the resources in src/d3d11/d3d11_resource.hpp and src/d3d11/d3d11_texture.cpp
- Windows: the interfaces the tests record for each creation path
- tests:
  - `wine-d3d10core` `test_create_buffer` (failed, 31 checks): `d3d10core.c:2564: Test failed: Got unexpected hr 0.`
  - `wine-d3d10core` `test_create_texture1d` (failed, 4 checks): `d3d10core.c:1795: Test failed: Got unexpected hr 0.`
  - `wine-d3d10core` `test_create_texture2d` (failed, 21 checks): `d3d10core.c:2105: Test failed: Got unexpected hr 0.`
  - `wine-d3d10core` `test_create_texture3d` (failed, 8 checks): `d3d10core.c:2380: Test failed: Got unexpected hr 0.`
  - `wine-d3d10core` `test_device_interfaces` (failed, 1 checks): `d3d10core.c:1725: Test failed: Adapter parent should not implement IDXGIFactory1.`
  - `wine-d3d10core` `test_texture1d_interfaces` (failed, 2 checks): `d3d10core.c:1945: Test failed: Got hr 0x80004002, expected 0.`
  - `wine-d3d11` `test_create_buffer` (failed, 86 checks): `d3d11.c:3913: Test failed: Test 0: Got unexpected device pointer 0000000000715270, expected NULL.`
  - `wine-d3d11` `test_create_texture1d` (failed, 4 checks): `d3d11.c:2598: Test failed: Got unexpected hr 0.`
  - `wine-d3d11` `test_create_texture2d` (failed, 22 checks): `d3d11.c:3103: Test failed: Got unexpected hr 0.`
  - `wine-d3d11` `test_create_texture3d` (failed, 8 checks): `d3d11.c:3424: Test failed: Got unexpected hr 0.`
  - `wine-d3d11` `test_device_interfaces` (failed, 21 checks): `d3d11.c:2375: Test failed: Feature level 0xb100: Got hr 0, expected 0x80004002.`
  - `wine-d3d11` `test_immediate_context` (failed, 1 checks): `d3d11.c:2450: Test failed: Got hr 0x80004002, expected 0.`
  - `wine-d3d11` `test_texture1d_interfaces` (failed, 8 checks): `d3d11.c:2925: Test failed: Got hr 0x80004002, expected 0.`
  - `wine-d3d11` `test_texture2d_interfaces` (failed, 5 checks): `d3d11.c:3364: Test failed: Test 0: Got unexpected device pointer 0000000000715270, expected NULL.`
  - `wine-d3d11` `test_texture3d_interfaces` (failed, 4 checks): `d3d11.c:3622: Test failed: Test 0: Got unexpected device pointer 0000000000715270, expected NULL.`
  - `wine-dxgi` `test_create_factory` (failed, 3 checks): `dxgi.c:4091: Test failed: Got unexpected hr 0, expected 0x80004002.`
  - `wine-dxgi` `test_subresource_surface` (failed, 53 checks): `dxgi.c:8596: Test failed: 0: 0: Got unexpected hr 0x80070057.`

### 16. fullscreen and display modes: SetFullscreenState and ResizeTarget do not give the window and the output the size and style asked for, and window messages, styles and the associated window differ

- class: bug; 15 tests
- PLAUSIBLE; where: SetFullscreenState (src/d3d11/d3d11_swapchain.cpp:272) and ResizeTarget (557), and their Direct3D 12 twins in src/d3d12/d3d12_swapchain.cpp; MakeWindowAssociation in src/dxgi/dxgi_factory.cpp ('Ignoring flags'); the causes are not traced
- Windows: the window covers the output in the mode asked for, with the styles and messages the tests record
- tests:
  - `wine-dxgi` `test_colour_space_support` (failed, 16 checks): `dxgi.c:7741: Test failed: d3d12: Got unexpected hr 0 for text 2.`
  - `wine-dxgi` `test_default_fullscreen_target_output` (failed, 2 checks): `dxgi.c:3187: Test failed: d3d12: Adapter 0 output 0: Expected a valid output.`
  - `wine-dxgi` `test_frame_latency_event` (failed, 12 checks): `dxgi.c:7461: Test failed: d3d10: Got unexpected hr 0.`
  - `wine-dxgi` `test_gamma_control` (failed, 2 checks): `dxgi.c:6769: Test failed: Got unexpected hr 0.`
  - `wine-dxgi` `test_inexact_modes` (failed, 36 checks): `dxgi.c:3988: Test failed: Got window rect (0,0)-(1512,982), expected (0,0)-(960,600).`
  - `wine-dxgi` `test_mode_change` (failed, 2 checks): `dxgi.c:7881: Test failed: d3d10: Got a different mode.`
  - `wine-dxgi` `test_resize_fullscreen` (failed, 18 checks): `dxgi.c:3384: Test failed: d3d10: No style change: Got style 0x4c00000, expected 0x4000000.`
  - `wine-dxgi` `test_resize_target` (failed, 344 checks): `dxgi.c:3672: Test failed: d3d10: Adapter 0: output 0: test 2: Got window rect (0,0)-(1512,982), expected (0,0)-(800,600).`
  - `wine-dxgi` `test_set_fullscreen` (failed, 68 checks): `dxgi.c:2845: Test failed: d3d10: Got unexpected hr 0.`
  - `wine-dxgi` `test_swapchain_backbuffer_index` (failed, 2 checks): `dxgi.c:5339: Test failed: d3d12: Got unexpected hr 0, expected 0x887a0001.`
  - `wine-dxgi` `test_swapchain_present_count` (failed, 4 checks): `dxgi.c:8163: Test failed: d3d12: test 1: Got unexpected hr 0.`
  - `wine-dxgi` `test_swapchain_resize` (failed, 23 checks): `dxgi.c:4499: Test failed: d3d10: Got unexpected hr 0.`
  - `wine-dxgi` `test_swapchain_window_messages` (failed, 32 checks): `dxgi.c:6325: Test failed: d3d10: Got unexpected hr 0x887a0001.`
  - `wine-dxgi` `test_swapchain_window_styles` (failed, 14 checks): `dxgi.c:6659: Test failed: Test 0: Got unexpected style 0x14000000, expected 0x4000000.`
  - `wine-dxgi` `test_window_association` (failed, 24 checks): `dxgi.c:6934: Test failed: d3d10: Expect null associated window.`

### 17. CreateGraphicsPipelineState, CreateComputePipelineState and CreatePipelineState accept what Windows refuses, or fail with E_FAIL where it says E_INVALIDARG: stages that do not match, a geometry shader's topology, dual source blending with several targets, a mesh with a vertex shader, bindings outside the root signature, a cached blob of another shader

- class: lenient; 12 tests
- PLAUSIBLE; where: src/d3d12/d3d12_pipeline_graphics.cpp Initialize, src/d3d12/d3d12_pipeline_compute.cpp; nothing there checks these
- Windows: E_INVALIDARG
- tests:
  - `vkd3d-proton` `test_descriptor_range_validation` (failed, 1 checks): `test_descriptor_range_validation:5018: Test failed: Unexpected success.`
  - `vkd3d-proton` `test_dual_source_blending_dxbc` (failed, 4 checks): `test_dual_source_blending_dxbc:1185: Test failed: Unexpected result, hr 0.`
  - `vkd3d-proton` `test_dual_source_blending_dxil` (failed, 4 checks): `test_dual_source_blending_dxil:1185: Test failed: Unexpected result, hr 0.`
  - `vkd3d-proton` `test_get_cached_blob` (failed, 3 checks): `test_get_cached_blob:103: Test failed: Unexpected hr 0.`
  - `vkd3d-proton` `test_gs_topology_mismatch_dxbc` (failed, 22 checks): `test_gs_topology_mismatch_dxbc:4896:Test 0,1: Test failed: Got hr 0, expected 0x80070057.`
  - `vkd3d-proton` `test_gs_topology_mismatch_dxil` (failed, 22 checks): `test_gs_topology_mismatch_dxil:4896:Test 0,1: Test failed: Got hr 0, expected 0x80070057.`
  - `vkd3d-proton` `test_mesh_shader_create_pipeline` (failed, 2 checks): `test_mesh_shader_create_pipeline:152: Test failed: Unexpected result for pipeline creation, hr 0.`
  - `vkd3d-proton` `test_mismatching_pso_stages` (failed, 1 checks): `test_mismatching_pso_stages:1601: Test failed: Unexpected hr #80004005.`
  - `vkd3d-proton` `test_missing_bindings_root_signature` (failed, 1 checks): `test_missing_bindings_root_signature:1809: Test failed: Unexpected hr #0.`
  - `vkd3d-proton` `test_shader_io_mismatch` (failed, 60 checks): `test_shader_io_mismatch:4704:Test 2: Test failed: Got hr 0, expected 0x80070057.`
  - `vkd3d-proton` `test_shader_sm66_wave_size` (failed, 2 checks): `test_shader_sm66_wave_size:572:Test 0: Test failed: Got hr #80004005, expected 0x80070057.`
  - `vkd3d-proton` `test_static_sampler_dynamic_index` (failed, 4 checks): `test_static_sampler_dynamic_index:6725: Test failed: Unexpected success.`

### 18. Direct3D 10 and 11 queries: GetData succeeds where the tests expect S_FALSE or an error, writes data it should leave, and counts differ; queries of deferred contexts fail (E_FAIL)

- class: bug; 11 tests
- PLAUSIBLE; where: src/d3d11/d3d11_query.cpp and GetData in src/d3d11/d3d11_context_impl.cpp; the causes are not traced
- Windows: the results and return codes the tests record
- tests:
  - `wine-d3d10core` `test_create_query` (failed, 1 checks): `d3d10core.c:4692: Test failed: Got unexpected refcount 1, expected >= 2.`
  - `wine-d3d10core` `test_occlusion_query` (failed, 4 checks): `d3d10core.c:4762: Test failed: Got unexpected hr 0.`
  - `wine-d3d10core` `test_pipeline_statistics_query` (failed, 15 checks): `d3d10core.c:4898: Test failed: Got unexpected hr 0.`
  - `wine-d3d10core` `test_so_statistics_query` (failed, 4 checks): `d3d10core.c:5114: Test failed: Got unexpected hr 0x1.`
  - `wine-d3d10core` `test_timestamp_query` (failed, 3 checks): `d3d10core.c:5035: Test failed: Got unexpected hr 0.`
  - `wine-d3d11` `test_create_query` (failed, 21 checks): `d3d11.c:6090: Test failed: Got hr 0, expected 0x80004002.`
  - `wine-d3d11` `test_deferred_context_queries` (failed, 9 checks): `d3d11.c:33800: Test failed: Got unexpected hr 0x80004005.`
  - `wine-d3d11` `test_occlusion_query` (failed, 5 checks): `d3d11.c:6182: Test failed: Got unexpected hr 0.`
  - `wine-d3d11` `test_pipeline_statistics_query` (failed, 16 checks): `d3d11.c:6361: Test failed: Got unexpected hr 0.`
  - `wine-d3d11` `test_so_statistics_query` (failed, 21 checks): `d3d11.c:6708: Test failed: Got unexpected hr 0x1.`
  - `wine-d3d11` `test_timestamp_query` (failed, 5 checks): `d3d11.c:6516: Test failed: Got unexpected hr 0.`

### 19. Direct3D 10 and 11 state objects: descriptions are not normalized as Windows normalizes them (a sampler's anisotropy and comparison function, a depth stencil state's masks), equal states are not one object, and states keep or lose references the tests count

- class: semantics; 10 tests
- PLAUSIBLE; where: CreateDepthStencilState (src/d3d11/d3d11_device.cpp:382), CreateSamplerState (414), src/d3d11/d3d11_state_object.cpp
- Windows: GetDesc returns the normalized description; equal descriptions give one object
- tests:
  - `wine-d3d10core` `test_create_blend_state` (failed, 7 checks): `d3d10core.c:4303: Test failed: Got unexpected src blend 0.`
  - `wine-d3d10core` `test_create_depthstencil_state` (failed, 10 checks): `d3d10core.c:4538: Test failed: Got unexpected stencil read mask 0.`
  - `wine-d3d10core` `test_create_sampler_state` (failed, 27 checks): `d3d10core.c:4143: Test failed: Got unexpected comparison func 8.`
  - `wine-d3d10core` `test_state_refcounting` (failed, 5 checks): `d3d10core.c:8986: Test failed: Got unexpected pointer 000000000073C8C0, expected NULL.`
  - `wine-d3d11` `test_create_blend_state` (failed, 49 checks): `d3d11.c:5692: Test failed: Got unexpected src blend 0 for render target 1.`
  - `wine-d3d11` `test_create_depthstencil_state` (failed, 12 checks): `d3d11.c:5852: Test failed: Got unexpected depth write mask 0.`
  - `wine-d3d11` `test_create_sampler_state` (failed, 28 checks): `d3d11.c:5453: Test failed: Got unexpected max anisotropy 16.`
  - `wine-d3d11` `test_deferred_context_swap_state` (failed, 1 checks): `d3d11.c:33469: Test failed: Got state 0000000000773710.`
  - `wine-d3d11` `test_device_context_state` (failed, 65 checks): `d3d11.c:7425: Test failed: Got hr 0, expected 0x80004002.`
  - `wine-d3d11` `test_state_refcounting` (failed, 8 checks): `d3d11.c:7180: Test failed: Feature level 0xb100: Got refcount 1, expected 0.`

### 20. CheckFeatureSupport fails or answers nothing where Windows answers: FORMAT_SUPPORT returns E_INVALIDARG for formats it does not have, MULTISAMPLE_QUALITY_LEVELS and FORMAT_INFO return E_FAIL, and sizes and features of newer SDKs get other answers

- class: bug; 9 tests
- CONFIRMED; where: src/d3d12/d3d12_device.cpp:309-333 (quality levels), 370-384 (format info), 514-522 (format support, MTLQueryDXGIFormatSupport in src/dxmt/dxmt_format.cpp)
- Windows: S_OK with no support bits, 1 quality level for one sample, plane counts for every format, E_INVALIDARG only for sizes it does not know
- tests:
  - `vkd3d-proton` `test_check_feature_support` (failed, 37 checks): `test_check_feature_support:266:format 0x42: Test failed: Got unexpected hr 0x80004005.`
  - `vkd3d-proton` `test_format_support` (failed, 82 checks): `test_format_support:440: Test failed: Got unexpected support1 0x20001.`
  - `vkd3d-proton` `test_misc_agility_sdk_feature_checks` (failed, 453 checks): `test_misc_agility_sdk_feature_checks:2742: Test failed: Expected D3D12_FEATURE_PREDICATION to return E_NOTIMPL.`
  - `vkd3d-proton` `test_multisample_quality_levels` (failed, 37 checks): `test_multisample_quality_levels:589:format 0: Test failed: Got unexpected quality levels 0.`
  - `vkd3d-proton` `test_planar_video_formats` (failed, 3 checks): `test_planar_video_formats:4163:Format 0x67: Test failed: Got invalid hr 0x80070057.`
  - `wine-d3d10core` `test_check_multisample_quality_levels` (failed, 5 checks): `d3d10core.c:10636: Test failed: Got unexpected hr 0x80070057.`
  - `wine-d3d10core` `test_format_support` (failed, 117 checks): `d3d10core.c:13824: Test failed: Got unexpected hr 0x80070057.`
  - `wine-d3d11` `test_check_multisample_quality_levels` (failed, 5 checks): `d3d11.c:15812: Test failed: Got unexpected hr 0x80070057.`
  - `wine-d3d11` `test_format_support` (failed, 1010 checks): `d3d11.c:21425: Test failed: Feature level 0x9300: Got unexpected hr 0x80070057.`

### 21. other calls accept what Windows refuses: a buffer with ALLOW_DEPTH_STENCIL, a misaligned multisampled placed resource, GetHeapProperties of a reserved resource, SetEventOnMultipleFenceCompletion without fences, an invalid resolve or WriteBufferImmediate mode at Close, and the device is not removed after an invalid list

- class: lenient; 8 tests
- PLAUSIBLE; where: src/d3d12/d3d12_device.cpp (CreateCommittedResource, CreatePlacedResource, SetEventOnMultipleFenceCompletion at 1068, GetDeviceRemovedReason at 911), src/d3d12/d3d12_command_list.cpp Close
- Windows: E_INVALIDARG, or DXGI_ERROR_INVALID_CALL from GetDeviceRemovedReason
- tests:
  - `vkd3d-proton` `test_buffer_rtv_dsv_usage` (failed, 1 checks): `test_buffer_rtv_dsv_usage:6302: Test failed: Unexpected hr #0.`
  - `vkd3d-proton` `test_create_reserved_resource` (failed, 11 checks): `test_create_reserved_resource:1013: Test failed: Got unexpected hr 0.`
  - `vkd3d-proton` `test_device_removed_reason` (failed, 3 checks): `test_device_removed_reason:1663: Test failed: Got unexpected hr 0.`
  - `vkd3d-proton` `test_fence_wait_multiple` (failed, 4 checks): `test_fence_wait_multiple:1498: Test failed: Got unexpected result, hr 0.`
  - `vkd3d-proton` `test_fence_wait_multiple_shared` (failed, 4 checks): `test_fence_wait_multiple_shared:1498: Test failed: Got unexpected result, hr 0.`
  - `vkd3d-proton` `test_multisample_resolve_strongly_typed` (failed, 3 checks): `test_multisample_resolve_strongly_typed:2785:Test 0: Test failed: Got hr 0, expected E_INVALIDARG.`
  - `vkd3d-proton` `test_placed_msaa_alignment_workaround` (failed, 1 checks): `test_placed_msaa_alignment_workaround:5953: Test failed: Unexpected hr #0.`
  - `vkd3d-proton` `test_write_buffer_immediate` (failed, 1 checks): `test_write_buffer_immediate:4773: Test failed: Got unexpected hr 0.`

### 22. reserved (tiled) buffers: a mapped tile reads 0, and remapped tiles read other tiles' data

- class: bug; 6 tests
- PLAUSIBLE; where: src/d3d12/d3d12_command_queue.cpp UpdateTileMappings and CopyTileMappings, src/d3d12/d3d12_buffer.cpp; not traced further
- Windows: a tile reads the heap memory mapped to it
- tests:
  - `vkd3d-proton` `test_buffer_feedback_instructions_dxil` (failed, 256 checks): `test_buffer_feedback_instructions_dxil:1799:Test 0: Test failed: Got 0x00000000, expected 0x00000001 at (0, 0, 0).`
  - `vkd3d-proton` `test_buffer_feedback_instructions_sm51` (failed, 256 checks): `test_buffer_feedback_instructions_sm51:1799:Test 0: Test failed: Got 0x00000000, expected 0x00000001 at (0, 0, 0).`
  - `vkd3d-proton` `test_sparse_buffer_memory_lifetime` (failed, 4 checks): `test_sparse_buffer_memory_lifetime:2305: Test failed: Got #0, expected 42.`
  - `vkd3d-proton` `test_update_tile_mappings` (failed, 127 checks): `test_update_tile_mappings:732: Test failed: Got 0x00000000, expected 0x00000001 at (0, 0, 0).`
  - `vkd3d-proton` `test_update_tile_mappings_remap_smem` (failed, 966 checks): `test_update_tile_mappings_remap_smem:500: Test failed: Iter 1: value 2: Expected 0, got 61653.`
  - `vkd3d-proton` `test_update_tile_mappings_remap_vmem` (failed, 959 checks): `test_update_tile_mappings_remap_vmem:500: Test failed: Iter 1: value 2: Expected 0, got 61653.`

### 23. CopySubresourceRegion of 1D and 3D textures, and of some 2D regions, leaves or takes the wrong texels

- class: bug; 5 tests
- PLAUSIBLE; where: src/d3d11/d3d11_context_impl.cpp CopySubresourceRegion; not traced further
- Windows: the box is copied
- tests:
  - `wine-d3d10core` `test_copy_subresource_region` (failed, 3 checks): `d3d10core.c:10156: Test failed: Got 0xff0000ff, expected 0x00000000 at (0, 0), sub-resource 0.`
  - `wine-d3d10core` `test_copy_subresource_region_1d` (failed, 16 checks): `d3d10core.c:10266: Test failed: Got color 0x00000000 at (0, 0), expected 0xff0000ff.`
  - `wine-d3d11` `test_copy_subresource_region` (failed, 3 checks): `d3d11.c:14971: Test failed: Got 0xff0000ff, expected 0x00000000 at (0, 0, 0), sub-resource 0.`
  - `wine-d3d11` `test_copy_subresource_region_1d` (failed, 16 checks): `d3d11.c:15084: Test failed: Got colour 0x00000000 at (0, 0), expected 0xff0000ff.`
  - `wine-d3d11` `test_copy_subresource_region_3d` (failed, 38 checks): `d3d11.c:15243: Test failed: Got 0x00000000, expected 0xff0000ff at (0, 0, 0), sub-resource 0.`

### 24. CreateStateObject accepts ray tracing pipelines and libraries Windows refuses (duplicate or missing subobjects, a missing root signature)

- class: lenient; 5 tests
- PLAUSIBLE; where: src/d3d12/d3d12_state_object.cpp
- Windows: E_INVALIDARG
- tests:
  - `vkd3d-proton` `test_raytracing_acceleration_structure_validation` (failed, 2 checks): `test_raytracing_acceleration_structure_validation:4804: Test failed: Got hr 0, expected E_INVALIDARG.`
  - `vkd3d-proton` `test_raytracing_embedded_subobjects` (failed, 1 checks): `test_raytracing_embedded_subobjects:3120: Test failed: Unexpected compilation success.`
  - `vkd3d-proton` `test_raytracing_missing_required_objects` (failed, 4 checks): `test_raytracing_missing_required_objects:2881: Test failed: Successfully created RTPSO object which is not expected.`
  - `vkd3d-proton` `test_raytracing_object_assignment_ignore_default` (failed, 1 checks): `test_raytracing_object_assignment_ignore_default:3327: Test failed: Unexpected success in compiling PSO.`
  - `vkd3d-proton` `test_raytracing_reject_duplicate_objects` (failed, 2 checks): `test_raytracing_reject_duplicate_objects:2974: Test failed: Successfully created RTPSO object which is not expected.`

### 25. an event of SetEventOnCompletion is not set by the time Signal has returned or the queue is idle

- class: bug; 4 tests
- PLAUSIBLE; where: src/d3d12/d3d12_fence.cpp and the queue's Signal in src/d3d12/d3d12_command_queue.cpp; the two availability tests passed before the queue's rework
- Windows: the event is set at once for a value already reached, and by the signal otherwise
- tests:
  - `vkd3d-proton` `test_cpu_signal_fence` (failed, 24 checks): `test_cpu_signal_fence:587: Test failed: Got unexpected return value 0x102.`
  - `vkd3d-proton` `test_fence_signal_availability_plain` (failed, 2 checks): `test_fence_signal_availability_plain:2209: Test failed: Expected 0, got 258.`
  - `vkd3d-proton` `test_fence_signal_availability_shared` (failed, 2 checks): `test_fence_signal_availability_shared:2209: Test failed: Expected 0, got 258.`
  - `vkd3d-proton` `test_gpu_signal_fence` (failed, 1 checks): `test_gpu_signal_fence:971: Test failed: Got unexpected return value 0x102.`

### 26. a copy between formats of one size but not one type (a block-compressed texture and the integer format of its blocks, or two formats of one bit layout) leaves the destination as it was

- class: bug; 3 tests
- PLAUSIBLE; where: src/d3d12/d3d12_command_list.cpp CopyTextureRegion (1244), src/d3d11/d3d11_context_impl.cpp CopySubresourceRegion; not traced further
- Windows: the bits are copied (Direct3D 10.1 format compatibility)
- tests:
  - `vkd3d-proton` `test_copy_buffer_texture_bc_rgba` (failed, 3 checks): `test_copy_buffer_texture_bc_rgba:1307: Test failed: Got {0, 0, 0, 0} at (16, 0), expected {0x10000010, 0x20001000, 0x30000010, 0x40100000}.`
  - `wine-d3d11` `test_compressed_format_compatibility` (failed, 5970 checks): `d3d11.c:29746: Test failed: Feature level 0xb000: 0x1 -> 0x46: Got unexpected colour 0xff0000ff at 10, expected 0x00000000.`
  - `wine-d3d11` `test_format_compatibility` (failed, 194 checks): `d3d11.c:29492: Test failed: Test 6: Got unexpected colour 0xff0000ff at (1, 1), expected 0x00000000.`

### 27. a buffer read past its end, or at an address that wraps, returns data

- class: bug; 3 tests
- PLAUSIBLE; where: the bounds a buffer view gets in src/d3d12/d3d12_descriptor_heap.cpp and the loads in src/airconv/dxil_converter.cpp; not traced further
- Windows: 0 (D3D11.3 functional specification: out of bounds reads return 0)
- tests:
  - `vkd3d-proton` `test_buffers_oob_behavior_vectorized_byte_address` (failed, 64 checks): `test_buffers_oob_behavior_vectorized_byte_address:400: Test failed: 32-bit value 0, 8: #83 != #0.`
  - `vkd3d-proton` `test_byte_buffer_addressing_wrap` (failed, 69 checks): `test_byte_buffer_addressing_wrap:2024: Test failed: reads: buffer_index 6, value 0, expected 0, got 3`
  - `vkd3d-proton` `test_structured_buffer_addressing_wrap` (failed, 105 checks): `test_structured_buffer_addressing_wrap:2299: Test failed: reads: buffer_index 5, value 0, expected 0, got 3`

### 28. 'ExecuteIndirect: stream output is not implemented yet'

- class: missing; 3 tests
- CONFIRMED; where: src/d3d12/d3d12_command_list.cpp:2374
- Windows: the draws are streamed out
- tests:
  - `vkd3d-proton` `test_execute_indirect_state` (failed, 139 checks): `test_execute_indirect_state:2932:Test 0: Test failed: Expected size 288, got 32.`
  - `vkd3d-proton` `test_vertex_id_dxbc` (failed, 21 checks): `test_vertex_id_dxbc:10278: Test failed: Got counter value 288, expected 608u.`
  - `vkd3d-proton` `test_vertex_id_dxil` (failed, 21 checks): `test_vertex_id_dxil:10278: Test failed: Got counter value 288, expected 608u.`

### 29. a swap chain is made on the desktop window and on a compute or copy queue

- class: lenient; 2 tests
- PLAUSIBLE; where: CreateSwapChain, src/d3d12/d3d12_swapchain.cpp:889
- Windows: E_ACCESSDENIED for the desktop window, DXGI_ERROR_INVALID_CALL for a queue that is not a direct one
- tests:
  - `wine-d3d12` `test_desktop_window` (failed, 3 checks): `d3d12.c:1454: Test failed: Got unexpected hr 0.`
  - `wine-d3d12` `test_invalid_command_queue_types` (failed, 2 checks): `d3d12.c:1525: Test failed: Got unexpected hr 0.`

### 30. rasterizer ordered views are reported and do not order

- class: bug; 2 tests
- PLAUSIBLE; where: src/d3d12/d3d12_device.cpp:417 reports them; raster_order_group in src/airconv/air_signature.hpp is what should order them; not traced further
- Windows: overlapping pixels' accesses happen in primitive order
- tests:
  - `vkd3d-proton` `test_rasterizer_ordered_views_dxbc` (failed, 419 checks): `test_rasterizer_ordered_views_dxbc:179:Test 0: Test failed: Pixel 0, 0: Got -2.000000, expected -4.000000.`
  - `vkd3d-proton` `test_rasterizer_ordered_views_dxil` (failed, 427 checks): `test_rasterizer_ordered_views_dxil:179:Test 0: Test failed: Pixel 0, 0: Got -2.000000, expected -4.000000.`

### 31. sampler feedback values differ from the suite's (tier 1.0 is reported), and CheckAccessFullyMapped says unmapped for mapped tiles

- class: bug; 2 tests
- PLAUSIBLE; where: src/d3d12/d3d12_device.cpp:503 reports the tier; src/airconv/dxil_converter.cpp:1598 (the status); not traced further
- Windows: the values vkd3d-proton recorded from Windows
- tests:
  - `vkd3d-proton` `test_texture_feedback_instructions_dxil` (failed, 4 checks): `test_texture_feedback_instructions_dxil:2121:Test 6: Test failed: Got residency 0, expected 0x1.`
  - `vkd3d-proton` `test_texture_feedback_instructions_sm51` (failed, 4 checks): `test_texture_feedback_instructions_sm51:2121:Test 6: Test failed: Got residency 0, expected 0x1.`

### 32. the shader converter refuses a shader: 'DXIL operation 58 (dx.op.cbufferLoad.i64) is not lowered yet', 'stream output of a primitive that only a geometry shader can take'

- class: missing; 2 tests
- CONFIRMED; where: src/airconv/dxil_converter.cpp:3379
- Windows: the pipeline is created
- tests:
  - `vkd3d-proton` `test_advanced_cbv_layout` (failed, 6 checks): `test_advanced_cbv_layout:2263:Test 1: Test failed: Failed to create compute pipeline state, hr 0x80004005.`
  - `vkd3d-proton` `test_create_pipeline_state` (failed, 1 checks): `test_create_pipeline_state:641:Test 2: Test failed: Got unexpected return value 0x80004005.`

### 33. a shader's denormal mode is not kept

- class: bug; 2 tests
- PLAUSIBLE; where: src/airconv/dxil_converter.cpp has no handling of fp32-denorm-mode
- Windows: denormals are preserved or flushed as the shader's attribute says
- tests:
  - `vkd3d-proton` `test_denorm_behavior_dxil` (failed, 1 checks): `test_denorm_behavior_dxil:2494:Test 3: Test failed: Value 1 mismatch, expected 5, got 0.`
  - `vkd3d-proton` `test_shader_sm62_denorm` (failed, 1 checks): `test_shader_sm62_denorm:1256:Test 1: Test failed: 0x0 != 0x14`

### 34. shared resources and keyed mutexes ('DeviceTexture: Failed to create shared handle', E_FAIL)

- class: missing; 2 tests
- PLAUSIBLE; where: OpenSharedResource (src/d3d11/d3d11_device.cpp:463, 650, 655) and the texture's CreateSharedHandle
- Windows: shared handles are created and opened
- tests:
  - `wine-d3d11` `test_keyed_mutex` (failed, 9 checks): `d3d11.c:35392: Test failed: got 0x80004005.`
  - `wine-d3d11` `test_shared_resource` (failed, 33 checks): `d3d11.c:35225: Test failed: Feature level 0x9100: MiscFlags 0x2, nthandle 0: got err:   DeviceTexture: Failed to create shared handle`

### 35. a back buffer an application holds does not hold its Direct3D 12 swap chain: the swap chain's count stays 1 where Windows has 2

- class: semantics; 1 tests
- CONFIRMED; where: src/d3d12/d3d12_swapchain.cpp:285 (GetBuffer hands out the buffer and counts nothing on the swap chain)
- Windows: the swap chain's count is one more while any of its buffers is held
- tests:
  - `wine-d3d12` `test_swapchain_refcount` (failed, 10 checks): `d3d12.c:1157: Test failed: Got refcount 1.`

### 36. render targets the pipeline does not name, or names while none is bound, get other targets' values

- class: bug; 1 tests
- PLAUSIBLE; where: the pass a draw starts in src/d3d12/d3d12_command_list.cpp PreDraw (about 560-600) against the pipeline's RTVFormats; not traced further
- Windows: a target the pipeline does not write is left alone
- tests:
  - `vkd3d-proton` `test_unused_attachments_mix_and_match` (failed, 1978 checks): `test_unused_attachments_mix_and_match:2887: Test failed: RT 0, pixel 1, expected 256.000000, got 995.000000`

### 37. a pipeline with blending enabled on an integer target the pixel shader does not write is refused ('pixel format 53 is not blendable')

- class: bug; 1 tests
- CONFIRMED; where: src/d3d12/d3d12_pipeline_graphics.cpp:468
- Windows: the pipeline is created; only one whose shader writes that target is refused
- tests:
  - `vkd3d-proton` `test_integer_blending_pipeline_state` (failed, 1 checks): `test_integer_blending_pipeline_state:147:Test 0: Test failed: Unexpected hr 0x80070057.`

### 38. the shader compiler faults ('the shader compiler faulted') on shaders with an embedded root signature

- class: bug; 1 tests
- PLAUSIBLE; where: src/airconv, reached from src/d3d12/d3d12_pipeline_graphics.cpp; the fault's place is not traced
- Windows: the pipeline is created, or refused with E_INVALIDARG where the signatures do not match
- tests:
  - `vkd3d-proton` `test_root_signature_embedded` (failed, 9 checks): `test_root_signature_embedded:1909:Test 0: Test failed: Expected hr #80070057, got hr #80004005`

### 39. feature levels 9_x: their shaders are refused ('Invalid DXBC bytecode') and their format support is not answered

- class: missing; 1 tests
- CONFIRMED; where: src/airconv/dxbc_converter.cpp:990 (shader model 2 and 3 code has no converter)
- Windows: a device of feature level 9_x compiles shader model 4_0_level_9_x code
- tests:
  - `wine-d3d11` `test_create_shader` (failed, 110 checks): `d3d11.c:5193: Test failed: Feature level 0xb100: Got unexpected hr err:   Failed to initialize shader: Invalid DXBC bytecode`

## Single tests: wrong results on valid input (PLAUSIBLE unless a place is named)

- `vkd3d-proton` `test_clear_unordered_access_view_buffer` (crashed, 1 checks): the test reads a readback that was not made (the copy of a cleared buffer view before it failed). First: `test_clear_unordered_access_view_buffer:778:Test 26: Test failed: Got 0x00000001, expected 0x00000000 at (262144, 0, 0).`. Crash at `d3d12.exe: test_clear_unordered_access_view_buffer at d3d12_clear.c:778:9`
- `vkd3d-proton` `test_copy_texture_buffer_d24` (failed, 17 checks): the depth plane of D24_UNORM_S8_UINT copied to a buffer is 0. First: `test_copy_texture_buffer_d24:855: Test failed: Got 0.000000, expected 0.066667 at (2,2).`
- `vkd3d-proton` `test_custom_border_color_srgb` (failed, 184 checks): a border color through an sRGB view. First: `test_custom_border_color_srgb:6465: Test failed: Value 0 (R, R, R, R), expected (0.125490, 0.125490, 0.125490, 0.125490) or (0.125490, 0.250980, 0.376`
- `vkd3d-proton` `test_depth_bias_formats` (failed, 4 checks): depth bias against the format's scale in 4 cases. First: `test_depth_bias_formats:2569:Test 0, pso 0: Test failed: Got 0xff00ff00, expected 0xff0000ff at (0, 0, 0).`
- `vkd3d-proton` `test_depth_stencil_sampling` (failed, 1 checks): a comparison sample of a depth texture after a clear. First: `test_depth_stencil_sampling:436:Test 3: Test failed: Got 1.00000000e+00, expected 0.00000000e+00 at (0, 0).`
- `vkd3d-proton` `test_fractional_viewports` (failed, 252 checks): SV_Position with a viewport at a fraction of a pixel. First: `test_fractional_viewports:435: Test failed: Got fragcoord {3.35000000e+01, 2.50000000e+00}, expected {1.00000000e+00, 1.00000000e+00} at (33, 2), offs`
- `vkd3d-proton` `test_index_buffer_edge_case_stream_output` (failed, 2 checks): stream output of an indexed draw at the index buffer's edge. First: `test_index_buffer_edge_case_stream_output:429: Test failed: Got {-1.00000000e+00, 1.00000000e+00, 0.00000000e+00, 1.00000000e+00}, expected {-1.000000`
- `vkd3d-proton` `test_large_heap` (failed, 18 checks): buffers placed in a large heap read 0. First: `test_large_heap:5213: Test failed: Got 0, expected 10 at 0.`
- `vkd3d-proton` `test_primitive_restart` (failed, 3 checks): the strip cut value in 3 of the cases (tests 1, 8, 10). First: `test_primitive_restart:1412:Test 1: Test failed: Got 0xffffffff, expected 0xff00ff00 at (0, 0, 0).`
- `vkd3d-proton` `test_resolve_subresource_depth` (failed, 16 checks): a depth resolve does not take what Windows takes (16 values). First: `test_resolve_subresource_depth:3690: Test failed: 0, 0: expected 0.015625, got 0.023438`
- `vkd3d-proton` `test_sample_instructions` (failed, 31 checks): 31 colors of sample instructions wrong (test 23). First: `test_sample_instructions:4504:Test 23: Test failed: Got color 0xff0000ff, expected 0xffffffff at (0, 0).`
- `vkd3d-proton` `test_sampler_feedback_decode_encode_min_mip` (failed, 92 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_decode_encode_min_mip:990: Test failed: Value 3: Expected 3, got 2`
- `vkd3d-proton` `test_sampler_feedback_format_features` (failed, 6 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_format_features:164: Test failed: Failed to query for format features, hr #80070057.`
- `vkd3d-proton` `test_sampler_feedback_grad` (failed, 60 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_grad:2456:Test 1: Test failed: Mip0: Expected #33, got #20.`
- `vkd3d-proton` `test_sampler_feedback_min_mip_level` (failed, 939 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_min_mip_level:566: Test failed: Coord 1, 0, 0: expected 4 or 255, got 5.`
- `vkd3d-proton` `test_sampler_feedback_min_mip_level_array` (failed, 1005 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_min_mip_level_array:566: Test failed: Coord 1, 0, 0: expected 4 or 255, got 5.`
- `vkd3d-proton` `test_sampler_feedback_npot_min_mip_level` (failed, 58 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_npot_min_mip_level:823: Test failed: Coord 16, 0: expected 0, got 255.`
- `vkd3d-proton` `test_sampler_feedback_npot_used_region` (failed, 32 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_npot_used_region:1868:Base resolution 15 x 16: Test failed: Mip 0, Coord 2, 3: expected 255, got 0.`
- `vkd3d-proton` `test_sampler_feedback_resource_creation` (failed, 4 checks): sampler feedback values differ from the suite's (tier 1.0 is reported). First: `test_sampler_feedback_resource_creation:99:Test 4: Test failed: Unexpected hr, expected #80070057, got #0.`
- `vkd3d-proton` `test_shader_instructions_dxil` (failed, 6 checks): 6 results of DXIL instructions wrong (tests 15 to 18). First: `test_shader_instructions_dxil:373:Test 15: Test failed: Value 3 mismatch: 4 != ffffffff`
- `vkd3d-proton` `test_shader_sm64_packed` (failed, 1 checks): one packed dot product result off by one. First: `test_shader_sm64_packed:1505:Test 12: Test failed: 0x4b800000 != 0x4b800001`
- `vkd3d-proton` `test_shader_sm66_is_helper_lane` (failed, 1 checks): IsHelperLane. First: `test_shader_sm66_is_helper_lane:2118: Test failed: Mismatch pixel 0, 0, (1.000000 4321.000000 4881.000000 8881.000000) != (1.000000 8321.000000 8881.0`
- `vkd3d-proton` `test_shader_waveop_maximal_convergence` (failed, 16 checks): wave operations after divergent flow do not reconverge as SM 6.x requires. First: `test_shader_waveop_maximal_convergence:1790: Test failed: Element 0, 25 != 12.`
- `vkd3d-proton` `test_suballocate_small_textures_size` (failed, 12150 checks): GetResourceAllocationInfo gives no 4 KiB alignment for small textures that ask for it. First: `test_suballocate_small_textures_size:2297:Test 0: fmt #47, bpp 4, width 512, height 256, levels 1, layers 2: Test failed: Alignment is not 4 KiB.`
- `vkd3d-proton` `test_view_min_lod` (failed, 11 checks): a view's ResourceMinLODClamp past its levels does not read 0 (11 cases). First: `test_view_min_lod:3742:Test 25: Test failed: Got 0x0f0f0f0f, expected 0x00000000 at (0, 0, 0).`
- `wine-d3d10core` `test_clear_state` (failed, 38 checks): SetVertexBuffers runs into an illegal instruction after ClearState. First: `d3d10core.c:5466: Test failed: Got unexpected blend factor {0.00000000e+000, 0.00000000e+000, 0.00000000e+000, 0.00000000e+000}.`
- `wine-d3d10core` `test_compressed_format_compatibility` (failed, 1590 checks): a copy between a block-compressed texture and the format of its blocks leaves the destination as it was (5970 pixels). First: `d3d10core.c:16647: Test failed: 0x1 -> 0x46: Got unexpected colour 0xff0000ff at 10, expected 0x00000000.`
- `wine-d3d10core` `test_format_compatibility` (failed, 279 checks): a copy between two formats of one bit layout leaves the destination as it was (194 pixels). First: `d3d10core.c:16453: Test failed: Test 6: Got unexpected colour 0xff0000ff at (1, 1), expected 0x00000000.`
- `wine-d3d10core` `test_stream_output` (failed, 126 checks): ExtractStreamOutputElements faults on a declaration Windows refuses. First: `d3d10core.c:15647: Test failed: Got unexpected hr 0.`

## Single tests: not implemented

- `vkd3d-proton` `test_fence_wait_robustness_shared` (timeout, 9 checks): OpenSharedHandle of a fence (E_NOTIMPL), then the test does not end. First: `test_fence_wait_robustness_shared:1261: Test failed: Failed to create shared handle, hr #80004001.`

## Single tests: Windows refuses, Mullion accepts

- `wine-d3d10core` `test_generate_mips` (failed, 111 checks): by its first failing check only. First: `d3d10core.c:17454: Test failed: Test 2: unexpectedly succeeded to create shader resource view, hr 0.`
- `wine-d3d10core` `test_geometry_shader` (failed, 2 checks): by its first failing check only. First: `d3d10core.c:15266: Test failed: Unexpected hr 0.`
- `wine-d3d10core` `test_input_layout_alignment` (failed, 6 checks): by its first failing check only. First: `d3d10core.c:12650: Test failed: Test 1: Got unexpected hr 0, expected 0x80070057.`
- `wine-d3d10core` `test_resource_access` (failed, 372 checks): by its first failing check only. First: `d3d10core.c:10484: Test failed: Got hr 0 for WRITE_DISCARD.`
- `wine-d3d10core` `test_swapchain_views` (failed, 1 checks): by its first failing check only. First: `d3d10core.c:11055: Test failed: Got unexpected hr 0.`
- `wine-d3d11` `test_buffer_srv` (failed, 1 checks): by its first failing check only. First: `d3d11.c:24845: Test failed: Got unexpected hr 0.`
- `wine-d3d11` `test_create_deferred_context` (failed, 2 checks): by its first failing check only. First: `d3d11.c:2534: Test failed: Got unexpected hr 0.`
- `wine-d3d11` `test_create_device` (timeout, 4 checks): by its first failing check only. First: `d3d11.c:2183: Test failed: Got unexpected hr 0.`
- `wine-d3d11` `test_fl10_stream_output_desc` (failed, 66 checks): by its first failing check only. First: `d3d11.c:27358: Test failed: Got unexpected hr 0.`
- `wine-d3d11` `test_generate_mips` (failed, 111 checks): by its first failing check only. First: `d3d11.c:30776: Test failed: Test 2: Got unexpected hr 0.`
- `wine-d3d11` `test_geometry_shader` (failed, 2 checks): by its first failing check only. First: `d3d11.c:26263: Test failed: Got unexpected hr 0.`
- `wine-d3d11` `test_getdc` (failed, 15 checks): by its first failing check only. First: `d3d11.c:17822: Test failed: Format B8G8R8A8_UNORM: Got unexpected hr 0.`
- `wine-d3d11` `test_input_layout_alignment` (failed, 6 checks): by its first failing check only. First: `d3d11.c:19378: Test failed: Test 1: Got unexpected hr 0, expected 0x80070057.`
- `wine-d3d11` `test_logic_op` (failed, 2 checks): by its first failing check only. First: `d3d11.c:34769: Test failed: Got hr 0.`
- `wine-d3d11` `test_resource_access` (timeout, 2246 checks): by its first failing check only. First: `d3d11.c:15647: Test failed: Feature level 0xa100: Got hr 0 for WRITE_DISCARD.`
- `wine-d3d11` `test_swapchain_views` (failed, 1 checks): by its first failing check only. First: `d3d11.c:16006: Test failed: Got unexpected hr 0.`

## Single tests: reference counts and object identity

- `vkd3d-proton` `test_create_graphics_pipeline_state` (failed, 5 checks): a pipeline holds a public reference on its root signature. First: `test_create_graphics_pipeline_state:198: Test failed: Got unexpected refcount 2.`
- `vkd3d-proton` `test_line_rasterization` (failed, 1 checks): a device reference is left. First: `test_line_rasterization:2758: Test failed: ID3D12Device has 1 references left.`
- `wine-d3d10core` `test_create_input_layout` (failed, 15 checks): by its first failing check only. First: `d3d10core.c:12558: Test failed: Got refcount 1, expected >= 2.`
- `wine-d3d10core` `test_create_shader` (failed, 8 checks): by its first failing check only. First: `d3d10core.c:3971: Test failed: Got unexpected refcount 1, expected >= 2.`
- `wine-d3d10core` `test_index_buffer_offset` (failed, 1 checks): by its first failing check only. First: `d3d10core.c:13531: Test failed: Device has 1 references left.`
- `wine-d3d10core` `test_stream_output_vs` (failed, 1 checks): by its first failing check only. First: `d3d10core.c:16021: Test failed: Device has 1 references left.`
- `wine-d3d11` `test_index_buffer_offset` (failed, 1 checks): by its first failing check only. First: `d3d11.c:21064: Test failed: Device has 1 references left.`
- `wine-d3d11` `test_stream_output_components` (failed, 1 checks): by its first failing check only. First: `d3d11.c:27853: Test failed: Device has 1 references left.`
- `wine-d3d11` `test_stream_output_vs` (failed, 1 checks): by its first failing check only. First: `d3d11.c:28035: Test failed: Device has 1 references left.`
- `wine-d3d11` `test_vertex_id` (failed, 1 checks): by its first failing check only. First: `d3d11.c:14095: Test failed: Device has 1 references left.`
- `wine-dxgi` `test_create_swapchain` (timeout, 10 checks): by its first failing check only. First: `dxgi.c:1830: Test failed: d3d10: Got unexpected refcount 3.`

## Unclassified: the first failing check, and nothing more looked at

- `wine-d3d10core` `test_blend` (failed, 1 checks): by its first failing check only. First: `d3d10core.c:6331: Test failed: Got unexpected color 0x007f0080.`
- `wine-d3d10core` `test_cube_maps` (failed, 18 checks): by its first failing check only. First: `d3d10core.c:7977: Test failed: Got {0.00000000e+000, 0.00000000e+000, 0.00000000e+000, 1.00000000e+000}, expected {1.00000000e+000, 0.00000000e+000, 0`
- `wine-d3d10core` `test_depth_clip` (failed, 3 checks): by its first failing check only. First: `d3d10core.c:18423: Test failed: Got 1.00000000e+000, expected 5.00000000e-001 at (0, 0), sub-resource 0.`
- `wine-d3d10core` `test_draw_depth_only` (failed, 3 checks): by its first failing check only. First: `d3d10core.c:11704: Test failed: Got 1.00000000e+000, expected 0.00000000e+000 at (0, 0), sub-resource 0.`
- `wine-d3d10core` `test_input_assembler` (failed, 1 checks): by its first failing check only. First: `d3d10core.c:12912: Test failed: Failed to create input layout for format 0x19, hr 0x80070057.`. Log: `err:   CreateInputLayout: Unsupported vertex format: 25`
- `wine-d3d10core` `test_layered_rtv_mismatch` (timeout, 1 checks): by its first failing check only. First: `d3d10core.c:19672: Test failed: Got colour {0.00000000e+000, 0.00000000e+000, 0.00000000e+000, 0.00000000e+000}.`. Log: `err:   Failed to create PSO: Error Domain=AGXMetalG17X Code=3 "output of type float4 is not compatible with a MTLPixelFormatRGBA32Uint color`
- `wine-d3d10core` `test_multiple_viewports` (crashed): by its first failing check only. Crash at `d3d10core_test.exe: test_multiple_viewports at d3d10core.c:18026:5`
- `wine-d3d10core` `test_multisample_resolve` (failed, 2 checks): by its first failing check only. First: `d3d10core.c:18312: Test failed: Got 0xffbcffbc, expected 0xff80ff80 at (0, 0), sub-resource 0.`
- `wine-d3d10core` `test_staging_buffers` (failed, 15 checks): by its first failing check only. First: `d3d10core.c:18511: Test failed: Got unexpected value 2.00000000e+000 at 1.`
- `wine-d3d10core` `test_swapchain_formats` (timeout): by its first failing check only
- `wine-d3d10core` `test_texture` (failed, 48 checks): by its first failing check only. First: `d3d10core.c:7684: Test failed: Test 6: Got unexpected color 0xff0000ff at (0, 0).`
- `wine-d3d11` `test_blend` (failed, 1 checks): by its first failing check only. First: `d3d11.c:8844: Test failed: Got unexpected colour 0x007f0080.`
- `wine-d3d11` `test_constant_buffer_offset` (failed, 18 checks): by its first failing check only. First: `d3d11.c:34377: Test failed: Got offset 2880154539.`
- `wine-d3d11` `test_cube_maps` (failed, 54 checks): by its first failing check only. First: `d3d11.c:10662: Test failed: Got {0.00000000e+000, 0.00000000e+000, 0.00000000e+000, 1.00000000e+000}, expected {1.00000000e+000, 0.00000000e+000, 0.00`
- `wine-d3d11` `test_deferred_context_map` (failed, 3 checks): by its first failing check only. First: `d3d11.c:33891: Test failed: Unexpected pointer 00000000DEADBEEF.`. Log: `err:   DeferredContext: Invalid NO_OVERWRITE map on deferred context occurs without any prior DISCARD map.`
- `wine-d3d11` `test_input_assembler` (failed, 1 checks): by its first failing check only. First: `d3d11.c:19642: Test failed: Format 0x19: Got unexpected hr 0x80070057.`. Log: `err:   CreateInputLayout: Unsupported vertex format: 25`
- `wine-d3d11` `test_multisample_resolve` (failed, 2 checks): by its first failing check only. First: `d3d11.c:31630: Test failed: Got 0xffbcffbc, expected 0xff80ff80 at (0, 0, 0), sub-resource 0.`
- `wine-d3d11` `test_negative_viewports` (failed, 2 checks): by its first failing check only. First: `d3d11.c:29083: Test failed: Feature level 0xb000: Got 0xff00ff00, expected 0xffffffff at (639, 479, 0), sub-resource 0.`
- `wine-d3d11` `test_nv12` (failed, 1 checks): by its first failing check only. First: `d3d11.c:36456: Test failed: Got hr 0x80070057.`
- `wine-d3d11` `test_quad_tessellation` (failed, 7 checks): by its first failing check only. First: `d3d11.c:26629: Test failed: Triangle 0 vertices {0.00000000e+000, 0.00000000e+000, 0.00000000e+000, 0.00000000e+000}, {0.00000000e+000, 0.00000000e+00`
- `wine-d3d11` `test_sample_shading` (failed, 4 checks): by its first failing check only. First: `d3d11.c:32025: Test failed: Test 2: Got unexpected value 4096.`
- `wine-d3d11` `test_staging_buffers` (failed, 15 checks): by its first failing check only. First: `d3d11.c:32296: Test failed: Got unexpected value 2.00000000e+000 at 1.`
- `wine-d3d11` `test_swapchain_formats` (timeout): by its first failing check only
- `wine-d3d11` `test_texture` (failed, 48 checks): by its first failing check only. First: `d3d11.c:10263: Test failed: Test 6: Got unexpected colour 0xff0000ff at (0, 0).`
- `wine-dxgi` `test_adapter_luid` (failed, 1 checks): by its first failing check only. First: `dxgi.c:1126: Test failed: Got unexpected hr 0x80004003.`
- `wine-dxgi` `test_factory_check_feature_support` (failed, 3 checks): by its first failing check only. First: `dxgi.c:7361: Test failed: Got unexpected hr 0x80070057.`. Log: `err:   DXGIFactory::CheckFeatureSupport: unknown feature 305419896`
- `wine-dxgi` `test_swapchain_formats` (timeout): by its first failing check only
- `wine-dxgi` `test_swapchain_present` (timeout): by its first failing check only

## The test's own assumptions (vkd3d-proton or Vulkan internals, vendor extensions, undefined behavior, todos of the suite)

- `vkd3d-proton` `test_ags_float8_conversion` (failed, 4064 checks): AMD's AGS shader extension. First: `test_ags_float8_conversion:6882: Test failed: value 0 (-0.000000): expected #80, got #0`
- `vkd3d-proton` `test_copy_texture_ds_edge_cases` (failed, 4 checks): a todo of the suite's own. First: `test_copy_texture_ds_edge_cases:436:Test 6: Test failed: Depth: expected #c00000, got #3fc00000.`
- `vkd3d-proton` `test_create_root_signature` (failed, 2 checks): a todo of the suite's own. First: `test_create_root_signature:122: Test failed: Got unexpected hr 0.`
- `vkd3d-proton` `test_custom_border_color_limits` (failed, 1024 checks): Vulkan's limit of custom border colors. First: `test_custom_border_color_limits:6216: Test failed: Value 7168, expected 0.000000, 0.000000, 0.000000, 0.000000, got 0.000000, 0.062745, 0.000000, 0.50`
- `vkd3d-proton` `test_custom_border_color_limits_compute` (failed, 1024 checks): Vulkan's limit of custom border colors. First: `test_custom_border_color_limits_compute:6216: Test failed: Value 7168, expected 0.000000, 0.000000, 0.000000, 0.000000, got 0.000000, 0.062745, 0.0000`
- `vkd3d-proton` `test_depth_bias_behaviour` (crashed): calls through ID3D12GraphicsCommandList9, which the list does not have. Crash at `ntdll.dll+0x345900000300000`
- `vkd3d-proton` `test_derivative_hoisting_dxbc` (failed, 2 checks): derivatives in non-uniform flow (D3D11.3 16.8.2), which the suite excuses on NVIDIA too. First: `test_derivative_hoisting_dxbc:14585: Test failed: (0, 0): Expected (11.000000, -14.000000, 1.000000, 1.000000), got (6.000000, -6.500000, 1.000000, 1.`
- `vkd3d-proton` `test_derivative_hoisting_dxil` (failed, 2 checks): derivatives in non-uniform flow (D3D11.3 16.8.2), which the suite excuses on NVIDIA too. First: `test_derivative_hoisting_dxil:14585: Test failed: (0, 0): Expected (11.000000, -14.000000, 1.000000, 1.000000), got (6.000000, -6.500000, 1.000000, 1.`
- `vkd3d-proton` `test_query_heap_cpu_resolve_occlusion` (crashed): calls through ID3D12Device15, which it found missing. Crash at `?+0x0`
- `vkd3d-proton` `test_query_heap_cpu_resolve_timestamp` (crashed): calls through ID3D12Device15, which it found missing. Crash at `d3d12.exe: test_query_heap_cpu_resolve_timestamp at d3d12_query.c:785:10`
- `vkd3d-proton` `test_raytracing_deferred_compilation` (failed, 1 checks): a todo of the suite's own. First: `test_raytracing_deferred_compilation:3747: Test failed: Expected identifier in collection.`
- `vkd3d-proton` `test_root_constant_indexing_dxil` (failed, 1 checks): a todo of the suite's own. First: `test_root_constant_indexing_dxil:14658: Test failed: Got hr 0, expected 0x80070057.`
- `vkd3d-proton` `test_sample_shading_coverage_mask` (failed, 34 checks): a todo of the suite's own. First: `test_sample_shading_coverage_mask:5333:PSO index 0, test index 0: Test failed: (0, 0, 0): expected 0000000f, got 2000000d`
- `vkd3d-proton` `test_sampler_rounding` (failed, 25 checks): where the hardware snaps a texel coordinate. First: `test_sampler_rounding:5626:Test 0: Test failed: UV [raw 255.609375] [snap lo 1.#00, snap hi 1.#00]: Expected value in range ([256.000000, 256.000000] `
- `vkd3d-proton` `test_uav_3d_sliced_view` (failed, 260 checks): a todo of the suite's own. First: `test_uav_3d_sliced_view:4295: Test failed: Error for mip 0 at 0, 0, 0. Got deadca7, expected 2040403.`
- `vkd3d-proton` `test_uav_counter_null_behavior_dxbc` (failed, 21 checks): undefined behavior (a UAV counter that is null). First: `test_uav_counter_null_behavior_dxbc:4546: Test failed: Unexpected value 0 = 79`
- `vkd3d-proton` `test_uav_counter_null_behavior_dxil` (failed, 21 checks): undefined behavior (a UAV counter that is null). First: `test_uav_counter_null_behavior_dxil:4546: Test failed: Unexpected value 0 = 78`
- `vkd3d-proton` `test_undefined_descriptor_heap_mismatch_types` (failed, 8 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_descriptor_heap_mismatch_types:5132: Test failed: Descriptor type 2, Shader type 3, expected 10.000000, got 0.000000.`
- `vkd3d-proton` `test_undefined_read_typed_buffer_as_untyped_simple_dxbc` (failed, 255 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_read_typed_buffer_as_untyped_simple_dxbc:1553: Test failed: Readback value for buffer iteration 1 is: 0`
- `vkd3d-proton` `test_undefined_read_typed_buffer_as_untyped_simple_dxil` (failed, 255 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_read_typed_buffer_as_untyped_simple_dxil:1553: Test failed: Readback value for buffer iteration 1 is: 0`
- `vkd3d-proton` `test_undefined_structured_raw_alias_dxbc` (failed, 880 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_structured_raw_alias_dxbc:865: Test failed: Structured: output 0, index 16, expected 268435473, got 268435456`
- `vkd3d-proton` `test_undefined_structured_raw_alias_dxil` (failed, 880 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_structured_raw_alias_dxil:865: Test failed: Structured: output 0, index 16, expected 268435473, got 268435456`
- `vkd3d-proton` `test_undefined_structured_raw_read_typed_dxbc` (failed, 552 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_structured_raw_read_typed_dxbc:1154: Test failed: output 0, index 0, expected (1, 1, 1, 1), got (0, 0, 0, 0)`
- `vkd3d-proton` `test_undefined_structured_raw_read_typed_dxil` (failed, 552 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_structured_raw_read_typed_dxil:1154: Test failed: output 0, index 0, expected (1, 1, 1, 1), got (0, 0, 0, 0)`
- `vkd3d-proton` `test_undefined_typed_read_structured_raw_dxbc` (failed, 608 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_typed_read_structured_raw_dxbc:1428: Test failed: output 0, index 0, expected (4, 4, 4, 4), got (0, 0, 0, 0)`
- `vkd3d-proton` `test_undefined_typed_read_structured_raw_dxil` (failed, 608 checks): undefined behavior, with the answers vkd3d-proton chose. First: `test_undefined_typed_read_structured_raw_dxil:1428: Test failed: output 0, index 0, expected (4, 4, 4, 4), got (0, 0, 0, 0)`
- `vkd3d-proton` `test_uninitialized_pixel_output_dxbc` (failed, 1 checks): undefined behavior (an output the shader does not write). First: `test_uninitialized_pixel_output_dxbc:14796: Test failed: Got {5.01960814e-01, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00}, expected {0.00000000e+0`
- `vkd3d-proton` `test_uninitialized_pixel_output_dxil` (failed, 1 checks): undefined behavior (an output the shader does not write). First: `test_uninitialized_pixel_output_dxil:14796: Test failed: Got {5.01960814e-01, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00}, expected {0.00000000e+0`
- `vkd3d-proton` `test_wmma_alloca` (failed, 256 checks): AMD's AGS WMMA shader extension. First: `test_wmma_alloca:6426: Test failed: Row 0, Col 0: Expected 21824.000000, got 0.000000`
- `vkd3d-proton` `test_wmma_copy_transpose` (failed, 768 checks): AMD's AGS WMMA shader extension. First: `test_wmma_copy_transpose:6325: Test failed: FP32: Row 0, Col 0: Expected 4.843750, got 0.000000`
- `vkd3d-proton` `test_wmma_element_wise` (failed, 2046 checks): AMD's AGS WMMA shader extension. First: `test_wmma_element_wise:6714: Test failed: FP32 ADD Value 0: Expected -200.000000, got 0.000000`
- `vkd3d-proton` `test_wmma_extract_insert` (failed, 511 checks): AMD's AGS WMMA shader extension. First: `test_wmma_extract_insert:6047: Test failed: Value 0: Expected 0xff, got 0x0`
- `vkd3d-proton` `test_wmma_fp32_fp8_conversions` (failed, 43651 checks): AMD's AGS WMMA shader extension. First: `test_wmma_fp32_fp8_conversions:5739: Test failed: -FP32 -> FP8 [0x8000 (-0.000000 / 512.0)]: Expected 0x80, got 0x0`
- `vkd3d-proton` `test_wmma_fp32_fp8_special_conversions` (failed, 12 checks): AMD's AGS WMMA shader extension. First: `test_wmma_fp32_fp8_special_conversions:5857: Test failed: FP32 unclamped (#7f7fffff) -> FP8: Expected 0x7f, got 0x0`
- `vkd3d-proton` `test_wmma_fp8_fp32_conversions` (failed, 252 checks): AMD's AGS WMMA shader extension. First: `test_wmma_fp8_fp32_conversions:5652: Test failed: FP8 -> FP32 [0x1]: Expected 0.001953, got 0.000000`
- `vkd3d-proton` `test_wmma_layout_assumptions` (failed, 256 checks): AMD's AGS WMMA shader extension. First: `test_wmma_layout_assumptions:6517: Test failed: Row 0, Col 0: Expected 101.000000, got 0.000000`
- `vkd3d-proton` `test_wmma_lds_layout` (failed, 512 checks): AMD's AGS WMMA shader extension. First: `test_wmma_lds_layout:6221: Test failed: value 0: Expected 1.000000, got 0.000000`
- `vkd3d-proton` `test_wmma_lds_transpose` (failed, 256 checks): AMD's AGS WMMA shader extension. First: `test_wmma_lds_transpose:6143: Test failed: Row 0, Col 0: Expected 317440.000000, got 0.000000`
- `vkd3d-proton` `test_wmma_matmul` (failed, 2816 checks): AMD's AGS WMMA shader extension. First: `test_wmma_matmul:5408:Test 0: Test failed: row 0, column 0, expected 2954.562500 (unrounded 2954.562500), got 0.000000`
- `vkd3d-proton` `test_wmma_matrix_length` (failed, 10 checks): AMD's AGS WMMA shader extension. First: `test_wmma_matrix_length:5948: Test failed: 0: Expected 8, got 0`
- `vkd3d-proton` `test_wmma_multi_matmul` (failed, 256 checks): AMD's AGS WMMA shader extension. First: `test_wmma_multi_matmul:5571: Test failed: row 0, col 0: Expected 0x58, got 0x00`
- `vkd3d-proton` `test_wmma_special_conversions` (failed, 252 checks): AMD's AGS WMMA shader extension. First: `test_wmma_special_conversions:6601: Test failed: Value 1: Expected 0.001953, got 0.000000`

## Not run again: timeouts of the first baseline

- `vkd3d-proton` `test_copy_block_spam` (timeout): a command buffer timed out on the GPU
- `vkd3d-proton` `test_copy_texture_mismatch_format` (timeout, 1 checks): Metal refuses a texture ('Texture Creation' assertion), then the test does not end. First: `test_copy_texture_mismatch_format:1376: Test failed: Expected 0x0a0a0a0a or 0x38003800, got #0`
- `vkd3d-proton` `test_fence_wait_robustness` (timeout): a command buffer timed out on the GPU (recorded before the queue's rework)
- `vkd3d-proton` `test_large_texel_buffer_view` (timeout, 9 checks): Metal refuses a texture descriptor (a buffer view too large for a texture), then the test does not end. First: `test_large_texel_buffer_view:5000:Test 5: Test failed: Got data 0, expected 0xaaaa at 0.`
- `vkd3d-proton` `test_null_rtv` (timeout, 1 checks): Metal's assertions 'A command encoder is already encoding to this command buffer' and 'commit command buffer with uncommitted encoder' with a null render target. First: `test_null_rtv:2857: Test failed: Got 0x00000000, expected 0xffffffff at (0, 0, 0).`
- `vkd3d-proton` `test_raytracing_huge_dispatch` (timeout): a DispatchRays of the largest size does not end
- `vkd3d-proton` `test_render_target_support_validation` (timeout, 2 checks): Metal refuses a render pipeline descriptor (a format it cannot render to), then the test does not end. First: `test_render_target_support_validation:5381: Test failed: Failed to create pipeline state, hr #80004005`
- `vkd3d-proton` `test_resolve_image_exhaustive_descriptors` (timeout): a command buffer fails with 'Insufficient Memory'
- `vkd3d-proton` `test_uav_clear_exhaustive_descriptors` (timeout): a command buffer fails with 'Insufficient Memory'

## Features Mullion does not report, and the tests that wait behind each

These tests are 'partly': they skipped their body with the reason given, which is the test's own wording. Most first.

- Workgraphs not supported (10): `test_workgraph_basic_recursion`, `test_workgraph_basic`, `test_workgraph_broadcast_input`, `test_workgraph_coalesced_input`, `test_workgraph_cross_group_sharing`, `test_workgraph_local_root_signature`, `test_workgraph_shared_inputs`, `test_workgraph_thread_input`, `test_workgraph_two_level_broadcast`, `test_workgraph_two_level_empty`
- Shader model 6.7 is not supported (8): `test_quad_vote_sm67_compute`, `test_sm67_dynamic_texture_offset`, `test_sm67_helper_lane_only_wave_ops`, `test_sm67_helper_lane_wave_ops`, `test_sm67_integer_sampling`, `test_sm67_multi_sample_uav`, `test_sm67_raw_gather`, `test_sm67_sample_cmp_level`
- SM 6.9 not supported, skipping (6): `test_sm69_fp16_isspecial`, `test_sm69_long_vector_intrinsics`, `test_sm69_long_vector_load_store_heap_desc`, `test_sm69_long_vector_load_store_root_desc`, `test_sm69_long_vector_storage`, `test_sm69_long_vector_waveops`
- Failed to get SDK configuration interface (5): `test_device_factory_create_device`, `test_device_factory`, `test_sdk_configuration1`, `test_sdk_configuration_creation`, `test_sdk_configuration_set_sdk_path`
- RelaxedFormatCasting is not supported (4): `test_enhanced_barrier_castable_dsv`, `test_enhanced_barrier_castable_formats_buffer`, `test_enhanced_barrier_castable_formats_validation`, `test_enhanced_barrier_castable_formats`
- VariableRateShading TIER_1 not supported (4): `test_vrs_clip_distance`, `test_vrs_depth_write_dxbc`, `test_vrs_depth_write_dxil`, `test_vrs_sample_mask`
- Device does not support SM 6.8 (3): `test_sm68_draw_parameters`, `test_sm68_sample_cmp_bias_grad`, `test_sm68_wave_size_range`
- Render passes not supported (3): `test_renderpass_rendering`, `test_renderpass_resolve_suspend_resume`, `test_renderpass_validation`
- SM 6.9 not supported, skipping Shader Execution Reordering test (3): `test_shader_execution_reordering_basic`, `test_shader_execution_reordering_ray_query`, `test_shader_execution_reordering_trace`
- Conservative rasterization not supported by device (2): `test_conservative_rasterization_dxbc`, `test_conservative_rasterization_dxil`
- Driver checks robustness based on byte offset (2): `test_uav_robustness_oob_structure_element_dxbc`, `test_uav_robustness_oob_structure_element_dxil`
- Raytracing tier  is not supported on this device. Skipping RT test (2): `test_raytracing_opacity_micro_map_ray_query`, `test_raytracing_opacity_micro_map`
- Some AMD drivers have a bug affecting the test (2): `wine-d3d10core` `test_depth_stencil_sampling`, `wine-d3d11` `test_depth_stencil_sampling`
- VariableRateShading TIER_2 not supported (2): `test_vrs_dxil`, `test_vrs_image`
- View instancing not supported by device (2): `test_view_instancing_indirect_state`, `test_view_instancing`
- Context does not support barycentrics (1): `test_sv_barycentric`
- CreateByteOffsetViewsSupported not set (1): `test_buffer_descriptor_byte_offset`
- Derivatives in mesh and amplification shaders not supported (1): `test_mesh_shader_rendering`
- Device does not support negative viewport height (1): `test_negative_viewports`
- DirectStorage meta command not supported (1): `test_dstorage_decompression`
- Dynamic index buffer strip cut is broken on AMD (native) (1): `test_dynamic_index_strip_cut`
- DynamicDepthBiasSupported not supported (1): `test_dynamic_depth_bias`
- ExecuteIndirect tier 1.1 not supported (1): `test_execute_indirect_state_tier_11`
- FP64 not supported (1): `test_denorm_behavior_dxbc`
- Failed to get ID3D11VideoDevice (1): `wine-d3d11` `test_h264_decoder`
- Failed to get device factory interface (1): `test_device_factory_creation`
- Feature level 11_0 required for unaligned UAV test (1): `wine-d3d11` `test_unaligned_raw_buffer_access`
- Format  is unsupported, skipping (1): `test_clear_uav_extreme_values`
- ID3D12DXVKInteropDevice1 not implemented (1): `test_vkd3d_dxvk_cmdbuf_interop`
- ID3D12Device11 not supported (1): `test_create_sampler2`
- ID3D12Device14 not supported (1): `test_raytracing_create_root_signature_from_subobject`
- ID3D12DeviceExt magic interface not exposed, skipping (1): `test_nvx_cubin`
- IndependentFrontAndBackStencilRefMaskSupported not supported (1): `test_depth_stencil_front_and_back`
- Min/max reduction filtering is not supported (1): `wine-d3d11` `test_filter_minmax`
- Non-normalized sampler coordinates not supported (1): `test_sampler_non_normalized_coordinates`
- Skipping FP64 test due to lack of feature support (1): `test_shader_instructions`
- Skipping exploratory test for root descriptor over/underflow test (1): `test_root_descriptor_offset_sign`
- Skipping test to avoid crash inside AMD driver (1): `test_sparse_default_mapping`
- Tight resource alignment not supported (1): `test_tight_resource_alignment`
- Tiled is not supported for D32 (1): `test_sparse_depth_stencil_rendering`
- Tiled resources tier 3 not supported (1): `test_get_resource_tiling`
- Tiled resources tier 4 not supported (1): `test_get_resource_tiling`
- Triangle fan topology not supported by device (1): `test_topology_triangle_fan`
- VariableRateShading not supported (1): `test_vrs`
- WARP adapter missing, skipping tests (1): `wine-dxgi` `test_multi_adapter`
- d3d10: This test requires two outputs (1): `wine-dxgi` `test_get_containing_output`
- d3d12: This test requires two outputs (1): `wine-dxgi` `test_get_containing_output`

