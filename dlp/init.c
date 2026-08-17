/* init.c — scaffold a new dlp project directory.
 *
 * `dlp init [dir]` creates dir/ (default ".") containing:
 *   schema.dhall   — worked-example typed schema (Bool-payload union DSL)
 *   data/          — EDB CSV/JSON inputs (S5)
 *   rules/         — .datalog rule files (S5)
 *   .build/        — scratch/build artifacts
 *
 * The template schema.dhall uses the S0-verified DSL: nested `let` (one `in`
 * per binding — dhall-c has no multi-let), a Bool-payload union type
 * `let ColumnType = < Natural : Bool | Text : Bool >`, union values
 * `< Text = True >` / `< Natural = True >`, and the final body annotated
 * `: Schema`.  The worked example declares node[Text], edge[Text,Text],
 * weight[Text,Natural], light_edge[Text,Text], tc[Text,Text].
 */
#include "dlp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TEMPLATE_SCHEMA \
    "-- dlp project schema (S4 worked example)\n" \
    "-- Bool-payload union DSL: < Natural = True > | < Text = True >\n" \
    "let ColumnType = < Natural : Bool | Text : Bool >\n" \
    "in let Column = { name : Text, type : ColumnType }\n" \
    "in let Relation = { name : Text, columns : List Column }\n" \
    "in let Schema = { relations : List Relation }\n" \
    "in { relations =\n" \
    "     [ { name = \"node\",\n" \
    "         columns = [ { name = \"id\", type = < Text = True > } ] },\n" \
    "       { name = \"edge\",\n" \
    "         columns = [ { name = \"src\", type = < Text = True > },\n" \
    "                     { name = \"dst\", type = < Text = True > } ] },\n" \
    "       { name = \"weight\",\n" \
    "         columns = [ { name = \"src\", type = < Text = True > },\n" \
    "                     { name = \"w\", type = < Natural = True > } ] },\n" \
    "       { name = \"light_edge\",\n" \
    "         columns = [ { name = \"src\", type = < Text = True > },\n" \
    "                     { name = \"dst\", type = < Text = True > } ] },\n" \
    "       { name = \"tc\",\n" \
    "         columns = [ { name = \"src\", type = < Text = True > },\n" \
    "                     { name = \"dst\", type = < Text = True > } ] } ] } : Schema\n"

static int mkdir_if_missing(const char *dir) {
    struct stat st;
    if (mkdir(dir, 0755) == 0) return 0;
    if (errno == EEXIST && stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return -1;
}

static int write_file(const char *path, const char *content, char *errbuf, size_t errcap) {
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(errbuf, errcap, "cannot create '%s': %s", path, strerror(errno)); return -1; }
    fputs(content, f);
    if (fclose(f) != 0) { snprintf(errbuf, errcap, "cannot write '%s': %s", path, strerror(errno)); return -1; }
    return 0;
}

int dlp_project_init(const char *dir, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    if (!dir || !*dir) dir = ".";

    char schema_path[1024], data_dir[1024], rules_dir[1024], build_dir[1024];
    if (strcmp(dir, ".") == 0) {
        snprintf(schema_path, sizeof schema_path, "schema.dhall");
        snprintf(data_dir, sizeof data_dir, "data");
        snprintf(rules_dir, sizeof rules_dir, "rules");
        snprintf(build_dir, sizeof build_dir, ".build");
    } else {
        if (snprintf(schema_path, sizeof schema_path, "%s/schema.dhall", dir) >= (int)sizeof schema_path ||
            snprintf(data_dir, sizeof data_dir, "%s/data", dir) >= (int)sizeof data_dir ||
            snprintf(rules_dir, sizeof rules_dir, "%s/rules", dir) >= (int)sizeof rules_dir ||
            snprintf(build_dir, sizeof build_dir, "%s/.build", dir) >= (int)sizeof build_dir) {
            snprintf(errbuf, errcap, "project path too long");
            return -1;
        }
    }

    if (mkdir_if_missing(dir) != 0) { snprintf(errbuf, errcap, "cannot create project dir '%s': %s", dir, strerror(errno)); return -1; }
    if (mkdir_if_missing(data_dir) != 0) { snprintf(errbuf, errcap, "cannot create '%s': %s", data_dir, strerror(errno)); return -1; }
    if (mkdir_if_missing(rules_dir) != 0) { snprintf(errbuf, errcap, "cannot create '%s': %s", rules_dir, strerror(errno)); return -1; }
    if (mkdir_if_missing(build_dir) != 0) { snprintf(errbuf, errcap, "cannot create '%s': %s", build_dir, strerror(errno)); return -1; }

    if (write_file(schema_path, TEMPLATE_SCHEMA, errbuf, errcap) != 0) return -1;

    printf("dlp: initialized project in '%s'\n", dir);
    printf("  %s\n", schema_path);
    printf("  %s/\n", data_dir);
    printf("  %s/\n", rules_dir);
    printf("  %s/\n", build_dir);
    return 0;
}
