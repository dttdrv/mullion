#!/usr/bin/env python3
"""fixtures for tests/coverage.py's PE optional-header reader.

A coverage report reads a library's preferred address out of its optional header to turn the symbol table's virtual
addresses into offsets. PE32 and PE32+ put ImageBase at different offsets (28 and 24 bytes into the optional header),
so reading the PE32+ field unconditionally misreads a 32-bit library. These fixtures are built, not taken from a
toolchain, so they run on every host."""

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import coverage  # noqa: E402


def pe(magic, image_base, header_at=0x40):
    data = bytearray(0x200)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, header_at)
    data[header_at:header_at + 4] = b"PE\0\0"
    struct.pack_into("<H", data, header_at + 4, 0x8664 if magic == 0x20B else 0x14C)
    struct.pack_into("<H", data, header_at + 24, magic)
    if magic == 0x10B:
        struct.pack_into("<I", data, header_at + 24 + 28, image_base)
    else:
        struct.pack_into("<Q", data, header_at + 24 + 24, image_base)
    return bytes(data)


class ImageBaseTest(unittest.TestCase):
    def test_pe32(self):
        self.assertEqual(coverage.image_base(pe(0x10B, 0x00400000)), 0x00400000)

    def test_pe32_high_base(self):
        self.assertEqual(coverage.image_base(pe(0x10B, 0x7FFF0000)), 0x7FFF0000)

    def test_pe32_plus(self):
        self.assertEqual(coverage.image_base(pe(0x20B, 0x140000000)), 0x140000000)

    def test_header_not_at_0x40(self):
        self.assertEqual(coverage.image_base(pe(0x10B, 0x10000000, header_at=0x80)), 0x10000000)

    def test_not_a_pe(self):
        data = bytearray(pe(0x20B, 0x140000000))
        data[0:2] = b"ZM"
        with self.assertRaises(ValueError):
            coverage.image_base(bytes(data))

    def test_bad_signature(self):
        data = bytearray(pe(0x20B, 0x140000000))
        data[0x40:0x44] = b"NE\0\0"
        with self.assertRaises(ValueError):
            coverage.image_base(bytes(data))

    def test_unknown_magic(self):
        data = bytearray(pe(0x20B, 0x140000000))
        struct.pack_into("<H", data, 0x40 + 24, 0x10C)
        with self.assertRaises(ValueError):
            coverage.image_base(bytes(data))

    def test_truncated(self):
        with self.assertRaises(ValueError):
            coverage.image_base(pe(0x20B, 0x140000000)[:0x50])


if __name__ == "__main__":
    unittest.main()
