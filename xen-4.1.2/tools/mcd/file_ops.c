/******************************************************************************
 * tools/xenpaging/file_ops.c
 *
 * Common file operations.
 *
 * Copyright (c) 2009 by Citrix Systems, Inc. (Patrick Colp)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <unistd.h>
#include <stdarg.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <xc_private.h>

#include "file_ops.h"

#define page_offset(_pfn)     (((off_t)(_pfn)) << PAGE_SHIFT)

static int file_op(int fd, void *page, int i,
                   ssize_t (*fn)(int, const void *, size_t))
{
    off_t seek_ret;
    int total;
    int bytes;
    int ret;

    seek_ret = lseek(fd, i << PAGE_SHIFT, SEEK_SET);

    total = 0;
    while ( total < PAGE_SIZE )
    {
        bytes = fn(fd, page + total, PAGE_SIZE - total);
        if ( bytes <= 0 )
        {
            ret = -errno;
            goto err;
        }

        total += bytes;
    }

    return 0;

 err:
    return ret;
}

ssize_t my_read(int fd, const void *buf, size_t count)
{
    return read(fd, (void *)buf, count);
}

ssize_t find_and_read(int fd, const void *buf, size_t start_byte, size_t count)
{
    lseek(fd, start_byte, SEEK_SET);
    return read(fd, (void *)buf, count);
}

ssize_t my_write(int fd, const void *buf, size_t count)
{
    return write(fd, (void *)buf, count);
}

int read_page(int fd, void *page, int i)
{
    return file_op(fd, page, i, &my_read);
}

int write_page(int fd, void *page, int i)
{
    return file_op(fd, page, i, &write);
}

int chk_dir_and_create(char *dir)
{
    struct stat st;
    if(stat(dir,&st) != 0) {
        mkdir(dir, S_IRWXU | S_IRWXG | S_IRWXO); // 777
    }

    return 0;
}

int chk_dir_and_clean_create(char *dir)
{
    struct stat st;
    if(stat(dir,&st) != 0) { // does not exist
        mkdir(dir, S_IRWXU | S_IRWXG | S_IRWXO); // 777
    } else { // exist
        del_dir(dir);
        mkdir(dir, S_IRWXU | S_IRWXG | S_IRWXO); // 777
    }

    return 0;
}

int del_dir(char *path) 
{
    DIR *d = opendir(path);
    size_t path_len = strlen(path);
    int r = -1;

    if (d) {
        struct dirent *p;
        r = 0;

        while (!r && (p=readdir(d))) {
            int r2 = -1;
            char *buf;
            size_t len;

            /* Skip the names "." and ".." as we don't want to recurse on them. */
            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
                continue;

            len = path_len + strlen(p->d_name) + 2; 
            buf = malloc(len);

            if (buf) {
                struct stat statbuf;
                snprintf(buf, len, "%s/%s", path, p->d_name);
                if (!stat(buf, &statbuf)) {
                    if (S_ISDIR(statbuf.st_mode)) {
                        r2 = del_dir(buf);
                    }
                    else {
                        r2 = unlink(buf);
                    }
                }
                free(buf);
            }
            r = r2;
        }
        closedir(d);
    }

    if (!r) {
        r = rmdir(path);
    }

    return r;
}

int del_file(char *file)
{
    return unlink(file);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End: 
 */
