# Preserving MIME defaults editor

This task #31 milestone is a pure Zig editor for the frozen per-user
mimeapps.list contract. It reads no paths, invokes no processes and changes no
desktop defaults by itself. The integration owner must journal the returned
bytes and prior value before publishing any file. Editor acceptance is20%of
#31; artifact publication/configuration/launch/native acceptance remain open.

Input is valid UTF-8, NUL-free, at most1MiB,4096lines and16384bytes per line.
Only the exact case-sensitive key in the single `[Default Applications]` group
is interpreted or edited. Every other line/group is copied byte-for-byte,
including comments, whitespace, CRLF and missing final newline. Duplicate target
keys or duplicate Default Applications groups fail closed without output.
Malformed target values and malformed section headers fail closed. Other keys
and their values are not normalized. MIME arguments use bounded ASCII type/subtype
syntax; default lists contain bounded UTF-8 desktop basenames ending`.desktop`,
separated by semicolons. Empty values are preserved distinctly from absence.

`rewrite` accepts a complete replacement list or null to remove the target key.
It returns owned output bytes and an optional owned exact trimmed prior value;
the supplied allocator must outlive both until idempotent deinit. Removing an
absent key makes no change. Replacing an existing key changes only that line.
Adding inserts at the end of the existing group, adopting its header newline;
if the group is absent, it appends a canonical group and line. A required
separator newline is added only when insertion follows an unterminated line.
No caller input is mutated or retained. Failure frees all partial allocations.

Tests cover multiple unrelated groups, comments, duplicate refusal, CRLF,
unterminated files, exact restoration/removal, MIME/list bounds and syntax,
allocation failure, quotas and Unicode. Real90%source line/taken-arc coverage
and Zig testing allocator gates are required. No C/OS ownership or sanitizer
claim is implied by this allocation-safe pure module.
