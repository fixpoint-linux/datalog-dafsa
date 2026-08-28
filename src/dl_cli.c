/*
 * dl_cli.c — CLI for the DAFSA-backed deductive database (M0+M1)
 *
 * Commands:
 *   dl load <csv> --rel <name>    Load facts from CSV into a relation
 *   dl lookup <rel> <v1> <v2>...  Exact lookup
 *   dl prefix <rel> [<v1> ...]    Prefix enumeration
 *   dl query <rule-or-file>       Parse, compile, run a Datalog rule
 *
 * Values that parse as integers are raw u32; everything else is interned.
 * The DB directory defaults to /tmp/dl-test-db or can be set via -d <dir>.
 *
 * M0 hack: reaches into the opaque dl_db to access the interner directly.
 * The dl_db struct layout is known from dl.c.  This will be replaced by
 * proper typed-value support in M1+.
 */

#include "dl.h"
#include "dl_internal.h"
#include "intern.h"
#include "snapshot.h"
#include "regexwalk.h"
#include "index.h"
#include "vector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *DB_DIR = "dl-test-db";

/* Directory of this binary (set in main): used to locate ./dl-embed for the
 * query-encode path, so vsearch works regardless of the caller's cwd. */
static char g_prog_dir[PATH_MAX] = ".";

/* Strictly parse a decimal string as an unsigned 32-bit value.  Rejects
 * empty/whitespace input, trailing garbage, a negative sign, and overflow —
 * unlike a bare strtoul (which parses "abc" as 0 and truncates silently). */
static int parse_u32_strict(const char *s, unsigned long *out)
{
    char *end = NULL;
    unsigned long v;

    if (!s || !*s) return 0;
    if (*s == '-') return 0;                    /* no negatives */
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno == ERANGE) return 0;              /* overflow */
    if (!end || *end != '\0') return 0;         /* trailing garbage */
    if (v > 0xFFFFFFFFUL) return 0;             /* exceeds u32 */
    *out = v;
    return 1;
}

/* Reach into the opaque dl_db handle via the shared internal layout
 * (dl_internal.h) to access the interner for CLI value parsing. */
static interner *cli_get_interner(dl_db *db)
{
    return db->ir;
}

/* Parse a CLI value: integer -> raw u32, else intern -> sym_id */
static uint32_t cli_parse_value(dl_db *db, const char *s)
{
    interner *ir = cli_get_interner(db);
    const char *p = s;
    int is_int = 1;

    if (!*s) return 0;
    while (*p) {
        if (*p < '0' || *p > '9') { is_int = 0; break; }
        p++;
    }
    if (is_int) {
        unsigned long v = strtoul(s, NULL, 10);
        if (v > 0xFFFFFFFFUL) {
            fprintf(stderr, "dl: integer overflow: %s\n", s);
            exit(1);
        }
        return (uint32_t)v;
    }
    return intern_str(ir, s);
}

/* Parse a concatenated hex string into an array of u32 values.
 * hex_str is a string of hex digits (no spaces, no 0x prefix).
 * Returns the number of u32 values parsed, or -1 on error.
 * n_words is the expected number of u32 values (e.g., VEC_SIG_WORDS=8, VEC_IVEC_WORDS=96). */
static int parse_hex_words(const char *hex_str, uint32_t *words, int n_words)
{
    size_t hex_len = strlen(hex_str);
    if (hex_len != (size_t)(n_words * 8)) {
        return -1;  /* Each u32 is 8 hex digits */
    }
    
    for (int i = 0; i < n_words; i++) {
        char buf[9];
        memcpy(buf, hex_str + i * 8, 8);
        buf[8] = '\0';
        if (sscanf(buf, "%8x", &words[i]) != 1) {
            return -1;
        }
    }
    return n_words;
}

/* Run the dl-embed encode helper: fork + execv (NO shell — the query is
 * passed as a single argv element, so no shell-quoting/injection surface
 * exists by construction).  Fills q_sig (VEC_SIG_WORDS) and q_int8
 * (VEC_IVEC_WORDS) from the helper's "sig_hex ivec_hex" stdout line.
 * Returns 0 on success, -1 on any failure (message printed). */
static int run_encode_helper(const char *db_dir, const char *query,
                             uint32_t *q_sig, uint32_t *q_int8)
{
    char helper[PATH_MAX + 16];
    int fds[2];
    pid_t pid;

    /* resolve dl-embed next to this binary, then ./dl-embed */
    snprintf(helper, sizeof helper, "%s/dl-embed", g_prog_dir);
    if (access(helper, X_OK) != 0) {
        snprintf(helper, sizeof helper, "./dl-embed");
        if (access(helper, X_OK) != 0) {
            fprintf(stderr, "dl: dl-embed not found (build it: make dl-embed)\n");
            return -1;
        }
    }

    if (pipe(fds) != 0) {
        fprintf(stderr, "dl: pipe failed\n");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        fprintf(stderr, "dl: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        char *cargv[6];
        int i = 0;
        cargv[i++] = (char *)"dl-embed";
        cargv[i++] = (char *)"encode";
        cargv[i++] = (char *)"--db";
        cargv[i++] = (char *)db_dir;
        cargv[i++] = (char *)query;
        cargv[i] = NULL;
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[0]);
        close(fds[1]);
        execv(helper, cargv);
        _exit(127);
    }
    close(fds[1]);
    {
        char sig_buf[65], ivec_buf[769];
        FILE *fp = fdopen(fds[0], "r");
        int status = 0, ok = 0;
        if (!fp) {
            close(fds[0]);
            waitpid(pid, &status, 0);
            fprintf(stderr, "dl: fdopen failed\n");
            return -1;
        }
        if (fscanf(fp, "%64s %768s", sig_buf, ivec_buf) == 2) {
            if (parse_hex_words(sig_buf, q_sig, VEC_SIG_WORDS) == VEC_SIG_WORDS &&
                parse_hex_words(ivec_buf, q_int8, VEC_IVEC_WORDS) == VEC_IVEC_WORDS)
                ok = 1;
        }
        fclose(fp);
        if (waitpid(pid, &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            ok = 0;
        if (!ok) {
            fprintf(stderr, "dl: encode helper failed\n");
            return -1;
        }
    }
    return 0;
}

/* Callback to collect candidate sym-ids from vector search */
struct vec_cand_collector {
    uint32_t *syms;
    int capacity;
    int count;
};

static int collect_vec_cand(uint32_t entity_sym, int score, void *user)
{
    struct vec_cand_collector *c = (struct vec_cand_collector *)user;
    (void)score;
    if (c->count < c->capacity) {
        c->syms[c->count++] = entity_sym;
    }
    return 0;
}

/* Callback to print ranked results from rerank */
struct vec_result_printer {
    dl_db *db;
    int printed;
};

/* Forward declarations: print_value / print_list are defined below but used
 * by the rerank callback above. */
static void print_value(dl_db *db, uint32_t v, int depth);

static int print_vec_result(uint32_t entity_sym, int score, void *user)
{
    struct vec_result_printer *p = (struct vec_result_printer *)user;
    (void)score;
    print_value(p->db, entity_sym, 0);
    printf("\n");
    p->printed++;
    return 0;
}

/* Ascending u32 comparison (for qsort/bsearch on sym-id sets). */
static int cmp_u32(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

/* Callback: capture the entity sym (col 0) of the observation row whose
 * content (col 1) equals the target obs_id. */
struct obs_entity_ctx {
    uint32_t target;
    uint32_t entity;
};

static int collect_obs_entity(const uint32_t *cols, uint8_t arity, void *user)
{
    struct obs_entity_ctx *c = (struct obs_entity_ctx *)user;
    (void)arity;
    if (c->target && cols[1] == c->target) {
        c->entity = cols[0];
        return 1;   /* stop: obs_id is unique in observation */
    }
    return 0;
}

/* Map a lexical obs_id (content sym) to the entity sym that owns it, via the
 * observation(entity, content) relation.  Returns 0 if the observation
 * relation is absent or the obs_id has no row. */
static uint32_t obs_to_entity(dl_db *db, uint32_t obs_id)
{
    struct obs_entity_ctx c = { obs_id, 0 };
    long n = dl_prefix(db, "observation", NULL, 0, collect_obs_entity, &c);
    if (n < 0)
        return 0;
    return c.entity;
}

/* Print a column value: resolve list handles (EXACT, first), then string
 * symbols (reverse-map heuristic), then raw ints.  Lists are first so list
 * printing is exact even though int-vs-symbol stays heuristic (B6). */
static void print_list(dl_db *db, uint32_t h, int depth);

static void print_value(dl_db *db, uint32_t v, int depth)
{
    interner *ir = cli_get_interner(db);
    if (term_is_list(db->terms, v)) {
        print_list(db, v, depth);
        return;
    }
    {
        const char *s = intern_str_of(ir, v);
        if (s && *s) {
            printf("%s", s);
        } else {
            printf("%u", v);
        }
    }
}

/* Render a list handle recursively.  The DAG is well-founded (acyclic), so
 * this terminates; the depth cap is a defensive bound against a pathological
 * long list overflowing the CLI stack. */
static void print_list(dl_db *db, uint32_t h, int depth)
{
    if (depth > 4096) { printf("[...]"); return; }
    printf("[");
    while (h != TERM_NIL) {
        print_value(db, term_car(db->terms, h), depth + 1);
        h = term_cdr(db->terms, h);
        if (h != TERM_NIL) printf(", ");
    }
    printf("]");
}

/* Callback for dl_prefix: print one tuple */
static int print_tuple(const uint32_t *cols, uint8_t arity, void *user)
{
    dl_db *db = (dl_db *)user;
    uint8_t i;
    for (i = 0; i < arity; i++) {
        if (i > 0) printf(" ");
        print_value(db, cols[i], 0);
    }
    printf("\n");
    return 0;
}

/* Callback for dl_prefix --raw: print one tuple as raw u32 columns (no symbol
 * reverse-mapping).  Machine-consumable (used by the S4 oracle/embed parsers
 * which need the exact packed ints). */
static int print_tuple_raw(const uint32_t *cols, uint8_t arity, void *user)
{
    uint8_t i;
    (void)user;
    for (i = 0; i < arity; i++) {
        if (i > 0) printf(" ");
        printf("%u", cols[i]);
    }
    printf("\n");
    return 0;
}

/* Callback for traverse: print node name */
static int traverse_print_cb(uint32_t node_sym, uint8_t depth, void *user)
{
    (void)depth;
    dl_db *db = (dl_db *)user;
    interner *ir = cli_get_interner(db);
    const char *s = intern_str_of(ir, node_sym);
    if (s && *s) {
        printf("%s\n", s);
    } else {
        printf("%u\n", node_sym);
    }
    return 0;
}

/* Callback for obs: print observation string */
static int obs_print_cb(const char *s, void *user)
{
    (void)user;
    printf("%s\n", s);
    return 0;
}

/* ─── Usage ───────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [-d <dir>] load <csv> --rel <name>\n"
        "  %s [-d <dir>] lookup <rel> <val> [<val> ...]\n"
        "  %s [-d <dir>] prefix <rel> [<val> ...]\n"
        "  %s [-d <dir>] query '<rule>' | <file.dl> <goal-rel>\n"
        "  %s [-d <dir>] qmagic '<rule>' | <file.dl> <goal-rel> [-a <adorn>] <val> [<val> ...]\n"
        "  %s [-d <dir>] publish [--keep N]\n"
        "  %s [-d <dir>] bound <rel> <val> [<val> ...]\n"
        "  %s [-d <dir>] pattern <rel> [<col>] '<regex>'\n"
        "  %s [-d <dir>] rev <entity>\n"
        "  %s [-d <dir>] cas <entity> <expected> <new>\n"
        "  %s [-d <dir>] txn\n"
        "  %s [-d <dir>] traverse <start> [depth] [--max-nodes N]\n"
        "  %s [-d <dir>] obs <node> [--max-obs N]\n"
        "  %s [-d <dir>] index\n"
        "  %s [-d <dir>] search '<terms>' [--top N] [--version N] (--version 0 = current)\n"
        "  %s [-d <dir>] vsearch '<query>' [--k N] [--radius R] [--version V] [--sig <hex64>] [--ivec <hex768>]\n"
        "  %s [-d <dir>] vhybrid '<terms>' '<query>' [--k N] [--radius R] [--version V]\n"
        "  %s [-d <dir>] versions\n"
        "Values: bare integer -> raw u32; anything else -> interned string\n"
        "Hex args: concatenated hex digits (no spaces, no 0x prefix)\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
    exit(1);
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *db_dir = DB_DIR;
    const char *cmd;
    dl_db *db;
    int argp = 1;

    if (argc < 2) usage(argv[0]);

    /* record this binary's directory for the dl-embed helper lookup */
    {
        size_t n = strlen(argv[0]);
        if (n >= sizeof g_prog_dir) n = sizeof g_prog_dir - 1;
        memcpy(g_prog_dir, argv[0], n);
        g_prog_dir[n] = '\0';
        char *slash = strrchr(g_prog_dir, '/');
        if (slash) *slash = '\0';
        else snprintf(g_prog_dir, sizeof g_prog_dir, ".");
    }

    /* Parse -d <dir> */
    if (argp < argc && strcmp(argv[argp], "-d") == 0) {
        argp++;
        if (argp >= argc) usage(argv[0]);
        db_dir = argv[argp++];
    }

    if (argp >= argc) usage(argv[0]);
    cmd = argv[argp++];

    db = dl_open(db_dir);
    if (!db) {
        fprintf(stderr, "dl: cannot open database at %s\n", db_dir);
        return 1;
    }

    if (strcmp(cmd, "load") == 0) {
        const char *csv_path, *rel_name = NULL;

        if (argp >= argc) usage(argv[0]);
        csv_path = argv[argp++];

        while (argp < argc) {
            if (strcmp(argv[argp], "--rel") == 0 && argp + 1 < argc) {
                argp++;
                rel_name = argv[argp++];
            } else {
                usage(argv[0]);
            }
        }

        if (!rel_name) usage(argv[0]);

        /* Peek at the first non-empty CSV line to determine arity */
        {
            FILE *f = fopen(csv_path, "r");
            char *line = NULL;
            size_t cap = 0;
            ssize_t len;
            int arity = 0;

            if (!f) {
                fprintf(stderr, "dl: cannot open %s\n", csv_path);
                dl_close(db);
                return 1;
            }

            while ((len = getline(&line, &cap, f)) > 0) {
                char *p = line;
                int in_quote = 0;
                if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
                if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
                if (len == 0) continue;

                arity = 1;
                while (*p) {
                    if (*p == '"') in_quote = !in_quote;
                    if (*p == ',' && !in_quote) arity++;
                    p++;
                }
                break;
            }
            free(line);
            fclose(f);

            if (arity == 0 || arity > 8) {
                fprintf(stderr, "dl: could not determine arity from %s\n",
                        csv_path);
                dl_close(db);
                return 1;
            }

            if (dl_declare_relation(db, rel_name, (uint8_t)arity) != 0) {
                fprintf(stderr, "dl: cannot declare relation %s/%d\n",
                        rel_name, arity);
                dl_close(db);
                return 1;
            }
        }

        {
            int loaded = dl_load_facts(db, rel_name, csv_path);
            if (loaded < 0) {
                fprintf(stderr, "dl: load failed\n");
                dl_close(db);
                return 1;
            }
            printf("Loaded %d facts into %s\n", loaded, rel_name);
        }

    } else if (strcmp(cmd, "lookup") == 0) {
        const char *rel_name;
        uint32_t cols[8];
        uint8_t arity;

        if (argp >= argc) usage(argv[0]);
        rel_name = argv[argp++];

        arity = 0;
        while (argp < argc && arity < 8) {
            cols[arity] = cli_parse_value(db, argv[argp]);
            arity++;
            argp++;
        }

        if (arity == 0) usage(argv[0]);

        {
            int found = dl_lookup(db, rel_name, cols, arity);
            printf("%s\n", found ? "found" : "not found");
        }

    } else if (strcmp(cmd, "prefix") == 0) {
        const char *rel_name;
        uint32_t leading[8];
        uint8_t k;
        int raw = 0;

        if (argp >= argc) usage(argv[0]);
        rel_name = argv[argp++];

        if (argp < argc && strcmp(argv[argp], "--raw") == 0) {
            raw = 1;
            argp++;
        }

        k = 0;
        while (argp < argc && k < 8) {
            leading[k] = cli_parse_value(db, argv[argp]);
            k++;
            argp++;
        }

        {
            long n = dl_prefix(db, rel_name, leading, k,
                               raw ? print_tuple_raw : print_tuple, db);
            if (n < 0) {
                fprintf(stderr, "dl: prefix query failed\n");
                dl_close(db);
                return 1;
            }
            if (n == 0)
                printf("(no results)\n");
        }

    } else if (strcmp(cmd, "query") == 0) {
        const char *source;
        const char *goal_rel;
        char *source_buf = NULL;
        long n;

        if (argp >= argc) usage(argv[0]);
        source = argv[argp++];

        if (argp >= argc) usage(argv[0]);
        goal_rel = argv[argp++];

        /* Check if source is a file path */
        {
            FILE *f = fopen(source, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 1024 * 1024) {
                    source_buf = malloc((size_t)sz + 1);
                    if (source_buf) {
                        size_t nr = fread(source_buf, 1, (size_t)sz, f);
                        source_buf[nr] = '\0';
                        source = source_buf;
                    }
                }
                fclose(f);
            }
        }

        if (dl_load_rules(db, source) != 0) {
            fprintf(stderr, "dl: failed to parse/compile rules\n");
            free(source_buf);
            dl_close(db);
            return 1;
        }
        free(source_buf);

        /* Publish (runs VM automatically if fixpoint_dirty), then query */
        if (dl_publish_snapshot(db) != 0) {
            fprintf(stderr, "dl: publish failed\n");
            dl_close(db);
            return 1;
        }

        n = dl_query(db, goal_rel, print_tuple, db);
        if (n < 0) {
            fprintf(stderr, "dl: query failed\n");
            dl_close(db);
            return 1;
        }
        if (n == 0)
            printf("(no results)\n");

    } else if (strcmp(cmd, "qmagic") == 0) {
        const char *source;
        const char *goal_rel;
        const char *adorn = NULL;
        char *source_buf = NULL;
        uint32_t vals[8];
        uint8_t nvals;
        long n;

        if (argp >= argc) usage(argv[0]);
        source = argv[argp++];

        if (argp >= argc) usage(argv[0]);
        goal_rel = argv[argp++];

        /* Optional -a <adorn>: arbitrary-adornment form (e.g. "fb" binds
         * position 1).  Without it, the legacy leading-prefix form is used. */
        if (argp < argc && strcmp(argv[argp], "-a") == 0) {
            argp++;
            if (argp >= argc) usage(argv[0]);
            adorn = argv[argp++];
        }

        /* Check if source is a file path */
        {
            FILE *f = fopen(source, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 1024 * 1024) {
                    source_buf = malloc((size_t)sz + 1);
                    if (source_buf) {
                        size_t nr = fread(source_buf, 1, (size_t)sz, f);
                        source_buf[nr] = '\0';
                        source = source_buf;
                    }
                }
                fclose(f);
            }
        }

        nvals = 0;
        while (argp < argc && nvals < 8) {
            vals[nvals] = cli_parse_value(db, argv[argp]);
            nvals++;
            argp++;
        }

        if (dl_load_rules(db, source) != 0) {
            fprintf(stderr, "dl: failed to parse/compile rules\n");
            free(source_buf);
            dl_close(db);
            return 1;
        }
        free(source_buf);

        if (adorn)
            n = dl_query_magic_adorn(db, goal_rel, adorn, vals, nvals,
                                     print_tuple, db);
        else
            n = dl_query_magic(db, goal_rel, vals, nvals, print_tuple, db);
        if (n < 0) {
            fprintf(stderr, "dl: magic query failed\n");
            dl_close(db);
            return 1;
        }
        if (n == 0)
            printf("(no results)\n");

    } else if (strcmp(cmd, "publish") == 0) {
        /* Optional --keep N: retain only the N most-recent snapshots, pruning
         * older ones after this publish (opt-in; 0 disables retention). */
        unsigned keep = 0;
        if (argp < argc) {
            if (argc - argp == 2 && strcmp(argv[argp], "--keep") == 0) {
                keep = (unsigned)strtoul(argv[argp + 1], NULL, 10);
                argp += 2;
            } else {
                usage(argv[0]);
            }
        }
        dl_set_snapshot_retain(db, keep);
        if (dl_publish_snapshot(db) != 0) {
            fprintf(stderr, "dl: publish failed\n");
            dl_close(db);
            return 1;
        }
        printf("Snapshot published.\n");

    } else if (strcmp(cmd, "bound") == 0) {
        const char *rel_name;
        uint32_t leading[8];
        uint8_t k;

        if (argp >= argc) usage(argv[0]);
        rel_name = argv[argp++];

        k = 0;
        while (argp < argc && k < 8) {
            leading[k] = cli_parse_value(db, argv[argp]);
            k++;
            argp++;
        }

        {
            long n = dl_query_bound(db, rel_name, leading, k,
                                    print_tuple, db);
            if (n < 0) {
                fprintf(stderr, "dl: bound query failed\n");
                dl_close(db);
                return 1;
            }
            if (n == 0)
                printf("(no results)\n");
        }


    } else if (strcmp(cmd, "pattern") == 0) {
        const char *rel_name;
        uint8_t col = 0;
        const char *pattern;

        if (argp >= argc) usage(argv[0]);
        rel_name = argv[argp++];

        /* Optional column argument */
        if (argp < argc) {
            char *endptr;
            long col_val = strtol(argv[argp], &endptr, 10);
            if (*endptr == 0 && col_val >= 0 && col_val <= 255) {
                col = (uint8_t)col_val;
                argp++;
            }
        }

        if (argp >= argc) usage(argv[0]);
        pattern = argv[argp++];

        {
            regex_dfa *dfa = regex_compile(pattern);
            if (dfa->errmsg) {
                fprintf(stderr, "dl: bad pattern\n");
                regex_dfa_free(dfa);
                dl_close(db);
                return 1;
            }

            long n = dl_pattern(db, rel_name, col, dfa, print_tuple, db);
            regex_dfa_free(dfa);
            if (n < 0) {
                fprintf(stderr, "dl: pattern query failed\n");
                dl_close(db);
                return 1;
            }
            if (n == 0)
                printf("(no results)\n");
        }

    } else if (strcmp(cmd, "rev") == 0) {
        const char *entity;
        uint32_t r;

        if (argp >= argc) usage(argv[0]);
        entity = argv[argp++];

        if (dl_rev_get(db, entity, &r) != 0) {
            fprintf(stderr, "dl: rev lookup failed\n");
            dl_close(db);
            return 1;
        }
        printf("%u\n", r);

    } else if (strcmp(cmd, "cas") == 0) {
        const char *entity;
        const char *exp_s, *new_s;
        unsigned long expected, new_value;
        int rc;

        if (argp + 2 >= argc) usage(argv[0]);
        entity = argv[argp++];
        exp_s = argv[argp++];
        new_s = argv[argp++];

        /* Strict unsigned-32 parsing: reject non-numeric input and overflow
         * instead of silently truncating via strtoul (which parses garbage as
         * 0, making `dl cas e abc def` look like a conflict). */
        if (!parse_u32_strict(exp_s, &expected) ||
            !parse_u32_strict(new_s, &new_value)) {
            fprintf(stderr, "dl: cas: expected and new must be unsigned 32-bit integers\n");
            dl_close(db);
            return 1;
        }

        rc = dl_cas_revision(db, entity, (uint32_t)expected,
                             (uint32_t)new_value);
        if (rc == 0) {
            printf("ok\n");
        } else if (rc == DL_E_CONFLICT) {
            printf("conflict\n");
        } else {
            fprintf(stderr, "dl: cas error\n");
            dl_close(db);
            return 1;
        }

    } else if (strcmp(cmd, "txn") == 0) {
        /* Fixed demo: begin, CAS demo-entity 0->1, add a fact on a small
         * declared relation, commit; print the result. */
        uint32_t sym_hello, sym_world;
        uint32_t cols[2];
        int rc;

        if (dl_declare_relation(db, "tnotes", 2) != 0) {
            fprintf(stderr, "dl: cannot declare relation tnotes\n");
            dl_close(db);
            return 1;
        }
        sym_hello = dl_intern_str(db, "hello");
        sym_world = dl_intern_str(db, "world");
        cols[0] = sym_hello; cols[1] = sym_world;

        if (dl_txn_begin(db) != 0) {
            fprintf(stderr, "dl: txn_begin failed\n");
            dl_close(db);
            return 1;
        }
        if (dl_txn_cas(db, "demo-entity", 0, 1) != 0 ||
            dl_txn_add_fact(db, "tnotes", cols, 2) != 0) {
            fprintf(stderr, "dl: txn buffer failed\n");
            dl_txn_rollback(db);
            dl_close(db);
            return 1;
        }
        rc = dl_txn_commit(db);
        if (rc == 0) {
            printf("txn committed\n");
        } else if (rc == DL_E_CONFLICT) {
            printf("txn conflict\n");
        } else {
            fprintf(stderr, "dl: txn commit error\n");
            dl_close(db);
            return 1;
        }

    } else if (strcmp(cmd, "traverse") == 0) {
        const char *start;
        int depth = 1;
        unsigned long max_nodes = 1000;

        if (argp >= argc) usage(argv[0]);
        start = argv[argp++];

        /* Optional [depth] then optional [--max-nodes N].  A malformed
         * numeric arg is an error, not a silent fallback to depth 1. */
        if (argp < argc && argv[argp][0] == '-' &&
            strncmp(argv[argp], "--max-nodes", 11) != 0) {
            fprintf(stderr, "dl: invalid argument '%s'\n", argv[argp]);
            dl_close(db);
            return 1;
        }
        if (argp < argc && strncmp(argv[argp], "--max-nodes", 11) != 0) {
            unsigned long d;
            if (!parse_u32_strict(argv[argp], &d) || d < 1 || d > 3) {
                fprintf(stderr, "dl: invalid depth '%s' (expect 1..3)\n",
                        argv[argp]);
                dl_close(db);
                return 1;
            }
            depth = (int)d;
            argp++;
        }
        if (argp < argc && strcmp(argv[argp], "--max-nodes") == 0) {
            if (argp + 1 >= argc ||
                !parse_u32_strict(argv[argp + 1], &max_nodes) ||
                max_nodes < 1) {
                fprintf(stderr, "dl: invalid --max-nodes value\n");
                dl_close(db);
                return 1;
            }
            argp += 2;
        }
        if (argp < argc) {
            fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
            dl_close(db);
            return 1;
        }

        {
            long n = dl_traverse(db, start, depth, (int)max_nodes,
                                  traverse_print_cb, db);
            if (n < 0) {
                fprintf(stderr, "dl: traverse failed\n");
                dl_close(db);
                return 1;
            }
            if (n == 0)
                printf("(no results)\n");
        }

    } else if (strcmp(cmd, "obs") == 0) {
        const char *node;
        unsigned long max_obs = 100;

        if (argp >= argc) usage(argv[0]);
        node = argv[argp++];

        if (argp < argc && strcmp(argv[argp], "--max-obs") == 0) {
            if (argp + 1 >= argc ||
                !parse_u32_strict(argv[argp + 1], &max_obs) ||
                max_obs < 1) {
                fprintf(stderr, "dl: invalid --max-obs value\n");
                dl_close(db);
                return 1;
            }
            argp += 2;
        }
        if (argp < argc) {
            fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
            dl_close(db);
            return 1;
        }

        {
            long n = dl_node_observations(db, node, (int)max_obs,
                                           obs_print_cb, NULL);
            if (n < 0) {
                fprintf(stderr, "dl: obs failed\n");
                dl_close(db);
                return 1;
            }
            if (n == 0)
                printf("(no observations)\n");
        }

    } else if (strcmp(cmd, "index") == 0) {
        if (argp < argc) {
            fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
            dl_close(db);
            return 1;
        }

        {
            long n = dl_index_observations(db);
            if (n < 0) {
                fprintf(stderr, "dl: index failed\n");
                dl_close(db);
                return 1;
            }
            printf("Indexed %ld postings\n", n);
        }

    } else if (strcmp(cmd, "search") == 0) {
        const char *terms_str;
        unsigned long top_n = 10;
        unsigned long version = 0;  /* 0 means live (current) */

        if (argp >= argc) usage(argv[0]);
        terms_str = argv[argp++];

        /* Parse optional flags in any order */
        while (argp < argc) {
            if (strcmp(argv[argp], "--top") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &top_n) || top_n < 1 ||
                    top_n > INT_MAX) {
                    fprintf(stderr, "dl: invalid --top value (must be 1..2147483647)\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--version") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &version)) {
                    fprintf(stderr, "dl: invalid --version value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else {
                fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
                dl_close(db);
                return 1;
            }
        }

        /* Tokenize the search terms */
        char **tokens = tokenize(terms_str, NULL);
        if (!tokens) {
            fprintf(stderr, "dl: tokenization failed\n");
            dl_close(db);
            return 1;
        }

        /* Count tokens */
        size_t n_tokens = 0;
        while (tokens[n_tokens]) n_tokens++;

        if (n_tokens == 0) {
            fprintf(stderr, "dl: no valid terms in search query\n");
            token_free(tokens);
            dl_close(db);
            return 1;
        }

        /* Intern all tokens */
        uint32_t *term_syms = malloc(n_tokens * sizeof(*term_syms));
        if (!term_syms) {
            token_free(tokens);
            dl_close(db);
            return 1;
        }
        size_t i;
        for (i = 0; i < n_tokens; i++) {
            term_syms[i] = dl_intern_str(db, tokens[i]);
            if (term_syms[i] == 0) {
                fprintf(stderr, "dl: intern failed for term '%s'\n", tokens[i]);
                free(term_syms);
                token_free(tokens);
                dl_close(db);
                return 1;
            }
        }
        token_free(tokens);

        /* Ensure postings relation exists */
        if (aux_index_ensure_postings(db) != 0) {
            fprintf(stderr, "dl: cannot ensure postings relation\n");
            free(term_syms);
            dl_close(db);
            return 1;
        }

        /* Collect results */
        uint32_t *obs_ids = malloc(top_n * sizeof(*obs_ids));
        int *scores = malloc(top_n * sizeof(*scores));
        if (!obs_ids || !scores) {
            free(obs_ids);
            free(scores);
            free(term_syms);
            dl_close(db);
            return 1;
        }

        int n_results;
        if (version == 0) {
            n_results = dl_search_top(db, term_syms, (int)n_tokens,
                                       obs_ids, scores, (int)top_n);
        } else {
            n_results = dl_search_top_version(db, (uint32_t)version, term_syms, (int)n_tokens,
                                               obs_ids, scores, (int)top_n);
        }
        free(term_syms);

        if (n_results < 0) {
            fprintf(stderr, "dl: search failed\n");
            free(obs_ids);
            free(scores);
            dl_close(db);
            return 1;
        }

        /* Print results */
        for (i = 0; i < (size_t)n_results; i++) {
            printf("%u (score=%d)\n", obs_ids[i], scores[i]);
        }
        if (n_results == 0)
            printf("(no results)\n");

        free(obs_ids);
        free(scores);

    } else if (strcmp(cmd, "vsearch") == 0) {
        const char *query_str;
        unsigned long k = 10;
        unsigned long radius = 2;
        unsigned long version = 0;
        const char *sig_hex = NULL;
        const char *ivec_hex = NULL;
        int cand_only = 0;

        if (argp >= argc) usage(argv[0]);
        query_str = argv[argp++];

        while (argp < argc) {
            if (strcmp(argv[argp], "--k") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &k) || k < 1 || k > INT_MAX) {
                    fprintf(stderr, "dl: invalid --k value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--radius") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &radius)) {
                    fprintf(stderr, "dl: invalid --radius value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--version") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &version)) {
                    fprintf(stderr, "dl: invalid --version value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--sig") == 0) {
                argp++;
                if (argp >= argc) {
                    fprintf(stderr, "dl: --sig requires a hex string\n");
                    dl_close(db);
                    return 1;
                }
                sig_hex = argv[argp++];
            } else if (strcmp(argv[argp], "--ivec") == 0) {
                argp++;
                if (argp >= argc) {
                    fprintf(stderr, "dl: --ivec requires a hex string\n");
                    dl_close(db);
                    return 1;
                }
                ivec_hex = argv[argp++];
            } else if (strcmp(argv[argp], "--cand-only") == 0) {
                argp++;
                cand_only = 1;
            } else {
                fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
                dl_close(db);
                return 1;
            }
        }

        /* Parse or encode query signature and int8 vector */
        uint32_t q_sig[VEC_SIG_WORDS];
        uint32_t q_int8[VEC_IVEC_WORDS];

        if (sig_hex && ivec_hex) {
            /* Programmatic path: use provided hex */
            if (parse_hex_words(sig_hex, q_sig, VEC_SIG_WORDS) != VEC_SIG_WORDS) {
                fprintf(stderr, "dl: invalid --sig (need 64 hex chars = 8 u32)\n");
                dl_close(db);
                return 1;
            }
            if (parse_hex_words(ivec_hex, q_int8, VEC_IVEC_WORDS) != VEC_IVEC_WORDS) {
                fprintf(stderr, "dl: invalid --ivec (need 768 hex chars = 96 u32)\n");
                dl_close(db);
                return 1;
            }
        } else if (sig_hex || ivec_hex) {
            fprintf(stderr, "dl: provide both --sig and --ivec, or neither\n");
            dl_close(db);
            return 1;
        } else {
            /* Encode the query via ./dl-embed (fork+execv — no shell, so
             * the user-controlled query can never inject commands). */
            if (run_encode_helper(db_dir, query_str, q_sig, q_int8) != 0) {
                dl_close(db);
                return 1;
            }
        }

        /* Collect candidates from vector search */
        uint32_t *cand_syms = malloc(k * 10 * sizeof(*cand_syms));
        if (!cand_syms) {
            fprintf(stderr, "dl: OOM\n");
            dl_close(db);
            return 1;
        }
        struct vec_cand_collector collector = {cand_syms, (int)(k * 10), 0};

        long n_cand;
        if (version == 0) {
            n_cand = dl_vector_search(db, q_sig, (int)(k * 10), (int)radius,
                                       collect_vec_cand, &collector);
        } else {
            n_cand = dl_vector_search_version(db, (uint32_t)version, q_sig,
                                               (int)(k * 10), (int)radius,
                                               collect_vec_cand, &collector);
        }
        if (n_cand < 0) {
            fprintf(stderr, "dl: vector search failed\n");
            free(cand_syms);
            dl_close(db);
            return 1;
        }

        /* Re-rank candidates (or, with --cand-only, print the raw MIH
         * candidate set — used by the S4 int8-vs-float precision oracle). */
        struct vec_result_printer printer = {db, 0};
        long n_results;
        if (cand_only) {
            for (int i = 0; i < collector.count; i++)
                printf("%u\n", cand_syms[i]);
            n_results = collector.count;
        } else if (version == 0) {
            n_results = dl_vector_rerank(db, q_int8, cand_syms, collector.count,
                                          (int)k, print_vec_result, &printer);
        } else {
            n_results = dl_vector_rerank(db, q_int8, cand_syms, collector.count,
                                          (int)k, print_vec_result, &printer);
        }
        if (n_results < 0) {
            fprintf(stderr, "dl: vector rerank failed\n");
            free(cand_syms);
            dl_close(db);
            return 1;
        }

        if (!cand_only && printer.printed == 0)
            printf("(no results)\n");

        free(cand_syms);

    } else if (strcmp(cmd, "vhybrid") == 0) {
        const char *terms_str, *query_str;
        unsigned long k = 10;
        unsigned long radius = 2;
        unsigned long version = 0;
        const char *sig_hex = NULL;
        const char *ivec_hex = NULL;

        if (argp + 1 >= argc) usage(argv[0]);
        terms_str = argv[argp++];
        query_str = argv[argp++];

        while (argp < argc) {
            if (strcmp(argv[argp], "--k") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &k) || k < 1 || k > INT_MAX) {
                    fprintf(stderr, "dl: invalid --k value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--radius") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &radius)) {
                    fprintf(stderr, "dl: invalid --radius value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--version") == 0) {
                argp++;
                if (argp >= argc || !parse_u32_strict(argv[argp], &version)) {
                    fprintf(stderr, "dl: invalid --version value\n");
                    dl_close(db);
                    return 1;
                }
                argp++;
            } else if (strcmp(argv[argp], "--sig") == 0) {
                argp++;
                if (argp >= argc) {
                    fprintf(stderr, "dl: --sig requires a hex string\n");
                    dl_close(db);
                    return 1;
                }
                sig_hex = argv[argp++];
            } else if (strcmp(argv[argp], "--ivec") == 0) {
                argp++;
                if (argp >= argc) {
                    fprintf(stderr, "dl: --ivec requires a hex string\n");
                    dl_close(db);
                    return 1;
                }
                ivec_hex = argv[argp++];
            } else {
                fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
                dl_close(db);
                return 1;
            }
        }

        /* Parse or encode the query signature and int8 vector. */
        uint32_t q_sig[VEC_SIG_WORDS];
        uint32_t q_int8[VEC_IVEC_WORDS];
        if (sig_hex && ivec_hex) {
            if (parse_hex_words(sig_hex, q_sig, VEC_SIG_WORDS) != VEC_SIG_WORDS) {
                fprintf(stderr, "dl: invalid --sig (need 64 hex chars = 8 u32)\n");
                dl_close(db);
                return 1;
            }
            if (parse_hex_words(ivec_hex, q_int8, VEC_IVEC_WORDS) != VEC_IVEC_WORDS) {
                fprintf(stderr, "dl: invalid --ivec (need 768 hex chars = 96 u32)\n");
                dl_close(db);
                return 1;
            }
        } else if (sig_hex || ivec_hex) {
            fprintf(stderr, "dl: provide both --sig and --ivec, or neither\n");
            dl_close(db);
            return 1;
        } else {
            /* Encode the query via ./dl-embed (fork+execv — no shell, so
             * the user-controlled query can never inject commands). */
            if (run_encode_helper(db_dir, query_str, q_sig, q_int8) != 0) {
                dl_close(db);
                return 1;
            }
        }

        /* Get lexical candidates */
        char **tokens = tokenize(terms_str, NULL);
        if (!tokens) {
            fprintf(stderr, "dl: tokenization failed\n");
            dl_close(db);
            return 1;
        }
        size_t n_tokens = 0;
        while (tokens[n_tokens]) n_tokens++;
        if (n_tokens == 0) {
            fprintf(stderr, "dl: no valid terms\n");
            token_free(tokens);
            dl_close(db);
            return 1;
        }

        uint32_t *term_syms = malloc(n_tokens * sizeof(*term_syms));
        if (!term_syms) {
            token_free(tokens);
            dl_close(db);
            return 1;
        }
        for (size_t i = 0; i < n_tokens; i++) {
            term_syms[i] = dl_intern_str(db, tokens[i]);
            if (term_syms[i] == 0) {
                fprintf(stderr, "dl: intern failed for '%s'\n", tokens[i]);
                free(term_syms);
                token_free(tokens);
                dl_close(db);
                return 1;
            }
        }
        token_free(tokens);

        uint32_t *lex_ids = malloc(k * 10 * sizeof(*lex_ids));
        int *lex_scores = malloc(k * 10 * sizeof(*lex_scores));
        if (!lex_ids || !lex_scores) {
            free(lex_ids);
            free(lex_scores);
            free(term_syms);
            dl_close(db);
            return 1;
        }

        int n_lex;
        if (version == 0) {
            n_lex = dl_search_top(db, term_syms, (int)n_tokens,
                                   lex_ids, lex_scores, (int)(k * 10));
        } else {
            n_lex = dl_search_top_version(db, (uint32_t)version, term_syms, (int)n_tokens,
                                           lex_ids, lex_scores, (int)(k * 10));
        }
        free(term_syms);
        if (n_lex < 0) {
            fprintf(stderr, "dl: lexical search failed\n");
            free(lex_ids);
            free(lex_scores);
            dl_close(db);
            return 1;
        }

        /* Get vector candidates */
        uint32_t *vec_syms = malloc(k * 10 * sizeof(*vec_syms));
        if (!vec_syms) {
            free(lex_ids);
            free(lex_scores);
            dl_close(db);
            return 1;
        }
        struct vec_cand_collector vec_collector = {vec_syms, (int)(k * 10), 0};
        long n_vec;
        if (version == 0) {
            n_vec = dl_vector_search(db, q_sig, (int)(k * 10), (int)radius,
                                     collect_vec_cand, &vec_collector);
        } else {
            n_vec = dl_vector_search_version(db, (uint32_t)version, q_sig,
                                             (int)(k * 10), (int)radius,
                                             collect_vec_cand, &vec_collector);
        }
        if (n_vec < 0) {
            fprintf(stderr, "dl: vector search failed\n");
            free(vec_syms);
            free(lex_ids);
            free(lex_scores);
            dl_close(db);
            return 1;
        }

        /* INTERSECT: build a sorted set of ENTITY sym-ids from the lexical
           results (dl_search_top returns obs/content sym-ids; map each to its
           owning entity via the observation relation), keep only the vector
           candidates that are also lexical hits, then re-rank the
           intersection by exact int8 cosine (post-hoc join on sym-ids). */
        uint32_t *inter = malloc((size_t)vec_collector.count * sizeof(*inter));
        if (!inter) {
            fprintf(stderr, "dl: OOM\n");
            free(vec_syms);
            free(lex_ids);
            free(lex_scores);
            dl_close(db);
            return 1;
        }
        {
            int n_lex_ent = 0;
            for (int i = 0; i < n_lex; i++) {
                uint32_t e = obs_to_entity(db, lex_ids[i]);
                if (e != 0)
                    lex_ids[n_lex_ent++] = e;
            }
            n_lex = n_lex_ent;
        }
        int n_inter = 0;
        qsort(lex_ids, (size_t)n_lex, sizeof(lex_ids[0]), cmp_u32);
        for (int i = 0; i < vec_collector.count; i++) {
            if (bsearch(&vec_syms[i], lex_ids, (size_t)n_lex,
                        sizeof(lex_ids[0]), cmp_u32))
                inter[n_inter++] = vec_syms[i];
        }

        if (n_inter > 0) {
            struct vec_result_printer printer = { db, 0 };
            long n_rank = dl_vector_rerank(db, q_int8, inter, n_inter,
                                           (int)k, print_vec_result, &printer);
            if (n_rank < 0) {
                fprintf(stderr, "dl: vector rerank failed\n");
                free(inter);
                free(vec_syms);
                free(lex_ids);
                free(lex_scores);
                dl_close(db);
                return 1;
            }
            if (printer.printed == 0)
                printf("(no results)\n");
        } else {
            printf("(no results)\n");
        }

        free(inter);
        free(vec_syms);
        free(lex_ids);
        free(lex_scores);

    } else if (strcmp(cmd, "versions") == 0) {
        if (argp < argc) {
            fprintf(stderr, "dl: unexpected argument '%s'\n", argv[argp]);
            dl_close(db);
            return 1;
        }

        {
            uint32_t versions[256];
            long n = dl_snapshot_versions(db, versions, 256);
            if (n < 0) {
                fprintf(stderr, "dl: versions query failed\n");
                dl_close(db);
                return 1;
            }
            size_t i;
            for (i = 0; i < (size_t)n; i++) {
                printf("%u\n", versions[i]);
            }
            if (n == 0)
                printf("(no versions published)\n");
        }

    } else {
        usage(argv[0]);
    }

    dl_close(db);
    return 0;
}
