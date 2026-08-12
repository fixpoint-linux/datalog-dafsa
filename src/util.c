/*
 * util.c — Shared utility helpers: atomic writes, directory fsync
 */
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ─── atomic_write_str ────────────────────────────────────────────────── */

int atomic_write_str(const char *path, const char *content)
{
    char tmp[8192];
    int fd, ret;
    size_t len = strlen(content);
    size_t off = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        if (w < 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }

    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    ret = fsync_dir_of_path(path);
    if (ret != 0) return -1;
    return 0;
}

/* ─── Directory fsync ─────────────────────────────────────────────────── */

int fsync_dir_of_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    int fd, ret = -1;

    if (!slash) {
        /* path has no '/' — fsync current directory */
        fd = open(".", O_RDONLY | O_DIRECTORY);
        if (fd >= 0) { ret = fsync(fd); close(fd); }
        return ret;
    }
    if (slash == path) {
        /* path is "/foo" */
        fd = open("/", O_RDONLY | O_DIRECTORY);
        if (fd >= 0) { ret = fsync(fd); close(fd); }
        return ret;
    }
    dir = strndup(path, (size_t)(slash - path));
    if (!dir) return -1;
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { ret = fsync(fd); close(fd); }
    free(dir);
    return ret;
}

int fsync_dir_path(const char *dirpath)
{
    int fd = open(dirpath, O_RDONLY | O_DIRECTORY);
    int ret = -1;
    if (fd >= 0) { ret = fsync(fd); close(fd); }
    return ret;
}
