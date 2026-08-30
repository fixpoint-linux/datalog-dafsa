//! util.zig — port of src/util.c (shared utility helpers: atomic write,
//! directory fsync).
//!
//! Strangler-hybrid ABI: every non-static C function is an `export fn` with
//! the exact C name and signature.  Raw libc bindings (std.c) are used so
//! syscall semantics match the C oracle byte-for-byte (partial writes, no
//! EINTR retry, identical error returns).
//!
//! Oracle: src/util.c (never modified).  C returns -1 on every failure path;
//! NULL inputs crash in C and are left unchecked here for the same reason.

const std = @import("std");
const c = std.c;

/// Local strlen (std.c does not re-export it).
fn cstrLen(s: [*:0]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {}
    return i;
}

/// snprintf(tmp, sizeof(tmp), "%s.tmp", path): builds "<path>.tmp",
/// truncating exactly like snprintf (at most len-1 chars + NUL).
fn tmpNameOf(buf: *[8192:0]u8, path: [*:0]const u8) void {
    const plen = cstrLen(path);
    const max_path = buf.len - 1 - ".tmp".len; // room for suffix + NUL
    const n = @min(plen, max_path);
    @memcpy(buf[0..n], path[0..n]);
    @memcpy(buf[n..][0..".tmp".len], ".tmp");
    buf[n + ".tmp".len] = 0;
}

/// int atomic_write_str(const char *path, const char *content)
/// Write `content` to `path` atomically: tmp + fsync + rename + dir fsync.
/// Returns 0 on success, -1 on error (tmp cleaned up on failure).
export fn atomic_write_str(path: [*:0]const u8, content: [*:0]const u8) c_int {
    var tmp: [8192:0]u8 = undefined;
    tmpNameOf(&tmp, path);

    const fd = c.open(&tmp, .{ .ACCMODE = .WRONLY, .CREAT = true, .TRUNC = true }, @as(c.mode_t, 0o644));
    if (fd < 0) return -1;

    const len = cstrLen(content);
    var off: usize = 0;
    while (off < len) {
        const w = c.write(fd, content + off, len - off);
        if (w < 0) {
            _ = c.close(fd);
            _ = c.unlink(&tmp);
            return -1;
        }
        off += @intCast(w);
    }

    if (c.fsync(fd) != 0) {
        _ = c.close(fd);
        _ = c.unlink(&tmp);
        return -1;
    }
    _ = c.close(fd);

    if (c.rename(&tmp, path) != 0) {
        _ = c.unlink(&tmp);
        return -1;
    }

    if (fsync_dir_of_path(path) != 0) return -1;
    return 0;
}

fn fsyncDir(dirpath: [*:0]const u8) c_int {
    const fd = c.open(dirpath, .{ .ACCMODE = .RDONLY, .DIRECTORY = true });
    var ret: c_int = -1;
    if (fd >= 0) {
        ret = c.fsync(fd);
        _ = c.close(fd);
    }
    return ret;
}

/// int fsync_dir_of_path(const char *path)
/// fsync the directory containing `path` (to make a prior rename durable).
/// Returns 0 on success, -1 on error.
pub export fn fsync_dir_of_path(path: [*:0]const u8) c_int {
    const len = cstrLen(path);
    var slash: ?usize = null;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        if (path[i] == '/') slash = i;
    }

    const s = slash orelse return fsyncDir(".");
    if (s == 0) return fsyncDir("/");

    // strndup(path, slash - path), open, fsync, free.
    const mem = c.malloc(s + 1) orelse return -1;
    const dir: [*]u8 = @ptrCast(mem);
    @memcpy(dir[0..s], path[0..s]);
    dir[s] = 0;
    const ret = fsyncDir(@ptrCast(dir));
    c.free(mem);
    return ret;
}

/// int fsync_dir_path(const char *dirpath)
export fn fsync_dir_path(dirpath: [*:0]const u8) c_int {
    return fsyncDir(dirpath);
}

// ─── Tests ────────────────────────────────────────────────────────────────

test "atomic_write_str + fsync_dir roundtrip" {
    const path = "/tmp/datalog_zig_u2_util_test.txt";
    try std.testing.expectEqual(@as(c_int, 0), atomic_write_str(path, "hello world\n"));
    const fd = c.open(path, .{ .ACCMODE = .RDONLY });
    try std.testing.expect(fd >= 0);
    var buf: [64]u8 = undefined;
    const n = c.read(fd, &buf, buf.len);
    _ = c.close(fd);
    try std.testing.expectEqual(@as(isize, 12), n);
    try std.testing.expectEqualStrings("hello world\n", buf[0..@intCast(n)]);
    try std.testing.expectEqual(@as(c_int, 0), fsync_dir_of_path(path));
    try std.testing.expectEqual(@as(c_int, 0), fsync_dir_path("/tmp"));
    // tmp file must be gone (renamed onto path)
    try std.testing.expect(c.unlink(path ++ ".tmp") != 0);
    try std.testing.expect(c.unlink(path) == 0);
}
