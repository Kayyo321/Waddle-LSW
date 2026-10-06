/** @file venus_tcp.h @brief Bounded authenticated test TCP protocol, separate from IVSHMEM. */
#ifndef WaddleVenusTcpH
/** @brief Include guard, no storage or ownership. */
#define WaddleVenusTcpH
#include "venus_request.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
/** @brief Exact client hello bytes; no native padding. */
#define VenusTcpClientHelloBytes 128u
/** @brief Exact server hello bytes, including actual160-byte receiver capset. */
#define VenusTcpServerHelloBytes 224u
/** @brief Exact authenticated session acknowledgment bytes. */
#define VenusTcpAckBytes 32u
/** @brief Maximum accepted command bytes in one TCP operation. */
#define VenusTcpMaxCommandBytes 65620u
/** @brief Maximum reply and synchronized mapped-copy bytes. */
#define VenusTcpMaxReplyBytes 4096u
/** @brief Maximum private JSON configuration file bytes. */
#define VenusTcpMaxConfigBytes 4096u
/** @brief Caller-owned decoded configuration; contains secret token, explicitly scrub after teardown.
 * No retained input pointers or heap; fields are immutable while initialized owners borrow it.
 * Decoding is thread-safe on disjoint records and zeros this entire output on failure.
 */
typedef struct venus_tcp_config_t {
    uint32_t version; /**< Exact schema version1. */
    uint32_t port; /**< Explicit TCP port1..65535. */
    uint32_t exchange_timeout_ms; /**< Whole exchange deadline1..60000ms. */
    char host[16]; /**< Owned NUL-terminated127.0.0.1 or10.0.2.2 literal. */
    uint8_t token[32]; /**< Owned secret capability bytes; never log. */
    char icd_path[1024]; /**< Owned NUL-terminated absolute ASCII Windows path. */
} venus_tcp_config_t;
/** @brief Private decoded client hello; secret token and nonce copied, never logged.
 * Caller owns/scrubs all bytes; no pointers, allocations or native wire casts.
 */
typedef struct venus_tcp_client_hello_t {
    uint8_t token[32]; /**< Owned secret authentication token. */
    uint8_t nonce[16]; /**< Owned per-launch random nonce. */
} venus_tcp_client_hello_t;
/** @brief Private decoded server hello; copied metadata, no pointers/allocations.
 * Caller owns record; capabilities require authoritative existing decode/compatibility checks.
 */
typedef struct venus_tcp_server_hello_t {
    uint64_t session; /**< Nonzero authenticated host session identity. */
    uint8_t nonce[16]; /**< Exact echoed client nonce, validated by caller. */
    uint8_t capabilities[160]; /**< Actual receiver capset bytes, not synthesized feature data. */
} venus_tcp_server_hello_t;
/** @brief Parse one bounded strict private JSON configuration.
 * @param[out] config Nonnull disjoint caller-owned record, zeroed on error.
 * @param[in] bytes Nonnull immutable private ASCII bytes[length], borrowed for call.
 * @param[in] length Actual bytes1..4096; no terminator required.
 * @return Ok; Invalid null; Corrupt malformed/schema/string/range; Limit file/parser budget.
 * @note Thread-safe on disjoint inputs/outputs; fixed64KiB parser scratch, no heap or retention.
 */
venus_ring_status_t venus_tcp_config_decode(venus_tcp_config_t *config, const void *bytes, size_t length);
/** @brief Encode a copied client hello into exactly128 bytes.
 * @param[in] hello Nonnull immutable private record, borrowed for call.
 * @param[out] bytes Nonnull disjoint caller-owned buffer[length], unchanged on error.
 * @param[in] length Exactly VenusTcpClientHelloBytes accessible bytes.
 * @return Ok or Invalid local argument/extent. Allocation-free/thread-safe, no retention.
 */
venus_ring_status_t venus_tcp_client_hello_encode(const venus_tcp_client_hello_t *hello, void *bytes, size_t length);
/** @brief Decode and validate complete client hello before authentication.
 * @param[out] hello Nonnull disjoint private record, zeroed on error; caller owns/scrubs token.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed for call.
 * @param[in] length Exactly128. @return Ok; Invalid null; Corrupt wire extent/shape.
 * @note Allocation-free/thread-safe; no ownership transfer or retained input pointers.
 */
venus_ring_status_t venus_tcp_client_hello_decode(venus_tcp_client_hello_t *hello, const void *bytes, size_t length);
/** @brief Encode server hello with actual receiver capabilities into exactly224 bytes.
 * @param[in] hello Nonnull immutable borrowed record, session must be nonzero.
 * @param[out] bytes Nonnull disjoint private buffer[length], unchanged on error.
 * @param[in] length Exactly224. @return Ok or Invalid local argument/session/extent.
 * @note Allocation-free/thread-safe; capset semantics remain existing codec's responsibility.
 */
venus_ring_status_t venus_tcp_server_hello_encode(const venus_tcp_server_hello_t *hello, void *bytes, size_t length);
/** @brief Decode complete server hello and copy its bounded actual capset bytes.
 * @param[out] hello Nonnull disjoint owned output, zeroed on error.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed for call.
 * @param[in] length Exactly224. @return Ok; Invalid null; Corrupt wire/session/extent.
 * @note Allocation-free/thread-safe; caller validates nonce and authoritative capset compatibility.
 */
venus_ring_status_t venus_tcp_server_hello_decode(venus_tcp_server_hello_t *hello, const void *bytes, size_t length);
/** @brief Encode exact authenticated session acknowledgment.
 * @param[in] session Nonzero established identity. @param[out] bytes Nonnull owned disjoint buffer.
 * @param[in] length Exactly32 accessible bytes. @return Ok or Invalid, output unchanged on failure.
 * @note Allocation-free/thread-safe; no retained storage or native ownership.
 */
venus_ring_status_t venus_tcp_ack_encode(uint64_t session, void *bytes, size_t length);
/** @brief Decode exact session acknowledgment; caller matches expected session.
 * @param[out] session Nonnull owned output, zeroed on failure.
 * @param[in] bytes Nonnull immutable disjoint private bytes[length], borrowed for call.
 * @param[in] length Exactly32. @return Ok; Invalid null; Corrupt shape/zeroidentity/extent.
 * @note Allocation-free/thread-safe; no retained storage.
 */
venus_ring_status_t venus_tcp_ack_decode(uint64_t *session, const void *bytes, size_t length);
/** @brief Constant-time equality for fixed-size secret authentication tokens.
 * @param[in] first Nullable borrowed accessible32-byte token; never retained/logged.
 * @param[in] second Nullable borrowed accessible32-byte token; never retained/logged.
 * @return One for equal nonnull inputs, zero for null/different. No allocation, thread-safe.
 */
int venus_tcp_token_equal(const uint8_t *first, const uint8_t *second);
/** @brief Copy exact compiled160-byte guest declaration, matching existing frontend profile.
 * @param[out] bytes Nonnull owned buffer[length], unchanged on failure.
 * @param[in] length Exactly160 accessible bytes. @return Ok or Invalid.
 * @note Allocation-free/thread-safe; does not advertise implemented Vulkan features.
 */
venus_ring_status_t venus_tcp_profile_encode(void *bytes, size_t length);
/** @brief Check exact compiled guest declaration without mutation.
 * @param[in] bytes Nullable immutable borrowed buffer[length]. @param[in] length Exactly160.
 * @return Ok for exact profile; Invalid null; Corrupt mismatch/extent. No allocation, thread-safe.
 */
venus_ring_status_t venus_tcp_profile_validate(const void *bytes, size_t length);
/** @brief Check TCP admitted operation/extent after existing envelope validation.
 * @param[in] request Nullable immutable decoded request with nonzero wire sequence.
 * @return Ok; Invalid null/direction/admitted kind; Limit command/reply/map ceiling.
 * @note Allocation-free/thread-safe; caller must also validate existing envelope/range/resource rules.
 */
venus_ring_status_t venus_tcp_request_limit(const venus_request_t *request);
/** @brief Check response pairing/extent after existing response envelope validation.
 * @param[in] request Nullable immutable already validated admitted wire request.
 * @param[in] response Nullable immutable decoded response, borrowed for call.
 * @return Ok; Invalid null; Corrupt pairing/direction/status/size mismatch; Limit payload ceiling.
 * @note Allocation-free/thread-safe; no pointers retained or storage mutated.
 */
venus_ring_status_t venus_tcp_response_limit(const venus_request_t *request, const venus_request_t *response);
/** @brief Sole-thread native socket owner, initialized only through these APIs.
 * Zero-initialize before first use; never copy a live owner. Windows holds one
 * balanced WSAStartup reference per live owner. Cancellation may be release-set
 * concurrently through the separately borrowed flag; close itself is serialized.
 */
typedef struct venus_tcp_socket_t {
    uintptr_t handle; /**< Private native socket; meaningful only when initialized. */
    uint32_t initialized; /**< One while socket and optional Winsock reference are owned. */
} venus_tcp_socket_t;
/** @brief Get monotonic milliseconds for a whole-operation deadline.
 * @return Native monotonic milliseconds, zero on unavailable clock. No allocation,
 * mutation or ownership; thread-safe. Add at most60000 without unsigned overflow.
 */
uint64_t venus_tcp_now_ms(void);
/** @brief Initialize a nonblocking connection to an admitted literal address.
 * @param[in,out] socket Nonnull empty caller owner; remains empty on failure.
 * @param[in] host Nonnull borrowed127.0.0.1 or10.0.2.2 NUL-terminated literal.
 * @param[in] port Explicit1..65535 TCP port. @param[in] deadline_ms Absolute
 * monotonic deadline, never extended after partial progress or interruption.
 * @param[in] cancel Nullable borrowed release-set atomic flag, alive for call.
 * @return Ok; Invalid arguments/live owner; Cancelled/Timeout; Closed native error.
 * Sole owner thread, no heap; success owns one socket/reference until close.
 */
venus_ring_status_t venus_tcp_socket_connect(venus_tcp_socket_t *socket, const char *host,
    uint32_t port, uint64_t deadline_ms, const _Atomic uint32_t *cancel);
/** @brief Initialize a private loopback-only nonblocking listener.
 * @param[in,out] socket Nonnull empty owner; unchanged on failure.
 * @param[in] port0 selects ephemeral native test port, otherwise1..65535.
 * @param[out] bound_port Nonnull disjoint owned port, zeroed on error.
 * @return Ok, Invalid or Closed. Sole owner thread; no heap or retained pointers.
 * Listener owns native handle/reference and requires idempotent close.
 */
venus_ring_status_t venus_tcp_socket_listen(venus_tcp_socket_t *socket, uint32_t port, uint32_t *bound_port);
/** @brief Accept one socket before an immutable whole-operation deadline.
 * @param[in] listener Nonnull live borrowed private listener; not consumed.
 * @param[in,out] socket Nonnull disjoint empty owner, unchanged on failure.
 * @param[in] deadline_ms Absolute monotonic deadline. @param[in] cancel Nullable
 * borrowed atomic cancellation flag. @return Ok, Invalid, Closed, Cancelled or Timeout.
 * Sole owner thread, no heap; accepted socket owns its own balanced Winsock reference.
 */
venus_ring_status_t venus_tcp_socket_accept(const venus_tcp_socket_t *listener,
    venus_tcp_socket_t *socket, uint64_t deadline_ms, const _Atomic uint32_t *cancel);
/** @brief Send all immutable private bytes under a single deadline.
 * @param[in,out] socket Nonnull live owner, borrowed for call, never closed here.
 * @param[in] bytes Nullable only for length zero, borrowed accessible bytes[length].
 * @param[in] length Actual bounded extent. @param[in] deadline_ms Absolute monotonic deadline.
 * @param[in] cancel Nullable atomic borrowed flag. @return Ok, Invalid, Closed,
 * Cancelled or Timeout; partial send on error is terminal to protocol caller.
 * Sole owner thread; no heap, pointer retention or implicit deadline resets.
 */
venus_ring_status_t venus_tcp_socket_send(venus_tcp_socket_t *socket, const void *bytes,
    size_t length, uint64_t deadline_ms, const _Atomic uint32_t *cancel);
/** @brief Receive exact bytes into private staging, publishing progress on EOF/error.
 * @param[in,out] socket Nonnull live owner, never closed here. @param[out] bytes
 * Nullable only for length zero; owned buffer[length] may contain partial bytes on error.
 * @param[in] length Actual bounded extent. @param[out] received Nonnull owned count,
 * zeroed before argument checks. @param[in] deadline_ms Fixed absolute deadline.
 * @param[in] cancel Nullable atomic borrowed flag. @return Ok, Invalid, Closed,
 * Cancelled or Timeout. Closed with received zero is clean EOF before a new header;
 * Closed with partial bytes must be treated as truncated protocol by the caller.
 * Sole owner thread; no heap, retained pointer or retry after terminal protocol failure.
 */
venus_ring_status_t venus_tcp_socket_receive(venus_tcp_socket_t *socket, void *bytes,
    size_t length, size_t *received, uint64_t deadline_ms, const _Atomic uint32_t *cancel);
/** @brief Half-close writes after frontend unbind, retaining reads for retirement Ack.
 * @param[in,out] socket Nonnull live owner, not consumed. @return Ok, Invalid or Closed.
 * Sole owner thread; no allocation. Call once during orderly session retirement.
 */
venus_ring_status_t venus_tcp_socket_shutdown_write(venus_tcp_socket_t *socket);
/** @brief Close native socket/reference and reset owner before native release.
 * @param[in,out] socket Nullable empty/live owner, sole thread after I/O stops.
 * Idempotent; no allocation, no borrowed-pointer ownership or cancellation mutation.
 */
void venus_tcp_socket_close(venus_tcp_socket_t *socket);
/** @brief Fill bounded private token/nonce/session bytes from the system CSPRNG.
 * @param[out] bytes Nonnull owned bytes[length], unchanged only for invalid input;
 * callers scrub complete buffer after any error. @param[in] length1..32.
 * @return Ok, Invalid or Closed. Thread-safe; no heap or retained pointer.
 * Linux uses nonblocking getrandom; Windows owns/closes an explicit Microsoft primitive CNG RNG provider per call.
 */
venus_ring_status_t venus_tcp_random(void *bytes, size_t length);
/** @brief Volatile scrub of owned sensitive storage before deterministic release.
 * @param[in,out] bytes Nullable only for length zero, owned accessible buffer[length].
 * @param[in] length Exact owner extent. No allocation/retention; thread-safe on disjoint bytes.
 */
void venus_tcp_scrub(void *bytes, size_t length);
_Static_assert(sizeof(venus_tcp_client_hello_t) == 48, "TCP private hello ABI");
_Static_assert(sizeof(venus_tcp_server_hello_t) == 184, "TCP private server ABI");
#endif
