/*
 * tokenizer.h — HuggingFace BertTokenizer-compatible WordPiece tokenizer
 * for bge-small-en-v1.5 (do_lower_case=true), reading its vocab from GGUF
 * metadata (tokenizer.ggml.tokens / tokenizer.ggml.token_type).
 *
 * No ggml dependency: the vocab is handed in as plain arrays so tests can
 * use a mini-vocab.  Semantics verified against HF tokenizers:
 *   - basic tokenizer preprocessing: clean_text (drop control/format chars,
 *     map whitespace to space), CJK-char space padding, lowercasing with
 *     accent stripping (NFD + drop combining marks), punctuation splitting.
 *   - WordPiece: greedy longest-match, first subword bare, later subwords
 *     with "##" prefix; if ANY position fails, the WHOLE word becomes a
 *     single [UNK] (HF semantics — NOT per-char UNKs).
 */
#ifndef EMBED_TOKENIZER_H
#define EMBED_TOKENIZER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned vocab: `texts[i]` are NOT copied (must outlive the handle). */
typedef struct {
    const char *const *texts;   /* vocab_size entries, id order */
    const int32_t     *types;   /* vocab_size entries (0 normal, 1 ##-cont) or NULL */
    int                n;
    /* internal sorted index for bsearch (built by wp_init) */
    int               *sorted; /* n entries: vocab ids sorted by text */
} wp_vocab;

int  wp_init(wp_vocab *v);      /* builds the sorted index; 0 ok, -1 OOM */
void wp_free(wp_vocab *v);

/* Tokenize utf8 text -> ids ([CLS] ... [SEP]), truncated to max_ids.
 * Returns the number of ids written. */
int wp_tokenize(const wp_vocab *v, const char *utf8_text,
                int cls_id, int sep_id, int unk_id,
                int *ids, int max_ids);

/* WordPiece over a single (already basic-tokenized) word.  Public for
 * tests.  Returns count written to out. */
int wp_wordpiece(const wp_vocab *v, const char *word, int unk_id,
                 int *out, int max_out);

/* Basic-tokenizer preprocessing (clean + CJK pad + lowercase + strip
 * accents + punctuation split).  Writes space-separated tokens into out
 * (each token NUL-separated... no: space-separated, word boundaries = ' ').
 * Returns number of bytes written, or -1 if truncated.  Public for tests. */
int wp_basic_tokenize(const char *utf8_in, char *out, size_t out_cap);

/* Find a vocab id by exact token text (e.g. "[CLS]"); -1 if absent. */
int wp_find_exact(const wp_vocab *v, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_TOKENIZER_H */
