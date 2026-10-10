# Native WSI test device capabilities

## Scope and observed cause

Repair the synthetic WSI device fixture in `src/vgpu/venus_icd.zig` and add a negative extension-admission oracle. Windows CI run `38001567317`, job `114060606421`, ran the complete 393-test ICD suite and reported exactly two failures: `WSI status replacement failure retires valid old chain but preserves image owners` expected `VK_ERROR_OUT_OF_DEVICE_MEMORY` but received `VK_ERROR_EXTENSION_NOT_PRESENT`; `native swapchain frontend validates device surface ancestors and publishes bound image ownership atomically` expected success but received that same extension error.

Both tests use `wsi_status_graph_t.init`, which reserves a synthetic device directly and bypasses `create_device`. Its cache previously left `enabled_state.extension_mask` zero. Production `create_swapchain` correctly checks bit zero, representing explicitly enabled `VK_KHR_swapchain`, on Windows before native-surface ownership or backend image creation. The fixture had never requested this prerequisite. Linux intentionally retains the internal portable WSI path without that Windows-only admission gate, explaining its passing tests.

Both complete Zig unit executables use the unchanged `venus_wsi.zig` test-only exported native sink: `test_extent` accepts handle one as the fixture's 64-by-64 surface and rejects handle two. The Windows unit recipe does not link the production Win32 sink object, so no synthetic HWND reaches the Windows API. The separately built production C fixture does link the real sink and remains unchanged. This existing test architecture is not modified by this repair.

In scope: the existing WSI graph helper, one new WSI regression, and these implementation records. Out of scope: production validation, advertised capabilities, protocol negotiation, transport behavior, external ABI, real driver support, coverage accounting, compiler settings, dependencies, build/link composition, AV code, or README. No production byte changes or platform test skips are permitted.

## Architecture and execution

1. Bind the existing synchronous fixture and reserve its instance, physical device, device, queue, semaphore, allocation, and image, preserving the existing ownership graph and live-object count of seven.
2. Build a local `device_native.owned_request_t` with one extension identifier, zero, on Windows, referencing a one-element independent literal name table containing `VK_KHR_swapchain`. On Linux the requested extension count stays zero, preserving its actual empty guest-extension advertisement rather than synthesizing unsupported enablement. Other request fields retain their zero defaults.
3. Use production `enabled_device_request` to project that explicit name into the same owned enablement snapshot used by real device creation. Assert that the exact extension mask is one on Windows and zero on Linux, and that all feature state deeply equals `disabled_device_state().features`.
4. Store that owned snapshot in the synthetic device cache, then initialize the same queue, surface, old swapchain, and bound image records as before. No raw name pointer is stored.
5. Execute every existing positive, malformed-input, native-loss, replacement-retirement, allocation-failure, and owner-publication case unchanged. These tests now reach the behavior they were written to verify on Windows as well as Linux.
6. The new test starts a fresh graph for each mask `0`, `2`, `4`, and `8`: no extension, robustness2 only, maintenance5 only, and pipeline-library only. None has the swapchain bit. It marks the old image acquired and attempts a valid replacement through the real ICD frontend.
7. Windows must return exactly `VK_ERROR_EXTENSION_NOT_PRESENT`, clear the caller's poisoned output, preserve the entire WSI registry including the old generation, next identifier, and acquired image, retain the exact image/allocation records and resource metadata, preserve the disabled mask, keep seven live objects, submit zero backend commands, and retain a healthy transport.
8. Linux executes that same call and assertions. Its portable frontend must reach the existing injected image-creation failure, return `VK_ERROR_OUT_OF_DEVICE_MEMORY`, retire the valid old chain, consume one tentative WSI identifier, submit exactly one command, and preserve the old acquired image and allocation owners. Expected registry changes are explicit, not a skipped negative test.
9. Each iteration defers existing `venus_icd_abandon`, which resets ICD/WSI state and releases fixture-owned cache storage.

## Data, ownership, and layout

No public structures, wire formats, alignments, atomics, or native layouts change. The existing enablement structure is copied by value. The request and extension name table are local, synchronously borrowed inputs; the helper retains no pointer into them. The test's snapshots copy the complete value-based WSI registry plus the image and memory native object records and resource metadata. No allocation or new heap owner is introduced by the assertions. The preexisting fixture owns all seven live objects until abandonment. Existing replacement tests retain all their output, retirement, slot, submission-count, and object-count checks.

## Concurrency and failure semantics

The fixture and all assertions run synchronously in the existing serialized test process. Production entrypoints continue to hold the ICD lock; the helper does not run concurrently with any frontend call. No new synchronization or callback is introduced. A failure of extension-name projection, accidental enablement of any feature, incorrect native admission, partial output publication, premature old-chain retirement, owner mutation, or unexpected backend submission is an assertion failure. Errors are never converted to success and no test returns early based on its platform.

## Verification and boundaries

Use official Zig 0.13.0 with the established SDK. Run focused WSI checks and the full unfiltered Linux Debug/ReleaseSafe ICD suites, direct C fixture, loader fixture, allocation-fault fixture, and unchanged relevant sanitizer attempts. Cross-compile and link both full unfiltered Windows ICD test executables with the workflow's complete C oracle set; clear Linux `CPATH`/`LIBRARY_PATH` for the Windows commands. Cross-linking is not native Windows execution.

Compare the final source against baseline `9762151`, allowing only the graph helper change and the new test block. Preserve all other source bytes, especially `create_swapchain`, prior negative feature/extension checks, both formerly failing tests, and all other tests. Obtain independent source/evidence review. Whole-ICD coverage is coordinated with the separate coverage-recorder repair and bound to the frozen ICD source hash; this task never changes or relaxes its denominator, threshold, exclusion policy, or test selection.

A native Windows CI run on the exact published commit remains required to qualify the original failure repair. Sanitizer runtime limitations must remain explicit. None of these local fixtures establishes real GPU, guest VM, DXVK, Wayland, or physical presentation acceptance. Parent owns publication and CI monitoring.
