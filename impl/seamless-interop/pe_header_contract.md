# Bounded PE classification contract

The task #31 classifier milestone implements IMPL_DESC §3.9.2. It is a routing
hint, not executable validation or permission to launch. The parser borrows only
the first 4096 bytes, allocates nothing and retains no pointers. Any malformed,
truncated, unknown or non-PE input returns GUI; only a bounded PE32/PE32+ header
with Subsystem3 returns Console. Subsystem2 returns GUI. Read errors are kept
separate by the future file/launch caller and must result in exit126.

All integer reads are unaligned little-endian slice reads. The DOS header must
contain MZ and the u32 at0x3c. e_lfanew must lie within0x40..4072. The PE signature
and20-byte COFF header must fit. SizeOfOptionalHeader must be at least70 bytes,
and the required70 optional-header bytes must fit both the supplied prefix and
the4096-byte ceiling. OptionalHeader.Magic must be0x10b or0x20b. Subsystem is the
u16 at optional offset68 for either format. Bytes after those required fields
are not validated, including section tables, loader validity or signatures.

The C ABI rejects null/nonempty, overflowing, unaligned output and overlapping
input/output ranges before constructing slices or publishing output. An empty
input may be NULL. The length must not exceed4096: callers must provide only
their bounded prefix, not an unbounded file. Valid API calls publish GUI for
malformed contents; invalid API calls leave output unchanged. Borrowed memory
must remain readable/stable or exclusively writable for the call; numerical
range checks cannot prove accessibility. Independent calls are thread-safe.

Tests include independent PE32/PE32+ byte fixtures, exact required-field and4096
boundaries, every truncation, malformed signatures/offsets/sizes/magic/subsystem,
unaligned input, C ABI alias/range/output preservation and deterministic random
input. Gates require90% production lines and taken arcs, bounds-checked Zig
tests/testing allocator and an ASan/LSan/UBSan C ABI consumer. Classification
alone accepts20% of #31; MIME preserving editor20%, journaled handler artifacts20%,
configuration/route integration20% and integrated/native qualification20% remain.

Field interpretation follows Microsoft's [PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
