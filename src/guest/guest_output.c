/** @file guest_output.c @brief Bounded output drainage and ordered EOF. */
#include "guest_pump.h"

static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) { bytes[i] = (uint8_t)(value >> (i * 8)); }
}

DWORD WINAPI guest_output_thread(void *context) {
    const guest_pump_t *pump = context;
    HANDLE output = pump->process->output[pump->stream - 1];
    uint8_t body[16392] = {0};
    body[0] = (uint8_t)pump->stream;
    if (output != NULL) {
        for (;;) {
            DWORD length = 0;
            if (!ReadFile(output, body + 8, 16384, &length, NULL)) {
                DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA) { guest_wire_fail(pump->wire); }
                break;
            }
            if (length == 0) { break; }
            put32(body + 4, length);
            // After transport failure keep draining until job/pseudoconsole closes.
            // This lets main close ConPTY even if its final screen update fills a pipe.
            if (WaitForSingleObject(pump->wire->failure, 0) != WAIT_OBJECT_0) {
                (void)guest_send(pump->wire, 3, body, length + 8);
            }
        }
    }
    uint8_t eof[4];
    put32(eof, pump->stream);
    (void)guest_send(pump->wire, 9, eof, sizeof(eof));
    return 0;
}
