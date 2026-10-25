# Keyboard layout sync reducer (TODO #15 layout sync)

`src/interop/keyboard_layout.zig` with C header `src/interop/keyboard_layout.h`
implements the host-side layout mapping and send-on-change decision of IMPL_DESC
3.6.3 "Layout sync". It owns no keymap parser, no libxkbcommon handle, no wire
encoder and no `LoadKeyboardLayoutW` call. The host event loop passes it the
XKB layout and variant names it already obtained. The guest side gets only the
KLID validator here; loading the layout is a guest API concern.

File name: IMPL_DESC 3.6.3 names `src/interop/keyboard_layouts.zig`. This module
is named `keyboard_layout.zig` as assigned for this lane. The lead must reconcile
the two names in IMPL_DESC or rename the file before merge.

## Protocol facts used

- Message `KeyboardLayout` is `0x0211`, host to guest, one required field
  `layout` (tag `0x0020`, string, critical). The value is a Windows KLID, eight
  hexadecimal digits (`include/waddle/interop_protocol.h`, `InteropTagLayout`).
- The codec accepts the eight digits in either case (`interop_codec.zig`,
  `hex_valid`). This module accepts either case and emits uppercase.
- `KeyboardLayout` carries no serial and does not consume the host serial
  counter (IMPL_DESC 3.6.2). The reducer therefore uses no serial.

## Public API (Zig)

| Symbol | Kind | Meaning |
|---|---|---|
| `MaxNameBytes` | const usize | 64; maximum bytes of a layout or variant name |
| `KlidBytes` | const usize | 8 |
| `error_t` | error set | `InvalidName`, `InvalidKlid` |
| `klid_t` | extern struct | `bytes: [8]u8`, uppercase hex |
| `entry_t` | struct | one table row: `layout`, `variant` (borrowed strings), `klid: [8]u8` |
| `KeyboardLayoutTable` | const [N]entry_t | fixed mapping table (below) |
| `outcome_kind_t` | enum(u8) | `Send`=0, `Unchanged`=1, `NoMapping`=2 |
| `outcome_t` | struct | `kind`, `klid` (zero for NoMapping) |
| `state_t` | extern struct | `has_applied: bool`, `applied: klid_t`. Zero = initial |
| `status_t` | enum(c_int) | `Ok`=0, `NullArgument`=1, `InvalidName`=2, `InvalidKlid`=3 |
| `outcome_abi_t` | extern struct | `kind: u8`, `klid: klid_t` |
| `validate_name(name)` | fn | XKB layout syntax |
| `validate_variant(variant)` | fn | XKB variant syntax; empty allowed |
| `validate_klid(text)` | fn | eight hex digits to canonical `klid_t` |
| `lookup(layout, variant)` | fn | exact table match, `?klid_t` (null = NoMapping) |
| `observe(state, layout, variant)` | fn | send-on-change decision |
| `reset(state)` | fn | forget the last sent KLID |

C ABI: `waddle_keyboard_layout_validate_klid`, `waddle_keyboard_layout_observe`,
`waddle_keyboard_layout_reset`. Name arguments are `(ptr, len)` byte ranges.
A NULL pointer with length 0 means the empty string; a NULL pointer with a
nonzero length returns `NullArgument`. Output is written only on `Ok`. The
header mirrors the structs: `state_t` is 9 bytes, `outcome_abi_t` is 9 bytes,
`klid_t` is 8 bytes. The Zig test `ABI struct sizes match the C header layouts`
and the C consumer `_Static_assert` pin them.

## Name rules

- Layout: 1 through 64 bytes from `[a-z0-9_]`. Empty, longer, uppercase, or any
  other byte is `InvalidName`.
- Variant: 0 through 64 bytes from `[a-z0-9_-]`. Empty is the layout default.
- Names are borrowed for the call only and never retained.
- The bounded-read rule: the ABI never reads more than 65 bytes of a name, so a
  length over 64 is rejected before the pointer is dereferenced past its
  declared length.

## KLID rules

`validate_klid` accepts exactly eight bytes from `[0-9A-Fa-f]` and rejects the
all-zero value `00000000`, which names no layout. Output is the uppercase form.
This validates the guest-received `layout` field (tag `0x0020`) before
`LoadKeyboardLayoutW` is considered.

## Mapping table and its source

Each row maps one XKB (layout, variant) pair to a KLID. Every KLID below was
checked against the Microsoft page "Default input profiles (input locales) in
Windows" (learn.microsoft.com, `default-input-locales-for-windows-language-packs`,
revision 2024-09-02), in the row for a locale whose physical layout equals the
xkb layout.

| xkb layout | variant | KLID | Keyboard name (page) |
|---|---|---|---|
| us | (default) | 00000409 | US |
| us | intl | 00020409 | United States-International |
| gb | (default) | 00000809 | United Kingdom |
| ie | (default) | 00001809 | Irish |
| au | (default) | 00000409 | US (en-AU default profile) |
| ara | (default) | 00000401 | Arabic (101) |
| de | (default) | 00000407 | German |
| ch | (default) | 00000807 | Swiss German (de-CH) |
| at | (default) | 00000407 | German (de-AT default profile) |
| fr | (default) | 0000040C | French |
| es | (default) | 0000040A | Spanish |
| it | (default) | 00000410 | Italian |
| pt | (default) | 00000816 | Portuguese (pt-PT) |
| br | (default) | 00000416 | Portuguese (Brazil ABNT) |
| se | (default) | 0000041D | Swedish |
| no | (default) | 00000414 | Norwegian |
| dk | (default) | 00000406 | Danish |
| fi | (default) | 0000040B | Finnish |
| is | (default) | 0000040F | Icelandic |
| pl | (default) | 00000415 | Polish (Programmers) |
| cz | (default) | 00000405 | Czech |
| sk | (default) | 0000041B | Slovak |
| hu | (default) | 0000040E | Hungarian |
| ro | (default) | 00010418 | Romanian (Standard) |
| hr | (default) | 0000041A | Croatian (Standard) |
| si | (default) | 00000424 | Slovenian |
| ee | (default) | 00000425 | Estonian |
| lv | (default) | 00020426 | Latvian (Standard) |
| ru | (default) | 00000419 | Russian |
| ua | (default) | 00020422 | Ukrainian (Enhanced) |
| by | (default) | 00000423 | Belarusian |
| gr | (default) | 00000408 | Greek |
| tr | (default) | 0000041F | Turkish Q |
| il | (default) | 0002040D | Hebrew (Standard) |
| th | (default) | 0000041E | Thai Kedmanee |

Rows deliberately absent, with reason:

- `us dvorak`: no Dvorak KLID appears on the verified page.
- `de nodeadkeys`: the page lists only the standard German KLID; a separate
  variant KLID is not verified.
- `be`: the page gives two KLIDs for the Belgian keyboard depending on language
  (fr-BE 0000080C, nl-BE 00000813). The xkb layout does not select a language,
  so no single row is correct.
- `nl`: the page gives nl-NL the United States-International KLID. That
  Windows default does not match the Dutch physical layout, so mapping xkb `nl`
  to it would be wrong.
- `jp`, `kr`, `cn`, `tw`, `vn`: the page gives these locales an IME profile
  (for example `Microsoft IME`, `Microsoft Pinyin`), not a keyboard KLID that the
  page confirms. No KLID is recorded.

Consequence: any layout or variant absent from the table is `NoMapping`, the
guest layout is left unchanged, and nothing is sent.

## Coverage gap versus IMPL_DESC

IMPL_DESC 3.6.3 requires "≥ 80 layouts". The table has 36 rows. The spec gives no
rows, so the missing ones cannot be verified from the spec. The table is
therefore not complete against that requirement. Closing the gap needs the
remaining layouts, verified the same way, or a decision from the lead that the
requirement is met by the verified subset. Until then TODO #15 layout sync is
partial.

## Behaviour

`lookup(layout, variant)` validates both names (`InvalidName` on failure), then
returns the row whose `layout` and `variant` both match exactly. A pair with no
row returns `null`. No fallback is applied: a variant without a row is never
mapped to its layout default.

`observe(state, layout, variant)` is send-on-change:

1. Look up the pair. Invalid names return `InvalidName` with no state change.
2. No row: return `NoMapping` with zero KLID and no state change. The last sent
   KLID is kept, so returning to it later is `Unchanged`, not a resend.
3. Row found and `has_applied` with the same KLID: return `Unchanged`, no change.
4. Otherwise store the KLID as applied, set `has_applied`, and return `Send`.

There is no time-based debounce. Duplicate suppression is equality with the last
value sent on this connection. The first mapped observation after `reset` is
always `Send`. The caller calls `reset` on disconnect and on any new
connection, so `KeyboardLayout` is sent on connect (IMPL_DESC 3.6.3).

## Ownership, lifetime and threading

- `state_t` is caller storage of 9 bytes. Zero-filled storage is valid. No init
  or free function exists, since nothing is allocated.
- Names and text are borrowed only for the call. Outputs are written only on
  `Ok`.
- `state_t` belongs to one host event-loop thread. `lookup`, `validate_name`,
  `validate_variant` and `validate_klid` are pure and callable from any thread.
- `KeyboardLayoutTable` is read-only and has static storage.
- No function allocates, logs or performs I/O.

## Error codes

| Status | Zig error | Trigger | State change |
|---|---|---|---|
| `NullArgument` | n/a | NULL state, output, or non-empty name pointer | none |
| `InvalidName` | `error.InvalidName` | empty layout, name over 64 bytes, byte outside charset | none |
| `InvalidKlid` | `error.InvalidKlid` | text not eight hex digits, or all zero | none |

`NoMapping` is a successful outcome, not an error. The ABI returns `Ok` with
`kind = WaddleKeyboardLayoutNoMapping`.

## Verification

Zig built-in tests in `keyboard_layout.zig` (run by `make interop-keyboard-lanes-test`):

- Layout name charset: boundaries (1 and 64 bytes accepted, 0 and 65 rejected),
  uppercase, dash, space, dot, NUL, 0xFF rejected.
- Variant charset: empty and 64 dashes accepted, 65 rejected, uppercase, dot,
  space rejected.
- KLID text: canonical uppercase output, lowercase input, 7, 9 and 0 bytes,
  non-hex byte, space, all-zero value.
- Table invariants: every row validates as name pair and KLID, and every pair is
  unique.
- Lookup: exact matches, unmapped pairs returning null, invalid names as errors.
- Observe: first Send, Unchanged on repeat, Send on change, NoMapping keeps the
  last sent value, invalid names leave state unchanged, reset forces Send.
- C ABI: every NULL position, every status, name length 0, 1, 64 and 65, and
  outputs unchanged on error.
- Randomized 20000-step sequence with mixed valid, unmapped and invalid names,
  and resets, checked against a reference lookup that builds `layout/variant`
  keys and compares them directly.
- ABI struct sizes match the C header.

`keyboard_layout_abi.c` is an independent C consumer. It is built with
`-fsanitize=address,leak,undefined` and run with `detect_leaks=1`
(`make interop-keyboard-lanes-sanitizers`).

Coverage (`python3 tests/av/coverage.py keyboard_layout`): line 100.00% (58/58),
branch 96.15% (50/52), measured at this commit.

Zig sanitizer runs are not claimed. The Zig suite uses no allocator, so
`std.testing.allocator` and `FailingAllocator` sweeps do not apply.

## Deviations and open items

- File name: `keyboard_layout.zig` instead of IMPL_DESC's `keyboard_layouts.zig`
  (see top of this document).
- Table completeness: 36 verified rows against the spec's ≥ 80 (see above).
- The table is built from the Windows default input profile for each locale.
  Where that profile is an IME or not a physical match, the row is omitted rather
  than approximated.
- The guest consumer (`LoadKeyboardLayoutW`, `WM_INPUTLANGCHANGEREQUEST`) is out of
  scope for this module; it needs its own owner.
