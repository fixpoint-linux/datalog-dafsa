/*
 * util.h — Shared utility helpers (atomic writes, fsync, etc.)
 */
#ifndef UTIL_H
#define UTIL_H

/* Write a string to a file with atomic rename (tmp+fsync+rename+dir-fsync).
 * Returns 0 on success, -1 on error (tmp cleaned up on failure). */
int atomic_write_str(const char *path, const char *content);

/* fsync the directory containing `path` (to make a prior rename durable).
 * Returns 0 on success, -1 on error. */
int fsync_dir_of_path(const char *path);

/* fsync the directory at `dirpath`. */
int fsync_dir_path(const char *dirpath);

#endif /* UTIL_H */
