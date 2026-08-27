/*
 * remote_embed.cpp — OpenAI-compatible /v1/embeddings client for the embedder.
 *
 * Plugs the HTTP transport + JSON parsing vendored from vibe-runtime
 * (vendor/http_client + vendor/yyjson) into a batching embedder that speaks
 * the OpenAI /embeddings API.  When DL_EMBED_API_URL is set, corpus build and
 * query encode route through the remote model (ollama on babylon's P40 serving
 * the same bge-small-en-v1.5 GGUF) instead of the local CPU ggml bert.
 *
 * 384-dim output matches the local bert space (same model), so the existing
 * vector tier stays byte-compatible.
 */
#include "remote_embed.h"
#include "http_client.h"
#include "yyjson.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <pthread.h>
#include <atomic>

#define EMBED_DIM 384
#define MAX_BATCH 512

static void errset(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

/* Sanitize s into a valid-UTF-8 copy: any invalid sequence (lone/truncated
 * bytes, CESU-8 surrogate halves) is replaced by U+FFFD so yyjson's mut-writer
 * (which rejects invalid UTF-8) never fails on corpus text.  Returns the
 * sanitized std::string. */
static std::string utf8_sanitize(const char *s, size_t len) {
    std::string out;
    out.reserve(len);
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80) { out.push_back((char)c); i++; continue; }
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else { out += "\xEF\xBF\xBD"; i++; continue; }
        if (i + need > len) { out += "\xEF\xBF\xBD"; i++; continue; }
        bool ok = true;
        for (size_t j = 1; j < need; j++)
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) { ok = false; break; }
        /* reject overlong + surrogates (CESU-8) + out-of-range */
        if (ok) {
            unsigned cp;
            if (need == 2) cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F);
            else if (need == 3) cp = ((c & 0x0F) << 12) | (((unsigned char)s[i+1] & 0x3F) << 6) | ((unsigned char)s[i+2] & 0x3F);
            else cp = ((c & 0x07) << 18) | (((unsigned char)s[i+1] & 0x3F) << 12) | (((unsigned char)s[i+2] & 0x3F) << 6) | ((unsigned char)s[i+3] & 0x3F);
            if ((need == 2 && cp < 0x80) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF)) ok = false;   /* overlong / range / surrogate */
        }
        if (!ok) { out += "\xEF\xBF\xBD"; i++; }
        else { for (size_t j = 0; j < need; j++) out.push_back(s[i + j]); i += need; }
    }
    return out;
}

int remote_embed_configured(void) {
    const char *url = getenv("DL_EMBED_API_URL");
    return url && *url;
}

/* parse "scheme://host[:port]/path" into host, port, and the path (which
 * includes the leading '/').  Returns 0 / -1. */
static int parse_base_url(const char *url, int *use_tls,
                          char *host, size_t host_cap, int *port,
                          char *path, size_t path_cap) {
    *use_tls = 0;
    if (strncmp(url, "https://", 8) == 0) { *use_tls = 1; url += 8; }
    else if (strncmp(url, "http://", 7) == 0) { url += 7; }
    else return -1;

    const char *p = url;
    const char *colon = NULL, *slash = NULL;
    for (; *p; p++) {
        if (*p == ':' && !colon) colon = p;
        else if (*p == '/' && !slash) slash = p;
    }
    if (colon && slash && colon > slash) colon = NULL; /* colon after path */

    size_t host_len;
    if (colon) host_len = (size_t)(colon - url);
    else if (slash) host_len = (size_t)(slash - url);
    else host_len = (size_t)(p - url);
    if (host_len == 0 || host_len >= host_cap) return -1;
    memcpy(host, url, host_len);
    host[host_len] = '\0';

    if (colon) {
        const char *ps = colon + 1;
        const char *pe = slash ? slash : p;
        long v = strtol(ps, NULL, 10);
        if (v <= 0 || v > 65535 || (pe && pe == ps)) return -1;
        *port = (int)v;
    } else {
        *port = *use_tls ? 443 : 80;
    }

    /* path: everything from slash (inclusive) to end; default "/" */
    if (slash) {
        size_t pl = (size_t)(p - slash);
        if (pl >= path_cap) return -1;
        memcpy(path, slash, pl);
        path[pl] = '\0';
    } else {
        snprintf(path, path_cap, "/");
    }
    return 0;
}

/* resolve + connect a TCP socket to host:port, with socket timeouts so a
 * stalled server yields EAGAIN -> recv/send -1 -> http_client error. */
static int tcp_connect(const char *host, int port, int timeout_secs) {
    struct timeval tv;
    tv.tv_sec = timeout_secs > 0 ? timeout_secs : 30;
    tv.tv_usec = 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) == 1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
        return fd;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    int g = getaddrinfo(host, portstr, &hints, &res);
    if (g != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* parse {data:[{index,embedding:[...]}]} into out (row-major n*384). */
static int parse_embeddings(const char *body, size_t blen, int n, float *out,
                            char *err, size_t errlen) {
    yyjson_doc *doc = yyjson_read(body, blen, 0);
    if (!doc) { errset(err, errlen, "bad json response"); return -1; }
    int rc = -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *pdata = yyjson_obj_get(root, "data");
    if (!yyjson_is_arr(pdata)) { errset(err, errlen, "no data array"); goto done; }
    if (yyjson_arr_size(pdata) != (size_t)n) {
        char tmp[96];
        snprintf(tmp, sizeof tmp, "expected %d embeds, got %zu", n, yyjson_arr_size(pdata));
        errset(err, errlen, tmp);
        goto done;
    }
    size_t idx, imax;
    yyjson_val *item;
    yyjson_arr_foreach(pdata, idx, imax, item) {
        yyjson_val *emb = yyjson_obj_get(item, "embedding");
        if (!yyjson_is_arr(emb) || yyjson_arr_size(emb) != EMBED_DIM) {
            errset(err, errlen, "bad embedding dim");
            goto done;
        }
        float *dst = out + idx * EMBED_DIM;
        size_t di, dmax;
        yyjson_val *f;
        yyjson_arr_foreach(emb, di, dmax, f) dst[di] = (float)yyjson_get_real(f);
    }
    rc = 0;
done:
    yyjson_doc_free(doc);
    return rc;
}

/* POST req_json to <url>/embeddings; on 200, read the full body into body[]
 * (NUL-terminated, cap HTTP_MAX_BODY_BYTES).  Returns body len, or -1 (err). */
static long http_post_json(const char *url, const char *api_key,
                           const char *req_json, size_t req_len,
                           char *body, size_t body_cap, int timeout_secs,
                           char *err, size_t errlen) {
    int use_tls, port;
    char host[256];
    char path[512];
    if (parse_base_url(url, &use_tls, host, sizeof host, &port,
                       path, sizeof path) != 0) {
        errset(err, errlen, "bad DL_EMBED_API_URL"); return -1;
    }
    if (use_tls) { errset(err, errlen, "https not supported (plain http only)"); return -1; }

    char req_path[1024];
    snprintf(req_path, sizeof req_path, "%s%s", path, "/embeddings");

    int fd = tcp_connect(host, port, timeout_secs);
    if (fd < 0) { errset(err, errlen, "connect failed"); return -1; }

    HttpBIO bio;
    HttpSocket sock;
    http_socket_bio(fd, &sock, &bio);

    char host_header[512];
    int defport = use_tls ? 443 : 80;
    snprintf(host_header, sizeof host_header, port != defport ? "%s:%d" : "%s", host, port);

    char auth_header[512];
    const char *hdrs[4];
    size_t nh = 0;
    hdrs[nh++] = "Content-Type: application/json";
    if (api_key && *api_key) {
        snprintf(auth_header, sizeof auth_header, "Authorization: Bearer %s", api_key);
        hdrs[nh++] = auth_header;
    }
    hdrs[nh] = NULL;

    long rc = -1;
    HttpResponse r;
    if (http_response_init(&r, bio) != 0) { errset(err, errlen, "resp init failed"); bio.close(bio.ctx); return -1; }
    if (http_request(&r.bio, "POST", req_path, host_header, hdrs,
                     req_json, req_len) != 0 ||
        http_read_head(&r) != 0) {
        errset(err, errlen, "http request failed");
        http_response_free(&r); bio.close(bio.ctx); return -1;
    }
    if (r.status != 200) {
        char tmp[96];
        snprintf(tmp, sizeof tmp, "http status %d", r.status);
        errset(err, errlen, tmp);
        http_response_free(&r); bio.close(bio.ctx); return -1;
    }
    size_t blen = 0;
    for (;;) {
        int c = http_body_next(&r);
        if (c < 0) { errset(err, errlen, "read error"); http_response_free(&r); bio.close(bio.ctx); return -1; }
        if (c == 0) break;
        if (blen >= body_cap - 1) { errset(err, errlen, "body too big"); http_response_free(&r); bio.close(bio.ctx); return -1; }
        body[blen++] = (char)c;
    }
    body[blen] = '\0';
    http_response_free(&r);
    bio.close(bio.ctx);
    rc = (long)blen;
    return rc;
}

static int embed_batch(const char *url, const char *model, const char *api_key,
                       const char *const *texts, int n, float *out,
                       int timeout_secs, char *err, size_t errlen) {
    if (n <= 0 || n > MAX_BATCH) { errset(err, errlen, "bad batch size"); return -1; }

    /* sanitize texts to valid UTF-8 (yyjson's writer rejects invalid bytes) */
    std::vector<std::string> san;
    san.reserve(n);
    for (int i = 0; i < n; i++) {
        const char *s = texts[i] ? texts[i] : "";
        san.push_back(utf8_sanitize(s, strlen(s)));
    }

    /* build JSON body {model, input:[...]} with yyjson mut API */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "model", model);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "input", arr);
    for (int i = 0; i < n; i++) yyjson_mut_arr_add_str(doc, arr, san[i].c_str());
    size_t req_len = 0;
    const char *req_json = yyjson_mut_write(doc, 0, &req_len);
    if (!req_json) {
        errset(err, errlen, "json build failed");
        yyjson_mut_doc_free(doc);
        return -1;
    }

    char body[HTTP_MAX_BODY_BYTES];
    long blen = http_post_json(url, api_key, req_json, req_len,
                               body, sizeof body, timeout_secs, err, errlen);
    free((void*)req_json);
    yyjson_mut_doc_free(doc);
    if (blen < 0) return -1;
    if (parse_embeddings(body, (size_t)blen, n, out, err, errlen) != 0) return -1;
    return 0;
}

/* worker state for concurrent batch embedding */
struct WorkerCtx {
    const char *const *texts;
    int n;              /* total texts */
    int batch;
    float *out;
    const char *url, *model, *api_key;
    int timeout;
    std::atomic<int> next;   /* next batch start index */
    std::atomic<int> failed;
};

static void *worker_main(void *arg) {
    WorkerCtx *w = (WorkerCtx *)arg;
    for (;;) {
        int i = w->next.fetch_add(w->batch);
        if (i >= w->n) break;
        int nb = w->n - i;
        if (nb > w->batch) nb = w->batch;
        char err[512];
        if (embed_batch(w->url, w->model, w->api_key, w->texts + i, nb,
                        w->out + (size_t)i * EMBED_DIM, w->timeout,
                        err, sizeof err) != 0) {
            fprintf(stderr, "\nremote embed batch failed at %d: %s\n", i, err);
            w->failed = 1;
            break;
        }
    }
    return NULL;
}

int remote_embed(const char *const *texts, int n, float *out,
                 char *err, size_t errlen) {
    const char *url = getenv("DL_EMBED_API_URL");
    const char *model = getenv("DL_EMBED_MODEL_NAME");
    if (!model || !*model) model = "hf.co/CompendiumLabs/bge-small-en-v1.5-gguf:f16";
    const char *api_key = getenv("DL_EMBED_API_KEY");
    int batch = atoi(getenv("DL_EMBED_BATCH") ? getenv("DL_EMBED_BATCH") : "");
    if (batch <= 0) batch = 64;
    if (batch > MAX_BATCH) batch = MAX_BATCH;
    int timeout = atoi(getenv("DL_EMBED_TIMEOUT_SECS") ? getenv("DL_EMBED_TIMEOUT_SECS") : "");
    if (timeout <= 0) timeout = 30;
    int workers = atoi(getenv("DL_EMBED_WORKERS") ? getenv("DL_EMBED_WORKERS") : "");
    if (workers <= 0) workers = 3;      /* ~saturates the P40 (measured) */
    if (workers > 16) workers = 16;

    /* single-text query path: no thread overhead, run inline */
    if (n <= batch || workers <= 1) {
        for (int i = 0; i < n; i += batch) {
            int nb = n - i;
            if (nb > batch) nb = batch;
            if (embed_batch(url, model, api_key, texts + i, nb,
                            out + (size_t)i * EMBED_DIM, timeout,
                            err, errlen) != 0) return -1;
        }
        return 0;
    }

    WorkerCtx w;
    w.texts = texts; w.n = n; w.batch = batch; w.out = out;
    w.url = url; w.model = model; w.api_key = api_key; w.timeout = timeout;
    w.next = 0; w.failed = 0;

    std::vector<pthread_t> th(workers);
    int spawn = 0;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&th[i], NULL, worker_main, &w) == 0) spawn++;
        else break;
    }
    for (int i = 0; i < spawn; i++) pthread_join(th[i], NULL);
    if (w.failed) { errset(err, errlen, "remote embed worker failed"); return -1; }
    return 0;
}
