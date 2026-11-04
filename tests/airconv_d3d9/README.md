Numeric tests for airconv's D3D9 (SM2/SM3) front end: each case renders one pixel on the GPU and is compared to a Python reference.

```sh
swiftc -O rendercheck.swift -o rendercheck
x86_64-w64-mingw32-g++ -O1 -static hlslc.cpp -o hlslc.exe -ld3dcompiler_47
AIRCONV=../../build/src/airconv/darwin/airconv RENDERCHECK=./rendercheck WINE=wine HLSLC=hlslc.exe python3 sm3_tests.py
AIRCONV=../../build/src/airconv/darwin/airconv RENDERCHECK=./rendercheck python3 sm3_raw_tests.py
```

`sm3_tests.py` compiles HLSL with Wine's d3dcompiler and must run first; `sm3_raw_tests.py` hand-assembles features that compiler never emits (bool and int registers, `loop aL`, `mova`, predication, subroutines, `texkill`).
