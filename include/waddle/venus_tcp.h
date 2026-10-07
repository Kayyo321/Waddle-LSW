/** @file venus_tcp.h @brief Bounded authenticated test TCP protocol, separate from IVSHMEM. */
#ifndef WaddleVenusTcpH
/** @brief Include guard, no storage or ownership. */
#define WaddleVenusTcpH
#include "venus_request.h"
#include "venus_capabilities.h"
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
/** @brief Maximum absolute Windows filename extent, including its final NUL. */
#define VenusTcpMaxWindowsPathBytes 1024u
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
    char icd_path[VenusTcpMaxWindowsPathBytes]; /**< Owned NUL-terminated absolute ASCII Windows path. */
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
/** @brief Validate one exact bounded absolute ASCII Windows filename.
 * @param[in] bytes Nonnull immutable accessible private array[length], borrowed for call.
 * @param[in] length Actual1..1024 bytes including its final NUL; no earlier NUL.
 * @return Ok; Invalid null/zero extent; Limit oversized extent; Corrupt missing/interior
 * terminator, empty/non-ASCII filename or relative/malformed drive/UNC path.
 * @note Allocation-free/thread-safe for immutable storage; no mutation, ownership or retention.
 * Reuses the strict config filename grammar; native file admission remains bootstrap-owned.
 */
venus_ring_status_t venus_tcp_windows_path_validate(const void *bytes, size_t length);
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
    uint32_t received_eof; /**< Last receive observed actual native recv0; reset per call, never inferred from error. */
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
 * Cancelled or Timeout. Last-receive received_eof is reset before checks and set only by actual recv0.
 * Closed with received zero AND received_eof1 is clean EOF before a new header;
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
/** @brief Caller-owned noncopyable sequential client, initialized from zero.
 * No hidden heap/thread; fixed private staging and authoritative host snapshot.
 * All fields are private/read-only to callers. Socket and record remain alive
 * while a bound ICD borrows the callback; retirement/abandon precedes free.
 */
typedef struct venus_tcp_client_t {
    venus_tcp_socket_t socket; /**< Owned native socket until terminal/retirement/free. */
    uint64_t session; /**< Copied nonzero authenticated identity, retained after socket loss. */
    uint64_t next_sequence; /**< Next nonzero outer identity; UINT64_MAX means exhausted. */
    uint32_t timeout_ms; /**< Immutable whole exchange maximum1..60000ms. */
    venus_ring_status_t lost; /**< Sticky terminal failure, RingOk while usable. */
    venus_capabilities_t capabilities; /**< Copied authoritative actual negotiated host snapshot. */
    uint8_t tx[VenusTcpMaxCommandBytes]; /**< Exclusive private submission staging. */
    uint8_t rx[VenusTcpMaxReplyBytes]; /**< Exclusive private response staging. */
} venus_tcp_client_t;
/** @brief Authenticate one actual host session before callback binding/discovery.
 * @param[in,out] client Nonnull empty caller record, unchanged on failure.
 * @param[in] config Nonnull immutable validated private config, borrowed only for call.
 * @param[in] cancel Nullable borrowed release-set flag, alive through call only.
 * @return Ok; Invalid local arguments/live owner; existing protocol/transport status.
 * Sole owner thread; one fixed5s handshake deadline, no heap or retained config/token.
 * Success owns socket and copied capabilities; call retire/free only after ICD quiescence.
 */
venus_ring_status_t venus_tcp_client_init(venus_tcp_client_t *client,
    const venus_tcp_config_t *config, const _Atomic uint32_t *cancel);
/** @brief Actual framed callback with exact existing command exchange signature.
 * @param[in,out] context Nonnull initialized exclusive client, borrowed while ICD bound.
 * @param[in] request Nonnull sequence-zero immutable decoded request, admitted subset.
 * @param[in] input Nullable for length zero, borrowed private input[length].
 * @param[in] length Exact payload extent. @param[out] response Nonnull disjoint
 * private record, zeroed on failure after alias validation. @param[out] output
 * Nullable only for capacity zero; unchanged until complete valid successful response.
 * @param[in] capacity Exact successful read/reply capacity, at most4096; zero otherwise.
 * @return Actual receiver Ring status; local Invalid before publication; terminal
 * network/protocol loss is sticky. Sole thread; no input retention/heap/reentrancy.
 * Input/output may alias each other; no argument overlaps owner/request/response.
 */
venus_ring_status_t venus_tcp_client_exchange(void *context, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity);
/** @brief Framed callback with an additional active-call cancellation borrow.
 * @param[in,out] client Nonnull exclusive initialized owner. Other buffer parameters
 * match venus_tcp_client_exchange exactly. @param[in] cancel Nullable release-set
 * atomic flag borrowed only through this call. @return Same callback status.
 * No pointer retention/thread/heap; use this from an owned bootstrap adapter.
 */
venus_ring_status_t venus_tcp_client_exchange_cancel(venus_tcp_client_t *client,
    const venus_request_t *request, const void *input, size_t length,
    venus_request_t *response, void *output, size_t capacity, const _Atomic uint32_t *cancel);
/** @brief Await actual host retirement after successful quiescent ICD unbind.
 * @param[in,out] client Nonnull live exclusive owner, application callbacks stopped.
 * @param[in] cancel Nullable release-set atomic flag borrowed through call.
 * @return Ok only for validated established-session retirement Ack; otherwise
 * sticky loss with no claim of worker retirement. No heap or retained pointer.
 * Success closes socket, preserves public accounting identity and marks Closed.
 */
venus_ring_status_t venus_tcp_client_retire(venus_tcp_client_t *client, const _Atomic uint32_t *cancel);
/** @brief Close/scrub/reset owner after trusted retirement or unbound standalone use.
 * @param[in,out] client Nullable empty/live owner; callbacks must already stop.
 * Caller must unbind, or externally verify exact host retirement then abandon,
 * before freeing an ICD-borrowed context. Idempotent, no heap/thread or fake Ack.
 */
void venus_tcp_client_free(venus_tcp_client_t *client);
/** @brief Borrowed actual negotiated frontend type; complete definition in venus_guest.h. */
typedef struct venus_guest_t venus_guest_t;
/** @brief Sole-thread authenticated server owner, zero-initialize and never copy.
 * No heap/thread; accepted socket belongs to this owner after authentication.
 * Actual guest/worker/controller remain externally owned and must retire before Ack.
 */
typedef struct venus_tcp_server_t {
    venus_tcp_socket_t socket; /**< Owned accepted native connection. */
    uint64_t session; /**< Nonzero CSPRNG authenticated receiver identity. */
    uint64_t next_sequence; /**< Exact next outer request sequence, never wraps. */
    uint64_t handshake_deadline; /**< Original whole5s authentication/negotiation deadline. */
    uint32_t timeout_ms; /**< Immutable1..60000ms whole operation budget. */
    uint32_t ready; /**< One only after actual capability/profile negotiation Ack. */
    uint32_t eof; /**< One for clean pre-header EOF, socket retained for actual retirement Ack. */
    venus_ring_status_t lost; /**< Sticky terminal result, RingOk while usable. */
    uint8_t nonce[16]; /**< Copied client nonce echoed once; no borrowed pointer. */
    uint8_t tx[VenusTcpMaxCommandBytes]; /**< Private acquired receiver-command staging. */
    uint8_t rx[VenusTcpMaxReplyBytes]; /**< Private actual receiver-response staging. */
} venus_tcp_server_t;
/** @brief Admit exactly one token-authenticated client before creating any worker.
 * @param[in,out] server Nonnull empty owner, unchanged on failure.
 * @param[in,out] accepted Nonnull disjoint live socket; consumed/zeroed after
 * valid local argument admission, including peer authentication failure.
 * @param[in] token Nonnull borrowed private32-byte launch capability, never retained/logged.
 * @param[in] timeout_ms Whole operation maximum1..60000ms.
 * @param[in] cancel Nullable atomic flag, borrowed for call. @return Existing Ring status.
 * No heap/thread; success owns socket/session/nonce, failure closes moved socket.
 */
venus_ring_status_t venus_tcp_server_authenticate(venus_tcp_server_t *server,
    venus_tcp_socket_t *accepted, const uint8_t *token, uint32_t timeout_ms,
    const _Atomic uint32_t *cancel);
/** @brief Send actual compatible receiver capset and validate exact guest declaration.
 * @param[in,out] server Nonnull authenticated unnegotiated sole-thread owner.
 * @param[in] guest Nonnull live actual negotiated frontend, borrowed only for call.
 * @param[in] capabilities Nonnull immutable captured actual160-byte wire snapshot,
 * must exactly match decoded guest capabilities. @param[in] cancel Nullable borrowed atomic flag.
 * @return Existing Ring status; failure is terminal and closes socket, no fabricated capset.
 * Uses remaining original5s handshake deadline; no allocation or retained guest pointer.
 */
venus_ring_status_t venus_tcp_server_negotiate(venus_tcp_server_t *server,
    const venus_guest_t *guest, const uint8_t *capabilities, const _Atomic uint32_t *cancel);
/** @brief Forward at most one exact framed request to the actual borrowed guest.
 * @param[in,out] server Nonnull ready exclusive owner. @param[in,out] guest
 * Nonnull live negotiated actual frontend; configured timeout must cover server maximum.
 * @param[in] cancel Nullable borrowed atomic flag, alive through call only;
 * when nonnull must match the actual guest RPC channel cancellation owner.
 * @return Ok for completed framed ordinary result; Closed/eof1 with retained socket
 * for clean pre-header EOF; otherwise sticky terminal status and closed socket.
 * Entire acquisition, guest remaining RPC budget and response use one deadline.
 * No heap/thread/retained guest or payload pointer; never synthesizes receiver replies.
 */
venus_ring_status_t venus_tcp_server_step(venus_tcp_server_t *server,
    venus_guest_t *guest, const _Atomic uint32_t *cancel);
/** @brief Acknowledge actual externally proven controller retirement, then close socket.
 * @param[in,out] server Nonnull clean-eof owner; callbacks/app calls already stopped.
 * @param[in] cancel Nullable call-borrowed atomic flag. @return Existing Ring status.
 * Caller MUST already stop/reap exact actual worker and release guest/channel/mapping
 * ownership; this function does not retire a renderer and must never fake that proof.
 * Sole thread, no heap; Ack uses established identity under a fresh configured deadline.
 */
venus_ring_status_t venus_tcp_server_ack_retired(venus_tcp_server_t *server,
    const _Atomic uint32_t *cancel);
/** @brief Close/scrub/reset socket owner, independently of borrowed actual controller.
 * @param[in,out] server Nullable zero/live owner, sole thread after calls stop.
 * Idempotent; no heap/thread or implicit worker retirement/acknowledgment.
 */
void venus_tcp_server_free(venus_tcp_server_t *server);
_Static_assert(sizeof(venus_tcp_client_hello_t) == 48, "TCP private hello ABI");
_Static_assert(sizeof(venus_tcp_server_hello_t) == 184, "TCP private server ABI");
/** @brief Start one explicitly controlled Windows TCP-to-ICD binding before Vulkan discovery.
 * @param[in] path Nonnull immutable private filename array[bytes], borrowed for call only.
 * @param[in] bytes Exact1..1024 extent including final NUL; bounded absolute ASCII grammar.
 * @return Ok bound; Again existing owned phase; Invalid local filename/privacy/binding;
 * Closed native acquisition/release; existing parser/socket/handshake/retirement statuses.
 * @note Windows x64 only. Owns fixed callback/socket and one exact loaded ICD reference;
 * resolves additive capability binding only. Caller serializes lifecycle, starts before
 * application calls and keeps bootstrap DLL loaded until stop/abandon succeeds. No DllMain I/O.
 * Native file/token/security-descriptor release failures retain SDK owners for stop retry.
 * Failure after admitted session can retain ownership: inspect session and complete stop or
 * trusted abandonment; no native path/config/token pointer is retained or logged.
 */
venus_ring_status_t venus_tcp_bootstrap_start(const char *path, size_t bytes);
/** @brief Unbind a quiescent Windows session and release only after exact retirement Ack.
 * @return Ok empty/released; Again live ICD objects; existing unbind/transport/retirement
 * status retaining session/module; Closed failed owned module release, retryable after retirement.
 * @note No arguments/allocation. Caller stops new Vulkan calls and waits callbacks first,
 * serializes lifecycle and retains bootstrap DLL. Successful unbind precedes TCP half-close;
 * failed Ack or FreeLibrary never discards session accounting or module ownership.
 */
venus_ring_status_t venus_tcp_bootstrap_stop(void);
/** @brief Return copied authenticated Windows session identity, zero while no session is owned.
 * @return Nonzero stable accounting identity until release; zero with no admitted session,
 * including SDK-only failed-start phases. Zero never proves every native owner was released.
 * @note No allocation, mutation or ownership transfer. Caller serializes lifecycle/getter;
 * this value does not itself prove actual host receiver retirement.
 */
uint64_t venus_tcp_bootstrap_session(void);
/** @brief Abandon only after trusted supervision proves exact host receiver retirement.
 * @param[in] retired_session Nonzero identity matching this owner's copied authenticated session.
 * @return Ok released; Invalid zero/mismatch/empty before mutation; Closed module release
 * failure retaining already retired module/session for retry. No wire-based retirement inference.
 * @note Caller must independently prove exact worker/group reap, stop all application/callback
 * use, discard virtual Vulkan handles, serialize lifecycle and retain bootstrap DLL. Invokes
 * resolved ICD abandon only for this owner's still-bound phase, then scrubs transport/releases
 * exact HMODULE. Unbound failed-start state never abandons an incumbent binding.
 */
venus_ring_status_t venus_tcp_bootstrap_abandon(uint64_t retired_session);
#endif
