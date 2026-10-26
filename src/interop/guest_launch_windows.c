/**
 * @file guest_launch_windows.c
 * @brief Win32 table for the guest launch executor: ShellExecuteExW, CreateProcessW, AUMID seam, CloseHandle.
 *
 * Every launch is a single native call that returns as soon as the target process exists. Nothing here waits for
 * the launched process, assigns it to a job, or ties it to the connection. Handles are closed by the caller
 * through close_process, except where a function documents that it closed its own handle.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#include "guest_launch_windows.h"

/** @brief Converts UTF-8 to a NUL-terminated UTF-16 buffer of `capacity` units. Returns 0 on success. */
static int utf8_to_wide(const char *text, wchar_t *out, size_t capacity) {
    int written;
    if (capacity == 0u) return 1;
    written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, (int)capacity);
    return written == 0 ? 1 : 0;
}

/** @brief Converts into a freshly allocated buffer sized from the UTF-8 length (UTF-16 units never exceed bytes). */
static wchar_t *utf8_to_wide_alloc(const char *text) {
    size_t capacity = strlen(text) + 1u;
    wchar_t *out = (wchar_t *)calloc(capacity, sizeof(wchar_t));
    if (out == NULL) return NULL;
    if (utf8_to_wide(text, out, capacity) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

static void set_error(guest_launch_native_result_t *result, DWORD error) {
    result->error = error != 0u ? (uint32_t)error : 1u;
    result->process_id = 0u;
    result->process = NULL;
}

static void win_shell_execute(void *context, const guest_launch_shell_t *shell, guest_launch_native_result_t *result) {
    /* Stack buffers sized to the executor's bounds; the executor checked every length before calling. */
    wchar_t file[GuestLaunchMaxText + 64u];
    wchar_t parameters[GuestLaunchMaxCommandLine + 1u];
    wchar_t directory[GuestLaunchMaxText + 1u];
    SHELLEXECUTEINFOW info;
    (void)context;
    if (utf8_to_wide(shell->file, file, sizeof(file) / sizeof(file[0])) != 0) {
        set_error(result, ERROR_INVALID_PARAMETER);
        return;
    }
    if (shell->parameters != NULL && utf8_to_wide(shell->parameters, parameters, sizeof(parameters) / sizeof(parameters[0])) != 0) {
        set_error(result, ERROR_INVALID_PARAMETER);
        return;
    }
    if (shell->directory != NULL && utf8_to_wide(shell->directory, directory, sizeof(directory) / sizeof(directory[0])) != 0) {
        set_error(result, ERROR_INVALID_PARAMETER);
        return;
    }
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = file;
    info.lpParameters = shell->parameters != NULL ? parameters : NULL;
    info.lpDirectory = shell->directory != NULL ? directory : NULL;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        set_error(result, GetLastError());
        return;
    }
    result->error = 0u;
    result->process = info.hProcess;
    result->process_id = info.hProcess != NULL ? (uint32_t)GetProcessId(info.hProcess) : 0u;
}

static void win_create_console(void *context, const char *command_line, const char *directory, guest_launch_native_result_t *result) {
    wchar_t *command;
    wchar_t *wide_directory = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    BOOL created;
    (void)context;
    command = utf8_to_wide_alloc(command_line);
    if (command == NULL) {
        set_error(result, ERROR_INVALID_PARAMETER);
        return;
    }
    if (directory != NULL) {
        wide_directory = utf8_to_wide_alloc(directory);
        if (wide_directory == NULL) {
            free(command);
            set_error(result, ERROR_INVALID_PARAMETER);
            return;
        }
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    created = CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, wide_directory, &startup, &process);
    if (!created) {
        set_error(result, GetLastError());
    } else {
        CloseHandle(process.hThread);
        result->error = 0u;
        result->process = process.hProcess;
        result->process_id = (uint32_t)process.dwProcessId;
    }
    free(wide_directory);
    free(command);
}

/**
 * @brief Packaged-app activation through explorer.exe. Explorer accepts the request and starts the app; the
 *        activated process is not known, so the result carries process id 0 and no handle. The process handle of
 *        explorer.exe itself is closed here. Arguments cannot be passed through an AppsFolder item: refused.
 */
static void win_activate_aumid(void *context, const char *aumid, const char *arguments, guest_launch_native_result_t *result) {
    static const char Prefix[] = "explorer.exe shell:AppsFolder\\";
    char command_utf8[sizeof(Prefix) + GuestLaunchMaxText];
    wchar_t *command;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    (void)context;
    if (arguments != NULL && arguments[0] != '\0') {
        set_error(result, ERROR_NOT_SUPPORTED);
        return;
    }
    if (strchr(aumid, ' ') != NULL || strchr(aumid, '"') != NULL) {
        set_error(result, ERROR_INVALID_NAME);
        return;
    }
    memcpy(command_utf8, Prefix, sizeof(Prefix) - 1u);
    strcpy(command_utf8 + sizeof(Prefix) - 1u, aumid);
    command = utf8_to_wide_alloc(command_utf8);
    if (command == NULL) {
        set_error(result, ERROR_INVALID_PARAMETER);
        return;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0u, NULL, NULL, &startup, &process)) {
        set_error(result, GetLastError());
    } else {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        result->error = 0u;
        result->process = NULL;
        result->process_id = 0u;
    }
    free(command);
}

static void win_close_process(void *context, void *process) {
    (void)context;
    if (process != NULL) CloseHandle((HANDLE)process);
}

void guest_launch_windows_native(guest_launch_native_t *table) {
    table->context = NULL;
    table->shell_execute = win_shell_execute;
    table->create_console = win_create_console;
    table->activate_aumid = win_activate_aumid;
    table->close_process = win_close_process;
}
