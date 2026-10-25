# Portable icon PNG encoder qualification

Root reviewed and independently qualified exact source:159/159 production lines
and74/74 taken arcs (100% each), no source exclusions. All seven frozen icon
sizes round-trip through an independent Python PNG/chunk/CRC/zlib/stored-block/
Adler32/pixel validator. The same real encoder and fixtures pass ASan/LSan/UBSan
with detect_leaks=1. Exact hashes, commands, raw gcov and sanitizer log retained.

Root added full declared output-capacity range/alias validation, size-query
output overflow rejection and independent invalid-pointer/unused-tail alias
regressions before acceptance. The encoder allocates nothing, validates all
inputs before writes and preserves outputs on error. Caller pixels must be
straight-alpha RGBA; native Shell extraction and premultiplication conversion
are separate and unqualified here. Accept PNG encoder10% of TODO #27 only,
adding0.40% overall (total22.40%). No icon extraction, catalog or desktop artifact
progress is implied by a valid generated PNG.
