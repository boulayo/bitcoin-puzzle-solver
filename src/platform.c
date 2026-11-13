/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* platform.c - what the two implementations of platform.h share: the removal of a directory tree, written on the directory functions that
 * POSIX and MinGW-w64 both have, over the pz_is_own_dir of the platform. The implementations are platform_posix.c and platform_win.c. */
#include "platform.h"
#include <dirent.h>
#include <unistd.h>

/* A link is removed, never followed: what it points at is not part of the tree (and may be someone else's). */
int pz_remove_tree(const char *path)
{
    if (!pz_is_own_dir(path)) return remove(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t l = strlen(path) + strlen(e->d_name) + 2;
        char *p = pz_xmalloc(l);
        snprintf(p, l, "%s/%s", path, e->d_name);
        if (pz_remove_tree(p) != 0) rc = -1;
        free(p);
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}
