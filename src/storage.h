#pragma once

#include <stddef.h>

int      fs_exists(const char *path);
int      fs_is_dir(const char *path);
int      fs_mkdir_p(const char *path);

long long fs_size(const char *path);
long long fs_free(const char *path);

/* Fills out with the raw statfs counters, for the log. */
void      fs_space_debug(const char *path, char *out, size_t out_len);
int       fs_fstype(const char *path, char *out, size_t out_len);

/* Finds the first *.{tar.xz,tar.gz,tar.zst} directly inside dir, skipping one
 * that is already called distro.tar.*. Returns 1 and fills out on success. */
int fs_find_archive(const char *dir, char *out, size_t out_len);

/* Returns 1 when a file can actually be created inside dir. */
int fs_test_write(const char *dir);

void fs_join(char *out, size_t out_len, const char *dir, const char *name);
