/* schema_check.c — S4 verification harness (expanded for Stage B).
 *
 * Standalone binary: writes the verified expanded schema.dhall (the full
 * ColumnType union; `node` exercising the scalar types, `catalog` exercising
 * List/Optional/Enum) to a temp file, loads+walks it via dlp_schema_load, and
 * asserts the resulting dl_schema walks to the expected tags + parameters
 * (List elem, Enum value set).
 *
 * Links the same sources as `dlp` (engine core + dhall-c).  Run via
 * `make dlp-check` or manually with ./dlp_schema_check.
 */
#include "dlp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *WORKED =
    "-- expanded worked-example schema (finish-dlp artifact-3 DSL)\n"
    "-- scalars carry Optional constraint payloads; NC/TC/SC cut verbosity\n"
    "let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} | Date : {=} | Timestamp : {=} | Signed : {=} >\n"
    "in let NC = { min = None Natural, max = None Natural }\n"
    "in let TC = { regex = None Text }\n"
    "in let SC = { min = None Integer, max = None Integer }\n"
    "in let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural } | Text : { regex : Optional Text } | Bool : {=} | Char : { min : Optional Natural, max : Optional Natural } | Date : { min : Optional Natural, max : Optional Natural } | Timestamp : { min : Optional Natural, max : Optional Natural } | Signed : { min : Optional Integer, max : Optional Integer } | List : { elem : Elem } | Optional : { elem : Elem } | Enum : { values : List Text } >\n"
    "in let Column = { name : Text, type : ColumnType }\n"
    "in let Relation = { name : Text, columns : List Column }\n"
    "in let Schema = { relations : List Relation }\n"
    "in { relations =\n"
    "     [ { name = \"node\",\n"
    "         columns = [ { name = \"id\", type = < Text = TC > },\n"
    "                     { name = \"active\", type = < Bool = {=} > },\n"
    "                     { name = \"born\", type = < Date = NC > },\n"
    "                     { name = \"seen\", type = < Timestamp = NC > },\n"
    "                     { name = \"initial\", type = < Char = NC > },\n"
    "                     { name = \"delta\", type = < Signed = SC > } ] },\n"
    "       { name = \"catalog\",\n"
    "         columns = [ { name = \"tags\", type = < List = { elem = < Text = {=} > } > },\n"
    "                     { name = \"nick\", type = < Optional = { elem = < Text = {=} > } > },\n"
    "                     { name = \"color\", type = < Enum = { values = [ \"red\", \"green\", \"blue\" ] } > } ] },\n"
    "       { name = \"constrained\",\n"
    "         columns = [ { name = \"score\", type = < Natural = { min = Some 0, max = Some 150 } > },\n"
    "                     { name = \"temp\", type = < Signed = { min = Some -10, max = Some +10 } > },\n"
    "                     { name = \"code\", type = < Text = { regex = Some \"^[A-Z][a-z]+$\" } > } ] } ] } : Schema\n";

static int failures = 0;
static int runs = 0;

static void expect(int cond, const char *what) {
    runs++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
    else printf("  OK: %s\n", what);
}

static const dl_reldef *find(const dl_schema *s, const char *name) {
    for (int i = 0; i < s->n_rels; i++)
        if (strcmp(s->rels[i].name, name) == 0) return &s->rels[i];
    return NULL;
}

/* Is `c` a flat scalar with the given tag? */
static int is_scalar(const dl_colspec *c, dl_coltype tag) {
    return c->tag == tag && c->n_evalues == 0;
}

int main(void) {
    const char *path = "/tmp/schema_check_worked.dhall";
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write temp schema\n"); return 1; }
    fputs(WORKED, f);
    fclose(f);

    dl_schema s;
    char errbuf[512];
    int rc = dlp_schema_load(&s, path, errbuf, sizeof errbuf);
    expect(rc == 0, "dlp_schema_load returns 0");
    if (rc != 0) { printf("  errbuf: %s\n", errbuf); return 1; }

    expect(s.n_rels == 3, "n_rels == 3");

    const dl_reldef *node = find(&s, "node");
    expect(node != NULL, "node present");
    if (node) {
        expect(node->arity == 6, "node arity == 6");
        expect(node->is_idb == 0, "node is EDB (is_idb 0)");
        expect(is_scalar(&node->cols[0], DLT_TEXT),      "node[0] Text");
        expect(is_scalar(&node->cols[1], DLT_BOOL),      "node[1] Bool");
        expect(is_scalar(&node->cols[2], DLT_DATE),      "node[2] Date");
        expect(is_scalar(&node->cols[3], DLT_TIMESTAMP), "node[3] Timestamp");
        expect(is_scalar(&node->cols[4], DLT_CHAR),      "node[4] Char");
        expect(is_scalar(&node->cols[5], DLT_SIGNED),    "node[5] Signed");
    }

    const dl_reldef *cat = find(&s, "catalog");
    expect(cat != NULL, "catalog present");
    if (cat) {
        expect(cat->arity == 3, "catalog arity == 3");

        /* List<Text> */
        expect(cat->cols[0].tag == DLT_LIST && cat->cols[0].elem == DLT_TEXT,
               "catalog[0] List<Text>");

        /* Optional<Text> */
        expect(cat->cols[1].tag == DLT_OPTIONAL && cat->cols[1].elem == DLT_TEXT,
               "catalog[1] Optional<Text>");

        /* Enum {red,green,blue} */
        expect(cat->cols[2].tag == DLT_ENUM && cat->cols[2].n_evalues == 3,
               "catalog[2] Enum arity 3");
        if (cat->cols[2].tag == DLT_ENUM) {
            expect(strcmp(cat->cols[2].evalues[0], "red") == 0 &&
                   strcmp(cat->cols[2].evalues[1], "green") == 0 &&
                   strcmp(cat->cols[2].evalues[2], "blue") == 0,
                   "catalog[2] Enum values {red,green,blue}");
        }
    }

    /* `constrained` exercises per-column constraints (finish-dlp Item 2). */
    const dl_reldef *con = find(&s, "constrained");
    expect(con != NULL, "constrained present");
    if (con) {
        expect(con->arity == 3, "constrained arity == 3");
        expect(con->cols[0].tag == DLT_NATURAL &&
               con->cols[0].has_min == 1 && con->cols[0].has_max == 1 &&
               con->cols[0].min == 0 && con->cols[0].max == 150,
               "constrained[0] Natural[0..150]");
        expect(con->cols[1].tag == DLT_SIGNED &&
               con->cols[1].has_min == 1 && con->cols[1].has_max == 1 &&
               con->cols[1].min == -10 && con->cols[1].max == 10,
               "constrained[1] Signed[-10..+10]");
        expect(con->cols[2].tag == DLT_TEXT && con->cols[2].has_regex == 1 &&
               strcmp(con->cols[2].regex, "^[A-Z][a-z]+$") == 0,
               "constrained[2] Text~^[A-Z][a-z]+$");
    }

    /* dl_colspec_eq must IGNORE constraints: two same-type columns with
       different ranges are structurally equal (occurrence-consistency). */
    {
        dl_colspec a, b;
        memset(&a, 0, sizeof a); a.tag = DLT_NATURAL;
        a.has_min = 1; a.min = 1; a.has_max = 1; a.max = 10;
        memset(&b, 0, sizeof b); b.tag = DLT_NATURAL;
        b.has_min = 1; b.min = 100; b.has_max = 1; b.max = 200;
        expect(dl_colspec_eq(a, b), "dl_colspec_eq ignores min/max constraints");
    }

    /* Render the coltype names (dlp schema output path) — must show the
       parameterized forms List<Text> / Optional<Text>. */
    {
        char tn[64];
        dlp_coltype_name(&cat->cols[0], tn, sizeof tn);
        expect(strcmp(tn, "List<Text>") == 0, "coltype name List<Text>");
        dlp_coltype_name(&cat->cols[1], tn, sizeof tn);
        expect(strcmp(tn, "Optional<Text>") == 0, "coltype name Optional<Text>");
        dlp_coltype_name(&cat->cols[2], tn, sizeof tn);
        expect(strcmp(tn, "Enum") == 0, "coltype name Enum");
    }

    printf("\n%d checks, %d failures\n", runs, failures);
    return failures == 0 ? 0 : 1;
}
