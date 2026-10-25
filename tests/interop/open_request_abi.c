/** @file open_request_abi.c @brief Independent C ABI consumer for the waddle open parser.
 *
 * Links the Zig object for src/interop/open_request.zig and the production
 * src/common/arguments.c. The quoting cross-check compares interop_open_quote()
 * with waddle_quote() on fixed and seeded random argument lists.
 */
#include "open_request.h"
#include "waddle/cli_protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Read-callback fixture; records calls and serves a synthetic PE header. */
typedef struct reader_t {
    unsigned calls; /**< Number of callback invocations. */
    size_t last_length; /**< Path length of the most recent invocation. */
    int fail; /**< Nonzero makes every invocation fail with this status. */
    uint32_t subsystem; /**< PE subsystem written into the header: 2 GUI, 3 CUI. */
    size_t reported; /**< Length reported to the parser; 0 means 256. */
} reader_t;

/** @brief Write a 256-byte PE32 header prefix into buffer. */
static void make_pe(uint8_t *buffer, uint32_t subsystem) {
    memset(buffer, 0, 256);
    buffer[0] = 'M';
    buffer[1] = 'Z';
    buffer[0x3c] = 0x80;
    memcpy(buffer + 0x80, "PE\0\0", 4);
    buffer[0x94] = 240;
    buffer[0x98] = 0x0b;
    buffer[0x99] = 0x01;
    buffer[0xdc] = (uint8_t)subsystem;
    buffer[0xdd] = (uint8_t)(subsystem >> 8);
}

static int read_image(void *context, const char *path, size_t path_length, uint8_t *buffer, size_t capacity, size_t *length_out) {
    reader_t *reader = context;
    assert(path != NULL && path_length > 0);
    assert(capacity == InteropOpenPrefixBytes);
    reader->calls++;
    reader->last_length = path_length;
    if (reader->fail != 0) return reader->fail;
    make_pe(buffer, reader->subsystem);
    *length_out = reader->reported != 0 ? reader->reported : 256;
    return 0;
}

static int parse_argv(size_t argc, const char *const *argv, uint32_t tty, reader_t *reader, interop_open_request_t *request) {
    interop_open_options_t options = { read_image, reader, tty };
    return interop_open_parse(argc, argv, &options, request);
}

static void check_app_form(void) {
    reader_t reader = { 0, 0, 0, 2, 0 };
    interop_open_request_t request;
    memset(&request, 0xa5, sizeof(request));
    const char *argv[] = { "--wait", "--app", "a-Microsoft_Notepad" };
    assert(parse_argv(3, argv, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.item_count == 1);
    assert(request.wait == 1 && request.new_console == 0);
    assert(request.device == NULL && request.device_length == 0);
    assert(request.items[0].kind == InteropOpenItemApp);
    assert(request.items[0].text_length == strlen("a-Microsoft_Notepad"));
    assert(memcmp(request.items[0].text, "a-Microsoft_Notepad", request.items[0].text_length) == 0);
    const char *late[] = { "--app", "a-Microsoft_Notepad", "--wait" };
    assert(parse_argv(3, late, 0, &reader, &request) == InteropOpenErrorOptionAfterTarget);
    assert(request.items[0].argument_count == 0 && request.items[0].argument_first == 0);
    assert(reader.calls == 0);
}

static void check_paths_and_device(void) {
    reader_t reader = { 0, 0, 0, 2, 0 };
    interop_open_request_t request;
    const char *argv[] = { "-d", "dev-1", "notes.txt", "setup.MSI", "shortcut.lnk", "game.exe" };
    assert(parse_argv(6, argv, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.item_count == 4);
    assert(request.device_length == 5 && memcmp(request.device, "dev-1", 5) == 0);
    assert(request.items[0].kind == InteropOpenItemPath);
    assert(request.items[1].kind == InteropOpenItemInstaller);
    assert(request.items[2].kind == InteropOpenItemShortcut);
    assert(request.items[3].kind == InteropOpenItemPath && request.items[3].image == InteropPeGui);
    assert(reader.calls == 1 && reader.last_length == strlen("game.exe"));
}

static void check_dash_form_and_tail(void) {
    reader_t reader = { 0, 0, 0, 2, 0 };
    interop_open_request_t request;
    const char *argv[] = { "--", "C:\\setup.exe", "--wait", "", "a \"b\"" };
    assert(parse_argv(5, argv, 1, &reader, &request) == InteropOpenErrorNone);
    assert(request.item_count == 1);
    assert(request.items[0].kind == InteropOpenItemExeWithArgs);
    assert(request.items[0].argument_first == 2 && request.items[0].argument_count == 3);
    char quoted[64];
    size_t length = 99;
    assert(interop_open_quote(3, argv + request.items[0].argument_first, quoted, sizeof(quoted), &length) == 0);
    assert(length == strlen("\"--wait\" \"\" \"a \\\"b\\\"\""));
    assert(memcmp(quoted, "\"--wait\" \"\" \"a \\\"b\\\"\"", length + 1) == 0);
}

static void check_console_routing(void) {
    reader_t reader = { 0, 0, 0, 3, 0 };
    interop_open_request_t request;
    const char *plain[] = { "tool.exe" };
    assert(parse_argv(1, plain, 1, &reader, &request) == InteropOpenErrorNone);
    assert(request.items[0].kind == InteropOpenItemDelegateExec && request.items[0].image == InteropPeConsole);
    const char *forced[] = { "--new-console", "tool.exe" };
    assert(parse_argv(2, forced, 1, &reader, &request) == InteropOpenErrorNone);
    assert(request.new_console == 1 && request.items[0].kind == InteropOpenItemConsole);
    assert(parse_argv(1, plain, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.items[0].kind == InteropOpenItemConsole);
}

static void check_read_failures(void) {
    reader_t reader = { 0, 0, 7, 2, 0 };
    interop_open_request_t request;
    memset(&request, 0x5a, sizeof(request));
    const char *argv[] = { "broken.exe" };
    assert(parse_argv(1, argv, 0, &reader, &request) == InteropOpenErrorImageUnreadable);
    assert(request.item_count == 0 && request.device == NULL);
    assert(interop_open_exit_code(InteropOpenErrorImageUnreadable) == InteropOpenExitNotExecutable);
    reader.fail = 1;
    assert(parse_argv(1, argv, 0, &reader, &request) == InteropOpenErrorNotFound);
    assert(interop_open_exit_code(InteropOpenErrorNotFound) == InteropOpenExitNotFound);
    reader.fail = 0;
    reader.reported = InteropOpenPrefixBytes + 1;
    assert(parse_argv(1, argv, 0, &reader, &request) == InteropOpenErrorImageUnreadable);
    interop_open_options_t no_reader = { NULL, NULL, 0 };
    assert(interop_open_parse(1, argv, &no_reader, &request) == InteropOpenErrorImageUnreadable);
}

static void check_bounds(void) {
    reader_t reader = { 0, 0, 0, 2, 0 };
    interop_open_request_t request;
    const char *many[InteropOpenMaxArgs];
    for (size_t index = 0; index < InteropOpenMaxArgs; index++) many[index] = "f.txt";
    assert(parse_argv(InteropOpenMaxArgs, many, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.item_count == InteropOpenMaxItems);
    const char *extra[InteropOpenMaxArgs + 1];
    for (size_t index = 0; index <= InteropOpenMaxArgs; index++) extra[index] = "f.txt";
    assert(parse_argv(InteropOpenMaxArgs + 1, extra, 0, &reader, &request) == InteropOpenErrorTooManyArgs);
    assert(request.item_count == 0);

    char long_arg[InteropOpenMaxArgBytes + 2];
    memset(long_arg, 'p', InteropOpenMaxArgBytes);
    long_arg[InteropOpenMaxArgBytes] = '\0';
    const char *exact[] = { long_arg };
    assert(parse_argv(1, exact, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.items[0].text_length == InteropOpenMaxArgBytes);
    memset(long_arg, 'p', InteropOpenMaxArgBytes + 1);
    long_arg[InteropOpenMaxArgBytes + 1] = '\0';
    const char *over[] = { long_arg };
    assert(parse_argv(1, over, 0, &reader, &request) == InteropOpenErrorArgumentTooLong);

    char device63[InteropOpenMaxDeviceBytes + 2];
    char device64[InteropOpenMaxDeviceBytes + 3];
    memset(device63, 'd', InteropOpenMaxDeviceBytes);
    device63[InteropOpenMaxDeviceBytes] = '\0';
    memset(device64, 'd', InteropOpenMaxDeviceBytes + 1);
    device64[InteropOpenMaxDeviceBytes + 1] = '\0';
    const char *device_ok[] = { "-d", device63, "--app", "a-x" };
    assert(parse_argv(4, device_ok, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.device_length == InteropOpenMaxDeviceBytes);
    const char *device_bad[] = { "-d", device64, "--app", "a-x" };
    assert(parse_argv(4, device_bad, 0, &reader, &request) == InteropOpenErrorInvalidDevice);

    char key200[InteropOpenMaxAppKeyBytes + 2];
    memset(key200, 'k', InteropOpenMaxAppKeyBytes);
    key200[0] = 'a';
    key200[1] = '-';
    key200[InteropOpenMaxAppKeyBytes] = '\0';
    const char *key_ok[] = { "--app", key200 };
    assert(parse_argv(2, key_ok, 0, &reader, &request) == InteropOpenErrorNone);
    assert(request.items[0].text_length == InteropOpenMaxAppKeyBytes);
    char key201[InteropOpenMaxAppKeyBytes + 3];
    memcpy(key201, key200, InteropOpenMaxAppKeyBytes);
    key201[InteropOpenMaxAppKeyBytes] = 'k';
    key201[InteropOpenMaxAppKeyBytes + 1] = '\0';
    const char *key_bad[] = { "--app", key201 };
    assert(parse_argv(2, key_bad, 0, &reader, &request) == InteropOpenErrorInvalidAppKey);

    char tail_fill[InteropOpenMaxArgBytes + 1];
    memset(tail_fill, 'z', 4094);
    tail_fill[4094] = '\0';
    const char *tail_ok[] = { "--", "x.exe", tail_fill };
    assert(parse_argv(3, tail_ok, 0, &reader, &request) == InteropOpenErrorNone);
    memset(tail_fill, 'z', 4095);
    tail_fill[4095] = '\0';
    assert(parse_argv(3, tail_ok, 0, &reader, &request) == InteropOpenErrorQuotedArgumentsTooLong);
}

static void check_usage_errors(void) {
    reader_t reader = { 0, 0, 0, 2, 0 };
    interop_open_request_t request;
    const char *empty[] = { NULL };
    assert(parse_argv(0, empty, 0, &reader, &request) == InteropOpenErrorMissingTarget);
    const char *dash_only[] = { "--" };
    assert(parse_argv(1, dash_only, 0, &reader, &request) == InteropOpenErrorEmptyExecutable);
    const char *not_exe[] = { "--", "x.txt" };
    assert(parse_argv(2, not_exe, 0, &reader, &request) == InteropOpenErrorExecutableNotExe);
    const char *bad_option[] = { "--device=x", "--app", "a-x" };
    assert(parse_argv(3, bad_option, 0, &reader, &request) == InteropOpenErrorUnknownOption);
    const char *late[] = { "a.txt", "--wait" };
    assert(parse_argv(2, late, 0, &reader, &request) == InteropOpenErrorOptionAfterTarget);
    const char *mixed[] = { "--app", "a-x", "a.txt" };
    assert(parse_argv(3, mixed, 0, &reader, &request) == InteropOpenErrorMixedTargets);
    assert(reader.calls == 0);
}

static void check_invalid_arguments(void) {
    interop_open_request_t request;
    memset(&request, 0x3c, sizeof(request));
    interop_open_options_t options = { NULL, NULL, 0 };
    assert(interop_open_parse(0, NULL, NULL, &request) == InteropOpenErrorInvalidArgument);
    assert(interop_open_parse(0, NULL, &options, NULL) == InteropOpenErrorInvalidArgument);
    assert(interop_open_parse(1, NULL, &options, &request) == InteropOpenErrorInvalidArgument);
    assert(request.item_count == (size_t)0x3c3c3c3c3c3c3c3cULL);
    assert(interop_open_exit_code(InteropOpenErrorInvalidArgument) == InteropOpenExitLocalError);
}

static void check_exit_tables(void) {
    assert(interop_open_exit_code(InteropOpenErrorNone) == InteropOpenExitOk);
    for (uint32_t code = InteropOpenErrorUnknownOption; code <= InteropOpenErrorExecutableNotExe; code++) {
        assert(interop_open_exit_code(code) == InteropOpenExitUsage);
    }
    assert(interop_open_exit_code(InteropOpenErrorTooManyArgs) == InteropOpenExitUsage);
    assert(interop_open_exit_code(InteropOpenErrorArgumentTooLong) == InteropOpenExitUsage);
    assert(interop_open_exit_code(InteropOpenErrorQuotedArgumentsTooLong) == InteropOpenExitUsage);
    assert(interop_open_exit_code(InteropOpenErrorOutsideShare) == InteropOpenExitNotExecutable);
    assert(interop_open_exit_code(InteropOpenErrorNotFound) == InteropOpenExitNotFound);
    assert(interop_open_exit_code(InteropOpenErrorLocalFailure) == InteropOpenExitLocalError);
    assert(interop_open_exit_code(InteropOpenErrorGuestUnreachable) == InteropOpenExitLocalError);
    assert(interop_open_exit_code(InteropOpenErrorGuestLaunchFailed) == InteropOpenExitGuestFailure);
    assert(interop_open_exit_code(InteropOpenErrorDelegateConflict) == InteropOpenExitUsage);
    assert(interop_open_exit_code(999u) == InteropOpenExitLocalError);
    assert(interop_open_launch_exit_code(0) == InteropOpenExitOk);
    assert(interop_open_launch_exit_code(1) == InteropOpenExitGuestFailure);
    assert(interop_open_launch_exit_code(9) == InteropOpenExitGuestFailure);
}

/** @brief Compare interop_open_quote with waddle_quote for one NULL-free argument list. */
static void compare_quote(const char *const *args, size_t count) {
    char *argv[InteropOpenMaxArgs + 1];
    for (size_t index = 0; index < count; index++) argv[index] = (char *)args[index];
    argv[count] = NULL;
    char *expected = waddle_quote(argv);
    assert(expected != NULL);
    size_t expected_length = strlen(expected);
    char actual[InteropOpenMaxQuotedBytes + 1];
    size_t length = 0;
    assert(interop_open_quote(count, args, actual, sizeof(actual), &length) == 0);
    assert(length == expected_length);
    assert(memcmp(actual, expected, length + 1) == 0);
    free(expected);
}

static void check_quote_vectors(void) {
    const char *single_space[] = { "a b" };
    compare_quote(single_space, 1);
    const char *quotes[] = { "say \"hi\"", "" };
    compare_quote(quotes, 2);
    const char *trailing[] = { "C:\\dir\\", "x\\y", "\\\\", "a\\\"b" };
    compare_quote(trailing, 4);
    const char *utf8[] = { "\xc3\xa9t\xc3\xa9", "tab\there" };
    compare_quote(utf8, 2);
    compare_quote(NULL, 0);
    char actual[8];
    size_t length = 0;
    assert(interop_open_quote(0, NULL, actual, sizeof(actual), &length) == 0);
    assert(length == 0 && actual[0] == '\0');
}

/** @brief Advance a 64-bit LCG and return 31 random bits. */
static uint32_t next_random(uint64_t *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(*state >> 33);
}

/** @brief Seeded random cross-check of the quoting port against waddle_quote. */
static void check_quote_random(void) {
    uint64_t state = 0x9e3779b97f4a7c15ull;
    const char alphabet[] = { 'a', '\\', '"', ' ', '\t', (char)0xc3, (char)0xa9 };
    for (unsigned iteration = 0; iteration < 5000; iteration++) {
        char storage[4][12];
        const char *args[4];
        size_t count = next_random(&state) % 5u;
        for (size_t index = 0; index < count; index++) {
            size_t length = next_random(&state) % 12u;
            for (size_t position = 0; position < length; position++) {
                storage[index][position] = alphabet[next_random(&state) % sizeof(alphabet)];
            }
            storage[index][length] = '\0';
            args[index] = storage[index];
        }
        compare_quote(args, count);
    }
}

static void check_quote_limits(void) {
    char small[4];
    size_t length = 0;
    const char *arg[] = { "abc" };
    assert(interop_open_quote(1, arg, small, 0, &length) == 3);
    assert(interop_open_quote(1, arg, small, sizeof(small), &length) == 3);
    char exact[7];
    assert(interop_open_quote(1, arg, exact, sizeof(exact), &length) == 0);
    assert(length == 5 && exact[5] == '\0');
    assert(interop_open_quote(1, arg, NULL, sizeof(exact), &length) == 1);
    assert(interop_open_quote(1, arg, exact, sizeof(exact), NULL) == 1);
    assert(interop_open_quote(1, NULL, exact, sizeof(exact), &length) == 1);
    assert(interop_open_quote(InteropOpenMaxArgs + 1, arg, exact, sizeof(exact), &length) == 1);
    char huge[InteropOpenMaxArgBytes + 2];
    memset(huge, 'h', InteropOpenMaxArgBytes + 1);
    huge[InteropOpenMaxArgBytes + 1] = '\0';
    const char *too_long[] = { huge };
    assert(interop_open_quote(1, too_long, exact, sizeof(exact), &length) == 1);
    char many[1500];
    memset(many, 'k', sizeof(many) - 1);
    many[sizeof(many) - 1] = '\0';
    const char *three[] = { many, many, many };
    char big[InteropOpenMaxQuotedBytes + 16];
    assert(interop_open_quote(3, three, big, sizeof(big), &length) == 2);
}

int main(void) {
    check_app_form();
    check_paths_and_device();
    check_dash_form_and_tail();
    check_console_routing();
    check_read_failures();
    check_bounds();
    check_usage_errors();
    check_invalid_arguments();
    check_exit_tables();
    check_quote_vectors();
    check_quote_random();
    check_quote_limits();
    printf("open request ABI: passed\n");
    return 0;
}
