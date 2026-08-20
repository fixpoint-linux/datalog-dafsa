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
    if (raw) argv[i++] = (char *)"--raw";
    argv[i++] = (char *)rel;
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
    char line[4096];
    uint32_t sym = 0;
    while (fgets(line, sizeof line, f)) {
        sym++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;              /* empty slot */
        if (n >= cap) {
            cap *= 2;
            char **nn = (char **)realloc(names, cap * sizeof(char *));
            uint32_t *ni = (uint32_t *)realloc(ids, cap * sizeof(uint32_t));
            if (!nn || !ni) { free(nn ? nn : names); free(ni ? ni : ids); fclose(f); return -1; }
            names = nn; ids = ni;
        }
        names[n] = strdup(line);
        ids[n] = sym;
        n++;
    }
    fclose(f);
    *names_out = names;
    *ids_out = ids;
    return (int)n;
}
