//! Client-side primitives for the local session.sock protocol, shared by `waddle open` and
//! `waddle session stop`: runtime directory verification, connect with a peer credential check,
//! the session.lock probe, and deadline-bounded reads and writes. Nothing here allocates and every
//! function is synchronous. Callers that need test doubles pass the poll function explicitly
//! (read_exact); everything else is the production implementation.
const std = @import("std");

/// Longest path the callers build for session.sock and session.lock; matches WaddleMaxPathLen.
pub const MaxPathBytes = 1024;
/// Fixed-capacity diagnostic text built without std.fmt or allocation. Every diagnostic these
/// clients print is at most a device name (63 bytes) plus a fixed sentence, so 256 bytes fit.
pub const Message = struct {
    buffer: [256]u8 = undefined,
    len: usize = 0,

    /// Append text; the caller keeps the total within the buffer.
    pub fn add(self: *Message, text: []const u8) void {
        @memcpy(self.buffer[self.len..][0..text.len], text);
        self.len += text.len;
    }

    /// Append a decimal number without leading zeros.
    pub fn add_number(self: *Message, value: u64) void {
        var digits: [20]u8 = undefined;
        var count: usize = 0;
        var rest = value;
        while (true) {
            digits[count] = @intCast('0' + rest % 10);
            count += 1;
            rest /= 10;
            if (rest == 0) break;
        }
        while (count > 0) {
            count -= 1;
            self.add(digits[count..][0..1]);
        }
    }

    /// The text written so far, borrowed from this value.
    pub fn slice(self: *const Message) []const u8 {
        return self.buffer[0..self.len];
    }
};

/// Session socket file name inside the device runtime directory.
pub const SessionSocketName = "session.sock";
/// Session lock file name inside the device runtime directory (created by the session owner).
pub const SessionLockName = "session.lock";

/// State of the device runtime directory before connecting.
pub const DirState = enum { missing, ok, unsafe };
/// Failure to connect to session.sock.
pub const ConnectError = error{ NotFound, Refused, Failed, PeerMismatch };
/// Failure of a socket read or write.
pub const IoError = error{ Failed, Timeout };
/// State of the session lock as observed by a non-acquiring probe.
pub const LockState = enum {
    /// No process holds session.lock (including when the file does not exist).
    unlocked,
    /// A process holds session.lock.
    held,
    /// The lock file cannot be inspected safely: a symbolic link, not a regular file, or not ours.
    unsafe,
};
/// Outcome of reading exactly one buffer under a deadline.
pub const ReadExact = enum { ok, timeout, eof_boundary, eof_partial, failed };
/// Read callback with the same shape as the open client's poll_read hook.
pub const PollFn = *const fn (ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) IoError!usize;

/// C constants from <sys/file.h>, identical on Linux and glibc.
const LockShared: c_int = 1;
const LockNonBlocking: c_int = 4;
const LockUnlock: c_int = 8;
extern "c" fn flock(fd: c_int, operation: c_int) c_int;
extern "c" fn getsockopt(fd: c_int, level: c_int, optname: c_int, optval: *anyopaque, optlen: *std.c.socklen_t) c_int;
/// SOL_SOCKET and SO_PEERCRED as defined by <sys/socket.h> on Linux.
const SolSocket: c_int = 1;
const SoPeerCred: c_int = 17;

/// struct ucred as returned by SO_PEERCRED on Linux.
const PeerCredentials = extern struct {
    pid: i32,
    uid: u32,
    gid: u32,
};

/// Classify the device runtime directory: it must be a directory owned by this user with mode 0700
/// when it exists. A missing directory is reported as .missing, which is never an error.
/// @param[in] dir Absolute directory path, borrowed for the call.
/// @return The directory state. Thread-safe.
pub fn dir_state(dir: []const u8) DirState {
    const st = std.posix.fstatat(std.posix.AT.FDCWD, dir, std.posix.AT.SYMLINK_NOFOLLOW) catch |err| return switch (err) {
        error.FileNotFound => .missing,
        else => .unsafe,
    };
    const is_dir = (st.mode & std.posix.S.IFMT) == std.posix.S.IFDIR;
    if (!is_dir or st.uid != std.os.linux.getuid() or (st.mode & 0o777) != 0o700) return .unsafe;
    if (!ancestors_safe(dir, std.os.linux.getuid())) return .unsafe;
    return .ok;
}

/// Every existing ancestor of dir must be a real directory (no symbolic links), owned by root or
/// the caller's uid, and not writable by others unless it is sticky (like /tmp). A missing ancestor
/// ends the walk: nothing below it can exist. The uid is a parameter so tests can name a foreign
/// owner for a fixture this user owns; dir_state always passes the effective uid.
/// @param[in] dir Absolute directory path, borrowed for the call.
/// @param[in] uid Owner accepted besides root.
/// @return true when every existing ancestor is safe. Thread-safe.
fn ancestors_safe(dir: []const u8, uid: std.posix.uid_t) bool {
    var parent = std.fs.path.dirname(dir);
    while (parent) |ancestor| : (parent = std.fs.path.dirname(ancestor)) {
        if (ancestor.len == 0 or std.mem.eql(u8, ancestor, "/")) break;
        const st = std.posix.fstatat(std.posix.AT.FDCWD, ancestor, std.posix.AT.SYMLINK_NOFOLLOW) catch |err| switch (err) {
            error.FileNotFound => return true,
            else => return false,
        };
        if ((st.mode & std.posix.S.IFMT) != std.posix.S.IFDIR) return false;
        if (st.uid != 0 and st.uid != uid) return false;
        const sticky = (st.mode & 0o1000) != 0;
        if ((st.mode & 0o022) != 0 and !sticky) return false;
    }
    return true;
}

/// Connect to a session.sock path. The connected peer's credentials must name this process's
/// effective user; the session also checks the peer on accept, so a mismatch is a refusal here.
/// @param[in] path Absolute socket path, borrowed for the call.
/// @return A connected descriptor the caller must close, or the connect failure.
pub fn connect(path: []const u8) ConnectError!c_int {
    return connect_as(path, std.os.linux.geteuid());
}

/// connect with the expected peer uid supplied by the caller. connect passes the effective uid;
/// tests pass a different uid to observe the PeerMismatch refusal without a second account.
/// @param[in] path Absolute socket path, borrowed for the call.
/// @param[in] uid Peer uid the connected server must run as.
/// @return A connected descriptor the caller must close, or the connect failure.
fn connect_as(path: []const u8, uid: std.posix.uid_t) ConnectError!c_int {
    const addr = std.net.Address.initUnix(path) catch return error.Failed;
    const fd = std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM | std.posix.SOCK.CLOEXEC, 0) catch return error.Failed;
    std.posix.connect(fd, &addr.any, addr.getOsSockLen()) catch |err| {
        std.posix.close(fd);
        return switch (err) {
            error.FileNotFound => error.NotFound,
            error.ConnectionRefused => error.Refused,
            else => error.Failed,
        };
    };
    if (!peer_is_self(fd, uid)) {
        std.posix.close(fd);
        return error.PeerMismatch;
    }
    return fd;
}

/// True when SO_PEERCRED names the given uid. Any failure to read it is false.
/// @param[in] fd Connected descriptor whose peer is inspected.
/// @param[in] uid Expected peer uid.
fn peer_is_self(fd: c_int, uid: std.posix.uid_t) bool {
    var credentials: PeerCredentials = undefined;
    var length: std.c.socklen_t = @sizeOf(PeerCredentials);
    if (getsockopt(fd, SolSocket, SoPeerCred, &credentials, &length) != 0) return false;
    if (length != @sizeOf(PeerCredentials)) return false;
    return credentials.uid == uid;
}

/// Close a descriptor. Errors are ignored: the descriptor is gone either way.
pub fn close(fd: c_int) void {
    std.posix.close(fd);
}

/// Write every byte or fail.
/// @param[in] fd Connected descriptor.
/// @param[in] bytes Bytes to send; the function returns only after all are written.
pub fn write_all(fd: c_int, bytes: []const u8) IoError!void {
    var sent: usize = 0;
    while (sent < bytes.len) {
        // MSG_NOSIGNAL turns a peer that has gone away into EPIPE instead of a process-killing SIGPIPE.
        sent += std.posix.send(fd, bytes[sent..], MsgNoSignal) catch return error.Failed;
    }
}

/// MSG_NOSIGNAL from <sys/socket.h> on Linux.
const MsgNoSignal: u32 = 0x4000;

/// Read some bytes, waiting at most timeout_ms (negative waits forever).
/// @return Bytes read; 0 at end of stream. error.Timeout when nothing became readable in time.
pub fn poll_read(fd: c_int, buffer: []u8, timeout_ms: i32) IoError!usize {
    var fds = [_]std.posix.pollfd{.{ .fd = fd, .events = std.posix.POLL.IN, .revents = 0 }};
    const ready = std.posix.poll(&fds, timeout_ms) catch return error.Failed;
    if (ready == 0) return error.Timeout;
    return std.posix.read(fd, buffer) catch error.Failed;
}

/// Fill buffer completely through poll. The poll function is the caller's hook (production
/// poll_read or a test double); ctx is passed through unchanged.
/// @return .ok when buffer is full; .eof_boundary when the stream ended before any byte;
///         .eof_partial when it ended mid-buffer; .timeout or .failed as reported by poll.
pub fn read_exact(ctx: *anyopaque, poll: PollFn, fd: c_int, buffer: []u8, timeout_ms: i32) ReadExact {
    var filled: usize = 0;
    while (filled < buffer.len) {
        const read = poll(ctx, fd, buffer[filled..], timeout_ms) catch |err| return if (err == error.Timeout) .timeout else .failed;
        if (read == 0) break;
        filled += read;
    }
    if (filled == buffer.len) return .ok;
    return if (filled == 0) .eof_boundary else .eof_partial;
}

/// Probe whether a process holds the device's session.lock, without taking it for longer than the
/// probe itself: a shared, nonblocking flock is taken and released before returning. The session
/// owner takes an exclusive lock, so a held lock makes the shared request fail with EWOULDBLOCK.
/// The lock file is opened read-only with O_NOFOLLOW, so a symbolic link is never followed.
/// Callers probe only when no live session accepted a connection; see session_stop_contract.md.
/// @param[in] dir Device runtime directory (the directory that contains session.sock).
/// @return unlocked when the file is absent or free; held when another open file holds it or the shared
///         probe lock is refused for any reason (a probe that cannot take the lock never claims it is free).
pub fn lock_state(dir: []const u8) LockState {
    var buffer: [MaxPathBytes]u8 = undefined;
    if (dir.len + 1 + SessionLockName.len >= buffer.len) return .unsafe;
    @memcpy(buffer[0..dir.len], dir);
    buffer[dir.len] = '/';
    @memcpy(buffer[dir.len + 1 ..][0..SessionLockName.len], SessionLockName);
    const path = buffer[0 .. dir.len + 1 + SessionLockName.len];

    const fd = std.posix.open(path, .{ .NOFOLLOW = true, .CLOEXEC = true }, 0) catch |err| return switch (err) {
        error.FileNotFound => .unlocked,
        else => .unsafe,
    };
    defer std.posix.close(fd);
    const st = std.posix.fstat(fd) catch return .unsafe;
    if ((st.mode & std.posix.S.IFMT) != std.posix.S.IFREG or st.uid != std.os.linux.getuid()) return .unsafe;

    const rc = flock(fd, LockShared | LockNonBlocking);
    if (rc != 0) return .held; // EWOULDBLOCK, or any refusal: never claim the device is unlocked.
    _ = flock(fd, LockUnlock);
    return .unlocked;
}

/// A scratch directory directly under /tmp (sticky and root-owned, so it is a safe ancestor). The
/// worktree's own ancestors may be group-writable, which the ancestor rule rightly refuses.
/// The caller frees the returned path and deletes the tree.
pub fn scratch_root(allocator: std.mem.Allocator) ![]u8 {
    var buffer: [96]u8 = undefined;
    const name = try std.fmt.bufPrint(&buffer, "/tmp/waddle-client-test-{d}-{d}", .{ std.os.linux.getpid(), std.time.nanoTimestamp() });
    const path = try allocator.dupe(u8, name);
    try std.fs.makeDirAbsolute(path);
    return path;
}

test "dir_state refuses a symbolic link in place of the device directory and a writable ancestor" {
    const testing = std.testing;
    const base = try scratch_root(testing.allocator);
    defer testing.allocator.free(base);
    defer std.fs.deleteTreeAbsolute(base) catch {};
    var root = try std.fs.openDirAbsolute(base, .{});
    defer root.close();
    try root.makeDir("real");
    try std.posix.fchmodat(root.fd, "real", 0o700, 0);
    try root.symLink("real", "linked", .{ .is_directory = true });
    const linked = try std.fmt.allocPrint(testing.allocator, "{s}/linked", .{base});
    defer testing.allocator.free(linked);
    // SYMLINK_NOFOLLOW: the link itself is classified, never the directory it names.
    try testing.expectEqual(DirState.unsafe, dir_state(linked));
    // An ancestor that other users can write, and is not sticky, makes the device directory unsafe.
    try root.makeDir("open");
    try std.posix.fchmodat(root.fd, "open", 0o777, 0);
    var open_dir = try root.openDir("open", .{});
    defer open_dir.close();
    try open_dir.makeDir("device");
    try std.posix.fchmodat(open_dir.fd, "device", 0o700, 0);
    const nested = try std.fmt.allocPrint(testing.allocator, "{s}/open/device", .{base});
    defer testing.allocator.free(nested);
    try testing.expectEqual(DirState.unsafe, dir_state(nested));
}

test "dir_state accepts only a private directory owned by this user" {
    const testing = std.testing;
    const base = try scratch_root(testing.allocator);
    defer testing.allocator.free(base);
    defer std.fs.deleteTreeAbsolute(base) catch {};
    var root = try std.fs.openDirAbsolute(base, .{});
    defer root.close();
    const missing = try std.fmt.allocPrint(testing.allocator, "{s}/absent", .{base});
    defer testing.allocator.free(missing);
    try testing.expectEqual(DirState.missing, dir_state(missing));
    try root.makeDir("private");
    try std.posix.fchmodat(root.fd, "private", 0o700, 0);
    const private = try std.fmt.allocPrint(testing.allocator, "{s}/private", .{base});
    defer testing.allocator.free(private);
    try testing.expectEqual(DirState.ok, dir_state(private));
    try std.posix.fchmodat(root.fd, "private", 0o755, 0);
    try testing.expectEqual(DirState.unsafe, dir_state(private));
}

test "lock_state reports unlocked, held, and unsafe lock files without leaving the lock taken" {
    const testing = std.testing;
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const base = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(base);
    // No lock file at all is unlocked.
    try testing.expectEqual(LockState.unlocked, lock_state(base));
    // A free lock file is unlocked, and the probe leaves it free for an exclusive taker.
    const lock = try tmp.dir.createFile(SessionLockName, .{ .mode = 0o600 });
    defer lock.close();
    try testing.expectEqual(LockState.unlocked, lock_state(base));
    try testing.expectEqual(@as(c_int, 0), flock(lock.handle, 2 | LockNonBlocking));
    _ = flock(lock.handle, LockUnlock);
    // An exclusive holder elsewhere makes the probe report held.
    try testing.expectEqual(@as(c_int, 0), flock(lock.handle, 2 | LockNonBlocking));
    try testing.expectEqual(LockState.held, lock_state(base));
    _ = flock(lock.handle, LockUnlock);
    try testing.expectEqual(LockState.unlocked, lock_state(base));
    // A symbolic link named session.lock is never followed.
    try tmp.dir.deleteFile(SessionLockName);
    try tmp.dir.symLink("/dev/null", SessionLockName, .{});
    try testing.expectEqual(LockState.unsafe, lock_state(base));
    // A directory named session.lock is not a lock file.
    try tmp.dir.deleteFile(SessionLockName);
    try tmp.dir.makeDir(SessionLockName);
    try testing.expectEqual(LockState.unsafe, lock_state(base));
    // A directory path too long for the buffer is unsafe rather than truncated.
    const long = ("d" ** MaxPathBytes);
    try testing.expectEqual(LockState.unsafe, lock_state(long));
}

test "read_exact maps poll results to boundary, partial, timeout and failure" {
    const testing = std.testing;
    const Script = struct {
        steps: []const union(enum) { bytes: []const u8, timeout, failed } = &.{},
        index: usize = 0,
        pending: []const u8 = &.{},

        fn poll(ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) IoError!usize {
            _ = fd;
            _ = timeout_ms;
            const self: *@This() = @ptrCast(@alignCast(ctx));
            if (self.index >= self.steps.len) return 0;
            const step = self.steps[self.index];
            self.index += 1;
            switch (step) {
                .timeout => return error.Timeout,
                .failed => return error.Failed,
                .bytes => |bytes| {
                    const count = @min(bytes.len, buffer.len);
                    @memcpy(buffer[0..count], bytes[0..count]);
                    return count;
                },
            }
        }
    };
    var buffer: [4]u8 = undefined;
    var full = Script{ .steps = &.{ .{ .bytes = "ab" }, .{ .bytes = "cd" } } };
    try testing.expectEqual(ReadExact.ok, read_exact(&full, Script.poll, 0, &buffer, 1));
    try testing.expectEqualSlices(u8, "abcd", &buffer);
    var empty = Script{ .steps = &.{} };
    try testing.expectEqual(ReadExact.eof_boundary, read_exact(&empty, Script.poll, 0, &buffer, 1));
    var partial = Script{ .steps = &.{.{ .bytes = "ab" }} };
    try testing.expectEqual(ReadExact.eof_partial, read_exact(&partial, Script.poll, 0, &buffer, 1));
    var late = Script{ .steps = &.{ .{ .bytes = "a" }, .timeout } };
    try testing.expectEqual(ReadExact.timeout, read_exact(&late, Script.poll, 0, &buffer, 1));
    var broken = Script{ .steps = &.{.failed} };
    try testing.expectEqual(ReadExact.failed, read_exact(&broken, Script.poll, 0, &buffer, 1));
}

test "connect reports a missing socket, a refused socket, a live one, and an over-long path" {
    const testing = std.testing;
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const base = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(base);
    const missing = try std.fmt.allocPrint(testing.allocator, "{s}/none.sock", .{base});
    defer testing.allocator.free(missing);
    try testing.expectError(error.NotFound, connect(missing));

    // A socket file whose owner has closed it refuses connections.
    const stale = try std.fmt.allocPrint(testing.allocator, "{s}/stale.sock", .{base});
    defer testing.allocator.free(stale);
    const stale_addr = try std.net.Address.initUnix(stale);
    const bound = try std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
    try std.posix.bind(bound, &stale_addr.any, stale_addr.getOsSockLen());
    std.posix.close(bound);
    try testing.expectError(error.Refused, connect(stale));

    // A listening socket accepts; the peer is this process, so the credential check passes.
    const live = try std.fmt.allocPrint(testing.allocator, "{s}/live.sock", .{base});
    defer testing.allocator.free(live);
    const live_addr = try std.net.Address.initUnix(live);
    const listener = try std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
    defer std.posix.close(listener);
    try std.posix.bind(listener, &live_addr.any, live_addr.getOsSockLen());
    try std.posix.listen(listener, 1);
    const fd = try connect(live);
    close(fd);

    // Paths longer than sun_path cannot be addressed at all.
    const long = "/" ++ ("p" ** 200);
    try testing.expectError(error.Failed, connect(long));
}

test "write_all to a peer that has closed fails with an error and never raises SIGPIPE" {
    const testing = std.testing;
    var pair: [2]std.posix.fd_t = undefined;
    try testing.expectEqual(@as(usize, 0), std.os.linux.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &pair));
    close(pair[1]);
    // Without MSG_NOSIGNAL this write kills the test process with SIGPIPE.
    try testing.expectError(error.Failed, write_all(pair[0], "stop"));
    close(pair[0]);
}

test "write_all reports a failed write; poll_read reports timeout, data, end of stream and a closed descriptor" {
    const testing = std.testing;
    // A socket whose write side is shut down refuses every write, and the failure is an error.
    var shut: [2]std.posix.fd_t = undefined;
    try testing.expectEqual(@as(usize, 0), std.os.linux.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &shut));
    try std.posix.shutdown(shut[0], .send);
    try testing.expectError(error.Failed, write_all(shut[0], "x"));
    close(shut[0]);
    close(shut[1]);

    var pair: [2]std.posix.fd_t = undefined;
    try testing.expectEqual(@as(usize, 0), std.os.linux.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &pair));
    const a = pair[0];
    const b = pair[1];
    var buffer: [8]u8 = undefined;
    try testing.expectError(error.Timeout, poll_read(a, &buffer, 0));
    try write_all(b, "hi");
    try testing.expectEqual(@as(usize, 2), try poll_read(a, &buffer, 1000));
    try testing.expectEqualSlices(u8, "hi", buffer[0..2]);
    try write_all(a, "yo");
    try testing.expectEqual(@as(usize, 2), try poll_read(b, &buffer, 1000));
    close(b);
    try testing.expectEqual(@as(usize, 0), try poll_read(a, &buffer, 1000));
    close(a);
    // A closed descriptor is reported ready with POLLNVAL, and the read then fails.
    try testing.expectError(error.Failed, poll_read(a, &buffer, 1000));
}

test "Message formats numbers without leading zeros, including zero and the largest u64" {
    var zero = Message{};
    zero.add_number(0);
    try std.testing.expectEqualSlices(u8, "0", zero.slice());
    var largest = Message{};
    largest.add_number(std.math.maxInt(u64));
    try std.testing.expectEqualSlices(u8, "18446744073709551615", largest.slice());
}

test "ancestors_safe accepts a relative path, a missing ancestor and a caller-owned ancestor, and refuses non-directories and foreign owners" {
    const testing = std.testing;
    const base = try scratch_root(testing.allocator);
    defer testing.allocator.free(base);
    defer std.fs.deleteTreeAbsolute(base) catch {};
    var root = try std.fs.openDirAbsolute(base, .{});
    defer root.close();
    const uid = std.os.linux.getuid();
    // A relative name has no parent directory, so there is nothing to walk.
    try testing.expect(ancestors_safe("device", uid));
    // A missing ancestor ends the walk as safe: nothing below it can exist.
    const missing = try std.fmt.allocPrint(testing.allocator, "{s}/absent/device", .{base});
    defer testing.allocator.free(missing);
    try testing.expect(ancestors_safe(missing, uid));
    // A regular file as the direct parent is not a directory.
    (try root.createFile("file.txt", .{})).close();
    const under_file = try std.fmt.allocPrint(testing.allocator, "{s}/file.txt/device", .{base});
    defer testing.allocator.free(under_file);
    try testing.expect(!ancestors_safe(under_file, uid));
    // A path that runs through a regular file fails with ENOTDIR, which std maps to FileNotFound,
    // so the walk ends as a missing ancestor. The behaviour is unchanged; nothing below can exist.
    const through_file = try std.fmt.allocPrint(testing.allocator, "{s}/file.txt/sub/device", .{base});
    defer testing.allocator.free(through_file);
    try testing.expect(ancestors_safe(through_file, uid));
    // An ancestor that cannot be searched (EACCES) and a self-referencing symbolic link (ELOOP) are
    // unexpected errors, which are unsafe. Test runs as a non-root user, so mode 0 denies search.
    try root.makeDir("locked");
    try std.posix.fchmodat(root.fd, "locked", 0o000, 0);
    defer std.posix.fchmodat(root.fd, "locked", 0o700, 0) catch {};
    const denied = try std.fmt.allocPrint(testing.allocator, "{s}/locked/sub/device", .{base});
    defer testing.allocator.free(denied);
    try testing.expect(!ancestors_safe(denied, uid));
    try root.symLink("loop", "loop", .{});
    const looped = try std.fmt.allocPrint(testing.allocator, "{s}/loop/device", .{base});
    defer testing.allocator.free(looped);
    try testing.expect(!ancestors_safe(looped, uid));
    // An ancestor owned by neither root nor the accepted uid is unsafe; the caller's own uid passes.
    try root.makeDir("own");
    try std.posix.fchmodat(root.fd, "own", 0o700, 0);
    const owned = try std.fmt.allocPrint(testing.allocator, "{s}/own/device", .{base});
    defer testing.allocator.free(owned);
    try testing.expect(ancestors_safe(owned, uid));
    try testing.expect(!ancestors_safe(owned, uid +% 1));
}

test "lock_state refuses a session.lock reached through a regular file and treats an absent directory as unlocked" {
    const testing = std.testing;
    const base = try scratch_root(testing.allocator);
    defer testing.allocator.free(base);
    defer std.fs.deleteTreeAbsolute(base) catch {};
    var root = try std.fs.openDirAbsolute(base, .{});
    defer root.close();
    // The directory component is a regular file, so open fails with ENOTDIR, not FileNotFound.
    (try root.createFile("plain", .{})).close();
    const through_file = try std.fmt.allocPrint(testing.allocator, "{s}/plain", .{base});
    defer testing.allocator.free(through_file);
    try testing.expectEqual(LockState.unsafe, lock_state(through_file));
    // A directory that does not exist has no lock file, which is unlocked.
    const absent = try std.fmt.allocPrint(testing.allocator, "{s}/absent", .{base});
    defer testing.allocator.free(absent);
    try testing.expectEqual(LockState.unlocked, lock_state(absent));
}

test "peer_is_self treats a non-socket descriptor as a failure, and connect_as refuses a mismatched peer uid" {
    const testing = std.testing;
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const plain = try tmp.dir.createFile("plain", .{ .read = true });
    defer plain.close();
    // getsockopt(SO_PEERCRED) fails on a regular file, so no peer is accepted.
    try testing.expect(!peer_is_self(plain.handle, std.os.linux.geteuid()));

    // A live listener runs as this user, so a caller that expects another uid is refused.
    const base = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(base);
    const live = try std.fmt.allocPrint(testing.allocator, "{s}/peer.sock", .{base});
    defer testing.allocator.free(live);
    const live_addr = try std.net.Address.initUnix(live);
    const listener = try std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
    defer std.posix.close(listener);
    try std.posix.bind(listener, &live_addr.any, live_addr.getOsSockLen());
    try std.posix.listen(listener, 1);
    try testing.expectError(error.PeerMismatch, connect_as(live, std.os.linux.geteuid() +% 1));
}

test "lock_state refuses a directory path longer than its buffer when the length is only known at run time" {
    const testing = std.testing;
    var long_buffer: [MaxPathBytes + 1]u8 = undefined;
    @memset(&long_buffer, 'd');
    // doNotOptimizeAway keeps the length opaque so the length check is compiled, not folded away.
    var length: usize = MaxPathBytes;
    std.mem.doNotOptimizeAway(&length);
    try testing.expectEqual(LockState.unsafe, lock_state(long_buffer[0..length]));
}

test "read_exact reports end of stream at a boundary and mid-buffer for a poll whose count is only known at run time" {
    const testing = std.testing;
    const Counted = struct {
        fn poll(ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) IoError!usize {
            _ = fd;
            _ = timeout_ms;
            const remaining: *usize = @ptrCast(@alignCast(ctx));
            if (remaining.* == 0) return 0;
            remaining.* -= 1;
            buffer[0] = 'x';
            return 1;
        }
    };
    var buffer: [4]u8 = undefined;
    var none: usize = 0;
    std.mem.doNotOptimizeAway(&none);
    try testing.expectEqual(ReadExact.eof_boundary, read_exact(&none, Counted.poll, 0, &buffer, 1));
    var two: usize = 2;
    std.mem.doNotOptimizeAway(&two);
    try testing.expectEqual(ReadExact.eof_partial, read_exact(&two, Counted.poll, 0, &buffer, 1));
}

test "write_all continues after a partial send and then reports the failure of the stalled remainder" {
    const testing = std.testing;
    var pair: [2]std.posix.fd_t = undefined;
    try testing.expectEqual(@as(usize, 0), std.os.linux.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &pair));
    defer close(pair[0]);
    defer close(pair[1]);
    // With a send timeout and no reader, the first send returns the part that fit in the socket buffer
    // (a partial count), and the next send times out, which write_all reports as Failed.
    const timeout = std.posix.timeval{ .tv_sec = 0, .tv_usec = 100_000 };
    try std.posix.setsockopt(pair[0], std.posix.SOL.SOCKET, std.posix.SO.SNDTIMEO, std.mem.asBytes(&timeout));
    const big = try testing.allocator.alloc(u8, 8 * 1024 * 1024);
    defer testing.allocator.free(big);
    @memset(big, 0x5a);
    try testing.expectError(error.Failed, write_all(pair[0], big));
}
