/* main.c — dlp (dl-project) CLI entry point.
 *
 *   dlp init [dir]          scaffold a project dir (schema.dhall + data/ rules/ .build/)
 *   dlp schema [dir]        load + walk schema.dhall, print the typed dl_schema
 *   dlp check-schema [dir]  alias for `schema`
 *   dlp check [dir]         validate schema + typecheck rules + dry-run data (no writes)
 *   dlp build [dir]         check, then build a snapshot under dir/.build
 *   dlp query [dir] 'goal'  build in-process and evaluate a query goal
 *
 * dir defaults to ".".  The engine's dl_cli.c main, lsp.c main, and
 * playground-wasm.c entry point are NOT linked here; this is dlp's own main.
 */
#include "dlp.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out) {
    fprintf(out,
        "dlp — datalog-dafsa project tool (Dhall-driven schema)\n"
        "usage:\n"
        "  dlp init [dir]           scaffold a new project directory\n"
        "  dlp schema [dir]         walk schema.dhall into a typed schema and print it\n"
        "  dlp check-schema [dir]   alias for `schema`\n"
        "  dlp check [dir]          validate schema + typecheck rules + dry-run data\n"
        "  dlp build [dir]          check, then build a snapshot under dir/.build\n"
        "  dlp query [dir] 'goal'   build in-process and evaluate a query goal\n"
        "\n"
        "dir defaults to \".\" (the current directory).\n");
}

static int cmd_schema(const char *dir) {
    char path[1024];
    if (!dir || !*dir || strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "schema.dhall");
    else
        snprintf(path, sizeof path, "%s/schema.dhall", dir);

    dl_schema s;
    char errbuf[512];
    if (dlp_schema_load(&s, path, errbuf, sizeof errbuf) != 0) {
        fprintf(stderr, "dlp: %s\n", errbuf);
        return 1;
    }

    printf("schema: %s\n", path);
    printf("%-12s %-6s %s\n", "relation", "arity", "columns");
    for (int i = 0; i < s.n_rels; i++) {
        const dl_reldef *r = &s.rels[i];
        printf("%-12s %-6d ", r->name, (int)r->arity);
        for (int j = 0; j < r->arity; j++) {
            char tn[64];
            dlp_coltype_name(&r->cols[j], tn, sizeof tn);
            printf("%s%s", j ? "," : "", tn);
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : NULL;
    const char *dir = argc > 2 ? argv[2] : ".";

    if (!cmd) { usage(stderr); return 2; }

    if (strcmp(cmd, "init") == 0) {
        char errbuf[512];
        if (dlp_project_init(dir, errbuf, sizeof errbuf) != 0) {
            fprintf(stderr, "dlp: %s\n", errbuf);
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "schema") == 0 || strcmp(cmd, "check-schema") == 0)
        return cmd_schema(dir);

    if (strcmp(cmd, "check") == 0) {
        char errbuf[512];
        int rc = dlp_project_check(dir, errbuf, sizeof errbuf);
        if (rc != 0 && errbuf[0])
            fprintf(stderr, "dlp: %s\n", errbuf);
        return rc != 0 ? 1 : 0;
    }

    if (strcmp(cmd, "build") == 0) {
        char errbuf[512];
        int rc = dlp_project_build(dir, errbuf, sizeof errbuf);
        if (rc != 0 && errbuf[0])
            fprintf(stderr, "dlp: %s\n", errbuf);
        return rc != 0 ? 1 : 0;
    }

    if (strcmp(cmd, "query") == 0) {
        const char *goal = argc > 3 ? argv[3] : NULL;
        if (!goal) {
            fprintf(stderr, "dlp: query requires a goal argument\n\n");
            usage(stderr);
            return 2;
        }
        char errbuf[512];
        int rc = dlp_project_query(dir, goal, errbuf, sizeof errbuf);
        if (rc != 0 && errbuf[0])
            fprintf(stderr, "dlp: %s\n", errbuf);
        return rc != 0 ? 1 : 0;
    }

    fprintf(stderr, "dlp: unknown command '%s'\n\n", cmd);
    usage(stderr);
    return 2;
}
