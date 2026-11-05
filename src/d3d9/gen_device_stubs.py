#!/usr/bin/env python3
# Regenerates d3d9_device_unimpl.cpp: a log-once stub for every IDirect3DDevice9Ex method not defined in the other
# device sources. Run from the repository root after adding or removing a device method.
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'include/native/directx/d3d9.h'
SOURCES = [ROOT / 'src/d3d9/d3d9_device.cpp', ROOT / 'src/d3d9/d3d9_device_draw.cpp', ROOT / 'src/d3d9/d3d9_device_ff.cpp',
           ROOT / 'src/d3d9/d3d9_device_misc.cpp', ROOT / 'src/d3d9/d3d9_device_process.cpp']
OUT = ROOT / 'src/d3d9/d3d9_device_unimpl.cpp'
CLASS = 'MTLD3D9Device'

# Setters that games call blindly at startup get D3D_OK so they keep going.
OK_RESULT = re.compile(r'^(Set|LightEnable$|MultiplyTransform$|DeletePatch$)')


def methods():
    src = HEADER.read_text()
    body = re.search(r'DECLARE_INTERFACE_\(IDirect3DDevice9Ex,[^)]*\)\s*\{(.*?)\n\};', src, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    for head, args in re.findall(r'STDMETHOD(_\([^,]+,\s*\w+\)|\(\w+\))\s*\((.*?)\)\s*PURE;', body, re.S):
        if head.startswith('_'):
            ret, name = [x.strip() for x in head[2:-1].split(',')]
        else:
            ret, name = 'HRESULT', head[1:-1]
        args = re.sub(r'^THIS_?\s*', '', ' '.join(args.split())).replace('WINBOOL', 'BOOL')
        yield ('BOOL' if ret == 'WINBOOL' else ret), name, args


def implemented():
    names = set()
    for path in SOURCES:
        names |= set(re.findall(rf'^{CLASS}::(\w+)\(', path.read_text(), re.M))
    return names


def main():
    done = implemented() | {'QueryInterface', 'AddRef', 'Release'}
    header = '\n'.join(SOURCES[0].read_text().splitlines()[:17])
    out = [header, '', '#include "d3d9_device.hpp"', '#include "log/log.hpp"', '', 'namespace mullion {', '']
    for ret, name, args in methods():
        if name in done:
            continue
        out.append(f'{ret} STDMETHODCALLTYPE\n{CLASS}::{name}({args}) {{')
        out.append(f'  ERR_ONCE("{CLASS}::{name}: not implemented");')
        if ret == 'HRESULT':
            out.append('  return D3D_OK;' if OK_RESULT.match(name) else '  return D3DERR_INVALIDCALL;')
        elif ret != 'void':
            out.append('  return 0;')
        out.append('}\n')
    out.append('} // namespace mullion\n')
    OUT.write_text('\n'.join(out))


main()
