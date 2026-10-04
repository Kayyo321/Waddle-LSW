/** @file guest_wire.c @brief Bounded blocking transport with concurrent senders. */
#include "guest_wire.h"
#include <string.h>

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}
static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) { bytes[i] = (uint8_t)(value >> (8 * i)); }
}

int guest_wire_init(guest_wire_t *wire, SOCKET socket) {
    memset(wire, 0, sizeof(*wire));
    wire->socket = socket;
    wire->incoming = 1;
    wire->outgoing = 1;
    wire->failure = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (wire->failure == NULL) { return -1; }
    InitializeCriticalSection(&wire->send_lock);
    return 0;
}
void guest_wire_close(guest_wire_t *wire) {
    CloseHandle(wire->failure);
    wire->failure = NULL;
    DeleteCriticalSection(&wire->send_lock);
}
void guest_wire_fail(guest_wire_t *wire) {
    SetEvent(wire->failure);
    shutdown(wire->socket, SD_BOTH);
}

void guest_wire_stop_input(guest_wire_t *wire) {
    InterlockedExchange(&wire->stopping, 1);
    shutdown(wire->socket, SD_RECEIVE);
}

static int receive_exact(guest_wire_t *wire, uint8_t *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        if (wire->cancellable) {
            if (InterlockedCompareExchange(&wire->stopping, 0, 0) != 0) { return -1; }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(wire->socket, &readable);
            struct timeval interval = {0, 1000};
            int ready = select(0, &readable, NULL, NULL, &interval);
            if (ready == 0) { continue; }
            if (ready < 0) {
                if (InterlockedCompareExchange(&wire->stopping, 0, 0) == 0) { guest_wire_fail(wire); }
                return -1;
            }
        }
        int received = recv(wire->socket, (char *)bytes + offset, (int)(length - offset), 0);
        if (received < 0 && WSAGetLastError() == WSAEWOULDBLOCK) { continue; }
        if (received <= 0) {
            if (InterlockedCompareExchange(&wire->stopping, 0, 0) == 0) { guest_wire_fail(wire); }
            return -1;
        }
        offset += (size_t)received;
    }
    return 0;
}
int guest_receive_header(guest_wire_t *wire, guest_frame_t *frame) {
    uint8_t header[32];
    if (receive_exact(wire, header, sizeof(header)) != 0) { return -1; }
    if (guest_header_validate(header, sizeof(header), wire->incoming, frame) != 0) {
        guest_wire_fail(wire);
        return -1;
    }
    return 0;
}
int guest_receive_body(guest_wire_t *wire, const guest_frame_t *frame, uint8_t *body) {
    if (receive_exact(wire, body, frame->length) != 0) { return -1; }
    if (guest_crc32(body, frame->length) != frame->crc) { guest_wire_fail(wire); return -1; }
    wire->incoming++;
    return 0;
}

static int send_exact(guest_wire_t *wire, const uint8_t *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        if (WaitForSingleObject(wire->failure, 0) == WAIT_OBJECT_0) { return -1; }
        size_t remaining = length - offset;
        int chunk = (int)(remaining > 4096 ? 4096 : remaining);
        int sent = send(wire->socket, (const char *)bytes + offset, chunk, 0);
        if (sent < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(wire->socket, &writable);
            struct timeval interval = {0, 1000};
            if (select(0, NULL, &writable, NULL, &interval) >= 0) { continue; }
        }
        if (sent <= 0) { guest_wire_fail(wire); return -1; }
        offset += (size_t)sent;
    }
    return 0;
}
int guest_send(guest_wire_t *wire, uint16_t type, const uint8_t *body, size_t length) {
    if (length > 1048576 || (length != 0 && body == NULL)) { return -1; }
    EnterCriticalSection(&wire->send_lock);
    int result = -1;
    if (WaitForSingleObject(wire->failure, 0) == WAIT_OBJECT_0) { goto done; }
    uint8_t header[32] = {0};
    put32(header, 0x57444c43);
    put16(header + 4, 1);
    put16(header + 6, type);
    header[8] = 1;
    put32(header + 16, (uint32_t)length);
    put32(header + 24, wire->outgoing++);
    put32(header + 28, length == 0 ? 0 : guest_crc32(body, length));
    if (send_exact(wire, header, sizeof(header)) == 0 && send_exact(wire, body, length) == 0) { result = 0; }
done:
    LeaveCriticalSection(&wire->send_lock);
    return result;
}
