/** @file guest_input.c @brief Session stdin, resize and signal receiver. */
#include "guest_pump.h"

static uint16_t get16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}
static uint32_t get32(const uint8_t *bytes) {
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void close_input(guest_process_t *process) {
    if (process->input != NULL) { CloseHandle(process->input); process->input = NULL; }
}

static int write_input(guest_pump_t *pump, const uint8_t *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length && pump->process->input != NULL) {
        if (InterlockedCompareExchange(&pump->wire->stopping, 0, 0) != 0) { return 0; }
        DWORD written = 0;
        if (!WriteFile(pump->process->input, bytes + offset, (DWORD)(length - offset), &written, NULL)) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
                close_input(pump->process);
                return 0; // Child may validly close stdin before host reaches EOF.
            }
            if (error == ERROR_OPERATION_ABORTED && InterlockedCompareExchange(&pump->wire->stopping, 0, 0)) { return 0; }
            return -1;
        }
        if (written == 0) { return -1; }
        offset += written;
    }
    return 0;
}

DWORD WINAPI guest_input_thread(void *context) {
    guest_pump_t *pump = context;
    guest_process_t *process = pump->process;
    int input_eof = 0;
    uint8_t body[16392];
    for (;;) {
        if (InterlockedCompareExchange(&pump->wire->stopping, 0, 0)) { break; }
        guest_frame_t frame;
        if (guest_receive_header(pump->wire, &frame) != 0) { break; }
        if (frame.length > sizeof(body)) { goto failure; }
        if (guest_receive_body(pump->wire, &frame, body) != 0) { break; }
        if (guest_control_validate(frame.type, body, frame.length,
                                    (int)process->interactive, input_eof) != 0) { goto failure; }
        switch (frame.type) {
            case 3:
                if (write_input(pump, body + 8, frame.length - 8) != 0) { goto failure; }
                break;
            case 9:
                input_eof = 1;
                close_input(process);
                break;
            case 4: {
                COORD dimensions = {(SHORT)get16(body + 2), (SHORT)get16(body)};
                if (FAILED(ResizePseudoConsole(process->console, dimensions))) { goto failure; }
                break;
            }
            case 5: {
                uint32_t signal = get32(body);
                if (signal == 9 || signal == 15) {
                    InterlockedExchange(&pump->termination, (LONG)signal);
                    if (!TerminateJobObject(process->job, 128 + signal)) { goto failure; }
                } else if (process->interactive) {
                    const uint8_t interrupt = 3;
                    if (write_input(pump, &interrupt, 1) != 0) { goto failure; }
                } else if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process->pid)) { goto failure; }
                break;
            }
            case 7:
                if (guest_send(pump->wire, 8, NULL, 0) != 0) { goto done; }
                break;
            default: goto failure;
        }
    }
    goto done;
failure:
    guest_wire_fail(pump->wire);
done:
    close_input(process);
    return 0;
}
