# CLVK runtime v2 admission audit

This note documents the standalone admission helper in `scripts/clvk_runtime_audit.py`. The build driver owns compiler discovery, cache construction, link invocation, and receipt publication. The helper is a read-only post-link check; it does not claim native execution or independently prove that a GNU map and PE image were emitted by the same process. The driver must invoke both checks on fresh outputs, compare `OUTPUT(...)` to the exact DLL path, then bind both returned SHA-256 digests and toolchain metadata into the build receipt.

## Callable interface

```python
verify_runtime_link(
    link_map,
    dll_path,
    expected_output_name="libOpenCL.dll",
    shim_object="clvk_runtime_tls.o",
    eh_pool_object="clvk_runtime_eh_pool.o",
    runtime_policy=policy,
    map_working_directory=build_root,
)

audit_runtime_tls(
    dll_path,
    nm="x86_64-w64-mingw32-nm",
    limits={"max_objects": 32, "max_bytes": 192, "max_align": 16},
    expected_exports=exports_parsed_from_reviewed_def,
    runtime_policy=policy,
)

verify_toolchain_identity(actual_identity, reviewed_identity)
```

All failures raise `audit_error_t`, a `ValueError` subclass. The APIs return JSON-serializable dictionaries. No validation relies on Python `assert`; callers should treat any exception as a failed build and must not publish a success receipt. `verify_runtime_link` enforces the five replacement ABI symbols from the Waddle shim, rejects the replaced libgcc members and static winpthreads, requires the EH-pool object, checks the map output path, and checks exact CRT section order plus symbol owners. It accepts maps up to 256 MiB; the preserved production candidate map is about 135 MB. `audit_runtime_tls` validates the PE and TLS data, inventories every named emutls control, bounds worst-case first-touch storage, attributes sensitive imported API references, and records the DLL digest.

## PE and TLS invariants

`pe_image_t` accepts only AMD64 PE32+ files. It validates DOS, PE, COFF, optional-header, data-directory and section-table bounds; rejects overlapping virtual or raw sections; and performs checked RVA/VA conversion. Import and TLS directory ranges, import strings, thunk arrays, IAT slots, TLS raw template, and callback table are all bounded before reads.

The export directory must be present and its complete named set must equal the caller-supplied names parsed from the reviewed `OpenCL.def`; unnamed exports, extra shim/test helpers, forwarded exports, duplicate names, invalid ordinals, and non-executable named export targets fail closed. The driver must include the DEF hash in the build receipt. The TLS directory must exist and provide a nonempty raw template in one file-backed section. Exactly one `runtime_tls_template` symbol must fall within that template and have a 16-byte-relative offset. `AddressOfIndex` must resolve to exactly one writable `_tls_index` symbol in mapped writable data. Every TLS callback must resolve to executable bytes and the callback list must terminate within 128 entries. The emutls accessor must contain the x64 `mov %gs:0x58` instruction sequence within the symbol’s code span. A complete image without symbols or executable code fails closed.

The PE must retain exactly one `waddle_clvk_runtime_tls_limits` symbol in read-only `.rdata`. Its 16 little-endian bytes are `{schema=1, arena_bytes, slot_capacity, max_align}`. The `limits` argument is required and must exactly match the three compiled values; the helper does not contain a fallback capacity. The build driver must obtain the expected values from the source/build configuration and record them with the object and DLL hashes. This prevents a caller from silently substituting the old v1 cap.

For each distinct-address `__emutls_v.*` control, the helper reads the GCC ABI size and alignment words from the image. Names that alias the same control address count once. It requires at least one control and checks count, power-of-two alignment, maximum alignment, and:

```text
sum(size + max(align, 1) - 1) <= max_bytes
```

This bound is independent of PE symbol order and therefore bounds any first-touch order used by the shim. It intentionally errs on the side of reserving a full alignment minus one byte per control. The final OpenCL.dll must be audited; probe counts are not substituted for production counts.

## Sensitive API and runtime policy

The helper parses every PE import descriptor and every named import slot, then scans executable sections for supported x64 RIP-relative IAT call/jump/load forms, import thunks, and direct rel32 calls/jumps to those thunks. It rejects LEA-based, absolute IAT-pointer, and non-executable refptr forms because it cannot prove their control flow. It attributes accepted references to the nearest preceding executable `nm` symbol. It scans non-executable sections at every byte offset for absolute pointers to sensitive IAT slots or thunks; any such unclassified data alias is rejected. Imported sensitive APIs with no attributable code reference are rejected as vacuous.

The driver must provide explicit caller allowlists for runtime APIs that are allowed in the final DLL. The observed caller set must equal each allowlist exactly; stale entries fail as well as missing entries. At minimum, `TlsAlloc` and `TlsFree` policies must enumerate the exact paired runtime key create/delete callers. The API requires a nonempty `tls_pairs` mapping from every permitted `TlsAlloc` caller to its permitted `TlsFree` caller; the observed caller sets must exactly equal those keys and values. Calls to `FlsAlloc` and `DuplicateHandle` fail unless their exact owners are explicitly allowlisted. This matters because the preserved full candidate imports `DuplicateHandle` for LLVM process, mapped-file, and I/O handling; the import itself does not identify a leak.

If `GetProcAddress` is imported, the policy must enumerate the complete observed caller set. In addition, the helper searches file-backed section data for static narrow and UTF-16 names of `TlsAlloc`, `TlsFree`, `FlsAlloc`, `FlsFree`, or `DuplicateHandle`, excluding only the corresponding import-name records. A sensitive API name found with dynamic lookup fails closed. Computed or obfuscated API names cannot be proven safe by byte scanning; any such behavior is outside this helper’s supported policy and must be removed or separately changed to a statically auditable form before admission.

Forbidden thread-exit runtime names and archive members are rejected. Current defaults include `tls_atexit.o`, `__mingw_cxa_atexit`, `__cxa_atexit`, `__dso_handle`, `__cxa_thread_atexit`, `__cxa_thread_atexit_impl`, and `__at_thread_exit`. If a required production helper symbol collides with this policy, the implementation must resolve that semantic issue explicitly; do not weaken it with a broad textual exception.

The CRT policy is fail-closed and cannot be weakened through caller policy. `.CRT$XC*` must contain, in order, `XCA` from `cinitexe.o`, `XCAWDL` from `clvk_runtime_eh_pool.o`, and `XCZ` from `cinitexe.o`. `.CRT$XI*` must contain, in order, `XIA` from `cinitexe.o`, `XIAA` from `dllcrt2.o`, and `XIZ` from `cinitexe.o`. The map must resolve `atexit` from `dllcrt2.o`, `__gnu_cxx::__freeres()` from `eh_alloc.o`, `waddle_clvk_eh_pool_register`, `waddle_clvk_eh_pool_release`, and `waddle_clvk_eh_pool_initializer` from the Waddle EH-pool object, and `waddle_clvk_runtime_tls_limits` from the Waddle TLS shim object. The expected order means the initializer registers the release before C++ constructors, and the registered onexit callback runs last under the reviewed CRT order. It does not make `__freeres` safe if code can allocate an exception afterward; that remains an implementation and native-qualification invariant.

## Toolchain identity contract

The driver supplies `verify_toolchain_identity` with `actual` and host-reviewed `expected` records. Each record has:

- `target`: exactly `x86_64-w64-mingw32`;
- `cc` and `cxx`: `resolved_path`, `sha256`, `version`, `dumpfullversion`, `dumpmachine`, and `thread_model`;
- `runtime_archives`: path and SHA-256 for `libgcc.a`, `libgcc_eh.a`, `libstdc++.a`, `libmingw32.a`, `libmsvcrt.a`, `dllcrt2.o`, `crtbegin.o`, and `crtend.o`;
- `critical_headers`: a nonempty mapping of resolved C++ runtime header paths to SHA-256.

Both compiler thread models must be `win32`, both targets must match, and both reported versions must identify GCC 14.2.0. The helper compares the full records exactly. It embeds no universal archive hashes: each build receipt binds to the exact reviewed private extraction or CI toolchain used for that DLL. A version string alone is not archive identity.

## Test evidence and limits

`tests/vgpu/clvk_runtime_audit_test.py` constructs synthetic PE32+ files and GNU maps. It covers missing/truncated headers, invalid machine, raw section overflow, TLS directory and template bounds, wrong TEB displacement, invalid `_tls_index` location, missing symbol tables and limits metadata, zero TLS inventory, 33 objects, 600-byte objects, alignment 32 and non-power-of-two alignment, the first-touch bound, exact-fit acceptance, compiled-limit mismatch, wrong/extra exports and bad export ordinals, IAT and thunk calls, data aliases, unsupported pointer encodings, dynamic lookup, FLS and duplicate-handle policy, missing/empty/wrong-output maps, wrong ABI owners, missing EH pool/CRT data, forbidden thread-exit symbols, and compiler/runtime hash mismatch. Run it with `python3 -O -B tests/vgpu/clvk_runtime_audit_test.py`; optimized mode is intentional to ensure policy checks do not depend on `assert` statements.

This is a bounded static admission check, not a general x86-64 control-flow or data-flow verifier. It detects the supported import-call encodings and rejects unsupported pointer and dynamic-name cases. It cannot prove arbitrary hand-written assembly, computed API names, runtime-loaded modules, callbacks registered outside the inspected PE, or that an object file represented in a map is byte-identical to a particular source file. The driver must compare object/source/tool hashes in its build metadata, rebuild in a fresh directory, invoke these checks on the produced paths, and retain the exact hashes. Native pre-existing-thread TLS, heap retention, FLS, handle/TLS counts, exception-pool reclamation, workload behavior, and natural 2048-cycle completion remain separate acceptance gates.

## Preserved production-candidate measurement

Read-only inspection of the preserved v1 full OpenCL.dll found 20 emutls symbol entries aliasing to 11 distinct control addresses, total instance sizes 88 bytes, maximum alignment 8, and conservative worst-case bound 157 bytes. The DLL had a 608-byte TLS template with `runtime_tls_template` inside the TLS directory. It imported `TlsAlloc`, `TlsFree`, `DuplicateHandle`, and `GetProcAddress`. The map contained `__at_thread_exit` helper artifacts. This legacy DLL is a measurement/control only; it was built but never executed, and it is rejected by the v2 teardown policy pending the implementation’s resolution of those linked helpers.

## Root review corrections

The independent integration review strengthened three boundaries: metadata must actually be non-writable as well as named `.rdata`; the complete derived x64 record (208 bytes for the current constants) must fit and be zero initialized, rather than merely admitting its first address; and sensitive IAT references ending at the last bytes of an executable section must be scanned. Synthetic negative controls exercise these cases. Repository-owned classes/types and constants follow the AGENTS naming policy; framework override names remain the external unittest interface. Seventeen synthetic groups pass normally and under Python optimization. No actual production image is admitted or executed by these tests.

### Mapped absence and unsupported ABI fences

The final-image rule continues to reject every defined/mapped thread-exit root, including `notify_all_at_thread_exit`. A GNU map's separate discarded-input inventory and literal wildcard script declarations are not mapped code; the parser recognizes the unique output boundary and rejects a missing/ambiguous boundary. Long section names can place input address/size/owner on a continuation row; exact ownership uses that row instead of a preceding unrelated object. Synthetic regressions cover both, alongside actual map validation. Undefined section-zero COFF names after deliberate removal are not mapped definitions. The companion undefined link wrappers and six actual negative controls in [CLVK_RUNTIME_V2.md](CLVK_RUNTIME_V2.md) are required to prevent use of discarded functions or the incompatible low-level gthread comparator; no broad symbol allowlist was introduced.

### Debian version label and exact package proof

The reviewed Debian packages are `14.2.0-19+27+b1`, but the real compiler drivers deliberately report `14-win32` (and predefined macros 14.0.0). Do not rewrite that output to 14.2.0 or accept a guessed family from the name. The helper now admits this exact Debian label only with all five reviewed package versions, package/receipt/provenance digests and complete compiler/backend/runtime/header member proof; the entire actual identity must still equal the reviewed record. Other versions and incomplete/mismatched provenance fail. The driver reads actual `.deb` metadata, verifies archive hashes, and compares selected extracted bytes to package member bytes before producing that proof. The standalone helper consumes those verified records; it does not independently execute package extraction.

Root independently recomputed all five package digests/versions and all 122 package-member/extracted-file hashes from `toolchain_capture_v2.json`. The reviewed identity and root receipt are preserved at `build/qualification_feature_20261021/root_tls/reviewed_toolchain.json` and `toolchain_root_review.json`. Twenty synthetic groups now include missing provenance, release drift, missing backend proof, changed member digest, absolute member path and unsupported schema. This verifies package identity, not native behavior or source approval for other SDK changes.
