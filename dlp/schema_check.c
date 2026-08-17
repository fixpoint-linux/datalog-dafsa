/* schema_check.c — S4 verification harness.
 *
 * Standalone binary: writes the S0-verified worked-example schema.dhall to a
 * temp file, loads+walks it via dlp_schema_load, and asserts the resulting
 * dl_schema is exactly:
 *   node[Text], edge[Text,Text], weight[Text,Natural],
 *   light_edge[Text,Text], tc[Text,Text]
 *
 * Links the same sources as `dlp` (engine core + dhall-c).  Run via
 * `make dlp-check` or manually with ./dlp_schema_check.
 */
#include "dlp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *WORKED =
    "-- worked-example schema (S4)\n"
    "let ColumnType = < Natural : Bool | Text : Bool >\n"
    "in let Column = { name : Text, type : ColumnType }\n"
    "in let Relation = { name : Text, columns : List Column }\n"
    "in let Schema = { relations : List Relation }\n"
    "in { relations =\n"
    "     [ { name = \"node\",\n"
    "         columns = [ { name = \"id\", type = < Text = True > } ] },\n"
    "       { name = \"edge\",\n"
    "         columns = [ { name = \"src\", type = < Text = True > },\n"
    "                     { name = \"dst\", type = < Text = True > } ] },\n"
    "       { name = \"weight\",\n"
    "         columns = [ { name = \"src\", type = < Text = True > },\n"
    "                     { name = \"w\", type = < Natural = True > } ] },\n"
    "       { name = \"light_edge\",\n"
    "         columns = [ { name = \"src\", type = < Text = True > },\n"
    "                     { name = \"dst\", type = < Text = True > } ] },\n"
    "       { name = \"tc\",\n"
    "         columns = [ { name = \"src\", type = < Text = True > },\n"
    "                     { name = \"dst\", type = < Text = True > } ] } ] } : Schema\n";

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

    expect(s.n_rels == 5, "n_rels == 5");

    const dl_reldef *node = find(&s, "node");
    expect(node != NULL, "node present");
    if (node) {
        expect(node->arity == 1 && node->cols[0] == DLT_TEXT, "node[Text]");
        expect(node->is_idb == 0, "node is EDB (is_idb 0)");
    }

    const dl_reldef *edge = find(&s, "edge");
    expect(edge != NULL, "edge present");
    if (edge) {
        expect(edge->arity == 2 && edge->cols[0] == DLT_TEXT && edge->cols[1] == DLT_TEXT, "edge[Text,Text]");
    }

    const dl_reldef *weight = find(&s, "weight");
    expect(weight != NULL, "weight present");
    if (weight) {
        expect(weight->arity == 2 && weight->cols[0] == DLT_TEXT && weight->cols[1] == DLT_NATURAL, "weight[Text,Natural]");
    }

    const dl_reldef *light_edge = find(&s, "light_edge");
    expect(light_edge != NULL, "light_edge present");
    if (light_edge) {
        expect(light_edge->arity == 2 && light_edge->cols[0] == DLT_TEXT && light_edge->cols[1] == DLT_TEXT, "light_edge[Text,Text]");
    }

    const dl_reldef *tc = find(&s, "tc");
    expect(tc != NULL, "tc present");
    if (tc) {
        expect(tc->arity == 2 && tc->cols[0] == DLT_TEXT && tc->cols[1] == DLT_TEXT, "tc[Text,Text]");
    }

    printf("\n%d checks, %d failures\n", runs, failures);
    return failures == 0 ? 0 : 1;
}
