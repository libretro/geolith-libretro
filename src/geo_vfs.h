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

#ifndef GEO_VFS_H
#define GEO_VFS_H

#include <stddef.h>
#include <stdint.h>

/* File access modes passed to the open operation */
#define GEO_VFS_READ   0
#define GEO_VFS_WRITE  1

/* Seek origins - deliberately numbered to match the values used by common
   virtual filesystem implementations, so a frontend shim is a pass-through.
*/
#define GEO_VFS_SEEK_SET 0
#define GEO_VFS_SEEK_CUR 1
#define GEO_VFS_SEEK_END 2

/* File I/O operations used by the core.

   All handles are opaque - the core never inspects them, it only hands them
   back to the implementation which produced them.

   open:    Return an opaque handle, or NULL on failure.
   close:   Return 0 on success, -1 on failure.
   read:    Return the number of bytes read, or -1 on failure.
   write:   Return the number of bytes written, or -1 on failure.
   seek:    Return the resulting absolute offset, or -1 on failure.
   tell:    Return the current absolute offset, or -1 on failure.
   size:    Return the size of the file in bytes, or -1 on failure. Must not
            disturb the current file position.
   resolve: Join a path relative to the directory containing base, writing the
            result to out. Return 1 on success, 0 on failure. May be NULL, in
            which case a built-in string join is used - frontends dealing in
            paths that are not plain filesystem paths (Android SAF content
            URIs, for example) should supply one.
*/
typedef struct {
    void*   (*open)(const char *path, unsigned mode);
    int     (*close)(void *file);
    int64_t (*read)(void *file, void *data, int64_t len);
    int64_t (*write)(void *file, const void *data, int64_t len);
    int64_t (*seek)(void *file, int64_t offset, int origin);
    int64_t (*tell)(void *file);
    int64_t (*size)(void *file);
    int     (*resolve)(char *out, size_t outsz, const char *base,
                       const char *rel);
} geo_vfs_t;

/* Install a set of file I/O operations, replacing the built-in stdio
   implementation. Pass NULL to restore the built-in implementation. Must be
   called before any content is loaded. The table is not copied, so it must
   remain valid for as long as it is installed.
*/
void geo_vfs_set_callbacks(const geo_vfs_t *vfs);

void*   geo_vfs_open(const char *path, unsigned mode);
int     geo_vfs_close(void *file);
int64_t geo_vfs_read(void *file, void *data, int64_t len);
int64_t geo_vfs_write(void *file, const void *data, int64_t len);
int64_t geo_vfs_seek(void *file, int64_t offset, int origin);
int64_t geo_vfs_tell(void *file);
int64_t geo_vfs_size(void *file);
int     geo_vfs_resolve(char *out, size_t outsz, const char *base,
                        const char *rel);

/* Read an entire file into a newly allocated buffer, which the caller must
   free. Returns NULL on failure. The buffer is NUL-terminated as a
   convenience for text data; the size written to the size pointer does not
   include the terminator.
*/
void* geo_vfs_read_file(const char *path, size_t *size);

#endif
