# Presented Worker API1.0 Query Runtime Correction

## Scope and concrete failure

The direct production-worker acceptance fixture creates a Vulkan1.0 instance
without enabled extensions, then requires both the core
`vkGetPhysicalDeviceFeatures2` and KHR spelling to resolve to the same nonnull
procedure. Production instance advertisement introduced in `4d571464` correctly
requires instance API1.1 for the core spelling and explicit
`VK_KHR_get_physical_device_properties2` enablement for the KHR spelling.
`venus_icd_bind_capabilities` retains the legacy API1.0/4KiB reply profile, so
raising the fixture's requested API would violate the binding's contract.

Linux CI run38002124471/job114062429263 at f04e358 passes native ICD tests and
loader integration, links the presented-worker fixture, then exits1 at
`Presented worker binding failed: mode=0` before its first device-name message.
The stale direct-only lookup assertion necessarily fails at that point once
worker negotiation and physical enumeration reach it. CI's original generic
message cannot independently distinguish earlier stages; the local exact
production-dispatch probe establishes this contradiction without claiming an
actual renderer execution.

Only `tests/vgpu/worker_presented.c` changes. Production capability policy,
backend, link inputs, transport, deadlines, ownership checks, workload selectors,
coverage thresholds, sanitizer settings, dependencies and README stay unchanged.

## Architecture and inter-component interactions

The fixture continues to exec the actual production worker and communicate using
its mapped region, stream RPC channel and separate frame socket. After actual
capability negotiation, direct ICD cycles explicitly enable the guest-owned KHR
physical-query extension in their instance create info. The production ICD
validates that name, remembers its bit with the instance, and strips guest-only
instance extension names from the pinned host request as before.

The direct fixture resolves the KHR query through both the instance and physical
ICD dispatchers, requires the same nonnull function pointer, and requires the
core1.1 spelling to remain null. Both KHR pointers execute their existing query
calls against the worker-negotiated cache. The loader build remains an API1.0
instance with no enabled extension and keeps its existing KHR-hidden assertion.
This is a legal opt-in correction, not relaxed production advertisement.

## Data structures, ABI, memory and ownership

A one-element stack array borrows the static Vulkan extension-name string for
the duration of `icd_cycles`; instance creation is synchronous and the production
ICD copies only the validated bit. No caller pointer is retained. The create
info, raw feature cache, Features2 root and robust/draw/unknown chain layouts are
unchanged. Both repeated calls retain exact command-count, core-byte,
header/link, zero-feature and unknown-sentinel checks.

Diagnostic stage strings are borrowed static immutable literals. The existing
image-stage pointer becomes a general ICD stage pointer; the loop index moves
outside the loop solely so error reporting can include it. The run-fixture
stage has the same lifetime and no allocation. Diagnostic messages include only
stage, cycle/mode and descriptor counts, not payloads, tokens or native pointers.
No public function, C ABI, wire field, alignment or ownership rule changes.

## Step-by-step execution

1. Resolve the real worker executable, acquire mapping/socket pairs, exec the
   worker, and negotiate the channel and actual capabilities.
2. Execute the existing raw version and instance cycles in the same short-circuit
   order. Each stage now has a distinct failure label.
3. Bind the ICD using the actual negotiated snapshot. Direct builds attach the
   one explicit KHR extension; loader builds do not.
4. For each of eight cycles per normal/corrupt session, create and enumerate the
   instance, enforce the legal lookup shape, query actual physical values, and
   retain all cached-query/canary checks.
5. Execute the unchanged core synchronization, mapping, image or selected
   compute/triangle workload and release its owners.
6. Require empty ICD unbind, invalid unregistered presentation rejection,
   unknown-release loss in corrupt mode, exact worker exit status and restoration
   of the descriptor baseline, with the existing watchdog/deadlines.

## Concurrency and synchronization

The fixture remains single-threaded. Actual worker/renderer processes retain
existing IPC and synchronization behavior. No mutex or atomic changes occur;
production dispatch continues its existing serialized state access. Stage
pointers and loop index belong exclusively to the fixture thread.

## Errors and cancellation

Every previous failure branch still returns failure. Splitting the raw-query,
raw-instance and ICD condition only inserts diagnostic labels between the same
short-circuit operations. `venus_worker_destroy` failure still forces result1;
when it is the first error it gains its own stage label. ICD abandonment remains
after the existing worker retirement attempt. No cleanup, descriptor close,
process wait, retry limit or failure-injection behavior is removed or reordered.

## Verification criteria

- Reproduce the original no-extension lookup shape against the production ICD
  and independently verify the enabled KHR/non-enabled core shape. The focused
  probe may reuse the existing independent encoder transport only as a native
  dispatch test; it must never replace the real worker or count as renderer proof.
- Preserve all existing feature output/cache/canary checks and the loader's
  no-extension negative path. Run existing native C ABI and loader fixtures.
- Build the exact direct/shared-loader normal and sanitized acceptance binaries.
  Record link-only exclusions of absent worker prerequisites explicitly.
- Attempt the unchanged CI aggregate with leak detection enabled. Missing local
  renderer prerequisites are a blocker, not a runtime or leak-free pass. Updated
  native CI remains mandatory for real normal and sanitized worker acceptance.
- Obtain independent source/evidence review and record an exact-file atomic
  commit and later attribution receipt. No feature-milestone credit follows
  from this fixture correction alone.
