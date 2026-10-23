/**
 * @file daemon_qmp.h
 * @brief Lightweight C client for QEMU Machine Protocol (QMP) JSON-RPC.
 *
 * Provides capabilities handshake, VM status query, ACPI powerdown, and quit
 * commands over local UNIX domain sockets without external JSON dependencies.
 */

#ifndef WaddleDaemonQmpH
#define WaddleDaemonQmpH

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum length of QMP JSON response buffer in bytes. */
#define QmpMaxResponseLen UINT32_C(4096)

/** @brief Default QMP socket timeout in milliseconds. */
#define QmpDefaultTimeoutMs UINT32_C(3000)

/**
 * @brief Handle representing a connected QMP session.
 */
typedef struct qmp_client_t {
    /** @brief Connected UNIX domain socket descriptor (-1 if closed). */
    int socket_fd;
    /** @brief Default I/O timeout in milliseconds. */
    uint32_t timeout_ms;
    /** @brief True if qmp_capabilities handshake has succeeded. */
    bool capabilities_negotiated;
} qmp_client_t;

/**
 * @brief Connects to a QEMU QMP UNIX domain socket and consumes the greeting banner.
 *
 * @param[out] client      Non-null pointer to qmp_client_t structure to initialize.
 * @param[in]  socket_path Path to QMP UNIX domain socket.
 * @param[in]  timeout_ms  Timeout in milliseconds for connection and greeting read.
 * @return 0 on success, or -1 on connection/I/O error (errno set).
 */
int qmp_connect(qmp_client_t *client, const char *socket_path, uint32_t timeout_ms);

/**
 * @brief Performs QMP capability negotiation handshake.
 *
 * Dispatches {"execute": "qmp_capabilities"} and verifies {"return": {}}.
 *
 * @param[in,out] client Non-null pointer to connected qmp_client_t.
 * @return 0 on success, or -1 on protocol/handshake failure.
 */
int qmp_negotiate_capabilities(qmp_client_t *client);

/**
 * @brief Sends an ACPI system_powerdown request to the VM.
 *
 * Triggers standard ACPI power button event in guest operating system.
 *
 * @param[in,out] client Non-null pointer to connected qmp_client_t.
 * @return 0 on success, or -1 on error.
 */
int qmp_system_powerdown(qmp_client_t *client);

/**
 * @brief Sends an immediate quit command to QEMU.
 *
 * @param[in,out] client Non-null pointer to connected qmp_client_t.
 * @return 0 on success, or -1 on error.
 */
int qmp_quit(qmp_client_t *client);

/**
 * @brief Queries current execution status of QEMU.
 *
 * @param[in,out] client     Non-null pointer to connected qmp_client_t.
 * @param[out]    is_running Non-null pointer set to 1 if VM status is running, 0 otherwise.
 * @return 0 on success, or -1 on error.
 */
int qmp_query_status(qmp_client_t *client, int *is_running);

/**
 * @brief Closes QMP connection and resets client state.
 *
 * @param[in,out] client Pointer to qmp_client_t. Safe to call with uninitialized or closed client.
 */
void qmp_close(qmp_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* WaddleDaemonQmpH */
