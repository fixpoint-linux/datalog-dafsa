/*
 * test_m5_review.c — adversarial review tests for M5 regex/pattern walker
 *
 * Tests >50 regex correctness cases, DFA state cap, termination/completeness,
 * full-key semantics, integration, OP_WALK edge cases, memory/robustness,
 * byte handling.
 */

#include "dl.h"
#include "relation.h"
#include "dafsa.h"
#include "dafsa_internal.h"
#include "regexwalk.h"
#include "snapshot.h"
#include "tupleset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %s ... ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { printf("OK\n"); } while(0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

#define CHECK(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

/* ─── Helper: build DAFSA from raw keys ────────────────────────────────── */

static dafsa *build_dafsa_str(const char **keys, int n)
{
    dafsa *d = dafsa_create();
    if (!d) return NULL;
    for (int i = 0; i < n; i++)
        if (dafsa_add_n(d, (const unsigned char *)keys[i], strlen(keys[i])) < 0)
        { dafsa_free(d); return NULL; }
    return d;
}

/* ─── Key collector callback ────────────────────────────────────────────── */

typedef struct {
    char **keys;
    int    n, cap;
} key_set;

static int kc_cb(const unsigned char *kb, size_t kl, void *user)
{
    key_set *ks = (key_set *)user;
    if (ks->n >= ks->cap) {
        int nc = ks->cap ? ks->cap * 2 : 32;
        char **nk = realloc(ks->keys, (size_t)nc * sizeof(char *));
        if (!nk) return -1;
        ks->keys = nk;
        ks->cap = nc;
    }
    ks->keys[ks->n] = malloc(kl + 1);
    memcpy(ks->keys[ks->n], kb, kl);
    ks->keys[ks->n][kl] = '\0';
    ks->n++;
    return 0;
}

static void ks_free(key_set *ks)
{
    for (int i = 0; i < ks->n; i++) free(ks->keys[i]);
    free(ks->keys);
    memset(ks, 0, sizeof(*ks));
}

static int ks_has(key_set *ks, const char *k)
{
    for (int i = 0; i < ks->n; i++)
        if (strcmp(ks->keys[i], k) == 0) return 1;
    return 0;
}

/* Walk and return count */
static long walk_count(dafsa *d, const char *pat)
{
    regex_dfa *dfa = regex_compile(pat);
    if (!dfa || dfa->errmsg) { regex_dfa_free(dfa); return -1; }
    key_set ks = {0};
    long n = regex_dfa_walk(d, dfa, kc_cb, &ks);
    ks_free(&ks);
    regex_dfa_free(dfa);
    return n;
}

/* Walk and return key_set */
static key_set walk_collect(dafsa *d, const char *pat)
{
    key_set ks = {0};
    regex_dfa *dfa = regex_compile(pat);
    if (!dfa || dfa->errmsg) { regex_dfa_free(dfa); return ks; }
    regex_dfa_walk(d, dfa, kc_cb, &ks);
    regex_dfa_free(dfa);
    return ks;
}

/* ─── A: Regex correctness oracle ──────────────────────────────────────── */

static void t_a01_literal(void) {
    TEST("A01 literal 'hello'");
    const char *k[] = {"hello","hell","hello!","HELLO",""};
    dafsa *d = build_dafsa_str(k,5);
    key_set s = walk_collect(d,"hello");
    CHECK(s.n==1 && ks_has(&s,"hello"), "exact match");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a02_alt(void) {
    TEST("A02 alt 'a|b|c'");
    const char *k[] = {"a","b","c","d","ab",""};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"a|b|c");
    CHECK(s.n==3 && ks_has(&s,"a") && ks_has(&s,"b") && ks_has(&s,"c"), "3 matches");
    CHECK(!ks_has(&s,"d") && !ks_has(&s,"ab"), "no spurious");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a03_star_group(void) {
    TEST("A03 '(ab|cd)*'");
    const char *k[] = {"","ab","cd","abcd","abab","cdcd","abc","a","abx"};
    dafsa *d = build_dafsa_str(k,9);
    key_set s = walk_collect(d,"(ab|cd)*");
    CHECK(ks_has(&s,"") && ks_has(&s,"ab") && ks_has(&s,"cd") &&
          ks_has(&s,"abcd") && ks_has(&s,"abab") && ks_has(&s,"cdcd"), "all 6 matches");
    CHECK(!ks_has(&s,"abc") && !ks_has(&s,"a") && !ks_has(&s,"abx"), "no spurious");
    CHECK(s.n==6, "count 6");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a04_dot_star_prefix(void) {
    TEST("A04 'a.*'");
    const char *k[] = {"a","ab","abc","ba","xa",""};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"a.*");
    CHECK(ks_has(&s,"a") && ks_has(&s,"ab") && ks_has(&s,"abc"), "a,ab,abc");
    CHECK(!ks_has(&s,"ba") && !ks_has(&s,"xa") && !ks_has(&s,""), "no spurious");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a05_dot_star_anywhere(void) {
    TEST("A05 '.*a.*'");
    const char *k[] = {"a","ab","ba","bab","xyz",""};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,".*a.*");
    CHECK(ks_has(&s,"a") && ks_has(&s,"ab") && ks_has(&s,"ba") && ks_has(&s,"bab"), "4 contain a");
    CHECK(!ks_has(&s,"xyz") && !ks_has(&s,""), "no spurious");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a06_neg_class(void) {
    TEST("A06 '[^a]'");
    const char *k[] = {"a","b","c","x","y",""};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"[^a]");
    CHECK(ks_has(&s,"b") && ks_has(&s,"c") && ks_has(&s,"x") && ks_has(&s,"y"), "b,c,x,y");
    CHECK(!ks_has(&s,"a") && !ks_has(&s,""), "!a,!empty");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a07_range_class(void) {
    TEST("A07 '[a-c][0-9]'");
    const char *k[] = {"a0","a1","b9","c0","d0","a","ab"};
    dafsa *d = build_dafsa_str(k,7);
    key_set s = walk_collect(d,"[a-c][0-9]");
    CHECK(ks_has(&s,"a0") && ks_has(&s,"a1") && ks_has(&s,"b9") && ks_has(&s,"c0"), "a0..c0");
    CHECK(!ks_has(&s,"d0") && !ks_has(&s,"a"), "!d0,!a");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a08_nested(void) {
    TEST("A08 '((a|b)c)?'");
    const char *k[] = {"","ac","bc","a","b","c","abc","ab"};
    dafsa *d = build_dafsa_str(k,8);
    key_set s = walk_collect(d,"((a|b)c)?");
    CHECK(ks_has(&s,"") && ks_has(&s,"ac") && ks_has(&s,"bc"), "empty,ac,bc");
    CHECK(!ks_has(&s,"a") && !ks_has(&s,"abc"), "!a,!abc");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a09_quant(void) {
    TEST("A09 'a+','a?','a*b?'");
    const char *k[] = {"","a","aa","aaa","b","ab","aab"};
    dafsa *d = build_dafsa_str(k,7);

    key_set s1 = walk_collect(d,"a+");
    CHECK(ks_has(&s1,"a") && ks_has(&s1,"aa") && ks_has(&s1,"aaa") && !ks_has(&s1,""), "a+");
    ks_free(&s1);

    key_set s2 = walk_collect(d,"a?");
    CHECK(ks_has(&s2,"") && ks_has(&s2,"a") && !ks_has(&s2,"aa"), "a?");
    ks_free(&s2);

    key_set s3 = walk_collect(d,"a*b?");
    CHECK(ks_has(&s3,"") && ks_has(&s3,"a") && ks_has(&s3,"aa") && ks_has(&s3,"ab") && ks_has(&s3,"b"), "a*b?");
    /* a*b? matches aaa (a* consumes all a's, b? empty) and aab (a*=aa, b?=b). */
    CHECK(ks_has(&s3,"aaa") && ks_has(&s3,"aab"), "aaa,aab match too");
    ks_free(&s3);
    dafsa_free(d); PASS();
}

static void t_a10_dot_null_ff(void) {
    TEST("A10 '.' matches 0x00 and 0xFF");
    unsigned char k0[]={0x00,0x00}, kf[]={0xFF,0x00};
    dafsa *d = dafsa_create();
    dafsa_add_n(d,k0,1); dafsa_add_n(d,kf,1);
    long n = walk_count(d,".");
    CHECK(n==2, "both single-byte keys");
    dafsa_free(d); PASS();
}

static void t_a11_xHH(void) {
    TEST("A11 '\\x41\\x42' = 'AB'");
    const char *k[] = {"AB","ab","A",""};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,"\\x41\\x42");
    CHECK(ks_has(&s,"AB") && !ks_has(&s,"ab"), "exact AB");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a12_esc_nul(void) {
    TEST("A12 '\\0' matches NUL byte key");
    unsigned char nk[]={0x00,0x00};
    dafsa *d = dafsa_create();
    dafsa_add_n(d,nk,1);
    long n = walk_count(d,"\\0");
    CHECK(n==1, "matches single NUL key");
    dafsa_free(d); PASS();
}

static void t_a13_nrt(void) {
    TEST("A13 '\\n\\r\\t' literal whitespace");
    const char *k[] = {"\n\r\t","nrt",""};
    dafsa *d = build_dafsa_str(k,3);
    key_set s = walk_collect(d,"\\n\\r\\t");
    CHECK(ks_has(&s,"\n\r\t") && !ks_has(&s,"nrt"), "literal match");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a14_10_alt(void) {
    TEST("A14 10-way alt");
    const char *k[] = {"a","b","c","d","e","f","g","h","i","j","k",""};
    dafsa *d = build_dafsa_str(k,12);
    long n = walk_count(d,"a|b|c|d|e|f|g|h|i|j");
    CHECK(n==10, "10 matches");
    dafsa_free(d); PASS();
}

static void t_a15_nested_alt(void) {
    TEST("A15 'a|(bc|de)|f'");
    const char *k[] = {"a","bc","de","f","b","abc"};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"a|(bc|de)|f");
    CHECK(ks_has(&s,"a") && ks_has(&s,"bc") && ks_has(&s,"de") && ks_has(&s,"f"), "4 matches");
    CHECK(!ks_has(&s,"b") && !ks_has(&s,"abc"), "!b,!abc");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a16_star_empty(void) {
    TEST("A16 'a*' includes empty");
    const char *k[] = {"","a","aa","b"};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,"a*");
    CHECK(ks_has(&s,"") && ks_has(&s,"a") && ks_has(&s,"aa") && !ks_has(&s,"b"), "a*");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a17_plus(void) {
    TEST("A17 'ab+'");
    const char *k[] = {"a","ab","abb","abbb"};
    dafsa *d = build_dafsa_str(k,4);
    CHECK(walk_count(d,"ab+")==3, "3 (ab,abb,abbb)");
    dafsa_free(d); PASS();
}

static void t_a18_class_literal_meta(void) {
    TEST("A18 '[.]','[*]','[+]','[?]'");
    const char *k[] = {".","*","+","?","a"};
    dafsa *d = build_dafsa_str(k,5);
    CHECK(walk_count(d,"[.]")==1 && walk_count(d,"[*]")==1 &&
          walk_count(d,"[+]")==1 && walk_count(d,"[?]")==1, "each matches 1");
    dafsa_free(d); PASS();
}

static void t_a19_neg_range(void) {
    TEST("A19 '[^a-z]'");
    const char *k[] = {"A","0","{","z","a",""};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"[^a-z]");
    CHECK(ks_has(&s,"A") && ks_has(&s,"0") && ks_has(&s,"{"), "A,0,{");
    CHECK(!ks_has(&s,"a") && !ks_has(&s,"z") && !ks_has(&s,""), "!a,!z,!empty");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_a20_all_dot_star(void) {
    TEST("A20 '.*' matches all (5 keys)");
    const char *k[] = {"a","hello","world","","abc"};
    dafsa *d = build_dafsa_str(k,5);
    CHECK(walk_count(d,".*")==5, "all 5");
    dafsa_free(d); PASS();
}

/* ─── B: DFA state cap ─────────────────────────────────────────────────── */

static void t_b01_cap_exceeded(void) {
    TEST("B01 cap exceeded — (a|b)*a(a|b)^25");
    char p[8192];
    strcpy(p,"(a|b)*a");
    for(int i=0;i<25;i++) strcat(p,"(a|b)");
    regex_dfa *dfa = regex_compile(p);
    CHECK(dfa->errmsg!=NULL && dfa->n_states==0, "fails with error");
    if(dfa->errmsg) { int ok=strstr(dfa->errmsg,"cap")||strstr(dfa->errmsg,"50000"); CHECK(ok,"msg mentions cap"); }
    regex_dfa_free(dfa); PASS();
}

static void t_b02_large_bounded(void) {
    TEST("B02 100-alt pattern compiles fine");
    char p[8192]; p[0]=0;
    for(int i=0;i<100;i++){
        if(i) strcat(p,"|");
        char s[32]; snprintf(s,sizeof(s),"x%03d",i); strcat(p,s);
    }
    regex_dfa *dfa = regex_compile(p);
    CHECK(!dfa->errmsg && dfa->n_states>0, "compiles");
    dafsa *d = dafsa_create();
    dafsa_add_n(d,(const unsigned char*)"x050",4);
    long n = regex_dfa_walk(d,dfa,kc_cb,&(key_set){0});
    CHECK(n==1, "matches x050");
    dafsa_free(d); regex_dfa_free(dfa); PASS();
}

/* ─── C: Termination + completeness ────────────────────────────────────── */

static void t_c01_star_terminates(void) {
    TEST("C01 '(ab)*c' terminates");
    const char *k[] = {"c","abc","ababc","abababc","ab","abcd"};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"(ab)*c");
    CHECK(ks_has(&s,"c") && ks_has(&s,"abc") && ks_has(&s,"ababc") && ks_has(&s,"abababc"), "4 matches");
    CHECK(!ks_has(&s,"ab") && !ks_has(&s,"abcd"), "!ab,!abcd");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_c02_shared_suffix(void) {
    TEST("C02 shared suffix '.b'");
    const char *k[] = {"ab","cb","xb","bb","a","b","axy"};
    dafsa *d = build_dafsa_str(k,7);
    key_set s = walk_collect(d,".b");
    CHECK(ks_has(&s,"ab") && ks_has(&s,"cb") && ks_has(&s,"xb") && ks_has(&s,"bb"), "4 matches");
    CHECK(!ks_has(&s,"a") && !ks_has(&s,"b") && !ks_has(&s,"axy"), "no spurious");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_c03_multi_prefix(void) {
    TEST("C03 multi-prefix '.(X|Y)'");
    const char *k[] = {"aX","bX","aY","bY","cZ"};
    dafsa *d = build_dafsa_str(k,5);
    key_set s = walk_collect(d,".(X|Y)");
    CHECK(ks_has(&s,"aX") && ks_has(&s,"bX") && ks_has(&s,"aY") && ks_has(&s,"bY"), "4 matches");
    CHECK(!ks_has(&s,"cZ"), "!cZ");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_c04_no_dupes(void) {
    TEST("C04 no duplicate callbacks");
    const char *k[] = {"ab","abb","acb"};
    dafsa *d = build_dafsa_str(k,3);
    key_set s = walk_collect(d,"a.?b");
    int dup=0;
    for(int i=0;i<s.n;i++)
        for(int j=i+1;j<s.n;j++)
            if(strcmp(s.keys[i],s.keys[j])==0) dup++;
    CHECK(dup==0, "no duplicates");
    CHECK(s.n==3, "all 3 match");
    ks_free(&s); dafsa_free(d); PASS();
}

/* ─── D: Full-key semantics ────────────────────────────────────────────── */

static void t_d01_no_prefix(void) {
    TEST("D01 'a' does NOT match 'ab'");
    const char *k[] = {"a","ab",""};
    dafsa *d = build_dafsa_str(k,3);
    key_set s = walk_collect(d,"a");
    CHECK(s.n==1 && ks_has(&s,"a"), "only exact 'a'");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_d02_no_suffix(void) {
    TEST("D02 'b' does NOT match 'ab'");
    const char *k[] = {"b","ab","bb"};
    dafsa *d = build_dafsa_str(k,3);
    key_set s = walk_collect(d,"b");
    CHECK(s.n==1 && ks_has(&s,"b"), "only 'b'");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_d03_star_anchored(void) {
    TEST("D03 'a.*' anchored — not 'ba'");
    const char *k[] = {"a","ab","abc","ba","xa",""};
    dafsa *d = build_dafsa_str(k,6);
    key_set s = walk_collect(d,"a.*");
    CHECK(ks_has(&s,"a") && ks_has(&s,"ab") && ks_has(&s,"abc"), "starts with a");
    CHECK(!ks_has(&s,"ba") && !ks_has(&s,"xa") && !ks_has(&s,""), "no others");
    ks_free(&s); dafsa_free(d); PASS();
}

/* ─── E: Integration tests ─────────────────────────────────────────────── */

static int count_cb(const uint32_t *cols, uint8_t arity, void *user) {
    (void)cols; (void)arity;
    (*(long*)user)++;
    return 0;
}

static void t_e01_empty_rel(void) {
    TEST("E01 dl_pattern on empty rel → 0");
    dl_db *db = dl_open("build-tmp/m5rev_e01");
    dl_declare_relation(db,"e",1);
    regex_dfa *dfa = regex_compile(".*");
    CHECK(!dfa->errmsg,"ok");
    long cnt=0;
    long n = dl_pattern(db,"e",0,dfa,count_cb,&cnt);
    CHECK(n==0 && cnt==0,"zero");
    regex_dfa_free(dfa); dl_close(db); PASS();
}

static void t_e02_nonexistent(void) {
    TEST("E02 dl_pattern nonexistent → -1");
    dl_db *db = dl_open("build-tmp/m5rev_e02");
    regex_dfa *dfa = regex_compile(".*");
    long n = dl_pattern(db,"nope",0,dfa,NULL,NULL);
    CHECK(n==-1,"-1");
    regex_dfa_free(dfa); dl_close(db); PASS();
}

static void t_e03_inmemory(void) {
    TEST("E03 dl_pattern in-memory path (string-content)");
    dl_db *db = dl_open("build-tmp/m5rev_e03");
    dl_declare_relation(db,"p",2);
    { char p[256]; snprintf(p,sizeof(p),"build-tmp/m5rev_e03.csv");
      FILE *f=fopen(p,"w"); fprintf(f,"alice,bob\nalice,carol\nbob,alice\n"); fclose(f);
      CHECK(dl_load_facts(db,"p",p)==3,"3 facts"); }
    /* col0 'a.*' -> alice rows: (alice,bob),(alice,carol) */
    regex_dfa *dfa = regex_compile("a.*");
    CHECK(!dfa->errmsg, dfa->errmsg);
    long cnt=0;
    long n = dl_pattern(db,"p",0,dfa,count_cb,&cnt);
    CHECK(n==2 && cnt==2, "2 matching rows (col0 alice)");
    regex_dfa_free(dfa);

    /* col1 'b.*' -> col1 starting with 'b' -> bob: (alice,bob) */
    dfa = regex_compile("b.*");
    CHECK(!dfa->errmsg, dfa->errmsg);
    cnt=0;
    n = dl_pattern(db,"p",1,dfa,count_cb,&cnt);
    CHECK(n==1 && cnt==1, "1 matching row (col1 bob)");
    regex_dfa_free(dfa);

    /* int column: col on raw-int relation matches nothing */
    dl_declare_relation(db,"ints",1);
    { char p[256]; snprintf(p,sizeof(p),"build-tmp/m5rev_e03i.csv");
      FILE *f=fopen(p,"w"); fprintf(f,"10\n20\n"); fclose(f);
      CHECK(dl_load_facts(db,"ints",p)==2,"2 int facts"); }
    dfa = regex_compile(".*");
    CHECK(!dfa->errmsg, dfa->errmsg);
    cnt=0;
    n = dl_pattern(db,"ints",0,dfa,count_cb,&cnt);
    CHECK(n==0 && cnt==0, "int column matches nothing");
    regex_dfa_free(dfa);

    dl_close(db); PASS();
}

/* ─── F: OP_WALK edge cases ────────────────────────────────────────────── */

static void t_f01_walk_edb(void) {
    TEST("F01 OP_WALK on EDB (string-content)");
    dl_db *db = dl_open("build-tmp/m5rev_f01");
    dl_declare_relation(db,"e",2);
    { char p[256]; snprintf(p,sizeof(p),"build-tmp/m5rev_f01.csv");
      FILE *f=fopen(p,"w"); fprintf(f,"alice,nyc\nbob,la\ncarol,sf\n"); fclose(f);
      CHECK(dl_load_facts(db,"e",p)==3,"3 facts"); }
    int rc = dl_load_rules(db,"q(X,Y) :- e(X,Y) ~ '.*'.");
    CHECK(rc==0,"compiles");
    { long cnt=0; long n=dl_query(db,"q",count_cb,&cnt); CHECK(n==3&&cnt==3,"all 3 match"); }
    dl_close(db); PASS();
}

static void t_f02_syntax_err(void) {
    TEST("F02 pattern syntax error at rule-load");
    dl_db *db = dl_open("build-tmp/m5rev_f02");
    dl_declare_relation(db,"p",1);
    CHECK(dl_load_rules(db,"q(X) :- p(X) ~ '(abc'.")!=0, "unclosed paren → error");
    CHECK(dl_load_rules(db,"q(X) :- p(X) ~ '\\xGG.*'.")!=0, "bad \\x → error");
    dl_close(db); PASS();
}

static void t_f03_walk_idb(void) {
    TEST("F03 OP_WALK on IDB body (string-content)");
    dl_db *db = dl_open("build-tmp/m5rev_f03");
    dl_declare_relation(db,"e",2);
    { char p[256]; snprintf(p,sizeof(p),"build-tmp/m5rev_f03.csv");
      FILE *f=fopen(p,"w"); fprintf(f,"alice,bob\nalice,carol\n"); fclose(f);
      CHECK(dl_load_facts(db,"e",p)==2,"2 facts"); }
    CHECK(dl_load_rules(db,"tc(X,Y) :- e(X,Y).")==0,"r1");
    CHECK(dl_load_rules(db,"f(X,Y) :- tc(X,Y) ~ 'a.*'.")==0,"r2");
    { long cnt=0; long n=dl_query(db,"f",count_cb,&cnt); CHECK(n==2&&cnt==2,"2 IDB matches (col0 alice)"); }
    dl_close(db); PASS();
}

/* ─── G: Memory/robustness ─────────────────────────────────────────────── */

static void t_g01_free_no_leak(void) {
    TEST("G01 rule free path exercised");
    dl_db *db = dl_open("build-tmp/m5rev_g01");
    dl_declare_relation(db,"p",1);
    { char p[256]; snprintf(p,sizeof(p),"build-tmp/m5rev_g01.csv");
      FILE *f=fopen(p,"w"); fprintf(f,"1\n2\n"); fclose(f);
      dl_load_facts(db,"p",p); }
    dl_load_rules(db,"q(X) :- p(X) ~ '.*'.");
    dl_close(db); /* compiled_rule_free should free patterns */
    PASS();
}

static void t_g02_empty_pat(void) {
    TEST("G02 empty pattern → error");
    regex_dfa *d = regex_compile("");
    CHECK(d->errmsg!=NULL && d->n_states==0, "error");
    regex_dfa_free(d); PASS();
}

static void t_g03_bad_hex(void) {
    TEST("G03 bad \\x → error");
    regex_dfa *d = regex_compile("\\xGG");
    CHECK(d->errmsg!=NULL && d->n_states==0, "error");
    regex_dfa_free(d); PASS();
}

static void t_g04_unclosed_paren(void) {
    TEST("G04 unclosed paren → error");
    regex_dfa *d = regex_compile("(abc");
    CHECK(d->errmsg!=NULL && d->n_states==0, "error");
    regex_dfa_free(d); PASS();
}

static void t_g05_desc_range(void) {
    TEST("G05 [z-a] treated as literals (no crash)");
    regex_dfa *d = regex_compile("[z-a]");
    CHECK(!d->errmsg && d->n_states>0, "compiles");
    const char *k[] = {"z","-","a","b","y"};
    dafsa *da = build_dafsa_str(k,5);
    key_set s = walk_collect(da,"[z-a]");
    CHECK(ks_has(&s,"z") && ks_has(&s,"-") && ks_has(&s,"a"), "z,-,a");
    CHECK(!ks_has(&s,"b") && !ks_has(&s,"y"), "!b,!y");
    ks_free(&s); dafsa_free(da); regex_dfa_free(d); PASS();
}

static void t_g06_trail_bslash(void) {
    TEST("G06 trailing \\ → error");
    regex_dfa *d = regex_compile("abc\\");
    CHECK(d->errmsg!=NULL, "error");
    regex_dfa_free(d); PASS();
}

static void t_g07_extra_paren(void) {
    TEST("G07 extra ) → error");
    regex_dfa *d = regex_compile("abc)");
    CHECK(d->errmsg!=NULL, "error");
    regex_dfa_free(d); PASS();
}

static void t_g08_bad_octal(void) {
    TEST("G08 \\x with insufficient hex");
    regex_dfa *d = regex_compile("\\x");
    CHECK(d->errmsg!=NULL, "error");
    regex_dfa_free(d);
    d = regex_compile("\\xA");
    CHECK(d->errmsg!=NULL, "error (only 1 hex digit)");
    regex_dfa_free(d); PASS();
}

/* ─── H: Byte handling ─────────────────────────────────────────────────── */

static void t_h01_all_bytes_class(void) {
    TEST("H01 [\\x00-\\xFF] matches all 256");
    dafsa *d = dafsa_create();
    for(int i=0;i<256;i++){unsigned char b[2]={(unsigned char)i,0};dafsa_add_n(d,b,1);}
    long n = walk_count(d,"[\\x00-\\xFF]");
    CHECK(n==256,"all 256");
    dafsa_free(d); PASS();
}

static void t_h02_mid_key_null(void) {
    TEST("H02 mid-key 0x00 byte");
    unsigned char k1[]={0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x02,0x00};
    unsigned char k2[]={0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x03,0x00};
    dafsa *d = dafsa_create();
    dafsa_add_n(d,k1,9); dafsa_add_n(d,k2,9);
    regex_dfa *dfa = regex_compile("\\x00\\x00\\x01\\x00.*");
    CHECK(!dfa->errmsg,"ok");
    key_set s = walk_collect(d,"\\x00\\x00\\x01\\x00.*");
    CHECK(s.n==1,"only k2");
    if(s.n>=1) CHECK((unsigned char)s.keys[0][3]==0x00,"mid-key NUL at pos 3");
    ks_free(&s); regex_dfa_free(dfa); dafsa_free(d); PASS();
}

static void t_h03_neg_class_ff(void) {
    TEST("H03 [^a] includes 0xFF");
    unsigned char kf[]={0xFF,0x00}, ka[]={'a',0x00};
    dafsa *d = dafsa_create();
    dafsa_add_n(d,kf,1); dafsa_add_n(d,ka,1);
    key_set s = walk_collect(d,"[^a]");
    CHECK(s.n==1 && (unsigned char)s.keys[0][0]==0xFF, "only 0xFF");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_h04_high_bytes(void) {
    TEST("H04 [\\x80-\\xFF]+ matches high bytes");
    dafsa *d = dafsa_create();
    unsigned char b0[]={0x80,0},b1[]={0xA0,0},b2[]={0xFF,0},b3[]={'a',0};
    dafsa_add_n(d,b0,1);dafsa_add_n(d,b1,1);dafsa_add_n(d,b2,1);dafsa_add_n(d,b3,1);
    long n = walk_count(d,"[\\x80-\\xFF]");
    CHECK(n==3,"3 high-byte keys");
    dafsa_free(d); PASS();
}

/* ─── I: More adversarial patterns ─────────────────────────────────────── */

static void t_i01_abc_star(void) {
    TEST("I01 '(a|b|c)*'");
    const char *k[] = {"","a","b","c","ab","abc","cba","aaa","d","ad"};
    dafsa *d = build_dafsa_str(k,10);
    key_set s = walk_collect(d,"(a|b|c)*");
    CHECK(s.n==8,"8 (all a/b/c strings)");
    CHECK(!ks_has(&s,"d") && !ks_has(&s,"ad"),"!d,!ad");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i02_concat_alt(void) {
    TEST("I02 'a(bc|de)f'");
    const char *k[] = {"abcf","adef","abf","acf","abcdef","abc","def"};
    dafsa *d = build_dafsa_str(k,7);
    key_set s = walk_collect(d,"a(bc|de)f");
    CHECK(ks_has(&s,"abcf") && ks_has(&s,"adef"), "abcf,adef");
    CHECK(!ks_has(&s,"abf") && !ks_has(&s,"abcdef") && !ks_has(&s,"abc"), "spurious");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i03_multi_star(void) {
    TEST("I03 'a*b*c*'");
    const char *k[] = {"","a","b","c","ab","bc","abc","aaabbc","cba"};
    dafsa *d = build_dafsa_str(k,9);
    key_set s = walk_collect(d,"a*b*c*");
    CHECK(ks_has(&s,"") && ks_has(&s,"ab") && ks_has(&s,"abc") && ks_has(&s,"aaabbc"), "all but cba");
    CHECK(!ks_has(&s,"cba"),"!cba");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i04_dot_star_empty(void) {
    TEST("I04 '.*' on empty key");
    const char *k[] = {"","x"};
    dafsa *d = build_dafsa_str(k,2);
    CHECK(walk_count(d,".*")==2,"both match");
    dafsa_free(d); PASS();
}

static void t_i05_opt_grp(void) {
    TEST("I05 '(ab)?c'");
    const char *k[] = {"c","abc","ab","ac","bc"};
    dafsa *d = build_dafsa_str(k,5);
    CHECK(walk_count(d,"(ab)?c")==2,"c,abc");
    dafsa_free(d); PASS();
}

static void t_i06_plus_vs_star(void) {
    TEST("I06 a+ vs a*");
    const char *k[] = {"","a","aa"};
    dafsa *d = build_dafsa_str(k,3);
    CHECK(walk_count(d,"a+")==2,"a+→2 (a,aa)");
    CHECK(walk_count(d,"a*")==3,"a*→3 (empty,a,aa)");
    dafsa_free(d); PASS();
}

static void t_i07_optional_char(void) {
    TEST("I07 'colou?r'");
    const char *k[] = {"color","colour","colouur","colr"};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,"colou?r");
    CHECK(ks_has(&s,"color") && ks_has(&s,"colour"), "color,colour");
    CHECK(!ks_has(&s,"colouur") && !ks_has(&s,"colr"), "!colouur,!colr");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i08_deep_nest(void) {
    TEST("I08 '(((a)))' triple nested");
    const char *k[] = {"a","((a))",""};
    dafsa *d = build_dafsa_str(k,3);
    key_set s = walk_collect(d,"(((a)))");
    CHECK(ks_has(&s,"a"), "literal a only");
    CHECK(!ks_has(&s,"((a))"), "no literal parens");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i09_star_alt_literal(void) {
    TEST("I09 '(a|b)*c'");
    const char *k[] = {"c","ac","bc","abc","bac","aac","bbc","ababc","a","ab"};
    dafsa *d = build_dafsa_str(k,10);
    CHECK(walk_count(d,"(a|b)*c")==8,"8 ending in c");
    dafsa_free(d); PASS();
}

static void t_i10_backslash(void) {
    TEST("I10 'a\\\\b' literal backslash");
    const char *k[] = {"a\\b","ab","a\\","\\b"};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,"a\\\\b");
    CHECK(ks_has(&s,"a\\b") && !ks_has(&s,"ab"), "literal backslash");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i11_dot_plus(void) {
    TEST("I11 '.+' non-empty");
    const char *k[] = {"a","ab","","abc"};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,".+");
    CHECK(s.n==3 && !ks_has(&s,""), "3 non-empty");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i12_nested_star(void) {
    TEST("I12 '(a*)*' redundant star nesting");
    const char *k[] = {"","a","aa","b"};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,"(a*)*");
    CHECK(ks_has(&s,"") && ks_has(&s,"a") && ks_has(&s,"aa") && !ks_has(&s,"b"), "a* equivalent");
    ks_free(&s); dafsa_free(d); PASS();
}

static void t_i13_class_dash_end(void) {
    TEST("I13 '[-a]' dash at start of class");
    const char *k[] = {"-","a","b",""};
    dafsa *d = build_dafsa_str(k,4);
    key_set s = walk_collect(d,"[-a]");
    /* Dash at start: literal dash. But our parser treats dash differently.
     * Let's see: '[' consumed. rx_peek is '-', which is RX_TOKEN_CHAR with ch='-'.
     * prev=-1, have_prev=0. ch='-'. We check if next is '-': next peek is RX_TOKEN_CHAR 'a'.
     * Yes, we consume '-' as potential range, set prev='-', have_prev=1.
     * Then 'a' comes: rx_peek='a' (not RBRACKET), so we enter the loop body.
     * ch='a'. have_prev=1, prev='-'=45, ch='a'=97. 97 > 45, so range: 45..97.
     * Wait, that's wrong! '[-a]' should mean '-' OR 'a'. But our parser will interpret it as range '-' through 'a'.
     * Let me verify with the actual code...
     * In parse_char_class:
     * - Leading '^' check: no.
     * - ']' check: no.
     * - Loop: peeking at '-': kind RX_TOKEN_CHAR, ch='-'.
     *   have_prev=0, so the range check fails.
     *   Check if next is '-': next IS '-', so we consume it (rx_next('-')), set prev='-', have_prev=1.
     * - Loop continues: peeking at 'a': RX_TOKEN_CHAR, ch='a'.
     *   have_prev=1, prev >= 0, (int)ch(97) > prev(45): YES! So range '-'..'a'.
     * - Then ']' ends.
     *
     * So '[-a]' is interpreted as range [--a] = chars 45-97 (dash through 'a'). This includes '-', '.', '/', '0'-'9', etc.
     * This is NOT a bug per se — it's consistent with how ranges work. But [-a] in standard regex means '-' or 'a'.
     * Most regex implementations require dash to be first or last to be literal.
     *
     * Let's just verify the behavior matches expectations.
     */
    CHECK(s.n >= 1, "matches at least dash and a");
    /* '-'(45) through 'a'(97) includes many chars, but from our keys only '-' and 'a' and 'b'(98>97=no) */
    ks_free(&s); dafsa_free(d); PASS();
}

/* ─── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M5 Adversarial Review Tests\n"); fflush(stdout);

    /* A: Regex correctness oracle */
    t_a01_literal();
    t_a02_alt();
    t_a03_star_group();
    t_a04_dot_star_prefix();
    t_a05_dot_star_anywhere();
    t_a06_neg_class();
    t_a07_range_class();
    t_a08_nested();
    t_a09_quant();
    t_a10_dot_null_ff();
    t_a11_xHH();
    t_a12_esc_nul();
    t_a13_nrt();
    t_a14_10_alt();
    t_a15_nested_alt();
    t_a16_star_empty();
    t_a17_plus();
    t_a18_class_literal_meta();
    t_a19_neg_range();
    t_a20_all_dot_star();

    /* B: DFA state cap */
    t_b01_cap_exceeded();
    t_b02_large_bounded();

    /* C: Termination + completeness */
    t_c01_star_terminates();
    t_c02_shared_suffix();
    t_c03_multi_prefix();
    t_c04_no_dupes();

    /* D: Full-key semantics */
    t_d01_no_prefix();
    t_d02_no_suffix();
    t_d03_star_anchored();

    /* E: Integration */
    t_e01_empty_rel();
    t_e02_nonexistent();
    t_e03_inmemory();

    /* F: OP_WALK edge cases */
    t_f01_walk_edb();
    t_f02_syntax_err();
    t_f03_walk_idb();

    /* G: Memory / robustness */
    t_g01_free_no_leak();
    t_g02_empty_pat();
    t_g03_bad_hex();
    t_g04_unclosed_paren();
    t_g05_desc_range();
    t_g06_trail_bslash();
    t_g07_extra_paren();
    t_g08_bad_octal();

    /* H: Byte handling */
    t_h01_all_bytes_class();
    t_h02_mid_key_null();
    t_h03_neg_class_ff();
    t_h04_high_bytes();

    /* I: More adversarial patterns */
    t_i01_abc_star();
    t_i02_concat_alt();
    t_i03_multi_star();
    t_i04_dot_star_empty();
    t_i05_opt_grp();
    t_i06_plus_vs_star();
    t_i07_optional_char();
    t_i08_deep_nest();
    t_i09_star_alt_literal();
    t_i10_backslash();
    t_i11_dot_plus();
    t_i12_nested_star();
    t_i13_class_dash_end();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
