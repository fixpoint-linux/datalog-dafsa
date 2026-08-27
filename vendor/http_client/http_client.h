/* src/llm2/http_client.h — LAYER-2 HTTP/1.1 CLIENT (Slice S0).
 *
 * An HTTP/1.1 client over a plain connect() fd, built on a wire-agnostic
 * BOUNDED transport abstraction (HttpBIO).  Plain HTTP only in S0; a later
 * slice (S1) layers mbedTLS behind the SAME recv/send/close fn pointers so
 * the caller never knows whether the wire is plain or TLS.
 *
 * Design (see handoff-c-runtime-l2-s0-ctx / -plan):
 *   - ONE bounded-read primitive for ALL network bytes: HttpBIO.recv is
 *     capped, partial reads accumulate in an internal read buffer.  Every
 *     read is length-capped.
 *   - NO strcpy/strcat/sprintf on network-origin bytes.  The request
 *     builder writes into a bounded buffer via snprintf-with-checked-return
 *     (the method/path/host/headers are caller-trusted, not network bytes).
 *   - Response parser: bounded head (HTTP_MAX_HEAD_BYTES) + chunked- or
 *     content-length body via http_body_next(), which pulls ONE body byte
 *     (1..255, 0 EOF, -1 err) — the byte-stream contract the S2 SSE parser
 *     consumes.
 *   - Bounds: HTTP_MAX_HEAD_BYTES 64KiB, HTTP_MAX_BODY_BYTES 4MiB.
 *
 * The memory-safety hotspot is the body/chunk state machine; see
 * http_body_next() in http_client.c.  Body bytes >= 1 are returned; a NUL
 * body byte is indistinguishable from EOF by design of the mandated
 * 1..255/0/-1 contract (SSE/JSON text bodies never contain NULs).
 */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Bounded transport abstraction (wire-agnostic) ──────────────────────── */
typedef struct HttpBIO {
    /* recv: read up to cap bytes into buf; return n>0 read, 0 on EOF, -1 err.
     * MUST be capped by the implementation. */
    int  (*recv)(void *ctx, void *buf, size_t cap);
    /* send: sendall — write all len bytes (block until done or err); 0/-1. */
    int  (*send)(void *ctx, const void *buf, size_t len);
    /* close: idempotent. */
    void (*close)(void *ctx);
    void *ctx;      /* impl private data */
} HttpBIO;

#define HTTP_MAX_HEAD_BYTES (64 * 1024)
#define HTTP_MAX_BODY_BYTES (4 * 1024 * 1024)

/* ── Response parse state ───────────────────────────────────────────────── */
typedef struct HttpResponse {
    HttpBIO bio;

    /* head (malloc'd, NUL-terminated, <= HTTP_MAX_HEAD_BYTES). */
    char  *head;
    size_t head_len;
    size_t head_cap;
    int    status;          /* e.g. 200 */
    size_t content_length;  /* 0 if chunked or unknown */
    int    chunked;         /* 1 if Transfer-Encoding: chunked */

    /* private body/read state — do not touch outside http_client.c. */
    char  *rbuf; size_t rcap, rlen, rpos; /* buffered bytes from the BIO */
    size_t body_bytes;      /* total pulled from the BIO (cap guard) */
    size_t body_consumed;   /* bytes returned in content-length mode */
    int    mode;            /* 0=content-length, 1=chunked, 2=read-to-EOF */
    int    chunk_state;     /* 0=size, 1=data, 2=crlf, 3=trailers */
    size_t chunk_remaining;
    char   size_buf[40]; size_t size_len;
    int    trailer_line_start;
} HttpResponse;

/* ── API ────────────────────────────────────────────────────────────────── */

/* Build + send an HTTP request.  req_body may be NULL (no Content-Length).
 * `headers` is a NULL-terminated array of raw "Name: value" strings, or NULL.
 * method/path/host/headers are caller-trusted (never network bytes).
 * Returns 0 on sent, -1 on build/send error. */
int http_request(HttpBIO *bio, const char *method, const char *path,
                 const char *host, const char **headers,
                 const char *req_body, size_t req_body_len);

/* Prepare a response parser over `bio` (the BIO is copied into `r`).  Allocates
 * the head + read buffers.  Returns 0 on success, -1 on OOM. */
int http_response_init(HttpResponse *r, HttpBIO bio);
/* Release r's owned buffers.  NULL safe. */
void http_response_free(HttpResponse *r);

/* Read + parse the response head (status line + bounded headers).  Captures
 * status / content_length / chunked.  0 on success, -1 on parse/oversize/err. */
int http_read_head(HttpResponse *r);

/* Pull the next body byte: 1..255 byte, 0 EOF, -1 error.  Handles
 * content-length and chunked transfer; unknown-length bodies read to EOF.
 * This is what the S2 SSE parser consumes. */
int http_body_next(HttpResponse *r);

/* Plain-socket BIO.  `sock` must outlive the BIO; close() marks fd=-1. */
typedef struct { int fd; } HttpSocket;
void http_socket_bio(int fd, HttpSocket *sock, HttpBIO *out);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
