/** @file integration_commands.h
 * @brief C entry point for `waddle integration` (per-user desktop-entry handler).
 */
#ifndef WaddleIntegrationCommandsH
#define WaddleIntegrationCommandsH

/**
 * @brief Run one `waddle integration` command.
 * @param[in] argc Number of operands after the `integration` word, 0..64.
 * @param[in] argv Non-null borrowed array of argc non-null NUL-terminated strings.
 * @return 0 success or drift reported, 1 blocked or failed, 2 usage, 125 resources.
 * @note Single-threaded entry point. Resolves HOME and XDG paths from the environment,
 * reads the selected device's integration.exe_handler policy from the daemon registry,
 * and writes text or one JSON object to stdout (errors to stderr). Retains no argv pointers.
 */
int waddle_integration_command(int argc, char **argv);

#endif
