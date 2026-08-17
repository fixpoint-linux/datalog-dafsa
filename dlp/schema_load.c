/* schema_load.c — evaluate a Dhall schema.dhall and walk its normal form into
   a dl_schema (parse_source -> infer_type -> normalize -> walk the Term tree).
   No JSON round-trip; the schema is typechecked against its own declarations
   (partial union literals are supported by the typechecker), and the walk
   still applies full runtime type-guarding.  The dnsd term-walker helpers
   (rec_get/text_flat/nat_u64/list_elems) are copied verbatim from the proven
   compendium/src/config.c and adapted for the schema shape:
   { relations = [ { name = "...", columns = [ { name = "...", type = <Text = {=}> }, ... ] }, ... ] } */#include "dhall.h"
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

/* Human-readable column-type name (List/Optional render one level of elem;
 * Enum renders bare). */
void dlp_coltype_name(const dl_colspec *c, char *out, size_t cap) {
    if (!c) { snprintf(out, cap, "?"); return; }
    switch (c->tag) {
    case DLT_NATURAL:   snprintf(out, cap, "Natural");   return;
    case DLT_TEXT:      snprintf(out, cap, "Text");      return;
    case DLT_BOOL:      snprintf(out, cap, "Bool");      return;
    case DLT_CHAR:      snprintf(out, cap, "Char");      return;
    case DLT_DATE:      snprintf(out, cap, "Date");      return;
    case DLT_TIMESTAMP: snprintf(out, cap, "Timestamp"); return;
    case DLT_SIGNED:    snprintf(out, cap, "Signed");    return;
    case DLT_LIST:
    case DLT_OPTIONAL: {
        dl_colspec ec;
        memset(&ec, 0, sizeof ec);
        ec.tag = c->elem;
        char en[32];
        dlp_coltype_name(&ec, en, sizeof en);
        snprintf(out, cap, "%s<%s>", c->tag == DLT_LIST ? "List" : "Optional", en);
        return;
    }
    case DLT_ENUM:      snprintf(out, cap, "Enum");      return;
    default:            snprintf(out, cap, "?");         return;
    }
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

/* ─── Constraint-payload readers (finish-dlp Item 2) ───────────────────── */

/* Read Some n / None _ for a Natural bound (must fit u32). */
static bool read_opt_nat(Term *t, uint32_t *out, bool *present) {
    if (!t) { walk_error("constraint field missing"); return false; }
    if (t->tag == TmNone) { *present = false; return true; }
    if (t->tag == TmSome && t->as.some.val && t->as.some.val->tag == TmConst) {
        Const c = t->as.some.val->as.c;
        uint64_t v;
        bool ok = true;
        if (c.kind != C_NAT) { walk_error("Natural bound expected"); return false; }
        v = c.bnat ? bignat_to_u64(c.bnat, &ok) : c.nat;
        if (!ok || v > 4294967295ULL) { walk_error("Natural bound out of u32 range"); return false; }
        *out = (uint32_t)v;
        *present = true;
        return true;
    }
    walk_error("Natural bound expected");
    return false;
}

/* Read Some n / None _ for an Integer bound (must fit i32). */
static bool read_opt_int(Term *t, int32_t *out, bool *present) {
    if (!t) { walk_error("constraint field missing"); return false; }
    if (t->tag == TmNone) { *present = false; return true; }
    if (t->tag == TmSome && t->as.some.val && t->as.some.val->tag == TmConst) {
        Const c = t->as.some.val->as.c;
        if (c.kind != C_INT) { walk_error("Integer bound expected"); return false; }
        if (c.big || c.i64 < INT32_MIN || c.i64 > INT32_MAX) {
            walk_error("Integer bound out of i32 range");
            return false;
        }
        *out = (int32_t)c.i64;
        *present = true;
        return true;
    }
    walk_error("Integer bound expected");
    return false;
}

/* Read Some s / None _ for a Text bound (regex). */
static bool read_opt_text(Term *t, char *out, size_t cap, bool *present) {
    if (!t) { walk_error("constraint field missing"); return false; }
    if (t->tag == TmNone) { *present = false; return true; }
    if (t->tag == TmSome && t->as.some.val) {
        *present = true;
        return text_flat(t->as.some.val, out, cap);
    }
    walk_error("Text bound expected");
    return false;
}

/* Read min/max bounds from a scalar payload record into `c`.  Natural/Char/
 * Date/Timestamp bounds are Natural (u32); Signed bounds are Integer (i32). */
static bool read_minmax_bounds(dl_colspec *c, Term *value, bool is_signed) {
    Term *mn = rec_get(value, "min");
    Term *mx = rec_get(value, "max");
    if (is_signed) {
        int32_t lo = 0, hi = 0;
        bool hlo = false, hhi = false;
        if (!read_opt_int(mn, &lo, &hlo)) return false;
        if (!read_opt_int(mx, &hi, &hhi)) return false;
        if (hlo) { c->has_min = 1; c->min = (int64_t)lo; }
        if (hhi) { c->has_max = 1; c->max = (int64_t)hi; }
    } else {
        uint32_t lo = 0, hi = 0;
        bool hlo = false, hhi = false;
        if (!read_opt_nat(mn, &lo, &hlo)) return false;
        if (!read_opt_nat(mx, &hi, &hhi)) return false;
        if (hlo) { c->has_min = 1; c->min = (int64_t)lo; }
        if (hhi) { c->has_max = 1; c->max = (int64_t)hi; }
    }
    if (c->has_min && c->has_max && c->min > c->max) {
        walk_error("min > max");
        return false;
    }
    return true;
}

/* Read a column's payload-union literal (< Text = {=} > / < Natural = {=} >
   / < List = { elem = < Text = {=} > } > / < Enum = { values = [...] } >)
   and map its selected alternative to a dl_colspec.  The 5 flat scalars map
   to their DLT_* tag (payload {=} ignored).  List/Optional read the payload
   record's 'elem' (which must resolve to a FLAT scalar — nested parameterized
   element types are rejected in v1); Enum reads 'values' into evalues[]. */
static bool walk_coltype(dl_colspec *out, Term *t) {
    if (t->tag != TmUnionLit) { walk_error("column type must be a union literal"); return false; }
    const char *label = NULL;
    Term *value = NULL;
    for (int i = 0; i < t->as.uni.n; i++)
        if (t->as.uni.fs[i].value) { label = t->as.uni.fs[i].label; value = t->as.uni.fs[i].value; break; }
    if (!label) { walk_error("column type union has no selected alternative"); return false; }

    dl_colspec c;
    memset(&c, 0, sizeof c);

    if      (!strcmp(label, "Natural"))   c.tag = DLT_NATURAL;
    else if (!strcmp(label, "Text"))      c.tag = DLT_TEXT;
    else if (!strcmp(label, "Bool"))      c.tag = DLT_BOOL;
    else if (!strcmp(label, "Char"))      c.tag = DLT_CHAR;
    else if (!strcmp(label, "Date"))      c.tag = DLT_DATE;
    else if (!strcmp(label, "Timestamp")) c.tag = DLT_TIMESTAMP;
    else if (!strcmp(label, "Signed"))    c.tag = DLT_SIGNED;
    else if (!strcmp(label, "List") || !strcmp(label, "Optional")) {
        /* The selected alternative's fs[i].value is the payload RECORD
           literal; read its 'elem' field and recurse. */
        Term *elem = rec_get(value, "elem");
        if (!elem) { walk_error("column type '%s' is missing its 'elem' field", label); return false; }
        dl_colspec ec;
        if (!walk_coltype(&ec, elem)) return false;
        if (ec.tag == DLT_LIST || ec.tag == DLT_OPTIONAL || ec.tag == DLT_ENUM) {
            walk_error("nested parameterized element type not supported (v1)");
            return false;
        }
        c.tag = !strcmp(label, "List") ? DLT_LIST : DLT_OPTIONAL;
        c.elem = ec.tag;
    }
    else if (!strcmp(label, "Enum")) {
        Term *vals = rec_get(value, "values");
        if (!vals) { walk_error("column type 'Enum' is missing its 'values' field"); return false; }
        static Term *elems[DL_ENUM_MAX_VALUES + 1];
        int n = list_elems(vals, elems, DL_ENUM_MAX_VALUES + 1);
        if (n < 0) return false;
        if (n == 0) { walk_error("Enum must have at least one value"); return false; }
        if (n > DL_ENUM_MAX_VALUES) {
            walk_error("Enum has more than %d values", DL_ENUM_MAX_VALUES);
            return false;
        }
        c.tag = DLT_ENUM;
        c.n_evalues = (uint8_t)n;
        for (int k = 0; k < n; k++)
            if (!text_flat(elems[k], c.evalues[k], DL_ENUM_VALUE_MAX)) return false;
    }
    else { walk_error("unknown column type '%s'", label); return false; }

    /* Per-column value constraints from the scalar payload record.  These are
     * DATA-LOAD metadata (min/max/regex); they do NOT affect structural type
     * equality (dl_colspec_eq ignores them).  Natural/Char/Date/Timestamp and
     * Signed carry min/max; Text carries an optional regex; Bool has none.
     * A payload with NO constraint fields (e.g. the `{=}` used as a List/
     * Optional element payload) leaves all flags clear — unconstrained. */
    if (c.tag == DLT_NATURAL || c.tag == DLT_CHAR || c.tag == DLT_DATE ||
        c.tag == DLT_TIMESTAMP) {
        if (rec_get(value, "min") || rec_get(value, "max")) {
            if (!read_minmax_bounds(&c, value, false)) return false;
        }
    } else if (c.tag == DLT_SIGNED) {
        if (rec_get(value, "min") || rec_get(value, "max")) {
            if (!read_minmax_bounds(&c, value, true)) return false;
        }
    } else if (c.tag == DLT_TEXT) {
        Term *rx = rec_get(value, "regex");
        if (rx) {
            bool present = false;
            if (!read_opt_text(rx, c.regex,
                               sizeof c.regex, &present)) return false;
            c.has_regex = present ? 1 : 0;
        }
    }

    *out = c;
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
        dl_colspec cols[DL_SCHEMA_MAX_ARITY];
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
