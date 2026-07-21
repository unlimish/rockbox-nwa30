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
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

/* /contents is the FAT/exFAT user partition; it cannot mmap files with
 * PROT_EXEC, so dlopen() on a codec or plugin loaded straight from there
 * fails with EPERM ("failed to map segment from shared object"). This is
 * the same restriction that forced rockbox.sony itself to be staged into
 * tmpfs by the bootloader before exec - do the same here, on demand, for
 * whichever .codec/.rock file is being loaded. */
#define NWA30_EXEC_CACHE_DIR "/tmp/rockbox/codecache"

static const char *nwa30_stage_for_exec(const char *fpath)
{
    static char staged[MAX_PATH];
    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;

    if (mkdir(NWA30_EXEC_CACHE_DIR, 0755) < 0 && errno != EEXIST)
    {
        printf("nwa30_stage_for_exec: mkdir(%s): %s\n",
            NWA30_EXEC_CACHE_DIR, strerror(errno));
        fflush(stdout);
        return fpath;
    }
    snprintf(staged, sizeof(staged), "%s/%s", NWA30_EXEC_CACHE_DIR, base);

    int in = open(fpath, O_RDONLY);
    if (in < 0)
    {
        printf("nwa30_stage_for_exec: open(%s): %s\n", fpath, strerror(errno));
        fflush(stdout);
        return fpath; /* let dlopen() report the real error */
    }
    int out = open(staged, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0)
    {
        printf("nwa30_stage_for_exec: open(%s): %s\n", staged, strerror(errno));
        fflush(stdout);
        close(in);
        return fpath;
    }
    char buf[16384];
    ssize_t len;
    bool ok = true;
    while (ok && (len = read(in, buf, sizeof(buf))) > 0)
    {
        ssize_t off = 0;
        while (off < len)
        {
            ssize_t wr = write(out, buf + off, len - off);
            if (wr <= 0)
            {
                printf("nwa30_stage_for_exec: write(%s): %s\n", staged,
                    strerror(errno));
                fflush(stdout);
                ok = false;
                break;
            }
            off += wr;
        }
    }
    close(in);
    close(out);
    if (!ok)
        return fpath;
    /* tmpfs files land 0644 regardless of the open() mode above; dlopen()
     * needs the exec bit or the PROT_EXEC mmap gets rejected the same way
     * the original /contents copy was. */
    if (chmod(staged, 0755) < 0)
    {
        printf("nwa30_stage_for_exec: chmod(%s): %s\n", staged, strerror(errno));
        fflush(stdout);
    }
    return staged;
}
#endif

void *lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    (void)buf;
    (void)buf_size;
    char path[MAX_PATH];

    const char *fpath = handle_special_dirs(filename, 0, path, sizeof(path));
#ifdef SONY_NWA30
    fpath = nwa30_stage_for_exec(fpath);
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
