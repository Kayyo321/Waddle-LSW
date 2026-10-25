#ifndef WaddleOpenCommandH
#define WaddleOpenCommandH
/** @file open_command.h @brief `waddle open` client: request parsing, path translation, session launch. */
#include "daemon_device.h"

/**
 * @brief Delegated exec entry, the cmd_exec function of host.c.
 * @param[in] argc Argument count, including the program name at argv[0].
 * @param[in] argv Borrowed argument array; argv[1] is the subcommand word.
 * @param[in] start_opt Index at which option scanning starts.
 * @return Process exit code of the delegated command.
 */
typedef int (*waddle_open_exec_fn)(int argc, char **argv, int start_opt);

/**
 * @brief Device resolution entry, the resolve_target_device function of host.c.
 * @param[in] specified_device Nullable device name; NULL selects the default or the only device.
 * @param[out] out_dev Nullable device record; written on success.
 * @param[out] out_socket_path Device socket path buffer.
 * @param[in] socket_path_cap Capacity of out_socket_path.
 * @param[in] require_single Nonzero to require one device when none is named.
 * @return 0 on success; a nonzero exit code after printing the reason.
 */
typedef int (*waddle_open_resolve_fn)(const char *specified_device, device_info_t *out_dev,
                                      char *out_socket_path, size_t socket_path_cap, int require_single);

/**
 * @brief Run `waddle open` with the arguments that follow the `open` word.
 * @param[in] argc Number of arguments after `open`, 0..64 valid.
 * @param[in] argv Borrowed NUL-terminated arguments, readable for argc entries.
 * @param[in] exec_fn Non-NULL delegate used for console executables routed to `waddle exec`.
 * @param[in] resolve_fn Non-NULL device resolver with the host.c selection rules.
 * @return Exit code: 0, 1, 2, 125, 126 or 127 as specified in open_request_contract.md.
 * @note Single-threaded entry. Allocates through libc malloc and frees every allocation
 * before returning. Writes diagnostics to stderr; writes nothing on success.
 */
int waddle_open_command(int argc, char **argv, waddle_open_exec_fn exec_fn, waddle_open_resolve_fn resolve_fn);

#endif
