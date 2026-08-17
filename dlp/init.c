/* init.c — scaffold a new dlp project directory.
 *
 * `dlp init [dir]` creates dir/ (default ".") containing:
 *   schema.dhall   — worked-example typed schema (empty-record-payload union DSL)
 *   data/          — EDB CSV/JSON inputs (S5)
 *   rules/         — .datalog rule files (S5)
 *   .build/        — scratch/build artifacts
 *
 * The template schema.dhall uses nested `let` (one `in` per binding — dhall-c
 * has no multi-let), an empty-record-payload union type
 * `let ColumnType = < ... >` covering the 5 flat scalars plus List/Optional/Enum
 * (parameterized; payload record < List = { elem = < Text = {=} > } >), and the
 * final body annotated `: Schema`.  Two relations stay within the schema's
 * arity-8 cap: `node` exercises the 6 scalar types (Text + 5 flat scalars) and
 * `catalog` exercises the 3 parameterized types (List/Optional/Enum).
 */
#include "dlp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TEMPLATE_SCHEMA \
    "-- dlp project schema (worked example)\n" \
    "-- Optional-payload union DSL: scalars carry Optional constraint payloads\n" \
    "--   < Natural = { min = None Natural, max = None Natural } > etc.\n" \
    "-- List/Optional/Enum carry a payload record: < List = { elem = < Text = {=} > } >\n" \
    "-- Constrain a scalar: < Natural = { min = Some 0, max = Some 150 } >\n" \
    "--                     < Signed = { min = Some -10, max = Some +10 } >  (explicit sign)\n" \
    "--                     < Text = { regex = Some \"[A-Z]+\" } >  (full-key match; no ^...$ anchors)\n" \
    "-- Helper bindings cut the verbosity of unconstrained scalars.\n" \
    "let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} | Date : {=} | Timestamp : {=} | Signed : {=} >\n" \
    "in let NC = { min = None Natural, max = None Natural }\n" \
    "in let TC = { regex = None Text }\n" \
    "in let SC = { min = None Integer, max = None Integer }\n" \
    "in let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural } | Text : { regex : Optional Text } | Bool : {=} | Char : { min : Optional Natural, max : Optional Natural } | Date : { min : Optional Natural, max : Optional Natural } | Timestamp : { min : Optional Natural, max : Optional Natural } | Signed : { min : Optional Integer, max : Optional Integer } | List : { elem : Elem } | Optional : { elem : Elem } | Enum : { values : List Text } >\n" \
    "in let Column = { name : Text, type : ColumnType }\n" \
    "in let Relation = { name : Text, columns : List Column }\n" \
    "in let Schema = { relations : List Relation }\n" \
    "in { relations =\n" \
    "     [ { name = \"node\",\n" \
    "         columns = [ { name = \"id\", type = < Text = TC > },\n" \
    "                     { name = \"active\", type = < Bool = {=} > },\n" \
    "                     { name = \"born\", type = < Date = NC > },\n" \
    "                     { name = \"seen\", type = < Timestamp = NC > },\n" \
    "                     { name = \"initial\", type = < Char = NC > },\n" \
    "                     { name = \"delta\", type = < Signed = SC > } ] },\n" \
    "       { name = \"catalog\",\n" \
    "         columns = [ { name = \"tags\", type = < List = { elem = < Text = {=} > } > },\n" \
    "                     { name = \"nick\", type = < Optional = { elem = < Text = {=} > } > },\n" \
    "                     { name = \"color\", type = < Enum = { values = [ \"red\", \"green\", \"blue\" ] } > } ] } ] } : Schema\n"

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
