#!/usr/bin/env python3
"""Independently verify every PNG chunk, checksum, zlib stream, and pixel."""
import pathlib
import struct
import sys
import zlib


def check_one(path: pathlib.Path, dimension: int) -> None:
    blob = path.read_bytes()
    assert blob[:8] == b"\x89PNG\r\n\x1a\n"
    offset = 8
    chunks = []
    while offset < len(blob):
        assert offset + 12 <= len(blob)
        length = struct.unpack_from(">I", blob, offset)[0]
        kind = blob[offset + 4:offset + 8]
        data = blob[offset + 8:offset + 8 + length]
        assert offset + length + 12 <= len(blob)
        crc = struct.unpack_from(">I", blob, offset + 8 + length)[0]
        assert zlib.crc32(kind + data) & 0xFFFFFFFF == crc
        chunks.append((kind, data))
        offset += length + 12
        if kind == b"IEND":
            break
    assert offset == len(blob)
    assert [kind for kind, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"]
    ihdr = chunks[0][1]
    assert ihdr == struct.pack(">IIBBBBB", dimension, dimension, 8, 6, 0, 0, 0)
    zlib_data = chunks[1][1]
    assert zlib_data[:2] == b"\x78\x01"
    compressed = zlib_data[2:-4]
    block_offset = 0
    block_count = 0
    while True:
        assert block_offset + 5 <= len(compressed)
        block_header = compressed[block_offset]
        assert block_header in (0, 1)  # BTYPE=00, byte-aligned, zero padding.
        final_block = block_header == 1
        block_length, inverse_length = struct.unpack_from("<HH", compressed,
                                                          block_offset + 1)
        assert block_length ^ inverse_length == 0xFFFF
        block_offset += 5
        assert block_offset + block_length <= len(compressed)
        block_offset += block_length
        block_count += 1
        if final_block:
            break
    assert block_offset == len(compressed)
    assert block_count == (dimension * (dimension * 4 + 1) + 65534) // 65535
    decoded = zlib.decompress(zlib_data)
    assert struct.unpack(">I", zlib_data[-4:])[0] == zlib.adler32(decoded) & 0xFFFFFFFF
    assert len(decoded) == dimension * (dimension * 4 + 1)
    expected = bytearray()
    for row in range(dimension):
        assert decoded[row * (dimension * 4 + 1)] == 0
        for column in range(dimension):
            alpha = (row + column) % 4
            expected.extend((row * 17 + column * 3 & 0xFF,
                             row * 7 + column * 29 & 0xFF,
                             row * 13 + column * 11 & 0xFF,
                             (0, 85, 170, 255)[alpha]))
    actual = b"".join(decoded[row * (dimension * 4 + 1) + 1:
                            (row + 1) * (dimension * 4 + 1)]
                      for row in range(dimension))
    assert actual == bytes(expected)


root = pathlib.Path(sys.argv[1])
for size in (16, 24, 32, 48, 64, 128, 256):
    check_one(root / f"icon-{size}.png", size)
print("independent PNG chunk, CRC, zlib, and pixel checks passed")
