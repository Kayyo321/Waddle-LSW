# Guest WSI resize and retirement lifetime

## Scope and boundaries

This bounded production change repairs the existing synchronous guest Win32 WSI path. It does not connect the AV HWND registry to host Wayland presentation, create direct guest DMA-BUF presentation, or qualify Windows/GPU behavior in a Linux-only test environment. The original surface and swapchain numeric identities remain monotonically allocated for each ICD binding. Reuse of an HWND without an intervening observed loss remains an open integration problem requiring window-destruction events; raw IsWindow checks cannot prove a window generation.

The covered transitions are zero-area/minimized windows, observed native window loss, resize detected before acquire or presentation, resize while GPU readback runs, failed swapchain replacement, and explicit image retirement. No dependencies are added. C remains the native Win32 boundary and Zig owns bounded state and temporary pixel allocation.

## Architecture and interactions

Vulkan ICD entry points hold the existing ICD lock and invoke venus_wsi. The registry owns at most 16 surfaces and 16 swapchains, each with up to three backend image/memory pairs. A borrowed native window belongs to the application. Backend callbacks own Vulkan execution and acknowledge image destruction or transfer lost-device resources to the preexisting abandon ledger. Readback synchronously completes before the native sink receives borrowed temporary pixel storage. The native sink acquires a DC only during one call and invokes ReleaseDC on every acquired path; a native release failure is reported, not assumed to have retired an OS resource.

No protocol layout, external wire format, host receiver, or AV interface changes. No new Vulkan feature or extension is advertised. Native window extent queries distinguish a valid zero-area client rectangle from invalid/destroyed windows. The new exact-size sink supplements the existing scaled compatibility API, so callers that deliberately request scaling keep their behavior.

## Data and state transitions

A surface receives a sticky lost flag after a native extent query reports window loss. Querying that surface again never resurrects it even if the numeric HWND later becomes live. A zero-area valid window is not lost. Win32 capabilities report identical current/minimum/maximum extents, with both axes normalized to zero when either native axis is empty. Acquire/create/present return OUT_OF_DATE rather than signaling sync or allocating backing for it.

A swapchain receives a separate sticky out_of_date flag after an extent mismatch or empty client rectangle. This flag is distinct from retired: retirement prohibits new acquire but permits previously acquired images to present if the extent remains compatible. OUT_OF_DATE prohibits acquire and present even when the window later returns to the same dimensions. Neither flag frees images; explicit swapchain/device destruction remains the sole image retirement site.

A valid oldSwapchain is retired before recoverable replacement failures, including image creation, registry exhaustion, identity exhaustion, and surface extent/loss errors. Malformed create headers, invalid/stale/wrong-owner old handles, and already retired old handles are rejected without touching an unrelated chain. Backend partial-create failures destroy only successfully created new pairs. Acquired old images and all old owners stay retained until explicit destruction. Output handles and acquire indices remain unchanged on failure in the state module; existing ICD wrappers retain their documented output policy.

## Execution sequence

Create surface validates the native HWND including zero extent, reserves a monotonically increasing ID and publishes the fixed slot. Capabilities, formats and support checks call one common live-surface validation helper. Create swapchain validates the request and old identity, retires the supplied valid old chain, validates current surface extent, allocates new image pairs transactionally, then publishes the replacement slot.

Acquire validates identity and retirement, then validates the current surface and extent before invoking the backend semaphore/fence signal. Failure cannot acquire an image, change its index, or consume a backend signal call. Present validates acquisition, window and dimensions before allocating pixels; after synchronous readback it validates the window and dimensions again. The exact-size sink rechecks extent after GetDC and never stretches to a changed client rectangle. A detected mismatch is OUT_OF_DATE; observed native loss is sticky SURFACE_LOST. Temporary pixel allocations retire on every exit, and acquired DCs always receive a release attempt. Presentation success, OUT_OF_DATE and SURFACE_LOST release the image acquisition while retaining its actual backend image/memory pair. Local pixel-allocation failure before queueing preserves acquisition; device-loss failure retains unresolved owners for the existing abandon path.

## Concurrency and lifetime

The ICD lock serializes registry access and backend callbacks. Native HWND lifetime and application window events remain externally owned; no subclass, WinEvent hook, property, listener cookie, new thread, or persistent native allocation is introduced. Concurrent OS resize can occur after any extent snapshot. Exact drawing uses source dimensions, preventing this API from scaling stale pixels; it cannot freeze application window destruction or provide a cross-thread HWND generation guarantee. Callers must retain their window during presentation. The same-value HWND reuse gap is explicitly not solved by these state checks.

## Failure handling

State allocation failures preserve old image ownership while retiring its acquire eligibility when used as a replacement. Resize or loss before acquire does not signal semaphores/fences. Resize/loss during readback does not submit stale pixels to the native sink. Local allocation failures preserve the acquired image. Every backend readback failure, including OOM and MEMORY_MAP_FAILED, becomes DEVICE_LOST because the callback does not prove whether submission already occurred. Invalid native parameters fail before GetDC. ReleaseDC is attempted on every acquired DC even after a post-acquisition extent mismatch, StretchDIBits failure, ReleaseDC failure, or FIFO pacing failure. Non-Windows native calls fail predictably without accessing the HWND.

## Verification and acceptance

Portable Zig tests exercise normal acquire/present, replacement success/failure/quota/exhaustion, acquired-old presentation, minimized restore, sticky observed loss and out-of-date generations, resize/destroy during readback, and exact image teardown accounting. Existing ICD embedded regression expectations are updated to the Vulkan retirement contract. Native C code is compiled against local test declarations with deterministic fake Win32 API behavior to execute error/DC ownership branches on Linux; those declarations are test doubles, not vendored SDK headers. The same production source and real Windows fixture are cross-compiled against Zig's existing Windows SDK support. Native execution remains untested here.

The native fixture's injected ReleaseDC failure checks a balanced release attempt and propagated error, not OS resource retirement after a failed call. Only real Windows execution can establish GDI/USER count stability.

The ICD consumes batch wait semaphores once before individual presentation. Any local OOM after these waits or an earlier enqueued presentation/rejection becomes DEVICE_LOST and poisons the current binding. A first local OOM with no prior queue effect stops the batch before a later item can invalidate the unchanged-state guarantee; remaining pResults receive the same OOM. Every backend readback failure always becomes DEVICE_LOST because its enqueue boundary is not exposed. This conservative policy requires reinitialization under ambiguous presentation resource pressure. Presentation result aggregation applies DEVICE_LOST, SURFACE_LOST, OUT_OF_DATE, SUBOPTIMAL, SUCCESS precedence independent of list order; no unsupported extension outcome is advertised. Negative invalid-usage codes remain defensive behavior outside valid Vulkan calls.

Run Debug and ReleaseSafe WSI tests, portable C tests with AddressSanitizer/LeakSanitizer/UBSan, owned Zig ASan/LSan instrumentation and allocator checks, and coverage gates >=90% for changed production state/native code. Preserve separate coverage and sanitizer artifacts under build. Record exact commands and evidence in TRACKER.md. No cloud-only result closes roadmap D's real Windows/Wayland or shared RTX acceptance.

## Normative references

Reviewed 2026-10-09: [VkSwapchainCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html) specifies old-swapchain retirement even when replacement fails and permits presentation of already acquired old images. [SetPropW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setpropw) requires explicit property removal before WM_NCDESTROY; therefore this milestone deliberately does not introduce a property scheme lacking window-event cleanup.

The [vkQueuePresentKHR reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html), reviewed 2026-10-09, defines result precedence, unchanged resource/synchronization guarantees for retryable OOM, DEVICE_LOST when that cannot be guaranteed, semaphore execution on presentation rejection, and release of image acquisition. The [Win32 WSI capability rules](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html) require exact minimum/current/maximum extents and paired zero axes.
