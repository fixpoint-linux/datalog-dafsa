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

#include <stdlib.h>
#include <string.h>

#define INTERN_REV_INIT_CAP 256

struct interner {
    dafsa   *fwd;          /* forward DAFSA: str\0\sym_id */
    char   **rev;          /* reverse array: rev[sym_id-1] -> string */
    uint32_t rev_cap;      /* capacity of rev */
    uint32_t next_id;      /* next sym_id to allocate (1-based) */
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
    if (ir->rev) {
        uint32_t i;
        for (i = 0; i < ir->next_id - 1 && i < ir->rev_cap; i++)
            free(ir->rev[i]);
        free(ir->rev);
    }
    free(ir);
}

/* ─── Core ops ────────────────────────────────────────────────────────── */

uint32_t intern_str(interner *ir, const char *str)
{
    size_t slen;
    struct capture_ctx ctx = {0, 0};

    if (!ir || !str) return 0;

    slen = strlen(str);

    /* 1. Check if already interned via prefix enum */
    dafsa_prefix_enum(ir->fwd, (const unsigned char *)str, slen,
                      capture_cb, &ctx);

    if (ctx.found)
        return ctx.id;

    /* 2. Allocate new sym_id */
    {
        uint32_t id = ir->next_id++;
        unsigned char key_buf[4096 + 1 + 4]; /* str + \0 + u32BE */
        size_t key_len = slen + 1 + 4;

        if (key_len > sizeof(key_buf)) return 0;  /* too long */

        memcpy(key_buf, str, slen);
        key_buf[slen] = 0x00;
        /* write id as u32 big-endian */
        key_buf[slen + 1] = (unsigned char)((id >> 24) & 0xFF);
        key_buf[slen + 2] = (unsigned char)((id >> 16) & 0xFF);
        key_buf[slen + 3] = (unsigned char)((id >> 8)  & 0xFF);
        key_buf[slen + 4] = (unsigned char)(id & 0xFF);

        if (dafsa_add_n(ir->fwd, key_buf, key_len) < 0)
            return 0;  /* add failed */

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

/* ─── Persistence ─────────────────────────────────────────────────────── */

int intern_save(interner *ir, const char *fwd_path, const char *rev_path)
{
    FILE *f;
    uint32_t i;

    if (!ir || !fwd_path || !rev_path) return -1;

    /* Save forward DAFSA */
    if (dafsa_save(ir->fwd, fwd_path) != 0) return -1;

    /* Save reverse array: one string per line (NUL is not valid in our
     * strings; if a stored string contains NUL, we truncate at it). */
    f = fopen(rev_path, "w");
    if (!f) return -1;

    for (i = 1; i < ir->next_id; i++) {
        const char *s = ir->rev[i - 1];
        if (s) {
            if (fputs(s, f) == EOF || fputc('\n', f) == EOF) {
                fclose(f);
                return -1;
            }
        } else {
            if (fputc('\n', f) == EOF) {
                fclose(f);
                return -1;
            }
        }
    }

    if (fclose(f) != 0) return -1;
    return 0;
}

interner *intern_load(const char *fwd_path, const char *rev_path)
{
    interner *ir;
    dafsa *fwd = NULL;
    FILE *rev_file = NULL;

    ir = calloc(1, sizeof(*ir));
    if (!ir) return NULL;

    /* Load forward DAFSA (may be NULL if file doesn't exist) */
    fwd = dafsa_load(fwd_path);
    if (!fwd) {
        fwd = dafsa_create();
        if (!fwd) { free(ir); return NULL; }
    }
    ir->fwd = fwd;

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
