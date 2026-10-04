/** @file guest_process.c @brief Restricted raw-pipe process creation and cleanup. */
#include "guest_process.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void close_handle(HANDLE *handle) {
    if (*handle != NULL && *handle != INVALID_HANDLE_VALUE) { CloseHandle(*handle); }
    *handle = NULL;
}

void guest_process_close(guest_process_t *process) {
    if (process->job != NULL) { TerminateJobObject(process->job, 137); }
    if (process->process != NULL) {
        TerminateProcess(process->process, 137);
        WaitForSingleObject(process->process, INFINITE);
    }
    close_handle(&process->input);
    close_handle(&process->output[0]);
    close_handle(&process->output[1]);
    if (process->console != NULL) { ClosePseudoConsole(process->console); process->console = NULL; }
    close_handle(&process->process);
    close_handle(&process->job);
}

int guest_process_launch(guest_process_t *process, const guest_spawn_t *spawn,
                         const uint8_t *executable) {
    memset(process, 0, sizeof(*process));
    wchar_t *cwd = NULL, *command = NULL, *application = NULL, *environment = NULL;
    STARTUPINFOEXW startup = {0};
    PROCESS_INFORMATION child = {0};
    HANDLE child_input = NULL, child_output = NULL, child_error = NULL;
    DWORD error = GetLastError();
    int initialized = 0, result = -1;
    cwd = guest_utf16(spawn->cwd, spawn->cwd_length);
    if (cwd == NULL) { goto native_error; }
    command = guest_utf16(spawn->command, spawn->command_length);
    if (command == NULL) { goto native_error; }
    application = guest_utf16(executable, strlen((const char *)executable));
    if (application == NULL) { goto native_error; }
    environment = guest_environment(spawn);
    if (environment == NULL) { goto native_error; }
    if (wcslen(command) + 1 > 32767) { error = ERROR_BAD_LENGTH; goto done; }
    if (spawn->interactive) { error = ERROR_NOT_SUPPORTED; goto done; }
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    if (!CreatePipe(&child_input, &process->input, &security, 0) ||
        !CreatePipe(&process->output[0], &child_output, &security, 0) ||
        !CreatePipe(&process->output[1], &child_error, &security, 0) ||
        !SetHandleInformation(process->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(process->output[0], HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(process->output[1], HANDLE_FLAG_INHERIT, 0)) { goto native_error; }
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input;
    startup.StartupInfo.hStdOutput = child_output;
    startup.StartupInfo.hStdError = child_error;
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = malloc(attribute_size);
    if (startup.lpAttributeList == NULL) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) { goto native_error; }
    initialized = 1;
    HANDLE inherited[] = {child_input, child_output, child_error};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherited, sizeof(inherited), NULL, NULL)) { goto native_error; }
    process->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (process->job == NULL || !SetInformationJobObject(process->job,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { goto native_error; }
    process->started = GetTickCount64();
    if (!CreateProcessW(application, command, NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
        EXTENDED_STARTUPINFO_PRESENT, environment, cwd, &startup.StartupInfo, &child)) { goto native_error; }
    process->process = child.hProcess;
    process->pid = child.dwProcessId;
    if (!AssignProcessToJobObject(process->job, process->process) ||
        ResumeThread(child.hThread) == (DWORD)-1) { goto native_error; }
    result = 0;
    goto done;
native_error:
    error = GetLastError();
done:
    close_handle(&child.hThread);
    close_handle(&child_input);
    close_handle(&child_output);
    close_handle(&child_error);
    if (initialized) { DeleteProcThreadAttributeList(startup.lpAttributeList); }
    free(startup.lpAttributeList);
    startup.lpAttributeList = NULL;
    free(cwd); cwd = NULL;
    free(command); command = NULL;
    free(application); application = NULL;
    free(environment); environment = NULL;
    if (result != 0) { guest_process_close(process); SetLastError(error); }
    return result;
}
