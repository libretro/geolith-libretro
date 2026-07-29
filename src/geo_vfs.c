/*
Copyright (c) 2026 Rupert Carmichael
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geo_vfs.h"

/* Built-in stdio implementation.

   Offsets are handled with long rather than a 64-bit type, which caps files
   at 2GB on platforms with a 32-bit long. This is comfortably beyond any Neo
   Geo CD image. Frontends which need more can install their own operations.
*/
static int geo_vfs_stdio_origin(int origin) {
    switch (origin) {
        case GEO_VFS_SEEK_CUR: return SEEK_CUR;
        case GEO_VFS_SEEK_END: return SEEK_END;
        default: return SEEK_SET;
    }
}

static void* geo_vfs_stdio_open(const char *path, unsigned mode) {
    return (void*)fopen(path, mode == GEO_VFS_WRITE ? "wb" : "rb");
}

static int geo_vfs_stdio_close(void *file) {
    return fclose((FILE*)file) ? -1 : 0;
}

static int64_t geo_vfs_stdio_read(void *file, void *data, int64_t len) {
    if (len < 0)
        return -1;
    return (int64_t)fread(data, 1, (size_t)len, (FILE*)file);
}

static int64_t geo_vfs_stdio_write(void *file, const void *data, int64_t len) {
    if (len < 0)
        return -1;
    return (int64_t)fwrite(data, 1, (size_t)len, (FILE*)file);
}

static int64_t geo_vfs_stdio_seek(void *file, int64_t offset, int origin) {
    if (fseek((FILE*)file, (long)offset, geo_vfs_stdio_origin(origin)))
        return -1;
    return (int64_t)ftell((FILE*)file);
}

static int64_t geo_vfs_stdio_tell(void *file) {
    return (int64_t)ftell((FILE*)file);
}

static int64_t geo_vfs_stdio_size(void *file) {
    FILE *fp = (FILE*)file;
    long cur = ftell(fp);
    long size;

    if (cur < 0 || fseek(fp, 0, SEEK_END))
        return -1;

    size = ftell(fp);
    fseek(fp, cur, SEEK_SET);

    return (int64_t)size;
}

/* Determine whether a path found inside a cue sheet is already absolute */
static int geo_vfs_path_is_abs(const char *path) {
    if (path[0] == '/' || path[0] == '\\')
        return 1;

    /* Windows drive letter */
    if (path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\'))
        return 1;

    return 0;
}

static int geo_vfs_stdio_resolve(char *out, size_t outsz, const char *base,
    const char *rel) {

    if (!out || !outsz || !rel)
        return 0;

    if (!base || geo_vfs_path_is_abs(rel)) {
        if (strlen(rel) >= outsz)
            return 0;
        snprintf(out, outsz, "%s", rel);
        return 1;
    }

    /* Find the last path separator in the base path */
    size_t dirlen = 0;
    for (size_t i = 0; base[i]; ++i) {
        if (base[i] == '/' || base[i] == '\\')
            dirlen = i + 1;
    }

    if (dirlen + strlen(rel) >= outsz)
        return 0;

    memcpy(out, base, dirlen);
    snprintf(out + dirlen, outsz - dirlen, "%s", rel);

    return 1;
}

static const geo_vfs_t geo_vfs_stdio = {
    geo_vfs_stdio_open,
    geo_vfs_stdio_close,
    geo_vfs_stdio_read,
    geo_vfs_stdio_write,
    geo_vfs_stdio_seek,
    geo_vfs_stdio_tell,
    geo_vfs_stdio_size,
    geo_vfs_stdio_resolve
};

static const geo_vfs_t *vfs = &geo_vfs_stdio;

void geo_vfs_set_callbacks(const geo_vfs_t *newvfs) {
    vfs = newvfs ? newvfs : &geo_vfs_stdio;
}

void* geo_vfs_open(const char *path, unsigned mode) {
    return vfs->open(path, mode);
}

int geo_vfs_close(void *file) {
    return file ? vfs->close(file) : -1;
}

int64_t geo_vfs_read(void *file, void *data, int64_t len) {
    return vfs->read(file, data, len);
}

int64_t geo_vfs_write(void *file, const void *data, int64_t len) {
    return vfs->write(file, data, len);
}

int64_t geo_vfs_seek(void *file, int64_t offset, int origin) {
    return vfs->seek(file, offset, origin);
}

int64_t geo_vfs_tell(void *file) {
    return vfs->tell(file);
}

int64_t geo_vfs_size(void *file) {
    return vfs->size(file);
}

int geo_vfs_resolve(char *out, size_t outsz, const char *base,
    const char *rel) {

    if (vfs->resolve)
        return vfs->resolve(out, outsz, base, rel);
    return geo_vfs_stdio_resolve(out, outsz, base, rel);
}

void* geo_vfs_read_file(const char *path, size_t *size) {
    void *file = geo_vfs_open(path, GEO_VFS_READ);
    if (!file)
        return NULL;

    int64_t sz = geo_vfs_size(file);
    if (sz <= 0) {
        geo_vfs_close(file);
        return NULL;
    }

    uint8_t *buf = (uint8_t*)malloc((size_t)sz + 1);
    if (!buf) {
        geo_vfs_close(file);
        return NULL;
    }

    if (geo_vfs_seek(file, 0, GEO_VFS_SEEK_SET) < 0 ||
        geo_vfs_read(file, buf, sz) != sz) {
        free(buf);
        geo_vfs_close(file);
        return NULL;
    }

    geo_vfs_close(file);

    buf[sz] = '\0'; // Convenience for text data, not counted in the size

    if (size)
        *size = (size_t)sz;

    return (void*)buf;
}
