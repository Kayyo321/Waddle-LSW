/** @file device_commands.h
 * @brief Memory-safe host device command parser and structured result writer.
 */
#ifndef WaddleDeviceCommandsH
#define WaddleDeviceCommandsH

/**
 * @brief Run a device command through the Zig parser.
 * @param[in] argc Number of arguments starting with the subcommand, 0..128.
 * @param[in] argv Non-null borrowed array of argc non-null NUL-terminated strings.
 * @return 0 success, 2 usage/validation, 1 operational failure, 125 resource failure.
 * @note Single-threaded command entry point. No argv pointers retained; every temporary
 * allocation is released before return. Output is UTF-8 text or one v1 JSON object.
 * Registry/default APIs acquire their own locks; caller must hold no registry lock.
 */
int waddle_device_command(int argc, char **argv);

#endif
