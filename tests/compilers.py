#!/usr/bin/env python3
"""puts Microsoft's shader compilers, which the tests compile their HLSL with, into a directory: one subdirectory per
architecture (arm64, x64, x86) with d3dcompiler_47.dll, dxcompiler.dll and dxil.dll, and the licenses they come with.

  tests/compilers.py [DIRECTORY]   (default: tests-compilers here; <build>/tests-compilers is where run.py looks)

run.py takes them from the directory --compilers names, or from the first build's tests-compilers, and puts those of
the machines a Wine runs into its prefix (docs/TESTING.md, section 4). they are downloaded once, from Microsoft's
releases, and checked against the hashes below: DXC's release (LICENSE-MS.txt covers dxil.dll, LICENSE-LLVM.txt the
rest) and the Windows SDK's Redist\\D3D, distributable code under the SDK's license. nothing here changes them."""

import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

DXC = ("https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip",
       "dxc_2026_09_29.zip", "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")
SDK = ("https://www.nuget.org/api/v2/package/Microsoft.Windows.SDK.CPP/10.0.28000.2705",
       "microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg",
       "a74ca8f9af98bd61925d9e2932ad98f746306123167b8f0bba99cd9ea9f03807")
ARCHITECTURES = ("arm64", "x64", "x86")
SDK_LICENSE = "https://aka.ms/WinSDKLicenseURL"


def fetched(downloads, url, name, sha256):
    """the file downloaded from url, once, with the hash it must have"""
    path = downloads / name
    if not path.exists():
        print(f"downloading {url}", flush=True)
        partial = path.with_suffix(".part")
        with urllib.request.urlopen(url) as response, open(partial, "wb") as file:
            shutil.copyfileobj(response, file)
        partial.rename(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != sha256:
        sys.exit(f"{path} has SHA-256 {digest}, not {sha256}: remove it to download it again")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", nargs="?", type=Path, default=Path("tests-compilers"))
    to = parser.parse_args().directory.resolve()
    downloads = to / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    dxc, sdk = fetched(downloads, *DXC), fetched(downloads, *SDK)
    with zipfile.ZipFile(dxc) as archive:
        # DXC's zip names its files with backslashes (bin\arm64\dxil.dll)
        for member in archive.namelist():
            path = member.replace("\\", "/")
            parts = path.split("/")
            if len(parts) == 3 and parts[0] == "bin" and parts[1] in ARCHITECTURES and parts[2] in ("dxcompiler.dll", "dxil.dll"):
                (to / parts[1]).mkdir(exist_ok=True)
                (to / parts[1] / parts[2]).write_bytes(archive.read(member))
            elif path.startswith("LICENSE-"):
                (to / path).write_bytes(archive.read(member))
    with zipfile.ZipFile(sdk) as archive:
        for architecture in ARCHITECTURES:
            (to / architecture).mkdir(exist_ok=True)
            (to / architecture / "d3dcompiler_47.dll").write_bytes(
                archive.read(f"c/Redist/D3D/{architecture}/d3dcompiler_47.dll"))
    (to / "NOTICE").write_text(f"dxcompiler.dll, dxil.dll: {DXC[1]} ({DXC[0]})\n"
                               f"d3dcompiler_47.dll: Windows SDK {SDK[1]} ({SDK[0]}), Redist\\D3D, under {SDK_LICENSE}\n")
    print(f"shader compilers: {to}")


if __name__ == "__main__":
    main()
