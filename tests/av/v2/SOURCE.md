# Frozen V2 compatibility parser

Source: commit `ab98759`, `include/waddle/av_protocol.h` and `src/av/av_codec.zig`. The only adaptations are the local header include and `v2_` exported/test-call symbol prefixes. Parsing, validation, and the old logical structure are unchanged. These files are test-only and never linked into runtime binaries. SHA256SUMS pins this fixture.
