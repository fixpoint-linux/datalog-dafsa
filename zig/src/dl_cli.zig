//! dl_cli.zig — Zig port of src/dl_cli.c (the `dl` CLI executable).
//!
//! Ported in U12 as the strangler migration's final exe piece: the C dl_cli.c
//! stays in the repo as the untouched ORACLE, and this exe is what the CLI
//! byte-diff harness (zig/tests/cli_diff.sh) compares against it byte-for-byte.
//!
//! PRINT FIDELITY is the whole point: every stdout/stderr byte must match the
//! C oracle, so all output goes through the SAME libc printf/fprintf calls
//! with the SAME format strings, all numeric parsing through the SAME
//! strtoul/strtol/sscanf semantics (including their whitespace/sign quirks),
//! all allocation through libc malloc/free (so huge --top/--k OOMs print the
//! same nothing), and process control through fork/execv/exit (libc exit
//! flushes stdio exactly like `return` from C main).  Engine calls resolve to
//! the libdatalog.so exports at link time: dl_* from the ported dl.zig,
//! regex_compile/regex_dfa_free from regexwalk.zig, intern_*/term_* from
//! intern.zig/termstore.zig — while tokenize/aux_index_ensure_postings/
//! dl_index_observations/dl_search_top*/dl_vector_* still come from the C
//! index.c/vector.c in the .so (deferred to U14).
//!
//! The dl_db layout is reached through the shared internal header
//! (@cImport of dl_internal.h) exactly like the C CLI — db->ir and db->terms
//! for CLI value parsing / list printing.  Layout drift is impossible: dl.zig
//! comptime-gates its authoritative mirror against the same header.

const std = @import("std");

// Umbrella cImport: dl_internal.h pulls in dl.h (which pulls vector.h) plus
// the intern/termstore/relation/snapshot/regexwalk headers — the dl_db layout
// and every engine typedef/callback; index.h adds the still-C tokenizer,
// postings and full-text search API (all in the same translate-c unit, so
// dl_db type identity is shared).
const c = @cImport({
    @cInclude("dl_internal.h");
    @cInclude("index.h");
});

// ─── libc decls — byte fidelity to the C oracle means using libc itself ────
extern "c" fn printf(fmt: [*:0]const u8, ...) c_int;
extern "c" fn fprintf(stream: *std.c.FILE, fmt: [*:0]const u8, ...) c_int;
extern "c" fn sscanf(str: [*c]const u8, fmt: [*:0]const u8, ...) c_int;
extern "c" fn fscanf(stream: *std.c.FILE, fmt: [*:0]const u8, ...) c_int;
extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*:0]const u8, ...) c_int;
extern "c" fn fopen(path: [*c]const u8, mode: [*:0]const u8) ?*std.c.FILE;
extern "c" fn fclose(stream: *std.c.FILE) c_int;
extern "c" fn fread(ptr: [*c]u8, size: usize, nmemb: usize, stream: *std.c.FILE) usize;
extern "c" fn fseek(stream: *std.c.FILE, off: c_long, whence: c_int) c_int;
extern "c" fn ftell(stream: *std.c.FILE) c_long;
extern "c" fn fdopen(fd: c_int, mode: [*:0]const u8) ?*std.c.FILE;
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *std.c.FILE) isize;
extern "c" fn malloc(size: usize) ?*anyopaque;
extern "c" fn free(ptr: ?*anyopaque) void;
extern "c" fn exit(status: c_int) noreturn;
extern "c" fn _exit(status: c_int) noreturn;
extern "c" fn pipe(fds: *[2]c_int) c_int;
extern "c" fn fork() c_int;
extern "c" fn dup2(oldfd: c_int, newfd: c_int) c_int;
extern "c" fn close(fd: c_int) c_int;
extern "c" fn execv(path: [*c]const u8, argv: [*c]const [*c]u8) c_int;
extern "c" fn waitpid(pid: c_int, status: *c_int, options: c_int) c_int;
extern "c" fn access(path: [*c]const u8, mode: c_int) c_int;
extern "c" fn strrchr(s: [*c]const u8, ch: c_int) [*c]u8;
extern "c" fn strtoul(nptr: [*c]const u8, endptr: [*c][*c]u8, base: c_int) c_ulong;
extern "c" fn strtol(nptr: [*c]const u8, endptr: [*c][*c]u8, base: c_int) c_long;
extern "c" fn __errno_location() *c_int;
extern "c" fn qsort(base: ?*anyopaque, nmemb: usize, size: usize, compar: CmpU32) void;
extern "c" fn bsearch(key: ?*const anyopaque, base: ?*const anyopaque, nmemb: usize, size: usize, compar: CmpU32) ?*const anyopaque;
extern "c" fn strlen(s: [*c]const u8) usize;

/// glibc's unbuffered stderr stream global (same symbol the C oracle uses).
extern "c" var stderr: *std.c.FILE;

const CmpU32 = ?*const fn (?*const anyopaque, ?*const anyopaque) callconv(.c) c_int;

// ─── Constants (mirrors of the C oracle's / headers' values) ────────────────
const PATH_MAX = 4096; // Linux PATH_MAX (src/dl_cli.c uses <limits.h>'s)
const ERANGE = 34;
const SEEK_SET = 0;
const SEEK_END = 2;
const X_OK = 1;
const STDOUT_FILENO = 1;
const DL_E_CONFLICT = 2; // dl.h

/// vector.h layout constants (shared verbatim with embed.py).  Defined here
/// rather than read from the cImport because translate-c cannot always fold
/// the division macros; the comptime gate below pins them to the header when
/// it did.
const VEC_SIG_WORDS = 8; // VEC_C(256)/32 — MSB-first u32 words
const VEC_IVEC_WORDS = 96; // VEC_D(384)/4 — 4 int8 packed per u32

comptime {
    if (@hasDecl(c, "VEC_SIG_WORDS")) std.debug.assert(VEC_SIG_WORDS == c.VEC_SIG_WORDS);
    if (@hasDecl(c, "VEC_IVEC_WORDS")) std.debug.assert(VEC_IVEC_WORDS == c.VEC_IVEC_WORDS);
}

inline fn wifexited(status: c_int) bool {
    return (status & 0x7f) == 0;
}
inline fn wexitstatus(status: c_int) c_int {
    return (status >> 8) & 0xff;
}
/// C's (int)some_unsigned_long — truncate to the low 32 bits, reinterpreted.
inline fn cIntTrunc(v: c_ulong) c_int {
    return @bitCast(@as(u32, @truncate(v)));
}

fn eq(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}

/// strncmp(argv[i], "--max-nodes", 11) == 0 — equal iff the first 11 bytes
/// match (a shorter arg fails at its NUL, exactly like strncmp).
fn eqFirst11(a: [:0]const u8, b: *const [11:0]u8) bool {
    return a.len >= 11 and std.mem.eql(u8, a[0..11], b);
}

// ─── Program directory (for the dl-embed helper lookup) ─────────────────────

/// Directory of this binary (set in main): used to locate ./dl-embed for the
/// query-encode path, so vsearch works regardless of the caller's cwd.
var g_prog_dir: [PATH_MAX]u8 = @splat(0); // C init "."; set in main

// ─── Strict u32 parsing (C parse_u32_strict, 1:1 via libc strtoul) ─────────

/// Strictly parse a decimal string as an unsigned 32-bit value.  Rejects
/// empty/whitespace-input-only, a negative sign, trailing garbage, and
/// overflow — but ACCEPTS a leading '+'/whitespace, exactly like the C
/// strtoul-based oracle (whatever strtoul accepts before '\0' passes).
fn parseU32Strict(s: [*c]const u8, out: *c_ulong) bool {
    if (s == null or s[0] == 0) return false;
    if (s[0] == '-') return false; // no negatives
    __errno_location().* = 0;
    var end: [*c]u8 = null;
    const v = strtoul(s, &end, 10);
    if (__errno_location().* == ERANGE) return false; // overflow
    if (end == null or end[0] != 0) return false; // trailing garbage
    if (v > 0xFFFFFFFF) return false; // exceeds u32
    out.* = v;
    return true;
}

/// Parse a CLI value: integer -> raw u32, else intern -> sym_id.
fn cliParseValue(db: [*c]c.dl_db, s: [*c]const u8) u32 {
    const ir = db.*.ir;
    if (s == null or s[0] == 0) return 0;
    var is_int = true;
    var p: usize = 0;
    while (s[p] != 0) : (p += 1) {
        if (s[p] < '0' or s[p] > '9') {
            is_int = false;
            break;
        }
    }
    if (is_int) {
        const v = strtoul(s, null, 10);
        if (v > 0xFFFFFFFF) {
            _ = fprintf(stderr, "dl: integer overflow: %s\n", s);
            exit(1);
        }
        return @intCast(v);
    }
    return c.intern_str(ir, s);
}

/// Parse a concatenated hex string into an array of u32 values (no spaces,
/// no 0x prefix; each u32 is 8 hex digits).  Returns n_words, or -1.
fn parseHexWords(hex_str: [*c]const u8, words: [*]u32, n_words: usize) isize {
    const hex_len = strlen(hex_str);
    if (hex_len != n_words * 8) {
        return -1; // each u32 is 8 hex digits
    }
    var i: usize = 0;
    while (i < n_words) : (i += 1) {
        var buf: [9]u8 = undefined;
        @memcpy(buf[0..8], hex_str[i * 8 ..][0..8]);
        buf[8] = 0;
        if (sscanf(&buf, "%8x", &words[i]) != 1) {
            return -1;
        }
    }
    return @intCast(n_words);
}

/// Run the dl-embed encode helper: fork + execv (NO shell — the query is
/// passed as a single argv element, so no shell-quoting/injection surface
/// exists by construction).  Fills q_sig (VEC_SIG_WORDS) and q_int8
/// (VEC_IVEC_WORDS) from the helper's "sig_hex ivec_hex" stdout line.
/// Returns 0 on success, -1 on any failure (message printed).
fn runEncodeHelper(db_dir: [*c]const u8, query: [*c]const u8, q_sig: *[VEC_SIG_WORDS]u32, q_int8: *[VEC_IVEC_WORDS]u32) c_int {
    var helper: [PATH_MAX + 16]u8 = undefined;
    var fds: [2]c_int = undefined;

    // resolve dl-embed next to this binary, then ./dl-embed
    _ = snprintf(&helper, helper.len, "%s/dl-embed", &g_prog_dir);
    if (access(&helper, X_OK) != 0) {
        _ = snprintf(&helper, helper.len, "./dl-embed");
        if (access(&helper, X_OK) != 0) {
            _ = fprintf(stderr, "dl: dl-embed not found (build it: make dl-embed)\n");
            return -1;
        }
    }

    if (pipe(&fds) != 0) {
        _ = fprintf(stderr, "dl: pipe failed\n");
        return -1;
    }
    const pid = fork();
    if (pid < 0) {
        _ = close(fds[0]);
        _ = close(fds[1]);
        _ = fprintf(stderr, "dl: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        var cargv: [6][*c]u8 = .{
            @constCast("dl-embed"),
            @constCast("encode"),
            @constCast("--db"),
            @constCast(db_dir),
            @constCast(query),
            null,
        };
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        _ = close(fds[0]);
        _ = close(fds[1]);
        _ = execv(&helper, &cargv);
        _exit(127);
    }
    _ = close(fds[1]);
    {
        var sig_buf: [65]u8 = undefined;
        var ivec_buf: [769]u8 = undefined;
        const fp = fdopen(fds[0], "r");
        var status: c_int = 0;
        var ok = false;
        if (fp == null) {
            _ = close(fds[0]);
            _ = waitpid(pid, &status, 0);
            _ = fprintf(stderr, "dl: fdopen failed\n");
            return -1;
        }
        if (fscanf(fp.?, "%64s %768s", &sig_buf, &ivec_buf) == 2) {
            if (parseHexWords(&sig_buf, q_sig, VEC_SIG_WORDS) == VEC_SIG_WORDS and
                parseHexWords(&ivec_buf, q_int8, VEC_IVEC_WORDS) == VEC_IVEC_WORDS)
            {
                ok = true;
            }
        }
        _ = fclose(fp.?);
        if (waitpid(pid, &status, 0) < 0 or
            !wifexited(status) or wexitstatus(status) != 0)
        {
            ok = false;
        }
        if (!ok) {
            _ = fprintf(stderr, "dl: encode helper failed\n");
            return -1;
        }
    }
    return 0;
}

// ─── Vector-search collectors / printers (C structs + callbacks) ────────────

/// Callback to collect candidate sym-ids from vector search.
const VecCandCollector = struct {
    syms: ?[*]u32,
    capacity: c_int,
    count: c_int,
};

fn collectVecCand(entity_sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const col: *VecCandCollector = @ptrCast(@alignCast(user.?));
    _ = score;
    if (col.count < col.capacity) {
        col.syms.?[@intCast(col.count)] = entity_sym;
        col.count += 1;
    }
    return 0;
}

/// Callback to print ranked results from rerank.
const VecResultPrinter = struct {
    db: [*c]c.dl_db,
    printed: c_int,
};

fn printVecResult(entity_sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const p: *VecResultPrinter = @ptrCast(@alignCast(user.?));
    _ = score;
    printValue(p.db.?, entity_sym, 0);
    _ = printf("\n");
    p.printed += 1;
    return 0;
}

/// Ascending u32 comparison (for qsort/bsearch on sym-id sets).
fn cmpU32(pa: ?*const anyopaque, pb: ?*const anyopaque) callconv(.c) c_int {
    const a = @as(*const u32, @ptrCast(@alignCast(pa.?))).*;
    const b = @as(*const u32, @ptrCast(@alignCast(pb.?))).*;
    return @as(c_int, @intFromBool(a > b)) - @as(c_int, @intFromBool(a < b));
}

/// Callback: capture the entity sym (col 0) of the observation row whose
/// content (col 1) equals the target obs_id.
const ObsEntityCtx = struct {
    target: u32,
    entity: u32,
};

fn collectObsEntity(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const cb: *ObsEntityCtx = @ptrCast(@alignCast(user.?));
    _ = arity;
    if (cb.target != 0 and cols[1] == cb.target) {
        cb.entity = cols[0];
        return 1; // stop: obs_id is unique in observation
    }
    return 0;
}

/// Map a lexical obs_id (content sym) to the entity sym that owns it, via the
/// observation(entity, content) relation.  Returns 0 if the observation
/// relation is absent or the obs_id has no row.
fn obsToEntity(db: [*c]c.dl_db, obs_id: u32) u32 {
    var ctx = ObsEntityCtx{ .target = obs_id, .entity = 0 };
    const n = c.dl_prefix(db, "observation", null, 0, collectObsEntity, &ctx);
    if (n < 0)
        return 0;
    return ctx.entity;
}

// ─── Value / tuple printing (byte-exact reverse-mapping heuristics) ────────

/// Print a column value: resolve list handles (EXACT, first), then string
/// symbols (reverse-map heuristic), then raw ints.  Lists are first so list
/// printing is exact even though int-vs-symbol stays heuristic (B6).
fn printValue(db: [*c]c.dl_db, v: u32, depth: c_int) void {
    const ir = db.*.ir;
    if (c.term_is_list(db.*.terms, v) != 0) {
        printList(db, v, depth);
        return;
    }
    const s = c.intern_str_of(ir, v);
    if (s != null and s[0] != 0) {
        _ = printf("%s", s);
    } else {
        _ = printf("%u", v);
    }
}

/// Render a list handle recursively.  The DAG is well-founded (acyclic), so
/// this terminates; the depth cap is a defensive bound against a pathological
/// long list overflowing the CLI stack.
fn printList(db: [*c]c.dl_db, h_in: u32, depth: c_int) void {
    var h = h_in;
    if (depth > 4096) {
        _ = printf("[...]");
        return;
    }
    _ = printf("[");
    while (h != c.TERM_NIL) {
        printValue(db, c.term_car(db.*.terms, h), depth + 1);
        h = c.term_cdr(db.*.terms, h);
        if (h != c.TERM_NIL) _ = printf(", ");
    }
    _ = printf("]");
}

/// Callback for dl_prefix: print one tuple.
fn printTuple(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const db: [*c]c.dl_db = @ptrCast(@alignCast(user.?));
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        if (i > 0) _ = printf(" ");
        printValue(db, cols[i], 0);
    }
    _ = printf("\n");
    return 0;
}

/// Callback for dl_prefix --raw: print one tuple as raw u32 columns (no
/// symbol reverse-mapping).  Machine-consumable (used by the S4 oracle/embed
/// parsers which need the exact packed ints).
fn printTupleRaw(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = user;
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        if (i > 0) _ = printf(" ");
        _ = printf("%u", cols[i]);
    }
    _ = printf("\n");
    return 0;
}

/// Callback for traverse: print node name.
fn traversePrintCb(node_sym: u32, depth: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = depth;
    const db: [*c]c.dl_db = @ptrCast(@alignCast(user.?));
    const ir = db.*.ir;
    const s = c.intern_str_of(ir, node_sym);
    if (s != null and s[0] != 0) {
        _ = printf("%s\n", s);
    } else {
        _ = printf("%u\n", node_sym);
    }
    return 0;
}

/// Callback for obs: print observation string.
fn obsPrintCb(s: [*c]const u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = user;
    _ = printf("%s\n", s);
    return 0;
}

// ─── Usage ──────────────────────────────────────────────────────────────────

fn usage(prog: [*:0]const u8) noreturn {
    _ = fprintf(stderr,
        \\Usage:
        \\  %s [-d <dir>] load <csv> --rel <name>
        \\  %s [-d <dir>] lookup <rel> <val> [<val> ...]
        \\  %s [-d <dir>] prefix <rel> [<val> ...]
        \\  %s [-d <dir>] query '<rule>' | <file.dl> <goal-rel>
        \\  %s [-d <dir>] qmagic '<rule>' | <file.dl> <goal-rel> [-a <adorn>] <val> [<val> ...]
        \\  %s [-d <dir>] publish [--keep N]
        \\  %s [-d <dir>] bound <rel> <val> [<val> ...]
        \\  %s [-d <dir>] pattern <rel> [<col>] '<regex>'
        \\  %s [-d <dir>] rev <entity>
        \\  %s [-d <dir>] cas <entity> <expected> <new>
        \\  %s [-d <dir>] txn
        \\  %s [-d <dir>] traverse <start> [depth] [--max-nodes N]
        \\  %s [-d <dir>] obs <node> [--max-obs N]
        \\  %s [-d <dir>] index
        \\  %s [-d <dir>] search '<terms>' [--top N] [--version N] (--version 0 = current)
        \\  %s [-d <dir>] vsearch '<query>' [--k N] [--radius R] [--version V] [--sig <hex64>] [--ivec <hex768>]
        \\  %s [-d <dir>] vhybrid '<terms>' '<query>' [--k N] [--radius R] [--version V]
        \\  %s [-d <dir>] versions
        \\Values: bare integer -> raw u32; anything else -> interned string
        \\Hex args: concatenated hex digits (no spaces, no 0x prefix)
        \\
    , prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
    exit(1);
}

// ─── Main ───────────────────────────────────────────────────────────────────

pub fn main(init: std.process.Init) void {
    const args = init.minimal.args.toSlice(init.arena.allocator()) catch
        @panic("dl: cannot read argv");
    exit(realMain(@intCast(args.len), args));
}

fn realMain(argc: c_int, argv: []const [:0]const u8) c_int {
    var db_dir: [*:0]const u8 = "dl-test-db";
    var argp: usize = 1;

    if (argc < 2) usage(argv[0].ptr);

    // record this binary's directory for the dl-embed helper lookup
    {
        const prog0 = argv[0];
        var n = prog0.len;
        if (n >= g_prog_dir.len) n = g_prog_dir.len - 1;
        @memcpy(g_prog_dir[0..n], prog0[0..n]);
        g_prog_dir[n] = 0;
        const slash = strrchr(&g_prog_dir, '/');
        if (slash != null) {
            slash[0] = 0;
        } else {
            _ = snprintf(&g_prog_dir, g_prog_dir.len, ".");
        }
    }

    // Parse -d <dir>
    if (argp < argv.len and eq(argv[argp], "-d")) {
        argp += 1;
        if (argp >= argv.len) usage(argv[0].ptr);
        db_dir = argv[argp].ptr;
        argp += 1;
    }

    if (argp >= argv.len) usage(argv[0].ptr);
    const cmd = argv[argp];
    argp += 1;

    const db = c.dl_open(db_dir);
    if (db == null) {
        _ = fprintf(stderr, "dl: cannot open database at %s\n", db_dir);
        return 1;
    }

    if (eq(cmd, "load")) {
        var rel_name: ?[*:0]const u8 = null;

        if (argp >= argv.len) usage(argv[0].ptr);
        const csv_path = argv[argp];
        argp += 1;

        while (argp < argv.len) {
            if (eq(argv[argp], "--rel") and argp + 1 < argv.len) {
                argp += 1;
                rel_name = argv[argp];
                argp += 1;
            } else {
                usage(argv[0].ptr);
            }
        }

        if (rel_name == null) usage(argv[0].ptr);

        // Peek at the first non-empty CSV line to determine arity
        {
            const f = fopen(csv_path.ptr, "r");
            var line: ?[*]u8 = null;
            var cap: usize = 0;
            var arity: c_int = 0;

            if (f == null) {
                _ = fprintf(stderr, "dl: cannot open %s\n", csv_path.ptr);
                c.dl_close(db);
                return 1;
            }

            while (true) {
                // (getline returns the \n-inclusive length; C uses that value,
                // NOT strlen — an embedded NUL must not change the arithmetic)
                const glen = getline(&line, &cap, f.?);
                if (glen <= 0) break;
                var len: usize = @intCast(glen);
                if (len > 0 and line.?[len - 1] == '\n') {
                    len -= 1;
                    line.?[len] = 0;
                }
                if (len > 0 and line.?[len - 1] == '\r') {
                    len -= 1;
                    line.?[len] = 0;
                }
                if (len == 0) continue;

                arity = 1;
                var p: usize = 0;
                var in_quote = false;
                while (line.?[p] != 0) : (p += 1) {
                    if (line.?[p] == '"') in_quote = !in_quote;
                    if (line.?[p] == ',' and !in_quote) arity += 1;
                }
                break;
            }
            free(line);
            _ = fclose(f.?);

            if (arity == 0 or arity > 8) {
                _ = fprintf(stderr, "dl: could not determine arity from %s\n", csv_path.ptr);
                c.dl_close(db);
                return 1;
            }

            if (c.dl_declare_relation(db, rel_name.?, @intCast(arity)) != 0) {
                _ = fprintf(stderr, "dl: cannot declare relation %s/%d\n", rel_name.?, arity);
                c.dl_close(db);
                return 1;
            }
        }

        {
            const loaded = c.dl_load_facts(db, rel_name.?, csv_path.ptr);
            if (loaded < 0) {
                _ = fprintf(stderr, "dl: load failed\n");
                c.dl_close(db);
                return 1;
            }
            _ = printf("Loaded %d facts into %s\n", loaded, rel_name.?);
        }

        c.dl_close(db);
        return 0;
    }

    // Every command below shares the C's fall-through structure: exactly one
    // dl_close + return at the end, so branch bodies use a labeled block with
    // early returns instead of mirroring the else-if chain.
    defer c.dl_close(db);

    if (eq(cmd, "lookup")) {
        var cols: [8]u32 = undefined;
        var arity: u8 = 0;

        if (argp >= argv.len) usage(argv[0].ptr);
        const rel_name = argv[argp];
        argp += 1;

        while (argp < argv.len and arity < 8) {
            cols[arity] = cliParseValue(db, argv[argp].ptr);
            arity += 1;
            argp += 1;
        }

        if (arity == 0) usage(argv[0].ptr);

        const found = c.dl_lookup(db, rel_name.ptr, &cols, arity);
        _ = printf("%s\n", if (found != 0) @as([*:0]const u8, "found") else @as([*:0]const u8, "not found"));
        return 0;
    }

    if (eq(cmd, "prefix")) {
        var leading: [8]u32 = undefined;
        var k: u8 = 0;
        var raw = false;

        if (argp >= argv.len) usage(argv[0].ptr);
        const rel_name = argv[argp];
        argp += 1;

        if (argp < argv.len and eq(argv[argp], "--raw")) {
            raw = true;
            argp += 1;
        }

        while (argp < argv.len and k < 8) {
            leading[k] = cliParseValue(db, argv[argp].ptr);
            k += 1;
            argp += 1;
        }

        const n = c.dl_prefix(db, rel_name.ptr, &leading, k, if (raw) printTupleRaw else printTuple, db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: prefix query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "query")) {
        if (argp >= argv.len) usage(argv[0].ptr);
        var source: [*c]const u8 = argv[argp].ptr;
        argp += 1;

        if (argp >= argv.len) usage(argv[0].ptr);
        const goal_rel = argv[argp];
        argp += 1;

        // Check if source is a file path
        var source_buf: ?*anyopaque = null;
        {
            const f = fopen(source, "r");
            if (f != null) {
                _ = fseek(f.?, 0, SEEK_END);
                const sz = ftell(f.?);
                _ = fseek(f.?, 0, SEEK_SET);
                if (sz > 0 and sz < 1024 * 1024) {
                    const buf = malloc(@intCast(sz + 1));
                    if (buf != null) {
                        const nr = fread(@ptrCast(buf.?), 1, @intCast(sz), f.?);
                        (@as([*]u8, @ptrCast(buf.?)))[nr] = 0;
                        source_buf = buf;
                        source = @ptrCast(buf.?);
                    }
                }
                _ = fclose(f.?);
            }
        }

        if (c.dl_load_rules(db, source) != 0) {
            _ = fprintf(stderr, "dl: failed to parse/compile rules\n");
            free(source_buf);
            return 1;
        }
        free(source_buf);

        // Publish (runs VM automatically if fixpoint_dirty), then query
        if (c.dl_publish_snapshot(db) != 0) {
            _ = fprintf(stderr, "dl: publish failed\n");
            return 1;
        }

        const n = c.dl_query(db, goal_rel.ptr, printTuple, db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "qmagic")) {
        if (argp >= argv.len) usage(argv[0].ptr);
        var source: [*c]const u8 = argv[argp].ptr;
        argp += 1;

        if (argp >= argv.len) usage(argv[0].ptr);
        const goal_rel = argv[argp];
        argp += 1;

        // Optional -a <adorn>: arbitrary-adornment form (e.g. "fb" binds
        // position 1).  Without it, the legacy leading-prefix form is used.
        var adorn: ?[*:0]const u8 = null;
        if (argp < argv.len and eq(argv[argp], "-a")) {
            argp += 1;
            if (argp >= argv.len) usage(argv[0].ptr);
            adorn = argv[argp];
            argp += 1;
        }

        // Check if source is a file path
        var source_buf: ?*anyopaque = null;
        {
            const f = fopen(source, "r");
            if (f != null) {
                _ = fseek(f.?, 0, SEEK_END);
                const sz = ftell(f.?);
                _ = fseek(f.?, 0, SEEK_SET);
                if (sz > 0 and sz < 1024 * 1024) {
                    const buf = malloc(@intCast(sz + 1));
                    if (buf != null) {
                        const nr = fread(@ptrCast(buf.?), 1, @intCast(sz), f.?);
                        (@as([*]u8, @ptrCast(buf.?)))[nr] = 0;
                        source_buf = buf;
                        source = @ptrCast(buf.?);
                    }
                }
                _ = fclose(f.?);
            }
        }

        var vals: [8]u32 = undefined;
        var nvals: u8 = 0;
        while (argp < argv.len and nvals < 8) {
            vals[nvals] = cliParseValue(db, argv[argp].ptr);
            nvals += 1;
            argp += 1;
        }

        if (c.dl_load_rules(db, source) != 0) {
            _ = fprintf(stderr, "dl: failed to parse/compile rules\n");
            free(source_buf);
            return 1;
        }
        free(source_buf);

        var n: c_long = undefined;
        if (adorn != null)
            n = c.dl_query_magic_adorn(db, goal_rel.ptr, adorn.?, &vals, nvals, printTuple, db)
        else
            n = c.dl_query_magic(db, goal_rel.ptr, &vals, nvals, printTuple, db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: magic query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "publish")) {
        // Optional --keep N: retain only the N most-recent snapshots, pruning
        // older ones after this publish (opt-in; 0 disables retention).
        var keep: c_uint = 0;
        if (argp < argv.len) {
            if (argv.len - argp == 2 and eq(argv[argp], "--keep")) {
                keep = @intCast(strtoul(argv[argp + 1].ptr, null, 10));
                argp += 2;
            } else {
                usage(argv[0].ptr);
            }
        }
        _ = c.dl_set_snapshot_retain(db, keep);
        if (c.dl_publish_snapshot(db) != 0) {
            _ = fprintf(stderr, "dl: publish failed\n");
            return 1;
        }
        _ = printf("Snapshot published.\n");
        return 0;
    }

    if (eq(cmd, "bound")) {
        var leading: [8]u32 = undefined;
        var k: u8 = 0;

        if (argp >= argv.len) usage(argv[0].ptr);
        const rel_name = argv[argp];
        argp += 1;

        while (argp < argv.len and k < 8) {
            leading[k] = cliParseValue(db, argv[argp].ptr);
            k += 1;
            argp += 1;
        }

        const n = c.dl_query_bound(db, rel_name.ptr, &leading, k, printTuple, db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: bound query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "pattern")) {
        var col: u8 = 0;

        if (argp >= argv.len) usage(argv[0].ptr);
        const rel_name = argv[argp];
        argp += 1;

        // Optional column argument
        if (argp < argv.len) {
            var endptr: [*c]u8 = null;
            const col_val = strtol(argv[argp].ptr, &endptr, 10);
            if (endptr[0] == 0 and col_val >= 0 and col_val <= 255) {
                col = @intCast(col_val);
                argp += 1;
            }
        }

        if (argp >= argv.len) usage(argv[0].ptr);
        const pattern = argv[argp];
        argp += 1;

        const dfa = c.regex_compile(pattern.ptr);
        if (dfa.*.errmsg != null) {
            _ = fprintf(stderr, "dl: bad pattern\n");
            c.regex_dfa_free(dfa);
            return 1;
        }

        const n = c.dl_pattern(db, rel_name.ptr, col, dfa, printTuple, db);
        c.regex_dfa_free(dfa);
        if (n < 0) {
            _ = fprintf(stderr, "dl: pattern query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "rev")) {
        if (argp >= argv.len) usage(argv[0].ptr);
        const entity = argv[argp];
        argp += 1;

        var r: u32 = undefined;
        if (c.dl_rev_get(db, entity.ptr, &r) != 0) {
            _ = fprintf(stderr, "dl: rev lookup failed\n");
            return 1;
        }
        _ = printf("%u\n", r);
        return 0;
    }

    if (eq(cmd, "cas")) {
        if (argp + 2 >= argv.len) usage(argv[0].ptr);
        const entity = argv[argp];
        argp += 1;
        const exp_s = argv[argp];
        argp += 1;
        const new_s = argv[argp];
        argp += 1;

        // Strict unsigned-32 parsing: reject non-numeric input and overflow
        // instead of silently truncating via strtoul (which parses garbage as
        // 0, making `dl cas e abc def` look like a conflict).
        var expected: c_ulong = undefined;
        var new_value: c_ulong = undefined;
        if (!parseU32Strict(exp_s.ptr, &expected) or
            !parseU32Strict(new_s.ptr, &new_value))
        {
            _ = fprintf(stderr, "dl: cas: expected and new must be unsigned 32-bit integers\n");
            return 1;
        }

        const rc = c.dl_cas_revision(db, entity.ptr, @intCast(expected), @intCast(new_value));
        if (rc == 0) {
            _ = printf("ok\n");
        } else if (rc == DL_E_CONFLICT) {
            _ = printf("conflict\n");
        } else {
            _ = fprintf(stderr, "dl: cas error\n");
            return 1;
        }
        return 0;
    }

    if (eq(cmd, "txn")) {
        // Fixed demo: begin, CAS demo-entity 0->1, add a fact on a small
        // declared relation, commit; print the result.
        if (c.dl_declare_relation(db, "tnotes", 2) != 0) {
            _ = fprintf(stderr, "dl: cannot declare relation tnotes\n");
            return 1;
        }
        const sym_hello = c.dl_intern_str(db, "hello");
        const sym_world = c.dl_intern_str(db, "world");
        var cols: [2]u32 = .{ sym_hello, sym_world };

        if (c.dl_txn_begin(db) != 0) {
            _ = fprintf(stderr, "dl: txn_begin failed\n");
            return 1;
        }
        if (c.dl_txn_cas(db, "demo-entity", 0, 1) != 0 or
            c.dl_txn_add_fact(db, "tnotes", &cols, 2) != 0)
        {
            _ = fprintf(stderr, "dl: txn buffer failed\n");
            _ = c.dl_txn_rollback(db);
            return 1;
        }
        const rc = c.dl_txn_commit(db);
        if (rc == 0) {
            _ = printf("txn committed\n");
        } else if (rc == DL_E_CONFLICT) {
            _ = printf("txn conflict\n");
        } else {
            _ = fprintf(stderr, "dl: txn commit error\n");
            return 1;
        }
        return 0;
    }

    if (eq(cmd, "traverse")) {
        var depth: c_int = 1;
        var max_nodes: c_ulong = 1000;

        if (argp >= argv.len) usage(argv[0].ptr);
        const start = argv[argp];
        argp += 1;

        // Optional [depth] then optional [--max-nodes N].  A malformed
        // numeric arg is an error, not a silent fallback to depth 1.
        if (argp < argv.len and argv[argp].len > 0 and argv[argp][0] == '-' and
            !eqFirst11(argv[argp], "--max-nodes"))
        {
            _ = fprintf(stderr, "dl: invalid argument '%s'\n", argv[argp].ptr);
            return 1;
        }
        if (argp < argv.len and !eqFirst11(argv[argp], "--max-nodes")) {
            var d: c_ulong = undefined;
            if (!parseU32Strict(argv[argp].ptr, &d) or d < 1 or d > 3) {
                _ = fprintf(stderr, "dl: invalid depth '%s' (expect 1..3)\n", argv[argp].ptr);
                return 1;
            }
            depth = @intCast(d);
            argp += 1;
        }
        if (argp < argv.len and eq(argv[argp], "--max-nodes")) {
            if (argp + 1 >= argv.len) {
                _ = fprintf(stderr, "dl: invalid --max-nodes value\n");
                return 1;
            }
            if (!parseU32Strict(argv[argp + 1].ptr, &max_nodes) or max_nodes < 1) {
                _ = fprintf(stderr, "dl: invalid --max-nodes value\n");
                return 1;
            }
            argp += 2;
        }
        if (argp < argv.len) {
            _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
            return 1;
        }

        const n = c.dl_traverse(db, start.ptr, depth, cIntTrunc(max_nodes), traversePrintCb, db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: traverse failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "obs")) {
        var max_obs: c_ulong = 100;

        if (argp >= argv.len) usage(argv[0].ptr);
        const node = argv[argp];
        argp += 1;

        if (argp < argv.len and eq(argv[argp], "--max-obs")) {
            if (argp + 1 >= argv.len) {
                _ = fprintf(stderr, "dl: invalid --max-obs value\n");
                return 1;
            }
            if (!parseU32Strict(argv[argp + 1].ptr, &max_obs) or max_obs < 1) {
                _ = fprintf(stderr, "dl: invalid --max-obs value\n");
                return 1;
            }
            argp += 2;
        }
        if (argp < argv.len) {
            _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
            return 1;
        }

        const n = c.dl_node_observations(db, node.ptr, cIntTrunc(max_obs), obsPrintCb, null);
        if (n < 0) {
            _ = fprintf(stderr, "dl: obs failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no observations)\n");
        return 0;
    }

    if (eq(cmd, "index")) {
        if (argp < argv.len) {
            _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
            return 1;
        }

        const n = c.dl_index_observations(db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: index failed\n");
            return 1;
        }
        _ = printf("Indexed %ld postings\n", n);
        return 0;
    }

    if (eq(cmd, "search")) {
        if (argp >= argv.len) usage(argv[0].ptr);
        const terms_str = argv[argp];
        argp += 1;

        var top_n: c_ulong = 10;
        var version: c_ulong = 0; // 0 means live (current)

        // Parse optional flags in any order
        while (argp < argv.len) {
            if (eq(argv[argp], "--top")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &top_n) or top_n < 1 or
                    top_n > 0x7FFFFFFF)
                {
                    _ = fprintf(stderr, "dl: invalid --top value (must be 1..2147483647)\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--version")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &version)) {
                    _ = fprintf(stderr, "dl: invalid --version value\n");
                    return 1;
                }
                argp += 1;
            } else {
                _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
                return 1;
            }
        }

        // Tokenize the search terms
        const tokens = c.tokenize(terms_str.ptr, null);
        if (tokens == null) {
            _ = fprintf(stderr, "dl: tokenization failed\n");
            return 1;
        }

        // Count tokens
        var n_tokens: usize = 0;
        while (tokens[n_tokens] != null) n_tokens += 1;

        if (n_tokens == 0) {
            _ = fprintf(stderr, "dl: no valid terms in search query\n");
            c.token_free(tokens);
            return 1;
        }

        // Intern all tokens
        const term_syms = malloc(n_tokens * 4);
        if (term_syms == null) {
            c.token_free(tokens);
            return 1;
        }
        const syms: [*]u32 = @ptrCast(@alignCast(term_syms.?));
        for (0..n_tokens) |i| {
            syms[i] = c.dl_intern_str(db, tokens[i]);
            if (syms[i] == 0) {
                _ = fprintf(stderr, "dl: intern failed for term '%s'\n", tokens[i]);
                free(term_syms);
                c.token_free(tokens);
                return 1;
            }
        }
        c.token_free(tokens);

        // Ensure postings relation exists
        if (c.aux_index_ensure_postings(db) != 0) {
            _ = fprintf(stderr, "dl: cannot ensure postings relation\n");
            free(term_syms);
            return 1;
        }

        // Collect results
        const obs_ids = malloc(top_n * 4);
        const scores = malloc(top_n * 4);
        if (obs_ids == null or scores == null) {
            free(obs_ids);
            free(scores);
            free(term_syms);
            return 1;
        }

        var n_results: c_int = undefined;
        if (version == 0) {
            n_results = c.dl_search_top(db, syms, @intCast(n_tokens), @ptrCast(@alignCast(obs_ids.?)), @ptrCast(@alignCast(scores.?)), @intCast(top_n));
        } else {
            n_results = c.dl_search_top_version(db, @intCast(version), syms, @intCast(n_tokens), @ptrCast(@alignCast(obs_ids.?)), @ptrCast(@alignCast(scores.?)), @intCast(top_n));
        }
        free(term_syms);

        if (n_results < 0) {
            _ = fprintf(stderr, "dl: search failed\n");
            free(obs_ids);
            free(scores);
            return 1;
        }

        // Print results
        const ids: [*]u32 = @ptrCast(@alignCast(obs_ids.?));
        const scs: [*]c_int = @ptrCast(@alignCast(scores.?));
        for (0..@intCast(n_results)) |i| {
            _ = printf("%u (score=%d)\n", ids[i], scs[i]);
        }
        if (n_results == 0)
            _ = printf("(no results)\n");

        free(obs_ids);
        free(scores);
        return 0;
    }

    if (eq(cmd, "vsearch")) {
        if (argp >= argv.len) usage(argv[0].ptr);
        const query_str = argv[argp];
        argp += 1;

        var k: c_ulong = 10;
        var radius: c_ulong = 2;
        var version: c_ulong = 0;
        var sig_hex: ?[*:0]const u8 = null;
        var ivec_hex: ?[*:0]const u8 = null;
        var cand_only = false;

        while (argp < argv.len) {
            if (eq(argv[argp], "--k")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &k) or k < 1 or k > 0x7FFFFFFF) {
                    _ = fprintf(stderr, "dl: invalid --k value\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--radius")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &radius)) {
                    _ = fprintf(stderr, "dl: invalid --radius value\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--version")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &version)) {
                    _ = fprintf(stderr, "dl: invalid --version value\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--sig")) {
                argp += 1;
                if (argp >= argv.len) {
                    _ = fprintf(stderr, "dl: --sig requires a hex string\n");
                    return 1;
                }
                sig_hex = argv[argp];
                argp += 1;
            } else if (eq(argv[argp], "--ivec")) {
                argp += 1;
                if (argp >= argv.len) {
                    _ = fprintf(stderr, "dl: --ivec requires a hex string\n");
                    return 1;
                }
                ivec_hex = argv[argp];
                argp += 1;
            } else if (eq(argv[argp], "--cand-only")) {
                argp += 1;
                cand_only = true;
            } else {
                _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
                return 1;
            }
        }

        // Parse or encode query signature and int8 vector
        var q_sig: [VEC_SIG_WORDS]u32 = undefined;
        var q_int8: [VEC_IVEC_WORDS]u32 = undefined;

        if (sig_hex != null and ivec_hex != null) {
            // Programmatic path: use provided hex
            if (parseHexWords(sig_hex.?, &q_sig, VEC_SIG_WORDS) != VEC_SIG_WORDS) {
                _ = fprintf(stderr, "dl: invalid --sig (need 64 hex chars = 8 u32)\n");
                return 1;
            }
            if (parseHexWords(ivec_hex.?, &q_int8, VEC_IVEC_WORDS) != VEC_IVEC_WORDS) {
                _ = fprintf(stderr, "dl: invalid --ivec (need 768 hex chars = 96 u32)\n");
                return 1;
            }
        } else if (sig_hex != null or ivec_hex != null) {
            _ = fprintf(stderr, "dl: provide both --sig and --ivec, or neither\n");
            return 1;
        } else {
            // Encode the query via ./dl-embed (fork+execv — no shell, so
            // the user-controlled query can never inject commands).
            if (runEncodeHelper(db_dir, query_str.ptr, &q_sig, &q_int8) != 0) {
                return 1;
            }
        }

        // Collect candidates from vector search
        const cand_syms = malloc(k * 10 * 4);
        if (cand_syms == null) {
            _ = fprintf(stderr, "dl: OOM\n");
            return 1;
        }
        var collector = VecCandCollector{ .syms = @ptrCast(@alignCast(cand_syms.?)), .capacity = cIntTrunc(k * 10), .count = 0 };

        var n_cand: c_long = undefined;
        if (version == 0) {
            n_cand = c.dl_vector_search(db, &q_sig, cIntTrunc(k * 10), cIntTrunc(radius), collectVecCand, &collector);
        } else {
            n_cand = c.dl_vector_search_version(db, @intCast(version), &q_sig, cIntTrunc(k * 10), cIntTrunc(radius), collectVecCand, &collector);
        }
        if (n_cand < 0) {
            _ = fprintf(stderr, "dl: vector search failed\n");
            free(cand_syms);
            return 1;
        }

        // Re-rank candidates (or, with --cand-only, print the raw MIH
        // candidate set — used by the S4 int8-vs-float precision oracle).
        var printer = VecResultPrinter{ .db = db, .printed = 0 };
        var n_results: c_long = undefined;
        if (cand_only) {
            const csyms: [*]u32 = @ptrCast(@alignCast(cand_syms.?));
            for (0..@intCast(collector.count)) |i|
                _ = printf("%u\n", csyms[i]);
            n_results = collector.count;
        } else if (version == 0) {
            n_results = c.dl_vector_rerank(db, &q_int8, @ptrCast(@alignCast(cand_syms.?)), collector.count, cIntTrunc(k), printVecResult, &printer);
        } else {
            n_results = c.dl_vector_rerank(db, &q_int8, @ptrCast(@alignCast(cand_syms.?)), collector.count, cIntTrunc(k), printVecResult, &printer);
        }
        if (n_results < 0) {
            _ = fprintf(stderr, "dl: vector rerank failed\n");
            free(cand_syms);
            return 1;
        }

        if (!cand_only and printer.printed == 0)
            _ = printf("(no results)\n");

        free(cand_syms);
        return 0;
    }

    if (eq(cmd, "vhybrid")) {
        if (argp + 1 >= argv.len) usage(argv[0].ptr);
        const terms_str = argv[argp];
        argp += 1;
        const query_str = argv[argp];
        argp += 1;

        var k: c_ulong = 10;
        var radius: c_ulong = 2;
        var version: c_ulong = 0;
        var sig_hex: ?[*:0]const u8 = null;
        var ivec_hex: ?[*:0]const u8 = null;

        while (argp < argv.len) {
            if (eq(argv[argp], "--k")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &k) or k < 1 or k > 0x7FFFFFFF) {
                    _ = fprintf(stderr, "dl: invalid --k value\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--radius")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &radius)) {
                    _ = fprintf(stderr, "dl: invalid --radius value\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--version")) {
                argp += 1;
                if (argp >= argv.len or !parseU32Strict(argv[argp].ptr, &version)) {
                    _ = fprintf(stderr, "dl: invalid --version value\n");
                    return 1;
                }
                argp += 1;
            } else if (eq(argv[argp], "--sig")) {
                argp += 1;
                if (argp >= argv.len) {
                    _ = fprintf(stderr, "dl: --sig requires a hex string\n");
                    return 1;
                }
                sig_hex = argv[argp];
                argp += 1;
            } else if (eq(argv[argp], "--ivec")) {
                argp += 1;
                if (argp >= argv.len) {
                    _ = fprintf(stderr, "dl: --ivec requires a hex string\n");
                    return 1;
                }
                ivec_hex = argv[argp];
                argp += 1;
            } else {
                _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
                return 1;
            }
        }

        // Parse or encode the query signature and int8 vector.
        var q_sig: [VEC_SIG_WORDS]u32 = undefined;
        var q_int8: [VEC_IVEC_WORDS]u32 = undefined;
        if (sig_hex != null and ivec_hex != null) {
            if (parseHexWords(sig_hex.?, &q_sig, VEC_SIG_WORDS) != VEC_SIG_WORDS) {
                _ = fprintf(stderr, "dl: invalid --sig (need 64 hex chars = 8 u32)\n");
                return 1;
            }
            if (parseHexWords(ivec_hex.?, &q_int8, VEC_IVEC_WORDS) != VEC_IVEC_WORDS) {
                _ = fprintf(stderr, "dl: invalid --ivec (need 768 hex chars = 96 u32)\n");
                return 1;
            }
        } else if (sig_hex != null or ivec_hex != null) {
            _ = fprintf(stderr, "dl: provide both --sig and --ivec, or neither\n");
            return 1;
        } else {
            // Encode the query via ./dl-embed (fork+execv — no shell, so
            // the user-controlled query can never inject commands).
            if (runEncodeHelper(db_dir, query_str.ptr, &q_sig, &q_int8) != 0) {
                return 1;
            }
        }

        // Get lexical candidates
        const tokens = c.tokenize(terms_str.ptr, null);
        if (tokens == null) {
            _ = fprintf(stderr, "dl: tokenization failed\n");
            return 1;
        }
        var n_tokens: usize = 0;
        while (tokens[n_tokens] != null) n_tokens += 1;
        if (n_tokens == 0) {
            _ = fprintf(stderr, "dl: no valid terms\n");
            c.token_free(tokens);
            return 1;
        }

        const term_syms = malloc(n_tokens * 4);
        if (term_syms == null) {
            c.token_free(tokens);
            return 1;
        }
        const syms: [*]u32 = @ptrCast(@alignCast(term_syms.?));
        for (0..n_tokens) |i| {
            syms[i] = c.dl_intern_str(db, tokens[i]);
            if (syms[i] == 0) {
                _ = fprintf(stderr, "dl: intern failed for '%s'\n", tokens[i]);
                free(term_syms);
                c.token_free(tokens);
                return 1;
            }
        }
        c.token_free(tokens);

        const lex_ids = malloc(k * 10 * 4);
        const lex_scores = malloc(k * 10 * 4);
        if (lex_ids == null or lex_scores == null) {
            free(lex_ids);
            free(lex_scores);
            free(term_syms);
            return 1;
        }

        var n_lex: c_int = undefined;
        if (version == 0) {
            n_lex = c.dl_search_top(db, syms, @intCast(n_tokens), @ptrCast(@alignCast(lex_ids.?)), @ptrCast(@alignCast(lex_scores.?)), cIntTrunc(k * 10));
        } else {
            n_lex = c.dl_search_top_version(db, @intCast(version), syms, @intCast(n_tokens), @ptrCast(@alignCast(lex_ids.?)), @ptrCast(@alignCast(lex_scores.?)), cIntTrunc(k * 10));
        }
        free(term_syms);
        if (n_lex < 0) {
            _ = fprintf(stderr, "dl: lexical search failed\n");
            free(lex_ids);
            free(lex_scores);
            return 1;
        }

        // Get vector candidates
        const vec_syms = malloc(k * 10 * 4);
        if (vec_syms == null) {
            free(lex_ids);
            free(lex_scores);
            return 1;
        }
        var vec_collector = VecCandCollector{ .syms = @ptrCast(@alignCast(vec_syms.?)), .capacity = cIntTrunc(k * 10), .count = 0 };
        var n_vec: c_long = undefined;
        if (version == 0) {
            n_vec = c.dl_vector_search(db, &q_sig, cIntTrunc(k * 10), cIntTrunc(radius), collectVecCand, &vec_collector);
        } else {
            n_vec = c.dl_vector_search_version(db, @intCast(version), &q_sig, cIntTrunc(k * 10), cIntTrunc(radius), collectVecCand, &vec_collector);
        }
        if (n_vec < 0) {
            _ = fprintf(stderr, "dl: vector search failed\n");
            free(vec_syms);
            free(lex_ids);
            free(lex_scores);
            return 1;
        }

        // INTERSECT: build a sorted set of ENTITY sym-ids from the lexical
        // results (dl_search_top returns obs/content sym-ids; map each to its
        // owning entity via the observation relation), keep only the vector
        // candidates that are also lexical hits, then re-rank the
        // intersection by exact int8 cosine (post-hoc join on sym-ids).
        const inter = malloc(@as(usize, @intCast(vec_collector.count)) * 4);
        if (inter == null) {
            _ = fprintf(stderr, "dl: OOM\n");
            free(vec_syms);
            free(lex_ids);
            free(lex_scores);
            return 1;
        }
        const ids: [*]u32 = @ptrCast(@alignCast(lex_ids.?));
        const vsyms: [*]u32 = @ptrCast(@alignCast(vec_syms.?));
        const isyms: [*]u32 = @ptrCast(@alignCast(inter.?));
        {
            var n_lex_ent: c_int = 0;
            for (0..@intCast(n_lex)) |i| {
                const e = obsToEntity(db, ids[i]);
                if (e != 0) {
                    ids[@intCast(n_lex_ent)] = e;
                    n_lex_ent += 1;
                }
            }
            n_lex = n_lex_ent;
        }
        var n_inter: usize = 0;
        qsort(lex_ids, @intCast(n_lex), 4, cmpU32);
        for (0..@intCast(vec_collector.count)) |i| {
            if (bsearch(@ptrCast(&vsyms[i]), lex_ids, @intCast(n_lex), 4, cmpU32) != null) {
                isyms[n_inter] = vsyms[i];
                n_inter += 1;
            }
        }

        if (n_inter > 0) {
            var printer = VecResultPrinter{ .db = db, .printed = 0 };
            const n_rank = c.dl_vector_rerank(db, &q_int8, @ptrCast(@alignCast(inter.?)), @intCast(n_inter), cIntTrunc(k), printVecResult, &printer);
            if (n_rank < 0) {
                _ = fprintf(stderr, "dl: vector rerank failed\n");
                free(inter);
                free(vec_syms);
                free(lex_ids);
                free(lex_scores);
                return 1;
            }
            if (printer.printed == 0)
                _ = printf("(no results)\n");
        } else {
            _ = printf("(no results)\n");
        }

        free(inter);
        free(vec_syms);
        free(lex_ids);
        free(lex_scores);
        return 0;
    }

    if (eq(cmd, "versions")) {
        if (argp < argv.len) {
            _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
            return 1;
        }

        var versions: [256]u32 = undefined;
        const n = c.dl_snapshot_versions(db, &versions, 256);
        if (n < 0) {
            _ = fprintf(stderr, "dl: versions query failed\n");
            return 1;
        }
        for (0..@intCast(n)) |i| {
            _ = printf("%u\n", versions[i]);
        }
        if (n == 0)
            _ = printf("(no versions published)\n");
        return 0;
    }

    usage(argv[0].ptr);
}
