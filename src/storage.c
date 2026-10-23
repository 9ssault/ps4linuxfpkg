#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>

int fs_exists(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0;
}

int fs_is_dir(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fs_mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;
    size_t i;

    if (!path || !*path)
        return 0;

    len = strlen(path);
    if (len >= sizeof(tmp))
        return 0;

    memcpy(tmp, path, len + 1);

    for (i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;

        tmp[i] = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
            tmp[i] = '/';
            return 0;
        }
        tmp[i] = '/';
    }

    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return 0;

    return fs_is_dir(path);
}

long long fs_size(const char *path)
{
    struct stat st;

    if (!path || stat(path, &st) != 0)
        return -1;

    return (long long)st.st_size;
}

long long fs_free(const char *path)
{
    struct statfs st;

    if (!path || statfs(path, &st) != 0)
        return -1;

    return (long long)st.f_bavail * (long long)st.f_bsize;
}

int fs_fstype(const char *path, char *out, size_t out_len)
{
    struct statfs st;

    if (!out || out_len == 0)
        return 0;

    out[0] = '\0';

    if (!path || statfs(path, &st) != 0)
        return 0;

    snprintf(out, out_len, "%s", st.f_fstypename);
    return 1;
}

static int has_archive_suffix(const char *name)
{
    return strstr(name, ".tar.xz") || strstr(name, ".tar.gz") || strstr(name, ".tar.zst");
}

int fs_find_archive(const char *dir, char *out, size_t out_len)
{
    DIR *d;
    struct dirent *ent;

    if (!dir || !out)
        return 0;

    d = opendir(dir);
    if (!d)
        return 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        if (!has_archive_suffix(ent->d_name))
            continue;

        fs_join(out, out_len, dir, ent->d_name);
        closedir(d);
        return 1;
    }

    closedir(d);
    return 0;
}

int fs_test_write(const char *dir)
{
    char probe[512];
    FILE *fp;

    if (!dir)
        return 0;

    fs_join(probe, sizeof(probe), dir, ".ps4li_probe");

    fp = fopen(probe, "wb");
    if (!fp)
        return 0;

    fputc('x', fp);
    fclose(fp);
    remove(probe);

    return 1;
}

void fs_join(char *out, size_t out_len, const char *dir, const char *name)
{
    size_t len;

    if (!out || out_len == 0)
        return;

    snprintf(out, out_len, "%s", dir ? dir : "");

    len = strlen(out);
    if (len > 0 && out[len - 1] != '/' && len + 1 < out_len) {
        out[len] = '/';
        out[len + 1] = '\0';
    }

    strncat(out, name ? name : "", out_len - strlen(out) - 1);
}
