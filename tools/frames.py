#!/usr/bin/env python3
"""Find transient regions in Mullion .frame captures, independently for each swap chain.

A pixel qualifies when both neighbours agree within --tolerance and the middle pixel differs
from each by more than it. A tile qualifies when at least --share of its pixels qualify.
Tolerance defaults to 0.02 per channel: UNORM channels are divided by their own maximum;
float channels retain their values. Share defaults to 0.75, tile size to 16 pixels.
Regions cover only qualifying tiles; difference is their qualifying pixels' mean maximum
channel change against either neighbour. Crops clamp RGB to [0, 1] for an 8-bit PPM.
"""
import argparse
import collections
import math
import re
import struct
import sys
from pathlib import Path

# DXGI_FORMAT (dxgiformat.h) and D3D11.3 19.1.3.1 define these channel widths and their storage order.
RGBA8, RGBA8_SRGB = 28, 29
BGRA8, BGRA8_SRGB = 87, 91
RGB10A2, RGBA16_FLOAT = 24, 10
FORMATS = {RGBA8: '<4B', RGBA8_SRGB: '<4B', BGRA8: '<4B', BGRA8_SRGB: '<4B', RGB10A2: '<I', RGBA16_FLOAT: '<4e'}


def read(path, header_only=False):
    with path.open('rb') as file:
        header = file.readline().decode('ascii').split()
        if len(header) != 7 or header[:2] != ['mullion', 'frame']:
            raise ValueError('expected mullion frame number width height format row-bytes')
        number, width, height, fmt, row = map(int, header[2:])
        if fmt not in FORMATS:
            raise ValueError(f'unsupported DXGI format {fmt}')
        if number < 0 or width <= 0 or height <= 0 or row < width * struct.calcsize(FORMATS[fmt]):
            raise ValueError('invalid number, size or row bytes')
        if path.stat().st_size - file.tell() != height * row:
            raise ValueError(f'expected {height * row} pixel bytes')
        return number, width, height, fmt, row, b'' if header_only else file.read()


def pixel(frame, x, y):
    _, _, _, fmt, row, data = frame
    values = struct.unpack_from(FORMATS[fmt], data, y * row + x * struct.calcsize(FORMATS[fmt]))
    if fmt == RGB10A2:
        word, = values
        values = []
        for bits in (10, 10, 10, 2):
            maximum = (1 << bits) - 1
            values.append((word & maximum) / maximum)
            word >>= bits
    elif fmt != RGBA16_FLOAT:
        values = [v / 255 for v in values]
        if fmt in (BGRA8, BGRA8_SRGB):
            values[0], values[2] = values[2], values[0]
    return values


def delta(a, b):
    return max(0 if x == y else abs(x - y) if math.isfinite(x - y) else math.inf for x, y in zip(a, b))


def regions(frames, tile, tolerance, share):
    _, width, height, _, _, _ = frames[1]
    tiles = {}
    for top in range(0, height, tile):
        for left in range(0, width, tile):
            count, change, total = 0, 0.0, 0
            for y in range(top, min(top + tile, height)):
                for x in range(left, min(left + tile, width)):
                    before, middle, after = [pixel(frame, x, y) for frame in frames]
                    a, b = delta(middle, before), delta(middle, after)
                    total += 1
                    if delta(before, after) <= tolerance and min(a, b) > tolerance:
                        count += 1
                        change += max(a, b)
            if count and count / total >= share:
                tiles[left, top] = (count, total, change)
    while tiles:
        left, top = min(tiles, key=lambda point: (point[1], point[0]))
        right, bottom = left + tile, top + tile
        while (right, top) in tiles:
            right += tile
        while all((x, bottom) in tiles for x in range(left, right, tile)):
            bottom += tile
        selected = [tiles.pop((x, y)) for y in range(top, bottom, tile) for x in range(left, right, tile)]
        count, total, change = map(sum, zip(*selected))
        yield (left, top, min(right, width) - left, min(bottom, height) - top), count / total, change / count


def crop(path, frames, rectangle):
    left, top, width, height = rectangle
    with path.open('wb') as file:
        file.write(f'P6\n{width * len(frames)} {height}\n255\n'.encode())
        for y in range(top, top + height):
            for frame in frames:
                for x in range(left, left + width):
                    file.write(bytes(round(min(1, max(0, c)) * 255) for c in pixel(frame, x, y)[:3]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('paths', nargs='+', type=Path, help='directories of .frame files, or individual files')
    parser.add_argument('--tolerance', type=float, default=0.02, help='per-channel tolerance (default: %(default)s)')
    parser.add_argument('--share', type=float, default=0.75, help='qualifying share per tile (default: %(default)s)')
    parser.add_argument('--tile', type=int, default=16, help='tile side in pixels (default: %(default)s)')
    parser.add_argument('--crops', type=Path, help='directory for before/middle/after crops side by side as PPM')
    args = parser.parse_args()
    if args.tile <= 0 or not math.isfinite(args.tolerance) or args.tolerance < 0 or not 0 < args.share <= 1:
        parser.error('tile must be positive, tolerance finite and nonnegative, share in (0, 1]')
    groups = collections.defaultdict(list)
    paths = {p.resolve() for path in args.paths for p in (path.glob('*.frame') if path.is_dir() else [path])}
    for path in sorted(paths):
        try:
            number = read(path, True)[0]
            match = re.fullmatch(r'(.*)_\d+', path.stem)
            groups[path.parent, match[1] if match else ''].append((number, path))
        except (OSError, ValueError) as error:
            print(f'{path}: {error}', file=sys.stderr)
    if args.crops:
        args.crops.mkdir(parents=True, exist_ok=True)
    for group, files in enumerate(groups.values()):
        neighbours = collections.deque(maxlen=3)
        for number, path in sorted(files):
            try:
                neighbours.append((path, read(path)))
            except (OSError, ValueError) as error:
                print(f'{path}: {error}', file=sys.stderr)
                neighbours.clear()
                continue
            if len(neighbours) != 3:
                continue
            frames = [frame for _, frame in neighbours]
            before, middle, after = frames
            if (before[0], after[0]) != (middle[0] - 1, middle[0] + 1):
                continue
            path = neighbours[1][0]
            if before[1:4] != middle[1:4] or after[1:4] != middle[1:4]:
                print(f'{path}: size or format differs from its neighbours; skipped', file=sys.stderr)
                continue
            for rectangle, share, difference in regions(frames, args.tile, args.tolerance, args.share):
                print(f'{path.name}: frame {middle[0]} rectangle {" ".join(map(str, rectangle))} '
                      f'share {share:.6f} difference {difference:.6f}')
                if args.crops:
                    crop(args.crops / f'{group}_{path.stem}_{"_".join(map(str, rectangle))}.ppm', frames, rectangle)


if __name__ == '__main__':
    main()
