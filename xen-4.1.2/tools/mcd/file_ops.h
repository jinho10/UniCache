/******************************************************************************
 * tools/xenpaging/file_ops.h
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


#ifndef __FILE_OPS_H__
#define __FILE_OPS_H__

int read_page(int fd, void *page, int i);
int write_page(int fd, void *page, int i);

ssize_t find_and_read(int fd, const void *buf, size_t start_byte, size_t count);
ssize_t my_read(int fd, const void *buf, size_t count);
ssize_t my_write(int fd, const void *buf, size_t count);

int chk_dir_and_create(char *dir);
int chk_dir_and_clean_create(char *dir);
int del_dir(char *path);
int del_file(char *file);

#endif


/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End: 
 */