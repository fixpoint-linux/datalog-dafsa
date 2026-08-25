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
    if (ir->fwd && (!want_mutable || !ir->fwd_ro))
        return 0;  /* already satisfies the request */

    if (ir->fwd && want_mutable) {
        /* read-only handle in place; replace it with a mutable rebuild below */
        dafsa_free(ir->fwd);
        ir->fwd = NULL;
    }

    if (ir->fwd) {
        /* satisfied read-only need */
        return 0;
    }

    if (!want_mutable) {
        d = dafsa_load_readonly(ir->fwd_path);
        if (!d) d = dafsa_create();      /* missing/corrupt file -> empty */
        if (!d) return -1;
        ir->fwd = d;
        ir->fwd_ro = 1;
        return 0;
    }

    /* Mutable rebuild from the reverse array: for each rev[i] (sym_id i+1)
     * build the same key intern_str constructs — str \0 id_u32BE. */
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
    ir->fwd_ro = 0;
    return 0;
}

/* NON-MUTATING lookup: same forward-DAFSA walk as intern_str, but never
 * allocates / never marks dirty.  Returns 0 if absent (or on NULL input). */
uint32_t intern_str_find(interner *ir, const char *str)
{
    size_t slen;
    struct capture_ctx ctx = {0, 0};

    if (!ir || !str) return 0;

    if (intern_ensure_fwd(ir, 0) != 0) return 0;   /* OOM: treat as absent */

    slen = strlen(str);
    dafsa_prefix_enum(ir->fwd, (const unsigned char *)str, slen,
                      capture_cb, &ctx);

    return ctx.found ? ctx.id : 0;
}

uint32_t intern_str(interner *ir, const char *str)
{
    size_t slen;
    struct capture_ctx ctx = {0, 0};

    if (!ir || !str) return 0;

    /* Lookup half needs fwd; the add half needs a MUTABLE fwd (it may grow).
     * Ensuring mutable here rebuilds once and covers both halves. */
    if (intern_ensure_fwd(ir, 1) != 0) return 0;   /* OOM */

    slen = strlen(str);

    /* 1. Check if already interned via prefix enum */
    dafsa_prefix_enum(ir->fwd, (const unsigned char *)str, slen,
                      capture_cb, &ctx);

    if (ctx.found)
        return ctx.id;

    /* 2. Allocate new sym_id */
    ir->dirty = 1;
    {
        uint32_t id;
        size_t key_len = slen + 1 + 4;
        unsigned char *key_buf; /* str + \0 + u32BE */

        if (key_len > MAX_WORD_LEN) return 0;  /* too long */

        key_buf = malloc(key_len);
        if (!key_buf) return 0;  /* OOM */

        id = ir->next_id++;

        memcpy(key_buf, str, slen);
        key_buf[slen] = 0x00;
        /* write id as u32 big-endian */
        key_buf[slen + 1] = (unsigned char)((id >> 24) & 0xFF);
        key_buf[slen + 2] = (unsigned char)((id >> 16) & 0xFF);
        key_buf[slen + 3] = (unsigned char)((id >> 8)  & 0xFF);
        key_buf[slen + 4] = (unsigned char)(id & 0xFF);

        if (dafsa_add_n(ir->fwd, key_buf, key_len) < 0) {
            free(key_buf);
            return 0;  /* add failed */
        }
        free(key_buf);

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

/* ─── Persistence ─────────────────────────────────────────────────────── */

int intern_save(interner *ir, const char *fwd_path, const char *rev_path)
{
    FILE *f;
    uint32_t i;
    char tmp_path[8192];

    if (!ir || !fwd_path || !rev_path) return -1;

    /* Save forward DAFSA (already atomic: dafsa_save does tmp+fsync+rename)
     * ONLY when it isn't already current at this path.  Skip iff clean AND
     * writing to the interner's own canonical path (the on-disk copy there is
     * identical).  This avoids rewriting an 89MB DAFSA on every clean close.
     * A different destination (e.g. a snapshot dir) must always be emitted.
     * dafsa_save is const, so a read-only handle suffices — no rebuild. */
    if (ir->dirty || !ir->fwd_path || !fwd_path ||
        strcmp(fwd_path, ir->fwd_path) != 0) {
        if (intern_ensure_fwd(ir, 0) != 0) return -1;
        if (dafsa_save(ir->fwd, fwd_path) != 0) return -1;
    }

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
