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

/* /contents (FAT/exFAT) rejects PROT_EXEC mmap, so dlopen() on a codec
 * loaded straight from there fails with EPERM. Staging the file into
 * tmpfs *at runtime*, from inside the running rockbox.sony process, does
 * not work either: a probe confirmed the process can create directories
 * under /tmp/rockbox but not new files, even directly in STAGE_DIR where
 * the bootloader itself wrote rockbox.sony and the .so libs moments
 * earlier (ENOENT on open(O_CREAT), despite mkdir()+stat() agreeing the
 * parent directory exists) - a permission model tied to which process
 * created the file, not a filesystem-state issue. An explicit dlopen() of
 * an already-staged file (libasound.so.2) worked fine, so it is file
 * *creation* that is being denied here, not dynamic loading in general.
 *
 * So the bootloader stages the codecs into tmpfs up front, the same way
 * it already does for rockbox.sony and its libraries - see
 * copy_dir_flat(ROCKBOX_CODECS_DIR, STAGE_CODECS_DIR) in
 * bootloader/nwz_linux.c. This just looks up that pre-staged copy. */
#define NWA30_STAGED_CODECS_DIR "/tmp/rockbox/codecs"

static const char *nwa30_staged_path(const char *fpath)
{
    static char staged[MAX_PATH];
    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;
    snprintf(staged, sizeof(staged), "%s/%s", NWA30_STAGED_CODECS_DIR, base);
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
        /* DEBUGF is compiled out in release builds, which is why this
         * failure was invisible: codecs would silently fail to load and
         * playback would just skip the track. Say why dlopen rejected it. */
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
