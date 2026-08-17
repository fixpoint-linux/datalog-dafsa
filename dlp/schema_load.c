/* schema_load.c — evaluate a Dhall schema.dhall and walk its normal form into
   a dl_schema (parse_source -> infer_type -> normalize -> walk the Term tree).
   No JSON round-trip; the schema is typechecked against its own declarations
   (partial union literals are supported by the typechecker), and the walk
   still applies full runtime type-guarding.  The dnsd term-walker helpers
   (rec_get/text_flat/nat_u64/list_elems) are copied verbatim from the proven
   compendium/src/config.c and adapted for the schema shape:
   { relations = [ { name = "...", columns = [ { name = "...", type = <Text = True> }, ... ] }, ... ] } */#include "dhall.h"
#include "dlp.h"

#include <ctype.h>
#include <stdarg.h>
#include <string.h>

/* Column-name table retained from the Dhall schema (S5).
 *
 * dl_reldef (src/schema.h) stores column TYPES only; header-name mapping in the
 * CSV loader needs the column NAMES too.  They are captured here, in relation
 * index order, during the walk, and exposed via dlp_schema_colname().  Because
 * dlp loads exactly one schema at a time, a single static table is sufficient.
 */
static char colnames[DL_SCHEMA_MAX_RELS][DL_SCHEMA_MAX_ARITY][DL_SCHEMA_NAME_MAX];

/* Return the name of column `col` (0-based) of relation `rel`, or NULL if the
 * relation/column is out of range. */
const char *dlp_schema_colname(const dl_schema *s, const char *rel, int col) {
    if (!s || !rel || col < 0 || col >= DL_SCHEMA_MAX_ARITY) return NULL;
    for (int i = 0; i < s->n_rels; i++)
        if (strcmp(s->rels[i].name, rel) == 0) {
            if (col >= s->rels[i].arity) return NULL;
            return colnames[i][col];
        }
    return NULL;
}

static char walk_err[256];
static void walk_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(walk_err, sizeof walk_err, fmt, ap); va_end(ap);
}

/* Look up a record-literal field BY LABEL. normalize() sorts fields
   alphabetically, so index-based access is wrong. */
static Term *rec_get(Term *t, const char *label) {
    if (!t || t->tag != TmRecordLit) return NULL;
    for (int i = 0; i < t->as.rec.n; i++)
        if (!strcmp(t->as.rec.fs[i].label, label)) return t->as.rec.fs[i].value;
    return NULL;
}

static bool text_flat(Term *t, char *out, size_t cap) {
    if (!t || t->tag != TmText || !t->as.text) { walk_error("expected Text"); return false; }
    size_t n = 0;
    for (TextPart *p = t->as.text; p; p = p->next) {
        if (p->expr) { walk_error("text interpolation not normalized"); return false; }
        if (p->lit) n += strlen(p->lit);
    }
    if (n + 1 > cap) { walk_error("text too long"); return false; }
    out[0] = '\0';
    for (TextPart *p = t->as.text; p; p = p->next) if (p->lit) strcat(out, p->lit);
    return true;
}

static int list_elems(Term *t, Term **elems, int cap) {
    int n = 0;
    for (Term *p = t; p->tag == TmCons; p = p->as.cons.tail) {
        if (n == cap) { walk_error("list too long (cap %d)", cap); return -1; }
        elems[n++] = p->as.cons.head;
    }
    if (t->tag != TmNil && n == 0) { walk_error("expected a list"); return -1; }
    return n;
}

/* Read a column's payload-union literal (< Text = True > / < Natural = True >)
   and map its selected alternative to a dl_coltype. */
static bool walk_coltype(dl_coltype *out, Term *t) {
    if (t->tag != TmUnionLit) { walk_error("column type must be a union literal"); return false; }
    const char *label = NULL;
    for (int i = 0; i < t->as.uni.n; i++)
        if (t->as.uni.fs[i].value) { label = t->as.uni.fs[i].label; break; }
    if (!label) { walk_error("column type union has no selected alternative"); return false; }
    if      (!strcmp(label, "Natural")) *out = DLT_NATURAL;
    else if (!strcmp(label, "Text"))    *out = DLT_TEXT;
    else { walk_error("unknown column type '%s'", label); return false; }
    return true;
}

static bool build_schema(dl_schema *s, Term *nf) {
    memset(s, 0, sizeof *s);
    if (!nf || nf->tag != TmRecordLit) { walk_error("schema must be a record"); return false; }
    Term *relations = rec_get(nf, "relations");
    if (!relations) { walk_error("schema missing 'relations'"); return false; }
    static Term *relems[DL_SCHEMA_MAX_RELS];
    int nrels = list_elems(relations, relems, DL_SCHEMA_MAX_RELS);
    if (nrels < 0) return false;
    for (int i = 0; i < nrels; i++) {
        static char rname[DL_SCHEMA_NAME_MAX];
        if (!text_flat(rec_get(relems[i], "name"), rname, sizeof rname)) return false;
        Term *columns = rec_get(relems[i], "columns");
        if (!columns) { walk_error("relation '%s' missing 'columns'", rname); return false; }
        static Term *celems[DL_SCHEMA_MAX_ARITY];
        int arity = list_elems(columns, celems, DL_SCHEMA_MAX_ARITY);
        if (arity < 0) return false;
        dl_coltype cols[DL_SCHEMA_MAX_ARITY];
        for (int j = 0; j < arity; j++) {
            /* Retain the column NAME (S5 header mapping) before the type. */
            static char cname[DL_SCHEMA_NAME_MAX];
            Term *cnamet = rec_get(celems[j], "name");
            if (!cnamet) { walk_error("relation '%s' column %d missing 'name'", rname, j); return false; }
            if (!text_flat(cnamet, cname, sizeof cname)) return false;
            if (j < DL_SCHEMA_MAX_ARITY)
                snprintf(colnames[i][j], sizeof colnames[i][j], "%s", cname);
            Term *type = rec_get(celems[j], "type");
            if (!type) { walk_error("relation '%s' column %d missing 'type'", rname, j); return false; }
            if (!walk_coltype(&cols[j], type)) return false;
        }
        /* is_idb is inferred later (S5 rule-head analysis); the walker does
           not know IDB yet, so all relations are declared EDB (0). */
        if (dl_schema_add(s, rname, (uint8_t)arity, cols, 0) != 0) {
            walk_error("cannot add relation '%s' (arity %d)", rname, arity);
            return false;
        }
    }
    return true;
}

static char *read_all(FILE *f, size_t *len_out) {
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

int dlp_schema_load(dl_schema *s, const char *path, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    FILE *in = fopen(path, "rb");
    if (!in) { snprintf(errbuf, errcap, "cannot open schema file '%s'", path); return -1; }
    size_t src_len = 0;
    char *src = read_all(in, &src_len);
    fclose(in);
    if (!src) { snprintf(errbuf, errcap, "out of memory reading '%s'", path); return -1; }

    if (!dhall_arena) dhall_arena = arena_new();
    arena_reset(dhall_arena);

    ImportLoader *loader = import_loader_new();
    import_loader_push_root(loader, path);

    Parser p;
    memset(&p, 0, sizeof p);
    p.loader = loader;
    DhallError err;
    dhall_error_clear(&err);

    Term *t = parse_source(&p, src, path, &err);
    free(src);
    if (!t) {
        snprintf(errbuf, errcap, "schema parse error: %s", err.msg);
        import_loader_free(loader);
        return -1;
    }
    /* Typecheck the schema against its own declarations before walking it. */
    Term *ty = infer_type(&p, t, &err);
    if (!ty) {
        snprintf(errbuf, errcap, "schema type error: %s", err.msg);
        import_loader_free(loader);
        return -1;
    }
    normalize_clear_error();
    Term *nf = normalize(t);
    if (normalize_has_error()) {
        err = *normalize_get_error();
        snprintf(errbuf, errcap, "schema normalize error: %s", err.msg);
        import_loader_free(loader);
        return -1;
    }
    import_loader_free(loader);

    bool ok = build_schema(s, nf);
    if (!ok) {
        snprintf(errbuf, errcap, "schema error: %s", walk_err);
        return -1;
    }
    return 0;
}
