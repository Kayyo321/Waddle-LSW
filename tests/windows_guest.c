/** @file windows_guest.c @brief Native AF_UNIX guest regression and handle stress suite. */
#include <winsock2.h>
#include <windows.h>
#include "guest_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/** @brief Test-owned UNIX address, native system ABI, no shared state. */
typedef struct test_address_t { uint16_t family; char path[108]; } test_address_t;
/** @brief Writer-thread borrowed socket state; retained until join. */
typedef struct test_writer_t { SOCKET socket; uint32_t sequence; } test_writer_t;
/** @brief Immutable watchdog handles borrowed until the watchdog joins. */
typedef struct test_deadline_t { HANDLE stopped; HANDLE listener; } test_deadline_t;
static DWORD WINAPI fixture_deadline(void *context) {
    const test_deadline_t *deadline = context;
    if (WaitForSingleObject(deadline->stopped, 180000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL: independent native fixture deadline\n");
        TerminateProcess(deadline->listener, 1);
        ExitProcess(1);
    }
    return 0;
}
static HANDLE listener_process;
static char socket_path[108];
static char self_path[4096];
static char cwd_path[4096];
static unsigned scenarios;

static void check(int valid, const char *message) {
    if (valid) { return; }
    fprintf(stderr, "FAIL at scenario %u: %s (Win32 %lu / Winsock %d)\n", scenarios + 1, message,
            (unsigned long)GetLastError(), WSAGetLastError());
    if (listener_process != NULL) { TerminateProcess(listener_process, 1); WaitForSingleObject(listener_process, 5000); }
    DeleteFileA(socket_path);
    exit(1);
}
static void put16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; i++) { p[i] = (uint8_t)(n >> (8 * i)); } }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void transfer(SOCKET socket, uint8_t *bytes, size_t length, int sending) {
    ULONGLONG deadline = GetTickCount64() + 60000;
    for (size_t offset = 0; offset < length;) {
        size_t remaining = length - offset;
        int chunk = (int)(remaining > 4096 ? 4096 : remaining);
        int n = sending ? send(socket, (const char *)bytes + offset, chunk, 0)
                        : recv(socket, (char *)bytes + offset, chunk, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            check(GetTickCount64() < deadline, "socket readiness deadline");
            fd_set ready;
            FD_ZERO(&ready); FD_SET(socket, &ready);
            struct timeval interval = {0, 1000};
            check(select(0, sending ? NULL : &ready, sending ? &ready : NULL, NULL, &interval) >= 0,
                  "socket readiness wait");
            continue;
        }
        check(n > 0, "socket transfer");
        offset += (size_t)n;
    }
}
static void send_frame(SOCKET socket, uint32_t *sequence, uint16_t type, const uint8_t *body, size_t length) {
    uint8_t header[32] = {0};
    put32(header, 0x57444c43); put16(header + 4, 1); put16(header + 6, type);
    header[8] = 1; put32(header + 16, (uint32_t)length); put32(header + 24, (*sequence)++);
    put32(header + 28, length == 0 ? 0 : guest_crc32(body, length));
    transfer(socket, header, sizeof(header), 1);
    transfer(socket, (uint8_t *)body, length, 1);
}
static uint16_t receive_frame(SOCKET socket, uint32_t *sequence, uint8_t *body, size_t *length) {
    uint8_t header[32];
    transfer(socket, header, sizeof(header), 0);
    check(get32(header) == 0x57444c43 && get16(header + 4) == 1 && get32(header + 8) == 1 &&
          get32(header + 12) == 0 && get32(header + 20) == 0 && get32(header + 24) == (*sequence)++, "guest header");
    *length = get32(header + 16);
    check(*length <= 16392, "bounded guest response");
    transfer(socket, body, *length, 0);
    check(guest_crc32(body, *length) == get32(header + 28), "guest CRC");
    return get16(header + 6);
}
static SOCKET connect_guest(void) {
    test_address_t address = {AF_UNIX, {0}};
    strcpy(address.path, socket_path);
    for (unsigned attempt = 0; attempt < 400; attempt++) {
        SOCKET socket = WSASocketW(AF_UNIX, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        check(socket != INVALID_SOCKET, "create client");
        if (connect(socket, (struct sockaddr *)&address, sizeof(address)) == 0) {
            DWORD timeout = 15000;
            check(setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0, "receive timeout");
            check(setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) == 0, "send timeout");
            u_long nonblocking = 1;
            check(ioctlsocket(socket, FIONBIO, &nonblocking) == 0, "nonblocking duplex client");
            return socket;
        }
        closesocket(socket);
        Sleep(25);
    }
    check(0, "connect guest deadline");
    return INVALID_SOCKET;
}
static void expect_closed(SOCKET socket) {
    ULONGLONG deadline = GetTickCount64() + 15000;
    for (;;) {
        uint8_t byte;
        int received = recv(socket, (char *)&byte, 1, 0);
        if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            check(GetTickCount64() < deadline, "closure deadline");
            Sleep(10);
            continue;
        }
        check(received == 0 || (received == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET),
              "connection closes without more messages");
        return;
    }
}
static SOCKET spawn_command(const char *command, const char *cwd, const char *environment,
                            size_t env_length, int interactive, uint32_t expected_status, DWORD *pid) {
    printf("scenario %u: spawn %s\n", scenarios + 1, command);
    SOCKET socket = connect_guest();
    size_t cwd_length = strlen(cwd), command_length = strlen(command);
    size_t total = 26 + cwd_length + command_length + env_length;
    uint8_t *body = calloc(total, 1);
    check(body != NULL, "spawn allocation");
    put32(body, interactive ? 1 : 2); put16(body + 4, 24); put16(body + 6, 80);
    put32(body + 12, (uint32_t)cwd_length); put32(body + 16, (uint32_t)command_length); put32(body + 20, (uint32_t)env_length);
    memcpy(body + 24, cwd, cwd_length); memcpy(body + 25 + cwd_length, command, command_length);
    if (env_length) { memcpy(body + 26 + cwd_length + command_length, environment, env_length); }
    uint32_t sequence = 1;
    send_frame(socket, &sequence, 1, body, total);
    free(body); body = NULL;
    uint8_t response[16392]; size_t length;
    sequence = 1;
    check(receive_frame(socket, &sequence, response, &length) == 2 && length >= 12, "spawn response");
    check(get32(response) == expected_status && get32(response + 8) == length - 12, "spawn status");
    if (pid != NULL) { *pid = get32(response + 4); }
    if (expected_status != 0) {
        expect_closed(socket);
        closesocket(socket);
        return INVALID_SOCKET;
    }
    return socket;
}
static void reject_handshake(unsigned mode) {
    SOCKET socket = connect_guest();
    uint8_t header[32] = {0}, body[26] = {0};
    put32(header, mode == 0 ? 0 : 0x57444c43);
    put16(header + 4, 1); put16(header + 6, 1); header[8] = 1;
    put32(header + 24, 1);
    put32(header + 16, mode == 1 ? 1048577 : sizeof(body));
    put32(header + 28, 1); // Deliberately wrong CRC for the third case.
    transfer(socket, header, sizeof(header), 1);
    if (mode == 2) { transfer(socket, body, sizeof(body), 1); }
    expect_closed(socket);
    closesocket(socket); scenarios++;
}
static void command_for(char *command, size_t capacity, const char *mode) {
    int n = snprintf(command, capacity, "\"%s\" \"--child\" \"%s\"", self_path, mode);
    check(n > 0 && (size_t)n < capacity, "command capacity");
}
static void send_eof(SOCKET socket, uint32_t *sequence) {
    const uint8_t eof[4] = {0}; send_frame(socket, sequence, 9, eof, sizeof(eof));
}
static void drain(SOCKET socket, const char *out, const char *err, uint32_t code, int signal, int large, int interactive) {
    uint32_t incoming = 2, outgoing = 3;
    size_t received[2] = {0, 0}; int eof[2] = {0, 0}, signaled = 0;
    for (;;) {
        uint8_t body[16392]; size_t length;
        uint16_t type = receive_frame(socket, &incoming, body, &length);
        if (type == 3) {
            check(length >= 8 && get32(body + 4) == length - 8 && body[0] >= 1 && body[0] <= 2 &&
                  body[1] == 0 && body[2] == 0 && body[3] == 0, "stream shape");
            unsigned index = body[0] - 1;
            check(!eof[index], "data before EOF");
            for (size_t j = 8; j < length; j++) {
                size_t offset = received[index] + j - 8;
                if (large) {
                    uint8_t want = index ? 'E' : (offset < 3 * 1048576 ? 'O' : (uint8_t)((offset - 3 * 1048576) % 251));
                    check(body[j] == want, "binary duplex content");
                } else if (!interactive) {
                    const char *want = index ? err : out;
                    check(want != NULL && offset < strlen(want) && body[j] == (uint8_t)want[offset], "stream content");
                }
            }
            if (large && received[index] == 0) {
                printf("duplex: first stream %u frame (%zu bytes)\n", index + 1, length - 8);
            }
            size_t previous = received[index];
            received[index] += length - 8;
            if (large && previous / 1048576 != received[index] / 1048576) {
                printf("duplex: stream %u received %zu MiB\n", index + 1, received[index] / 1048576);
            }
            if (signal && !signaled && index == 0 && received[0] >= 6) {
                uint8_t control[4]; put32(control, (uint32_t)signal);
                send_frame(socket, &outgoing, 5, control, sizeof(control)); signaled = 1;
            }
        } else if (type == 9) {
            check(length == 4 && get32(body) >= 1 && get32(body) <= 2, "EOF shape");
            unsigned index = get32(body) - 1; check(!eof[index], "single EOF"); eof[index] = 1;
        } else if (type == 6) {
            check(length == 16 && eof[0] && eof[1] && get32(body) == code, "exit after drains");
            if (signal == 9 || signal == 15) { check(get32(body + 4) == 1, "termination status"); }
            break;
        } else { check(0, "unexpected guest message"); }
    }
    if (large) { check(received[0] == 8 * 1048576 && received[1] == 3 * 1048576, "16 MiB duplex lengths"); }
    else if (!interactive) { check(received[0] == strlen(out) && received[1] == strlen(err), "output lengths"); }
    else { check(received[0] > 0 && received[1] == 0, "merged ConPTY output"); }
    closesocket(socket);
    scenarios++;
    printf("scenario %u: drained and exited\n", scenarios);
}
static void run_case(const char *mode, const char *out, const char *err, uint32_t code, int signal, int interactive) {
    char command[8192]; command_for(command, sizeof(command), mode);
    SOCKET socket = spawn_command(command, cwd_path, "", 0, interactive, 0, NULL);
    uint32_t sequence = 2;
    if (interactive) {
        uint8_t resize[8] = {0}; put16(resize, 31); put16(resize + 2, 99);
        send_frame(socket, &sequence, 4, resize, sizeof(resize));
    }
    send_eof(socket, &sequence);
    drain(socket, out, err, code, signal, 0, interactive);
}
static DWORD WINAPI binary_writer(void *context) {
    test_writer_t *writer = context;
    uint8_t body[16392] = {0};
    put32(body + 4, 16384);
    for (size_t offset = 0; offset < 5 * 1048576; offset += 16384) {
        for (size_t i = 0; i < 16384; i++) { body[i + 8] = (uint8_t)((offset + i) % 251); }
        send_frame(writer->socket, &writer->sequence, 3, body, sizeof(body));
        if (offset == 0 || (offset + 16384) % 1048576 == 0) {
            printf("duplex: stdin sent %zu bytes\n", offset + 16384);
        }
    }
    send_eof(writer->socket, &writer->sequence);
    return 0;
}
static void write_bytes(HANDLE output, const uint8_t *bytes, DWORD length) {
    DWORD offset = 0;
    while (offset < length) { DWORD n = 0; check(WriteFile(output, bytes + offset, length - offset, &n, NULL) && n > 0, "child output"); offset += n; }
}
static BOOL WINAPI interrupt_handler(DWORD event) {
    if (event == CTRL_BREAK_EVENT || event == CTRL_C_EVENT) { ExitProcess(130); }
    return FALSE;
}
static int child(int argc, wchar_t **argv) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE), error = GetStdHandle(STD_ERROR_HANDLE);
    if (wcscmp(argv[2], L"streams") == 0) {
        write_bytes(output, (const uint8_t *)"stdout\n", 7); write_bytes(error, (const uint8_t *)"stderr\n", 7); return 42;
    }
    if (wcscmp(argv[2], L"exit259") == 0) { return 259; }
    if (wcscmp(argv[2], L"args") == 0) {
        check(argc == 8 && wcscmp(argv[3], L"") == 0 && wcscmp(argv[4], L"a b") == 0 &&
              wcscmp(argv[5], L"foo\"bar") == 0 && wcscmp(argv[6], L"C:\\Program Files\\") == 0 &&
              wcscmp(argv[7], L"日本語") == 0, "native argv decoding");
        write_bytes(output, (const uint8_t *)"args ok\n", 8); return 0;
    }
    if (wcscmp(argv[2], L"context") == 0) {
        wchar_t value[100], cwd[4096];
        check(GetEnvironmentVariableW(L"WADDLE_TEST", value, 100) > 0 && wcscmp(value, L"日本語") == 0, "case-insensitive environment override");
        check(GetCurrentDirectoryW(4096, cwd) > 0 && wcsstr(cwd, L"日本語") != NULL, "Unicode cwd");
        write_bytes(output, (const uint8_t *)"context ok\n", 11); return 0;
    }
    if (wcscmp(argv[2], L"sleep") == 0) {
        SetConsoleCtrlHandler(interrupt_handler, TRUE);
        write_bytes(output, (const uint8_t *)"READY\n", 6); Sleep(INFINITE); return 1;
    }
    if (wcscmp(argv[2], L"interactive") == 0) {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        check(SetConsoleCtrlHandler(interrupt_handler, TRUE), "interactive interrupt handler");
        check(SetConsoleMode(input, ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT),
              "interactive input mode");
        write_bytes(output, (const uint8_t *)"READY\n", 6);
        char line[80]; DWORD received = 0;
        check(ReadFile(input, line, sizeof(line), &received, NULL) && received >= 2 &&
              line[0] == 'g' && line[1] == 'o', "interactive line input");
        CONSOLE_SCREEN_BUFFER_INFO dimensions;
        check(GetConsoleScreenBufferInfo(output, &dimensions), "interactive dimensions");
        char report[100];
        int length = snprintf(report, sizeof(report), "SIZE %d %d\n", dimensions.dwSize.Y, dimensions.dwSize.X);
        check(length > 0 && (size_t)length < sizeof(report), "interactive report bound");
        write_bytes(output, (const uint8_t *)report, (DWORD)length);
        write_bytes(error, (const uint8_t *)"MERGED\n", 7);
        Sleep(INFINITE); return 1;
    }
    if (wcscmp(argv[2], L"files") == 0) {
        check(argc == 5, "mapped file argument count");
        HANDLE file = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        check(file != INVALID_HANDLE_VALUE, "open exported input");
        uint8_t bytes[100]; DWORD received = 0;
        check(ReadFile(file, bytes, sizeof(bytes), &received, NULL) && received == 19 &&
              memcmp(bytes, "export read marker\n", 19) == 0, "read exported input");
        CloseHandle(file);
        file = CreateFileW(argv[4], GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        check(file != INVALID_HANDLE_VALUE, "create exported output");
        write_bytes(file, (const uint8_t *)"guest export write\n", 19);
        CloseHandle(file);
        write_bytes(output, (const uint8_t *)"files ok\n", 9); return 0;
    }
    if (wcscmp(argv[2], L"large") == 0) {
        uint8_t bytes[16384]; memset(bytes, 'O', sizeof(bytes));
        for (unsigned i = 0; i < 192; i++) { write_bytes(output, bytes, sizeof(bytes)); }
        memset(bytes, 'E', sizeof(bytes));
        for (unsigned i = 0; i < 192; i++) { write_bytes(error, bytes, sizeof(bytes)); }
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes, sizeof(bytes), &n, NULL)) { check(GetLastError() == ERROR_BROKEN_PIPE, "child stdin read"); break; }
            if (n == 0) { break; } write_bytes(output, bytes, n);
        }
        return 0;
    }
    return 2;
}

/** @brief Native test entry point; owns listener/WSA and cleans them on exit.
 * @param[in] argc Argument count. @param[in] argv Borrowed CRT wide arguments.
 * @return Zero on success, one on test failure, child fixture exit otherwise.
 * @note Main thread only; test workers are joined before their state is released. */
int wmain(int argc, wchar_t **argv) {
    if (argc >= 3 && wcscmp(argv[1], L"--child") == 0) { return child(argc, argv); }
    setvbuf(stdout, NULL, _IONBF, 0);
    WSADATA startup; check(WSAStartup(MAKEWORD(2, 2), &startup) == 0, "Winsock startup");
    wchar_t self[4096], cwd[4096];
    check(GetModuleFileNameW(NULL, self, 4096) > 0 && GetCurrentDirectoryW(4096, cwd) > 0, "fixture paths");
    check(WideCharToMultiByte(CP_UTF8, 0, self, -1, self_path, sizeof(self_path), NULL, NULL) > 0, "UTF-8 executable");
    check(WideCharToMultiByte(CP_UTF8, 0, cwd, -1, cwd_path, sizeof(cwd_path), NULL, NULL) > 0, "UTF-8 cwd");
    snprintf(socket_path, sizeof(socket_path), "waddle_guest_test_%lu.sock", (unsigned long)GetCurrentProcessId());
    wchar_t listener_command[512];
    swprintf(listener_command, 512, L"build\\waddle-guest-exec.exe --socket-path waddle_guest_test_%lu.sock", (unsigned long)GetCurrentProcessId());
    STARTUPINFOW info = {0}; info.cb = sizeof(info); PROCESS_INFORMATION listener = {0};
    check(CreateProcessW(NULL, listener_command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &info, &listener), "start listener");
    listener_process = listener.hProcess; CloseHandle(listener.hThread);
    test_deadline_t deadline = {CreateEventW(NULL, TRUE, FALSE, NULL), listener_process};
    check(deadline.stopped != NULL, "watchdog event");
    HANDLE watchdog = CreateThread(NULL, 0, fixture_deadline, &deadline, 0, NULL);
    check(watchdog != NULL, "watchdog thread");
    run_case("streams", "stdout\n", "stderr\n", 42, 0, 0);
    run_case("exit259", "", "", 259, 0, 0);
    char command[8192]; command_for(command, sizeof(command), "args");
    strcat(command, " \"\" \"a b\" \"foo\\\"bar\" \"C:\\Program Files\\\\\" \"日本語\"");
    SOCKET socket = spawn_command(command, cwd_path, "", 0, 0, 0, NULL); uint32_t sequence = 2;
    send_eof(socket, &sequence); drain(socket, "args ok\n", "", 0, 0, 0, 0);
    wchar_t unicode_cwd[4096]; swprintf(unicode_cwd, 4096, L"%ls\\waddle_日本語_%lu", cwd, (unsigned long)GetCurrentProcessId());
    check(CreateDirectoryW(unicode_cwd, NULL), "Unicode fixture directory");
    char unicode_path[8192]; check(WideCharToMultiByte(CP_UTF8, 0, unicode_cwd, -1, unicode_path, sizeof(unicode_path), NULL, NULL) > 0, "Unicode directory encoding");
    command_for(command, sizeof(command), "context");
    const char environment[] = "WADDLE_TEST=first\0waddle_test=日本語\0";
    socket = spawn_command(command, unicode_path, environment, sizeof(environment) - 1, 0, 0, NULL); sequence = 2;
    send_eof(socket, &sequence); drain(socket, "context ok\n", "", 0, 0, 0, 0);
    check(RemoveDirectoryW(unicode_cwd), "remove fixture directory");
    (void)spawn_command("\"no-such-waddle-executable-987654321.exe\"", cwd_path, "", 0, 0, 2, NULL); scenarios++;
    socket = spawn_command("\"cmd.exe\" \"/c\" \"exit 37\"", cwd_path, "", 0, 0, 0, NULL); sequence = 2;
    send_eof(socket, &sequence); drain(socket, "", "", 37, 0, 0, 0);
    command_for(command, sizeof(command), "large");
    socket = spawn_command(command, cwd_path, "", 0, 0, 0, NULL);
    test_writer_t writer = {socket, 2}; HANDLE thread = CreateThread(NULL, 0, binary_writer, &writer, 0, NULL);
    check(thread != NULL, "duplex writer"); drain(socket, "", "", 0, 0, 1, 0);
    check(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0, "join duplex writer"); CloseHandle(thread);
    run_case("sleep", "READY\n", "", 143, 15, 0);
    run_case("sleep", "READY\n", "", 130, 2, 0);
    command_for(command, sizeof(command), "sleep"); DWORD pid;
    socket = spawn_command(command, cwd_path, "", 0, 0, 0, &pid);
    HANDLE child_process = OpenProcess(SYNCHRONIZE, FALSE, pid); check(child_process != NULL, "observe disconnect child");
    closesocket(socket); check(WaitForSingleObject(child_process, 15000) == WAIT_OBJECT_0, "disconnect kills child"); CloseHandle(child_process); scenarios++;
    socket = spawn_command(command, cwd_path, "", 0, 0, 0, &pid);
    child_process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    check(child_process != NULL, "observe blocked-input child");
    uint8_t blocked_input[16392] = {0}; put32(blocked_input + 4, 16384);
    sequence = 2; send_frame(socket, &sequence, 3, blocked_input, sizeof(blocked_input));
    Sleep(100); closesocket(socket);
    check(WaitForSingleObject(child_process, 15000) == WAIT_OBJECT_0, "disconnect cancels blocked stdin");
    CloseHandle(child_process); scenarios++;
    for (unsigned mode = 0; mode < 3; mode++) { reject_handshake(mode); }
    run_case("streams", NULL, NULL, 42, 0, 1);
    DWORD before, after;
    Sleep(100); check(GetProcessHandleCount(listener_process, &before), "initial handle count");
    for (unsigned i = 0; i < 32; i++) { run_case("exit259", "", "", 259, 0, 0); }
    Sleep(100); check(GetProcessHandleCount(listener_process, &after) && after <= before + 1, "repeated session handle stability");
    SetEvent(deadline.stopped); WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog); CloseHandle(deadline.stopped);
    TerminateProcess(listener_process, 0); WaitForSingleObject(listener_process, 5000); CloseHandle(listener_process); listener_process = NULL;
    DeleteFileA(socket_path); WSACleanup();
    printf("windows guest: %u scenarios passed; handles %lu -> %lu\n", scenarios, (unsigned long)before, (unsigned long)after);
    return 0;
}
