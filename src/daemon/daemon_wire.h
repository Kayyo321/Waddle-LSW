/** @file daemon_wire.h
 * @brief Allocation-free, bounds-checked daemon framing and readiness codec.
 */
#ifndef WaddleDaemonWireH
#define WaddleDaemonWireH
#include "waddle/daemon_protocol.h"

/** @brief Decode one exact 16-byte little-endian daemon header.
 * @param[in] bytes Borrowed readable length-byte range; NULL is rejected.
 * @param[in] length Must equal WaddleDaemonHeaderSize.
 * @param[out] out_header Non-null aligned writable header, disjoint from bytes.
 * @return 0 success, -1 invalid input; output is unchanged on failure.
 * @note No allocation or retention; thread-safe for disjoint output storage.
 */
int daemon_wire_decode_header(const uint8_t *bytes, size_t length,
                              waddle_daemon_header_t *out_header);

/** @brief Decode the exact four-byte GuestReady timeout and apply its default.
 * @param[in] bytes Borrowed readable length-byte range; NULL is rejected.
 * @param[in] length Must equal four.
 * @param[out] out_timeout Non-null writable u32, disjoint from bytes.
 * @return 0 success (1..120000 ms), -1 invalid input; output unchanged on failure.
 * @note No allocation, retention or shared state; thread-safe.
 */
int daemon_wire_decode_ready_request(const uint8_t *bytes, size_t length,
                                     uint32_t *out_timeout);

/** @brief Decode an exact eight-byte validated GuestReady response.
 * @param[in] bytes Borrowed readable length-byte range; NULL is rejected.
 * @param[in] length Must equal eight.
 * @param[out] out_response Non-null writable response, disjoint from bytes.
 * @return 0 success, -1 invalid status/CID/length; output unchanged on failure.
 * @note No allocation, retention or shared state; thread-safe.
 */
int daemon_wire_decode_ready_response(const uint8_t *bytes, size_t length,
                                      waddle_daemon_guest_ready_resp_t *out_response);

/** @brief Decode the errno of an exact legacy 264-byte ErrorResp.
 * @param[in] bytes Borrowed readable length-byte range, non-null.
 * @param[in] length Must equal 264; trailing legacy diagnostic bytes remain opaque.
 * @param[out] out_error Non-null aligned u32, disjoint from bytes.
 * @return 0 for errno1..4095, -1 otherwise with output unchanged.
 * @note No allocation, retained pointers or shared state; thread-safe.
 */
int daemon_wire_decode_error_response(const uint8_t *bytes, size_t length,
                                      uint32_t *out_error);
/** @brief Decode one exact legacy status response for session admission.
 * @param[in] bytes Non-null borrowed readable payload of exactly 8560 bytes.
 * @param[in] length Exact payload size; no trailing bytes are permitted.
 * @param[out] out_response Non-null aligned writable value, disjoint from bytes.
 * @return 0 success; -1 malformed length/state/CID/port/resource/mount data,
 * leaving output unchanged. State0..6, CID3..UINT32_MAX-1, nonzero port,
 * memory512..65536 MiB, vCPU1..128, count0..8, reserved zero are required.
 * Active mount strings must be nonempty/NUL-terminated, readonly0/1, reserved0.
 * @note Copies validated fields; inactive mounts zero. No path authority is
 * inferred from copied strings. Allocation-free, no retained pointer or shared
 * state; thread-safe for disjoint outputs. Caller validates header correlation.
 */
int daemon_wire_decode_status_response(const uint8_t *bytes, size_t length,
                                      waddle_daemon_status_resp_t *out_response);
#endif
