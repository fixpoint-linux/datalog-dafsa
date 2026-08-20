/*
 * test_vector_storage.c — S1 storage slice: vector tier storage relations
 * Round-trip tests for __sig0__..__sig15__, __vec_q__, __itq_basis__
 * Uses ONLY existing dl_declare_relation + dl_load_facts + dl_prefix + dl_iter.
 * ZERO new engine code.
 */
#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %s ... ", name); \
    fflush(stdout); \
} while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

static const char *BASE = "build-tmp/vector-storage";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* ─── Band layout helpers (C1 fix from scope doc) ──────────────────── */

/* sig is 8 u32 words (c=256 bits, MSB-first).
   band j (0..15) is a 16-bit slice.
   band 0 = high 16 of sig[0], band 1 = low 16 of sig[0],
   band 2 = high 16 of sig[1], band 3 = low 16 of sig[1], etc. */
static uint32_t band_slice(const uint32_t *sig, int j) {
    return (sig[j / 2] >> ((1u - (j % 2u)) * 16u)) & 0xFFFFu;
}

static void band_set(uint32_t *sig, int j, uint32_t val16) {
    int shift = (int)((1u - (j % 2u)) * 16u);
    sig[j/2] = (sig[j/2] & ~(0xFFFFu << shift)) | ((val16 & 0xFFFFu) << shift);
}

/* ─── Pack4 / unpack4 helpers for __vec_q__ ──────────────────────────── */

/* Pack 4 int8 values into a u32 (little-endian: b0|b1<<8|b2<<16|b3<<24) */
static uint32_t pack4(int8_t b0, int8_t b1, int8_t b2, int8_t b3) {
    return (uint32_t)(uint8_t)b0 | ((uint32_t)(uint8_t)b1 << 8) |
           ((uint32_t)(uint8_t)b2 << 16) | ((uint32_t)(uint8_t)b3 << 24);
}

/* Unpack a u32 into 4 int8 values */
static void unpack4(uint32_t packed, int8_t *out) {
    out[0] = (int8_t)(packed & 0xFF);
    out[1] = (int8_t)((packed >> 8) & 0xFF);
    out[2] = (int8_t)((packed >> 16) & 0xFF);
    out[3] = (int8_t)((packed >> 24) & 0xFF);
}

/* ─── Callback helpers ───────────────────────────────────────────────── */

typedef struct {
    int *count;
    uint32_t expected_val;
    int8_t *expected_bytes;
    int error;
} cb_user_t;

static int count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)cols; (void)arity;
    int *count = (int *)user;
    (*count)++;
    return 0;
}

static int check_entity_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)arity;
    cb_user_t *u = (cb_user_t *)user;
    if (cols[1] == u->expected_val) {
        (*u->count)++;
    }
    return 0;
}

static int check_vec_q_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)arity;
    cb_user_t *u = (cb_user_t *)user;
    uint32_t chunk_idx = cols[1];
    uint32_t packed = cols[2];
    int8_t unpacked[4];
    unpack4(packed, unpacked);
    
    int base = chunk_idx * 4;
    if (unpacked[0] != u->expected_bytes[base+0] ||
        unpacked[1] != u->expected_bytes[base+1] ||
        unpacked[2] != u->expected_bytes[base+2] ||
        unpacked[3] != u->expected_bytes[base+3]) {
        u->error = 1;
        return 1;
    }
    (*u->count)++;
    return 0;
}

static int check_u32_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)arity;
    cb_user_t *u = (cb_user_t *)user;
    if (cols[2] == u->expected_val) {
        *u->count = 1;
    }
    return 0;
}

static int noop_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)cols; (void)arity; (void)user;
    return 0;
}

/* ─── Test: band layout round-trip (C1 gate) ──────────────────────────── */

static void t_band_layout_roundtrip(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_band_layout");

    TEST("band layout round-trip (C1 gate)");

    /* Create a known 256-bit signature: each band j gets a distinct 16-bit value */
    uint32_t sig[8] = {0};
    uint32_t expected_bands[16];
    int j;
    for (j = 0; j < 16; j++) {
        expected_bands[j] = (uint32_t)j * 0x111u + 1; /* j*0x111+1 fits in 16 bits for j<16 */
        band_set(sig, j, expected_bands[j]);
    }

    /* Verify band_slice recovers the same values */
    for (j = 0; j < 16; j++) {
        uint32_t sliced = band_slice(sig, j);
        if (sliced != expected_bands[j]) {
            FAIL("band_slice mismatch");
            dl_close(db);
            return;
        }
    }

    /* Reconstruct sig from band_slice outputs and verify */
    uint32_t reconstructed[8] = {0};
    for (j = 0; j < 16; j++) {
        band_set(reconstructed, j, band_slice(sig, j));
    }
    for (j = 0; j < 8; j++) {
        if (reconstructed[j] != sig[j]) {
            FAIL("reconstruction mismatch");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test: declare sig_j relations ───────────────────────────────────── */

static void t_declare_sig_relations(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_declare_sig");

    TEST("declare __sig0__..__sig15__ (arity 2)");

    int j;
    for (j = 0; j < 16; j++) {
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        if (dl_declare_relation(db, rel_name, 2) != 0) {
            FAIL("failed to declare sig relation");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test: sig_j round-trip with dl_add_fact + dl_prefix ─────────────── */

static void t_sig_j_roundtrip(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_sig_roundtrip");

    TEST("sig_j round-trip via dl_add_fact + dl_prefix");

    /* Declare all sig relations */
    int j;
    for (j = 0; j < 16; j++) {
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        if (dl_declare_relation(db, rel_name, 2) != 0) {
            FAIL("failed to declare sig relation");
            dl_close(db);
            return;
        }
    }

    /* Create a known signature and add facts */
    uint32_t sig[8] = {0};
    uint32_t entity_id = 42;
    for (j = 0; j < 16; j++) {
        uint32_t band_val = (j + 1) * 0x1111u;
        band_set(sig, j, band_val);
        
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        uint32_t cols[2] = {band_val, entity_id};
        if (dl_add_fact(db, rel_name, cols, 2) != 1) {
            FAIL("failed to add sig fact");
            dl_close(db);
            return;
        }
    }

    /* Read back via dl_prefix for each sig_j */
    for (j = 0; j < 16; j++) {
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        uint32_t band_val = (j + 1) * 0x1111u;
        
        uint32_t leading[1] = {band_val};
        int count = 0;
        cb_user_t user = {&count, entity_id, NULL, 0};
        long n = dl_prefix(db, rel_name, leading, 1, check_entity_cb, &user);
        
        if (n != 1) {
            FAIL("expected 1 fact per sig_j");
            dl_close(db);
            return;
        }
        if (count != 1) {
            FAIL("expected entity_id 42");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test: SET-semantic dedup ─────────────────────────────────────────── */

static void t_sig_dedup(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_sig_dedup");

    TEST("SET-semantic dedup: adding same key twice yields 1 fact");

    if (dl_declare_relation(db, "__sig0__", 2) != 0) {
        FAIL("failed to declare __sig0__");
        dl_close(db);
        return;
    }

    uint32_t cols[2] = {0x1234u, 42u};
    
    /* Add first time */
    if (dl_add_fact(db, "__sig0__", cols, 2) != 1) {
        FAIL("first add should return 1");
        dl_close(db);
        return;
    }
    
    /* Add duplicate */
    if (dl_add_fact(db, "__sig0__", cols, 2) != 0) {
        FAIL("duplicate add should return 0");
        dl_close(db);
        return;
    }
    
    /* Verify only 1 fact exists */
    uint32_t leading[1] = {0x1234u};
    int count = 0;
    long n = dl_prefix(db, "__sig0__", leading, 1, count_cb, &count);
    
    if (n != 1) {
        FAIL("expected exactly 1 fact after dedup");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* ─── Test: __vec_q__ round-trip ───────────────────────────────────────── */

static void t_vec_q_roundtrip(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_vec_q");

    TEST("__vec_q__ round-trip: pack4/unpack + dl_prefix");

    if (dl_declare_relation(db, "__vec_q__", 3) != 0) {
        FAIL("failed to declare __vec_q__");
        dl_close(db);
        return;
    }

    uint32_t entity_id = 100;
    int8_t raw_bytes[384];
    
    /* Fill with a known pattern: byte i = (i % 256) - 128 (signed) */
    int i;
    for (i = 0; i < 384; i++) {
        raw_bytes[i] = (int8_t)(i % 256) - 128;
    }
    
    /* Add all 96 chunks */
    for (i = 0; i < 96; i++) {
        uint32_t packed = pack4(
            raw_bytes[i*4+0],
            raw_bytes[i*4+1],
            raw_bytes[i*4+2],
            raw_bytes[i*4+3]
        );
        uint32_t cols[3] = {entity_id, (uint32_t)i, packed};
        if (dl_add_fact(db, "__vec_q__", cols, 3) != 1) {
            FAIL("failed to add vec_q fact");
            dl_close(db);
            return;
        }
    }
    
    /* Read back via dl_prefix with k=1 (entity_id only) */
    uint32_t leading[1] = {entity_id};
    int chunk_count = 0;
    cb_user_t user = {&chunk_count, 0, raw_bytes, 0};
    long n = dl_prefix(db, "__vec_q__", leading, 1, check_vec_q_cb, &user);
    
    if (n != 96) {
        FAIL("expected 96 chunks");
        dl_close(db);
        return;
    }
    if (chunk_count != 96) {
        FAIL("chunk count mismatch");
        dl_close(db);
        return;
    }
    if (user.error) {
        FAIL("pack/unpack mismatch");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* ─── Test: __vec_q__ with u32 > INT32_MAX (F6 gate) ──────────────────── */

static void t_vec_q_full_u32(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_vec_q_full_u32");

    TEST("__vec_q__ with packed u32 > INT32_MAX (F6 CSV parsing gate)");

    if (dl_declare_relation(db, "__vec_q__", 3) != 0) {
        FAIL("failed to declare __vec_q__");
        dl_close(db);
        return;
    }

    uint32_t entity_id = 1;
    uint32_t chunk_idx = 0;
    
    /* Create a packed value > INT32_MAX (0x80000000) */
    /* Use bytes: 0xFF, 0xFF, 0xFF, 0xFF -> packed = 0xFFFFFFFF > INT32_MAX */
    uint32_t packed = pack4(-1, -1, -1, -1); /* 0xFFFFFFFF */
    
    uint32_t cols[3] = {entity_id, chunk_idx, packed};
    if (dl_add_fact(db, "__vec_q__", cols, 3) != 1) {
        FAIL("failed to add fact with u32 > INT32_MAX");
        dl_close(db);
        return;
    }
    
    /* Read back and verify */
    uint32_t leading[1] = {entity_id};
    int found = 0;
    cb_user_t user = {&found, packed, NULL, 0};
    long n = dl_prefix(db, "__vec_q__", leading, 1, check_u32_cb, &user);
    
    if (n != 1) {
        FAIL("expected 1 fact");
        dl_close(db);
        return;
    }
    if (!found) {
        FAIL("expected packed value 0xFFFFFFFF");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* ─── Test: __itq_basis__ round-trip ────────────────────────────────────── */

static void t_itq_basis_roundtrip(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_itq_basis");

    TEST("__itq_basis__ round-trip via dl_add_fact + dl_prefix");

    if (dl_declare_relation(db, "__itq_basis__", 3) != 0) {
        FAIL("failed to declare __itq_basis__");
        dl_close(db);
        return;
    }

    /* Add a few basis facts */
    /* dim_i, dim_j, float32_bits_u32 */
    uint32_t facts[3][3] = {
        {0u, 0u, 0x3F800000u},   /* 1.0f */
        {0u, 1u, 0x40000000u},   /* 2.0f */
        {1u, 0u, 0x40400000u},   /* 3.0f */
    };
    
    int i;
    for (i = 0; i < 3; i++) {
        if (dl_add_fact(db, "__itq_basis__", facts[i], 3) != 1) {
            FAIL("failed to add itq_basis fact");
            dl_close(db);
            return;
        }
    }
    
    /* Read back via dl_prefix with k=2 (dim_i, dim_j) */
    for (i = 0; i < 3; i++) {
        uint32_t leading[2] = {facts[i][0], facts[i][1]};
        long n = dl_prefix(db, "__itq_basis__", leading, 2, noop_cb, NULL);
        
        if (n != 1) {
            FAIL("expected 1 fact per (dim_i, dim_j)");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test: dl_iter over __vec_q__ ──────────────────────────────────────── */

static void t_vec_q_iter(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_vec_q_iter");

    TEST("__vec_q__ dl_iter round-trip");

    if (dl_declare_relation(db, "__vec_q__", 3) != 0) {
        FAIL("failed to declare __vec_q__");
        dl_close(db);
        return;
    }

    uint32_t entity_id = 200;
    
    /* Add 5 chunks */
    for (int i = 0; i < 5; i++) {
        uint32_t packed = pack4((int8_t)i, (int8_t)(i+1), (int8_t)(i+2), (int8_t)(i+3));
        uint32_t cols[3] = {entity_id, (uint32_t)i, packed};
        if (dl_add_fact(db, "__vec_q__", cols, 3) != 1) {
            FAIL("failed to add vec_q fact");
            dl_close(db);
            return;
        }
    }
    
    /* Iterate with dl_iter */
    uint32_t leading[1] = {entity_id};
    dl_iter *it = dl_iter_open(db, "__vec_q__", leading, 1);
    if (!it) {
        FAIL("failed to open iterator");
        dl_close(db);
        return;
    }
    
    int count = 0;
    uint32_t cols[3];
    while (dl_iter_next(it, cols) == 1) {
        count++;
        if (cols[0] != entity_id) {
            FAIL("entity_id mismatch in iterator");
            dl_iter_close(it);
            dl_close(db);
            return;
        }
    }
    
    dl_iter_close(it);
    
    if (count != 5) {
        FAIL("expected 5 chunks from iterator");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* ─── Test: dl_load_facts CSV with full u32 values ────────────────────── */

static void t_load_facts_full_u32(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_load_facts");

    TEST("dl_load_facts CSV with full u32 values (F6)");

    if (dl_declare_relation(db, "__vec_q__", 3) != 0) {
        FAIL("failed to declare __vec_q__");
        dl_close(db);
        return;
    }

    /* Write a CSV file with values > INT32_MAX */
    FILE *fp = fopen("build-tmp/test_vec_q.csv", "w");
    if (!fp) {
        FAIL("failed to create CSV file");
        dl_close(db);
        return;
    }
    
    /* entity_id, chunk_idx, packed_4x_int8_u32 */
    /* 0xFFFFFFFF > INT32_MAX */
    fprintf(fp, "1,0,4294967295\n");
    /* 0x80000000 == INT32_MIN as unsigned */
    fprintf(fp, "2,0,2147483648\n");
    /* Normal value */
    fprintf(fp, "3,0,12345\n");
    
    fclose(fp);
    
    /* Load the CSV */
    int n_loaded = dl_load_facts(db, "__vec_q__", "build-tmp/test_vec_q.csv");
    if (n_loaded != 3) {
        FAIL("expected 3 facts loaded");
        dl_close(db);
        return;
    }
    
    /* Verify the > INT32_MAX value was loaded correctly */
    uint32_t leading[1] = {1};
    int found = 0;
    cb_user_t user1 = {&found, 0xFFFFFFFFu, NULL, 0};
    long n = dl_prefix(db, "__vec_q__", leading, 1, check_u32_cb, &user1);
    
    if (n != 1) {
        FAIL("expected 1 fact for entity 1");
        dl_close(db);
        return;
    }
    if (!found) {
        FAIL("expected packed value 0xFFFFFFFF for entity 1");
        dl_close(db);
        return;
    }
    
    /* Verify the INT32_MIN-as-unsigned value */
    leading[0] = 2;
    found = 0;
    cb_user_t user2 = {&found, 0x80000000u, NULL, 0};
    n = dl_prefix(db, "__vec_q__", leading, 1, check_u32_cb, &user2);
    
    if (n != 1) {
        FAIL("expected 1 fact for entity 2");
        dl_close(db);
        return;
    }
    if (!found) {
        FAIL("expected packed value 0x80000000 for entity 2");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
    
    /* Clean up CSV */
    remove("build-tmp/test_vec_q.csv");
}

/* ─── Test: all 16 sig relations with dl_prefix ───────────────────────── */

static void t_all_sig_relations(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_all_sig");

    TEST("all 16 sig relations: declare + add + prefix");

    /* Declare all 16 */
    for (int j = 0; j < 16; j++) {
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        if (dl_declare_relation(db, rel_name, 2) != 0) {
            FAIL("failed to declare sig relation");
            dl_close(db);
            return;
        }
    }

    /* Add facts to each */
    for (int j = 0; j < 16; j++) {
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        uint32_t cols[2] = {j * 0x1000u, (uint32_t)(j * 100 + 1)};
        if (dl_add_fact(db, rel_name, cols, 2) != 1) {
            FAIL("failed to add sig fact");
            dl_close(db);
            return;
        }
    }

    /* Verify each via dl_prefix */
    for (int j = 0; j < 16; j++) {
        char rel_name[32];
        snprintf(rel_name, sizeof(rel_name), "__sig%d__", j);
        uint32_t leading[1] = {j * 0x1000u};
        int count = 0;
        long n = dl_prefix(db, rel_name, leading, 1, count_cb, &count);
        if (n != 1) {
            FAIL("expected 1 fact per sig relation");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_vector_storage (S1) ===\n");
    
    /* Band layout (C1 gate) */
    t_band_layout_roundtrip();
    
    /* Relation declarations */
    t_declare_sig_relations();
    
    /* sig_j round-trip */
    t_sig_j_roundtrip();
    
    /* SET-semantic dedup */
    t_sig_dedup();
    
    /* __vec_q__ round-trip */
    t_vec_q_roundtrip();
    
    /* __vec_q__ with full u32 > INT32_MAX (F6) */
    t_vec_q_full_u32();
    
    /* __itq_basis__ round-trip */
    t_itq_basis_roundtrip();
    
    /* dl_iter over __vec_q__ */
    t_vec_q_iter();
    
    /* dl_load_facts with full u32 */
    t_load_facts_full_u32();
    
    /* All 16 sig relations */
    t_all_sig_relations();
    
    printf("\n");
    if (tests_failed == 0)
        printf("All %d tests passed.\n", tests_run);
    else
        printf("%d/%d tests passed, %d failed.\n",
               tests_run - tests_failed, tests_run, tests_failed);
    
    return tests_failed != 0;
}
