# Linux socket-owner guard helper review

Read-only comparison of `tests/interop/linux_socket_owner_guards.c` with `include/waddle/interop_linux_socket.h` and the §8.23 owner contract. The earlier ownership-transfer and byte-alignment assumptions have been corrected: create retains the accepted fd until destroy, and a `uint8_t *` may point at any byte address.

## Findings

1. **The two “wrapped pointer” create tests do not reach the wrap check.** At lines 189–197, each address is formed as `UINTPTR_MAX - sizeof(range) + 2`. On the supported 64-bit ABI, `sizeof(config)` and `sizeof(owner)` are multiples of 8, while `UINTPTR_MAX` is 7 modulo 8. Adding 2 therefore makes each address 1 modulo 8. `range_valid` checks alignment before nonwrapping extent, so these invocations are rejected as misaligned and never exercise the overflow branch. Use `UINTPTR_MAX - sizeof(range) + 1` instead: the start is aligned and adding the full extent wraps. Keep the separate misalignment tests.

2. **Several sentinel snapshots use C structure assignment before `memcmp`, which does not portably preserve padding bytes.** Examples include `before = config` at line 182, `poll_before = poll_out` at 231/246/347, `work_before = work` at 240/251/263/272, and `event_before = event` at 306. The poll/work records contain padding in the frozen ABI. C permits padding bytes to take unspecified values when a struct value is stored, including by assignment, so an otherwise correct implementation may fail these bytewise comparisons under another compiler/optimization. Snapshot the representations into `unsigned char [sizeof object]` buffers with `memcpy`, then compare against those byte buffers.

3. **The misaligned output-slot probe’s byte extent extends beyond its backing object.** At lines 173–178, `misaligned_slot` starts one byte into the `owner` pointer object but is passed as an output range of `sizeof(interop_linux_socket_t *)`, so the range covers one byte beyond `owner`. The API must reject alignment before access, but the fixture can provide a fully accessible invalid-alignment range instead: use an `_Alignas(interop_linux_socket_t *)` byte array sized `sizeof(pointer) + _Alignof(pointer)`, and pass `storage + 1`.

## Checked behavior

The successful-create check now correctly requires the fd to remain open, then verifies destroy closes it. The frame `+1` case is correctly treated as a valid `uint8_t` address; its full 65,536-byte range is backed by the 65,537-byte array. The overlap output snapshot at lines 337–344 is taken before the call with `memcpy`, so it does not have the former sentinel-overwrite error. The thread-affinity calls use an authentic live owner and verify the failed destroy preserves the caller’s owner slot.

The owner-alias poll output probe is intentionally invalid under the header’s disjoint-output rule; the later successful drive with the earlier cookie is a useful indirect check that the rejected call did not consume that cookie. The tests do not establish socketpair end-to-end transport behavior or production VSOCK behavior; those belong to the integrated fixture and platform gates.
