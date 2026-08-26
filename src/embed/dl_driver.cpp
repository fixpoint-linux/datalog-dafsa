/*
 * dl_driver.cpp — fork/execv driver for ./dl (see dl_driver.h).
 */
#include "dl_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int dld_run(const char *dl_path, char *const argv[], char *out, size_t out_cap) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        /* child: stdout -> pipe, stderr inherited */
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[0]);
        close(fds[1]);
        execv(dl_path, argv);
        _exit(127);
    }
    close(fds[1]);
    size_t got = 0;
    if (out && out_cap) {
        out[0] = '\0';
        for (;;) {
            if (got + 1 >= out_cap) break;
            ssize_t r = read(fds[0], out + got, out_cap - 1 - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        out[got] = '\0';
    }
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int dld_prefix(const char *dl_path, const char *db, const char *rel,
               int raw, const char *leading, char *out, size_t out_cap,
               int *n_lines) {
    char *argv[16];
    int i = 0;
    argv[i++] = (char *)"dl";
    argv[i++] = (char *)"-d";
    argv[i++] = (char *)db;
    argv[i++] = (char *)"prefix";
    argv[i++] = (char *)rel;           /* CLI order: prefix <rel> [--raw] */
    if (raw) argv[i++] = (char *)"--raw";
    if (leading) argv[i++] = (char *)leading;
    argv[i] = NULL;
    if (dld_run(dl_path, argv, out, out_cap) != 0) return -1;
    /* split into NUL-separated lines */
    int n = 0;
    for (char *p = out; *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        n++;
        if (!nl) break;
        p = nl + 1;
    }
    if (n_lines) *n_lines = n;
    return 0;
}

int dld_load(const char *dl_path, const char *db, const char *csv,
             const char *rel) {
    char *argv[16];
    int i = 0;
    argv[i++] = (char *)"dl";
    argv[i++] = (char *)"-d";
    argv[i++] = (char *)db;
    argv[i++] = (char *)"load";
    argv[i++] = (char *)csv;
    argv[i++] = (char *)"--rel";
    argv[i++] = (char *)rel;
    argv[i] = NULL;
    char sink[256];
    return dld_run(dl_path, argv, sink, sizeof sink) == 0 ? 0 : -1;
}

/* A tuple that was truncated at the 16 MiB out_buf boundary would parse as a
 * partial/empty row — reject it rather than silently emitting a wrong corpus. */
#define DLD_PREFIX_RAW_CAP (16 << 20)

int dld_prefix_raw(const char *dl_path, const char *db, const char *rel,
                   uint32_t **cols_out, int *n_rows_out, uint8_t *arity_out) {
    char *out = (char *)malloc(DLD_PREFIX_RAW_CAP);
    if (!out) return -1;
    out[0] = '\0';
    char *argv[8];
    int i = 0;
    argv[i++] = (char *)"dl";
    argv[i++] = (char *)"-d";
    argv[i++] = (char *)db;
    argv[i++] = (char *)"prefix";
    argv[i++] = (char *)rel;           /* CLI order: prefix <rel> [--raw] */
    argv[i++] = (char *)"--raw";
    argv[i] = NULL;
    int rc = dld_run(dl_path, argv, out, DLD_PREFIX_RAW_CAP);
    if (rc != 0) { free(out); return -1; }

    /* count rows + columns; detect truncation (last byte would be a digit). */
    int n_rows = 0;
    int arity = 0;
    uint8_t col_this_row = 0;
    int got_cap = 0;
    for (char *p = out; *p; ) {
        char *nl = strchr(p, '\n');
        if (!nl) { got_cap = 1; break; }   /* unterminated -> buffer was full */
        char *q = p;
        uint8_t c = 0;
        /* strip trailing \r */
        size_t l = (size_t)(nl - q);
        while (l > 0 && (q[l-1] == '\r')) l--;
        /* count whitespace-separated tokens; also require all-u32 */
        size_t pos = 0;
        while (pos < l) {
            while (pos < l && (q[pos] == ' ' || q[pos] == '\t')) pos++;
            if (pos >= l) break;
            unsigned long long val = 0;
            int any = 0;
            while (pos < l && q[pos] >= '0' && q[pos] <= '9') {
                if (val > 0xFFFFFFFFu / 10u) { free(out); return -1; }
                val = val * 10u + (unsigned long long)(q[pos] - '0');
                pos++; any = 1;
            }
            if (!any) { free(out); return -1; }   /* non-numeric token */
            if (val > 0xFFFFFFFFu) { free(out); return -1; }
            c++;
        }
        if (c == 0) { free(out); return -1; }     /* blank line (corrupt) */
        if (arity == 0) arity = c;
        else if ((int)c != arity) { free(out); return -1; }
        col_this_row = c;
        n_rows++;
        p = nl + 1;
    }
    if (got_cap && n_rows == 0) { free(out); return -1; }
    /* no trailing-newline guard: if we never saw a terminator, it was full */
    size_t olen = strlen(out);
    if (olen > 0 && olen + 1 >= DLD_PREFIX_RAW_CAP) { free(out); return -1; }

    /* second pass: parse into a row-major buffer. */
    uint32_t *cols = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)arity * (size_t)n_rows);
    if (!cols) { free(out); return -1; }
    int r = 0;
    for (char *p = out; *p && r < n_rows; ) {
        char *nl = strchr(p, '\n');
        char *q = p;
        size_t l = (size_t)(nl ? nl - q : strlen(q));
        while (l > 0 && (q[l-1] == '\r')) l--;
        size_t pos = 0;
        int ci = 0;
        while (pos < l && ci < arity) {
            while (pos < l && (q[pos] == ' ' || q[pos] == '\t')) pos++;
            if (pos >= l) break;
            uint32_t val = 0;
            while (pos < l && q[pos] >= '0' && q[pos] <= '9') {
                val = val * 10u + (uint32_t)(q[pos] - '0');
                pos++;
            }
            cols[(size_t)r * (size_t)arity + (size_t)ci] = val;
            ci++;
        }
        (void)col_this_row;
        r++;
        if (!nl) break;
        p = nl + 1;
    }
    free(out);
    *cols_out = cols;
    *n_rows_out = n_rows;
    if (arity_out) *arity_out = (uint8_t)arity;
    return 0;
}

int dld_publish(const char *dl_path, const char *db) {
    char *argv[16];
    int i = 0;
    argv[i++] = (char *)"dl";
    argv[i++] = (char *)"-d";
    argv[i++] = (char *)db;
    argv[i++] = (char *)"publish";
    argv[i] = NULL;
    char sink[256];
    return dld_run(dl_path, argv, sink, sizeof sink) == 0 ? 0 : -1;
}

int dld_symbols_array(const char *db, char ***names_out, uint32_t **ids_out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/symbols.array", db);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t cap = 64, n = 0;
    char **names = (char **)malloc(cap * sizeof(char *));
    uint32_t *ids = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!names || !ids) { free(names); free(ids); fclose(f); return -1; }
    /* getline: observation-content strings can exceed any fixed line buffer,
       and a truncated line would shift every subsequent sym-id. */
    char *line = NULL;
    size_t linecap = 0;
    ssize_t len;
    uint32_t sym = 0;
    while ((len = getline(&line, &linecap, f)) >= 0) {
        sym++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;              /* empty slot */
        if (n >= cap) {
            cap *= 2;
            char **nn = (char **)realloc(names, cap * sizeof(char *));
            uint32_t *ni = (uint32_t *)realloc(ids, cap * sizeof(uint32_t));
            if (!nn || !ni) { free(nn ? nn : names); free(ni ? ni : ids); free(line); fclose(f); return -1; }
            names = nn; ids = ni;
        }
        names[n] = strdup(line);
        ids[n] = sym;
        n++;
    }
    free(line);
    fclose(f);
    *names_out = names;
    *ids_out = ids;
    return (int)n;
}
