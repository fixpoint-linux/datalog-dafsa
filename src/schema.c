/*
 * schema.c — dl_schema builder + lookup.
 *
 * dl_typecheck_rules (declared in schema.h) is implemented in src/typecheck.c
 * (S3); it is NOT defined here.  The guarded call site in dl_load_rules is live
 * and resolves to typecheck.o at link time.
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
