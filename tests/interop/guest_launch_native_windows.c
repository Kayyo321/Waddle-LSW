/**
 * @file guest_launch_native_windows.c
 * @brief Windows desktop smoke for the guest launch executor over the real Win32 table.
 *
 * Checks, in order:
 *   (a) Console kind: `cmd.exe /c exit 0` in a new console; the returned process id waits to exit code 0.
 *   (b) Path kind: `notepad.exe` through ShellExecuteExW; the process is alive, is NOT in a job object, and is
 *       terminated by the test.
 *   (c) Path kind on a nonexistent file: NotFound.
 *   (d) Installer and Shortcut kinds: the msiexec command line and the shortcut target are formed as specified.
 *       A recording wrapper stands in for ShellExecuteExW here, so no installer or shortcut ever runs.
 *
 * Run on a Windows desktop session: .\build\interop_guest_launch_native_test.exe
 * Exit code 0 when every check passes. Prints one line per check.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "guest_launch.h"
#include "guest_launch_windows.h"
#include "waddle/interop_protocol.h"

static int failures = 0;

#define Expect(condition, label) do { \
    if (condition) { \
        printf("PASS %s\n", label); \
    } else { \
        printf("FAIL %s (line %d)\n", label, __LINE__); \
        failures += 1; \
    } \
} while (0)

/** @brief Recording wrapper: captures shell_execute calls and never reaches Win32. */
typedef struct recorder_t {
    guest_launch_native_t real;
    char file[1100];
    char parameters[8200];
    int has_parameters;
    int calls;
} recorder_t;

static void record_shell(void *context, const guest_launch_shell_t *shell, guest_launch_native_result_t *result) {
    recorder_t *recorder = (recorder_t *)context;
    recorder->calls += 1;
    strncpy(recorder->file, shell->file, sizeof(recorder->file) - 1u);
    recorder->has_parameters = shell->parameters != NULL;
    if (shell->parameters != NULL) strncpy(recorder->parameters, shell->parameters, sizeof(recorder->parameters) - 1u);
    result->error = 0u;
    result->process_id = 0u;
    result->process = NULL;
}

/** @brief Builds one LaunchRequest in ascending tag order. Returns the frame length or 0. */
static size_t build_request(uint8_t *frame, size_t capacity, uint64_t kind, const char *path, const char *arguments) {
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    if (path != NULL && interop_put_bytes(&writer, InteropTagTargetPath, path, (uint16_t)strlen(path)) != InteropErrorNone) return 0;
    if (arguments != NULL && interop_put_bytes(&writer, InteropTagArguments, arguments, (uint16_t)strlen(arguments)) != InteropErrorNone) return 0;
    if (interop_put_uint(&writer, InteropTagLaunchKind, kind) != InteropErrorNone) return 0;
    if (interop_writer_finish(&writer, InteropMsgLaunchRequest, 1u, InteropFromHost, &length) != InteropErrorNone) return 0;
    return length;
}

/** @brief Runs one request and decodes the status and process id of the reply. Returns 0 on success. */
static int run(const guest_launch_native_t *table, uint64_t kind, const char *path, const char *arguments,
               uint32_t *status, uint32_t *process_id) {
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t result[GuestLaunchResultBytes];
    size_t result_length = 0;
    size_t length = build_request(frame, sizeof(frame), kind, path, arguments);
    guest_launch_env_t env;
    interop_header_t header;
    uint64_t value = 0;
    if (length == 0u) return 1;
    env.native = table;
    env.resolve = NULL;
    env.resolve_context = NULL;
    if (guest_launch_execute(&env, frame, length, result, sizeof(result), &result_length) != 0) return 1;
    if (interop_header_decode(result, result_length, &header) != InteropErrorNone) return 1;
    if (interop_field_uint(result + InteropHeaderBytes, header.length, InteropTagStatus, 4u, &value) != 0) return 1;
    *status = (uint32_t)value;
    if (interop_field_uint(result + InteropHeaderBytes, header.length, InteropTagProcessId, 4u, &value) != 0) return 1;
    *process_id = (uint32_t)value;
    return 0;
}

static void system_path(char *buffer, size_t capacity, const char *leaf) {
    char directory[MAX_PATH];
    GetSystemDirectoryA(directory, sizeof(directory));
    snprintf(buffer, capacity, "%s\\%s", directory, leaf);
}

int main(void) {
    guest_launch_native_t real;
    recorder_t recorder;
    char cmd[MAX_PATH];
    char notepad[MAX_PATH];
    uint32_t status = 0;
    uint32_t process_id = 0;
    HANDLE process;
    DWORD exit_code = 0;
    BOOL in_job = TRUE;
    BOOL self_in_job = TRUE;

    guest_launch_windows_native(&real);
    memset(&recorder, 0, sizeof(recorder));
    recorder.real = real;
    recorder.real.shell_execute = record_shell;
    recorder.real.context = &recorder;

    /* (a) cmd.exe /c exit 0 in a new console. */
    system_path(cmd, sizeof(cmd), "cmd.exe");
    Expect(run(&real, InteropLaunchConsole, cmd, "/c exit 0", &status, &process_id) == 0, "console run returns a reply");
    Expect(status == InteropOk, "console status is Ok");
    Expect(process_id != 0u, "console reports a process id");
    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    Expect(process != NULL, "console process can be opened by its id");
    if (process != NULL) {
        Expect(WaitForSingleObject(process, 30000) == WAIT_OBJECT_0, "console process exits within 30 s");
        Expect(GetExitCodeProcess(process, &exit_code) && exit_code == 0u, "console process exit code is 0");
        CloseHandle(process);
    }

    /* (b) notepad.exe through ShellExecuteExW: alive, not in a job, then terminated by the test. */
    system_path(notepad, sizeof(notepad), "notepad.exe");
    Expect(run(&real, InteropLaunchPath, notepad, NULL, &status, &process_id) == 0, "path run returns a reply");
    Expect(status == InteropOk, "path status is Ok");
    if (process_id != 0u) {
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, process_id);
        Expect(process != NULL, "notepad process can be opened by its id");
        if (process != NULL) {
            Expect(GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE, "notepad process is alive");
            /* The agent creates no job, so the child is in a job only if this test process already is (an
             * inherited job from the runner). Compare the two memberships instead of assuming none. */
            Expect(IsProcessInJob(GetCurrentProcess(), NULL, &self_in_job) && IsProcessInJob(process, NULL, &in_job) &&
                       in_job == self_in_job, "notepad process has no job membership the test process lacks");
            Expect(TerminateProcess(process, 0u), "notepad process is terminated by the test");
            WaitForSingleObject(process, 10000);
            CloseHandle(process);
        }
    } else {
        Expect(FALSE, "notepad reports a process id");
    }

    /* (c) A nonexistent file is NotFound. */
    Expect(run(&real, InteropLaunchPath, "C:\\waddle-no-such-dir\\nope.exe", NULL, &status, &process_id) == 0,
           "missing file run returns a reply");
    Expect(status == InteropNotFound, "missing file status is NotFound");
    Expect(process_id == 0u, "missing file reports process id 0");

    /* (d) Installer and Shortcut command formation through the recording wrapper. */
    Expect(run(&recorder.real, InteropLaunchInstaller, "D:\\installers\\setup.msi", "/qn", &status, &process_id) == 0,
           "installer run returns a reply");
    Expect(recorder.calls == 1 && strcmp(recorder.file, "msiexec.exe") == 0, "installer runs msiexec.exe");
    Expect(recorder.has_parameters && strcmp(recorder.parameters, "/i \"D:\\installers\\setup.msi\" /qn") == 0,
           "installer command line is /i quoted path plus arguments");
    Expect(run(&recorder.real, InteropLaunchShortcut, "C:\\Users\\dev\\Desktop\\app.lnk", NULL, &status, &process_id) == 0,
           "shortcut run returns a reply");
    Expect(recorder.calls == 2 && strcmp(recorder.file, "C:\\Users\\dev\\Desktop\\app.lnk") == 0 && !recorder.has_parameters,
           "shortcut opens the link itself with no parameters");

    printf("%s: %d failure(s)\n", failures == 0 ? "guest_launch_native_test PASSED" : "guest_launch_native_test FAILED", failures);
    return failures == 0 ? 0 : 1;
}
