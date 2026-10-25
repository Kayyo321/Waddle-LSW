# Keyboard layout sync reducer (TODO #15 layout sync)

`src/interop/keyboard_layouts.zig` with C header `src/interop/keyboard_layouts.h`
implements the host-side layout mapping and send-on-change decision of IMPL_DESC
3.6.3 "Layout sync". It owns no keymap parser, no libxkbcommon handle, no wire
encoder and no `LoadKeyboardLayoutW` call. The host event loop passes the XKB
layout and variant names it already obtained. The guest side gets only the KLID
validator here.

The module name and file name follow IMPL_DESC 3.6.3 (`keyboard_layouts`).

## Protocol facts used

- Message `KeyboardLayout` is `0x0211`, host to guest, one required field
  `layout` (tag `0x0020`, string, critical). The value is a Windows KLID: eight
  hexadecimal digits (`include/waddle/interop_protocol.h`, `InteropTagLayout`).
- The codec accepts either case. This module accepts either case and emits
  uppercase.
- `KeyboardLayout` carries no serial and does not consume the host serial counter
  (IMPL_DESC 3.6.2).
- IME-based locales (Japanese, Korean, Chinese, Vietnamese) need the KLID so that
  the guest selects the matching IME (IMPL_DESC 3.10.5).

## Public API (Zig)

| Symbol | Kind | Meaning |
|---|---|---|
| `MaxNameBytes` | const usize | 64; maximum bytes of a layout or variant name |
| `KlidBytes` | const usize | 8 |
| `error_t` | error set | `InvalidName`, `InvalidKlid` |
| `klid_t` | extern struct | `bytes: [8]u8`, uppercase hex |
| `entry_t` | struct | one row: `layout`, `variant` (borrowed), `klid: [8]u8` |
| `KeyboardLayoutTable` | const [82]entry_t | fixed mapping table (below) |
| `outcome_kind_t` | enum(u8) | `Send`=0, `Unchanged`=1, `NoMapping`=2 |
| `outcome_t` | struct | `kind`, `klid` (zero for NoMapping) |
| `state_t` | extern struct | `has_applied: bool`, `applied: klid_t`. Zero = initial |
| `status_t` | enum(c_int) | `Ok`=0, `NullArgument`=1, `InvalidName`=2, `InvalidKlid`=3 |
| `outcome_abi_t` | extern struct | `kind: u8`, `klid: klid_t` |
| `validate_name`, `validate_variant`, `validate_klid`, `lookup`, `observe`, `reset` | fn | see Behaviour |

C ABI: `waddle_keyboard_layouts_validate_klid`, `waddle_keyboard_layouts_observe`,
`waddle_keyboard_layouts_reset`. Name arguments are `(ptr, len)` byte ranges. A
NULL pointer with length 0 means the empty string. A NULL pointer with a nonzero
length returns `NullArgument`. Output is written only on `Ok`. `state_t` is 9
bytes, `outcome_abi_t` is 9 bytes, `klid_t` is 8 bytes. The Zig test
`ABI struct sizes match the C header layouts` and the C consumer's
`_Static_assert` pin them.

## Name rules

- Layout: 1 through 64 bytes from `[a-z0-9_]`. Anything else, including uppercase,
  is `InvalidName`.
- Variant: 0 through 64 bytes from `[A-Za-z0-9_-]`. Empty is the layout default.
  Uppercase is accepted, so names such as `T3` or `phonetic_YAZHERTY` are
  well-formed and reach the table, where they are unmapped (`NoMapping`). Other
  bytes (space, dot, and so on) are `InvalidName`.
- Names are borrowed for the call only.
- The ABI never reads more than 65 bytes of a name, so a longer declared length
  is rejected before the pointer is read past its length.

## KLID rules

`validate_klid` accepts exactly eight bytes from `[0-9A-Fa-f]` and rejects the
all-zero value. Output is the uppercase form. This validates the guest-received
`layout` field (tag `0x0020`).

## Mapping table

Each row maps one XKB (layout, variant) pair to a Windows KLID and names the
Windows keyboard. The source column says where the KLID was confirmed:

- **page**: the keyboard name and KLID appear together in Microsoft's "Default
  input profiles (input locales) in Windows" page (learn.microsoft.com,
  `default-input-locales-for-windows-language-packs`, revision 2024-09-02). The
  page was read in full and every pair was extracted from it.
- **lead**: a value the lead specified. It does not appear on the page. It is a
  standard Windows KLID, but this table could not confirm it from the page.

| xkb layout | variant | KLID | Windows keyboard name | source |
|---|---|---|---|---|
| us | (default) | 00000409 | US | page |
| us | intl | 00020409 | United States-International | page |
| us | altgr-intl | 00020409 | United States-International | page (variant mapped to same KLID) |
| us | dvorak | 00010409 | United States-Dvorak | lead (not a default profile, so not on page) |
| gb | (default) | 00000809 | United Kingdom | page |
| gb | extd | 00000452 | United Kingdom Extended | page |
| ie | (default) | 00001809 | Irish | page |
| au | (default) | 00000409 | US (en-AU default profile) | page |
| nz | (default) | 00001409 | NZ Aotearoa | page |
| ca | (default) | 00001009 | Canadian French | page |
| latam | (default) | 0000080A | Latin American | page |
| be | (default) | 0000080C | Belgian French | lead (page confirms the KLID for Belgian French) |
| de | (default) | 00000407 | German | page |
| de | nodeadkeys | 00000407 | German (no dead keys: same KLID) | page (variant mapped to same KLID) |
| at | (default) | 00000407 | German (de-AT default profile) | page |
| ch | (default) | 00000807 | Swiss German | page |
| ch | fr | 0000100C | Swiss French | page |
| fr | (default) | 0000040C | French | page |
| es | (default) | 0000040A | Spanish | page |
| it | (default) | 00000410 | Italian | page |
| pt | (default) | 00000816 | Portuguese | page |
| br | (default) | 00010416 | Portuguese (Brazil ABNT2) | lead (xkb `br` is ABNT2; page lists ABNT as 00000416) |
| se | (default) | 0000041D | Swedish | page |
| se | smi | 0000083B | Swedish with Sami | page |
| no | (default) | 00000414 | Norwegian | page |
| no | smi | 0000043B | Norwegian with Sami | page |
| dk | (default) | 00000406 | Danish | page |
| fi | (default) | 0000040B | Finnish | page |
| fi | smi | 0001083B | Finnish with Sami | page |
| is | (default) | 0000040F | Icelandic | page |
| pl | (default) | 00000415 | Polish (Programmers) | page |
| cz | (default) | 00000405 | Czech | page |
| sk | (default) | 0000041B | Slovak | page |
| hu | (default) | 0000040E | Hungarian | page |
| ro | std | 00010418 | Romanian (Standard) | page |
| hr | (default) | 0000041A | Standard (Croatian) | page |
| si | (default) | 00000424 | Slovenian | page |
| rs | (default) | 0000081A | Serbian (Latin) | page |
| ee | (default) | 00000425 | Estonian | page |
| lv | (default) | 00020426 | Latvian (Standard) | page |
| lt | (default) | 00010427 | Lithuanian | page |
| ru | (default) | 00000419 | Russian | page |
| ru | tt | 00010444 | Tatar | page |
| ua | (default) | 00020422 | Ukrainian (Enhanced) | page |
| by | (default) | 00000423 | Belarusian | page |
| bg | (default) | 00030402 | Bulgarian | page |
| mk | (default) | 0001042F | Macedonian - Standard | page |
| gr | (default) | 00000408 | Greek | page |
| tr | (default) | 0000041F | Turkish Q | page |
| al | (default) | 0000041C | Albanian | page |
| mt | (default) | 0000043A | Maltese 47-Key | page |
| ge | (default) | 00010437 | Georgian (QWERTY) | page |
| am | (default) | 0002042B | Armenian Phonetic | page |
| az | (default) | 0000042C | Azerbaijani Latin | page |
| az | cyrillic | 0000082C | Azerbaijani Cyrillic | page |
| kz | (default) | 0000043F | Kazakh | page |
| mn | (default) | 00000450 | Mongolian Cyrillic | page |
| tj | (default) | 00000428 | Tajik | page |
| tm | (default) | 00000442 | Turkmen | page |
| kg | (default) | 00000440 | Kyrgyz Cyrillic | page |
| af | (default) | 00000463 | Pashto (Afghanistan) | page |
| pk | (default) | 00000420 | Urdu | page |
| ir | (default) | 00000429 | Persian | page |
| il | (default) | 0002040D | Hebrew (Standard) | page |
| ara | (default) | 00000401 | Arabic (101) | page |
| ara | azerty | 00020401 | Arabic (102) AZERTY | page |
| eg | (default) | 00000401 | Arabic (101) | page |
| sy | (default) | 0000045A | Syriac | page |
| np | (default) | 00000461 | Nepali | page |
| lk | (default) | 0000045B | Sinhala | page |
| in | (default) | 00000439 | Devanagari - INSCRIPT | page |
| th | (default) | 0000041E | Thai Kedmanee | page |
| vn | (default) | 0000042A | Vietnamese | page |
| jp | (default) | 00000411 | Japanese | lead (IME-based; page gives the IME, not a keyboard KLID) |
| kr | (default) | 00000412 | Korean | lead (IME-based; same reason) |
| cn | (default) | 00000804 | Chinese (Simplified) | lead (IME-based; same reason) |
| tw | (default) | 00000404 | Chinese (Traditional) | lead (IME-based; same reason) |
| cn | ug | 00010480 | Uyghur | page |
| mao | (default) | 00000481 | Maori | page |
| kh | (default) | 00000453 | Khmer | page |
| la | (default) | 00000454 | Lao | page |
| bt | (default) | 00000C51 | Dzongkha | page |

Count: 82 rows, meeting the spec's ≥ 80 requirement (`KeyboardLayoutTable.len >= 80`
is asserted by the Zig test). 75 rows are **page**-confirmed and 7 are **lead**
(see below).

Rows not on the page are: `us dvorak`, `br`, `be`, `jp`, `kr`, `cn`, `tw`. Each is
a standard Windows KLID that the lead specified, and each is marked **lead** above.
They are not confirmed by the page, so the lead should check them before merge.

## Excluded layouts (not mapped, so NoMapping)

Each of these was specified or considered but is left out because its KLID could not
be confirmed. Any of them can be added once confirmed.

- `nl` (Dutch): the lead gave `00000413`, but the page never lists that KLID. The
  page gives nl-NL the United States-International profile, which is not the Dutch
  physical layout.
- `ca multix` (Canadian Multilingual Standard, lead gave `00011009`): not on the page.
- `ca fr-legacy` (Canadian French (Legacy), lead gave `00000C0C`): not on the page.
  The page gives Canadian French as `00001009`, which is the row used above.
- `ro` default (Romanian Programmers, lead gave `00020418`): not on the page.
  `ro std` (`00010418`) is on the page and is the only Romanian row.
- `tr f` (Turkish F, lead gave `0001041F`): not on the page.
- `lv` default with `00010426`: not on the page. The page gives `00020426`, which is
  the row used.
- `us colemak`: no Windows KLID. NoMapping, as the lead specified.
- `de T3`, `ru phonetic_YAZHERTY`: the variants are uppercase or unknown, so NoMapping.
- `gb dvorak`, `ie CloGaelach`, `in ben`, `in guj`, `in tam`, `in kan` and other
  Indic variants: the xkb variant does not map to one Windows keyboard with
  confidence.
- `uz` (Uzbek Latin): the lead gave `00000443`, which is not on the page. The page
  lists `Uzbek Cyrillic` as `00000843`, which is a different script.
- `gr polytonic` (`00010408`), `fo` Faroese (`00000438`), `id` Indonesian
  (`00000421`): not on the page.
- `bg` typewriter (`00000402`): not on the page. The default Bulgarian row uses
  `00030402`, which is on the page.
- `mk` as the lead wrote it (`0000042F`): not a KLID on the page. The page's
  Macedonian keyboard is `0001042F`, used above.
- `ge` as the lead wrote it (`00000437`): not on the page. The page's Georgian
  keyboard is `00010437`, used above.
- `am` as the lead wrote it (`0000042B`): not on the page. The page's Armenian
  Phonetic keyboard is `0002042B`, used above.

## Behaviour

`lookup(layout, variant)` validates both names (`InvalidName` on failure), then
returns the row whose `layout` and `variant` both match exactly. A pair with no row
returns `null`. No fallback is applied: a variant without a row is never mapped to
its layout default. Uppercase variants never match a row.

`observe(state, layout, variant)` is send-on-change:

1. Look up the pair. Invalid names return `InvalidName` with no state change.
2. No row: return `NoMapping` with zero KLID and no state change. The last sent KLID
   is kept, so returning to it later is `Unchanged`, not a resend.
3. Row found and `has_applied` with the same KLID: return `Unchanged`, no change.
4. Otherwise store the KLID as applied, set `has_applied`, and return `Send`.

There is no time-based debounce. Duplicate suppression is equality with the last
value sent on this connection. The first mapped observation after `reset` is always
`Send`. The caller calls `reset` on disconnect and on any new connection, so
`KeyboardLayout` is sent on connect (IMPL_DESC 3.6.3).

## Ownership, lifetime and threading

- `state_t` is caller storage of 9 bytes. Zero-filled storage is valid. There is no
  init or free function, since nothing is allocated.
- Names and text are borrowed only for the call. Outputs are written only on `Ok`.
- `state_t` belongs to one host event-loop thread. `lookup`, `validate_name`,
  `validate_variant` and `validate_klid` are pure and callable from any thread.
- `KeyboardLayoutTable` is read-only and has static storage.
- No function allocates, logs or performs I/O.

## Error codes

| Status | Zig error | Trigger | State change |
|---|---|---|---|
| `NullArgument` | n/a | NULL state, output, or non-empty name pointer | none |
| `InvalidName` | `error.InvalidName` | empty or over-long layout, over-long variant, byte outside the charset | none |
| `InvalidKlid` | `error.InvalidKlid` | text not eight hex digits, or all zero | none |

`NoMapping` is a successful outcome. The ABI returns `Ok` with
`kind = WaddleKeyboardLayoutsNoMapping`.

## Verification

Zig built-in tests in `keyboard_layouts.zig` (run by `make interop-keyboard-lanes-test`):

- Layout charset: boundaries (1 and 64 bytes accepted, 0 and 65 rejected),
  uppercase, dash, space, dot, NUL and 0xFF rejected.
- Variant charset: empty and 64 dashes accepted, 65 rejected, uppercase accepted
  (`Dvorak`, `T3`, `phonetic_YAZHERTY`), dot and space rejected.
- KLID text: canonical uppercase output, lowercase input, 7, 9 and 0 bytes, non-hex
  byte, space, all-zero value.
- Table invariants: at least 80 rows, each row validates as name pair and KLID,
  and every pair is unique.
- Lookup: exact matches (including `us dvorak`, `de nodeadkeys`, `br`), unmapped
  pairs and uppercase variants returning null, `us colemak` returning null, invalid
  layout names as errors.
- Observe: first Send, Unchanged on repeat, Send on change, NoMapping keeps the last
  sent value, invalid names leave state unchanged, uppercase variants give NoMapping,
  reset forces Send.
- C ABI: every NULL position, every status, name lengths 0, 1, 64 and 65, and
  outputs unchanged on error.
- Randomized 20000-step sequence with valid, unmapped, uppercase and invalid names,
  and resets, checked against a reference lookup that builds `layout/variant` keys.
- ABI struct sizes match the C header.

`keyboard_layouts_abi.c` is an independent C consumer built with
`-fsanitize=address,leak,undefined` and run with `detect_leaks=1`
(`make interop-keyboard-lanes-sanitizers`).

Coverage (`python3 tests/av/coverage.py keyboard_layouts`), measured at this commit:
line 100.00% (59/59), branch 96.30% (52/54). Three compiler panic guards are excluded
by the tool.

Zig sanitizer runs are not claimed. The Zig suite uses no allocator, so
`std.testing.allocator` and `FailingAllocator` sweeps do not apply.

## Deviations and open items

- The lead's unconfirmed rows (`us dvorak`, `br`, `be`, `jp`, `kr`, `cn`, `tw`) are
  in the table, marked **lead** above, for the lead to confirm.
- The excluded-layouts list above is the complete set of lead-specified values that
  were not included.
- TODO #15 layout sync remains partial. The guest consumer (`LoadKeyboardLayoutW`,
  `WM_INPUTLANGCHANGEREQUEST`) is outside this module.
