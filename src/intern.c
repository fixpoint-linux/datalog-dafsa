/*
 * intern.c — Term interner implementation
 *
 * Forward: DAFSA keyed  utf8_bytes \0 sym_id_u32BE  (W\0 semantics).
 *   dafsa_prefix_enum("hello") → walks "hello", requires \0, then DFS
 *   the 4-byte payload.  Callback extracts sym_id.
 *
 * Reverse: growable array of char* pointers, indexed by sym_id-1.
 */

#include "intern.h"
#include "dafsa.h"
#include "dafsa_internal.h"   /* MAX_WORD_LEN guard */
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define INTERN_REV_INIT_CAP 256

struct interner {
    dafsa   *fwd;          /* forward DAFSA: str\0\sym_id (NULL until lazily loaded) */
    char   **rev;          /* reverse array: rev[sym_id-1] -> string */
    uint32_t rev_cap;      /* capacity of rev */
    uint32_t next_id;      /* next sym_id to allocate (1-based) */
    int      dirty;        /* M7: 1 if new syms added since last save */
    char    *fwd_path;     /* on-disk path of the forward DAFSA (lazy-load) */
    int      fwd_ro;       /* 1 if fwd is a read-only handle that must NOT be mutated */
    uint32_t fwd_gen;      /* next_id the in-memory fwd was built from (stale if != next_id) */
};

/* ─── Callback for dafsa_prefix_enum: capture the first sym_id ──────── */

struct capture_ctx {
    uint32_t id;
    int      found;
};

static int capture_cb(const unsigned char *payload, size_t payload_len,
                      void *user)
{
    struct capture_ctx *ctx = (struct capture_ctx *)user;
    (void)payload_len;
    /* payload is the 4-byte big-endian sym_id */
    if (payload_len >= 4) {
        ctx->id = ((uint32_t)payload[0] << 24) |
                  ((uint32_t)payload[1] << 16) |
                  ((uint32_t)payload[2] << 8)  |
                  ((uint32_t)payload[3]);
        ctx->found = 1;
    }
    return 1; /* stop after first match */
}

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

interner *intern_create(void)
{
    interner *ir = calloc(1, sizeof(*ir));
    if (!ir) return NULL;

    ir->fwd = dafsa_create();
    if (!ir->fwd) { free(ir); return NULL; }

    ir->rev_cap = INTERN_REV_INIT_CAP;
    ir->rev = calloc(ir->rev_cap, sizeof(char *));
    if (!ir->rev) { dafsa_free(ir->fwd); free(ir); return NULL; }

    ir->next_id = 1;  /* 1-based; 0 = not-found */
    return ir;
}

void intern_free(interner *ir)
{
    if (!ir) return;
    dafsa_free(ir->fwd);
    free(ir->fwd_path);
    if (ir->rev) {
        uint32_t i;
        for (i = 0; i < ir->next_id - 1 && i < ir->rev_cap; i++)
            free(ir->rev[i]);
        free(ir->rev);
    }
    free(ir);
}

/* ─── Core ops ────────────────────────────────────────────────────────── */

/* Materialize ir->fwd on first need (lazy forward-DAFSA load).
 *
 * want_mutable=0: a search-only handle suffices — load via the fast read-only
 *   loader (skips inode/register rebuild) if symbols.dafsa exists, else an
 *   empty DAFSA.  The resulting handle must NOT be mutated (fwd_ro=1).
 * want_mutable=1: rebuild a fully mutable DAFSA from the reverse array so
 *   dafsa_add_n can grow it.  If fwd is currently read-only it is replaced.
 *
 * Returns 0 on success, -1 on OOM (fwd left NULL / prior handle freed only
 * after the replacement is fully built). */
static int intern_ensure_fwd(interner *ir, int want_mutable)
{
    dafsa *d;
    uint32_t i;

    if (!ir) return -1;
    if (ir->fwd && (!want_mutable || !ir->fwd_ro) &&
        ir->fwd_gen == ir->next_id)
        return 0;  /* already satisfies the request */

    if (ir->fwd) {
        /* stale (gen mismatch) or wrong-mode handle; replace with a rebuild */
        dafsa_free(ir->fwd);
        ir->fwd = NULL;
    }

    /* Rebuild the forward DAFSA from the reverse array (the source of truth).
     * The on-disk symbols.dafsa is a pure derived cache and is never trusted:
     * after P1 intern_save writes only symbols.array, so the file may be stale
     * or absent.  For each rev[i] (sym_id i+1) build the same key intern_str
     * constructs — str \0 id_u32BE. */
    d = dafsa_create();
    if (!d) return -1;
    for (i = 0; i < ir->next_id - 1 && i < ir->rev_cap; i++) {
        const char *s = ir->rev[i];
        size_t slen, key_len;
        unsigned char *key;
        uint32_t id;
        if (!s) continue;
        slen = strlen(s);
        key_len = slen + 1 + 4;
        if (key_len > MAX_WORD_LEN) continue;   /* already-validated at add time */
        key = malloc(key_len);
        if (!key) { dafsa_free(d); return -1; }
        id = i + 1;
        memcpy(key, s, slen);
        key[slen] = 0x00;
        key[slen + 1] = (unsigned char)((id >> 24) & 0xFF);
        key[slen + 2] = (unsigned char)((id >> 16) & 0xFF);
        key[slen + 3] = (unsigned char)((id >> 8)  & 0xFF);
        key[slen + 4] = (unsigned char)(id & 0xFF);
        if (dafsa_add_n(d, key, key_len) < 0) {
            free(key);
            dafsa_free(d);
            return -1;
        }
        free(key);
    }
    ir->fwd = d;
    ir->fwd_ro = want_mutable ? 0 : 1;
    ir->fwd_gen = ir->next_id;
    return 0;
}

/* NON-MUTATING lookup: walk the forward DAFSA if it is materialized and
 * current (fwd_gen == next_id), else linear-scan the reverse array.  Returns
 * 0 if absent (or on NULL input).  Never allocates / never marks dirty. */
static uint32_t intern_lookup(interner *ir, const char *str)
{
    if (!ir || !str) return 0;

    if (ir->fwd && ir->fwd_gen == ir->next_id) {
        size_t slen = strlen(str);
        struct capture_ctx ctx = {0, 0};
        dafsa_prefix_enum(ir->fwd, (const unsigned char *)str, slen,
                          capture_cb, &ctx);
        return ctx.found ? ctx.id : 0;
    }

    {
        uint32_t i;
        for (i = 0; i < ir->next_id - 1 && i < ir->rev_cap; i++)
            if (ir->rev[i] && strcmp(ir->rev[i], str) == 0)
                return i + 1;
    }
    return 0;
}

uint32_t intern_str_find(interner *ir, const char *str)
{
    return ir ? intern_lookup(ir, str) : 0;
}

uint32_t intern_str(interner *ir, const char *str)
{
    size_t slen;
    uint32_t id;

    if (!ir || !str) return 0;

    /* Lookup half never mutates / never rebuilds: uses the fwd walk when the
     * cache is current, else the rev[] scan. */
    id = intern_lookup(ir, str);
    if (id) return id;

    slen = strlen(str);

    /* Allocate new sym_id: append to rev[] only.  The forward DAFSA is a pure
     * derived cache — it is NOT grown here (that would make intern_save's full
     * rebuild pointless); the just-added symbol lives in rev[] and the cache
     * is invalidated below so lookups fall back to the rev[] scan. */
    ir->dirty = 1;
    {
        size_t key_len = slen + 1 + 4;

        if (key_len > MAX_WORD_LEN) return 0;  /* too long */

        id = ir->next_id++;

        /* 3. Grow reverse array if needed */
        if (id > ir->rev_cap) {
            uint32_t new_cap = ir->rev_cap * 2;
            if (new_cap < id) new_cap = id * 2;
            char **new_rev = realloc(ir->rev, new_cap * sizeof(char *));
            if (!new_rev) return 0;
            memset(new_rev + ir->rev_cap, 0,
                   (new_cap - ir->rev_cap) * sizeof(char *));
            ir->rev = new_rev;
            ir->rev_cap = new_cap;
        }

        /* 4. Store string in reverse array */
        ir->rev[id - 1] = strdup(str);
        if (!ir->rev[id - 1]) return 0;

        /* Grow the forward-DAFSA cache instead of freeing it when the handle is
         * MUTABLE (fwd_ro==0) and CURRENT (fwd_gen == id covers ids 1..id-1,
         * i.e. every prior sym).  Adding the new symbol in place keeps the
         * cache usable across a whole rebuild, avoiding the linear rev[] scan.
         * The key is laid out exactly as intern_ensure_fwd builds it: str \0
         * id_u32BE.  On any add failure (OOM), fall back to the existing safe
         * degradation — drop the DAFSA so lookups use the always-correct rev[]
         * scan (never keep a partially-grown cache claiming to be current). */
        if (ir->fwd && !ir->fwd_ro && ir->fwd_gen == id) {
            unsigned char *k = malloc(key_len);
            if (!k) {
                dafsa_free(ir->fwd);
                ir->fwd = NULL;
            } else {
                memcpy(k, str, slen);
                k[slen] = 0x00;
                k[slen + 1] = (unsigned char)((id >> 24) & 0xFF);
                k[slen + 2] = (unsigned char)((id >> 16) & 0xFF);
                k[slen + 3] = (unsigned char)((id >> 8)  & 0xFF);
                k[slen + 4] = (unsigned char)(id & 0xFF);
                if (dafsa_add_n(ir->fwd, k, key_len) < 0) {
                    free(k);
                    dafsa_free(ir->fwd);
                    ir->fwd = NULL;
                } else {
                    free(k);
                    ir->fwd_gen = ir->next_id;  /* stay current */
                }
            }
        } else {
            /* Invalidate the stale/read-only forward-DAFSA cache: fwd_gen !=
             * next_id now, and the cached handle no longer covers the new
             * symbol. */
            if (ir->fwd) {
                dafsa_free(ir->fwd);
                ir->fwd = NULL;
            }
        }

        return id;
    }
}

const char *intern_str_of(interner *ir, uint32_t sym_id)
{
    if (!ir || sym_id == 0 || sym_id >= ir->next_id) return NULL;
    if (sym_id > ir->rev_cap) return NULL;
    return ir->rev[sym_id - 1];
}

/* ─── Accessors ───────────────────────────────────────────────────────── */

const dafsa *intern_fwd(interner *ir)
{
    if (!ir) return NULL;
    if (intern_ensure_fwd(ir, 0) != 0) return NULL;   /* OOM */
    return ir->fwd;
}

/* Return a MUTABLE forward DAFSA (fwd_ro==0), building it if absent/stale so
 * intern_str can grow it in place instead of freeing it on each new symbol.
 * Used by the index rebuild to keep interning off the linear rev[] scan. */
const dafsa *intern_fwd_mutable(interner *ir)
{
    if (!ir) return NULL;
    if (intern_ensure_fwd(ir, 1) != 0) return NULL;   /* OOM */
    return ir->fwd;
}

/* ─── Persistence ─────────────────────────────────────────────────────── */

int intern_save(interner *ir, const char *fwd_path, const char *rev_path)
{
    FILE *f;
    uint32_t i;
    char tmp_path[8192];

    if (!ir || !fwd_path || !rev_path) return -1;

    /* The forward DAFSA is a pure DERIVED cache of the reverse array; it is
     * NOT persisted here.  Only symbols.array is the source of truth and the
     * only thing WAL recovery needs, so a save is just the atomic array write
     * below.  symbols.dafsa is left stale/absent on disk and rebuilt lazily
     * from rev[] by intern_ensure_fwd.  fwd_path is kept in the signature for
     * caller compatibility but is intentionally ignored. */

    /* Save reverse array atomically: streaming tmp+fsync+rename+dir-fsync */
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", rev_path);
    f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (i = 1; i < ir->next_id; i++) {
        const char *s = ir->rev[i - 1];
        if (s) {
            /* Escape '\\' and '\n' so embedded newlines round-trip as one
             * physical line (see intern_load).  '\n' becomes the two chars
             * '\\' 'n' (0x5C 0x6E). */
            for (; *s; s++) {
                int c = (unsigned char)*s;
                if (c == '\\' || c == '\n') {
                    if (fputc('\\', f) == EOF) {
                        fclose(f);
                        unlink(tmp_path);
                        return -1;
                    }
                    c = (c == '\\') ? '\\' : 'n';
                }
                if (fputc(c, f) == EOF) {
                    fclose(f);
                    unlink(tmp_path);
                    return -1;
                }
            }
            if (fputc('\n', f) == EOF) {
                fclose(f);
                unlink(tmp_path);
                return -1;
            }
        } else {
            if (fputc('\n', f) == EOF) {
                fclose(f);
                unlink(tmp_path);
                return -1;
            }
        }
    }

    if (fflush(f) != 0) { fclose(f); unlink(tmp_path); return -1; }
    if (fsync(fileno(f)) != 0) { fclose(f); unlink(tmp_path); return -1; }
    if (fclose(f) != 0) { unlink(tmp_path); return -1; }

    if (rename(tmp_path, rev_path) != 0) { unlink(tmp_path); return -1; }
    if (fsync_dir_of_path(rev_path) != 0) return -1;

    ir->dirty = 0;
    return 0;
}

interner *intern_load(const char *fwd_path, const char *rev_path)
{
    interner *ir;
    FILE *rev_file = NULL;

    ir = calloc(1, sizeof(*ir));
    if (!ir) return NULL;

    /* Lazy forward DAFSA: leave ir->fwd NULL.  The 89MB symbols.dafsa is only
     * parsed on first need via intern_ensure_fwd (read-only load = ~0.48s),
     * so dl_open parses the small reverse array only. */
    if (fwd_path) {
        ir->fwd_path = strdup(fwd_path);
        if (!ir->fwd_path) { free(ir); return NULL; }
    }

    /* Set up reverse array */
    ir->rev_cap = INTERN_REV_INIT_CAP;
    ir->rev = calloc(ir->rev_cap, sizeof(char *));
    if (!ir->rev) { dafsa_free(ir->fwd); free(ir); return NULL; }

    /* Load reverse array line by line */
    rev_file = fopen(rev_path, "r");
    if (rev_file) {
        char *line = NULL;
        size_t linecap = 0;
        ssize_t linelen;

        ir->next_id = 1;
        while ((linelen = getline(&line, &linecap, rev_file)) > 0) {
            /* strip trailing newline */
            if (linelen > 0 && line[linelen - 1] == '\n')
                line[--linelen] = '\0';
            if (linelen > 0 && line[linelen - 1] == '\r')
                line[--linelen] = '\0';

            /* Decode escapes: '\\' '\\' -> 0x5C, '\\' 'n' -> 0x0A, else
             * verbatim.  Only sequences intern_save ever produces are
             * '\\' '\\' and '\\' 'n'. */
            {
                char *in = line;
                char *out = line;
                while (*in) {
                    if (*in == '\\') {
                        in++;
                        if (*in == 'n') {
                            *out++ = '\n';
                            in++;
                        } else if (*in == '\\') {
                            *out++ = '\\';
                            in++;
                        } else {
                            /* lone trailing backslash, or unknown escape */
                            *out++ = '\\';
                        }
                    } else {
                        *out++ = *in++;
                    }
                }
                *out = '\0';
            }

            /* Grow rev array if needed */
            if (ir->next_id > ir->rev_cap) {
                uint32_t new_cap = ir->rev_cap * 2;
                if (new_cap < ir->next_id) new_cap = ir->next_id * 2;
                char **new_rev = realloc(ir->rev, new_cap * sizeof(char *));
                if (!new_rev) { free(line); fclose(rev_file); goto fail; }
                memset(new_rev + ir->rev_cap, 0,
                       (new_cap - ir->rev_cap) * sizeof(char *));
                ir->rev = new_rev;
                ir->rev_cap = new_cap;
            }

            ir->rev[ir->next_id - 1] = strdup(line);
            if (!ir->rev[ir->next_id - 1]) {
                free(line); fclose(rev_file); goto fail;
            }
            ir->next_id++;
        }
        free(line);
        fclose(rev_file);
    } else {
        /* No rev file: start empty */
        ir->next_id = 1;
    }

    return ir;

fail:
    intern_free(ir);
    return NULL;
}

/* ─── Dirty tracking (M7) ──────────────────────────────────────────────── */

int intern_is_dirty(interner *ir)
{
    if (!ir) return 0;
    return ir->dirty;
}

void intern_clear_dirty(interner *ir)
{
    if (ir) ir->dirty = 0;
}
