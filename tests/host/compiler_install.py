"""Exercise the runner's shader compiler install with minimal PE machine headers."""
import runpy
import struct
import sys
import tempfile
from pathlib import Path

install = runpy.run_path(sys.argv[1])["install_compilers"]


def pe(path, machine, payload):
    data = bytearray(160)
    struct.pack_into("<I", data, 0x3C, 128)
    struct.pack_into("<H", data, 132, machine)
    path.parent.mkdir(exist_ok=True, parents=True)
    path.write_bytes(data + payload)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source, wine, prefix = root / "source", root / "wine", root / "prefix"
    windows = prefix / "drive_c/windows"
    native = windows / "system32"
    native.mkdir(parents=True)
    for arch, machine in [("x64", 0x8664), ("x86", 0x14C)]:
        for name in ["dxcompiler.dll", "dxil.dll"]:
            pe(source / arch / name, machine, arch.encode())

    install(source, wine, prefix, replace=False)
    for dll in (source / "x64").iterdir():
        assert (native / dll.name).read_bytes() == dll.read_bytes(), dll.name

    # Repair a prefix from an older run with a compiler but no validator.
    (native / "dxil.dll").unlink()
    pe(native / "dxcompiler.dll", 0x8664, b"existing")
    (windows / "syswow64").mkdir()
    install(source, wine, prefix, replace=False)
    assert (native / "dxcompiler.dll").read_bytes().endswith(b"existing")
    assert (native / "dxil.dll").read_bytes() == (source / "x64/dxil.dll").read_bytes()
    for dll in (source / "x86").iterdir():
        assert (windows / "syswow64" / dll.name).read_bytes() == dll.read_bytes()

    install(source, wine, prefix, replace=True)
    assert (native / "dxcompiler.dll").read_bytes() == (source / "x64/dxcompiler.dll").read_bytes()

    prefix32 = root / "32-bit-prefix"
    system32 = prefix32 / "drive_c/windows/system32"
    pe(system32 / "ntdll.dll", 0x14C, b"")
    install(source, wine, prefix32, replace=True)
    for dll in (source / "x86").iterdir():
        assert (system32 / dll.name).read_bytes() == dll.read_bytes()
print("passed: shader compiler installation")
