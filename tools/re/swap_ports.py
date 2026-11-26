"""Swap two controller ports in a .bm64rec from an input frame on.

Usage: python swap_ports.py in.bm64rec out.bm64rec <from_frame> [port_a port_b]   (ports default 0 1)

Each frame is four 6-byte port records { u16 button; s8 x; s8 y; u8 err; u8 reserved }. The first
five bytes of the two records are swapped; the reserved byte stays where it is, because port 0's
carries the director command. Used to replay a co-op run with the other player doing the walking
(for example to see which game logic only reacts to the leader).
"""
import struct, sys


def main(src, dst, start, a=0, b=1):
    data = bytearray(open(src, 'rb').read())
    if data[:8] != b'BM64INP\0':
        raise SystemExit('not a recording')
    version, seed, save_size = struct.unpack_from('<III', data, 8)
    pos = 20
    if version >= 2:
        pos += 8
    pos += save_size
    frames = (len(data) - pos) // 24
    swapped = 0
    for i in range(frames):
        if i < start:
            continue
        base = pos + i * 24
        ra = data[base + a * 6: base + a * 6 + 5]
        rb = data[base + b * 6: base + b * 6 + 5]
        data[base + a * 6: base + a * 6 + 5] = rb
        data[base + b * 6: base + b * 6 + 5] = ra
        swapped += 1
    open(dst, 'wb').write(data)
    print(f'{frames} frames, ports {a} and {b} swapped on {swapped} from frame {start}')


if __name__ == '__main__':
    if len(sys.argv) not in (4, 6):
        raise SystemExit(__doc__)
    ports = [int(x) for x in sys.argv[4:6]] or [0, 1]
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), *ports)
