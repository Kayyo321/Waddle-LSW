/**
 * @file session_stop_command.h
 * @brief `waddle session stop` client entry point (IMPL_DESC 3.12.5).
 *
 * The implementation is session_stop_command.zig. It resolves the device through the CLI resolver,
 * then asks that device's session to stop over session.sock. Contract:
 * impl/seamless-interop/claude_45_percent/session_stop_contract.md.
 */
#ifndef WaddleSessionStopCommandH
#define WaddleSessionStopCommandH

#include <stddef.h>

#include "daemon_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Device resolver with the same contract as resolve_target_device in host.c.
 * @param[in] specified_device Device name, or NULL for the default device; borrowed for the call.
 * @param[out] out_dev Filled on success; written only when the function returns 0.
 * @param[out] out_socket_path Registry socket path of the device, NUL-terminated on success.
 * @param[in] socket_path_cap Capacity of out_socket_path in bytes, including the terminator.
 * @param[in] require_single Nonzero to refuse ambiguous defaults.
 * @return 0 on success, otherwise the resolver's own nonzero exit code after its diagnostic.
 */
typedef int (*waddle_session_resolve_fn)(const char *specified_device, device_info_t *out_dev,
                                         char *out_socket_path, size_t socket_path_cap, int require_single);

/**
 * @brief Run `waddle session ...`. Only `stop` is defined.
 * @param[in] argc Number of arguments after `session`, zero or more.
 * @param[in] argv Arguments after `session`, readable for argc entries; NULL only when argc is 0.
 * @param[in] resolve_fn Non-NULL resolver.
 * @return 0 stopped or no session running, 1 unconfirmed or refused, 2 usage, otherwise the
 *         resolver's exit code, or 125 when the resolver is missing.
 * @note Not thread-safe: it uses process stdio and the clock. Single caller per process.
 */
int waddle_session_command(int argc, char **argv, waddle_session_resolve_fn resolve_fn);

#ifdef __cplusplus
}
#endif

#endif
