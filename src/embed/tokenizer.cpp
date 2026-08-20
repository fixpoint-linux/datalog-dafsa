/*
 * tokenizer.cpp — BertTokenizer(bge-small-en-v1.5) port.  See tokenizer.h.
 *
 * Fidelity notes (checked against transformers' tokenization_bert.py):
 *  - _clean_text: drop cp==0, cp==0xFFFD, and control/format codepoints
 *    (Cc/Cf); \t \n \r (and non-breaking spaces, Zs) become ' '.
 *  - _tokenize_chinese_chars: CJK codepoints get a space on each side.
 *  - lowercase + _run_strip_accents: NFD then drop combining marks (Mn).
 *    Implemented exactly for ASCII + the Latin-1/Latin-Extended-A tables
 *    (the common corpus case); other scripts pass through unmodified.
 *  - _run_split_on_punc: each ASCII punctuation char is its own token
 *    (ranges 33-47, 58-64, 91-96, 123-126 — NOTE '_' is punctuation here).
 *    Non-ASCII punctuation is left inside the word (documented limitation;
 *    corpus entity names are ASCII).
 *  - WordPiece greedy longest-match with "##" continuation and single-UNK
 *    fallback for the whole word.
 */
#include "tokenizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ── UTF-8 decode ───────────────────────────────────────────────────────── */

/* Decode one codepoint at s; advances *len.  Invalid bytes decode as
 * themselves (length 1) and are dropped later by clean_text when they are
 * control chars — matches the fast tokenizer's behavior closely enough for
 * entity-name corpora. */
static uint32_t utf8_next(const char *s, int *len) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *len = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *len = 2; return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *len = 3; return ((uint32_t)(p[0] & 0x0F) << 12) |
               ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *len = 4; return ((uint32_t)(p[0] & 0x07) << 18) |
               ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) |
               (p[3] & 0x3F);
    }
    *len = 1; return 0xFFFDu;   /* invalid -> replacement, dropped later */
}

/* Encode cp into out; returns bytes written (0 if cp is a surrogate/overlong
 * guard rejects it — callers never pass those for our corpus inputs). */
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ── Codepoint classification (HF _is_* equivalents) ────────────────────── */

static int cp_is_whitespace(uint32_t cp) {
    switch (cp) {
    case ' ': case '\t': case '\n': case '\r':
    case 0x00A0: case 0x1680: case 0x202F: case 0x205F: case 0x3000:
        return 1;
    default:
        return (cp >= 0x2000 && cp <= 0x200A);
    }
}

static int cp_is_control(uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r') return 0;
    if (cp < 0x20 || cp == 0x7F) return 1;              /* Cc (ASCII side) */
    if (cp >= 0x80 && cp <= 0x9F) return 1;             /* C1 */
    /* Cf: a few common ones; Zl/Zp */
    if (cp == 0x00AD || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x200B || cp == 0x200E || cp == 0x200F || cp == 0xFEFF)
        return 1;
    return 0;
}

static int cp_is_cjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2EBEF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FA1F);
}

static int cp_is_punct_ascii(uint32_t cp) {
    return (cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) ||
           (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126);
}

/* ── Accent stripping: NFD + drop Mn, for the Latin tables ─────────────── */

/* Precise accent-strip map: precomposed Latin letter -> base ASCII letter.
 * codepoint -> base letter, for precomposed Latin letters. */
static uint32_t strip_accent_cp(uint32_t cp) {
    switch (cp) {
    /* Latin-1 Supplement */
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xC7: return 'C';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
    case 0xD1: return 'N';
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: return 'O';
    case 0xD8: return 'O';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
    case 0xDD: return 'Y';
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
    case 0xE7: return 'c';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
    case 0xF1: return 'n';
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: return 'o';
    case 0xF8: return 'o';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
    case 0xFD: case 0xFF: return 'y';
    /* Latin Extended-A (common) */
    case 0x100: case 0x102: case 0x104: return 'A';
    case 0x101: case 0x103: case 0x105: return 'a';
    case 0x106: case 0x108: case 0x10A: case 0x10C: return 'C';
    case 0x107: case 0x109: case 0x10B: case 0x10D: return 'c';
    case 0x10E: case 0x110: return 'D';
    case 0x10F: case 0x111: return 'd';
    case 0x112: case 0x114: case 0x116: case 0x118: case 0x11A: return 'E';
    case 0x113: case 0x115: case 0x117: case 0x119: case 0x11B: return 'e';
    case 0x11C: case 0x11E: case 0x120: case 0x122: return 'G';
    case 0x11D: case 0x11F: case 0x121: case 0x123: return 'g';
    case 0x124: case 0x126: return 'H';
    case 0x125: case 0x127: return 'h';
    case 0x128: case 0x12A: case 0x12C: case 0x12E: case 0x130: return 'I';
    case 0x129: case 0x12B: case 0x12D: case 0x12F: case 0x131: return 'i';
    case 0x134: return 'J'; case 0x135: return 'j';
    case 0x136: return 'K'; case 0x137: return 'k';
    case 0x139: case 0x13B: case 0x13D: case 0x13F: case 0x141: return 'L';
    case 0x13A: case 0x13C: case 0x13E: case 0x140: case 0x142: return 'l';
    case 0x143: case 0x145: case 0x147: return 'N';
    case 0x144: case 0x146: case 0x148: case 0x149: return 'n';
    case 0x14C: case 0x14E: case 0x150: return 'O';
    case 0x14D: case 0x14F: case 0x151: return 'o';
    case 0x154: case 0x156: case 0x158: return 'R';
    case 0x155: case 0x157: case 0x159: return 'r';
    case 0x15A: case 0x15C: case 0x15E: case 0x160: return 'S';
    case 0x15B: case 0x15D: case 0x15F: case 0x161: return 's';
    case 0x162: case 0x164: case 0x166: return 'T';
    case 0x163: case 0x165: case 0x167: return 't';
    case 0x168: case 0x16A: case 0x16C: case 0x16E: case 0x170: return 'U';
    case 0x169: case 0x16B: case 0x16D: case 0x16F: case 0x171: return 'u';
    case 0x172: case 0x174: case 0x176: return 'W';
    case 0x173: case 0x175: case 0x177: return 'w';
    case 0x178: return 'Y'; case 0x179: case 0x17B: return 'Z';
    case 0x17A: case 0x17C: return 'z';
    default: return 0;
    }
}

/* ── Basic tokenizer ────────────────────────────────────────────────────── */

/* Lowercase one codepoint, stripping Latin accents (HF order: lowercase,
 * then NFD+strip — equivalent for our tables). */
static uint32_t lower_strip(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp - 'A' + 'a';
    uint32_t base = strip_accent_cp(cp);
    if (base) {
        if (base >= 'A' && base <= 'Z') base = base - 'A' + 'a';
        return base;
    }
    return cp;
}

int wp_basic_tokenize(const char *utf8_in, char *out, size_t out_cap) {
    /* Pass 1: clean + CJK pad + whitespace fold into `buf` (space-sep words
     * with punctuation still inside words). */
    char buf[8192];
    size_t w = 0;
    int len;
    for (const char *p = utf8_in; *p; p += len) {
        uint32_t cp = utf8_next(p, &len);
        if (cp == 0 || cp == 0xFFFDu) continue;               /* invalid/repl */
        if (cp_is_control(cp)) continue;
        if (cp_is_whitespace(cp)) {
            if (w + 1 >= sizeof buf) return -1;
            buf[w++] = ' ';
            continue;
        }
        if (cp_is_cjk(cp)) {
            if (w + 6 >= sizeof buf) return -1;
            buf[w++] = ' ';
            w += (size_t)utf8_encode(cp, buf + w);
            buf[w++] = ' ';
            continue;
        }
        if (w + 5 >= sizeof buf) return -1;
        w += (size_t)utf8_encode(cp, buf + w);
    }
    buf[w] = '\0';

    /* Pass 2: per whitespace word: lowercase+strip accents, then split on
     * ASCII punctuation into space-separated tokens. */
    size_t o = 0;
    char *save = NULL;
    static const char *delim = " ";
    for (char *tok = strtok_r(buf, delim, &save); tok;
         tok = strtok_r(NULL, delim, &save)) {
        /* lower+strip the word first */
        char low[512];
        size_t lw = 0;
        for (const char *p = tok; *p;) {
            uint32_t cp = utf8_next(p, &len);
            p += len;
            cp = lower_strip(cp);
            lw += (size_t)utf8_encode(cp, low + lw);
            if (lw + 5 >= sizeof low) break;
        }
        low[lw] = '\0';
        /* punctuation split */
        for (size_t i = 0; i < lw; i++) {
            unsigned char ch = (unsigned char)low[i];
            uint32_t cp = ch;
            if (ch >= 0x80) {           /* multi-byte tail: copy through */
                if (o + 1 >= out_cap) return -1;
                out[o++] = low[i];
                continue;
            }
            if (cp_is_punct_ascii(cp)) {
                if (o && out[o - 1] != ' ') { if (o + 1 >= out_cap) return -1; out[o++] = ' '; }
                if (o + 1 >= out_cap) return -1;
                out[o++] = (char)ch;
                if (o + 1 >= out_cap) return -1;
                out[o++] = ' ';
            } else {
                if (o + 1 >= out_cap) return -1;
                out[o++] = low[i];
            }
        }
        if (o && out[o - 1] != ' ') { if (o + 1 >= out_cap) return -1; out[o++] = ' '; }
    }
    /* trim trailing space */
    while (o > 0 && out[o - 1] == ' ') o--;
    if (o + 1 >= out_cap) return -1;
    out[o] = '\0';
    return (int)o;
}

/* ── Vocab index ────────────────────────────────────────────────────────── */

typedef struct { const char *text; int id; } wp_entry;

static int wp_cmp(const void *a, const void *b) {
    return strcmp(((const wp_entry *)a)->text, ((const wp_entry *)b)->text);
}

static int wp_cmp_key(const void *key, const void *item) {
    return strcmp((const char *)key, ((const wp_entry *)item)->text);
}

int wp_init(wp_vocab *v) {
    v->sorted = (int *)malloc(sizeof(int) * (size_t)v->n);
    if (!v->sorted) return -1;
    wp_entry *tmp = (wp_entry *)malloc(sizeof(wp_entry) * (size_t)v->n);
    if (!tmp) { free(v->sorted); v->sorted = NULL; return -1; }
    for (int i = 0; i < v->n; i++) { tmp[i].text = v->texts[i]; tmp[i].id = i; }
    qsort(tmp, (size_t)v->n, sizeof(wp_entry), wp_cmp);
    for (int i = 0; i < v->n; i++) v->sorted[i] = tmp[i].id;
    free(tmp);
    return 0;
}

/* Contiguous sorted entries cached in a file-static block keyed by the
 * handle (dl-embed and the tests use a single active vocab at a time). */
static wp_entry *g_entries = NULL;
static const wp_vocab *g_entries_owner = NULL;

static wp_entry *vocab_entries(const wp_vocab *v) {
    if (g_entries_owner == v && g_entries) return g_entries;
    free(g_entries);
    g_entries = (wp_entry *)malloc(sizeof(wp_entry) * (size_t)v->n);
    if (!g_entries) return NULL;
    for (int i = 0; i < v->n; i++) {
        g_entries[i].text = v->texts[v->sorted[i]];
        g_entries[i].id = v->sorted[i];
    }
    g_entries_owner = v;
    return g_entries;
}

void wp_free(wp_vocab *v) {
    free(v->sorted);
    v->sorted = NULL;
    if (g_entries_owner == v) { free(g_entries); g_entries = NULL; g_entries_owner = NULL; }
}

static int vocab_lookup(const wp_vocab *v, const char *text) {
    wp_entry *ent = vocab_entries(v);
    if (!ent) return -1;
    wp_entry *hit = (wp_entry *)bsearch(text, ent, (size_t)v->n,
                                        sizeof(wp_entry), wp_cmp_key);
    return hit ? hit->id : -1;
}

int wp_find_exact(const wp_vocab *v, const char *text) {
    return vocab_lookup(v, text);
}

/* ── WordPiece ──────────────────────────────────────────────────────────── */

int wp_wordpiece(const wp_vocab *v, const char *word, int unk_id,
                 int *out, int max_out) {
    int wl = (int)strlen(word);
    int start = 0, ns = 0, is_bad = 0;
    int *sub = (int *)malloc(sizeof(int) * (size_t)(wl + 1));
    if (!sub) return 0;
    while (start < wl) {
        int end = wl, matched = -1;
        while (start < end) {
            char qbuf[300];
            int plen = end - start;
            if (plen > 255) { end--; continue; }
            if (start > 0) {
                qbuf[0] = '#'; qbuf[1] = '#';
                memcpy(qbuf + 2, word + start, (size_t)plen);
                qbuf[2 + plen] = '\0';
            } else {
                memcpy(qbuf, word + start, (size_t)plen);
                qbuf[plen] = '\0';
            }
            matched = vocab_lookup(v, qbuf);
            if (matched >= 0) break;
            end--;
        }
        if (matched < 0) { is_bad = 1; break; }
        sub[ns++] = matched;
        start = end;
    }
    int n;
    if (is_bad) {
        if (max_out >= 1) out[0] = unk_id;
        n = 1;
    } else {
        n = ns > max_out ? max_out : ns;
        for (int i = 0; i < n; i++) out[i] = sub[i];
    }
    free(sub);
    return n;
}

/* ── Full tokenize ──────────────────────────────────────────────────────── */

int wp_tokenize(const wp_vocab *v, const char *utf8_text,
                int cls_id, int sep_id, int unk_id,
                int *ids, int max_ids) {
    char basic[16384];
    if (wp_basic_tokenize(utf8_text, basic, sizeof basic) < 0)
        basic[0] = '\0';

    int n = 0;
    if (n < max_ids) ids[n++] = cls_id;
    char *save = NULL;
    for (char *tok = strtok_r(basic, " ", &save); tok && n < max_ids - 1;
         tok = strtok_r(NULL, " ", &save)) {
        n += wp_wordpiece(v, tok, unk_id, ids + n, max_ids - 1 - n);
    }
    if (n < max_ids) ids[n++] = sep_id;
    return n;
}
