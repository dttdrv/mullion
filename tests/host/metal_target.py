# metal_target.py <library> <-mmacosx-version-min=X.Y>: every Metal library the built library embeds is made for the
# oldest macOS Mullion runs on (meson.build's macos_min). a library made for a newer macOS does not load there, and
# the metal compiler makes one for the SDK's own version unless told otherwise
import re
import struct
import sys

data = open(sys.argv[1], 'rb').read()
want = tuple(int(n) for n in re.fullmatch(r'-mmacosx-version-min=(\d+)\.(\d+)', sys.argv[2]).groups())
found = []
at = data.find(b'MTLB')
while at >= 0:
    # the header: 'MTLB', the platform word 0x8001, the file version, the type, the OS (0x81 is macOS), its major and
    # minor version
    if data[at + 4:at + 6] == b'\x01\x80' and data[at + 11] == 0x81:
        found.append(struct.unpack_from('<HH', data, at + 12))
    at = data.find(b'MTLB', at + 4)
print(sys.argv[1], 'embeds Metal libraries for macOS', ', '.join('%d.%d' % v for v in found) or 'none',
      '; Mullion runs on %d.%d' % want)
sys.exit(0 if found and all(v == want for v in found) else 1)
