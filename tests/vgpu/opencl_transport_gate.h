/** @file opencl_transport_gate.h @brief Qualification-only authenticated cancel gate. */
#ifndef WaddleOpenclTransportGateH
#define WaddleOpenclTransportGateH

#include "waddle/venus_tcp.h"

#if defined(WaddleOpenclTransportGate)
/** @brief Publish an authenticated request witness, then wait for its real cancel flag.
 * @param[in] session Nonzero authenticated outer TCP session identity.
 * @param[in] sequence Nonzero outer TCP request sequence, before inner sequence removal.
 * @param[in] opcode Must be18, the fully received OpenCL submit opcode.
 * @param[in] deadline_ms Existing absolute monotonic exchange deadline; never extended.
 * @param[in] cancel Nonnull borrowed live atomic used by the real transport exchange.
 * @return RingCancelled only after acquire-observing cancel nonzero; RingTimeout if the
 * deadline expires first; RingInvalid for absent/malformed witness configuration or
 * arguments; RingClosed for marker I/O, clock, or wait failures. No request is forwarded.
 * The environment path is copied into a bounded stack buffer. The marker file is created
 * exclusively with mode0600, written, and closed before waiting; on setup failure any
 * partially created marker is unlinked. No heap, fd, path, or atomic ownership is retained.
 * Caller owns cancel and its lifetime for this synchronous call. The configured path is
 * process-global: do not mutate the environment during a call, and serialize calls that
 * use the same configured path. A second concurrent caller will fail the O_EXCL collision.
 */
venus_ring_status_t venus_opencl_transport_gate_wait(uint64_t session,uint64_t sequence,
    uint32_t opcode,uint64_t deadline_ms,const _Atomic uint32_t *cancel);
#endif

#endif
