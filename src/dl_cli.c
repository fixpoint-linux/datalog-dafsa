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
#include "intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_DIR = "dl-test-db";

/*
 * The dl_db struct layout (must match dl.c).
 * We reach in to access the interner for CLI value parsing.
 */
struct dl_db_internal {
    char      *dir;
    interner  *ir;
};

static interner *cli_get_interner(dl_db *db)
{
    struct dl_db_internal *dbi = (struct dl_db_internal *)db;
    return dbi->ir;
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

/* Print a column value: resolve string symbols, print ints as-is.
 * Heuristic: look up in reverse map; if found, print string; else int. */
static void print_value(dl_db *db, uint32_t v)
{
    interner *ir = cli_get_interner(db);
    const char *s = intern_str_of(ir, v);
    if (s && *s) {
        printf("%s", s);
    } else {
        printf("%u", v);
    }
}

/* Callback for dl_prefix: print one tuple */
static int print_tuple(const uint32_t *cols, uint8_t arity, void *user)
{
    dl_db *db = (dl_db *)user;
    uint8_t i;
    for (i = 0; i < arity; i++) {
        if (i > 0) printf(" ");
        print_value(db, cols[i]);
    }
    printf("\n");
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
        "  %s [-d <dir>] query '<rule>' | <file.dl>\n"
        "Values: bare integer -> raw u32; anything else -> interned string\n",
        prog, prog, prog, prog);
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

        if (argp >= argc) usage(argv[0]);
        rel_name = argv[argp++];

        k = 0;
        while (argp < argc && k < 8) {
            leading[k] = cli_parse_value(db, argv[argp]);
            k++;
            argp++;
        }

        {
            long n = dl_prefix(db, rel_name, leading, k,
                               print_tuple, db);
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
        char *source_buf = NULL;

        if (argp >= argc) usage(argv[0]);
        source = argv[argp++];

        /* Check if source is a file path */
        {
            FILE *f = fopen(source, "r");
            if (f) {
                /* Read entire file into a buffer */
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

        if (dl_compile(db) != 0) {
            fprintf(stderr, "dl: rule evaluation failed\n");
            dl_close(db);
            return 1;
        }

        printf("Rules compiled and evaluated.\n");

    } else {
        usage(argv[0]);
    }

    dl_close(db);
    return 0;
}
