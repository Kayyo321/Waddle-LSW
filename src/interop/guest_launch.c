/**
 * @file guest_launch.c
 * @brief Portable guest launch core: request validation, kind dispatch, error mapping, result encoding.
 *
 * Compiles on Linux and mingw. No Win32 header is included; every Win32 call is behind guest_launch_native_t.
 */
#include "guest_launch.h"

#include <stdio.h>
#include <string.h>

/* Win32 error numbers the mapping names. Plain constants so the core stays portable. */
#define GuestLaunchWinFileNotFound 2u
#define GuestLaunchWinPathNotFound 3u
#define GuestLaunchWinAccessDenied 5u
#define GuestLaunchWinBadFormat 11u
#define GuestLaunchWinInvalidName 123u
#define GuestLaunchWinBusy 170u
#define GuestLaunchWinNotSupported 50u
#define GuestLaunchWinBadExeFormat 193u
#define GuestLaunchWinNoAssociation 31u
#define GuestLaunchWinCancelled 1223u
#define GuestLaunchWinTimeout 1460u

/** @brief Shell item prefix for AppsFolder activation of an unpackaged AUMID. */
#define GuestLaunchAppsFolderPrefix "shell:AppsFolder\\"
/** @brief Installer program; the quoted path follows the `/i "` prefix. */
#define GuestLaunchInstallerProgram "msiexec.exe"
#define GuestLaunchInstallerPrefix "/i \""

/** @brief Decoded request fields, borrowed from the request frame. Lengths are bytes. */
typedef struct guest_launch_request_t {
    uint64_t kind; /**< interop_launch_kind_t value; the codec enforces the range. */
    const uint8_t *app_key;
    size_t app_key_length;
    const uint8_t *path;
    size_t path_length;
    const uint8_t *arguments;
    size_t arguments_length;
    const uint8_t *working_dir;
    size_t working_dir_length;
    const uint8_t *token;
    size_t token_length;
} guest_launch_request_t;

/**
 * @brief Scratch storage for one request, owned by the caller's stack frame. Every string handed to Win32 is a
 *        NUL-terminated copy here, so no caller memory is ever written.
 */
typedef struct guest_launch_work_t {
    guest_launch_target_t target; /**< Resolver output for App requests. */
    char path[GuestLaunchMaxText + 1u];
    char arguments[GuestLaunchMaxArguments + 1u];
    char working_dir[GuestLaunchMaxText + 1u];
    char directory[GuestLaunchMaxText + 1u];
    char aumid[GuestLaunchMaxText + 1u];
    char file[GuestLaunchMaxText + sizeof(GuestLaunchAppsFolderPrefix)];
    char parameters[GuestLaunchMaxCommandLine + 1u];
    char command[GuestLaunchMaxCommandLine + 1u];
    char message[64u];
} guest_launch_work_t;

/** @brief Outcome of one request before encoding. */
typedef struct guest_launch_outcome_t {
    interop_status_t status;
    uint32_t process_id;
    const char *note; /**< Fixed diagnostic for validation and resolver failures; NULL when a Win32 error applies. */
    uint32_t error; /**< Win32 error from the native call, 0 otherwise. */
} guest_launch_outcome_t;

interop_status_t guest_launch_status_for_error(uint32_t error) {
    switch (error) {
    case 0u:
        return InteropOk;
    case GuestLaunchWinFileNotFound:
    case GuestLaunchWinPathNotFound:
    case GuestLaunchWinInvalidName:
        return InteropNotFound;
    case GuestLaunchWinAccessDenied:
        return InteropAccessDenied;
    case GuestLaunchWinBadFormat:
    case GuestLaunchWinBadExeFormat:
    case GuestLaunchWinNoAssociation:
    case GuestLaunchWinNotSupported:
        return InteropUnsupported;
    case GuestLaunchWinBusy:
        return InteropBusy;
    case GuestLaunchWinCancelled:
        return InteropCancelled;
    case GuestLaunchWinTimeout:
        return InteropTimedOut;
    default:
        return InteropGuestError;
    }
}

/* ---- Text validation -------------------------------------------------------------------------- */

/** @brief Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
static int utf8_valid(const uint8_t *text, size_t length) {
    size_t index = 0;
    while (index < length) {
        uint8_t lead = text[index];
        size_t extra;
        uint32_t code;
        size_t offset;
        if (lead < 0x80u) {
            index += 1u;
            continue;
        }
        if (lead >= 0xC2u && lead <= 0xDFu) {
            extra = 1u;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            extra = 2u;
            code = lead & 0x0Fu;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            extra = 3u;
            code = lead & 0x07u;
        } else {
            return 0;
        }
        if (length - index <= extra) return 0;
        for (offset = 1u; offset <= extra; ++offset) {
            if ((text[index + offset] & 0xC0u) != 0x80u) return 0;
            code = (code << 6u) | (text[index + offset] & 0x3Fu);
        }
        if (extra == 2u && (code < 0x800u || (code >= 0xD800u && code <= 0xDFFFu))) return 0;
        if (extra == 3u && (code < 0x10000u || code > 0x10FFFFu)) return 0;
        index += extra + 1u;
    }
    return 1;
}

/**
 * @brief Text the executor passes to Win32: bounded, no NUL, no control bytes, valid UTF-8.
 * @param[in] allow_tab Nonzero permits horizontal tabs (arguments only).
 */
static int text_ok(const uint8_t *text, size_t length, size_t maximum, int allow_tab) {
    size_t index;
    if (length > maximum) return 0;
    for (index = 0; index < length; ++index) {
        uint8_t byte = text[index];
        if (byte == 0u || byte == 0x7Fu) return 0;
        if (byte < 0x20u && !(allow_tab && byte == '\t')) return 0;
    }
    return utf8_valid(text, length);
}

/** @brief Absolute guest path: a drive path (X:\ or X:/) or a UNC path (\\server\...), without quote characters. */
static int absolute_path_ok(const uint8_t *path, size_t length) {
    int drive;
    int unc;
    if (length < 3u || !text_ok(path, length, GuestLaunchMaxText, 0)) return 0;
    if (memchr(path, '"', length) != NULL) return 0;
    drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
            (path[2] == '\\' || path[2] == '/');
    unc = path[0] == '\\' && path[1] == '\\' && length > 3u;
    return drive || unc;
}

/** @brief Length of the directory part of an absolute path, or 0 when it has none. "C:\x.exe" gives "C:\". */
static size_t directory_length(const uint8_t *path, size_t length) {
    size_t index = length;
    while (index > 0u) {
        index -= 1u;
        if (path[index] == '\\' || path[index] == '/') {
            if (index == 2u && path[1] == ':') return 3u;
            return index;
        }
    }
    return 0u;
}

/** @brief Case-insensitive ASCII suffix check. */
static int suffix_ci(const uint8_t *text, size_t length, const char *suffix) {
    size_t suffix_length = strlen(suffix);
    size_t index;
    if (length < suffix_length) return 0;
    for (index = 0; index < suffix_length; ++index) {
        uint8_t byte = text[length - suffix_length + index];
        if (byte >= 'A' && byte <= 'Z') byte = (uint8_t)(byte - 'A' + 'a');
        if (byte != (uint8_t)suffix[index]) return 0;
    }
    return 1;
}

/** @brief Copies bytes into a NUL-terminated buffer; the caller checked that the buffer is large enough. */
static void copy_text(char *destination, const uint8_t *source, size_t length) {
    if (length != 0u) memcpy(destination, source, length);
    destination[length] = '\0';
}

/* ---- Field extraction ------------------------------------------------------------------------- */

/** @brief Borrows one optional text field. Returns 0 on success (absent leaves pointer NULL), -1 when malformed. */
static int read_text(const uint8_t *payload, size_t length, uint16_t tag, const uint8_t **value, size_t *value_length) {
    uint16_t field_length = 0u;
    const uint8_t *field = NULL;
    int rc = interop_field_get(payload, length, tag, 0u, &field, &field_length);
    if (rc < 0) return -1;
    *value = rc == 0 ? field : NULL;
    *value_length = rc == 0 ? field_length : 0u;
    return 0;
}

static int decode_request(const uint8_t *payload, size_t length, guest_launch_request_t *request) {
    memset(request, 0, sizeof(*request));
    if (interop_field_uint(payload, length, InteropTagLaunchKind, 1u, &request->kind) != 0) return -1;
    if (read_text(payload, length, InteropTagAppKey, &request->app_key, &request->app_key_length) != 0) return -1;
    if (read_text(payload, length, InteropTagTargetPath, &request->path, &request->path_length) != 0) return -1;
    if (read_text(payload, length, InteropTagArguments, &request->arguments, &request->arguments_length) != 0) return -1;
    if (read_text(payload, length, InteropTagWorkingDir, &request->working_dir, &request->working_dir_length) != 0) return -1;
    if (read_text(payload, length, InteropTagActivationToken, &request->token, &request->token_length) != 0) return -1;
    return 0;
}

/* ---- Native calls and outcomes ---------------------------------------------------------------- */

/**
 * @brief Converts one native result to an outcome and closes its process handle. The only close site.
 * @note A non-NULL handle is closed even when the call reported an error.
 */
static guest_launch_outcome_t finish_native(const guest_launch_native_t *native, const guest_launch_native_result_t *result) {
    guest_launch_outcome_t outcome;
    if (result->process != NULL) native->close_process(native->context, result->process);
    outcome.status = guest_launch_status_for_error(result->error);
    outcome.process_id = result->error == 0u ? result->process_id : 0u;
    outcome.note = NULL;
    outcome.error = result->error;
    return outcome;
}

static guest_launch_outcome_t fail(interop_status_t status, const char *note) {
    guest_launch_outcome_t outcome;
    outcome.status = status;
    outcome.process_id = 0u;
    outcome.note = note;
    outcome.error = 0u;
    return outcome;
}

static guest_launch_outcome_t run_shell(const guest_launch_native_t *native, const char *file, const char *parameters,
                                        const char *directory) {
    guest_launch_native_result_t result;
    guest_launch_shell_t shell;
    shell.verb = "open";
    shell.file = file;
    shell.parameters = parameters;
    shell.directory = directory;
    result.error = 0u;
    result.process_id = 0u;
    result.process = NULL;
    native->shell_execute(native->context, &shell, &result);
    return finish_native(native, &result);
}

/**
 * @brief Path, Installer, Shortcut and Console dispatch on validated text. `work->path` holds the target path
 *        (NUL-terminated, `path_length` bytes), and `work->arguments` and `work->working_dir` hold the optional
 *        fields (their lengths are passed in).
 */
static guest_launch_outcome_t dispatch_file(const guest_launch_native_t *native, guest_launch_work_t *work, uint64_t kind,
                                            size_t path_length, size_t arguments_length, size_t working_dir_length) {
    const char *directory = NULL;
    size_t dir_length = directory_length((const uint8_t *)work->path, path_length);
    if (working_dir_length != 0u) {
        directory = work->working_dir;
    } else if (dir_length != 0u) {
        copy_text(work->directory, (const uint8_t *)work->path, dir_length);
        directory = work->directory;
    }
    if (kind == InteropLaunchPath) {
        return run_shell(native, work->path, arguments_length != 0u ? work->arguments : NULL, directory);
    }
    if (kind == InteropLaunchInstaller) {
        size_t used;
        if (!suffix_ci((const uint8_t *)work->path, path_length, ".msi")) return fail(InteropUnsupported, "installer launch needs a .msi file");
        used = (size_t)snprintf(work->parameters, sizeof(work->parameters), GuestLaunchInstallerPrefix "%s\"", work->path);
        if (used >= sizeof(work->parameters)) return fail(InteropGuestError, "installer command line too long");
        if (arguments_length != 0u) {
            if (used + 1u + arguments_length >= sizeof(work->parameters)) return fail(InteropGuestError, "installer command line too long");
            work->parameters[used] = ' ';
            memcpy(work->parameters + used + 1u, work->arguments, arguments_length);
            work->parameters[used + 1u + arguments_length] = '\0';
        }
        return run_shell(native, GuestLaunchInstallerProgram, work->parameters, directory);
    }
    if (kind == InteropLaunchShortcut) {
        if (!suffix_ci((const uint8_t *)work->path, path_length, ".lnk")) return fail(InteropUnsupported, "shortcut launch needs a .lnk file");
        if (arguments_length != 0u) return fail(InteropUnsupported, "shortcut launch cannot take arguments");
        return run_shell(native, work->path, NULL, directory);
    }
    if (kind == InteropLaunchConsole) {
        guest_launch_native_result_t result;
        int written = snprintf(work->command, sizeof(work->command), "\"%s\"", work->path);
        if (written < 0 || (size_t)written >= sizeof(work->command)) return fail(InteropGuestError, "command line too long");
        if (arguments_length != 0u) {
            if ((size_t)written + 1u + arguments_length >= sizeof(work->command)) return fail(InteropGuestError, "command line too long");
            work->command[written] = ' ';
            memcpy(work->command + written + 1u, work->arguments, arguments_length);
            work->command[(size_t)written + 1u + arguments_length] = '\0';
        }
        result.error = 0u;
        result.process_id = 0u;
        result.process = NULL;
        native->create_console(native->context, work->command, directory, &result);
        return finish_native(native, &result);
    }
    return fail(InteropGuestError, "unknown launch kind");
}

/** @brief App request: resolve the catalog entry, then launch it by AUMID or by its target path. */
static guest_launch_outcome_t dispatch_app(const guest_launch_env_t *env, guest_launch_work_t *work,
                                           const guest_launch_request_t *request) {
    guest_launch_target_t *target = &work->target;
    int rc;
    if (env->resolve == NULL) return fail(InteropGuestError, "no catalog resolver");
    rc = env->resolve(env->resolve_context, request->app_key, request->app_key_length, target);
    if (rc == 1) return fail(InteropNotFound, "app key is not in the catalog");
    if (rc != 0) return fail(InteropGuestError, "catalog lookup failed");
    if (target->aumid_length > GuestLaunchMaxText || target->path_length > GuestLaunchMaxText ||
        target->arguments_length > GuestLaunchMaxArguments || target->working_dir_length > GuestLaunchMaxText) {
        return fail(InteropGuestError, "catalog entry is out of bounds");
    }
    if (target->aumid_length != 0u && !text_ok((const uint8_t *)target->aumid, target->aumid_length, GuestLaunchMaxText, 0)) {
        return fail(InteropGuestError, "catalog AUMID is not valid text");
    }
    if (target->path_length != 0u && !absolute_path_ok((const uint8_t *)target->path, target->path_length)) {
        return fail(InteropGuestError, "catalog target path is not an absolute guest path");
    }
    if (target->working_dir_length != 0u && !absolute_path_ok((const uint8_t *)target->working_dir, target->working_dir_length)) {
        return fail(InteropGuestError, "catalog working directory is not an absolute guest path");
    }
    if (!text_ok((const uint8_t *)target->arguments, target->arguments_length, GuestLaunchMaxArguments, 1)) {
        return fail(InteropGuestError, "catalog arguments are not valid text");
    }
    if (target->aumid_length != 0u) {
        copy_text(work->aumid, (const uint8_t *)target->aumid, target->aumid_length);
        if (target->packaged != 0u) {
            guest_launch_native_result_t result;
            result.error = 0u;
            result.process_id = 0u;
            result.process = NULL;
            env->native->activate_aumid(env->native->context, work->aumid, target->arguments, &result);
            return finish_native(env->native, &result);
        }
        if (target->arguments_length != 0u) return fail(InteropUnsupported, "AppsFolder launch cannot take arguments");
        memcpy(work->file, GuestLaunchAppsFolderPrefix, sizeof(GuestLaunchAppsFolderPrefix) - 1u);
        memcpy(work->file + sizeof(GuestLaunchAppsFolderPrefix) - 1u, work->aumid, target->aumid_length);
        work->file[sizeof(GuestLaunchAppsFolderPrefix) - 1u + target->aumid_length] = '\0';
        return run_shell(env->native, work->file, NULL, NULL);
    }
    if (target->path_length != 0u) {
        copy_text(work->path, (const uint8_t *)target->path, target->path_length);
        copy_text(work->arguments, (const uint8_t *)target->arguments, target->arguments_length);
        copy_text(work->working_dir, (const uint8_t *)target->working_dir, target->working_dir_length);
        return dispatch_file(env->native, work, InteropLaunchPath, target->path_length, target->arguments_length,
                             target->working_dir_length);
    }
    return fail(InteropGuestError, "catalog entry has no launch target");
}

/** @brief Validates a request's fields and runs the launch it names. */
static guest_launch_outcome_t launch(const guest_launch_env_t *env, guest_launch_work_t *work, const guest_launch_request_t *request) {
    if (request->kind == InteropLaunchApp) return dispatch_app(env, work, request);
    if (request->path_length == 0u) return fail(InteropGuestError, "launch request has no target path");
    if (!absolute_path_ok(request->path, request->path_length)) return fail(InteropGuestError, "target path is not an absolute guest path");
    if (request->working_dir_length != 0u && !absolute_path_ok(request->working_dir, request->working_dir_length)) {
        return fail(InteropGuestError, "working directory is not an absolute guest path");
    }
    if (!text_ok(request->arguments, request->arguments_length, GuestLaunchMaxArguments, 1)) {
        return fail(InteropGuestError, "arguments are not valid text");
    }
    copy_text(work->path, request->path, request->path_length);
    copy_text(work->arguments, request->arguments, request->arguments_length);
    copy_text(work->working_dir, request->working_dir, request->working_dir_length);
    return dispatch_file(env->native, work, request->kind, request->path_length, request->arguments_length, request->working_dir_length);
}

/* ---- Result encoding -------------------------------------------------------------------------- */

/**
 * @brief Writes a LaunchResult frame. Tags in ascending order: ProcessId 0x0009, ActivationToken 0x0037,
 *        Status 0x0038, Message 0x0039.
 */
static int encode_result(uint8_t *result, size_t capacity, uint32_t request_id, const guest_launch_request_t *request,
                         const guest_launch_outcome_t *outcome, const char *message, size_t *result_length) {
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, result, capacity);
    if (interop_put_uint(&writer, InteropTagProcessId, outcome->process_id) != InteropErrorNone) return -1;
    if (request->token_length != 0u &&
        interop_put_bytes(&writer, InteropTagActivationToken, request->token, (uint16_t)request->token_length) != InteropErrorNone) {
        return -1;
    }
    if (interop_put_uint(&writer, InteropTagStatus, (uint64_t)outcome->status) != InteropErrorNone) return -1;
    if (message != NULL && interop_put_bytes(&writer, InteropTagMessage, message, (uint16_t)strlen(message)) != InteropErrorNone) {
        return -1;
    }
    if (interop_writer_finish(&writer, InteropMsgLaunchResult, request_id, InteropFromGuest, &length) != InteropErrorNone) return -1;
    *result_length = length;
    return 0;
}

/** @brief Reply for a frame whose header decoded but whose payload did not. The request has no usable token. */
static int reply_malformed(uint8_t *result, size_t capacity, uint32_t request_id, size_t *result_length) {
    guest_launch_request_t none;
    guest_launch_outcome_t outcome = fail(InteropGuestError, "malformed launch request");
    memset(&none, 0, sizeof(none));
    return encode_result(result, capacity, request_id, &none, &outcome, outcome.note, result_length);
}

int guest_launch_execute(const guest_launch_env_t *env, const uint8_t *request, size_t request_length, uint8_t *result,
                         size_t result_capacity, size_t *result_length) {
    interop_header_t header;
    guest_launch_request_t decoded;
    guest_launch_outcome_t outcome;
    guest_launch_work_t work;
    char message[64u];
    if (env == NULL || env->native == NULL || request == NULL || result == NULL || result_length == NULL) return -1;
    if (result_capacity < GuestLaunchResultBytes) return -1;
    if (interop_header_decode(request, request_length, &header) != InteropErrorNone) return -1;
    if (header.type != InteropMsgLaunchRequest) return -1;
    if (request_length != (size_t)InteropHeaderBytes + header.length) return -1;
    if (interop_payload_validate(header.type, InteropFromHost, request + InteropHeaderBytes, header.length) != InteropErrorNone) {
        return reply_malformed(result, result_capacity, header.request_id, result_length);
    }
    if (decode_request(request + InteropHeaderBytes, header.length, &decoded) != 0) {
        return reply_malformed(result, result_capacity, header.request_id, result_length);
    }
    outcome = launch(env, &work, &decoded);
    if (outcome.status == InteropOk) {
        return encode_result(result, result_capacity, header.request_id, &decoded, &outcome, NULL, result_length);
    }
    if (outcome.note != NULL) {
        return encode_result(result, result_capacity, header.request_id, &decoded, &outcome, outcome.note, result_length);
    }
    (void)snprintf(message, sizeof(message), "launch failed: Win32 error %lu", (unsigned long)outcome.error);
    return encode_result(result, result_capacity, header.request_id, &decoded, &outcome, message, result_length);
}
