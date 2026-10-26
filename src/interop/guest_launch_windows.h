/**
 * @file guest_launch_windows.h
 * @brief Real Win32 implementation of the guest launch native table (Windows only).
 */
#ifndef WADDLE_GUEST_LAUNCH_WINDOWS_H
#define WADDLE_GUEST_LAUNCH_WINDOWS_H

#include "guest_launch.h"

/**
 * @brief Fills a native table with the Win32 implementation.
 * @param[out] table Nonnull caller-owned table; every function pointer and the context are set to the real
 *                   implementation, with context NULL.
 * @note Win32 only (compiled by mingw and MSVC; never by the Linux build). No job object is created or
 *       assigned, and no function waits for a launched process to exit.
 */
void guest_launch_windows_native(guest_launch_native_t *table);

#endif
