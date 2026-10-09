# Native ICD extension projection oracle

## Scope and cause

Repair platform-stale test expectations in `tests/vgpu/icd.c` and the existing public extension intersection test in `src/vgpu/venus_icd.zig`. No production code, advertised capability, protocol, dependency, coverage rule, or threshold changes are permitted. The complete native Windows unit run reached the C lifecycle fixture and aborted at its assumption that an unnegotiated physical device exposes zero extensions.

`venus_icd_bind` selects the legacy, unnegotiated transport profile. `supported_device_extensions` deliberately contributes the implemented guest `VK_KHR_swapchain` facade at spec version 70 on Windows before considering the host reply profile. On Linux the same legacy binding contributes no device extensions. With a negotiated reply profile, the three implemented host extensions additionally require both the protocol capability bit and the host's actual name/version. A host's empty list therefore leaves one guest extension on Windows and none on Linux. Host query errors must leave all caller output and its original count unchanged even on Windows; the local guest name must not become a partially published success.

The global instance-extension query in the unbound C routing fixture correctly returns zero on both platforms: instance extension advertisement requires negotiated capabilities. Queue-family zero-capacity and physical-device zero-capacity checks are unrelated, truthful enumeration assertions and remain unchanged. A source audit found one additional stale platform assumption in the Zig public extension intersection test: its fixed host-only count, first-record index, empty-list output preservation, and filtered count also omitted the Windows guest record. Both affected oracles are repaired together.

## Architecture and data ownership

The C fixture invokes the public `vkEnumerateDeviceExtensionProperties` dispatch pointer against a real fixture-owned physical-device handle in each of 128 lifecycle iterations. Its independent expected native record is exactly `VK_KHR_swapchain`, version 70, for `_WIN32`; the expected record count is exactly zero elsewhere. Native `VkExtensionProperties` outputs and surrounding canary records are caller-owned stack arrays. No pointer escapes and no heap allocation is introduced.

The Zig test retains its existing negotiated mock renderer. That renderer returns the three host extension names with independently recognizable spec versions 2, 3, and 4. Expected output uses an independent literal name/version table, prepending the guest swapchain only for Windows. The query code under test remains unchanged. The mock's owned raw cache is released by the existing deferred `venus_icd_abandon` after every case.

No ABI structure, wire format, alignment, atomic primitive, storage lifetime, production API ceiling, or transport admission rule changes. The implementation retains its existing ICD mutex and physical namespace ownership; the new assertions use synchronous calls from the sole test thread.

## Execution and failure checks

1. Query with a null output and an arbitrary incoming count; require the exact platform count.
2. Enumerate every capacity from zero through one beyond the expected extent. Require the exact copied count, `VK_INCOMPLETE` only when capacity is insufficient, exact initialized name/version bytes, and unchanged bytes for all records outside the copied prefix.
3. Repeat count/fill queries and verify they introduce no legacy transport submissions or repeat negotiated host queries after cache publication.
4. Reject a layer, null physical handle, stale physical handle, and null count without modifying caller outputs/counts or submitting transport work.
5. Under negotiated host projection, check full and empty host lists on both platforms, then remove the robustness2 protocol bit. Require the remaining exact guest/host records and versions, rather than accepting merely a positive count.
6. Keep every existing host-query error case. Negative results preserve the entire poisoned output array and original count; no partial guest prefix may escape.
7. Destroy the instance or abandon the negotiated fixture through existing teardown, retaining existing lifetime/leak checks.

## Verification and limits

Run the full Linux ICD Debug and ReleaseSafe suites, direct C ABI fixture, loader fixture, allocation-fault variant, relevant sanitizers, and complete Windows Debug/ReleaseSafe cross-links with all current workflow oracle objects and codec libraries. Windows cross-compilation clears Linux SDK `CPATH` and `LIBRARY_PATH`; official Zig 0.13.0 and the established SDK are unchanged. Pin the two relevant source SHA-256 hashes for final evidence. Independently review the exact diff before completion.

Linux execution establishes the exact zero/host-only projection and error/capacity semantics. Windows cross-linking compiles the exact guest-plus-host expectations but cannot establish native runtime success. Native Windows CI remains an explicit pending qualification until the parent publishes and observes the exact commit. No real GPU, guest VM, DXVK, Wayland, or hardware acceptance is inferred.
