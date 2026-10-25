# Portable RGBA8 PNG writer contract

## Scope and ownership

`src/interop/png_writer.c` provides a deterministic size query and a caller-buffer PNG encoder for square straight-alpha RGBA8 images with dimensions 16, 24, 32, 48, 64, 128, or 256. It is portable C with no allocation, external library, retained pointer, global state, or input mutation. Icon acquisition and catalog storage are separate components.

The caller owns and stabilizes the borrowed source pixels and output buffer for the full call. The encoder writes only into caller-provided output storage after validating the complete input/output extents and exact capacity. The caller owns the encoded bytes after success. No allocation needs disposal.

## Public interface and validation

`png_writer_encoded_size(dimension, size_out)` accepts one supported dimension and a non-NULL, naturally aligned, writable `size_t` output disjoint from the input arguments. It writes the exact length only on success.

`png_writer_encode_rgba8(rgba, stride, dimension, output, output_capacity, written_out)` accepts non-NULL, naturally aligned source/output/length pointers. `stride` must be at least `dimension * 4`. The readable source extent is `(dimension - 1) * stride + dimension * 4`; all multiplication/addition and pointer-end calculations are checked before the first read/write. The complete declared output_capacity extent must have a nonoverflowing pointer range. Source and full output extents must not overlap, and `written_out` must not overlap either buffer. `output_capacity` must be at least the exact result from the size query. On every failure, output bytes and `written_out` remain unchanged. Statuses distinguish invalid arguments, unsupported dimensions, overflow, and insufficient capacity.

## PNG wire format

The complete output is the eight-byte PNG signature, one 13-byte IHDR, one IDAT, and an empty IEND. IHDR contains big-endian width/height, bit depth 8, color type 6 (RGBA), compression method 0, filter method 0, and interlace method 0. Each top-down source row is emitted as a scanline prefixed with filter byte 0. Pixels are copied as straight-alpha R,G,B,A bytes without channel swapping or premultiplication.

IDAT contains a zlib stream with header `0x78 0x01`, one or more RFC 1951 stored-deflate blocks of at most 65,535 bytes, and a big-endian Adler-32 over the complete filtered scanline stream. Each block has correct BFINAL, LEN, and one's-complement NLEN fields. Adler-32 uses modulus 65,521. Each PNG chunk contains a big-endian data length, four-byte type, data, and CRC-32 over type plus data using polynomial `0xedb88320`, initial/final XOR `0xffffffff`.

For dimension `n`, filtered bytes are `n * (4*n + 1)`. The stored-deflate block count is the ceiling of filtered bytes divided by 65,535. Zlib stream size is `2 + filtered_bytes + 5 * block_count + 4`; total PNG size is 57 plus that zlib size. Frozen maximum dimensions bound these calculations below `SIZE_MAX`; caller-provided stride extents still use checked arithmetic.

## Errors, concurrency, and verification

There is no shared mutable state; independent calls are thread-safe. Simultaneous access to any aliased caller memory remains the caller's responsibility. No partial output is reported as success.

Tests cover all frozen dimensions, exact size arithmetic, capacity and stride boundaries, pointer overlap and overflow rejection, output-unchanged guarantees, and channel/alpha fidelity. `png_check.py` independently parses every output chunk, checks CRCs, decompresses the zlib stream, verifies stored blocks and Adler-32, and compares every decoded pixel. The exact production source must reach at least 90% line and taken-arc coverage. C tests run under ASan, LSan, and UBSan with leak detection enabled.
