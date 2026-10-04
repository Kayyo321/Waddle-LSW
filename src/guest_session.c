/** @file guest_session.c @brief Spawn handshake, child monitoring and ordered teardown. */
#include "guest_session.h"
#include "guest_pump.h"
#include <stdio.h>
#include <stdlib.h>

static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) { bytes[i] = (uint8_t)(value >> (8 * i)); }
}
static void put64(uint8_t *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) { bytes[i] = (uint8_t)(value >> (8 * i)); }
}
static void join_input(guest_wire_t *wire, HANDLE thread) {
    guest_wire_stop_input(wire);
    if (thread == NULL) { return; }
    // Repeat cancellation closes the check-before-WriteFile race: a worker which
    // has not yet entered a synchronous write cannot miss teardown permanently.
    while (WaitForSingleObject(thread, 10) == WAIT_TIMEOUT) { CancelSynchronousIo(thread); }
    CloseHandle(thread);
}

int guest_session(SOCKET socket) {
    guest_wire_t wire;
    if (guest_wire_init(&wire, socket) != 0) { return -1; }
    int result = -1;
    uint8_t *body = NULL, *executable = NULL;
    guest_frame_t frame;
    guest_spawn_t spawn;
    guest_process_t process = {0};
    HANDLE input = NULL, readers[2] = {NULL, NULL};
    guest_pump_t input_context = {&wire, &process, 0, 0};
    guest_pump_t output_context[2] = {{&wire, &process, 1, 0}, {&wire, &process, 2, 0}};
    if (guest_receive_header(&wire, &frame) != 0 || frame.type != 1 || frame.length < 26) { goto done; }
    body = malloc(frame.length);
    executable = malloc(frame.length + 1);
    if (body == NULL || executable == NULL) { goto done; }
    if (guest_receive_body(&wire, &frame, body) != 0 ||
        guest_spawn_validate(body, frame.length, &spawn, executable, frame.length + 1) != 0) { goto done; }
    if (guest_process_launch(&process, &spawn, executable) != 0) {
        DWORD error = GetLastError();
        uint8_t response[140] = {0};
        int length = snprintf((char *)response + 12, sizeof(response) - 12,
                                "Windows process creation failed (error %lu)", (unsigned long)error);
        if (length < 0 || (size_t)length >= sizeof(response) - 12) { goto done; }
        put32(response, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 2 : 126);
        put32(response + 8, (uint32_t)length);
        result = guest_send(&wire, 2, response, 12 + (size_t)length);
        goto done;
    }
    free(body); body = NULL;
    free(executable); executable = NULL;
    uint8_t response[12] = {0};
    put32(response + 4, process.pid);
    if (guest_send(&wire, 2, response, sizeof(response)) != 0) { goto done; }
    DWORD timeout = 0;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0) { goto done; }
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) { goto done; }
    wire.cancellable = 1;
    readers[0] = CreateThread(NULL, 0, guest_output_thread, &output_context[0], 0, NULL);
    readers[1] = CreateThread(NULL, 0, guest_output_thread, &output_context[1], 0, NULL);
    input = CreateThread(NULL, 0, guest_input_thread, &input_context, 0, NULL);
    if (input == NULL || readers[0] == NULL || readers[1] == NULL) { guest_wire_fail(&wire); goto finish; }
    HANDLE waits[2] = {process.process, wire.failure};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 50);
        if (wait == WAIT_OBJECT_0) { break; }
        if (wait != WAIT_TIMEOUT) { guest_wire_fail(&wire); break; }
        // A pipe writer can block before the receive worker observes socket EOF.
        // Poll only exceptional socket conditions; never consume its framed data.
        WSAPOLLFD peer = {socket, 0, 0};
        int status = WSAPoll(&peer, 1, 0);
        if (status < 0 || (peer.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            guest_wire_fail(&wire);
            break;
        }
    }
finish: {
    DWORD exit_code = 126;
    if (WaitForSingleObject(process.process, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.process, &exit_code)) { guest_wire_fail(&wire); }
    // Kill descendants after natural exit so inherited pipe writers cannot hold EOF.
    TerminateJobObject(process.job, 137);
    join_input(&wire, input);
    input = NULL;
    if (process.console != NULL) {
        if (readers[0] == NULL || WaitForSingleObject(readers[0], 0) == WAIT_OBJECT_0) {
            if (process.output[0] != NULL) { CloseHandle(process.output[0]); process.output[0] = NULL; }
        }
        ClosePseudoConsole(process.console);
        process.console = NULL;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (readers[i] != NULL) {
            WaitForSingleObject(readers[i], INFINITE);
            CloseHandle(readers[i]);
            readers[i] = NULL;
        }
    }
    if (WaitForSingleObject(wire.failure, 0) != WAIT_OBJECT_0) {
        uint8_t exit_body[16] = {0};
        put32(exit_body, exit_code);
        put32(exit_body + 4, input_context.termination != 0 ? 1 : 0);
        put64(exit_body + 8, GetTickCount64() - process.started);
        result = guest_send(&wire, 6, exit_body, sizeof(exit_body));
    }
}
done:
    if (result != 0) { guest_wire_fail(&wire); }
    guest_process_close(&process);
    free(body); body = NULL;
    free(executable); executable = NULL;
    guest_wire_close(&wire);
    return result;
}
