/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2010 by Thomas Martitz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <string.h> /* size_t */
#include <dlfcn.h>
#include "file.h"
#include "debug.h"
#include "load_code.h"
#ifdef SONY_NWA30
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* Codecs are loaded from a copy the bootloader put in tmpfs, not from
 * /contents. That partition rejects PROT_EXEC mmap, so dlopen() straight off
 * it fails with EPERM - and the running rockbox.sony cannot stage the file
 * itself either: it may create directories under /tmp/rockbox but not files,
 * even in the very directory the bootloader wrote rockbox.sony into moments
 * earlier (open(O_CREAT) gives ENOENT while mkdir() and stat() agree the parent
 * exists). dlopen() of an already-staged library works, so what is denied is
 * file creation rather than dynamic loading. See copy_dir_flat() in
 * bootloader/nwz_linux.c for the other half of this. */
#define NWA30_STAGED_CODECS_DIR "/tmp/rockbox/codecs"

static const char *nwa30_staged_path(const char *fpath)
{
    static char staged[MAX_PATH];
    const char *name = strrchr(fpath, '/');
    name = name ? name + 1 : fpath;
    snprintf(staged, sizeof(staged), "%s/%s", NWA30_STAGED_CODECS_DIR, name);
    if (access(staged, R_OK) == 0)
        return staged;
    printf("nwa30_staged_path: %s not staged (%s), falling back to %s\n",
        staged, strerror(errno), fpath);
    fflush(stdout);
    return fpath; /* let dlopen() report the real error */
}
#endif

void *lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    (void)buf;
    (void)buf_size;
    char path[MAX_PATH];

    const char *fpath = handle_special_dirs(filename, 0, path, sizeof(path));
#ifdef SONY_NWA30
    fpath = nwa30_staged_path(fpath);
#endif

    void *handle = dlopen(fpath, RTLD_NOW);
    if (handle == NULL)
    {
        DEBUGF("failed to load %s\n", filename);
        DEBUGF("lc_open(%s): %s\n", filename, dlerror());
#ifdef SONY_NWA30
        /* DEBUGF is compiled out of release builds, which made this invisible:
         * a codec would fail to load and playback would just skip the track. */
        printf("lc_open(%s): %s\n", fpath, dlerror());
        fflush(stdout);
#endif
    }
    return handle;
}

void *lc_get_header(void *handle)
{
    char *ret = dlsym(handle, "__header");
    if (ret == NULL)
        ret = dlsym(handle, "___header");

    return ret;
}

void lc_close(void *handle)
{
    dlclose(handle);
}
