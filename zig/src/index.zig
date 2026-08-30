//! index.zig — port of src/index.c (auxiliary postings index primitive +
//! tokenizer + the full-text search tier as its first concrete consumer).
//!
//! Design (inherited from the C oracle):
//! - The postings relation is arity-2: (term_sym, obs_id)
//! - Tokenizer: split on non-alphanumeric, lowercase
//! - SET-semantic store: DAFSA collapses duplicate keys, so tf-from-duplicates
//!   is IMPOSSIBLE.  Ranking uses distinct matched terms per obs_id.
//!
//! Strangler-hybrid ABI: no concrete C struct is dereferenced here — `dl_db`
//! is dl.zig's authoritative DlDb extern struct (comptime-gated against
//! dl_internal.h there).  dl_prefix / dl_query_bound_version / dl_lookup /
//! dl_add_fact / dl_declare_relation / dl_intern_* / dl_txn_* come from
//! dl.zig; the relation store through the same RelEnumCb as dl.zig.
//! LIVE vs VERSION read discipline mirrors vector.c: dl_prefix is LIVE-only,
//! version reads MUST use dl_query_bound_version.
//!
//! Oracle: src/index.c (never modified).  All allocation goes through raw
//! libc like the C oracle.

const std = @import("std");
const c = std.c;

const dl = @import("dl.zig");
const relation = @import("relation.zig");

// ─── Constants (mirror src/index.c) ───────────────────────────────────────

const POSTINGS_REL_NAME = "__postings__";

// ─── Callback types (mirror dl.h / index.h typedefs) ──────────────────────

/// dl_tuple_cb == rel_enum_cb (shared type identity with dl.zig's DlTupleCb).
const DlTupleCb = relation.RelEnumCb;

/// typedef int (*dl_search_cb)(uint32_t obs_id, int score, void *user);
pub const DlSearchCb = ?*const fn (obs_id: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int;

// ─── Tokenizer ─────────────────────────────────────────────────────────────

/// Check if a byte is alphanumeric (ASCII).
fn isAlnumAscii(ch: u8) bool {
    return (ch >= 'a' and ch <= 'z') or
        (ch >= 'A' and ch <= 'Z') or
        (ch >= '0' and ch <= '9');
}

/// Lowercase an ASCII character.
fn toLowerAscii(ch: u8) u8 {
    if (ch >= 'A' and ch <= 'Z')
        return ch - 'A' + 'a';
    return ch;
}

/// char **tokenize(const char *text, size_t *n_out) — split on non-alnum,
/// lowercase each token; NULL-terminated array (caller frees via token_free).
pub export fn tokenize(text: [*c]const u8, n_out: ?*usize) ?[*]?[*:0]u8 {
    if (n_out) |p| p.* = 0;
    if (text == null) return null;

    // Count tokens first to allocate the array.
    var n: usize = 0;
    var p: [*c]const u8 = text;
    while (p[0] != 0) {
        // Skip non-alphanumeric
        while (p[0] != 0 and !isAlnumAscii(p[0])) p += 1;
        if (p[0] == 0) break;
        n += 1;
        // Skip alphanumeric
        while (p[0] != 0 and isAlnumAscii(p[0])) p += 1;
    }

    if (n == 0) return null;

    // Allocate token array (+ NULL terminator) — calloc keeps the tail NULL
    // so token_free walks only the filled entries (also on the OOM path).
    const tokens: [*]?[*:0]u8 = @ptrCast(@alignCast(c.calloc(n + 1, @sizeOf(?[*:0]u8)) orelse return null));

    // Extract tokens
    p = text;
    var i: usize = 0;
    while (p[0] != 0 and i < n) {
        // Skip non-alphanumeric
        while (p[0] != 0 and !isAlnumAscii(p[0])) p += 1;
        if (p[0] == 0) break;

        // Start of token
        const start = p;
        while (p[0] != 0 and isAlnumAscii(p[0])) p += 1;

        // Allocate and copy, lowercasing
        const len: usize = @intFromPtr(p) - @intFromPtr(start);
        const tok: [*:0]u8 = @ptrCast(c.malloc(len + 1) orelse {
            token_free(tokens);
            return null;
        });
        var j: usize = 0;
        while (j < len) : (j += 1)
            tok[j] = toLowerAscii(start[j]);
        tok[len] = 0;
        tokens[i] = tok;
        i += 1;
    }

    if (n_out) |o| o.* = n;
    return tokens;
}

pub export fn token_free(tokens: ?[*]?[*:0]u8) void {
    const t = tokens orelse return;
    var i: usize = 0;
    while (t[i]) |tok| : (i += 1)
        c.free(@ptrCast(tok));
    c.free(@ptrCast(t));
}

// ─── Postings index ───────────────────────────────────────────────────────

pub export fn aux_index_ensure_postings(db: ?*dl.DlDb) c_int {
    if (db == null) return -1;
    // dl_declare_relation is idempotent - just call it
    return dl.dl_declare_relation(db, POSTINGS_REL_NAME, 2);
}

pub export fn aux_index_add_posting(db: ?*dl.DlDb, term_sym: u32, obs_id: u32) c_int {
    if (db == null) return -1;
    if (term_sym == 0 or obs_id == 0) return -1; // invalid sym_ids

    // Ensure the relation exists
    if (aux_index_ensure_postings(db) != 0)
        return -1;

    const cols = [2]u32{ term_sym, obs_id };
    return dl.dl_add_fact(db, POSTINGS_REL_NAME, &cols, 2);
}

// ─── Set operations for obs_id sets ────────────────────────────────────────

/// Simple dynamic int set (obs_id is u32).
const IntSet = struct {
    data: ?[*]u32 = null,
    count: usize = 0,
    cap: usize = 0,
};

fn intSetFree(s: *IntSet) void {
    c.free(@ptrCast(s.data));
    s.* = .{};
}

/// Linear search; small sets expected (postings per term).
fn intSetContains(s: *const IntSet, val: u32) bool {
    var i: usize = 0;
    while (i < s.count) : (i += 1)
        if (s.data.?[i] == val) return true;
    return false;
}

/// Returns 1 if inserted, 0 if already present, -1 on OOM.
fn intSetAdd(s: *IntSet, val: u32) c_int {
    if (intSetContains(s, val))
        return 0; // already present
    if (s.count >= s.cap) {
        const new_cap: usize = if (s.cap != 0) s.cap * 2 else 8;
        const new_data: [*]u32 = @ptrCast(@alignCast(c.realloc(@ptrCast(s.data), new_cap * @sizeOf(u32)) orelse return -1));
        s.data = new_data;
        s.cap = new_cap;
    }
    s.data.?[s.count] = val;
    s.count += 1;
    return 1;
}

/// Open-addressing hash set of u64 keys (packed (term_sym<<32)|obs_id), used
/// to dedup postings added within one dl_index_observations run.  The txn path
/// (dl_txn_add_fact) returns 0 regardless of whether the fact is new, and
/// buffered ops are invisible to dl_lookup until commit, so a local set is the
/// only way to count genuinely-new postings.
const U64Set = struct {
    slots: ?[*]u64 = null, // 0 == empty slot (keys are never 0)
    count: usize = 0,
    cap: usize = 0, // power of two
};

fn u64SetFree(s: *U64Set) void {
    c.free(@ptrCast(s.slots));
    s.* = .{};
}

fn u64SetHash(k_in: u64) u64 {
    var k = k_in;
    k ^= k >> 33;
    k *%= 0xff51afd7ed558ccd;
    k ^= k >> 33;
    return k;
}

fn u64SetGrow(s: *U64Set) c_int {
    const ncap: usize = if (s.cap != 0) s.cap * 2 else 64;
    const ns: [*]u64 = @ptrCast(@alignCast(c.calloc(ncap, @sizeOf(u64)) orelse return -1));
    var i: usize = 0;
    while (s.cap != 0 and i < s.cap) : (i += 1) {
        if (s.slots.?[i] != 0) {
            var idx = u64SetHash(s.slots.?[i]) & (ncap - 1);
            while (ns[idx] != 0) idx = (idx + 1) & (ncap - 1);
            ns[idx] = s.slots.?[i];
        }
    }
    c.free(@ptrCast(s.slots));
    s.slots = ns;
    s.cap = ncap;
    return 0;
}

/// Returns true if k is present in the set.  Read-only probe.
fn u64SetContains(s: *const U64Set, k: u64) bool {
    if (s.cap == 0) return false;
    var idx = u64SetHash(k) & (s.cap - 1);
    while (s.slots.?[idx] != 0) {
        if (s.slots.?[idx] == k) return true;
        idx = (idx + 1) & (s.cap - 1);
    }
    return false;
}

/// Returns 1 if inserted, 0 if already present, -1 on OOM.
fn u64SetAdd(s: *U64Set, k: u64) c_int {
    if (k == 0) return 0;
    if (s.cap == 0 or (s.count +% 1) * 4 >= s.cap * 3) {
        if (u64SetGrow(s) != 0) return -1;
    }
    var idx = u64SetHash(k) & (s.cap - 1);
    while (s.slots.?[idx] != 0) {
        if (s.slots.?[idx] == k) return 0;
        idx = (idx + 1) & (s.cap - 1);
    }
    s.slots.?[idx] = k;
    s.count += 1;
    return 1;
}

/// Callback context for collecting obs_ids
const CollectCtx = struct {
    set: *IntSet,
    error_occurred: bool,
};

/// Callback for dl_prefix to collect obs_ids
fn collectObsIdCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity; // arity should be 2
    const ctx: *CollectCtx = @ptrCast(@alignCast(user.?));
    if (intSetAdd(ctx.set, cols.?[1]) < 0) {
        ctx.error_occurred = true;
        return 1; // stop
    }
    return 0;
}

/// Collect all obs_ids for a single term via dl_prefix on the postings relation.
fn collectTermObsIds(db: ?*dl.DlDb, term_sym: u32, set: *IntSet) c_int {
    intSetFree(set);

    var ctx = CollectCtx{ .set = set, .error_occurred = false };

    const n = dl.dl_prefix(db, POSTINGS_REL_NAME, @ptrCast(&term_sym), 1, collectObsIdCb, &ctx);
    if (n < 0 or ctx.error_occurred) return -1;

    return 0;
}

/// Collect all obs_ids for a single term from a specific snapshot version.
/// Uses dl_query_bound_version to query the __postings__ relation as-of
/// `version`.
fn collectTermObsIdsVersion(db: ?*dl.DlDb, version: u32, term_sym: u32, set: *IntSet) c_int {
    intSetFree(set);

    var ctx = CollectCtx{ .set = set, .error_occurred = false };

    const n = dl.dl_query_bound_version(db, version, POSTINGS_REL_NAME, @ptrCast(&term_sym), 1, collectObsIdCb, &ctx);
    // dl_query_bound_version returns -1 if the relation is ABSENT from that
    // version OR version==0/nonexistent.  This is the error contract we must
    // propagate (not treat as 0 results).
    if (n < 0 or ctx.error_occurred) return -1;

    return 0;
}

/// Intersect multiple obs_id sets using smallest-first strategy.
/// Returns a new set containing the intersection, or NULL on OOM.
fn intSetIntersect(sets: [*]*IntSet, n_sets: usize) ?*IntSet {
    if (n_sets == 0) return null;

    var result: *IntSet = undefined;
    if (n_sets == 1) {
        result = @ptrCast(@alignCast(c.malloc(@sizeOf(IntSet)) orelse return null));
        result.* = sets[0].*;
        sets[0].data = null; // steal the data
        sets[0].count = 0;
        sets[0].cap = 0;
        return result;
    }

    // Find the smallest set to iterate over
    var smallest_idx: usize = 0;
    var smallest_count = sets[0].count;
    var i: usize = 1;
    while (i < n_sets) : (i += 1) {
        if (sets[i].count < smallest_count) {
            smallest_count = sets[i].count;
            smallest_idx = i;
        }
    }

    // For each obs_id in the smallest set, check if it's in all other sets
    result = @ptrCast(@alignCast(c.malloc(@sizeOf(IntSet)) orelse return null));
    result.* = .{};

    var j: usize = 0;
    while (j < sets[smallest_idx].count) : (j += 1) {
        const obs_id = sets[smallest_idx].data.?[j];
        var in_all = true;
        i = 0;
        while (i < n_sets) : (i += 1) {
            if (i == smallest_idx) continue;
            if (!intSetContains(sets[i], obs_id)) {
                in_all = false;
                break;
            }
        }
        if (in_all) {
            if (intSetAdd(result, obs_id) < 0) {
                intSetFree(result);
                c.free(@ptrCast(result));
                return null;
            }
        }
    }

    return result;
}

/// Sort results by score descending.
const ScoredResult = struct {
    obs_id: u32,
    score: c_int,
};

/// Stable sort keeps the input (intersection) order for equal scores —
/// what glibc qsort's mergesort does for the all-equal AND-search results.
fn scoredResultLt(_: void, a: ScoredResult, b: ScoredResult) bool {
    return a.score > b.score; // descending
}

/// Callback context for dl_search_top
const TopCtx = struct {
    obs_ids: [*]u32,
    scores: [*]c_int,
    cap: c_int,
    count: c_int,
};

/// Callback for dl_search to collect top results
fn topSearchCb(obs_id: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *TopCtx = @ptrCast(@alignCast(user.?));
    if (ctx.count >= ctx.cap)
        return 1; // stop
    ctx.obs_ids[@intCast(ctx.count)] = obs_id;
    ctx.scores[@intCast(ctx.count)] = score;
    ctx.count += 1;
    return 0;
}

/// Shared AND-intersect/rank/emit body of dl_search / dl_search_version —
/// the two C entry points are byte-identical except for the collect call,
/// so the shared impl branches ONLY on the read primitive (the same
/// read-discipline structure as vector.c's vector_search_impl).
fn searchImpl(
    db: ?*dl.DlDb,
    version: u32,
    use_version: bool,
    terms: ?[*]const u32,
    n_terms: c_int,
    cb: DlSearchCb,
    user: ?*anyopaque,
) c_long {
    // Collect obs_id sets for each term
    const sets_raw = c.calloc(@intCast(n_terms), @sizeOf(?*IntSet)) orelse return -1;
    const sets: [*]?*IntSet = @ptrCast(@alignCast(sets_raw));

    var i: usize = 0;
    var collect_err = false;
    var all_empty = true;
    while (i < @as(usize, @intCast(n_terms))) : (i += 1) {
        sets[i] = @ptrCast(@alignCast(c.malloc(@sizeOf(IntSet))));
        if (sets[i] == null) {
            var j: usize = 0;
            while (j < i) : (j += 1) {
                intSetFree(sets[j].?);
                c.free(sets[j]);
            }
            c.free(sets_raw);
            return -1;
        }
        sets[i].?.* = .{};

        // collect_term_obs_ids[_version] returns 0 for an ABSENT term (empty
        // set, valid) and -1 for a genuine error (missing postings relation /
        // OOM / relation absent from that version / version==0-nonexistent).
        // A genuine error must NOT be treated as "0 results".
        const rc = if (use_version)
            collectTermObsIdsVersion(db, version, terms.?[i], sets[i].?)
        else
            collectTermObsIds(db, terms.?[i], sets[i].?);
        if (rc < 0) {
            collect_err = true;
            break;
        }
        if (sets[i].?.count > 0)
            all_empty = false;
    }

    if (collect_err) {
        var j: usize = 0;
        while (j <= i) : (j += 1) {
            intSetFree(sets[j].?);
            c.free(sets[j]);
        }
        c.free(sets_raw);
        return -1;
    }

    // If any term has no postings, the AND is empty
    if (all_empty) {
        i = 0;
        while (i < @as(usize, @intCast(n_terms))) : (i += 1) {
            intSetFree(sets[i].?);
            c.free(sets[i]);
        }
        c.free(sets_raw);
        return 0;
    }

    // Intersect all sets — alias the same array as non-optional pointers
    // (every entry is non-null here).
    const sets_arr: [*]*IntSet = @ptrCast(sets);
    const intersection = intSetIntersect(sets_arr, @intCast(n_terms));

    // Free the individual sets
    i = 0;
    while (i < @as(usize, @intCast(n_terms))) : (i += 1) {
        intSetFree(sets[i].?);
        c.free(sets[i]);
    }
    c.free(sets_raw);

    if (intersection == null)
        return -1;

    // Score each result: for AND search, all results match all terms.
    // Since the relation store is SET-semantic (DAFSA collapses duplicates),
    // we cannot compute tf from duplicate keys.  We rank by the number of
    // distinct matched terms per obs_id.  For AND search, this is always
    // n_terms.

    // Build scored results
    if (intersection.?.count == 0) {
        intSetFree(intersection.?);
        c.free(@ptrCast(intersection));
        return 0; // disjoint terms -> no matches, not an error
    }

    var k: usize = 0;
    const results: [*]ScoredResult = @ptrCast(@alignCast(c.malloc(intersection.?.count * @sizeOf(ScoredResult)) orelse {
        intSetFree(intersection.?);
        c.free(@ptrCast(intersection));
        return -1;
    }));

    k = 0;
    while (k < intersection.?.count) : (k += 1) {
        results[k].obs_id = intersection.?.data.?[k];
        results[k].score = n_terms; // All matched all terms
    }

    // Sort by score descending
    std.mem.sort(ScoredResult, results[0..intersection.?.count], {}, scoredResultLt);

    // Emit via callback.  Count the emission BEFORE invoking the cb so a
    // callback that stops early still counts the result it consumed (mirrors
    // dl_prefix's count-before-callback semantics).
    var emitted: c_long = 0;
    k = 0;
    while (k < intersection.?.count) : (k += 1) {
        emitted += 1;
        if (cb.?(results[k].obs_id, results[k].score, user) != 0)
            break;
    }

    c.free(@ptrCast(results));
    intSetFree(intersection.?);
    c.free(@ptrCast(intersection));

    return emitted;
}

pub export fn dl_search(db: ?*dl.DlDb, terms: ?[*]const u32, n_terms: c_int, cb: DlSearchCb, user: ?*anyopaque) c_long {
    if (db == null or terms == null or n_terms <= 0 or cb == null)
        return -1;

    return searchImpl(db, 0, false, terms, n_terms, cb, user);
}

pub export fn dl_search_top(db: ?*dl.DlDb, terms: ?[*]const u32, n_terms: c_int, obs_ids_out: ?[*]u32, scores_out: ?[*]c_int, limit: c_int) c_int {
    if (db == null or terms == null or n_terms <= 0 or obs_ids_out == null or scores_out == null or limit <= 0)
        return -1;

    var ctx = TopCtx{ .obs_ids = obs_ids_out.?, .scores = scores_out.?, .cap = limit, .count = 0 };

    const n = dl_search(db, terms, n_terms, topSearchCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

/// Version-aware search: same AND-intersect + rank logic as dl_search,
/// but collects each term's obs_ids as-of a published snapshot `version` via
/// dl_query_bound_version on the "__postings__" relation.
/// Since the sym_id space is append-only/never-reused, pre-interned term
/// sym_ids are valid across versions — no re-interning needed.
pub export fn dl_search_version(db: ?*dl.DlDb, version: u32, terms: ?[*]const u32, n_terms: c_int, cb: DlSearchCb, user: ?*anyopaque) c_long {
    if (db == null or version == 0 or terms == null or n_terms <= 0 or cb == null)
        return -1;

    return searchImpl(db, version, true, terms, n_terms, cb, user);
}

/// Version-aware convenience: dl_search_version with a --top N limit.
/// Same contract as dl_search_top but for a specific snapshot version.
pub export fn dl_search_top_version(db: ?*dl.DlDb, version: u32, terms: ?[*]const u32, n_terms: c_int, obs_ids_out: ?[*]u32, scores_out: ?[*]c_int, limit: c_int) c_int {
    if (db == null or version == 0 or terms == null or n_terms <= 0 or obs_ids_out == null or scores_out == null or limit <= 0)
        return -1;

    var ctx = TopCtx{ .obs_ids = obs_ids_out.?, .scores = scores_out.?, .cap = limit, .count = 0 };

    const n = dl_search_version(db, version, terms, n_terms, topSearchCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// ─── Observation indexing ────────────────────────────────────────────────────

/// Callback context for collecting observation tuples
const IndexObsCtx = struct {
    db: ?*dl.DlDb,
    postings_added: c_long,
    error_occurred: bool,
    seen: U64Set = .{}, // (term_sym<<32)|obs_id pairs added this run, for dedup
    indexed: *const U64Set, // distinct content syms already in __postings__
};

/// Callback for dl_prefix to process each observation tuple.  Runs inside a
/// dl_txn, so postings go through dl_txn_add_fact (buffered, committed with
/// one WAL fsync + one interner save) rather than per-posting dl_add_fact.
fn indexObsCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *IndexObsCtx = @ptrCast(@alignCast(user.?));

    if (ctx.error_occurred) return 1; // stop on error

    // The observation relation must be arity-2 (entity, content).  A fixed
    // relation of any other arity, or a variadic variant of a different
    // arity, would make cols[1] (the content column) read out of bounds.
    if (arity != 2) {
        ctx.error_occurred = true;
        return 1;
    }

    // cols[0] = entity_sym, cols[1] = content_sym
    // Incremental rebuild: skip contents whose sym is ALREADY in __postings__
    // (threaded in via ctx.indexed, the short-circuit's distinct set).  Only
    // genuinely-new contents are tokenized + indexed.
    if (u64SetContains(ctx.indexed, cols.?[1]))
        return 0;

    const content = dl.dl_intern_str_of(ctx.db, cols.?[1]) orelse {
        ctx.error_occurred = true;
        return 1;
    };

    // Tokenize the content
    const tokens = tokenize(content, null) orelse {
        // No tokens to index for this observation
        return 0;
    };

    // For each token, intern and buffer a posting (dedup in-run via the hash
    // set; dedup against the persisted base via dl_lookup).  A posting counts
    // as "added" only when it is genuinely new — dl_txn_add_fact gives no
    // added-vs-dup signal, so we count it ourselves here.
    var i: usize = 0;
    while (tokens[i]) |tok| : (i += 1) {
        const term_sym = dl.dl_intern_str(ctx.db, tok);
        if (term_sym == 0) {
            ctx.error_occurred = true;
            token_free(tokens);
            return 1;
        }
        const pc = [2]u32{ term_sym, cols.?[1] };
        const is_new = u64SetAdd(&ctx.seen, (@as(u64, term_sym) << 32) | cols.?[1]);
        if (is_new < 0) {
            ctx.error_occurred = true;
            token_free(tokens);
            return 1;
        }
        if (is_new == 1 and dl.dl_lookup(ctx.db, POSTINGS_REL_NAME, &pc, 2) == 0) {
            if (dl.dl_txn_add_fact(ctx.db, POSTINGS_REL_NAME, &pc, 2) != 0) {
                ctx.error_occurred = true;
                token_free(tokens);
                return 1;
            }
            ctx.postings_added += 1;
        }
    }

    token_free(tokens);
    return 0;
}

/// Callback to count DISTINCT observation content symbols (column 1) that are
/// tokenizable (contain at least one ASCII alnum byte).  Used by the
/// short-circuit completeness check: many observations share a content
/// symbol, so this counts deduped contents, matching what the rebuild would
/// index.
const TokenObsCtx = struct {
    db: ?*dl.DlDb,
    seen: U64Set = .{},
    count: c_long = 0,
    error_occurred: bool = false,
};

fn tokenObsCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *TokenObsCtx = @ptrCast(@alignCast(user.?));
    const r = u64SetAdd(&ctx.seen, cols.?[1]);
    if (r < 0) {
        ctx.error_occurred = true;
        return 1; // stop
    }
    if (r == 1) {
        var has_alnum = false;
        if (dl.dl_intern_str_of(ctx.db, cols.?[1])) |content| {
            var i: usize = 0;
            while (content[i] != 0 and !has_alnum) : (i += 1)
                has_alnum = isAlnumAscii(content[i]);
        }
        if (has_alnum) ctx.count += 1;
    }
    return 0;
}

/// Callback to collect DISTINCT obs_id values (postings column 1) into a
/// u64_set.  Used by the short-circuit completeness check.
const DistinctObsCtx = struct {
    seen: U64Set = .{},
    distinct: c_long = 0,
    error_occurred: bool = false,
};

fn distinctObsCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *DistinctObsCtx = @ptrCast(@alignCast(user.?));
    const r = u64SetAdd(&ctx.seen, cols.?[1]);
    if (r < 0) {
        ctx.error_occurred = true;
        return 1; // stop
    }
    if (r == 1) ctx.distinct += 1;
    return 0;
}

pub export fn dl_index_observations(db: ?*dl.DlDb) c_long {
    if (db == null) return -1;

    // Ensure postings relation exists
    if (aux_index_ensure_postings(db) != 0)
        return -1;

    // Short-circuit: if the index already covers every observation content, it
    // is complete — return 0 without re-tokenizing or re-writing anything.
    // "Covered" is checked by comparing the number of DISTINCT obs_id already
    // in __postings__ with the number of DISTINCT TOKENIZABLE observation
    // contents (contents deduped by symbol, counted only if they contain at
    // least one ASCII alnum byte — exactly when tokenize() would yield tokens).
    // Both are cheap passes (a set-dedup + alnum scan, no tokenization, no
    // writes), so a second call with no new observations returns near-
    // instantly.  dc.seen (the distinct contents already in __postings__) is
    // kept ALIVE past this block and threaded into the rebuild so it skips
    // re-tokenizing already-indexed contents (Fix A incremental rebuild).
    var dc = DistinctObsCtx{};
    {
        var oc = TokenObsCtx{ .db = db };
        if (dl.dl_prefix(db, "observation", null, 0, tokenObsCb, &oc) < 0)
            return 0; // no observation relation -> nothing to index
        if (oc.error_occurred) {
            u64SetFree(&oc.seen);
            return -1;
        }

        if (dl.dl_prefix(db, POSTINGS_REL_NAME, null, 0, distinctObsCb, &dc) < 0) {
            u64SetFree(&oc.seen);
            u64SetFree(&dc.seen);
            return -1;
        }
        if (dc.error_occurred) {
            u64SetFree(&oc.seen);
            u64SetFree(&dc.seen);
            return -1;
        }
        if (dc.distinct >= oc.count) {
            u64SetFree(&oc.seen);
            u64SetFree(&dc.seen);
            return 0; // index already complete
        }
        u64SetFree(&oc.seen);
        // dc.seen survives: it now drives the incremental rebuild below.
    }

    // Materialize a MUTABLE forward DAFSA so interning the rebuild's tokens
    // grows it in place (instead of the ~111µs/sym linear rev[] scan).  On
    // OOM it degrades safely to the rev[] scan — still correct, just slower.
    _ = dl.dl_intern_fwd_mutable(db);

    // Buffer the whole enumeration in one transaction: every posting commit
    // lands with ONE WAL fsync and ONE interner save (dl_txn_commit does the
    // M7 interner/term-store ordering internally) instead of one fsync + one
    // intern_save per posting.  The txn buffer grows dynamically, so there is
    // no capacity cap to chunk against.
    if (dl.dl_txn_begin(db) != 0) {
        u64SetFree(&dc.seen);
        return -1;
    }

    var ctx = IndexObsCtx{ .db = db, .postings_added = 0, .error_occurred = false, .indexed = &dc.seen };
    const n = dl.dl_prefix(db, "observation", null, 0, indexObsCb, &ctx);
    if (n < 0 or ctx.error_occurred) {
        _ = dl.dl_txn_rollback(db);
        u64SetFree(&ctx.seen);
        u64SetFree(&dc.seen);
        return -1;
    }

    u64SetFree(&ctx.seen);

    if (dl.dl_txn_commit(db) != 0) {
        u64SetFree(&dc.seen);
        return -1;
    }

    u64SetFree(&dc.seen);
    return ctx.postings_added;
}

// ─── Tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

fn collectVecCb(obs_id: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const out: *std.ArrayList(u64) = @ptrCast(@alignCast(user.?));
    const packed_hit = (@as(u64, obs_id) << 32) | @as(u64, @as(u32, @bitCast(score)));
    out.append(testing.allocator, packed_hit) catch return 1;
    return 0;
}

test "tokenize: split + lowercase roundtrip, NULL/empty contract" {
    var n: usize = 0;

    const toks = tokenize("Hello, World! 42_x  don't", &n) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(usize, 6), n);
    try testing.expectEqualStrings("hello", std.mem.span(toks[0].?));
    try testing.expectEqualStrings("world", std.mem.span(toks[1].?));
    try testing.expectEqualStrings("42", std.mem.span(toks[2].?));
    try testing.expectEqualStrings("x", std.mem.span(toks[3].?));
    try testing.expectEqualStrings("don", std.mem.span(toks[4].?));
    try testing.expectEqualStrings("t", std.mem.span(toks[5].?));
    try testing.expect(toks[6] == null); // NULL-terminated
    token_free(toks);

    // empty / separator-only / NULL text -> NULL + n_out = 0
    try testing.expectEqual(@as(?[*]?[*:0]u8, null), tokenize("", &n));
    try testing.expectEqual(@as(usize, 0), n);
    try testing.expectEqual(@as(?[*]?[*:0]u8, null), tokenize(" !!! ... ", &n));
    try testing.expectEqual(@as(usize, 0), n);
    try testing.expectEqual(@as(?[*]?[*:0]u8, null), tokenize(null, &n));
    try testing.expectEqual(@as(usize, 0), n);

    token_free(null); // NULL-safe
}

/// rm -rf `path` (bounded: test paths only).  dl_open(NULL) is an error in
/// the oracle too (dl.c dl_open_common rejects it), so tests use real dirs —
/// the same fresh_db pattern as the C suites / compiler.zig tests.
extern "c" fn system(cmd: [*:0]const u8) c_int;

fn testRmRf(path: [*:0]const u8) void {
    var buf: [256]u8 = undefined;
    const cmd = std.fmt.bufPrintZ(&buf, "rm -rf {s}", .{std.mem.span(path)}) catch return;
    _ = system(cmd.ptr);
}

fn testOpenDb(path: [*:0]const u8) *dl.DlDb {
    testRmRf(path);
    return dl.dl_open(path) orelse @panic("dl_open failed");
}

fn testCloseDb(db: *dl.DlDb, path: [*:0]const u8) void {
    dl.dl_close(db);
    testRmRf(path);
}

test "dl_search: AND-intersect + score = distinct matched terms" {
    const path = "/tmp/datalog_zig_u14_index_search";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), aux_index_ensure_postings(db));
    // Idempotent re-declare.
    try testing.expectEqual(@as(c_int, 0), aux_index_ensure_postings(db));

    // term 1 -> obs {10, 11}; term 2 -> obs {11}; term 3 -> obs {99}.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, POSTINGS_REL_NAME, &[_]u32{ 1, 10 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, POSTINGS_REL_NAME, &[_]u32{ 1, 11 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, POSTINGS_REL_NAME, &[_]u32{ 2, 11 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, POSTINGS_REL_NAME, &[_]u32{ 3, 99 }, 2));

    // aux_index_add_posting: adds (4, 77) — returns dl_add_fact's 1=added;
    // rejects 0 syms / NULL db.
    try testing.expectEqual(@as(c_int, 1), aux_index_add_posting(db, 4, 77));
    try testing.expectEqual(@as(c_int, -1), aux_index_add_posting(db, 0, 77));
    try testing.expectEqual(@as(c_int, -1), aux_index_add_posting(null, 4, 77));

    // AND(1,2) -> obs 11, score 2.
    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    const terms = [2]u32{ 1, 2 };
    const emitted = dl_search(db, &terms, 2, collectVecCb, &got);
    try testing.expectEqual(@as(c_long, 1), emitted);
    try testing.expectEqual(@as(usize, 1), got.items.len);
    try testing.expectEqual(@as(u64, 11), got.items[0] >> 32);
    try testing.expectEqual(@as(u32, 2), @as(u32, @truncate(got.items[0])));

    // AND(1,4) -> no match (4 only has obs 77) -> 0 results, not an error.
    var got2: std.ArrayList(u64) = .empty;
    defer got2.deinit(testing.allocator);
    const terms2 = [2]u32{ 1, 4 };
    try testing.expectEqual(@as(c_long, 0), dl_search(db, &terms2, 2, collectVecCb, &got2));

    // AND with a wholly absent term -> 0.
    var got3: std.ArrayList(u64) = .empty;
    defer got3.deinit(testing.allocator);
    const terms3 = [2]u32{ 1, 12345 };
    try testing.expectEqual(@as(c_long, 0), dl_search(db, &terms3, 2, collectVecCb, &got3));

    // Error contract: NULL terms / n_terms == 0 / NULL db.
    try testing.expectEqual(@as(c_long, -1), dl_search(db, null, 2, collectVecCb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_search(db, &terms, 0, collectVecCb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_search(null, &terms, 2, collectVecCb, &got));

    // dl_search_top: collects up to limit, returns the count.
    var ids: [8]u32 = undefined;
    var scores: [8]c_int = undefined;
    const ntop = dl_search_top(db, &terms, 2, &ids, &scores, 8);
    try testing.expectEqual(@as(c_int, 1), ntop);
    try testing.expectEqual(@as(u32, 11), ids[0]);
    try testing.expectEqual(@as(c_int, 2), scores[0]);
    try testing.expectEqual(@as(c_int, -1), dl_search_top(db, &terms, 2, &ids, &scores, 0));
}

test "dl_index_observations: tokenize + intern + txn postings, short-circuit on second call" {
    const path = "/tmp/datalog_zig_u14_index_obs";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    // observation(entity, content) facts: two distinct contents + one shared.
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "observation", 2));
    try testing.expectEqual(@as(u32, 1), dl.dl_intern_str(db, "alice"));
    try testing.expectEqual(@as(u32, 2), dl.dl_intern_str(db, "red apples taste great"));
    try testing.expectEqual(@as(u32, 3), dl.dl_intern_str(db, "green apples"));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "observation", &[_]u32{ 1, 2 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "observation", &[_]u32{ 1, 3 }, 2));

    // "red apples taste great" -> 4 postings; "green apples" -> 2.
    try testing.expectEqual(@as(c_long, 6), dl_index_observations(db));

    // Second call: index already complete -> 0 (short-circuit).
    try testing.expectEqual(@as(c_long, 0), dl_index_observations(db));

    // Postings searchable: AND(apples, great) -> content sym 2.
    const apples = dl.dl_intern_str_find(db, "apples");
    const great = dl.dl_intern_str_find(db, "great");
    try testing.expect(apples != 0 and great != 0);
    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    const terms = [2]u32{ apples, great };
    try testing.expectEqual(@as(c_long, 1), dl_search(db, &terms, 2, collectVecCb, &got));
    try testing.expectEqual(@as(u64, 2), got.items[0] >> 32);

    // No observation relation at all -> 0 (nothing to index).
    const path2 = "/tmp/datalog_zig_u14_index_obs2";
    const db2 = testOpenDb(path2);
    defer testCloseDb(db2, path2);
    try testing.expectEqual(@as(c_long, 0), dl_index_observations(db2));
}
