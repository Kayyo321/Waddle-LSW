/**
 * @file common.h
 * @brief Common buffering, wire framing, and utility declarations for Waddle CLI.
 *
 * Provides queue_t for dynamic transmission buffering and decoder_t for incremental
 * stream decoding of wire frames.
 */

#ifndef WADDLE_COMMON_H
#define WADDLE_COMMON_H

#include "waddle/cli_protocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dynamic contiguous FIFO buffer for queuing outbound wire data.
 */
typedef struct queue_t {
    /** @brief Dynamically allocated buffer memory (capacity WaddleQueueSize). */
    uint8_t *data;
    /** @brief Read offset in bytes from the start of the buffer. */
    size_t off;
    /** @brief Current buffered payload length in bytes. */
    size_t len;
} queue_t;

/**
 * @brief Incremental stream decoder for inbound wire frames.
 */
typedef struct decoder_t {
    /** @brief Staging buffer for the 32-byte wire header. */
    uint8_t header[WaddleHeaderSize];
    /** @brief Dynamically allocated buffer holding the current message payload. */
    uint8_t *body;
    /** @brief Number of bytes currently read into header or body. */
    size_t have;
    /** @brief Expected payload length in bytes for current message. */
    size_t length;
    /** @brief Expected next sequence number for message ordering verification. */
    uint32_t next;
    /** @brief Message type of current frame (from waddle_cli_msg_type_t). */
    uint16_t type;
    /** @brief Non-zero if decoder has processed at least one frame. */
    int started;
} decoder_t;

/* Backward compatibility typedef aliases */
typedef queue_t queue;
typedef decoder_t decoder;

/**
 * @brief Initializes a transmission queue and allocates its internal buffer.
 *
 * @param[out] q Non-null pointer to queue_t to initialize.
 * @return 0 on success, or -1 on allocation failure (errno set to ENOMEM).
 * @note Allocates WaddleQueueSize bytes. Caller must eventually release via queue_free().
 */
int queue_init(queue_t *q);

/**
 * @brief Releases internal storage of a transmission queue and defensively clears its fields.
 *
 * @param[in,out] q Pointer to queue_t to free. Safe to call with NULL or zeroed struct.
 * @note Defensively frees and NULLs q->data.
 */
void queue_free(queue_t *q);

/**
 * @brief Computes available remaining space in the transmission queue.
 *
 * @param[in] q Non-null pointer to initialized queue_t.
 * @return Number of free bytes available for appending.
 */
size_t queue_space(const queue_t *q);

/**
 * @brief Appends contiguous byte data to the transmission queue.
 *
 * Automatically compacts queue memory if trailing capacity is insufficient but
 * total free space exists.
 *
 * @param[in,out] q Non-null pointer to initialized queue_t.
 * @param[in]     p Pointer to byte data to append. May be NULL if n is 0.
 * @param[in]     n Number of bytes to append.
 * @return 0 on success, or -1 if queue capacity is exceeded (errno set to ENOBUFS).
 */
int queue_append(queue_t *q, const void *p, size_t n);

/**
 * @brief Non-blockingly flushes queued bytes to a destination file descriptor.
 *
 * @param[in,out] q  Non-null pointer to initialized queue_t.
 * @param[in]     fd File descriptor open for non-blocking writes.
 * @return 0 on complete or partial flush, or -1 on write error (errno preserved).
 */
int queue_flush(queue_t *q, int fd);

/**
 * @brief Constructs and enqueues a complete wire protocol frame.
 *
 * Serializes the 32-byte header with sequence number, CRC-32 checksum, and payload.
 *
 * @param[in,out] q    Non-null pointer to initialized queue_t.
 * @param[in,out] seq  Non-null pointer to sequence counter; incremented on success.
 * @param[in]     type Message type identifier (from waddle_cli_msg_type_t).
 * @param[in]     p    Pointer to payload data to transmit. May be NULL if n is 0.
 * @param[in]     n    Byte length of payload data. Must not exceed WaddleMaxPayloadSize.
 * @return 0 on success, or -1 on buffer exhaustion or invalid size (errno set).
 */
int wire_send(queue_t *q, uint32_t *seq, uint16_t type, const void *p, size_t n);

/**
 * @brief Formats and enqueues a multiplexed standard I/O stream chunk.
 *
 * Wraps payload in a waddle_msg_stream_data_t payload and calls wire_send().
 *
 * @param[in,out] q      Non-null pointer to initialized queue_t.
 * @param[in,out] seq    Non-null pointer to sequence counter.
 * @param[in]     stream Stream identifier (WaddleStreamStdin, Stdout, or Stderr).
 * @param[in]     p      Pointer to stream data to enqueue.
 * @param[in]     n      Byte length of stream data. Must not exceed WaddleChunkSize.
 * @return 0 on success, or -1 on error.
 */
int wire_stream(queue_t *q, uint32_t *seq, unsigned stream, const void *p, size_t n);

/**
 * @brief Formats and enqueues an end-of-file notification for a stream.
 *
 * @param[in,out] q      Non-null pointer to initialized queue_t.
 * @param[in,out] seq    Non-null pointer to sequence counter.
 * @param[in]     stream Stream identifier reaching EOF.
 * @return 0 on success, or -1 on error.
 */
int wire_eof(queue_t *q, uint32_t *seq, unsigned stream);

/**
 * @brief Non-blockingly reads and parses the next wire frame from a socket.
 *
 * @param[in,out] d  Non-null pointer to decoder_t.
 * @param[in]     fd File descriptor open for non-blocking reads.
 * @retval 1  A complete valid frame was parsed (available in d->header and d->body).
 * @retval 0  Operation would block; more data needed.
 * @retval -1 Protocol or I/O error occurred (errno set).
 * @retval -2 Clean peer transport EOF before any partial frame was received.
 */
int wire_read(decoder_t *d, int fd);

/**
 * @brief Releases the current message payload and advances decoder sequence state.
 *
 * @param[in,out] d Non-null pointer to decoder_t.
 * @note Defensively frees and NULLs d->body.
 */
void wire_consume(decoder_t *d);

/**
 * @brief Completely resets decoder state and releases any allocated payload memory.
 *
 * @param[in,out] d Non-null pointer to decoder_t.
 * @note Defensively frees and NULLs d->body.
 */
void wire_destroy(decoder_t *d);

/**
 * @brief Sets a file descriptor to non-blocking mode.
 *
 * @param[in] fd Valid open file descriptor.
 * @return 0 on success, or -1 on fcntl failure.
 */
int nonblock(int fd);

/**
 * @brief Returns the current monotonic clock timestamp in milliseconds.
 *
 * @return Monotonic millisecond timestamp, or 0 on system clock failure.
 */
uint64_t monotonic_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_COMMON_H */
