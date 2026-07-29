/*
Copyright (c) 2026 Romain Tisserand
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
#include <ctype.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO // All file access goes through the VFS layer
#include <dr/dr_flac.h>

#include "geo.h"
#include "geo_cue.h"
#include "geo_disc.h"
#include "geo_vfs.h"

#define MAX_FILES 99
#define MAX_TRACKS GEO_DISC_MAX_TRACKS

// File types
#define FTYPE_BIN  0  // Raw 2352-byte sectors (BIN)
#define FTYPE_ISO  1  // 2048-byte MODE1 sectors
#define FTYPE_WAV  2  // PCM WAV audio
#define FTYPE_FLAC 3  // FLAC audio

typedef struct {
    void *fp;           // VFS file handle
    char path[1024];
    int type;           // FTYPE_*
    uint32_t data_off;  // Offset to PCM data (WAV header skip)
    drflac *flac;       // FLAC decoder handle (FTYPE_FLAC only)
} cue_file_t;

typedef struct {
    int file_idx;           // Index into files[]
    uint32_t file_offset;   // Byte offset within file for start of track
    uint32_t sector_size;   // 2352 (BIN/audio) or 2048 (ISO)
    uint8_t type;           // GEO_DISC_TRACK_DATA or GEO_DISC_TRACK_AUDIO
    uint32_t start;         // CD position (LBA, what BIOS sees) — index 01
    uint32_t frames;        // Number of frames (index 01 length)
    uint32_t pregap;        // Pregap frames (from PREGAP or INDEX 00)
} cue_track_t;

static cue_file_t files[MAX_FILES];
static unsigned num_files = 0;

static cue_track_t tracks[MAX_TRACKS];
static unsigned num_tracks = 0;
static uint32_t leadout_lba = 0;

// FLAC last-read state to avoid redundant seeks
static int flac_last_track = -1;
static uint64_t flac_last_frame = 0;

// Compare two strings ignoring case in a Jolly Good manner
static int strjcasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int d = toupper((unsigned char)a[i]) - toupper((unsigned char)b[i]);
        if (d)
            return d;
        if (!a[i])
            return 0;
    }
    return 0;
}

// Detect type of file being opened
static int geo_cue_detect_file_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext)
        return FTYPE_BIN;
    if (!strjcasecmp(ext, ".iso", 4))
        return FTYPE_ISO;
    if (!strjcasecmp(ext, ".wav", 4))
        return FTYPE_WAV;
    if (!strjcasecmp(ext, ".flac", 5))
        return FTYPE_FLAC;
    return FTYPE_BIN;
}

// Parse WAV header, return offset to PCM data. 0 on failure.
static uint32_t geo_cue_wav_parse_header(void *fp) {
    uint8_t hdr[44];
    if (geo_vfs_read(fp, hdr, 44) != 44)
        return 0;

    // Check RIFF header
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        return 0;

    // Walk chunks to find "data"
    if (geo_vfs_seek(fp, 12, GEO_VFS_SEEK_SET) < 0)
        return 0;
    while (1) {
        uint8_t chunk[8];
        if (geo_vfs_read(fp, chunk, 8) != 8)
            return 0;
        uint32_t chunk_size = chunk[4] | (chunk[5] << 8) |
                              (chunk[6] << 16) | (chunk[7] << 24);
        if (!memcmp(chunk, "data", 4))
            return (uint32_t)geo_vfs_tell(fp);
        if (geo_vfs_seek(fp, chunk_size, GEO_VFS_SEEK_CUR) < 0)
            return 0;
    }
}

// dr_flac read callback, backed by the VFS layer
static size_t geo_cue_flac_read(void *userdata, void *buf, size_t len) {
    int64_t ret = geo_vfs_read(userdata, buf, (int64_t)len);
    return ret < 0 ? 0 : (size_t)ret;
}

// dr_flac seek callback, backed by the VFS layer
static drflac_bool32 geo_cue_flac_seek(void *userdata, int offset,
    drflac_seek_origin origin) {

    int whence;

    switch (origin) {
        case DRFLAC_SEEK_CUR: whence = GEO_VFS_SEEK_CUR; break;
        case DRFLAC_SEEK_END: whence = GEO_VFS_SEEK_END; break;
        default: whence = GEO_VFS_SEEK_SET; break;
    }

    return geo_vfs_seek(userdata, offset, whence) < 0 ?
        DRFLAC_FALSE : DRFLAC_TRUE;
}

// dr_flac tell callback, backed by the VFS layer
static drflac_bool32 geo_cue_flac_tell(void *userdata, drflac_int64 *cursor) {
    int64_t pos = geo_vfs_tell(userdata);

    if (pos < 0)
        return DRFLAC_FALSE;

    *cursor = (drflac_int64)pos;

    return DRFLAC_TRUE;
}

// Open a file specified in a cue sheet
static int geo_cue_open_file(int idx) {
    cue_file_t *f = &files[idx];

    f->fp = geo_vfs_open(f->path, GEO_VFS_READ);
    if (!f->fp) {
        geo_log(GEO_LOG_ERR, "CUE: Failed to open file: %s\n", f->path);
        return 0;
    }

    if (f->type == FTYPE_FLAC) {
        f->flac = drflac_open(geo_cue_flac_read, geo_cue_flac_seek,
            geo_cue_flac_tell, f->fp, NULL);
        if (!f->flac) {
            geo_log(GEO_LOG_ERR, "CUE: Failed to open FLAC: %s\n", f->path);
            geo_vfs_close(f->fp);
            f->fp = NULL;
            return 0;
        }
        return 1;
    }

    if (f->type == FTYPE_WAV) {
        f->data_off = geo_cue_wav_parse_header(f->fp);
        if (!f->data_off) {
            geo_log(GEO_LOG_ERR, "CUE: Invalid WAV header: %s\n", f->path);
            geo_vfs_close(f->fp);
            f->fp = NULL;
            return 0;
        }
    }

    return 1;
}

// Parse MSF timestamp "MM:SS:FF" and return frame count
static uint32_t geo_cue_parse_msf(const char *str) {
    unsigned m, s, f;
    if (sscanf(str, "%u:%u:%u", &m, &s, &f) != 3)
        return 0;
    return (m * 60 * 75) + (s * 75) + f; // Convert MSF to frames and return
}

// Get file size for a BIN/ISO file
static uint32_t geo_cue_file_size_frames(int file_idx) {
    cue_file_t *f = &files[file_idx];
    uint32_t sector_sz = (f->type == FTYPE_ISO) ? GEO_DISC_DATA_SIZE
                                                 : GEO_DISC_SECTOR_SIZE;
    if (f->type == FTYPE_FLAC) {
        if (f->flac)
            return (uint32_t)(f->flac->totalPCMFrameCount / 588);
        return 0;
    }

    if (!f->fp)
        return 0;

    int64_t size = geo_vfs_size(f->fp);
    if (size < 0)
        return 0;

    if (f->type == FTYPE_WAV)
        size -= f->data_off;

    return (uint32_t)(size / sector_sz);
}

// Open a cue sheet
int geo_cue_open(const char *path) {
    num_files = 0;
    num_tracks = 0;
    leadout_lba = 0;
    flac_last_track = -1;
    memset(files, 0, sizeof(files));
    memset(tracks, 0, sizeof(tracks));

    /* Cue sheets are small, so read the whole thing in and walk it line by
       line rather than requiring the VFS layer to provide a gets operation.
    */
    size_t cuesz = 0;
    char *cuedata = (char*)geo_vfs_read_file(path, &cuesz);
    if (!cuedata) {
        geo_log(GEO_LOG_ERR, "CUE: Failed to open: %s\n", path);
        return 0;
    }

    char line[1024];
    int cur_file = -1;
    int cur_track = -1;
    uint32_t cd_pos = 0;         // CD-level LBA position
    int pending_index00 = 0;     // Whether we got INDEX 00 before INDEX 01
    uint32_t index00_offset = 0; // File offset at INDEX 00

    for (size_t lpos = 0; lpos < cuesz; ) {
        // Measure the line, then copy it out of the buffer
        size_t llen = 0;
        while (lpos + llen < cuesz && cuedata[lpos + llen] != '\n')
            ++llen;

        size_t copylen = llen < sizeof(line) ? llen : sizeof(line) - 1;
        memcpy(line, cuedata + lpos, copylen);
        line[copylen] = '\0';
        lpos += llen + 1;

        // Trim trailing newline/whitespace
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' '))
            line[--len] = '\0';

        // Skip empty lines
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            ++p;
        if (!*p)
            continue;

        if (!strjcasecmp(p, "FILE ", 5)) {
            // FILE "filename" BINARY/WAVE/etc.
            if (num_files >= MAX_FILES)
                break;

            // Parse filename (may be quoted)
            char *fname_start = p + 5;
            char filename[512];

            if (*fname_start == '"') {
                char *end = strchr(fname_start + 1, '"');
                if (!end)
                    continue;
                size_t flen = end - fname_start - 1;
                if (flen >= sizeof(filename))
                    flen = sizeof(filename) - 1;
                memcpy(filename, fname_start + 1, flen);
                filename[flen] = '\0';
            }
            else {
                // Unquoted: take until next space
                char *end = strchr(fname_start, ' ');
                size_t flen = end ? (size_t)(end - fname_start)
                                  : strlen(fname_start);
                if (flen >= sizeof(filename))
                    flen = sizeof(filename) - 1;
                memcpy(filename, fname_start, flen);
                filename[flen] = '\0';
            }

            // Build full path, relative to the directory holding the cue
            cue_file_t *f = &files[num_files];
            if (!geo_vfs_resolve(f->path, sizeof(f->path), path, filename)) {
                geo_log(GEO_LOG_ERR, "CUE: Failed to resolve path: %s\n",
                    filename);
                free(cuedata);
                geo_cue_close();
                return 0;
            }
            f->type = geo_cue_detect_file_type(filename);

            if (!geo_cue_open_file(num_files)) {
                free(cuedata);
                geo_cue_close();
                return 0;
            }

            cur_file = num_files;
            num_files++;
        }
        else if (!strjcasecmp(p, "TRACK ", 6)) {
            // TRACK NN MODE1/2352 or TRACK NN AUDIO
            if (num_tracks >= MAX_TRACKS || cur_file < 0)
                continue;

            // Finalize previous track's frame count if needed
            if (cur_track >= 0 && tracks[cur_track].frames == 0) {
                // Previous track runs to end of its file (or to current pos)
                // Will be finalized at INDEX 01 of next track or at EOF
                // FIXME - do something here
            }

            unsigned tnum;
            char ttype[64];
            if (sscanf(p + 6, "%u %63s", &tnum, ttype) < 2)
                continue;

            cur_track = num_tracks;
            cue_track_t *t = &tracks[cur_track];
            t->file_idx = cur_file;
            t->pregap = 0;
            pending_index00 = 0;

            if (!strjcasecmp(ttype, "AUDIO", 5)) {
                t->type = GEO_DISC_TRACK_AUDIO;
                t->sector_size = GEO_DISC_SECTOR_SIZE;
            }
            else {
                t->type = GEO_DISC_TRACK_DATA;
                // MODE1/2352 or MODE1/2048
                if (strstr(ttype, "2048") ||
                    files[cur_file].type == FTYPE_ISO) {
                    t->sector_size = GEO_DISC_DATA_SIZE;
                }
                else {
                    t->sector_size = GEO_DISC_SECTOR_SIZE;
                }
            }

            num_tracks++;
        }
        else if (!strjcasecmp(p, "INDEX ", 6)) {
            // INDEX NN MM:SS:FF
            if (cur_track < 0)
                continue;

            unsigned idx;
            char msf[32];
            if (sscanf(p + 6, "%u %31s", &idx, msf) < 2)
                continue;

            uint32_t offset = geo_cue_parse_msf(msf);

            if (idx == 0) {
                pending_index00 = 1;
                index00_offset = offset;
            }
            else if (idx == 1) {
                cue_track_t *t = &tracks[cur_track];

                // Set pregap from INDEX 00 to INDEX 01
                if (pending_index00) {
                    t->pregap = offset - index00_offset;
                    pending_index00 = 0;
                }

                // For multi-BIN (one file per track), offset is typically 0
                // For single-BIN (merged), offset is the position in the file
                t->file_offset = (uint64_t)offset * t->sector_size;

                // Advance CD position by pregap
                cd_pos += t->pregap;
                t->start = cd_pos;

                // Finalize previous track length and advance cd_pos
                if (cur_track > 0) {
                    cue_track_t *prev = &tracks[cur_track - 1];
                    if (prev->frames == 0 &&
                        prev->file_idx == t->file_idx) {
                        // Same file: previous track runs to this track's start
                        uint32_t prev_start_frame =
                            prev->file_offset / prev->sector_size;
                        uint32_t this_start_frame = offset;
                        prev->frames = this_start_frame - prev_start_frame;
                        cd_pos = prev->start + prev->frames + t->pregap;
                        t->start = cd_pos;
                    }
                    else if (prev->frames == 0) {
                        // Different file: compute length from file size
                        uint32_t total =
                            geo_cue_file_size_frames(prev->file_idx);
                        uint32_t start_frame =
                            prev->file_offset / prev->sector_size;
                        prev->frames = total - start_frame;
                        cd_pos = prev->start + prev->frames + t->pregap;
                        t->start = cd_pos;
                    }
                }
            }
        }
        else if (!strjcasecmp(p, "PREGAP ", 7)) {
            // PREGAP MM:SS:FF — silence pregap (not in file)
            if (cur_track >= 0) {
                tracks[cur_track].pregap = geo_cue_parse_msf(p + 7);
            }
        }
        // REM, POSTGAP, SONGWRITER, etc. — ignored
    }

    free(cuedata);

    if (num_tracks == 0) {
        geo_log(GEO_LOG_ERR, "CUE: No tracks found\n");
        geo_cue_close();
        return 0;
    }

    // Finalize last track's frame count from file size
    for (unsigned i = 0; i < num_tracks; ++i) {
        if (tracks[i].frames == 0) {
            uint32_t total = geo_cue_file_size_frames(tracks[i].file_idx);
            uint32_t start_frame =
                tracks[i].file_offset / tracks[i].sector_size;
            tracks[i].frames = total - start_frame;
        }
    }

    // Compute leadout
    if (num_tracks > 0) {
        cue_track_t *last = &tracks[num_tracks - 1];
        leadout_lba = last->start + last->frames;
    }

    // Log
    for (unsigned i = 0; i < num_tracks; ++i) {
        geo_log(GEO_LOG_DBG, "CUE Track %u: type=%s start=%u frames=%u "
            "pregap=%u file=%d offset=%u\n",
            i + 1,
            tracks[i].type == GEO_DISC_TRACK_AUDIO ? "AUDIO" : "DATA",
            tracks[i].start, tracks[i].frames, tracks[i].pregap,
            tracks[i].file_idx, tracks[i].file_offset);
    }

    geo_log(GEO_LOG_INF, "CUE opened: %u tracks, %u files, leadout at LBA %u\n",
        num_tracks, num_files, leadout_lba);

    return 1;
}

// Close a cue sheet
void geo_cue_close(void) {
    for (unsigned i = 0; i < num_files; ++i) {
        if (files[i].flac) { // Decoder holds the file handle, close it first
            drflac_close(files[i].flac);
            files[i].flac = NULL;
        }
        if (files[i].fp) {
            geo_vfs_close(files[i].fp);
            files[i].fp = NULL;
        }
    }
    num_files = 0;
    num_tracks = 0;
    leadout_lba = 0;
    flac_last_track = -1;
}

// Find which track a disc LBA belongs to
static int geo_cue_find_track(uint32_t disc_lba) {
    for (int i = (int)num_tracks - 1; i >= 0; --i) {
        if (disc_lba >= tracks[i].start)
            return i;
    }
    return 0;
}

// Read a sector worth of disc data
int geo_cue_read_sector(uint32_t disc_lba, uint8_t *buf) {
    int ti = geo_cue_find_track(disc_lba);
    cue_track_t *t = &tracks[ti];
    cue_file_t *f = &files[t->file_idx];

    if (!f->fp || f->type == FTYPE_FLAC)
        return 0;

    uint32_t track_offset = disc_lba - t->start;
    uint64_t byte_offset = t->file_offset +
                           (uint64_t)track_offset * t->sector_size;

    // Raw 2352: skip 16-byte header. ISO 2048: read directly.
    if (t->sector_size == GEO_DISC_SECTOR_SIZE)
        byte_offset += 16;

    if (geo_vfs_seek(f->fp, (int64_t)byte_offset, GEO_VFS_SEEK_SET) < 0)
        return 0;

    if (geo_vfs_read(f->fp, buf, GEO_DISC_DATA_SIZE) != GEO_DISC_DATA_SIZE)
        return 0;

    return 1;
}

// Read CDDA audio
int geo_cue_read_audio(uint32_t disc_lba, int16_t *buf) {
    int ti = geo_cue_find_track(disc_lba);
    cue_track_t *t = &tracks[ti];
    cue_file_t *f = &files[t->file_idx];

    uint32_t track_offset = disc_lba - t->start;

    if (f->type == FTYPE_FLAC) {
        if (!f->flac)
            return 0;

        // 588 stereo PCM frames per CD sector
        uint64_t target_frame = (uint64_t)track_offset * 588;

        // Only seek if not sequential
        if (flac_last_track != ti ||
            flac_last_frame != target_frame) {
            drflac_seek_to_pcm_frame(f->flac, target_frame);
        }

        drflac_uint64 read = drflac_read_pcm_frames_s16(f->flac, 588,
                                                         (drflac_int16*)buf);
        if (read < 588)
            memset(buf + read * 2, 0, (588 - read) * 2 * sizeof(int16_t));

        flac_last_track = ti;
        flac_last_frame = target_frame + 588;
        return 1;
    }

    if (!f->fp)
        return 0;

    uint64_t byte_offset;
    if (f->type == FTYPE_WAV) {
        byte_offset = f->data_off +
                      (uint64_t)track_offset * GEO_DISC_SECTOR_SIZE;
    } else {
        // BIN: raw 2352 bytes per sector
        byte_offset = t->file_offset +
                      (uint64_t)track_offset * t->sector_size;
    }

    if (geo_vfs_seek(f->fp, (int64_t)byte_offset, GEO_VFS_SEEK_SET) < 0) {
        memset(buf, 0, GEO_DISC_SECTOR_SIZE);
        return 0;
    }

    // BIN/WAV audio is little-endian — no byte-swap needed
    if (geo_vfs_read(f->fp, buf, GEO_DISC_SECTOR_SIZE) !=
        GEO_DISC_SECTOR_SIZE) {
        memset(buf, 0, GEO_DISC_SECTOR_SIZE);
        return 0;
    }

    return 1;
}

unsigned geo_cue_num_tracks(void) {
    return num_tracks;
}

int geo_cue_track_is_audio(unsigned track) {
    if (track == 0 || track > num_tracks)
        return 0;
    return tracks[track - 1].type == GEO_DISC_TRACK_AUDIO;
}

uint32_t geo_cue_track_start(unsigned track) {
    if (track == 0 || track > num_tracks)
        return 0;
    return tracks[track - 1].start;
}

uint32_t geo_cue_track_frames(unsigned track) {
    if (track == 0 || track > num_tracks)
        return 0;
    return tracks[track - 1].frames;
}

uint32_t geo_cue_leadout(void) {
    return leadout_lba;
}
