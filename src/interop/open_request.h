#ifndef WaddleInteropOpenRequestH
#define WaddleInteropOpenRequestH
/** @file open_request.h @brief Pure `waddle open` argv parser, quoting and exit-code C ABI. */
#include <stddef.h>
#include <stdint.h>
#include "pe_header.h"

/** @brief Maximum argv entries after the `open` word. */
#define InteropOpenMaxArgs 64u
/** @brief Maximum bytes per argv entry, excluding its NUL. */
#define InteropOpenMaxArgBytes 4096u
/** @brief Maximum launch items in one request. */
#define InteropOpenMaxItems 64u
/** @brief Maximum device name bytes, excluding NUL. */
#define InteropOpenMaxDeviceBytes 63u
/** @brief Maximum app key bytes, excluding NUL. */
#define InteropOpenMaxAppKeyBytes 200u
/** @brief Maximum quoted argument tail bytes, excluding NUL. */
#define InteropOpenMaxQuotedBytes 4096u
/** @brief Bytes of executable prefix passed to the read callback. */
#define InteropOpenPrefixBytes 4096u

/** @brief Frozen process exit codes (IMPL_DESC 3.9.1). */
typedef enum interop_open_exit_t {
    InteropOpenExitOk = 0, /**< Launch handed off, or Ok LaunchResult under --wait. */
    InteropOpenExitGuestFailure = 1, /**< Guest launch failure. */
    InteropOpenExitUsage = 2, /**< Usage error. */
    InteropOpenExitLocalError = 125, /**< Local error or guest unreachable within 120 s. */
    InteropOpenExitNotExecutable = 126, /**< Outside share, not executable, or image unreadable. */
    InteropOpenExitNotFound = 127 /**< Local path not found. */
} interop_open_exit_t;

/** @brief Parse, local and caller outcome codes; zero is success. */
typedef enum interop_open_error_t {
    InteropOpenErrorNone = 0, /**< Success; request output is valid. */
    InteropOpenErrorInvalidArgument = 1, /**< NULL output, options or argv pointer; output untouched. Maps to 125. */
    InteropOpenErrorUnknownOption = 2, /**< Unrecognized token beginning with `-`. */
    InteropOpenErrorDuplicateOption = 3, /**< Single-occurrence option or flag repeated. */
    InteropOpenErrorMissingOptionValue = 4, /**< Value option without a value, or value begins with `-`. */
    InteropOpenErrorInvalidDevice = 5, /**< Device name outside 1..63 bytes of [A-Za-z0-9_-]. */
    InteropOpenErrorInvalidAppKey = 6, /**< App key outside the codec app_key rule. */
    InteropOpenErrorOptionAfterTarget = 7, /**< Option token after a PATH or `--`. */
    InteropOpenErrorMissingTarget = 8, /**< No --app, PATH or `--` target. */
    InteropOpenErrorMixedTargets = 9, /**< More than one target form. */
    InteropOpenErrorEmptyPath = 10, /**< Zero-length PATH token. */
    InteropOpenErrorEmptyExecutable = 11, /**< `--` without a nonempty EXE token. */
    InteropOpenErrorExecutableNotExe = 12, /**< `--` EXE token not ending in `.exe`. */
    InteropOpenErrorTooManyArgs = 13, /**< More than 64 argv entries. */
    InteropOpenErrorArgumentTooLong = 14, /**< An argv entry longer than 4096 bytes. */
    InteropOpenErrorQuotedArgumentsTooLong = 15, /**< Quoted `--` argument tail above 4096 bytes. */
    InteropOpenErrorImageUnreadable = 16, /**< Read callback failed, absent or reported an invalid length. */
    InteropOpenErrorOutsideShare = 17, /**< Caller-reported: outside every share (126). */
    InteropOpenErrorNotFound = 18, /**< Caller-reported: local path missing (127). */
    InteropOpenErrorLocalFailure = 19, /**< Caller-reported: local failure (125). */
    InteropOpenErrorGuestUnreachable = 20, /**< Caller-reported: guest not reachable in 120 s (125). */
    InteropOpenErrorGuestLaunchFailed = 21, /**< Caller-reported: guest launch failure (1). */
    InteropOpenErrorDelegateConflict = 22, /**< Usage: several DelegateExec items, or DelegateExec mixed with sent items. Reported after the read callback. */
    InteropOpenErrorInvalidUtf8 = 23 /**< Usage: PATH, EXE or argument-tail bytes are not valid UTF-8 (codec requirement). */
} interop_open_error_t;

/** @brief Launch item kinds; Unused is the zero value of slots beyond item_count. */
typedef enum interop_open_item_kind_t {
    InteropOpenItemUnused = 0, /**< Empty slot; never valid below item_count. */
    InteropOpenItemApp = 1, /**< Catalog app key from --app. */
    InteropOpenItemPath = 2, /**< GUI executable or unknown file type; ShellExecute open. */
    InteropOpenItemConsole = 3, /**< Console executable for a new guest console window. */
    InteropOpenItemInstaller = 4, /**< .msi file. */
    InteropOpenItemShortcut = 5, /**< .lnk file. */
    InteropOpenItemExeWithArgs = 6, /**< GUI executable from the `--` form with an argument tail. */
    InteropOpenItemDelegateExec = 7 /**< Console executable delegated to local waddle exec; never sent. */
} interop_open_item_kind_t;

/**
 * @brief Read callback that fills the leading bytes of one executable path.
 * @param[in] context Opaque pointer from interop_open_options_t, forwarded unchanged.
 * @param[in] path Borrowed path bytes, not necessarily NUL terminated.
 * @param[in] path_length Number of bytes in path, 1..4096.
 * @param[out] buffer Writable parser-owned buffer of capacity bytes, valid for this call only.
 * @param[in] capacity Bytes writable in buffer; always InteropOpenPrefixBytes.
 * @param[out] length_out Nonnull; receives the byte count written, 0..capacity.
 * @return 0 on success; 1 when the path does not exist (NotFound, exit 127); any other
 *         nonzero value when the file cannot be read (ImageUnreadable, exit 126).
 * @note Runs synchronously on the calling thread. Must not retain any pointer.
 */
typedef int (*interop_open_read_prefix_fn_t)(void *context, const char *path, size_t path_length, uint8_t *buffer, size_t capacity, size_t *length_out);

/** @brief Caller-supplied parse options; borrowed for one call. */
typedef struct interop_open_options_t {
    interop_open_read_prefix_fn_t read_prefix; /**< In, nullable; NULL makes every .exe item ImageUnreadable. */
    void *read_context; /**< In, opaque; never dereferenced by the parser. */
    uint32_t stdio_is_tty; /**< In; 1 only when stdin and stdout are both terminals. */
} interop_open_options_t;

/** @brief One launch item; text borrows the argv entry. */
typedef struct interop_open_item_t {
    interop_open_item_kind_t kind; /**< Out; classified kind. */
    interop_pe_kind_t image; /**< Out; Gui or Console routing for executables, Gui otherwise. */
    const char *text; /**< Out; app key or host path, borrowed from argv. */
    size_t text_length; /**< Out; byte count of text, excluding any NUL. */
    size_t argument_first; /**< Out; argv index of the first tail token, or 0 when argument_count is 0. */
    size_t argument_count; /**< Out; number of argument-tail tokens after the EXE token. */
} interop_open_item_t;

/** @brief Parsed request. Caller-owned; written on success, zeroed on errors other than InvalidArgument. */
typedef struct interop_open_request_t {
    size_t item_count; /**< Out; valid entries in items, 0..64. */
    uint32_t wait; /**< Out; 1 when --wait was given. */
    uint32_t new_console; /**< Out; 1 when --new-console was given. */
    const char *device; /**< Out, nullable; borrowed device name, NULL when absent. */
    size_t device_length; /**< Out; device byte count, 0 when device is NULL. */
    interop_open_item_t items[InteropOpenMaxItems]; /**< Out; first item_count entries are valid. */
} interop_open_request_t;

_Static_assert(sizeof(interop_open_item_t) == 40u, "interop_open_item_t layout is shared with open_request.zig");
_Static_assert(sizeof(interop_open_request_t) == 2592u, "interop_open_request_t layout is shared with open_request.zig");
_Static_assert(sizeof(interop_open_options_t) == 24u, "interop_open_options_t layout is shared with open_request.zig");

/**
 * @brief Parse argv tokens that follow `open` into a request.
 * @param[in] argc Number of argv entries, 0..64 valid; above 64 yields TooManyArgs.
 * @param[in] argv NULL only when argc is 0. Entries are NUL-terminated and readable up to
 *            their NUL or 4097 bytes, whichever is first.
 * @param[in] options Nonnull options. A NULL read callback is permitted.
 * @param[out] request Nonnull output. Untouched on InvalidArgument; zeroed on other errors.
 * @return An interop_open_error_t value; 0 on success.
 * @note Pure and reentrant. No allocation. argv, device and item text borrow caller storage.
 */
int interop_open_parse(size_t argc, const char *const *argv, const interop_open_options_t *options, interop_open_request_t *request);

/**
 * @brief Write the waddle_quote form of an argument tail.
 * @param[in] argc Number of tokens, 0..64. Each token is at most 4096 bytes.
 * @param[in] argv NULL only when argc is 0. Must not overlap out.
 * @param[out] out Nonnull destination of at least capacity bytes.
 * @param[in] capacity Bytes available in out, including the terminating NUL.
 * @param[out] length_out Nonnull; receives the length excluding the NUL on success.
 * @return 0 Ok with NUL written; 1 InvalidArgument; 2 quoted output above 4096 bytes;
 *         3 NoSpace (capacity too small, including capacity 0).
 * @note Pure and reentrant. Output is byte-identical to waddle_quote() for every input
 *       whose quoted form fits in 4096 bytes.
 */
int interop_open_quote(size_t argc, const char *const *argv, char *out, size_t capacity, size_t *length_out);

/**
 * @brief Map an interop_open_error_t to a process exit code.
 * @param[in] error Any interop_open_error_t value; unknown values map to 125.
 * @return One of the interop_open_exit_t values.
 * @note Pure and thread-safe.
 */
uint8_t interop_open_exit_code(uint32_t error);

/**
 * @brief Map a LaunchResult status (interop_status_t) to a --wait exit code.
 * @param[in] status Wire status value.
 * @return InteropOpenExitOk for status 0; InteropOpenExitGuestFailure otherwise.
 * @note Pure and thread-safe.
 */
uint8_t interop_open_launch_exit_code(uint32_t status);

#endif
