# Metadata-bound native coverage recorder

## Scope and cause

The coverage compiler intentionally retains its native ReleaseSafe target. A Zig
0.13.0 Zen 3 compilation of c11ce21 emits exactly 76,512 whole-ICD recorder sites,
reproducing the failed PR job. The corresponding CPU-dependent loop cloning and
vectorization can change raw sites and source keys. Selecting another CPU profile
would discard some emitted native keys, so target substitution is not the repair.

## Ownership and integrity

Every existing raw record keeps its original index, edge arity, source identity,
and hit aggregation. One atomic 32-bit mask replaces 32 atomic byte flags per
site. Relaxed atomic OR preserves concurrent observations of distinct edges.
The generated arity table and hit masks share one statically owned structure;
its actual `sizeof`, including alignment padding, must fit the existing 512KiB
storage budget. No heap allocation is introduced. The 76,512-site example needs
382,560 bytes on the current ABI, including all 76,512 arities.

A private generated header supplies the exact site count, every arity, and a
recorder function name bound to SHA-256 digests of the original LLVM IR and exact
record metadata. A stale runtime/header cannot satisfy another instrumented
object's symbol. Generation verifies contiguous index use without omission,
valid arities 1..32, bounded byte arithmetic, and all generated calls. The runtime
checks every metadata arity before tests, checks each site and its actual arity
before shifting/indexing, and aborts with a diagnostic on malformed input.
Legacy callers keep the existing 16,384-site/32-edge ABI but use atomic masks.

## Measurement policy

Native compilation, MaxEdges=32, 90% gates, fixture boundaries, panic/stack-canary
and line-zero policy, per-source aggregation, and test inputs remain unchanged.
No site is truncated, sampled, merged early, or excluded to fit storage. Exceeding
the budget remains a diagnosed failure. The original IR and records are retained
before validation, with a narrowly whitelisted compiler/target manifest. CI
uploads failed coverage IR, records and manifests without dumping environment
variables or changing job failure status.

## Verification

Focused tests exercise exact generated shape and digest binding, every site above
the former limit, last valid sites/edges, concurrent edge unions, malformed
arities, invalid indices/edges, size overflow, and mismatched link identities.
An independent review precedes commit. The whole unchanged native ICD gate is
rerun on frozen source; the reproduced Zen 3 record list must remain byte-for-byte
unchanged. Runtime, compiler and test limits are recorded in the tracker.

## Generated IR summaries

Site extraction finishes before generated-IR summary cleanup. Function/call-site
`memory(...)`, `speculatable`, `nosync`, `willreturn`, and `nofree` claims no longer
hold after inserting recorder calls: relaxed atomic writes invalidate purity,
and invalid-data diagnostics can synchronize, free libc-owned storage, or abort.
Only attribute groups and suffixes after callee arguments are rewritten. Quoted
strings, debug metadata, parameter contracts, CPU features, ABI, nounwind and
stack protections survive. Regression compilation at `-O2` proves all 16,385
inserted calls still produce their exact persisted edges.

Hit-producing threads must be joined or otherwise quiescent before exit emission;
this is the recorder's existing process-lifetime boundary. Focused compiler and
runner subprocesses have finite timeouts. CI runs the probes both normally and
with AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer enabled.

## Fail-closed sanitizer qualification

Sanitizer-mode fixture builds add `-fno-sanitize-recover=all`. Every probe replaces
its complete ASAN_OPTIONS, LSAN_OPTIONS and UBSAN_OPTIONS strings, instead of
appending to inherited options: leak detection and exit scanning stay enabled,
ASan/UBSan halt and abort, and LSan uses a nonzero leak exit code. No inherited
suppression file or recoverable/zero-exit setting reaches the probe. Ordinary
probe behavior is unchanged. A sanitizer-only signed-overflow negative control
must report the specific UBSan violation, terminate by SIGABRT before its recovery
marker, and must not pass because of the unrelated local LeakSanitizer ptrace
restriction. The normal suite and remaining strict sanitizer probes still apply.
