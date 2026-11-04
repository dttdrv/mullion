Direct3D 9 tests: each program draws, reads the result back and prints `PASS` or `FAIL` for each check. `tests/run.py` runs them like any other test, on a build with `-Denable_d3d9=true`.

```sh
tests/d3d9/run.sh <wine-install-prefix>
```

Runs them 64- and 32-bit from `build/` and `build32/` in a fresh prefix under `toolchains/test`. `D3D9_TEST_BUILDS=build32` and `D3D9_TESTS="d3d9_ps1 d3d9_stretch"` narrow the run, and `D3D9_TEST_FULLSCREEN=1` adds `d3d9_fullscreen`, which takes over the display. `tart.sh` runs the same in a throwaway macOS VM, whose GPU is Mac2-only and whose 32-bit results are not reliable. `winetest.sh` runs Wine's own d3d9 conformance suites.

Rebuilt DLLs reach the prefix through `install.sh`; a test run directly loads the last ones installed.
