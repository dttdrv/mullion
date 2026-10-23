# a measurement, not a test: nothing here is declared with test(), so the suite's counts never include it.
# tools/frametime.py drives it under a Wine as each GPU family.
executable('frametime', ['frametime.cpp'],
  dependencies: [ cpp.find_library('d3d11'), cpp.find_library('d3d12'), cpp.find_library('dxgi'),
                  cpp.find_library('d3dcompiler_47') ]
)
