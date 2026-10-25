# Keyboard translation milestone contract

This bounded milestone implements the frozen TODO #15 evdev translation table in `src/av/av_input.c`. It does not implement a new transport or native Windows key-injection policy.

## Translation model

`av_input_translate_key()` accepts an unsigned Linux evdev value and returns a plain by-value `av_key_translation_t`. It is pure, allocation-free, thread-safe, and owns no memory. The input domain is the frozen evdev range 1..767. All fields not selected by `kind` are zero.

The result kind is one of:

- `AvKeyTranslationScanCode`: `scan_code` is one Set-1 byte; `extended` is 1 only for the E0 prefix.
- `AvKeyTranslationVirtualKey`: `virtual_key` is the Windows VK value. Microsoft Win32 defines `VK_HANGUL=0x15` and `VK_HANJA=0x19` ([Virtual-Key Codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)); these are checked independently of the CSV in the C regression tests.
- `AvKeyTranslationPause`: `pause_make` is `E1 1D 45`; `pause_break` is `E1 9D C5`. The result deliberately remains typed and is not flattened into a scan byte.
- `AvKeyTranslationSuppressed`: KEY_POWER and KEY_SLEEP are represented but unauthorized for injection by default.
- `AvKeyTranslationUnsupported`: no mapping exists. Values outside 1..767 always return this kind.

The sorted immutable table covers the existing Set-1-compatible evdev codes 1..83 and every additional key listed in frozen IMPL_DESC §3.6.3, including ISO/JIS keys, E0 navigation and media keys, F13..F24, Pause, HANGEUL/HANJA, and suppressed POWER/SLEEP. `tests/interop/scancodes.csv` is the checked-in golden table. The test exhaustively compares every code 1..767 against it, verifies unsupported gaps, and compares inverse equivalence classes so intentional many-to-one mappings are explicit.

`av_input_scan_code()` retains its existing C ABI and packed return convention: low byte is the scan byte and bit 8 indicates E0. Its admission domain remains the legacy AV range 1..127. It returns zero for non-scan kinds, unsupported values, and any key outside that legacy range. Pause, virtual-key and suppressed results can never be mistaken for ordinary scan codes by this compatibility API.

The original IMPL_DESC §3.6.3 labels HANGEUL/HANJA as VK values `0xF2`/`0xF1`. Root's versioned amendment §8.37 corrects them to Microsoft's documented `VK_HANGUL=0x15` and `VK_HANJA=0x19`; source and independent regression assertions use these values.

The admitted input state machine continues to use this legacy scan-only API. It explicitly rejects key indexes outside 1..127 and button indexes outside 1..3 before held-state access. The wire codec's 1..127 key bound and the 128-byte held-key registry remain unchanged.

## Integration boundary

This milestone does not change `av_wayland.c`, `av_windows.c`, or any wire schema. The existing legacy Wayland path filters events whose legacy `av_input_scan_code()` result is zero; therefore Pause, HANGEUL/HANJA and suppressed POWER/SLEEP are currently not forwarded through that path. The generic typed table is ready for an authorized native injection boundary, but no runtime delivery or Windows injection behavior is claimed here. In particular, the Pause metadata records frozen E1 bytes without choosing how Win32 `SendInput` should synthesize them.

## Verification

With the existing AV codec object available, run:

```sh
python3 tests/interop/keyboard_scan_coverage.py
```

The runner combines existing AV input ownership/replay/release tests with the golden-table checks, requires at least 90% line and taken-branch coverage of the entire `av_input.c` module, and runs the same tests under AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer. It preserves its uniquely named evidence directory under `/tmp/waddle-keyboard-scan-coverage/`.
