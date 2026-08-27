/* src/llm2/http_client.c — LAYER-2 HTTP/1.1 CLIENT (Slice S0).
 *
 * Plain-HTTP request builder + response head/body parser over a bounded,
 * wire-agnostic HttpBIO.  See http_client.h for the design notes.
 *
 * Memory-safety rules honored here:
 *   - every network read goes through one capped primitive (HttpBIO.recv)
 *     into a fixed read buffer; partial reads accumulate.
 *   - the request builder writes method/path/host/headers — all
 *     caller-trusted, never network-origin — via snprintf with checked
 *     return/truncate.  NO strcpy/strcat/sprintf on network bytes.
 *   - the response head and chunk-size line are length-capped.
 */
#include "http_client.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ── small bounded string helpers (no network bytes ever passed in) ─────── */

/* Case-insensitive compare of a header name of bounded length. */
static int hdr_eq(const char *s, size_t len, const char *want) {
    size_t wl = strlen(want);
    if (len != wl) return 0;
    for (size_t i = 0; i < wl; i++) {
        char a = s[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int token_has(const char *s, size_t slen, const char *want) {
    /* Case-insensitive match of a comma-separated token, ignoring OWS. */
    size_t wl = strlen(want);
    size_t i = 0;
    while (i < slen) {
        while (i < slen && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) i++;
        size_t start = i;
        while (i < slen && s[i] != ',') i++;
        size_t tl = i - start;
        while (tl > 0 && (s[start + tl - 1] == ' ' || s[start + tl - 1] == '\t')) tl--;
        if (tl == wl && hdr_eq(s + start, tl, want)) return 1;
    }
    return 0;
}

static size_t parse_uint(const char *s, size_t len) {
    size_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        if (v > (SIZE_MAX - 9) / 10) break;
        v = v * 10 + (size_t)(s[i] - '0');
    }
    return v;
}

static size_t parse_hex(const char *s, size_t len) {
    size_t v = 0;
    for (size_t i = 0; i < len; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else break;
        if (v > (SIZE_MAX - (size_t)d) / 16) return SIZE_MAX; /* saturate on overflow */
        v = v * 16 + (size_t)d;
    }
    return v;
}

/* ── response lifecycle ─────────────────────────────────────────────────── */

int http_response_init(HttpResponse *r, HttpBIO bio) {
    memset(r, 0, sizeof *r);
    r->bio = bio;
    r->head = (char *)malloc(HTTP_MAX_HEAD_BYTES);
    if (!r->head) return -1;
    r->head_cap = HTTP_MAX_HEAD_BYTES;
    r->rbuf = (char *)malloc(4096);
    if (!r->rbuf) { free(r->head); r->head = NULL; return -1; }
    r->rcap = 4096;
    return 0;
}

void http_response_free(HttpResponse *r) {
    if (!r) return;
    free(r->head); r->head = NULL;
    free(r->rbuf); r->rbuf = NULL;
}

/* ── bounded byte reader ────────────────────────────────────────────────── */

/* Refill rbuf from the BIO.  Returns 1 = got data, 0 = EOF, -1 = err. */
static int http_fill(HttpResponse *r) {
    int n = r->bio.recv(r->bio.ctx, r->rbuf, r->rcap);
    if (n < 0) return -1;
    if (n == 0) return 0;
    r->rpos = 0;
    r->rlen = (size_t)n;
    r->body_bytes += (size_t)n;
    if (r->body_bytes > HTTP_MAX_BODY_BYTES) return -1; /* DoS cap */
    return 1;
}

/* Next raw byte: 1..255, 0 on EOF, -1 on err. */
static int http_byte(HttpResponse *r) {
    for (;;) {
        if (r->rpos < r->rlen) return (unsigned char)r->rbuf[r->rpos++];
        int s = http_fill(r);
        if (s == 1) continue;
        return (s == 0) ? 0 : -1;
    }
}

/* ── response head parse ────────────────────────────────────────────────── */

static int http_parse_head(HttpResponse *r) {
    const char *h = r->head;
    size_t hl = r->head_len;
    if (hl < 5 || memcmp(h, "HTTP/", 5) != 0) return -1;

    /* status line: HTTP/x.y SP status */
    const char *sp = (const char *)memchr(h + 5, ' ', hl - 5);
    if (!sp) return -1;
    sp++;
    while (sp < h + hl && *sp == ' ') sp++;
    if (sp + 2 >= h + hl) return -1;
    if (sp[0] < '0' || sp[0] > '9' || sp[1] < '0' || sp[1] > '9' ||
        sp[2] < '0' || sp[2] > '9') return -1;
    r->status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');

    /* skip to end of status line */
    const char *p = sp;
    while (p < h + hl && *p != '\r' && *p != '\n') p++;

    /* header region: advance past the status line's CRLF */
    const char *line = p;
    if (line < h + hl && *line == '\r') line++;
    if (line < h + hl && *line == '\n') line++;
    while (line < h + hl && *line != '\r' && *line != '\n') {
        const char *nl = line;
        while (nl < h + hl && *nl != '\r' && *nl != '\n') nl++;
        size_t llen = (size_t)(nl - line);
        if (llen > 0) {
            const char *colon = (const char *)memchr(line, ':', llen);
            if (colon) {
                size_t nlen = (size_t)(colon - line);
                const char *val = colon + 1;
                while (val < nl && (*val == ' ' || *val == '\t')) val++;
                size_t vlen = (size_t)(nl - val);
                if (hdr_eq(line, nlen, "content-length")) {
                    r->content_length = parse_uint(val, vlen);
                } else if (hdr_eq(line, nlen, "transfer-encoding")) {
                    if (token_has(val, vlen, "chunked")) r->chunked = 1;
                }
            }
        }
        line = nl;
        if (line < h + hl && *line == '\r') line++;
        if (line < h + hl && *line == '\n') line++;
    }

    if (r->chunked) r->mode = 1;
    else if (r->content_length > 0) r->mode = 0;
    else r->mode = 2; /* unknown length: read to EOF */
    return 0;
}

int http_read_head(HttpResponse *r) {
    if (!r->head) return -1;
    r->head_len = 0;
    for (;;) {
        int b = http_byte(r);
        if (b < 0) return -1;
        if (b == 0) return -1; /* EOF before head terminator */
        if (r->head_len + 1 >= HTTP_MAX_HEAD_BYTES) return -1; /* oversized */
        r->head[r->head_len++] = (char)b;
        r->head[r->head_len] = '\0';
        if (r->head_len >= 4 &&
            memcmp(r->head + r->head_len - 4, "\r\n\r\n", 4) == 0) break;
        if (r->head_len >= 2 &&
            memcmp(r->head + r->head_len - 2, "\n\n", 2) == 0) break;
    }
    return http_parse_head(r);
}

/* ── body: chunked state machine ────────────────────────────────────────── */

static int http_chunked_byte(HttpResponse *r) {
    for (;;) {
        switch (r->chunk_state) {
        case 0: { /* chunk-size line */
            int b = http_byte(r);
            if (b < 0) return -1;
            if (b == 0) return -1; /* premature EOF */
            if (b == '\n') {
                r->size_buf[r->size_len] = '\0';
                r->chunk_remaining = parse_hex(r->size_buf, r->size_len);
                r->size_len = 0;
                if (r->chunk_remaining > HTTP_MAX_BODY_BYTES) return -1;
                if (r->chunk_remaining == 0) {
                    r->chunk_state = 3;          /* trailers */
                    r->trailer_line_start = 1;
                } else {
                    r->chunk_state = 1;          /* data */
                }
                continue;
            }
            if (b == '\r') continue;             /* ignore CR in size line */
            if (r->size_len >= sizeof(r->size_buf) - 1) return -1;
            r->size_buf[r->size_len++] = (char)b;
            continue;
        }
        case 1: { /* chunk data */
            if (r->chunk_remaining == 0) { r->chunk_state = 2; continue; }
            int b = http_byte(r);
            if (b < 0) return -1;
            if (b == 0) return -1;
            r->chunk_remaining--;
            return b;
        }
        case 2: { /* CRLF after chunk */
            int b = http_byte(r);
            if (b < 0) return -1;
            if (b == 0) return -1;
            if (b == '\n') r->chunk_state = 0;
            continue;
        }
        default: { /* trailers until an empty line ends the body */
            int b = http_byte(r);
            if (b < 0) return -1;
            if (b == 0) return 0;                /* clean EOF ends body */
            if (b == '\r') continue;
            if (b == '\n') {
                if (r->trailer_line_start) return 0; /* empty line -> done */
                r->trailer_line_start = 1;
                continue;
            }
            r->trailer_line_start = 0;
            continue;
        }
        }
    }
}

int http_body_next(HttpResponse *r) {
    if (r->mode == 1) return http_chunked_byte(r);
    if (r->mode == 0) { /* content-length */
        if (r->body_consumed >= r->content_length) return 0; /* EOF */
        int b = http_byte(r);
        if (b < 0) return -1;
        if (b == 0) return -1; /* premature EOF */
        r->body_consumed++;
        return b;
    }
    return http_byte(r); /* read to EOF */
}

/* ── request builder ────────────────────────────────────────────────────── */

typedef struct { char buf[8192]; size_t len; } ReqBuf;

static int req_add(ReqBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, sizeof(b->buf) - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(b->buf) - b->len) return -1; /* truncate */
    b->len += (size_t)n;
    return 0;
}

int http_request(HttpBIO *bio, const char *method, const char *path,
                 const char *host, const char **headers,
                 const char *req_body, size_t req_body_len) {
    ReqBuf b;
    b.len = 0;
    if (req_add(&b, "%s %s HTTP/1.1\r\n", method, path) != 0) return -1;
    if (host && req_add(&b, "Host: %s\r\n", host) != 0) return -1;
    if (req_body && req_add(&b, "Content-Length: %zu\r\n", req_body_len) != 0) return -1;
    if (req_add(&b, "Connection: close\r\n") != 0) return -1;
    if (headers) {
        for (size_t i = 0; headers[i]; i++)
            if (req_add(&b, "%s\r\n", headers[i]) != 0) return -1;
    }
    if (req_add(&b, "\r\n") != 0) return -1;

    if (bio->send(bio->ctx, b.buf, b.len) != 0) return -1;
    if (req_body && req_body_len &&
        bio->send(bio->ctx, req_body, req_body_len) != 0) return -1;
    return 0;
}

/* ── plain-socket BIO ───────────────────────────────────────────────────── */

static int sock_recv(void *ctx, void *buf, size_t cap) {
    HttpSocket *s = (HttpSocket *)ctx;
    if (cap == 0) return 0;
    if (cap > (size_t)INT_MAX) cap = (size_t)INT_MAX;
    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n < 0) return -1;
    return (int)n;
}

static int sock_send(void *ctx, const void *buf, size_t len) {
    HttpSocket *s = (HttpSocket *)ctx;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > (size_t)INT_MAX) chunk = (size_t)INT_MAX;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = send(s->fd, (const char *)buf + off, chunk, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* zero progress: bail, would otherwise spin forever */
        off += (size_t)n;
    }
    return 0;
}

static void sock_close(void *ctx) {
    HttpSocket *s = (HttpSocket *)ctx;
    if (s && s->fd >= 0) { close(s->fd); s->fd = -1; }
}

void http_socket_bio(int fd, HttpSocket *sock, HttpBIO *out) {
    sock->fd = fd;
    out->recv = sock_recv;
    out->send = sock_send;
    out->close = sock_close;
    out->ctx = sock;
}
