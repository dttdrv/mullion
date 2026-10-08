# contract: a tile belongs to the middle frame only when its two neighbours agree and it differs from both,
# by the requested per-channel tolerance and pixel share (the frame diagnostic's file and comparison contract).
# channel storage follows D3D11.3 19.1.3.1: "Channel ordering of R/G/B/A/D/S/X in format name, read from left to
# right indicates order of placement of the channel storage from \"first\" (left) to \"last\" (right)."
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    tool = Path(sys.argv[1])
    if not tool.is_file():
        raise SystemExit(f'frame script missing: {tool}')
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)

        def write(directory, number, rectangles=(), fmt=28, size=(12, 8), padding=0):
            directory.mkdir(exist_ok=True)
            width, height = size
            data = bytearray()
            for y in range(height):
                for x in range(width):
                    color = next((c for left, top, w, h, c in rectangles
                                  if left <= x < left + w and top <= y < top + h), (0, 0, 0, 255))
                    if fmt in (28, 29, 87, 91):
                        data.extend(bytes(color if fmt in (28, 29) else (color[2], color[1], color[0], color[3])))
                    elif fmt == 24:
                        channels = [round(c * maximum / 255) for c, maximum in zip(color, (1023, 1023, 1023, 3))]
                        data.extend(struct.pack('<I', sum(c << shift for c, shift in zip(channels, (0, 10, 20, 30)))))
                    else:
                        data.extend(struct.pack('<4e', *(c / 255 for c in color)))
                data.extend(bytes(padding))
            row = width * (struct.calcsize('<4e') if fmt == 10 else struct.calcsize('<I')) + padding
            path = directory / f'swap_{number}.frame'
            path.write_bytes(f'mullion frame {number} {width} {height} {fmt} {row}\n'.encode() + data)
            return path

        def run(directory, *options):
            result = subprocess.run([sys.executable, str(tool), str(directory), '--tile', '2', *options],
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            return result.stdout.splitlines(), result.stderr

        red = (255, 0, 0, 255)
        rectangle = (2, 2, 4, 4, red)
        for fmt in (28, 29, 87, 91, 24, 10):
            directory = root / str(fmt)
            # reverse creation and lexical order: the header's number decides the neighbours
            for number in (11, 10, 9):
                write(directory, number, [rectangle] if number == 10 else [], fmt, padding=4)
            crops = directory / 'crops'
            lines, errors = run(directory, '--crops', str(crops))
            assert len(lines) == 1 and 'frame 10 rectangle 2 2 4 4 ' in lines[0], lines
            assert 'share 1.000000' in lines[0] and 'difference 1.000000' in lines[0], lines
            assert not errors, errors
            crop, = crops.glob('*.ppm')
            header, dimensions, maximum, pixels = crop.read_bytes().split(b'\n', 3)
            assert (header, dimensions, maximum) == (b'P6', b'12 4', b'255')
            assert pixels == (bytes((0, 0, 0)) * 4 + bytes(red[:3]) * 4 + bytes((0, 0, 0)) * 4) * 4

        for name, middle, after in [('equal', [], []), ('stays', [rectangle], [rectangle])]:
            directory = root / name
            for number, rectangles in enumerate(([], middle, after)):
                write(directory, number, rectangles)
            assert run(directory)[0] == []

        directory = root / 'neighbours'
        for number, value in enumerate((0, 255, 128)):
            write(directory, number, [(*rectangle[:4], (value, 0, 0, 255))])
        lines, errors = run(directory)
        assert not lines and not errors, (lines, errors)

        directory = root / 'chains'
        for number in range(3):
            path = write(directory, number, [rectangle] if number == 1 else [])
            path.rename(directory / f'one_{number}.frame')
            path = write(directory, number, [(8, 0, 2, 2, red)] if number == 1 else [])
            path.rename(directory / f'two_{number}.frame')
        lines, _ = run(directory)
        assert len(lines) == 2 and any('rectangle 2 2 4 4 ' in line for line in lines)
        assert any('rectangle 8 0 2 2 ' in line for line in lines)

        directory = root / 'moves'
        other = (8, 2, 2, 4, red)
        for number in range(5):
            write(directory, number, [other if number % 2 else rectangle])
        lines, _ = run(directory)
        assert len(lines) == 6, lines
        for number in range(1, 4):
            own = [line for line in lines if f'frame {number} rectangle' in line]
            assert len(own) == 2 and any('rectangle 2 2 4 4 ' in line for line in own)
            assert any('rectangle 8 2 2 4 ' in line for line in own)

        directory = root / 'thresholds'
        for number in range(3):
            write(directory, number, [(0, 0, 1, 1, (16, 0, 0, 255))] if number == 1 else [])
        assert run(directory, '--tolerance', str(16 / 255), '--share', '0.25')[0] == []
        lines, _ = run(directory, '--tolerance', str(15 / 255), '--share', '0.25')
        assert len(lines) == 1 and 'rectangle 0 0 2 2 ' in lines[0] and 'share 0.250000' in lines[0]
        assert run(directory, '--tolerance', '0', '--share', '0.26')[0] == []
        # the share is joint per pixel: two different sets of pixels changed against the two neighbours do not count
        write(directory, 0, [(0, 0, 1, 1, red)])
        write(directory, 1, [(0, 0, 2, 1, red)])
        write(directory, 2, [(1, 0, 1, 1, red)])
        assert run(directory, '--share', '0.25')[0] == []

        directory = root / 'edge'
        for number in range(3):
            write(directory, number, [(4, 2, 1, 1, red)] if number == 1 else [], size=(5, 3))
        assert 'rectangle 4 2 1 1 ' in run(directory)[0][0]

        directory = root / 'shape'
        for number in range(3):
            write(directory, number, [(0, 0, 4, 2, red), (0, 2, 2, 2, red)] if number == 1 else [])
        lines, _ = run(directory)
        assert len(lines) == 2 and 'rectangle 0 0 4 2 ' in lines[0] and 'rectangle 0 2 2 2 ' in lines[1], lines

        directory = root / 'alpha'
        for number in range(3):
            write(directory, number, [(0, 0, 2, 2, (0, 0, 0, 0))] if number == 1 else [])
        assert 'rectangle 0 0 2 2 ' in run(directory)[0][0]

        directory = root / 'sizes'
        for number in range(5):
            write(directory, number, [rectangle] if number == 2 else [], size=(10, 8) if number == 2 else (12, 8))
        lines, errors = run(directory)
        assert not lines and 'swap_2.frame' in errors and 'size' in errors, (lines, errors)

        directory = root / 'gap'
        for number in (0, 2, 3):
            write(directory, number, [rectangle] if number == 2 else [])
        assert run(directory)[0] == []

        directory = root / 'bad'
        path = write(directory, 1)
        path.write_bytes(path.read_bytes()[:-1])
        unknown = write(directory, 2, fmt=100)
        lines, errors = run(directory)
        assert not lines and path.name in errors and 'bytes' in errors and unknown.name in errors and 'format' in errors
        result = subprocess.run([sys.executable, str(tool), str(root), '--tile', '0'], capture_output=True)
        assert result.returncode != 0
    print('passed: frame regions, formats, crops, thresholds, edges, gaps and invalid files')


if __name__ == '__main__':
    main()
