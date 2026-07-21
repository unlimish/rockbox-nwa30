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

/* One-shot probe to tell apart two very different failure modes: creating
 * a *new* file under a name that looks like code (matches the mkdir-then-
 * ENOENT-on-open symptom we're chasing) versus dlopen() itself being
 * blocked for this process regardless of where the .so lives. Runs once,
 * before the first real staging attempt. */
static void nwa30_probe_once(void)
{
    static bool done = false;
    if (done)
        return;
    done = true;

    /* Test 1: create a neutrally-named file under our own new subdirectory. */
    mkdir(NWA30_EXEC_CACHE_DIR, 0755);
    char neutral[MAX_PATH];
    snprintf(neutral, sizeof(neutral), "%s/probe.dat", NWA30_EXEC_CACHE_DIR);
    int fd = open(neutral, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        printf("nwa30_probe: open(%s) [codecache subdir]: %s\n", neutral, strerror(errno));
    else
    {
        printf("nwa30_probe: open(%s) [codecache subdir]: ok\n", neutral);
        close(fd);
    }

    /* Test 2: create a neutrally-named file directly in STAGE_DIR itself -
     * the same directory the bootloader already wrote rockbox.sony and the
     * .so libs into before exec. Tells apart "nothing new can be created
     * under STAGE_DIR by the running app" from "only the codecache
     * subdirectory specifically is blocked". */
    fd = open("/tmp/rockbox/probe.dat", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        printf("nwa30_probe: open(/tmp/rockbox/probe.dat) [STAGE_DIR itself]: %s\n",
            strerror(errno));
    else
    {
        printf("nwa30_probe: open(/tmp/rockbox/probe.dat) [STAGE_DIR itself]: ok\n");
        close(fd);
    }

    /* Test 3: explicit runtime dlopen() of a .so the bootloader already
     * staged and that is proven to work (it's linked in via DT_NEEDED at
     * exec time - ALSA is clearly functioning elsewhere in this same log).
     * The bootloader copies libs flat into STAGE_DIR, not a "lib"
     * subdirectory - see boot_rockbox() in bootloader/nwz_linux.c. */
    void *h = dlopen("/tmp/rockbox/libasound.so.2", RTLD_NOW);
    if (h == NULL)
        printf("nwa30_probe: dlopen(/tmp/rockbox/libasound.so.2, RTLD_NOW): %s\n",
            dlerror());
    else
    {
        printf("nwa30_probe: dlopen(/tmp/rockbox/libasound.so.2, RTLD_NOW): ok\n");
        dlclose(h);
    }
    fflush(stdout);
}

static const char *nwa30_stage_for_exec(const char *fpath)
{
    static char staged[MAX_PATH];
    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;

    nwa30_probe_once();

    int mkdir_rc = mkdir(NWA30_EXEC_CACHE_DIR, 0755);
    int mkdir_errno = errno;
    struct stat dst;
    int stat_rc = stat(NWA30_EXEC_CACHE_DIR, &dst);
    printf("nwa30_stage_for_exec: mkdir(%s) -> %d (%s); stat -> %d (%s)%s\n",
        NWA30_EXEC_CACHE_DIR, mkdir_rc, mkdir_rc < 0 ? strerror(mkdir_errno) : "ok",
        stat_rc, stat_rc < 0 ? strerror(errno) : "ok",
        stat_rc == 0 ? (S_ISDIR(dst.st_mode) ? ", isdir" : ", NOT a dir") : "");
    fflush(stdout);
    if (mkdir_rc < 0 && mkdir_errno != EEXIST)
        return fpath;
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
