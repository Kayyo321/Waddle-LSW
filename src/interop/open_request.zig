//! Pure `waddle open` request parser: frozen argv grammar, PE routing through a
//! caller callback, `waddle_quote` port and exit-code mappings. Performs no I/O.
const std = @import("std");
const pe_header = @import("pe_header.zig");

/// Maximum argv entries after the `open` word.
pub const MaxArgs: usize = 64;
/// Maximum bytes per argv entry, excluding the terminating NUL.
pub const MaxArgBytes: usize = 4096;
/// Maximum launch items; equal to MaxArgs because each item consumes one entry.
pub const MaxItems: usize = 64;
/// Maximum device name bytes; the daemon limit WaddleMaxDeviceNameLen 64 includes NUL.
pub const MaxDeviceBytes: usize = 63;
/// Maximum app key bytes, matching InteropMaxAppKeyBytes.
pub const MaxAppKeyBytes: usize = 200;
/// Maximum quoted argument tail bytes, matching InteropMaxStringBytes.
pub const MaxQuotedBytes: usize = 4096;
/// Leading bytes of each executable handed to the read callback.
pub const PrefixBytes: usize = pe_header.PrefixBytes;

/// Exit code 0: launch handed off, or a successful LaunchResult under --wait.
pub const ExitOk: u8 = 0;
/// Exit code 1: guest launch failure.
pub const ExitGuestFailure: u8 = 1;
/// Exit code 2: command line usage error.
pub const ExitUsage: u8 = 2;
/// Exit code 125: local error or guest unreachable within the 120 s window.
pub const ExitLocalError: u8 = 125;
/// Exit code 126: outside share, not executable, or image unreadable.
pub const ExitNotExecutable: u8 = 126;
/// Exit code 127: local path not found.
pub const ExitNotFound: u8 = 127;

/// Parse, mapping and caller outcome codes. Zero is success; values are the C ABI codes.
pub const open_error_t = enum(u32) {
    /// Success; the request output is valid.
    None = 0,
    /// C ABI NULL output, options or argv pointer, a caller defect. Maps to 125.
    InvalidArgument = 1,
    /// Token beginning with `-` that is not a recognized option.
    UnknownOption = 2,
    /// A single-occurrence option or flag was repeated.
    DuplicateOption = 3,
    /// A value option had no value, or its value begins with `-`.
    MissingOptionValue = 4,
    /// Device name empty, longer than 63 bytes, or outside `[A-Za-z0-9_-]`.
    InvalidDevice = 5,
    /// App key outside the codec app_key rule or longer than 200 bytes.
    InvalidAppKey = 6,
    /// Option token appeared after a PATH or `--`.
    OptionAfterTarget = 7,
    /// No `--app`, PATH or `--` target was present.
    MissingTarget = 8,
    /// Two different target forms were given.
    MixedTargets = 9,
    /// A PATH token had zero length.
    EmptyPath = 10,
    /// `--` was missing its EXE token or the EXE token was empty.
    EmptyExecutable = 11,
    /// The `--` EXE token did not end in `.exe` (case-insensitive).
    ExecutableNotExe = 12,
    /// More than 64 argv entries.
    TooManyArgs = 13,
    /// An argv entry was longer than 4096 bytes.
    ArgumentTooLong = 14,
    /// The quoted `--` argument tail exceeded 4096 bytes.
    QuotedArgumentsTooLong = 15,
    /// The read callback failed, was absent, or reported an invalid length.
    ImageUnreadable = 16,
    /// Caller-reported: path is outside every share (exit 126).
    OutsideShare = 17,
    /// Caller-reported: local path does not exist (exit 127).
    NotFound = 18,
    /// Caller-reported: local failure (exit 125).
    LocalFailure = 19,
    /// Caller-reported: guest not reachable within 120 s (exit 125).
    GuestUnreachable = 20,
    /// Caller-reported: guest reported a launch failure (exit 1).
    GuestLaunchFailed = 21,
    /// Usage: more than one DelegateExec item, or DelegateExec mixed with sent items.
    /// Depends on classification, so it is reported after the read callback has run.
    DelegateConflict = 22,
    /// Usage: a PATH, the EXE token or an argument-tail token is not valid UTF-8.
    /// The codec requires UTF-8 for TargetPath and Arguments, so the request could not be sent.
    InvalidUtf8 = 23,
};

/// Launch item kind before path translation. Unused is the zero value of empty slots.
pub const item_kind_t = enum(u32) {
    /// Empty slot beyond item_count; never produced for a valid index.
    Unused = 0,
    /// Catalog application key (`--app`).
    App = 1,
    /// GUI executable or unknown file type, launched with ShellExecute "open".
    Path = 2,
    /// Console executable routed to a new guest console window.
    Console = 3,
    /// `.msi` file, launched through msiexec.
    Installer = 4,
    /// `.lnk` shortcut.
    Shortcut = 5,
    /// GUI executable from the `--` form, carrying an argument tail.
    ExeWithArgs = 6,
    /// Console executable delegated locally to `waddle exec`; never sent on the wire.
    DelegateExec = 7,

    /// Map to the wire `launch_kind` (IMPL_DESC 3.2.5). Returns null for DelegateExec.
    /// @param self Item kind; no ownership. Thread-safe.
    /// @return Wire value 1..5, or null when the item is not sent.
    pub fn wire_launch_kind(self: item_kind_t) ?u8 {
        return switch (self) {
            .Unused, .DelegateExec => null,
            .App => 1,
            .Path, .ExeWithArgs => 2,
            .Installer => 3,
            .Shortcut => 4,
            .Console => 5,
        };
    }
};

/// Callback status: the prefix was read.
pub const ReadStatusOk: c_int = 0;
/// Callback status: the path does not exist (parser reports NotFound, exit 127).
pub const ReadStatusNotFound: c_int = 1;

/// Read callback: fills up to `capacity` leading bytes of one path and reports the count.
/// Returns ReadStatusOk on success, ReadStatusNotFound when the path is missing, and any
/// other nonzero value when the file exists but cannot be read (exit 126). The callee must
/// not retain pointers.
pub const read_prefix_fn_t = *const fn (?*anyopaque, [*]const u8, usize, [*]u8, usize, *usize) callconv(.C) c_int;

/// Caller-supplied options; borrowed for the duration of one parse call.
pub const options_t = extern struct {
    /// Read callback for `.exe` items; null makes every `.exe` item an ImageUnreadable error.
    read_prefix: ?read_prefix_fn_t,
    /// Opaque pointer forwarded unchanged to read_prefix; the parser never dereferences it.
    read_context: ?*anyopaque,
    /// 1 only when stdin and stdout are both terminals; 0 otherwise.
    stdio_is_tty: u32,
};

/// One launch item. `text` borrows the argv entry; valid while argv storage lives.
pub const item_t = extern struct {
    /// Item kind after classification.
    kind: item_kind_t,
    /// Gui or Console routing value from the PE classifier; Gui for non-executables.
    image: pe_header.image_kind_t,
    /// App key or host path bytes; null only in unused slots.
    text: ?[*]const u8,
    /// Byte length of `text`, excluding any terminator.
    text_length: usize,
    /// argv index of the first argument-tail token; 0 when argument_count is 0.
    argument_first: usize,
    /// Number of argument-tail tokens following the EXE token.
    argument_count: usize,
};

/// Parsed request. Caller-owned storage; written by parse on success only.
pub const request_t = extern struct {
    /// Number of valid entries in items, 0..64.
    item_count: usize,
    /// 1 when --wait was given.
    wait: u32,
    /// 1 when --new-console was given.
    new_console: u32,
    /// Borrowed device name, or null when --device was absent.
    device: ?[*]const u8,
    /// Byte length of device; 0 when device is null.
    device_length: usize,
    /// Launch items in argv order; only the first item_count entries are valid.
    items: [MaxItems]item_t,

    /// Borrow the text of one item. Returns an empty slice for null text.
    /// @param self Request; not modified.
    /// @param index Item index below item_count (caller-checked).
    /// @return Slice borrowed from argv; valid while argv lives.
    pub fn item_text(self: *const request_t, index: usize) []const u8 {
        const item = self.items[index];
        const pointer = item.text orelse return &.{};
        return pointer[0..item.text_length];
    }
};

/// Quoting failures; both are reported as usage or invalid argument by callers.
pub const quote_error_t = error{ QuotedTooLarge, NoSpace };

/// Parse argv tokens that follow `open` into `out`. The grammar, bounds and
/// conflict rules are those of open_request_contract.md sections 1-4.
/// @param args Borrowed argument tokens, each without a NUL terminator; count and
///   lengths are checked before grammar decisions.
/// @param options Borrowed options; read_prefix is called only for `.exe` items.
/// @param out Caller-owned output, always fully written: the parsed request on
///   success, all zeroes on error.
/// @return open_error_t.None on success, otherwise the first grammar or local error.
/// @note Pure and thread-safe for independent outputs. Allocation-free.
pub fn parse(args: []const []const u8, options: *const options_t, out: *request_t) open_error_t {
    var result = std.mem.zeroes(request_t);
    const status = parse_tokens(args, options, &result);
    out.* = if (status == .None) result else std.mem.zeroes(request_t);
    return status;
}

/// Exact length of the quoted form of args; fails above MaxQuotedBytes.
/// @param args Borrowed argument tokens; may be empty (length 0).
/// @return Quoted length in bytes, excluding any NUL, or QuotedTooLarge.
/// @note Pure and thread-safe.
pub fn quoted_length(args: []const []const u8) quote_error_t!usize {
    return quote_core(args, null);
}

/// Write the `waddle_quote` form of args into out. Port of src/common/arguments.c.
/// @param args Borrowed argument tokens. Each is wrapped in double quotes and
///   separated by one space; an empty list yields an empty output.
/// @param out Caller-owned destination; not NUL-terminated by this function.
/// @return Number of bytes written, or QuotedTooLarge / NoSpace. On error the
///   destination may hold partial bytes and must not be used.
/// @note Pure and thread-safe for independent destinations.
pub fn quote_into(args: []const []const u8, out: []u8) quote_error_t!usize {
    return quote_core(args, out);
}

/// Map a parse, local or caller outcome to the frozen process exit code.
/// @param err Any open_error_t value; every value is listed exhaustively.
/// @return One of the Exit* constants.
/// @note Pure and thread-safe.
pub fn exit_code_for_error(err: open_error_t) u8 {
    return switch (err) {
        .None => ExitOk,
        .InvalidArgument, .LocalFailure, .GuestUnreachable => ExitLocalError,
        .UnknownOption,
        .DuplicateOption,
        .MissingOptionValue,
        .InvalidDevice,
        .InvalidAppKey,
        .OptionAfterTarget,
        .MissingTarget,
        .MixedTargets,
        .EmptyPath,
        .EmptyExecutable,
        .ExecutableNotExe,
        .TooManyArgs,
        .ArgumentTooLong,
        .QuotedArgumentsTooLong,
        .DelegateConflict,
        .InvalidUtf8,
        => ExitUsage,
        .ImageUnreadable, .OutsideShare => ExitNotExecutable,
        .NotFound => ExitNotFound,
        .GuestLaunchFailed => ExitGuestFailure,
    };
}

/// Map a LaunchResult status (interop_status_t) to a `--wait` exit code.
/// @param status Wire status value; unknown values are treated as failures.
/// @return ExitOk only for InteropOk (0); ExitGuestFailure for every other value.
/// @note Pure and thread-safe. See open_request_contract.md section 5.
pub fn exit_code_for_launch_status(status: u32) u8 {
    return if (status == 0) ExitOk else ExitGuestFailure;
}

/// C ABI parse. Each argv entry is read up to its NUL or 4097 bytes at most.
/// @param[in] argc Number of argv entries after `open`, 0..64 valid.
/// @param[in] argv Nullable only when argc is 0. Entries are NUL-terminated.
/// @param[in] options Nonnull options; read callback may be NULL.
/// @param[out] request Nonnull output; untouched on InvalidArgument, zeroed on other errors.
/// @return interop_open_error_t value as int; 0 on success.
/// @note Reentrant; output and option storage belong to the caller.
export fn interop_open_parse(argc: usize, argv: ?[*]const [*:0]const u8, options: ?*const options_t, request: ?*request_t) c_int {
    const out = request orelse return error_code(open_error_t.InvalidArgument);
    const opts = options orelse return error_code(open_error_t.InvalidArgument);
    if (argc > MaxArgs) {
        out.* = std.mem.zeroes(request_t);
        return error_code(open_error_t.TooManyArgs);
    }
    if (argc != 0 and argv == null) return error_code(open_error_t.InvalidArgument);
    var storage: [MaxArgs][]const u8 = undefined;
    for (0..argc) |index| storage[index] = span(argv.?[index]);
    return error_code(parse(storage[0..argc], opts, out));
}

/// C ABI quote. Writes the NUL terminator when capacity allows.
/// @param[in] argc Number of argument tokens, 0..64.
/// @param[in] argv Nullable only when argc is 0. Entries are NUL-terminated, at most 4096 bytes.
/// @param[out] out Nonnull destination of at least capacity bytes; must not overlap argv.
/// @param[in] capacity Bytes available in out, including the NUL.
/// @param[out] length_out Nonnull; receives the length excluding NUL on success.
/// @return 0 Ok, 1 InvalidArgument, 2 QuotedTooLarge, 3 NoSpace.
/// @note Reentrant; no allocation.
export fn interop_open_quote(argc: usize, argv: ?[*]const [*:0]const u8, out: ?[*]u8, capacity: usize, length_out: ?*usize) c_int {
    const destination = out orelse return 1;
    const length_ptr = length_out orelse return 1;
    if (argc > MaxArgs or (argc != 0 and argv == null)) return 1;
    var storage: [MaxArgs][]const u8 = undefined;
    for (0..argc) |index| {
        storage[index] = span(argv.?[index]);
        if (storage[index].len > MaxArgBytes) return 1;
    }
    if (capacity == 0) return 3;
    const length = quote_into(storage[0..argc], destination[0 .. capacity - 1]) catch |err| return switch (err) {
        error.QuotedTooLarge => 2,
        error.NoSpace => 3,
    };
    destination[length] = 0;
    length_ptr.* = length;
    return 0;
}

/// C ABI exit-code mapping. Unknown values map to ExitLocalError.
/// @param[in] error interop_open_error_t value.
/// @return Exit code in 0..127 or 125/126/127 as listed in the contract.
/// @note Pure and thread-safe.
export fn interop_open_exit_code(err: u32) u8 {
    const value = std.meta.intToEnum(open_error_t, err) catch return ExitLocalError;
    return exit_code_for_error(value);
}

/// C ABI LaunchResult status mapping.
/// @param[in] status interop_status_t value.
/// @return ExitOk for InteropOk, otherwise ExitGuestFailure.
/// @note Pure and thread-safe.
export fn interop_open_launch_exit_code(status: u32) u8 {
    return exit_code_for_launch_status(status);
}

fn error_code(err: open_error_t) c_int {
    return @intCast(@intFromEnum(err));
}

/// Result of the grammar pass: borrowed argv indices and flags. Filled without any I/O.
const plan_t = struct {
    app: ?[]const u8 = null,
    device: ?[]const u8 = null,
    wait: bool = false,
    new_console: bool = false,
    path_indices: [MaxItems]usize = undefined,
    path_count: usize = 0,
    exe_index: ?usize = null,
    tail_first: usize = 0,
    tail_count: usize = 0,
};

fn parse_tokens(args: []const []const u8, options: *const options_t, result: *request_t) open_error_t {
    var plan = plan_t{};
    const grammar = plan_tokens(args, &plan);
    if (grammar != .None) return grammar;
    return classify_plan(args, options, &plan, result);
}

/// Pass one: every usage and bounds decision over the whole argv. Performs no
/// classification and never calls the read callback.
fn plan_tokens(args: []const []const u8, plan: *plan_t) open_error_t {
    if (args.len > MaxArgs) return .TooManyArgs;
    for (args) |arg| if (arg.len > MaxArgBytes) return .ArgumentTooLong;
    if (args.len == 0) return .MissingTarget;
    var index: usize = 0;
    while (index < args.len) {
        const token = args[index];
        if (eql(token, "--")) {
            if (plan.app != null or plan.path_count > 0) return .MixedTargets;
            return plan_exe(args, index + 1, plan);
        } else if (eql(token, "--app")) {
            if (plan.path_count > 0) return .MixedTargets;
            if (plan.app != null) return .DuplicateOption;
            const value = value_at(args, index + 1) orelse return .MissingOptionValue;
            if (!app_key_valid(value)) return .InvalidAppKey;
            plan.app = value;
            index += 2;
        } else if (eql(token, "--device") or eql(token, "-d")) {
            if (plan.app != null or plan.path_count > 0) return .OptionAfterTarget;
            if (plan.device != null) return .DuplicateOption;
            const value = value_at(args, index + 1) orelse return .MissingOptionValue;
            if (!device_valid(value)) return .InvalidDevice;
            plan.device = value;
            index += 2;
        } else if (eql(token, "--wait") or eql(token, "--new-console")) {
            if (plan.app != null or plan.path_count > 0) return .OptionAfterTarget;
            const flag = if (eql(token, "--wait")) &plan.wait else &plan.new_console;
            if (flag.*) return .DuplicateOption;
            flag.* = true;
            index += 1;
        } else if (token.len > 0 and token[0] == '-') {
            return .UnknownOption;
        } else {
            if (token.len == 0) return .EmptyPath;
            if (plan.app != null) return .MixedTargets;
            if (!std.unicode.utf8ValidateSlice(token)) return .InvalidUtf8;
            plan.path_indices[plan.path_count] = index;
            plan.path_count += 1;
            index += 1;
        }
    }
    if (plan.app == null and plan.path_count == 0) return .MissingTarget;
    return .None;
}

fn plan_exe(args: []const []const u8, exe_index: usize, plan: *plan_t) open_error_t {
    if (exe_index >= args.len) return .EmptyExecutable;
    const exe = args[exe_index];
    if (exe.len == 0) return .EmptyExecutable;
    if (!is_exe_path(exe)) return .ExecutableNotExe;
    if (!std.unicode.utf8ValidateSlice(exe)) return .InvalidUtf8;
    const tail = args[exe_index + 1 ..];
    for (tail) |arg| if (!std.unicode.utf8ValidateSlice(arg)) return .InvalidUtf8;
    _ = quoted_length(tail) catch return .QuotedArgumentsTooLong;
    plan.exe_index = exe_index;
    plan.tail_first = if (tail.len == 0) 0 else exe_index + 1;
    plan.tail_count = tail.len;
    return .None;
}

/// Pass two: classify the planned items. The only place the read callback runs.
fn classify_plan(args: []const []const u8, options: *const options_t, plan: *const plan_t, result: *request_t) open_error_t {
    var count: usize = 0;
    if (plan.app) |key| {
        result.items[0] = .{ .kind = .App, .image = .Gui, .text = key.ptr, .text_length = key.len, .argument_first = 0, .argument_count = 0 };
        count = 1;
    } else if (plan.exe_index) |exe_index| {
        const exe = args[exe_index];
        var kind: item_kind_t = .Unused;
        var image: pe_header.image_kind_t = .Gui;
        const status = route_exe(exe, options, plan.new_console, .ExeWithArgs, &kind, &image);
        if (status != .None) return status;
        result.items[0] = .{ .kind = kind, .image = image, .text = exe.ptr, .text_length = exe.len, .argument_first = plan.tail_first, .argument_count = plan.tail_count };
        count = 1;
    } else {
        for (plan.path_indices[0..plan.path_count]) |index| {
            const token = args[index];
            var kind: item_kind_t = .Unused;
            var image: pe_header.image_kind_t = .Gui;
            const status = route_path(token, options, plan.new_console, &kind, &image);
            if (status != .None) return status;
            result.items[count] = .{ .kind = kind, .image = image, .text = token.ptr, .text_length = token.len, .argument_first = 0, .argument_count = 0 };
            count += 1;
        }
    }
    var delegated: usize = 0;
    for (result.items[0..count]) |item| delegated += @intFromBool(item.kind == .DelegateExec);
    if (delegated > 1 or (delegated == 1 and count > 1)) return .DelegateConflict;
    result.item_count = count;
    result.wait = @intFromBool(plan.wait);
    result.new_console = @intFromBool(plan.new_console);
    if (plan.device) |device| {
        result.device = device.ptr;
        result.device_length = device.len;
    }
    return .None;
}

fn route_path(path: []const u8, options: *const options_t, new_console: bool, kind: *item_kind_t, image: *pe_header.image_kind_t) open_error_t {
    const extension = extension_of(path);
    if (extension_is(extension, "msi")) {
        kind.* = .Installer;
        return .None;
    }
    if (extension_is(extension, "lnk")) {
        kind.* = .Shortcut;
        return .None;
    }
    if (extension_is(extension, "exe")) return route_exe(path, options, new_console, .Path, kind, image);
    kind.* = .Path;
    return .None;
}

fn route_exe(path: []const u8, options: *const options_t, new_console: bool, gui_kind: item_kind_t, kind: *item_kind_t, image: *pe_header.image_kind_t) open_error_t {
    var prefix: [PrefixBytes]u8 = undefined;
    var length: usize = 0;
    const read_status = read_header(path, options, &prefix, &length);
    if (read_status != .None) return read_status;
    image.* = pe_header.classify(prefix[0..length]);
    if (image.* == .Gui) {
        kind.* = gui_kind;
    } else if (options.stdio_is_tty != 0 and !new_console) {
        kind.* = .DelegateExec;
    } else {
        kind.* = .Console;
    }
    return .None;
}

fn read_header(path: []const u8, options: *const options_t, buffer: *[PrefixBytes]u8, length: *usize) open_error_t {
    const reader = options.read_prefix orelse return .ImageUnreadable;
    var reported: usize = 0;
    const status = reader(options.read_context, path.ptr, path.len, buffer, PrefixBytes, &reported);
    if (status == ReadStatusNotFound) return .NotFound;
    if (status != ReadStatusOk) return .ImageUnreadable;
    if (reported > PrefixBytes) return .ImageUnreadable;
    length.* = reported;
    return .None;
}

/// An option value is always the next token, whatever its spelling. Its own
/// validator (device name or app key) decides whether it is acceptable. A
/// missing next token is MissingOptionValue.
fn value_at(args: []const []const u8, index: usize) ?[]const u8 {
    if (index >= args.len) return null;
    return args[index];
}

fn device_valid(value: []const u8) bool {
    if (value.len == 0 or value.len > MaxDeviceBytes) return false;
    for (value) |byte| if (!ident_byte(byte)) return false;
    return true;
}

fn app_key_valid(value: []const u8) bool {
    if (value.len < 3 or value.len > MaxAppKeyBytes) return false;
    if (value[0] != 'a' and value[0] != 'p') return false;
    if (value[1] != '-') return false;
    for (value) |byte| if (!ident_byte(byte)) return false;
    return true;
}

fn ident_byte(byte: u8) bool {
    return std.ascii.isAlphanumeric(byte) or byte == '-' or byte == '_';
}

fn is_exe_path(path: []const u8) bool {
    return extension_is(extension_of(path), "exe");
}

fn extension_is(extension: []const u8, want: []const u8) bool {
    if (extension.len != want.len) return false;
    for (extension, want) |have, expected| {
        if (std.ascii.toLower(have) != expected) return false;
    }
    return true;
}

fn extension_of(path: []const u8) []const u8 {
    const slash = std.mem.lastIndexOfScalar(u8, path, '/');
    const name = if (slash) |position| path[position + 1 ..] else path;
    const dot = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    return name[dot + 1 ..];
}

fn eql(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}

fn span(pointer: [*:0]const u8) []const u8 {
    var length: usize = 0;
    while (length <= MaxArgBytes and pointer[length] != 0) : (length += 1) {}
    return pointer[0..length];
}

fn quote_put(out: ?[]u8, length: *usize, byte: u8) quote_error_t!void {
    if (length.* >= MaxQuotedBytes) return error.QuotedTooLarge;
    if (out) |buffer| {
        if (length.* >= buffer.len) return error.NoSpace;
        buffer[length.*] = byte;
    }
    length.* += 1;
}

fn quote_core(args: []const []const u8, out: ?[]u8) quote_error_t!usize {
    var length: usize = 0;
    for (args, 0..) |arg, index| {
        if (index > 0) try quote_put(out, &length, ' ');
        try quote_put(out, &length, '"');
        var position: usize = 0;
        while (position < arg.len) {
            var slashes: usize = 0;
            while (position < arg.len and arg[position] == '\\') : (position += 1) slashes += 1;
            const at_end = position == arg.len;
            const quote_next = !at_end and arg[position] == '"';
            var emitted: usize = if (at_end or quote_next) 2 * slashes else slashes;
            while (emitted > 0) : (emitted -= 1) try quote_put(out, &length, '\\');
            if (quote_next) try quote_put(out, &length, '\\');
            if (!at_end) {
                try quote_put(out, &length, arg[position]);
                position += 1;
            }
        }
        try quote_put(out, &length, '"');
    }
    return length;
}

// Test-only fixtures.
const testing = std.testing;

const fake_entry_t = struct {
    path: []const u8,
    bytes: []const u8 = &.{},
    status: c_int = 0,
    reported: ?usize = null,
};

const fake_reader_t = struct {
    entries: []const fake_entry_t,
    calls: usize = 0,
    last_length: usize = 0,
    paths: [MaxArgs][]const u8 = undefined,
};

fn fake_read(context: ?*anyopaque, path: [*]const u8, path_length: usize, buffer: [*]u8, capacity: usize, length_out: *usize) callconv(.C) c_int {
    const fake: *fake_reader_t = @ptrCast(@alignCast(context.?));
    fake.calls += 1;
    fake.last_length = path_length;
    if (fake.calls <= MaxArgs) fake.paths[fake.calls - 1] = path[0..path_length];
    for (fake.entries) |entry| {
        if (!eql(entry.path, path[0..path_length])) continue;
        if (entry.status != 0) return entry.status;
        @memcpy(buffer[0..entry.bytes.len], entry.bytes);
        std.debug.assert(entry.bytes.len <= capacity);
        length_out.* = entry.reported orelse entry.bytes.len;
        return 0;
    }
    return 2;
}

fn pe_image(buffer: *[256]u8, subsystem: u16) []const u8 {
    @memset(buffer, 0);
    buffer[0] = 'M';
    buffer[1] = 'Z';
    std.mem.writeInt(u32, buffer[0x3c..0x40], 0x80, .little);
    @memcpy(buffer[0x80..0x84], "PE\x00\x00");
    std.mem.writeInt(u16, buffer[0x94..0x96], 240, .little);
    std.mem.writeInt(u16, buffer[0x98..0x9a], 0x10b, .little);
    std.mem.writeInt(u16, buffer[0xdc..0xde], subsystem, .little);
    return buffer;
}

fn run(args: []const []const u8, fake: *fake_reader_t, tty: u32, out: *request_t) open_error_t {
    // Route the tokens through memory the optimizer cannot see into, so every
    // grammar branch is a run-time decision in the instrumented binary.
    var storage: [MaxArgs + 1][]const u8 = undefined;
    @memcpy(storage[0..args.len], args);
    std.mem.doNotOptimizeAway(&storage);
    const options = options_t{ .read_prefix = fake_read, .read_context = fake, .stdio_is_tty = tty };
    return parse(storage[0..args.len], &options, out);
}

/// Hide a value from the optimizer so that tests exercise run-time branches.
fn hide(comptime value_t: type, value: value_t) value_t {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

fn no_reader_options() options_t {
    return .{ .read_prefix = null, .read_context = null, .stdio_is_tty = 0 };
}

/// Decode one quoted group in the Win32 CommandLineToArgvW convention used by quote_core.
fn decode_group(quoted: []const u8, position: *usize, out: []u8, out_length: *usize) bool {
    if (position.* >= quoted.len or quoted[position.*] != '"') return false;
    position.* += 1;
    var in_quotes = true;
    while (in_quotes) {
        var slashes: usize = 0;
        while (position.* < quoted.len and quoted[position.*] == '\\') : (position.* += 1) slashes += 1;
        if (position.* >= quoted.len) return false;
        const byte = quoted[position.*];
        position.* += 1;
        if (byte == '"') {
            for (0..slashes / 2) |_| {
                out[out_length.*] = '\\';
                out_length.* += 1;
            }
            if (slashes % 2 == 1) {
                out[out_length.*] = '"';
                out_length.* += 1;
            } else {
                in_quotes = false;
            }
        } else {
            for (0..slashes) |_| {
                out[out_length.*] = '\\';
                out_length.* += 1;
            }
            out[out_length.*] = byte;
            out_length.* += 1;
        }
    }
    return true;
}

test "grammar: app target with device and flags in every legal order" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    const form_t = struct { args: []const []const u8, key: []const u8 };
    const forms = [_]form_t{
        .{ .args = &.{ "--app", "a-Microsoft_Notepad" }, .key = "a-Microsoft_Notepad" },
        .{ .args = &.{ "-d", "dev-1", "--wait", "--new-console", "--app", "p-x" }, .key = "p-x" },
        .{ .args = &.{ "--device", "dev_2", "--wait", "--app", "a-x" }, .key = "a-x" },
        .{ .args = &.{ "--new-console", "--wait", "-d", "d", "--app", "a-Q" }, .key = "a-Q" },
    };
    for (forms) |form| {
        try testing.expectEqual(open_error_t.None, run(form.args, &fake, 0, &out));
        try testing.expectEqual(@as(usize, 1), out.item_count);
        try testing.expectEqual(item_kind_t.App, out.items[0].kind);
        try testing.expectEqual(pe_header.image_kind_t.Gui, out.items[0].image);
        try testing.expectEqual(@as(usize, 0), out.items[0].argument_count);
        try testing.expectEqualSlices(u8, form.key, out.item_text(0));
    }
    try testing.expectEqual(@as(usize, 0), fake.calls);
    _ = run(&.{ "-d", "dev-1", "--wait", "--new-console", "--app", "p-x" }, &fake, 0, &out);
    try testing.expectEqualSlices(u8, "dev-1", out.device.?[0..out.device_length]);
    try testing.expectEqual(@as(u32, 1), out.wait);
    try testing.expectEqual(@as(u32, 1), out.new_console);
    _ = run(&.{ "--app", "a-x" }, &fake, 0, &out);
    try testing.expect(out.device == null);
    try testing.expectEqual(@as(usize, 0), out.device_length);
    try testing.expectEqual(@as(u32, 0), out.wait);
    try testing.expectEqualSlices(u8, "a-x", out.item_text(0));
}

test "grammar: plain paths, multiple paths and non-executable extensions" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{ "/home/u/notes.txt", "/tmp/a.b", "noext", "trailing.", ".hidden" }, &fake, 1, &out));
    try testing.expectEqual(@as(usize, 5), out.item_count);
    for (0..5) |index| {
        try testing.expectEqual(item_kind_t.Path, out.items[index].kind);
        try testing.expectEqual(pe_header.image_kind_t.Gui, out.items[index].image);
        try testing.expectEqual(@as(usize, 0), out.items[index].argument_first);
    }
    try testing.expectEqualSlices(u8, "/tmp/a.b", out.item_text(1));
    try testing.expectEqual(@as(usize, 0), fake.calls);
}

test "grammar: the --dash form carries EXE and verbatim argument tail" {
    var pe_buffer: [256]u8 = undefined;
    const gui = pe_image(&pe_buffer, 2);
    const entries = [_]fake_entry_t{.{ .path = "C:\\setup.exe", .bytes = gui }};
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    const args = [_][]const u8{ "--", "C:\\setup.exe", "--wait", "", "--", "a b" };
    try testing.expectEqual(open_error_t.None, run(&args, &fake, 1, &out));
    try testing.expectEqual(@as(usize, 1), out.item_count);
    try testing.expectEqual(item_kind_t.ExeWithArgs, out.items[0].kind);
    try testing.expectEqual(@as(usize, 2), out.items[0].argument_first);
    try testing.expectEqual(@as(usize, 4), out.items[0].argument_count);
    try testing.expectEqualSlices(u8, "C:\\setup.exe", out.item_text(0));
    try testing.expectEqual(@as(u32, 0), out.wait);
}

test "grammar: the --dash form with no argument tail uses first 0 and count 0" {
    var pe_buffer: [256]u8 = undefined;
    const gui = pe_image(&pe_buffer, 2);
    const entries = [_]fake_entry_t{.{ .path = "x.EXE", .bytes = gui }};
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{ "--", "x.EXE" }, &fake, 0, &out));
    try testing.expectEqual(item_kind_t.ExeWithArgs, out.items[0].kind);
    try testing.expectEqual(@as(usize, 0), out.items[0].argument_first);
    try testing.expectEqual(@as(usize, 0), out.items[0].argument_count);
}

test "grammar: installer and shortcut extensions are routed without reading" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{ "/x/setup.MSI", "/x/app.Lnk" }, &fake, 0, &out));
    try testing.expectEqual(item_kind_t.Installer, out.items[0].kind);
    try testing.expectEqual(item_kind_t.Shortcut, out.items[1].kind);
    try testing.expectEqual(@as(usize, 0), fake.calls);
}

test "usage errors each report their first triggering token" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    const cases = [_]struct { args: []const []const u8, expected: open_error_t }{
        .{ .args = &.{}, .expected = .MissingTarget },
        .{ .args = &.{ "--wait", "--new-console" }, .expected = .MissingTarget },
        .{ .args = &.{"--bogus"}, .expected = .UnknownOption },
        .{ .args = &.{"-"}, .expected = .UnknownOption },
        .{ .args = &.{"--device=dev"}, .expected = .UnknownOption },
        .{ .args = &.{ "--app", "a-x", "--app", "a-y" }, .expected = .DuplicateOption },
        .{ .args = &.{ "-d", "a", "--device", "b", "--app", "a-x" }, .expected = .DuplicateOption },
        .{ .args = &.{ "--wait", "--wait", "--app", "a-x" }, .expected = .DuplicateOption },
        .{ .args = &.{ "--new-console", "--new-console", "a.txt" }, .expected = .DuplicateOption },
        .{ .args = &.{"--app"}, .expected = .MissingOptionValue },
        .{ .args = &.{ "--app", "--wait" }, .expected = .InvalidAppKey },
        .{ .args = &.{"-d"}, .expected = .MissingOptionValue },
        .{ .args = &.{ "--wait", "-d" }, .expected = .MissingOptionValue },
        .{ .args = &.{ "--app", "" }, .expected = .InvalidAppKey },
        .{ .args = &.{ "--app", "a" }, .expected = .InvalidAppKey },
        .{ .args = &.{ "--app", "ax-y" }, .expected = .InvalidAppKey },
        .{ .args = &.{ "--app", "b-y" }, .expected = .InvalidAppKey },
        .{ .args = &.{ "--app", "a-x y" }, .expected = .InvalidAppKey },
        .{ .args = &.{ "-d", "" , "--app", "a-x" }, .expected = .InvalidDevice },
        .{ .args = &.{ "-d", "bad name", "--app", "a-x" }, .expected = .InvalidDevice },
        .{ .args = &.{ "-d", "dot.dev", "--app", "a-x" }, .expected = .InvalidDevice },
        .{ .args = &.{ "--app", "a-x", "a.txt" }, .expected = .MixedTargets },
        .{ .args = &.{ "a.txt", "--app", "a-x" }, .expected = .MixedTargets },
        .{ .args = &.{ "a.txt", "--", "x.exe" }, .expected = .MixedTargets },
        .{ .args = &.{ "--app", "a-x", "--", "x.exe" }, .expected = .MixedTargets },
        .{ .args = &.{ "a.txt", "--wait" }, .expected = .OptionAfterTarget },
        .{ .args = &.{ "a.txt", "-d", "dev" }, .expected = .OptionAfterTarget },
        .{ .args = &.{ "a.txt", "--device", "dev" }, .expected = .OptionAfterTarget },
        .{ .args = &.{ "a.txt", "--new-console" }, .expected = .OptionAfterTarget },
        .{ .args = &.{""}, .expected = .EmptyPath },
        .{ .args = &.{"--"}, .expected = .EmptyExecutable },
        .{ .args = &.{ "--", "" }, .expected = .EmptyExecutable },
        .{ .args = &.{ "--", "x.txt" }, .expected = .ExecutableNotExe },
        .{ .args = &.{ "--", "x" }, .expected = .ExecutableNotExe },
        .{ .args = &.{ "--", "x.msi" }, .expected = .ExecutableNotExe },
        .{ .args = &.{ "--", "--app" }, .expected = .ExecutableNotExe },
    };
    for (cases) |case| {
        try testing.expectEqual(case.expected, run(case.args, &fake, 0, &out));
        try testing.expectEqual(@as(usize, 0), out.item_count);
        try testing.expect(out.device == null);
    }
    try testing.expectEqual(@as(usize, 0), fake.calls);
}

test "usage errors are reported in grammar order over bounds" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.UnknownOption, run(&.{ "--zzz", "-d" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.MissingOptionValue, run(&.{ "-d" }, &fake, 0, &out));
    const long = [_]u8{'a'} ** (MaxArgBytes + 1);
    try testing.expectEqual(open_error_t.ArgumentTooLong, run(&.{ "--zzz", &long }, &fake, 0, &out));
}

test "boundaries: 64 arguments accepted and 65 rejected" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    var sixty_four: [MaxArgs][]const u8 = undefined;
    for (&sixty_four) |*slot| slot.* = "f.txt";
    try testing.expectEqual(open_error_t.None, run(&sixty_four, &fake, 0, &out));
    try testing.expectEqual(MaxItems, out.item_count);
    var sixty_five: [MaxArgs + 1][]const u8 = undefined;
    for (&sixty_five) |*slot| slot.* = "f.txt";
    var count: usize = MaxArgs + 1;
    std.mem.doNotOptimizeAway(&count);
    try testing.expectEqual(open_error_t.TooManyArgs, run(sixty_five[0..count], &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), out.item_count);
}

test "boundaries: 4096-byte argument accepted and 4097 rejected" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    const exact = [_]u8{'p'} ** MaxArgBytes;
    try testing.expectEqual(open_error_t.None, run(&.{&exact}, &fake, 0, &out));
    try testing.expectEqual(@as(usize, MaxArgBytes), out.item_text(0).len);
    const over = [_]u8{'p'} ** (MaxArgBytes + 1);
    try testing.expectEqual(open_error_t.ArgumentTooLong, run(&.{&over}, &fake, 0, &out));
}

test "boundaries: device 63 bytes accepted, 64 rejected; app key 200 accepted, 201 rejected" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    const device63 = [_]u8{'d'} ** MaxDeviceBytes;
    const device64 = [_]u8{'d'} ** (MaxDeviceBytes + 1);
    try testing.expectEqual(open_error_t.None, run(&.{ "-d", &device63, "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqual(MaxDeviceBytes, out.device_length);
    try testing.expectEqual(open_error_t.InvalidDevice, run(&.{ "-d", &device64, "--app", "a-x" }, &fake, 0, &out));
    var key200: [MaxAppKeyBytes]u8 = undefined;
    @memcpy(key200[0..2], "a-");
    @memset(key200[2..], 'k');
    try testing.expectEqual(open_error_t.None, run(&.{ "--app", &key200 }, &fake, 0, &out));
    var key201: [MaxAppKeyBytes + 1]u8 = undefined;
    @memcpy(key201[0..2], "a-");
    @memset(key201[2..], 'k');
    try testing.expectEqual(open_error_t.InvalidAppKey, run(&.{ "--app", &key201 }, &fake, 0, &out));
}

test "boundaries: quoted argument tail of exactly 4096 accepted and 4097 rejected" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    const exe = "x.exe";
    var pe_buffer: [256]u8 = undefined;
    const gui = pe_image(&pe_buffer, 2);
    const entries = [_]fake_entry_t{.{ .path = exe, .bytes = gui }};
    fake.entries = &entries;
    const fill = [_]u8{'z'} ** 4094;
    try testing.expectEqual(open_error_t.None, run(&.{ "--", exe, &fill }, &fake, 0, &out));
    const over = [_]u8{'z'} ** 4095;
    try testing.expectEqual(open_error_t.QuotedArgumentsTooLong, run(&.{ "--", exe, &over }, &fake, 0, &out));
}

test "classification table over GUI, CUI, malformed, unknown and TTY states" {
    var pe_buffer: [256]u8 = undefined;
    const gui = pe_image(&pe_buffer, 2);
    var cui_storage: [256]u8 = undefined;
    const cui = pe_image(&cui_storage, 3);
    var junk = [_]u8{ 'M', 'Z', 1, 2, 3 };
    const entries = [_]fake_entry_t{
        .{ .path = "g.exe", .bytes = gui },
        .{ .path = "c.exe", .bytes = cui },
        .{ .path = "j.exe", .bytes = &junk },
        .{ .path = "e.exe", .bytes = &.{} },
        .{ .path = "bad.exe", .status = 5 },
        .{ .path = "long.exe", .bytes = cui, .reported = PrefixBytes + 1 },
        .{ .path = "C.EXE", .bytes = cui },
    };
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    const row_t = struct { path: []const u8, tty: u32, new_console: bool, kind: item_kind_t, image: pe_header.image_kind_t };
    const rows = [_]row_t{
        .{ .path = "g.exe", .tty = 0, .new_console = false, .kind = .Path, .image = .Gui },
        .{ .path = "g.exe", .tty = 1, .new_console = true, .kind = .Path, .image = .Gui },
        .{ .path = "c.exe", .tty = 0, .new_console = false, .kind = .Console, .image = .Console },
        .{ .path = "c.exe", .tty = 1, .new_console = false, .kind = .DelegateExec, .image = .Console },
        .{ .path = "c.exe", .tty = 1, .new_console = true, .kind = .Console, .image = .Console },
        .{ .path = "c.exe", .tty = 0, .new_console = true, .kind = .Console, .image = .Console },
        .{ .path = "j.exe", .tty = 1, .new_console = false, .kind = .Path, .image = .Gui },
        .{ .path = "e.exe", .tty = 0, .new_console = false, .kind = .Path, .image = .Gui },
        .{ .path = "C.EXE", .tty = 0, .new_console = false, .kind = .Console, .image = .Console },
    };
    for (rows) |row| {
        var tokens: [2][]const u8 = .{ "--new-console", row.path };
        const selected = if (row.new_console) tokens[0..2] else tokens[1..2];
        const status = run(selected, &fake, row.tty, &out);
        try testing.expectEqual(open_error_t.None, status);
        try testing.expectEqual(row.kind, out.items[0].kind);
        try testing.expectEqual(row.image, out.items[0].image);
    }
    try testing.expectEqual(open_error_t.ImageUnreadable, run(&.{"bad.exe"}, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), out.item_count);
    try testing.expectEqual(open_error_t.ImageUnreadable, run(&.{"long.exe"}, &fake, 0, &out));
    try testing.expectEqual(open_error_t.ImageUnreadable, run(&.{"unlisted.exe"}, &fake, 0, &out));
    const options_no_reader = no_reader_options();
    try testing.expectEqual(open_error_t.ImageUnreadable, parse(&.{"c.exe"}, &options_no_reader, &out));
    try testing.expectEqual(open_error_t.None, parse(&.{"notes.txt"}, &options_no_reader, &out));
}

test "callback is called once per exe item in argv order with capacity 4096" {
    var pe_buffer: [256]u8 = undefined;
    const gui = pe_image(&pe_buffer, 2);
    const entries = [_]fake_entry_t{ .{ .path = "one.exe", .bytes = gui }, .{ .path = "two.exe", .bytes = gui } };
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{ "one.exe", "readme.txt", "two.exe", "pkg.msi" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 2), fake.calls);
    try testing.expectEqualSlices(u8, "one.exe", fake.paths[0]);
    try testing.expectEqualSlices(u8, "two.exe", fake.paths[1]);
    try testing.expectEqual(@as(usize, 4), out.item_count);
}

test "wire launch kinds match the codec table and delegation is never sent" {
    try testing.expectEqual(@as(?u8, null), item_kind_t.Unused.wire_launch_kind());
    try testing.expectEqual(@as(?u8, 1), item_kind_t.App.wire_launch_kind());
    try testing.expectEqual(@as(?u8, 2), item_kind_t.Path.wire_launch_kind());
    try testing.expectEqual(@as(?u8, 2), item_kind_t.ExeWithArgs.wire_launch_kind());
    try testing.expectEqual(@as(?u8, 3), item_kind_t.Installer.wire_launch_kind());
    try testing.expectEqual(@as(?u8, 4), item_kind_t.Shortcut.wire_launch_kind());
    try testing.expectEqual(@as(?u8, 5), item_kind_t.Console.wire_launch_kind());
    try testing.expectEqual(@as(?u8, null), item_kind_t.DelegateExec.wire_launch_kind());
}

test "exit tables cover every error value and LaunchResult status" {
    const expected = [_]struct { err: open_error_t, code: u8 }{
        .{ .err = .None, .code = 0 },
        .{ .err = .InvalidArgument, .code = 125 },
        .{ .err = .UnknownOption, .code = 2 },
        .{ .err = .DuplicateOption, .code = 2 },
        .{ .err = .MissingOptionValue, .code = 2 },
        .{ .err = .InvalidDevice, .code = 2 },
        .{ .err = .InvalidAppKey, .code = 2 },
        .{ .err = .OptionAfterTarget, .code = 2 },
        .{ .err = .MissingTarget, .code = 2 },
        .{ .err = .MixedTargets, .code = 2 },
        .{ .err = .EmptyPath, .code = 2 },
        .{ .err = .EmptyExecutable, .code = 2 },
        .{ .err = .ExecutableNotExe, .code = 2 },
        .{ .err = .TooManyArgs, .code = 2 },
        .{ .err = .ArgumentTooLong, .code = 2 },
        .{ .err = .QuotedArgumentsTooLong, .code = 2 },
        .{ .err = .ImageUnreadable, .code = 126 },
        .{ .err = .OutsideShare, .code = 126 },
        .{ .err = .NotFound, .code = 127 },
        .{ .err = .LocalFailure, .code = 125 },
        .{ .err = .GuestUnreachable, .code = 125 },
        .{ .err = .GuestLaunchFailed, .code = 1 },
        .{ .err = .DelegateConflict, .code = 2 },
        .{ .err = .InvalidUtf8, .code = 2 },
    };
    for (expected) |case| {
        try testing.expectEqual(case.code, exit_code_for_error(case.err));
        try testing.expectEqual(case.code, interop_open_exit_code(@intFromEnum(case.err)));
    }
    try testing.expectEqual(@as(u8, 125), interop_open_exit_code(999));
    try testing.expectEqual(@as(u8, 0), exit_code_for_launch_status(0));
    for (1..10) |status| try testing.expectEqual(@as(u8, 1), exit_code_for_launch_status(@intCast(status)));
    try testing.expectEqual(@as(u8, 1), interop_open_launch_exit_code(777));
    try testing.expectEqual(@as(u8, 0), interop_open_launch_exit_code(0));
}

test "quoting vectors match the waddle_quote byte sequences" {
    const vector_t = struct { args: []const []const u8, expected: []const u8 };
    const vectors = [_]vector_t{
        .{ .args = &.{}, .expected = "" },
        .{ .args = &.{""}, .expected = "\"\"" },
        .{ .args = &.{ "a", "" }, .expected = "\"a\" \"\"" },
        .{ .args = &.{"a b"}, .expected = "\"a b\"" },
        .{ .args = &.{"say \"hi\""}, .expected = "\"say \\\"hi\\\"\"" },
        .{ .args = &.{"C:\\dir\\"}, .expected = "\"C:\\dir\\\\\"" },
        .{ .args = &.{"a\\\"b"}, .expected = "\"a\\\\\\\"b\"" },
        .{ .args = &.{"x\\y"}, .expected = "\"x\\y\"" },
        .{ .args = &.{"\\\\"}, .expected = "\"\\\\\\\\\"" },
        .{ .args = &.{"tab\there"}, .expected = "\"tab\there\"" },
        .{ .args = &.{"\xc3\xa9t\xc3\xa9"}, .expected = "\"\xc3\xa9t\xc3\xa9\"" },
        .{ .args = &.{ "--wait", "--", "\"" }, .expected = "\"--wait\" \"--\" \"\\\"\"" },
    };
    for (vectors) |vector| {
        var buffer: [128]u8 = undefined;
        const length = try quote_into(vector.args, &buffer);
        try testing.expectEqualSlices(u8, vector.expected, buffer[0..length]);
        try testing.expectEqual(vector.expected.len, try quoted_length(vector.args));
    }
}

test "quoting reports NoSpace and QuotedTooLarge without writing past capacity" {
    var small: [3]u8 = undefined;
    try testing.expectError(error.NoSpace, quote_into(&.{"abc"}, &small));
    var exact: [5]u8 = undefined;
    try testing.expectEqual(@as(usize, 5), try quote_into(&.{"abc"}, &exact));
    const big = [_]u8{'q'} ** 2000;
    try testing.expectError(error.QuotedTooLarge, quoted_length(&.{ &big, &big, &big }));
    var sink: [MaxQuotedBytes + 8]u8 = undefined;
    try testing.expectError(error.QuotedTooLarge, quote_into(&.{ &big, &big, &big }, &sink));
    const slashes = [_]u8{'\\'} ** 1000;
    try testing.expectEqual(@as(usize, 2 + 1000 * 2), try quoted_length(&.{&slashes}));
    const too_many = [_]u8{'\\'} ** 3000;
    try testing.expectError(error.QuotedTooLarge, quoted_length(&.{&too_many}));
}

test "quoting round trips through the Win32 decoder over seeded random arguments" {
    var prng = std.Random.DefaultPrng.init(0x6f70656e5f717431);
    const random = prng.random();
    const alphabet = [_]u8{ 'a', '\\', '"', ' ', 0xc3, 0xa9, '\t' };
    for (0..20000) |_| {
        var storage: [4][8]u8 = undefined;
        var slices: [4][]const u8 = undefined;
        const count = random.uintLessThan(usize, 5);
        for (0..count) |index| {
            const length = random.uintLessThan(usize, 9);
            for (0..length) |position| storage[index][position] = alphabet[random.uintLessThan(usize, alphabet.len)];
            slices[index] = storage[index][0..length];
        }
        var quoted: [256]u8 = undefined;
        const length = try quote_into(slices[0..count], &quoted);
        try testing.expectEqual(length, try quoted_length(slices[0..count]));
        var position: usize = 0;
        var decoded: [256]u8 = undefined;
        var decoded_length: usize = 0;
        var offset: usize = 0;
        for (0..count) |index| {
            if (index > 0) {
                try testing.expectEqual(@as(u8, ' '), quoted[position]);
                position += 1;
            }
            const start = decoded_length;
            try testing.expect(decode_group(quoted[0..length], &position, &decoded, &decoded_length));
            try testing.expectEqualSlices(u8, slices[index], decoded[start..decoded_length]);
            offset = position;
        }
        try testing.expectEqual(length, offset);
    }
}

test "seeded random token streams keep the output contract on every outcome" {
    var prng = std.Random.DefaultPrng.init(0x7061727365);
    const random = prng.random();
    const tokens = [_][]const u8{ "--app", "--device", "-d", "--wait", "--new-console", "--", "a.txt", "b.exe", "c.msi", "d.lnk", "a-app", "dev", "", "-x", "x.EXE", "p-q_1" };
    var pe_buffer: [256]u8 = undefined;
    const gui = pe_image(&pe_buffer, 2);
    const entries = [_]fake_entry_t{ .{ .path = "b.exe", .bytes = gui }, .{ .path = "x.EXE", .bytes = gui } };
    var fake = fake_reader_t{ .entries = &entries };
    var succeeded: usize = 0;
    for (0..20000) |_| {
        var stream: [8][]const u8 = undefined;
        const count = 1 + random.uintLessThan(usize, stream.len);
        for (0..count) |index| stream[index] = tokens[random.uintLessThan(usize, tokens.len)];
        var out = std.mem.zeroes(request_t);
        const status = run(stream[0..count], &fake, random.uintLessThan(u32, 2), &out);
        if (status != .None) {
            try testing.expectEqual(@as(usize, 0), out.item_count);
            try testing.expect(out.device == null);
            continue;
        }
        succeeded += 1;
        try testing.expect(out.item_count >= 1 and out.item_count <= MaxItems);
        for (0..out.item_count) |index| {
            const text = out.item_text(index);
            var found = false;
            for (stream[0..count]) |token| found = found or (token.ptr == text.ptr and token.len == text.len);
            try testing.expect(found);
            try testing.expect(out.items[index].kind != .Unused);
        }
    }
    try testing.expect(succeeded > 0);
}

test "C ABI parse maps NULL, count and argument-length conditions" {
    var fake = fake_reader_t{ .entries = &.{} };
    const options = options_t{ .read_prefix = fake_read, .read_context = &fake, .stdio_is_tty = 0 };
    var out = std.mem.zeroes(request_t);
    const app = "--app";
    const key = "a-x";
    const argv = [_][*:0]const u8{ app, key };
    try testing.expectEqual(@as(c_int, 0), interop_open_parse(hide(usize, 2), &argv, &options, &out));
    try testing.expectEqual(@as(usize, 1), out.item_count);
    try testing.expectEqualSlices(u8, "a-x", out.item_text(0));
    try testing.expectEqual(@as(c_int, 1), interop_open_parse(hide(usize, 2), &argv, null, &out));
    try testing.expectEqual(@as(c_int, 1), interop_open_parse(hide(usize, 2), &argv, &options, null));
    try testing.expectEqual(@as(c_int, 1), interop_open_parse(hide(usize, 1), null, &options, &out));
    try testing.expectEqual(@as(c_int, 8), interop_open_parse(hide(usize, 0), null, &options, &out));
    try testing.expectEqual(@as(usize, 0), out.item_count);
    const many = [_][*:0]const u8{"f.txt"} ** (MaxArgs + 1);
    try testing.expectEqual(@as(c_int, 13), interop_open_parse(hide(usize, MaxArgs + 1), &many, &options, &out));
    try testing.expectEqual(@as(usize, 0), out.item_count);
    var long: [MaxArgBytes + 1:0]u8 = [_:0]u8{0} ** (MaxArgBytes + 1);
    @memset(long[0 .. MaxArgBytes + 1], 'q');
    const long_argv = [_][*:0]const u8{ &long, "a.txt" };
    try testing.expectEqual(@as(c_int, 14), interop_open_parse(hide(usize, 2), &long_argv, &options, &out));
    var exact: [MaxArgBytes:0]u8 = [_:0]u8{0} ** MaxArgBytes;
    @memset(exact[0..MaxArgBytes], 'q');
    const exact_argv = [_][*:0]const u8{&exact};
    try testing.expectEqual(@as(c_int, 0), interop_open_parse(hide(usize, 1), &exact_argv, &options, &out));
    try testing.expectEqual(@as(usize, MaxArgBytes), out.item_text(0).len);
    var fill_z: [MaxArgBytes:0]u8 = [_:0]u8{0} ** MaxArgBytes;
    @memset(fill_z[0..4095], 'z');
    var exe_name = "x.exe".*;
    const entries_exe = [_]fake_entry_t{.{ .path = "x.exe", .bytes = &.{} }};
    fake.entries = &entries_exe;
    const tail_argv = [_][*:0]const u8{ "--", &exe_name, &fill_z };
    try testing.expectEqual(@as(c_int, 15), interop_open_parse(hide(usize, 3), &tail_argv, &options, &out));
}

test "C ABI quote maps every argument and capacity condition" {
    var buffer: [16]u8 = undefined;
    var length: usize = 99;
    const argv = [_][*:0]const u8{ "a", "b c" };
    try testing.expectEqual(@as(c_int, 0), interop_open_quote(hide(usize, 2), &argv, &buffer, buffer.len, &length));
    try testing.expectEqualSlices(u8, "\"a\" \"b c\"", buffer[0..length]);
    try testing.expectEqual(@as(u8, 0), buffer[length]);
    try testing.expectEqual(@as(c_int, 0), interop_open_quote(hide(usize, 0), null, &buffer, buffer.len, &length));
    try testing.expectEqual(@as(usize, 0), length);
    try testing.expectEqual(@as(c_int, 1), interop_open_quote(hide(usize, 1), &argv, hide(?[*]u8, null), buffer.len, &length));
    try testing.expectEqual(@as(c_int, 1), interop_open_quote(hide(usize, 1), &argv, &buffer, buffer.len, null));
    try testing.expectEqual(@as(c_int, 1), interop_open_quote(hide(usize, 1), null, &buffer, buffer.len, &length));
    try testing.expectEqual(@as(c_int, 3), interop_open_quote(hide(usize, 2), &argv, &buffer, 0, &length));
    try testing.expectEqual(@as(c_int, 3), interop_open_quote(hide(usize, 2), &argv, &buffer, 9, &length));
    try testing.expectEqual(@as(c_int, 0), interop_open_quote(hide(usize, 2), &argv, &buffer, 10, &length));
    try testing.expectEqual(@as(c_int, 1), interop_open_quote(hide(usize, MaxArgs + 1), &argv, &buffer, buffer.len, &length));
    var long: [MaxArgBytes + 1:0]u8 = [_:0]u8{0} ** (MaxArgBytes + 1);
    @memset(long[0 .. MaxArgBytes + 1], 'q');
    const long_argv = [_][*:0]const u8{&long};
    var big: [MaxQuotedBytes + 16]u8 = undefined;
    try testing.expectEqual(@as(c_int, 1), interop_open_quote(hide(usize, 1), &long_argv, &big, big.len, &length));
    var filler: [1499:0]u8 = [_:0]u8{0} ** 1499;
    @memset(filler[0..1499], 'k');
    var many: [3][*:0]const u8 = undefined;
    for (&many) |*slot| slot.* = &filler;
    try testing.expectEqual(@as(c_int, 2), interop_open_quote(hide(usize, 3), &many, &big, big.len, &length));
}

test "C ABI parse rejects NULL pointers without touching the request" {
    var fake = fake_reader_t{ .entries = &.{} };
    const options = options_t{ .read_prefix = fake_read, .read_context = &fake, .stdio_is_tty = 0 };
    var out = std.mem.zeroes(request_t);
    out.item_count = 7;
    try testing.expectEqual(@as(c_int, 1), interop_open_parse(hide(usize, 0), null, hide(?*const options_t, null), &out));
    try testing.expectEqual(@as(c_int, 1), interop_open_parse(hide(usize, 0), null, &options, hide(?*request_t, null)));
    try testing.expectEqual(@as(usize, 7), out.item_count);
}

test "extern layouts match open_request.h byte for byte" {
    try testing.expectEqual(@as(usize, 40), @sizeOf(item_t));
    try testing.expectEqual(@as(usize, 2592), @sizeOf(request_t));
    try testing.expectEqual(@as(usize, 24), @sizeOf(options_t));
    try testing.expectEqual(@as(usize, 0), @offsetOf(item_t, "kind"));
    try testing.expectEqual(@as(usize, 4), @offsetOf(item_t, "image"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(item_t, "text"));
    try testing.expectEqual(@as(usize, 16), @offsetOf(item_t, "text_length"));
    try testing.expectEqual(@as(usize, 24), @offsetOf(item_t, "argument_first"));
    try testing.expectEqual(@as(usize, 32), @offsetOf(item_t, "argument_count"));
    try testing.expectEqual(@as(usize, 32), @offsetOf(request_t, "items"));
    try testing.expectEqual(@as(usize, 24), @offsetOf(request_t, "device_length"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(options_t, "read_context"));
    try testing.expectEqual(@as(usize, 16), @offsetOf(options_t, "stdio_is_tty"));
}

test "callback status 1 is NotFound (127) and other nonzero statuses are unreadable (126)" {
    const entries = [_]fake_entry_t{ .{ .path = "missing.exe", .status = 1 }, .{ .path = "locked.exe", .status = 5 } };
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.NotFound, run(&.{"missing.exe"}, &fake, 0, &out));
    try testing.expectEqual(open_error_t.NotFound, run(&.{ "--", "missing.exe", "a" }, &fake, 0, &out));
    try testing.expectEqual(ExitNotFound, exit_code_for_error(.NotFound));
    try testing.expectEqual(@as(usize, 0), out.item_count);
    try testing.expectEqual(open_error_t.ImageUnreadable, run(&.{"locked.exe"}, &fake, 0, &out));
    try testing.expectEqual(ExitNotExecutable, exit_code_for_error(.ImageUnreadable));
}

test "usage errors win over classification: an invalid argv never invokes the callback" {
    const gui = pe_image_fixture();
    const entries = [_]fake_entry_t{
        .{ .path = "missing.exe", .status = 1 },
        .{ .path = "g.exe", .bytes = &gui },
        .{ .path = "h.exe", .bytes = &gui },
    };
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.OptionAfterTarget, run(&.{ "missing.exe", "--wait" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), fake.calls);
    try testing.expectEqual(open_error_t.UnknownOption, run(&.{ "g.exe", "--bogus" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), fake.calls);
    try testing.expectEqual(open_error_t.EmptyPath, run(&.{ "g.exe", "h.exe", "" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), fake.calls);
    try testing.expectEqual(@as(usize, 0), out.item_count);
    try testing.expectEqual(open_error_t.None, run(&.{ "g.exe", "h.exe" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 2), fake.calls);
}

fn pe_image_fixture() [256]u8 {
    var buffer: [256]u8 = undefined;
    _ = pe_image(&buffer, 2);
    return buffer;
}

test "option values are the next token and are checked by their own rule" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{ "-d", "-dev", "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqualSlices(u8, "-dev", out.device.?[0..out.device_length]);
    try testing.expectEqual(open_error_t.None, run(&.{ "--device", "-dev", "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqualSlices(u8, "-dev", out.device.?[0..out.device_length]);
    try testing.expectEqual(open_error_t.None, run(&.{ "-d", "--wait", "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqualSlices(u8, "--wait", out.device.?[0..out.device_length]);
    try testing.expectEqual(@as(u32, 0), out.wait);
    try testing.expectEqual(open_error_t.InvalidAppKey, run(&.{ "--app", "--wait" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.InvalidDevice, run(&.{ "-d", "", "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.MissingOptionValue, run(&.{"-d"}, &fake, 0, &out));
    try testing.expectEqual(open_error_t.MissingOptionValue, run(&.{"--device"}, &fake, 0, &out));
    try testing.expectEqual(open_error_t.MissingOptionValue, run(&.{ "--wait", "--app" }, &fake, 0, &out));
}

test "flags may precede the target in any order; options after --app are usage errors" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{ "--wait", "--new-console", "-d", "dev", "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.None, run(&.{ "--new-console", "--device", "dev", "--wait", "--app", "a-x" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.OptionAfterTarget, run(&.{ "--app", "a-x", "--wait" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.OptionAfterTarget, run(&.{ "--app", "a-x", "-d", "dev" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.OptionAfterTarget, run(&.{ "--app", "a-x", "--new-console" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.DuplicateOption, run(&.{ "--app", "a-x", "--app", "a-y" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.MixedTargets, run(&.{ "--app", "a-x", "a.txt" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), out.item_count);
}

test "DelegateExec must be the only item: two delegated executables or a mix are usage errors" {
    var pe_cui: [256]u8 = undefined;
    const cui = pe_image(&pe_cui, 3);
    const entries = [_]fake_entry_t{
        .{ .path = "c1.exe", .bytes = cui },
        .{ .path = "c2.exe", .bytes = cui },
    };
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.DelegateConflict, run(&.{ "c1.exe", "c2.exe" }, &fake, 1, &out));
    try testing.expectEqual(open_error_t.DelegateConflict, run(&.{ "c1.exe", "notes.txt" }, &fake, 1, &out));
    try testing.expectEqual(open_error_t.DelegateConflict, run(&.{ "notes.txt", "c1.exe" }, &fake, 1, &out));
    try testing.expectEqual(@as(usize, 0), out.item_count);
    try testing.expectEqual(ExitUsage, exit_code_for_error(.DelegateConflict));
    try testing.expectEqual(open_error_t.None, run(&.{ "c1.exe", "c2.exe" }, &fake, 0, &out));
    try testing.expectEqual(item_kind_t.Console, out.items[1].kind);
    try testing.expectEqual(open_error_t.None, run(&.{ "--new-console", "c1.exe", "c2.exe" }, &fake, 1, &out));
    try testing.expectEqual(open_error_t.None, run(&.{ "c1.exe" }, &fake, 1, &out));
    try testing.expectEqual(item_kind_t.DelegateExec, out.items[0].kind);
    try testing.expectEqual(open_error_t.None, run(&.{ "--", "c1.exe", "x" }, &fake, 1, &out));
    try testing.expectEqual(item_kind_t.DelegateExec, out.items[0].kind);
}

test "non-UTF-8 PATH, EXE or argument-tail bytes are usage errors before any callback" {
    const gui = pe_image_fixture();
    const entries = [_]fake_entry_t{.{ .path = "g.exe", .bytes = &gui }};
    var fake = fake_reader_t{ .entries = &entries };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.InvalidUtf8, run(&.{"\xff.txt"}, &fake, 0, &out));
    try testing.expectEqual(open_error_t.InvalidUtf8, run(&.{ "g.exe", "\xc3" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.InvalidUtf8, run(&.{ "--", "\xffx.exe" }, &fake, 0, &out));
    try testing.expectEqual(open_error_t.InvalidUtf8, run(&.{ "--", "g.exe", "ok", "\xed\xa0\x80" }, &fake, 0, &out));
    try testing.expectEqual(@as(usize, 0), fake.calls);
    try testing.expectEqual(@as(usize, 0), out.item_count);
    try testing.expectEqual(ExitUsage, exit_code_for_error(.InvalidUtf8));
    try testing.expectEqual(open_error_t.None, run(&.{"caf\xc3\xa9.txt"}, &fake, 0, &out));
    try testing.expectEqualSlices(u8, "caf\xc3\xa9.txt", out.item_text(0));
    try testing.expectEqual(open_error_t.None, run(&.{ "--", "g.exe", "\xc3\xa9t\xc3\xa9" }, &fake, 0, &out));
}

test "dash-prefixed targets: ./-x is a PATH, -x is UnknownOption, -- notes.txt is usage" {
    var fake = fake_reader_t{ .entries = &.{} };
    var out = std.mem.zeroes(request_t);
    try testing.expectEqual(open_error_t.None, run(&.{"./-x.txt"}, &fake, 0, &out));
    try testing.expectEqualSlices(u8, "./-x.txt", out.item_text(0));
    try testing.expectEqual(item_kind_t.Path, out.items[0].kind);
    try testing.expectEqual(open_error_t.UnknownOption, run(&.{"-x.txt"}, &fake, 0, &out));
    try testing.expectEqual(open_error_t.ExecutableNotExe, run(&.{ "--", "notes.txt" }, &fake, 0, &out));
    try testing.expectEqual(ExitUsage, exit_code_for_error(.ExecutableNotExe));
    try testing.expectEqual(@as(usize, 0), fake.calls);
}
