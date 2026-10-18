/*
 * test_embed.c — engine-side byte-identity gate for the encoder emission.
 *
 * Proves the src/embed/vec_bits.h emission formulas (pack4, float32 bits) are
 * EXACTLY what the query path consumes: an entity is indexed via vec_bits.h
 * exactly as the external encoder emits it — `entity` liveness plus the 96
 * packed __vec_q__ chunks, and nothing else — and the REAL dl_vector_search /
 * dl_vector_rerank engine paths must find/rank it.
 *
 * Since U9 (2026-08-11) the encoder emits NO 256-bit band signature: the query
 * path scans __vec_q__ and ranks by exact cosine, so there is no band layout
 * for the two sides to disagree about (that was the silent-failure mode the
 * old __sig{j}__ pipeline carried).
 */
#include "dl.h"
#include "vector.h"

#include "../src/embed/vec_bits.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int tests_run = 0, tests_failed = 0;
#define TEST(name) do { tests_run++; printf("  %s ... ", name); fflush(stdout); } while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

static const char *BASE = "build-tmp/embed";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[1024];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* ─── 1. golden byte-identity of the emission formulas ──────────────────── */

static void t_vec_bits_goldens(void)
{
    TEST("vec_bits: pack4/unpack4/float32_bits goldens");
    int ok = 1;
    if (vec_pack4_le(1, 2, 3, 4) != 0x04030201u) ok = 0;
    /* -1=0xFF, -128=0x80, 127=0x7F, 0=0x00 -> 0x007F80FF */
    if (vec_pack4_le(-1, -128, 127, 0) != 0x007F80FFu) ok = 0;
    if (vec_float32_bits(1.0f) != 0x3F800000u) ok = 0;
    if (vec_float32_bits(-2.0f) != 0xC0000000u) ok = 0;
    if (vec_bits_float32(0x3F800000u) != 1.0f) ok = 0;
    /* every byte pattern round-trips through pack4/unpack4 */
    for (int i = 0; i < 256; i++) {
        int8_t b0 = (int8_t)(i & 0xFF);
        int8_t b1 = (int8_t)((i * 7 + 13) & 0xFF);
        int8_t b2 = (int8_t)((i * 31 + 5) & 0xFF);
        int8_t b3 = (int8_t)((i * 13 + 3) & 0xFF);
        int8_t out[4];
        vec_unpack4_le(vec_pack4_le(b0, b1, b2, b3), out);
        if (out[0] != b0 || out[1] != b1 || out[2] != b2 || out[3] != b3) ok = 0;
    }
    if (ok) PASS(); else FAIL("vec_bits golden mismatch");
}

/* ─── 2. emission <-> engine round-trip (the real C1 gate) ─────────────── */

typedef struct { uint32_t syms[64]; int scores[64]; int n; } res_coll;

static int res_cb(uint32_t sym, int score, void *user)
{
    res_coll *c = (res_coll *)user;
    if (c->n < 64) { c->syms[c->n] = sym; c->scores[c->n] = score; c->n++; }
    return 0;
}

static void t_emission_roundtrip(void)
{
    TEST("vec_bits emission <-> dl_vector_search/rerank round-trip");
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_roundtrip");
    if (!db) { FAIL("dl_open"); return; }

    /* deterministic int8 vector for one entity */
    static uint32_t st = 0xABCDEF01u;
    int8_t vec[VEC_D];
    for (int d = 0; d < VEC_D; d++) {
        st = st * 1664525u + 1013904223u;
        vec[d] = (int8_t)(st % 255u) - 127;
    }
    uint32_t ivec[VEC_IVEC_WORDS];
    for (int c = 0; c < VEC_IVEC_WORDS; c++)
        ivec[c] = vec_pack4_le(vec[c*4+0], vec[c*4+1], vec[c*4+2], vec[c*4+3]);

    /* declare relations + emit exactly like the external encoder does:
       `entity` liveness + the 96 packed __vec_q__ chunks, nothing else. */
    dl_declare_relation(db, VEC_ENTITY_REL, 2);
    dl_declare_relation(db, "__vec_q__", 3);
    uint32_t sym = dl_intern_str(db, "roundtrip_entity");
    uint32_t type = dl_intern_str(db, "t");
    uint32_t ent_row[2] = { sym, type };
    if (dl_add_fact(db, VEC_ENTITY_REL, ent_row, 2) != 1) { FAIL("add entity"); dl_close(db); return; }
    for (int c = 0; c < VEC_IVEC_WORDS; c++) {
        uint32_t row[3] = { sym, (uint32_t)c, ivec[c] };
        if (dl_add_fact(db, "__vec_q__", row, 3) != 1) { FAIL("add vecq fact"); dl_close(db); return; }
    }

    /* search with the entity's OWN vector: the entity must be returned and
     * its score must be the integer dot q.v = v.v (cosine 1.0 with itself). */
    int64_t vd = 0;
    for (int d = 0; d < VEC_D; d++) vd += (int64_t)vec[d] * (int64_t)vec[d];
    res_coll c1 = { {0}, {0}, 0 };
    long n = dl_vector_search(db, ivec, 10, res_cb, &c1);
    int ok = (n == 1 && c1.n == 1 && c1.syms[0] == sym &&
              vd <= INT32_MAX && (int64_t)c1.scores[0] == vd);
    if (!ok) {
        printf("\n    search returned n=%ld cnt=%d score=%d (want %lld) ", n, c1.n,
               c1.n ? c1.scores[0] : -1, (long long)vd);
        FAIL("search must return the entity with score = v.v");
        dl_close(db);
        return;
    }

    /* rerank of that one candidate must agree (same ranking path). */
    res_coll c2 = { {0}, {0}, 0 };
    uint32_t cands[1] = { sym };
    long m = dl_vector_rerank(db, ivec, cands, 1, 10, res_cb, &c2);
    ok = (m == 1 && c2.n == 1 && c2.syms[0] == sym);
    if (ok && vd <= INT32_MAX && vd >= INT32_MIN &&
        (int64_t)c2.scores[0] != vd)
        ok = 0;
    if (ok) PASS(); else FAIL("rerank dot score mismatch");
    dl_close(db);
}

/* ─── 3. quantize semantics parity (C-side mirror of the C++ golden) ───── */

static void t_quantize_truncation(void)
{
    TEST("pack4 negative golden + unpack round-trip");
    int8_t un[4];
    vec_unpack4_le(0x007F80FFu, un);
    if (un[0] == -1 && un[1] == -128 && un[2] == 127 && un[3] == 0) PASS();
    else FAIL("unpack4 mismatch");
}

int main(void)
{
    printf("test_embed (engine-side byte identity)\n");
    t_vec_bits_goldens();
    t_emission_roundtrip();
    t_quantize_truncation();
    printf("test_embed: %d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
