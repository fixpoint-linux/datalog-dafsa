//! dl_cli.zig — Zig port of src/dl_cli.c (the `dl` CLI executable).
//!
//! Ported in U12 as the strangler migration's final exe piece: the C dl_cli.c
//! stays in the repo as the untouched ORACLE, and this exe is what the CLI
//! byte-diff harness (zig/tests/cli_diff.sh) compares against it byte-for-byte.
//!
//! PRINT FIDELITY is the whole point: every stdout/stderr byte must match the
//! C oracle, so all output goes through the SAME libc printf/fprintf calls
//! with the SAME format strings, all numeric parsing through the SAME
//! strtoul/strtol semantics (including their whitespace/sign quirks),
//! all allocation through libc malloc/free (so huge --top/--k OOMs print the
//! same nothing), and termination through libc exit (it flushes stdio exactly
//! like `return` from C main).  Engine calls resolve to
//! the libdatalog.so exports at link time: dl_* from the ported dl.zig,
//! regex_compile/regex_dfa_free from regexwalk.zig, intern_*/term_* from
//! intern.zig/termstore.zig, and tokenize/aux_index_ensure_postings/
//! dl_index_observations/dl_search_top* from the ported index.zig.
//!
//! The dl_db layout is reached through the shared internal module
//! (@import of dl_internal.zig) exactly like the C CLI — db->ir and db->terms
//! for CLI value parsing / list printing.  Layout drift is impossible: dl.zig
//! comptime-gates its authoritative mirror against the same module.

const std = @import("std");

// The datalog engine is now Zig.  dl_internal.zig hand-declares the C-layout
// dl_db struct and the whole engine extern ABI (dl_*/intern_*/term_*/regex_*/
// index.zig's tokenize/dl_search_top/dl_index_observations/aux_index_*), so
// dl_db type identity is shared with libdatalog's own modules.
const c = @import("dl_abi.zig");

// ─── libc decls — byte fidelity to the C oracle means using libc itself ────
extern "c" fn printf(fmt: [*:0]const u8, ...) c_int;
extern "c" fn fprintf(stream: *std.c.FILE, fmt: [*:0]const u8, ...) c_int;
extern "c" fn fopen(path: [*c]const u8, mode: [*:0]const u8) ?*std.c.FILE;
extern "c" fn fclose(stream: *std.c.FILE) c_int;
extern "c" fn fread(ptr: [*c]u8, size: usize, nmemb: usize, stream: *std.c.FILE) usize;
extern "c" fn fseek(stream: *std.c.FILE, off: c_long, whence: c_int) c_int;
extern "c" fn ftell(stream: *std.c.FILE) c_long;
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *std.c.FILE) isize;
extern "c" fn malloc(size: usize) ?*anyopaque;
extern "c" fn free(ptr: ?*anyopaque) void;
extern "c" fn exit(status: c_int) noreturn;
extern "c" fn strtoul(nptr: [*c]const u8, endptr: [*c][*c]u8, base: c_int) c_ulong;
extern "c" fn strtol(nptr: [*c]const u8, endptr: [*c][*c]u8, base: c_int) c_long;
extern "c" fn __errno_location() *c_int;
extern "c" fn setenv(name: [*:0]const u8, value: [*:0]const u8, overwrite: c_int) c_int;

/// glibc's unbuffered stderr stream global (same symbol the C oracle uses).
extern "c" var stderr: *std.c.FILE;

// ─── Constants (mirrors of the C oracle's / headers' values) ────────────────
const ERANGE = 34;
const SEEK_SET = 0;
const SEEK_END = 2;
const DL_E_CONFLICT = 2; // dl.h

/// C's (int)some_unsigned_long — truncate to the low 32 bits, reinterpreted.
inline fn cIntTrunc(v: c_ulong) c_int {
    return @bitCast(@as(u32, @truncate(v)));
}

fn eq(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}

/// CLI commands that only READ the store (opened with dl_open_ro, so they
/// work while another process holds the writer handle).  Deliberately
/// excluded, because they WRITE: load, publish, cas, txn, index, and search
/// (it interns the query terms into the live interner and ensures the
/// __postings__ relation exists).
fn commandReadsOnly(cmd: []const u8) bool {
    const ro_cmds = [_][]const u8{
        "lookup", "prefix", "bound", "pattern", "rev", "traverse", "obs", "similar", "versions",
    };
    for (ro_cmds) |n| {
        if (eq(cmd, n)) return true;
    }
    return false;
}

/// strncmp(argv[i], "--max-nodes", 11) == 0 — equal iff the first 11 bytes
/// match (a shorter arg fails at its NUL, exactly like strncmp).
fn eqFirst11(a: [:0]const u8, b: *const [11:0]u8) bool {
    return a.len >= 11 and std.mem.eql(u8, a[0..11], b);
}

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

// ─── Value / tuple printing (byte-exact reverse-mapping heuristics) ────────

/// What a printed tuple's ids MEAN: the live term store, plus the SYMBOL MAP
/// they belong to.  `version != 0` means the rows came from snapshot
/// `version` (dl_pattern / dl_query / dl_query_bound all route to the snapshot
/// whenever one is published), so names must resolve in THAT snapshot's
/// vocabulary — the live interner maps some of those ids onto other strings on
/// a store whose db-root array has diverged from the snapshot's, which printed
/// the right rows under the WRONG NAME with exit code 0.  `version == 0` is the
/// live in-memory relation (prefix and friends).
const PrintCtx = struct {
    db: [*c]c.dl_db,
    version: u32,
};

/// Print a column value: resolve list handles (EXACT, first), then string
/// symbols (reverse-map heuristic), then raw ints.  Lists are first so list
/// printing is exact even though int-vs-symbol stays heuristic (B6).
fn printValue(ctx: *const PrintCtx, v: u32, depth: c_int) void {
    const db = ctx.db;
    if (c.term_is_list(db.*.terms, v) != 0) {
        printList(ctx, v, depth);
        return;
    }
    // A version read resolves in the snapshot's map; a version whose array is
    // missing/damaged answers NULL, and the raw id is printed — never a name
    // from the other map (the version arms of dl_pattern/dl_query have already
    // failed loudly in that case; this is the fallback for a raw int column).
    const s = if (ctx.version != 0)
        c.dl_intern_str_of_version(db, ctx.version, v)
    else
        c.intern_str_of(db.*.ir, v);
    if (s != null and s[0] != 0) {
        _ = printf("%s", s);
    } else {
        _ = printf("%u", v);
    }
}

/// Render a list handle recursively.  The DAG is well-founded (acyclic), so
/// this terminates; the depth cap is a defensive bound against a pathological
/// long list overflowing the CLI stack.
fn printList(ctx: *const PrintCtx, h_in: u32, depth: c_int) void {
    const db = ctx.db;
    var h = h_in;
    if (depth > 4096) {
        _ = printf("[...]");
        return;
    }
    _ = printf("[");
    while (h != c.TERM_NIL) {
        printValue(ctx, c.term_car(db.*.terms, h), depth + 1);
        h = c.term_cdr(db.*.terms, h);
        if (h != c.TERM_NIL) _ = printf(", ");
    }
    _ = printf("]");
}

/// Callback for dl_prefix / dl_query / dl_query_bound / dl_pattern: print one
/// tuple.  `user` is a *PrintCtx (see it for the live-vs-version map rule).
fn printTuple(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *const PrintCtx = @ptrCast(@alignCast(user.?));
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        if (i > 0) _ = printf(" ");
        printValue(ctx, cols[i], 0);
    }
    _ = printf("\n");
    return 0;
}

/// Callback for dl_prefix --raw: print one tuple as raw u32 columns (no
/// symbol reverse-mapping).  Machine-consumable: the exact packed ints, with
/// no reverse-mapping heuristic in the way.
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

// ─── Fuzzy (dl similar) hit collection ─────────────────────────────────────

const FuzzyHit = struct {
    dist: u32,
    name: [*:0]const u8,
};

fn fuzzyLess(_: void, a: FuzzyHit, b: FuzzyHit) bool {
    if (a.dist != b.dist) return a.dist < b.dist;
    return std.mem.lessThan(u8, std.mem.span(a.name), std.mem.span(b.name));
}

fn collectFuzzyHit(sym_id: u32, name: [*c]const u8, name_len: usize, dist: u32, user: ?*anyopaque) callconv(.c) c_int {
    _ = sym_id;
    _ = name_len;
    const hits: *std.ArrayList(FuzzyHit) = @ptrCast(@alignCast(user orelse return -1));
    hits.append(std.heap.c_allocator, FuzzyHit{ .dist = dist, .name = @ptrCast(name) }) catch return -1;
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
        \\  %s [-d <dir>] publish [--keep N] [--with-derived]
        \\  %s [-d <dir>] bound <rel> <val> [<val> ...]
        \\  %s [-d <dir>] pattern <rel> [<col>] '<regex>'
        \\  %s [-d <dir>] similar '<name>' [--k N]  (edit distance, k = 0..2, default 1)
        \\  %s [-d <dir>] rev <entity>
        \\  %s [-d <dir>] cas <entity> <expected> <new>
        \\  %s [-d <dir>] txn
        \\  %s [-d <dir>] traverse <start> [depth] [--max-nodes N]
        \\  %s [-d <dir>] obs <node> [--max-obs N]
        \\  %s [-d <dir>] index
        \\  %s [-d <dir>] search '<terms>' [--top N] [--version N] [--min-terms N] (--version 0 = current)
        \\  %s [-d <dir>] versions
        \\Values: bare integer -> raw u32; anything else -> interned string
        \\
    , prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
    exit(1);
}

// ─── Argument-shape helpers ─────────────────────────────────────────────────
//
// The command arms share three shapes: "require a positional", "consume a
// `--flag <u32-in-range>`", and "reject a leftover argument".  These carry the
// SHAPE only — every user-visible message stays at its own call site, because
// each one is pinned byte-for-byte by the goldens (zig/tests/golden/).

/// Require one positional argument: bounds check + usage() on absence, else
/// consume it (the C's `if (argp >= argc) usage()` + take-next shape).
fn requirePositional(argv: []const [:0]const u8, argp: *usize) [:0]const u8 {
    if (argp.* >= argv.len) usage(argv[0].ptr);
    const a = argv[argp.*];
    argp.* += 1;
    return a;
}

/// Parse a `--flag <u32-in-range>` value: step over the flag name, then
/// require one token that parseU32Strict accepts inside [min, max].  Returns
/// null WITHOUT printing (having consumed nothing on failure) so the caller
/// can emit its own pinned message and return 1.  `max == null` = unbounded
/// above (still bounded by parseU32Strict's u32 cap).
fn parseFlagU32(argv: []const [:0]const u8, argp: *usize, min: c_ulong, max: ?c_ulong) ?c_ulong {
    argp.* += 1; // step over the flag name
    if (argp.* >= argv.len) return null;
    var v: c_ulong = undefined;
    if (!parseU32Strict(argv[argp.*].ptr, &v)) return null;
    if (v < min or (max != null and v > max.?)) return null;
    argp.* += 1;
    return v;
}

/// Reject a leftover argument with the shared `dl: unexpected argument '%s'`
/// message.  Returns true (having printed) iff there was one, so callers read
/// `if (rejectExtra(argv, argp)) return 1;`.
fn rejectExtra(argv: []const [:0]const u8, argp: usize) bool {
    if (argp >= argv.len) return false;
    _ = fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp].ptr);
    return true;
}

// ─── search: option parsing + evaluation ───────────────────────────────────
//
// `search` used to be one arm doing four things at once (flag parsing, the
// --min-terms/--version rejection, tokenization, and the three-way engine
// dispatch).  The options now live in SearchParams and the evaluation in
// runSearch, leaving the command arm as: take the terms, parse the flags,
// call runSearch.

/// Parsed `dl search` options (the defaults ARE the documented semantics).
const SearchParams = struct {
    top_n: c_ulong = 10, // --top: max results, 1..2147483647
    version: c_ulong = 0, // --version: 0 = live (current snapshot)
    min_terms: c_ulong = 0, // --min-terms: 0 = AND (dl_search_top)

    /// Parse the optional flags, in any order.  Returns null after printing
    /// the offending flag's own pinned message.
    fn parse(argv: []const [:0]const u8, argp: *usize) ?SearchParams {
        var p: SearchParams = .{};
        while (argp.* < argv.len) {
            if (eq(argv[argp.*], "--top")) {
                p.top_n = parseFlagU32(argv, argp, 1, 0x7FFFFFFF) orelse {
                    _ = fprintf(stderr, "dl: invalid --top value (must be 1..2147483647)\n");
                    return null;
                };
            } else if (eq(argv[argp.*], "--version")) {
                p.version = parseFlagU32(argv, argp, 0, null) orelse {
                    _ = fprintf(stderr, "dl: invalid --version value\n");
                    return null;
                };
            } else if (eq(argv[argp.*], "--min-terms")) {
                // Partial-match (OR) retrieval: a document is a result iff it
                // matches at least N of the query terms, ranked by matched-term
                // count.  Absent flag = today's AND semantics, unchanged.
                p.min_terms = parseFlagU32(argv, argp, 1, 0x7FFFFFFF) orelse {
                    _ = fprintf(stderr, "dl: invalid --min-terms value (must be 1..2147483647)\n");
                    return null;
                };
            } else {
                if (rejectExtra(argv, argp.*)) return null;
            }
        }

        // dl_search_ranked has no version-aware form (a partial-match query
        // as-of a snapshot would need the per-term collectors' version path
        // wired through it); reject the combination rather than silently
        // ignoring one of the two flags.
        if (p.min_terms != 0 and p.version != 0) {
            _ = fprintf(stderr, "dl: --min-terms cannot be combined with --version\n");
            return null;
        }
        return p;
    }
};

/// Evaluate a `dl search`: tokenize + intern the terms, ensure the postings
/// relation exists, dispatch to the engine, print the hits.  Returns the
/// process exit code (all output and codes are unchanged from the C oracle).
fn runSearch(db: [*c]c.dl_db, terms_str: [:0]const u8, opts: SearchParams) c_int {
    const top_n = opts.top_n;
    const version = opts.version;
    const min_terms = opts.min_terms;

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

    // Intern all tokens.
    //
    // LIVE-vs-VERSION READ DISCIPLINE: a `--version N` search matches ids
    // against THAT SNAPSHOT's __postings__, so the terms must be resolved in
    // that snapshot's vocabulary (dl_intern_str_version — lookup-only).  The
    // live interner is not that map: on a store whose db-root symbols.array
    // diverges from the snapshot's (a legacy store, a restored/edited root
    // array) an id it hands out names ANOTHER string as-of that snapshot, so
    // `search "blue" --version 1` answered with the "red" row.  A term the
    // snapshot does not contain resolves to 0 = absent -> no matches, never a
    // freshly minted id.
    const versioned = version != 0;
    const term_syms = malloc(n_tokens * 4);
    if (term_syms == null) {
        c.token_free(tokens);
        return 1;
    }
    const syms: [*]u32 = @ptrCast(@alignCast(term_syms.?));
    var absent_from_version = false;
    for (0..n_tokens) |i| {
        if (versioned) {
            var sym: u32 = 0;
            if (c.dl_intern_str_version(db, @intCast(version), tokens[i], &sym) != 0) {
                // Missing/damaged snapshot vocabulary is an ERROR, not an
                // empty term set (same message as a failed version read).
                _ = fprintf(stderr, "dl: search failed\n");
                free(term_syms);
                c.token_free(tokens);
                return 1;
            }
            if (sym == 0) absent_from_version = true;
            syms[i] = sym;
        } else {
            syms[i] = c.dl_intern_str(db, tokens[i]);
            if (syms[i] == 0) {
                _ = fprintf(stderr, "dl: intern failed for term '%s'\n", tokens[i]);
                free(term_syms);
                c.token_free(tokens);
                return 1;
            }
        }
    }
    c.token_free(tokens);

    // A term that snapshot's vocabulary does not contain has no postings
    // as-of that version: the AND is empty.  Not an error — the same answer a
    // live search for an unknown term gives — and it is decided by the
    // vocabulary alone, without consulting that version's __postings__ (or
    // creating it in the live store, which the pre-fix path did).
    if (absent_from_version) {
        _ = printf("(no results)\n");
        free(term_syms);
        return 0;
    }

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
    if (min_terms != 0) {
        // Partial-match path: reject a threshold above the number of DISTINCT
        // query terms here with a specific message (the engine also rejects it
        // with -1).  Counting tokens would let a repeated term like 'a a a'
        // push the threshold past what the query can match.
        var n_distinct: usize = 0;
        var k: usize = 0;
        while (k < n_tokens) : (k += 1) {
            var dup = false;
            var j: usize = 0;
            while (j < n_distinct) : (j += 1) {
                if (syms[j] == syms[k]) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                syms[n_distinct] = syms[k];
                n_distinct += 1;
            }
        }
        if (min_terms > n_distinct) {
            _ = fprintf(stderr, "dl: --min-terms %lu exceeds the %lu distinct query terms\n", min_terms, n_distinct);
            free(term_syms);
            free(obs_ids);
            free(scores);
            return 1;
        }
        n_results = c.dl_search_ranked_top(db, syms, @intCast(n_distinct), @intCast(min_terms), @ptrCast(@alignCast(obs_ids.?)), @ptrCast(@alignCast(scores.?)), @intCast(top_n));
    } else if (version == 0) {
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

    // Parse -d <dir>
    if (argp < argv.len and eq(argv[argp], "-d")) {
        argp += 1;
        db_dir = requirePositional(argv, &argp).ptr;
    }

    const cmd = requirePositional(argv, &argp);

    // Open mode.  The CLI's read paths mint symbol ids for their string
    // arguments (cliParseValue -> intern_str), so a read command normally
    // takes the read-write handle and behaves exactly as it always has.  When
    // a WRITER IN ANOTHER PROCESS holds the db, the read-write open is
    // refused (DL_E_LOCKED); a READ-ONLY command then falls back to the
    // shared read-only open, which coexists with that writer (dl.h
    // CONCURRENCY MODEL) — the arguments still resolve, in that handle's
    // replayed vocabulary, and nothing is persisted.  Commands that WRITE
    // (load/publish/cas/txn/index, and `search`, which interns its terms and
    // may declare __postings__) have no read-only fallback: they need the
    // writer handle.
    //
    // A read command therefore asks for the FAIL-FAST acquisition
    // (DL_LOCK_FAIL_FAST): the engine's default is to QUEUE for byte 0, which
    // would make `dl query` block for the whole lifetime of another process's
    // writer instead of falling back to the read-only open.  A WRITE command
    // keeps the default and queues — it has no fallback, so waiting is the
    // only useful thing it can do.
    if (commandReadsOnly(cmd))
        _ = setenv("DL_LOCK_FAIL_FAST", "1", 1);
    var db: [*c]c.dl_db = c.dl_open(db_dir);
    if (db == null and commandReadsOnly(cmd)) {
        var oerr: c_int = 0;
        db = c.dl_open_ro(db_dir, &oerr);
    }
    if (db == null) {
        _ = fprintf(stderr, "dl: cannot open database at %s\n", db_dir);
        return 1;
    }

    if (eq(cmd, "load")) {
        var rel_name: ?[*:0]const u8 = null;

        const csv_path = requirePositional(argv, &argp);

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

        const rel_name = requirePositional(argv, &argp);

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

        const rel_name = requirePositional(argv, &argp);

        if (argp < argv.len and eq(argv[argp], "--raw")) {
            raw = true;
            argp += 1;
        }

        while (argp < argv.len and k < 8) {
            leading[k] = cliParseValue(db, argv[argp].ptr);
            k += 1;
            argp += 1;
        }

        // LIVE relation: the ids are the live interner's (version 0).
        var pctx = PrintCtx{ .db = db, .version = 0 };
        const n = c.dl_prefix(db, rel_name.ptr, &leading, k, if (raw) printTupleRaw else printTuple, @ptrCast(&pctx));
        if (n < 0) {
            _ = fprintf(stderr, "dl: prefix query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "query")) {
        var source: [*c]const u8 = requirePositional(argv, &argp).ptr;
        const goal_rel = requirePositional(argv, &argp);

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

        // Publish (runs VM automatically if fixpoint_dirty), then query.  The
        // publish is SKIPPED when the store is provably already what CURRENT
        // describes (dl_publish_if_needed — no version, nothing written); on an
        // unchanged store that is the whole cost of a read-only query, since
        // the snapshot the query reads is already the answer.
        if (c.dl_publish_if_needed(db) != 0) {
            _ = fprintf(stderr, "dl: publish failed\n");
            return 1;
        }

        // A published store makes this a VERSION read (dl_query scans the
        // snapshot whenever snap_version > 0), so render its ids in that
        // snapshot's vocabulary.
        var qctx = PrintCtx{ .db = db, .version = db.*.snap_version };
        const n = c.dl_query(db, goal_rel.ptr, printTuple, @ptrCast(&qctx));
        if (n < 0) {
            _ = fprintf(stderr, "dl: query failed\n");
            return 1;
        }
        if (n == 0)
            _ = printf("(no results)\n");
        return 0;
    }

    if (eq(cmd, "publish")) {
        // Optional flags, in any order:
        //   --keep N        retain only the N most-recent snapshots, pruning
        //                   older ones after this publish (0 = keep every one)
        //   --with-derived  also persist <snapshot>/symbols.dafsa (the same
        //                   switch as DL_PUB_DERIVED=1: a self-contained
        //                   snapshot that answers a pattern query with no
        //                   live interner, at one dafsa_save per vocabulary
        //                   change)
        var keep: c_uint = 0;
        while (argp < argv.len) {
            if (argv.len - argp == 2 and eq(argv[argp], "--keep")) {
                keep = @intCast(strtoul(argv[argp + 1].ptr, null, 10));
                argp += 2;
            } else if (eq(argv[argp], "--with-derived")) {
                _ = setenv("DL_PUB_DERIVED", "1", 1);
                argp += 1;
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

        const rel_name = requirePositional(argv, &argp);

        while (argp < argv.len and k < 8) {
            leading[k] = cliParseValue(db, argv[argp].ptr);
            k += 1;
            argp += 1;
        }

        // VERSION read whenever a snapshot is published (same rule as query).
        var bctx = PrintCtx{ .db = db, .version = db.*.snap_version };
        const n = c.dl_query_bound(db, rel_name.ptr, &leading, k, printTuple, @ptrCast(&bctx));
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

        const rel_name = requirePositional(argv, &argp);

        // Optional column argument
        if (argp < argv.len) {
            var endptr: [*c]u8 = null;
            const col_val = strtol(argv[argp].ptr, &endptr, 10);
            if (endptr[0] == 0 and col_val >= 0 and col_val <= 255) {
                col = @intCast(col_val);
                argp += 1;
            }
        }

        const pattern = requirePositional(argv, &argp);

        const dfa = c.regex_compile(pattern.ptr);
        if (dfa.*.errmsg != null) {
            _ = fprintf(stderr, "dl: bad pattern\n");
            c.regex_dfa_free(dfa);
            return 1;
        }

        // `pattern` on a published store filters the SNAPSHOT's rows by the
        // SNAPSHOT's sym ids (dl_pattern), so the names must come from that
        // snapshot's vocabulary too — the live map printed the right rows under
        // another string's name on a diverged store.
        var pctx = PrintCtx{ .db = db, .version = db.*.snap_version };
        const n = c.dl_pattern(db, rel_name.ptr, col, dfa, printTuple, @ptrCast(&pctx));
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
        const entity = requirePositional(argv, &argp);

        var r: u32 = undefined;
        if (c.dl_rev_get(db, entity.ptr, &r) != 0) {
            _ = fprintf(stderr, "dl: rev lookup failed\n");
            return 1;
        }
        _ = printf("%u\n", r);
        return 0;
    }

    if (eq(cmd, "cas")) {
        const entity = requirePositional(argv, &argp);
        const exp_s = requirePositional(argv, &argp);
        const new_s = requirePositional(argv, &argp);

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

        const start = requirePositional(argv, &argp);

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
            max_nodes = parseFlagU32(argv, &argp, 1, null) orelse {
                _ = fprintf(stderr, "dl: invalid --max-nodes value\n");
                return 1;
            };
        }
        if (rejectExtra(argv, argp)) return 1;

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

        const node = requirePositional(argv, &argp);

        if (argp < argv.len and eq(argv[argp], "--max-obs")) {
            max_obs = parseFlagU32(argv, &argp, 1, null) orelse {
                _ = fprintf(stderr, "dl: invalid --max-obs value\n");
                return 1;
            };
        }
        if (rejectExtra(argv, argp)) return 1;

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
        if (rejectExtra(argv, argp)) return 1;

        const n = c.dl_index_observations(db);
        if (n < 0) {
            _ = fprintf(stderr, "dl: index failed\n");
            return 1;
        }
        _ = printf("Indexed %ld postings\n", n);
        return 0;
    }

    if (eq(cmd, "search")) {
        const terms_str = requirePositional(argv, &argp);
        const opts = SearchParams.parse(argv, &argp) orelse return 1;
        return runSearch(db, terms_str, opts);
    }

    if (eq(cmd, "similar")) {
        // dl similar '<name>' [--k N] — typo-tolerant entity-name lookup.
        // No index and no index BUILD: the engine scans the live symbol table
        // with a length band (|len - k| .. len + k) and a capped edit distance.
        // BYTE distance (ASCII exact; a non-ASCII codepoint costs one edit per
        // UTF-8 byte).
        const needle = requirePositional(argv, &argp);

        var k: u32 = 1;
        if (argp < argv.len) {
            if (argv.len - argp == 2 and eq(argv[argp], "--k")) {
                // Strict: a non-numeric or trailing-garbage value (`--k foo`,
                // `--k 2x`) is a usage error, not a silent k=0 / k=2.
                var kv: c_ulong = undefined;
                if (!parseU32Strict(argv[argp + 1].ptr, &kv)) usage(argv[0].ptr);
                if (kv > 2) { // regexwalk.LEV_MAX_K
                    _ = fprintf(stderr, "dl: --k must be 0..2 (k>=3 refused: state explosion)\n");
                    return 1;
                }
                k = @intCast(kv);
                argp += 2;
            }
            if (argp < argv.len) usage(argv[0].ptr);
        }

        var hits: std.ArrayList(FuzzyHit) = .empty;
        defer hits.deinit(std.heap.c_allocator);
        const n = c.dl_fuzzy_lookup(db, needle.ptr, k, collectFuzzyHit, &hits);
        if (n < 0) {
            _ = fprintf(stderr, "dl: fuzzy lookup failed\n");
            return 1;
        }
        if (hits.items.len == 0) {
            _ = printf("(no results)\n");
            return 0;
        }
        std.mem.sort(FuzzyHit, hits.items, {}, fuzzyLess);
        for (hits.items) |h| {
            _ = printf("%u\t%s\n", h.dist, h.name);
        }
        return 0;
    }

    if (eq(cmd, "versions")) {
        if (rejectExtra(argv, argp)) return 1;

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
