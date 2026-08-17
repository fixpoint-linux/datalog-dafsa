/*
 * schema.c — dl_schema builder + lookup + (S2) no-op rule-typecheck hook
 *
 * S2: the schema data model is fully implemented (dl_schema_add / dl_schema_find).
 * dl_typecheck_rules is a no-op stub returning 0; the real rule typechecker
 * body is wired in S3 (src/typecheck.c).  The guarded call site in
 * dl_load_rules is live from S2, so S3 only implements this body.
 */

#include "schema.h"

#include <string.h>

int dl_schema_add(dl_schema *s, const char *name, uint8_t arity,
                  const dl_coltype *cols, int is_idb)
{
    if (!s || !name || !cols)
        return -1;
    if (arity < 1 || arity > DL_SCHEMA_MAX_ARITY)
        return -1;
    if (s->n_rels < 0 || s->n_rels >= DL_SCHEMA_MAX_RELS)
        return -1; /* full or corrupted n_rels */

    {
        int i;
        for (i = 0; i < s->n_rels; i++) {
            if (strcmp(s->rels[i].name, name) == 0)
                return -1; /* duplicate name */
        }
    }

    {
        dl_reldef *r = &s->rels[s->n_rels];
        strncpy(r->name, name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
        r->arity = arity;
        r->is_idb = is_idb ? 1 : 0;
        memcpy(r->cols, cols, arity * sizeof(dl_coltype));
    }
    s->n_rels++;
    return 0;
}

const dl_reldef *dl_schema_find(const dl_schema *s, const char *name)
{
    int i;
    if (!s || !name)
        return NULL;
    for (i = 0; i < s->n_rels; i++) {
        if (strcmp(s->rels[i].name, name) == 0)
            return &s->rels[i];
    }
    return NULL;
}

int dl_typecheck_rules(const dl_schema *schema, void *rules, int n_rules,
                       char *errbuf, size_t errcap)
{
    /* S2: no-op.  The real rule typechecker against `schema` is wired in S3.
     * Until then a non-NULL attached schema never rejects a program, so
     * existing tests (which attach none) are unaffected. */
    (void)schema;
    (void)rules;
    (void)n_rules;
    (void)errbuf;
    (void)errcap;
    return 0;
}
