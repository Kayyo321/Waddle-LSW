/**
 * @file guest_launch_suite.c
 * @brief Linux suite for the guest launch executor over the native double and the real interop codec.
 *
 * Covers every launch kind, every error mapping, the validation rejections, the frame-level refusals that send
 * no reply, and exactly-once closing of every process handle. Run: make interop-guest-launch-test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_launch.h"
#include "guest_launch_double.h"
#include "waddle/interop_protocol.h"

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "guest_launch_suite.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define RequestId 77u
#define ResultCapacity GuestLaunchResultBytes

/** @brief Decoded LaunchResult fields. */
typedef struct decoded_result_t {
    uint32_t request_id;
    uint32_t process_id;
    uint32_t status;
    int has_message;
    char message[300];
    int has_token;
    char token[300];
} decoded_result_t;

static const char *cmd_path = "C:\\Windows\\System32\\cmd.exe";
static const char *msi_path = "D:\\installers\\setup.msi";
static const char *lnk_path = "C:\\Users\\dev\\Desktop\\app.lnk";
static const char *exe_path = "C:\\apps\\tool\\tool.exe";

/** @brief Builds one LaunchRequest frame in ascending tag order. Returns the frame length, or 0. */
static size_t build_request(uint8_t *frame, size_t capacity, uint32_t request_id, uint64_t kind, const char *app_key,
                            const char *path, const char *arguments, const char *working_dir, const char *token,
                            uint32_t type) {
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    if (app_key != NULL && interop_put_bytes(&writer, InteropTagAppKey, app_key, (uint16_t)strlen(app_key)) != InteropErrorNone) return 0;
    if (path != NULL && interop_put_bytes(&writer, InteropTagTargetPath, path, (uint16_t)strlen(path)) != InteropErrorNone) return 0;
    if (arguments != NULL && interop_put_bytes(&writer, InteropTagArguments, arguments, (uint16_t)strlen(arguments)) != InteropErrorNone) return 0;
    if (working_dir != NULL && interop_put_bytes(&writer, InteropTagWorkingDir, working_dir, (uint16_t)strlen(working_dir)) != InteropErrorNone) return 0;
    if (interop_put_uint(&writer, InteropTagLaunchKind, kind) != InteropErrorNone) return 0;
    if (token != NULL && interop_put_bytes(&writer, InteropTagActivationToken, token, (uint16_t)strlen(token)) != InteropErrorNone) return 0;
    if (interop_writer_finish(&writer, type, request_id, InteropFromHost, &length) != InteropErrorNone) return 0;
    return length;
}

/** @brief Decodes a LaunchResult frame with the real codec. Returns 0 on success. */
static int decode_result(const uint8_t *frame, size_t length, decoded_result_t *out) {
    interop_header_t header;
    const uint8_t *value = NULL;
    uint16_t value_length = 0;
    uint64_t number = 0;
    memset(out, 0, sizeof(*out));
    if (interop_header_decode(frame, length, &header) != InteropErrorNone) return 1;
    if (header.type != InteropMsgLaunchResult) return 1;
    if (length != (size_t)InteropHeaderBytes + header.length) return 1;
    if (interop_payload_validate(header.type, InteropFromGuest, frame + InteropHeaderBytes, header.length) != InteropErrorNone) return 1;
    out->request_id = header.request_id;
    if (interop_field_uint(frame + InteropHeaderBytes, header.length, InteropTagProcessId, 4u, &number) != 0) return 1;
    out->process_id = (uint32_t)number;
    if (interop_field_uint(frame + InteropHeaderBytes, header.length, InteropTagStatus, 4u, &number) != 0) return 1;
    out->status = (uint32_t)number;
    if (interop_field_get(frame + InteropHeaderBytes, header.length, InteropTagMessage, 0u, &value, &value_length) == 0) {
        if (value_length >= sizeof(out->message)) return 1;
        memcpy(out->message, value, value_length);
        out->message[value_length] = '\0';
        out->has_message = 1;
    }
    value = NULL;
    value_length = 0;
    if (interop_field_get(frame + InteropHeaderBytes, header.length, InteropTagActivationToken, 0u, &value, &value_length) == 0) {
        if (value_length >= sizeof(out->token)) return 1;
        memcpy(out->token, value, value_length);
        out->token[value_length] = '\0';
        out->has_token = 1;
    }
    return 0;
}

/** @brief Runs one request through the executor and decodes its reply. Returns 0 when a reply was decoded. */
static int run_request(guest_launch_double_t *d, guest_launch_catalog_t *catalog, uint64_t kind, const char *app_key,
                       const char *path, const char *arguments, const char *working_dir, const char *token,
                       decoded_result_t *out) {
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t result[ResultCapacity];
    size_t length = 0;
    size_t result_length = 0;
    guest_launch_env_t env;
    int rc;
    length = build_request(frame, sizeof(frame), RequestId, kind, app_key, path, arguments, working_dir, token, InteropMsgLaunchRequest);
    if (length == 0u) return 1;
    env.native = &d->table;
    env.resolve = guest_launch_double_resolve;
    env.resolve_context = catalog;
    rc = guest_launch_execute(&env, frame, length, result, sizeof(result), &result_length);
    if (rc != 0) return 2;
    return decode_result(result, result_length, out);
}

/** @brief Every process handle the executor received was closed exactly once. */
static int handles_balanced(const guest_launch_double_t *d) {
    return d->handles_opened == d->handles_closed && d->double_closes == 0u && d->open_count == 0u;
}

/* ---- Status mapping -------------------------------------------------------------------------- */

static int test_status_mapping(void) {
    Check(guest_launch_status_for_error(0u) == InteropOk);
    Check(guest_launch_status_for_error(2u) == InteropNotFound);
    Check(guest_launch_status_for_error(3u) == InteropNotFound);
    Check(guest_launch_status_for_error(123u) == InteropNotFound);
    Check(guest_launch_status_for_error(5u) == InteropAccessDenied);
    Check(guest_launch_status_for_error(11u) == InteropUnsupported);
    Check(guest_launch_status_for_error(31u) == InteropUnsupported);
    Check(guest_launch_status_for_error(50u) == InteropUnsupported);
    Check(guest_launch_status_for_error(193u) == InteropUnsupported);
    Check(guest_launch_status_for_error(170u) == InteropBusy);
    Check(guest_launch_status_for_error(1223u) == InteropCancelled);
    Check(guest_launch_status_for_error(1460u) == InteropTimedOut);
    Check(guest_launch_status_for_error(87u) == InteropGuestError);
    return 0;
}

/* ---- Kinds ------------------------------------------------------------------------------------ */

static int test_path_kind(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, exe_path, "--flag value", NULL, NULL, &reply) == 0);
    Check(d.call_count == 1u);
    Check(d.calls[0].kind == GuestLaunchDoubleShell);
    Check(strcmp(d.calls[0].verb, "open") == 0);
    Check(strcmp(d.calls[0].file, exe_path) == 0);
    Check(d.calls[0].has_parameters && strcmp(d.calls[0].parameters, "--flag value") == 0);
    Check(d.calls[0].has_directory && strcmp(d.calls[0].directory, "C:\\apps\\tool") == 0);
    Check(reply.request_id == RequestId);
    Check(reply.status == InteropOk);
    Check(reply.process_id == 4321u);
    Check(!reply.has_message);
    Check(handles_balanced(&d));
    return 0;
}

static int test_path_kind_with_working_dir_and_no_arguments(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    d.return_handle = 1;
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:/apps/tool.exe", NULL, "D:\\work", "tok-1", &reply) == 0);
    Check(d.calls[0].file[0] != '\0' && strcmp(d.calls[0].file, "C:/apps/tool.exe") == 0);
    Check(!d.calls[0].has_parameters);
    Check(d.calls[0].has_directory && strcmp(d.calls[0].directory, "D:\\work") == 0);
    Check(reply.has_token && strcmp(reply.token, "tok-1") == 0);
    Check(reply.status == InteropOk);
    Check(handles_balanced(&d));
    return 0;
}

static int test_path_kind_document_without_process(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    d.process_id = 0u;
    d.return_handle = 0;
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:\\docs\\notes.txt", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk && reply.process_id == 0u);
    Check(strcmp(d.calls[0].directory, "C:\\docs") == 0);
    Check(handles_balanced(&d));
    return 0;
}

static int test_installer_kind(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchInstaller, NULL, msi_path, NULL, NULL, NULL, &reply) == 0);
    Check(strcmp(d.calls[0].file, "msiexec.exe") == 0);
    Check(strcmp(d.calls[0].parameters, "/i \"D:\\installers\\setup.msi\"") == 0);
    Check(strcmp(d.calls[0].directory, "D:\\installers") == 0);
    Check(reply.status == InteropOk);
    Check(handles_balanced(&d));

    /* Upper-case suffix and appended arguments. */
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchInstaller, NULL, "E:\\SETUP.MSI", "/qn", "E:\\x", NULL, &reply) == 0);
    Check(strcmp(d.calls[0].parameters, "/i \"E:\\SETUP.MSI\" /qn") == 0);
    Check(strcmp(d.calls[0].directory, "E:\\x") == 0);
    Check(reply.status == InteropOk);
    return 0;
}

static int test_installer_rejects_other_suffixes(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchInstaller, NULL, exe_path, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropUnsupported && d.call_count == 0u);
    return 0;
}

static int test_shortcut_kind(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchShortcut, NULL, lnk_path, NULL, NULL, NULL, &reply) == 0);
    Check(strcmp(d.calls[0].file, lnk_path) == 0);
    Check(!d.calls[0].has_parameters);
    Check(strcmp(d.calls[0].directory, "C:\\Users\\dev\\Desktop") == 0);
    Check(reply.status == InteropOk);
    Check(handles_balanced(&d));

    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchShortcut, NULL, lnk_path, "-x", NULL, NULL, &reply) == 0);
    Check(reply.status == InteropUnsupported && d.call_count == 0u);
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchShortcut, NULL, exe_path, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropUnsupported && d.call_count == 0u);
    return 0;
}

static int test_console_kind(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    d.return_handle = 1;
    Check(run_request(&d, NULL, InteropLaunchConsole, NULL, cmd_path, "/c exit 0", NULL, NULL, &reply) == 0);
    Check(d.call_count == 1u && d.calls[0].kind == GuestLaunchDoubleConsole);
    Check(strcmp(d.calls[0].command, "\"C:\\Windows\\System32\\cmd.exe\" /c exit 0") == 0);
    Check(strcmp(d.calls[0].directory, "C:\\Windows\\System32") == 0);
    Check(reply.status == InteropOk && reply.process_id == 4321u);
    Check(handles_balanced(&d));

    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchConsole, NULL, cmd_path, NULL, "C:\\tmp", NULL, &reply) == 0);
    Check(strcmp(d.calls[0].command, "\"C:\\Windows\\System32\\cmd.exe\"") == 0);
    Check(strcmp(d.calls[0].directory, "C:\\tmp") == 0);
    return 0;
}

static int test_app_kind_packaged_aumid(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-Contoso_Notes-App";
    catalog.target.packaged = 1u;
    strcpy(catalog.target.aumid, "Contoso.Notes_8wekyb3d8bbwe!App");
    catalog.target.aumid_length = (uint32_t)strlen(catalog.target.aumid);
    Check(run_request(&d, &catalog, InteropLaunchApp, catalog.app_key, NULL, NULL, NULL, NULL, &reply) == 0);
    Check(catalog.calls == 1u);
    Check(d.call_count == 1u && d.calls[0].kind == GuestLaunchDoubleActivate);
    Check(strcmp(d.calls[0].aumid, "Contoso.Notes_8wekyb3d8bbwe!App") == 0);
    Check(strcmp(d.calls[0].arguments, "") == 0);
    Check(reply.status == InteropOk);
    Check(handles_balanced(&d));
    return 0;
}

static int test_app_kind_unpackaged_aumid(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-Microsoft_Windows_Explorer-3f9e";
    strcpy(catalog.target.aumid, "Microsoft.Windows.Explorer");
    catalog.target.aumid_length = (uint32_t)strlen(catalog.target.aumid);
    Check(run_request(&d, &catalog, InteropLaunchApp, catalog.app_key, NULL, NULL, NULL, NULL, &reply) == 0);
    Check(d.call_count == 1u && d.calls[0].kind == GuestLaunchDoubleShell);
    Check(strcmp(d.calls[0].verb, "open") == 0);
    Check(strcmp(d.calls[0].file, "shell:AppsFolder\\Microsoft.Windows.Explorer") == 0);
    Check(!d.calls[0].has_parameters && !d.calls[0].has_directory);
    Check(reply.status == InteropOk);
    Check(handles_balanced(&d));

    /* An AppsFolder item cannot take arguments: refused without a native call. */
    guest_launch_double_init(&d);
    strcpy(catalog.target.arguments, "/x");
    catalog.target.arguments_length = 2u;
    Check(run_request(&d, &catalog, InteropLaunchApp, catalog.app_key, NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropUnsupported && d.call_count == 0u);
    return 0;
}

static int test_app_kind_target_path(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "p-7a3c";
    strcpy(catalog.target.path, exe_path);
    catalog.target.path_length = (uint32_t)strlen(exe_path);
    strcpy(catalog.target.arguments, "--start");
    catalog.target.arguments_length = 7u;
    strcpy(catalog.target.working_dir, "C:\\apps\\tool\\data");
    catalog.target.working_dir_length = (uint32_t)strlen(catalog.target.working_dir);
    Check(run_request(&d, &catalog, InteropLaunchApp, catalog.app_key, NULL, NULL, NULL, NULL, &reply) == 0);
    Check(d.call_count == 1u && strcmp(d.calls[0].file, exe_path) == 0);
    Check(strcmp(d.calls[0].parameters, "--start") == 0);
    Check(strcmp(d.calls[0].directory, "C:\\apps\\tool\\data") == 0);
    Check(reply.status == InteropOk);
    Check(handles_balanced(&d));
    return 0;
}

static int test_app_kind_failures(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;

    /* Unknown key: NotFound from the resolver, no native call. */
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-known";
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-missing", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropNotFound && d.call_count == 0u && catalog.calls == 1u);
    Check(reply.has_message && strcmp(reply.message, "app key is not in the catalog") == 0);

    /* Resolver failure: GuestError. */
    catalog.fail = 1;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-known", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    catalog.fail = 0;

    /* No resolver at all. */
    {
        uint8_t frame[InteropMaxFrameBytes];
        uint8_t result[ResultCapacity];
        size_t length = build_request(frame, sizeof(frame), RequestId, InteropLaunchApp, "a-known", NULL, NULL, NULL, NULL, InteropMsgLaunchRequest);
        size_t result_length = 0;
        guest_launch_env_t env;
        env.native = &d.table;
        env.resolve = NULL;
        env.resolve_context = NULL;
        Check(guest_launch_execute(&env, frame, length, result, sizeof(result), &result_length) == 0);
        Check(decode_result(result, result_length, &reply) == 0 && reply.status == InteropGuestError);
    }

    /* Catalog entries with no launch target, or with values the wire rules refuse. */
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-empty";
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-empty", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-rel";
    strcpy(catalog.target.path, "relative\\tool.exe");
    catalog.target.path_length = 16u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-rel", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-bad-wd";
    strcpy(catalog.target.path, exe_path);
    catalog.target.path_length = (uint32_t)strlen(exe_path);
    strcpy(catalog.target.working_dir, "not-absolute");
    catalog.target.working_dir_length = 12u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bad-wd", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-bad-aumid";
    catalog.target.aumid[0] = '\x01';
    catalog.target.aumid_length = 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bad-aumid", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-bad-args";
    strcpy(catalog.target.path, exe_path);
    catalog.target.path_length = (uint32_t)strlen(exe_path);
    catalog.target.arguments[0] = '\x07';
    catalog.target.arguments_length = 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bad-args", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-utf8";
    static const char utf8_bad[] = {'C', ':', '\\', 'a', (char)0xC3, (char)0x28, '.', 'e', 'x', 'e'};
    memcpy(catalog.target.path, utf8_bad, sizeof(utf8_bad));
    catalog.target.path_length = (uint32_t)sizeof(utf8_bad);
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-utf8", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-oversize";
    catalog.target.path_length = GuestLaunchMaxText + 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-oversize", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(handles_balanced(&d));
    return 0;
}

/* ---- Validation of Path, Installer, Shortcut and Console -------------------------------------- */

static int test_path_validation(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "relative\\tool.exe", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:\\a\\\"b.exe", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:\\a\tb.exe", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:\\ok.exe", "bad\targs\x01", NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:\\ok.exe", NULL, "wd-relative", NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    /* A UNC path and a tab inside arguments are both valid. */
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "\\\\server\\share\\tool.exe", "a\tb", NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk && d.call_count == 1u);
    Check(strcmp(d.calls[0].directory, "\\\\server\\share") == 0);
    Check(handles_balanced(&d));
    return 0;
}

/* ---- Native errors ---------------------------------------------------------------------------- */

static int test_native_error_mapping_and_handle_on_error(void) {
    static guest_launch_double_t d;
    static const uint32_t errors[] = {2u, 3u, 5u, 11u, 31u, 50u, 123u, 170u, 193u, 1223u, 1460u, 87u};
    static const interop_status_t statuses[] = {InteropNotFound, InteropNotFound, InteropAccessDenied, InteropUnsupported,
                                                InteropUnsupported, InteropUnsupported, InteropNotFound, InteropBusy,
                                                InteropUnsupported, InteropCancelled, InteropTimedOut, InteropGuestError};
    decoded_result_t reply;
    uint32_t index;
    for (index = 0; index < sizeof(errors) / sizeof(errors[0]); ++index) {
        guest_launch_double_init(&d);
        d.error_for[GuestLaunchDoubleShell] = errors[index];
        d.return_handle_on_error = 1;
        d.process_id = 999u;
        Check(run_request(&d, NULL, InteropLaunchPath, NULL, exe_path, NULL, NULL, NULL, &reply) == 0);
        Check(reply.status == (uint32_t)statuses[index]);
        Check(reply.process_id == 0u);
        Check(reply.has_message);
        Check(handles_balanced(&d));
        Check(d.handles_opened == 1u);
    }
    /* The message carries the Win32 code for unmapped errors. */
    guest_launch_double_init(&d);
    d.error_for[GuestLaunchDoubleShell] = 87u;
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, exe_path, NULL, NULL, NULL, &reply) == 0);
    Check(strcmp(reply.message, "launch failed: Win32 error 87") == 0);
    return 0;
}

static int test_console_and_activate_errors(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    d.error_for[GuestLaunchDoubleConsole] = 2u;
    d.return_handle_on_error = 1;
    Check(run_request(&d, NULL, InteropLaunchConsole, NULL, cmd_path, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropNotFound && handles_balanced(&d) && d.handles_opened == 1u);

    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-pkg";
    catalog.target.packaged = 1u;
    strcpy(catalog.target.aumid, "Pkg!App");
    catalog.target.aumid_length = 7u;
    d.error_for[GuestLaunchDoubleActivate] = 1223u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-pkg", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropCancelled && reply.process_id == 0u && handles_balanced(&d));
    return 0;
}

static int test_success_closes_its_handle_once(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    d.return_handle = 1;
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, exe_path, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk && reply.process_id == 4321u);
    Check(d.handles_opened == 1u && d.handles_closed == 1u && d.double_closes == 0u);
    return 0;
}

/* ---- Frame-level refusals: no reply, no native call -------------------------------------------- */

static int test_frames_that_get_no_reply(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t result[ResultCapacity];
    size_t length = build_request(frame, sizeof(frame), RequestId, InteropLaunchPath, NULL, exe_path, NULL, NULL, NULL, InteropMsgLaunchRequest);
    size_t result_length = 0;
    guest_launch_env_t env;
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    env.native = &d.table;
    env.resolve = guest_launch_double_resolve;
    env.resolve_context = &catalog;
    Check(length > 0u);

    /* Invalid arguments. */
    Check(guest_launch_execute(NULL, frame, length, result, sizeof(result), &result_length) == -1);
    Check(guest_launch_execute(&env, NULL, length, result, sizeof(result), &result_length) == -1);
    Check(guest_launch_execute(&env, frame, length, NULL, sizeof(result), &result_length) == -1);
    Check(guest_launch_execute(&env, frame, length, result, sizeof(result), NULL) == -1);
    env.native = NULL;
    Check(guest_launch_execute(&env, frame, length, result, sizeof(result), &result_length) == -1);
    env.native = &d.table;

    /* Output space below the required size is refused before anything is launched. */
    Check(guest_launch_execute(&env, frame, length, result, GuestLaunchResultBytes - 1u, &result_length) == -1);

    /* A frame that is too short for a header, a header that disagrees with the length, a wrong type. */
    Check(guest_launch_execute(&env, frame, 4u, result, sizeof(result), &result_length) == -1);
    Check(guest_launch_execute(&env, frame, length - 1u, result, sizeof(result), &result_length) == -1);
    Check(d.call_count == 0u);
    Check(handles_balanced(&d));
    return 0;
}

static int test_malformed_payload_gets_guest_error_reply(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t result[ResultCapacity];
    size_t result_length = 0;
    size_t length;
    size_t index;
    int patched = 0;
    decoded_result_t reply;
    guest_launch_env_t env;
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    env.native = &d.table;
    env.resolve = guest_launch_double_resolve;
    env.resolve_context = &catalog;

    /* Start from a valid Path request, then set its launch_kind value (critical u16 tag 0x8036, length 1) to 6. */
    length = build_request(frame, sizeof(frame), 31u, InteropLaunchPath, NULL, exe_path, NULL, NULL, NULL, InteropMsgLaunchRequest);
    Check(length > 0u);
    for (index = InteropHeaderBytes; index + 5u < length; ++index) {
        if (frame[index] == 0x36u && frame[index + 1u] == 0x80u && frame[index + 2u] == 0x01u && frame[index + 3u] == 0x00u) {
            frame[index + 4u] = 6u;
            patched = 1;
        }
    }
    Check(patched == 1);
    Check(guest_launch_execute(&env, frame, length, result, sizeof(result), &result_length) == 0);
    Check(decode_result(result, result_length, &reply) == 0);
    Check(reply.request_id == 31u && reply.status == InteropGuestError && reply.process_id == 0u);
    Check(d.call_count == 0u);
    return 0;
}

static int test_reply_echoes_request_id_and_token(void) {
    static guest_launch_double_t d;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    d.return_handle = 1;
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, exe_path, NULL, NULL, "activation-token-with-length", &reply) == 0);
    Check(reply.request_id == RequestId);
    Check(reply.has_token && strcmp(reply.token, "activation-token-with-length") == 0);
    Check(!reply.has_message);
    Check(handles_balanced(&d));
    return 0;
}

static int test_result_capacity_is_checked_for_the_reply(void) {
    static guest_launch_double_t d;
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t result[ResultCapacity];
    size_t length = build_request(frame, sizeof(frame), RequestId, InteropLaunchPath, NULL, exe_path, NULL, NULL, NULL, InteropMsgLaunchRequest);
    size_t result_length = 0;
    guest_launch_env_t env;
    guest_launch_double_init(&d);
    env.native = &d.table;
    env.resolve = NULL;
    env.resolve_context = NULL;
    /* The minimum capacity is enough for every reply the executor can form. */
    Check(guest_launch_execute(&env, frame, length, result, GuestLaunchResultBytes, &result_length) == 0);
    Check(result_length <= GuestLaunchResultBytes);
    Check(d.call_count == 1u);
    return 0;
}

/* ---- Text rules through the catalog path (the codec refuses bad text before the executor) ----------- */

typedef struct text_case_t {
    const char *bytes;
    size_t length;
    int valid;
} text_case_t;

static void catalog_set_bytes(char *field, uint32_t *field_length, const char *bytes, size_t length) {
    memcpy(field, bytes, length);
    field[length] = '\0';
    *field_length = (uint32_t)length;
}

static int test_utf8_text_rules(void) {
    static const text_case_t cases[] = {
        {"\xc3\xa9", 2u, 1}, {"\xe2\x82\xac", 3u, 1}, {"\xf0\x9f\x98\x80", 4u, 1}, {"\xe0\xa0\x80", 3u, 1},
        {"\xf4\x8f\xbf\xbf", 4u, 1}, {"\xed\x9f\xbf", 3u, 1},
        {"\xc0\x80", 2u, 0}, {"\xc1\xbf", 2u, 0}, {"\xe0\x80\x80", 3u, 0}, {"\xed\xa0\x80", 3u, 0},
        {"\xf4\x90\x80\x80", 4u, 0}, {"\xf5\x80\x80\x80", 4u, 0}, {"\xc3", 1u, 0}, {"\xe2\x82", 2u, 0},
        {"\xc3\x41", 2u, 0}, {"\x80", 1u, 0}, {"\xf0\x9f\x98", 3u, 0}, {"\xf0\x80\x80\x80", 4u, 0},
    };
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;
    size_t index;
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char path[80];
        char arguments[80];
        const char *prefix = "C:\\";
        const char *suffix = "\\a.exe";
        /* In the path: the case sits between the drive and a file name. */
        guest_launch_double_init(&d);
        memset(&catalog, 0, sizeof(catalog));
        catalog.app_key = "a-utf8";
        memcpy(path, prefix, 3u);
        memcpy(path + 3u, cases[index].bytes, cases[index].length);
        memcpy(path + 3u + cases[index].length, suffix, 6u);
        catalog_set_bytes(catalog.target.path, &catalog.target.path_length, path, 3u + cases[index].length + 6u);
        Check(run_request(&d, &catalog, InteropLaunchApp, "a-utf8", NULL, NULL, NULL, NULL, &reply) == 0);
        Check(reply.status == (cases[index].valid ? InteropOk : InteropGuestError));
        Check(handles_balanced(&d));

        /* In the arguments: the same bytes are text the executor may pass to Win32 only when valid. */
        guest_launch_double_init(&d);
        memset(&catalog, 0, sizeof(catalog));
        catalog.app_key = "a-utf8";
        strcpy(catalog.target.path, exe_path);
        catalog.target.path_length = (uint32_t)strlen(exe_path);
        memcpy(arguments, cases[index].bytes, cases[index].length);
        catalog_set_bytes(catalog.target.arguments, &catalog.target.arguments_length, arguments, cases[index].length);
        Check(run_request(&d, &catalog, InteropLaunchApp, "a-utf8", NULL, NULL, NULL, NULL, &reply) == 0);
        Check(reply.status == (cases[index].valid ? InteropOk : InteropGuestError));
        Check(handles_balanced(&d));
    }

    /* Control bytes are refused in paths and arguments; a tab is allowed only in arguments. */
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-ctl";
    strcpy(catalog.target.path, exe_path);
    catalog.target.path_length = (uint32_t)strlen(exe_path);
    strcpy(catalog.target.arguments, "x\ty");
    catalog.target.arguments_length = 3u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-ctl", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk);
    catalog.target.arguments[1] = '\x7f';
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-ctl", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);
    catalog.target.arguments[1] = '\t';
    catalog.target.path[3] = '\x02';
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-ctl", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);
    return 0;
}

static int test_absolute_path_and_working_directory_bounds(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    decoded_result_t reply;
    guest_launch_double_init(&d);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-bounds";
    /* Lower-case drive and forward slashes are absolute; a drive without a separator is not. */
    strcpy(catalog.target.path, "c:/apps/tool.exe");
    catalog.target.path_length = 16u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk);
    Check(strcmp(d.calls[0].directory, "c:/apps") == 0);
    Check(handles_balanced(&d));

    strcpy(catalog.target.path, "C:tool.exe");
    catalog.target.path_length = 10u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);
    strcpy(catalog.target.path, "1:\\tool.exe");
    catalog.target.path_length = 11u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);
    strcpy(catalog.target.path, "\\\\ab");
    catalog.target.path_length = 4u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk);
    strcpy(catalog.target.path, "\\\\a");
    catalog.target.path_length = 3u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);

    /* A working directory must be absolute too: a drive root directory is accepted, a relative one is not. */
    strcpy(catalog.target.path, exe_path);
    catalog.target.path_length = (uint32_t)strlen(exe_path);
    strcpy(catalog.target.working_dir, "C:/");
    catalog.target.working_dir_length = 3u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk && strcmp(d.calls[d.call_count - 1u].directory, "C:/") == 0);
    strcpy(catalog.target.working_dir, "D:relative");
    catalog.target.working_dir_length = 10u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);
    Check(handles_balanced(&d));

    /* An AUMID with a control byte is refused; a file too short to carry an installer or shortcut suffix is refused. */
    memset(&catalog.target, 0, sizeof(catalog.target));
    catalog_set_bytes(catalog.target.aumid, &catalog.target.aumid_length, "Pkg\tBad", 7u);
    catalog.target.packaged = 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-bounds", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError);
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchInstaller, NULL, "C:\\", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropUnsupported && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchShortcut, NULL, "C:\\", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropUnsupported && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:\\", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropOk && strcmp(d.calls[0].directory, "C:\\") == 0);
    return 0;
}

/* ---- Wire bounds, short and malformed paths, and frame types ----------------------------------- */

static int test_wire_bounds_and_frame_types(void) {
    static guest_launch_double_t d;
    static guest_launch_catalog_t catalog;
    static char long_text[2001u];
    decoded_result_t reply;
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t result[ResultCapacity];
    size_t result_length = 0;
    size_t length;
    guest_launch_env_t env;

    /* A path longer than the executor's bound comes straight off the wire (the codec allows 4096 bytes). */
    guest_launch_double_init(&d);
    memset(long_text, 'b', sizeof(long_text) - 1u);
    memcpy(long_text, "C:\\", 3u);
    long_text[2000] = '\0';
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, long_text, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(handles_balanced(&d));

    /* Paths too short to be absolute, and the UNC and drive shapes the rule tells apart. */
    guest_launch_double_init(&d);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "C:", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "\\x\\y", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(run_request(&d, NULL, InteropLaunchPath, NULL, "1:\\y", NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);

    /* A catalog entry with a NUL byte inside its path, and oversize arguments or working directory. */
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-nul";
    memcpy(catalog.target.path, "C:\\a\0b.exe", 10u);
    catalog.target.path_length = 10u;
    guest_launch_double_init(&d);
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-nul", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    memset(&catalog, 0, sizeof(catalog));
    catalog.app_key = "a-big";
    strcpy(catalog.target.path, exe_path);
    catalog.target.path_length = (uint32_t)strlen(exe_path);
    catalog.target.arguments_length = GuestLaunchMaxArguments + 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-big", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    catalog.target.arguments_length = 0u;
    catalog.target.working_dir_length = GuestLaunchMaxText + 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-big", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    catalog.target.working_dir_length = 0u;
    catalog.target.aumid_length = GuestLaunchMaxText + 1u;
    Check(run_request(&d, &catalog, InteropLaunchApp, "a-big", NULL, NULL, NULL, NULL, &reply) == 0);
    Check(reply.status == InteropGuestError && d.call_count == 0u);
    Check(handles_balanced(&d));

    /* A LaunchResult-typed frame from the guest is not a request: no reply, no launch. */
    {
        interop_writer_t writer;
        size_t out_length = 0;
        interop_writer_init(&writer, frame, sizeof(frame));
        Check(interop_put_uint(&writer, InteropTagProcessId, 5u) == InteropErrorNone);
        Check(interop_put_uint(&writer, InteropTagStatus, InteropOk) == InteropErrorNone);
        Check(interop_writer_finish(&writer, InteropMsgLaunchResult, 9u, InteropFromGuest, &out_length) == InteropErrorNone);
        length = out_length;
        env.native = &d.table;
        env.resolve = NULL;
        env.resolve_context = NULL;
        Check(guest_launch_execute(&env, frame, length, result, sizeof(result), &result_length) == -1);
        Check(d.call_count == 0u);
    }
    return 0;
}

int main(void) {
    static const char *names[] = {
        "status mapping", "path kind", "path kind with working dir and token", "path kind document without process",
        "installer kind", "installer rejects other suffixes", "shortcut kind", "console kind", "app kind packaged AUMID",
        "app kind unpackaged AUMID", "app kind target path", "app kind failures", "path validation",
        "native error mapping and handle on error", "console and activate errors", "success closes handle once",
        "frames that get no reply", "malformed payload gets guest error", "reply echoes request id and token",
        "result capacity is checked", "utf8 text rules", "absolute path and working directory bounds",
        "wire bounds and frame types",
    };
    static int (*const tests[])(void) = {
        test_status_mapping, test_path_kind, test_path_kind_with_working_dir_and_no_arguments,
        test_path_kind_document_without_process, test_installer_kind, test_installer_rejects_other_suffixes,
        test_shortcut_kind, test_console_kind, test_app_kind_packaged_aumid, test_app_kind_unpackaged_aumid,
        test_app_kind_target_path, test_app_kind_failures, test_path_validation,
        test_native_error_mapping_and_handle_on_error, test_console_and_activate_errors,
        test_success_closes_its_handle_once, test_frames_that_get_no_reply,
        test_malformed_payload_gets_guest_error_reply, test_reply_echoes_request_id_and_token,
        test_result_capacity_is_checked_for_the_reply, test_utf8_text_rules, test_absolute_path_and_working_directory_bounds,
        test_wire_bounds_and_frame_types,
    };
    size_t index;
    int failures = 0;
    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index]() != 0) {
            fprintf(stderr, "FAILED: %s\n", names[index]);
            failures += 1;
        } else {
            printf("ok: %s\n", names[index]);
        }
    }
    if (failures != 0) {
        fprintf(stderr, "guest_launch_suite: %d test(s) failed\n", failures);
        return 1;
    }
    printf("guest_launch_suite: all tests passed\n");
    return 0;
}
