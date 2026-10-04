/** @file guest_wire.h @brief Serialized Winsock frame transport. */
#ifndef WaddleGuestWireH
/** @brief Include guard; compile-time marker with no storage or ownership. */
#define WaddleGuestWireH
#include <winsock2.h>
#include <windows.h>
#include "guest_codec.h"
/** @brief Session-owned wire state; socket borrowed from listener until workers join.
 * @note One receive thread, multiple send threads; lock covers entire frames. */
typedef struct guest_wire_t {
    SOCKET socket; /**< Borrowed connected socket, shutdown on failure. */
    HANDLE failure; /**< Owned manual-reset failure event. */
    CRITICAL_SECTION send_lock; /**< Owned send serialization lock. */
    int cancellable; /**< Main enables before workers start; receive waits check cancellation. */
    volatile LONG stopping; /**< Atomic receive cancellation flag, main sets before shutdown. */
    uint32_t incoming; /**< Receive-thread-only sequence, starts at one. */
    uint32_t outgoing; /**< Send-lock-protected sequence, starts at one. */
} guest_wire_t;
/** @brief Initialize transport state, single caller.
 * @param[out] wire Nonnull caller-owned record, destroy after all workers join.
 * @param[in] socket Borrowed connected blocking Winsock socket.
 * @return 0 success, -1 Win32 event allocation failure, GetLastError preserved. */
int guest_wire_init(guest_wire_t *wire, SOCKET socket);
/** @brief Destroy event and lock, after all threads join; does not close socket.
 * @param[in,out] wire Nonnull successfully initialized record; main thread only. */
void guest_wire_close(guest_wire_t *wire);
/** @brief Signal session failure and wake blocked socket operations; thread-safe.
 * @param[in,out] wire Nonnull initialized session; no ownership transfer. */
void guest_wire_fail(guest_wire_t *wire);
/** @brief Cancel receives during orderly teardown, preserving outgoing drains.
 * @param[in,out] wire Nonnull initialized session; thread-safe and idempotent. */
void guest_wire_stop_input(guest_wire_t *wire);
/** @brief Receive and validate a header before payload allocation; sole receive thread.
 * @param[in,out] wire Nonnull initialized session.
 * @param[out] frame Nonnull caller-owned validated result.
 * @return 0 success, -1 malformed frame or socket closure/error; sets failure event. */
int guest_receive_header(guest_wire_t *wire, guest_frame_t *frame);
/** @brief Receive payload and validate CRC, advancing sequence; sole receive thread.
 * @param[in,out] wire Nonnull initialized session.
 * @param[in] frame Nonnull validated current header, retained until return.
 * @param[out] body Nonnull caller-owned writable buffer, at least frame->length bytes.
 * @return 0 success, -1 socket/CRC failure; sets failure event. */
int guest_receive_body(guest_wire_t *wire, const guest_frame_t *frame, uint8_t *body);
/** @brief Send one frame atomically with sequence assignment; thread-safe.
 * @param[in,out] wire Nonnull initialized session.
 * @param[in] type Guest message type.
 * @param[in] body Borrowed readable length-byte buffer; NULL only for zero length.
 * @param[in] length Payload bytes, at most 1 MiB.
 * @return 0 success, -1 socket/size/session failure; wakes peers on I/O failure. */
int guest_send(guest_wire_t *wire, uint16_t type, const uint8_t *body, size_t length);
#endif
