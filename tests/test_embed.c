/*
 * test_embed.c — engine-side byte-identity gate for the dl-embed emission.
 *
 * Proves the src/embed/vec_bits.h emission formulas (band slicing, pack4,
 * float32 bits) are EXACTLY what src/vector.c consumes: entities are indexed
 * via vec_bits.h exactly as dl-embed pipeline would emit them, and the REAL
 * dl_vector_search / dl_vector_rerank engine paths must find/rank them.
 *
 * (The dl-embed end-to-end pipeline needs the GGUF model and runs under
 * `make embed-test` / the guarded make-test hook instead.)
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
    TEST("vec_bits: band_slice/pack4/float32_bits goldens");
    uint32_t sig[VEC_SIG_WORDS];
    int w, ok = 1;
    static uint32_t state = 0xC0FFEEu;
    for (w = 0; w < VEC_SIG_WORDS; w++) {
        state = state * 1664525u + 1013904223u;
        sig[w] = state;
    }
    /* independent recomputation of the C1 formula */
    for (int j = 0; j < VEC_M; j++) {
        uint32_t want = (sig[j / 2] >> ((1u - (uint32_t)(j % 2)) * 16u)) & 0xFFFFu;
        if (vec_band_slice(sig, j) != want) ok = 0;
    }
    if (vec_band_slice(sig, 0) != (sig[0] >> 16)) ok = 0;
    if (vec_band_slice(sig, 1) != (sig[0] & 0xFFFFu)) ok = 0;
    if (vec_band_slice(sig, 15) != (sig[7] & 0xFFFFu)) ok = 0;
    if (vec_pack4_le(1, 2, 3, 4) != 0x04030201u) ok = 0;
    /* -1=0xFF, -128=0x80, 127=0x7F, 0=0x00 -> 0x007F80FF */
    if (vec_pack4_le(-1, -128, 127, 0) != 0x007F80FFu) ok = 0;
    if (vec_float32_bits(1.0f) != 0x3F800000u) ok = 0;
    if (vec_float32_bits(-2.0f) != 0xC0000000u) ok = 0;
    if (vec_bits_float32(0x3F800000u) != 1.0f) ok = 0;
    /* round-trip */
    uint32_t recon[VEC_SIG_WORDS] = {0};
    for (int j = 0; j < VEC_M; j++) vec_band_set(recon, j, vec_band_slice(sig, j));
    if (memcmp(sig, recon, sizeof sig) != 0) ok = 0;
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

    /* deterministic signature + int8 vector for one entity */
    static uint32_t st = 0xABCDEF01u;
    uint32_t sig[VEC_SIG_WORDS];
    for (int w = 0; w < VEC_SIG_WORDS; w++) {
        st = st * 1664525u + 1013904223u;
        sig[w] = st;
    }
    int8_t vec[VEC_D];
    for (int d = 0; d < VEC_D; d++) {
        st = st * 1664525u + 1013904223u;
        vec[d] = (int8_t)(st % 255u) - 127;
    }
    uint32_t ivec[VEC_IVEC_WORDS];
    for (int c = 0; c < VEC_IVEC_WORDS; c++)
        ivec[c] = vec_pack4_le(vec[c*4+0], vec[c*4+1], vec[c*4+2], vec[c*4+3]);

    /* declare relations + emit exactly like dl-embed pipeline does */
    dl_declare_relation(db, VEC_ENTITY_REL, 2);
    dl_declare_relation(db, "__vec_q__", 3);
    for (int j = 0; j < VEC_M; j++) {
        char rel[32];
        snprintf(rel, sizeof(rel), "__sig%d__", j);
        dl_declare_relation(db, rel, 2);
        uint32_t row[2] = { vec_band_slice(sig, j), 0 /*sym*/ };
        (void)row;
    }
    uint32_t sym = dl_intern_str(db, "roundtrip_entity");
    uint32_t type = dl_intern_str(db, "t");
    uint32_t ent_row[2] = { sym, type };
    if (dl_add_fact(db, VEC_ENTITY_REL, ent_row, 2) != 1) { FAIL("add entity"); dl_close(db); return; }
    for (int j = 0; j < VEC_M; j++) {
        char rel[32];
        uint32_t row[2];
        snprintf(rel, sizeof(rel), "__sig%d__", j);
        row[0] = vec_band_slice(sig, j);
        row[1] = sym;
        if (dl_add_fact(db, rel, row, 2) != 1) { FAIL("add sig fact"); dl_close(db); return; }
    }
    for (int c = 0; c < VEC_IVEC_WORDS; c++) {
        uint32_t row[3] = { sym, (uint32_t)c, ivec[c] };
        if (dl_add_fact(db, "__vec_q__", row, 3) != 1) { FAIL("add vecq fact"); dl_close(db); return; }
    }

    /* search with the SAME signature at radius 0: every band must match ->
     * score must be exactly VEC_M and the entity must be returned. */
    res_coll c1 = { {0}, {0}, 0 };
    long n = dl_vector_search(db, sig, 10, 0, res_cb, &c1);
    int ok = (n == 1 && c1.n == 1 && c1.syms[0] == sym && c1.scores[0] == VEC_M);
    if (!ok) {
        printf("\n    search returned n=%ld cnt=%d score=%d ", n, c1.n,
               c1.n ? c1.scores[0] : -1);
        FAIL("radius-0 search must match all 16 bands");
        dl_close(db);
        return;
    }

    /* rerank with the same int8 vector: the integer dot must equal v.v
     * (score is int; only compared when the true dot fits) */
    int64_t vd = 0;
    for (int d = 0; d < VEC_D; d++) vd += (int64_t)vec[d] * (int64_t)vec[d];
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
