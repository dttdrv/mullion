# the D3D12 performance probe, of the build's machine: a benchmark that perf_probe.py runs, not a test
executable('d3d12_perf_probe', 'd3d12_perf_probe.cpp', dependencies: d3d12_test_deps + [cpp.find_library('user32')])
# the D3D11 performance probe: a benchmark of the immediate context's CPU costs, run by hand, not a test
executable('d3d11_perf_probe', 'd3d11_perf_probe.cpp',
           dependencies: [cpp.find_library('d3d11'), cpp.find_library('dxgi'), cpp.find_library('d3dcompiler_47')])
