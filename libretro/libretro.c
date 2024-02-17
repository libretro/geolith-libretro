/*
Copyright (c) 2022-2024 Rupert Carmichael
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "geo.h"
#include "geo_lspc.h"
#include "geo_m68k.h"
#include "geo_mixer.h"
#include "geo_neo.h"

#include "libretro.h"
#include "libretro_core_options.h"

#if defined(_WIN32)
   static const char pss = '\\';
#else
   static const char pss = '/';
#endif

// Audio/Video buffers
static int16_t  *abuf = NULL;
static uint32_t *vbuf = NULL;

// Copy of the ROM data passed in by the frontend
static void *romdata = NULL;

// Game name without path or extension
static char gamename[128];

// Save directory for NVRAM, SRAM, and Memory Cards
static const char *savedir;

// Variables for managing the libretro port
static int bitmasks = 0;
static int systype = SYSTEM_AES;
static int region = REGION_US;
static int settingmode = 0;
static int freeplay = 0;
static int fourplayer = 0;
static int video_crop_t = 8;
static int video_crop_b = 8;
static int video_crop_l = 8;
static int video_crop_r = 8;
static int video_width_visible = LSPC_WIDTH_VISIBLE;
static int video_height_visible = LSPC_HEIGHT_VISIBLE;
static float video_aspect = 0.0;

// libretro callbacks
static retro_log_printf_t log_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_audio_sample_t audio_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_environment_t environ_cb = NULL;
static retro_input_poll_t input_poll_cb = NULL;
static retro_input_state_t input_state_cb = NULL;

// libretro input descriptors
static struct retro_input_descriptor input_desc_js[] = { // Joysticks (Default)
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "D"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select/Coin"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "C+D"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "A+B"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "B+C+D"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "A+B+C"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Test"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Service"},


{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "D"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select/Coin"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "C+D"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "A+B"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "B+C+D"},
{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "A+B+C"},

{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "D"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select/Coin"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "C+D"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "A+B"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "B+C+D"},
{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "A+B+C"},

{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "D"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select/Coin"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "C+D"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "A+B"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "B+C+D"},
{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "A+B+C"},

{0}
};

static struct retro_input_descriptor input_desc_vliner[] = { // V-Liner
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Payout Table/Big"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Bet/Small"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Stop/Double-Up"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Start/Collect"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Operator Menu"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Clear Credit"},
{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Hopper Out"},
{0}
};

typedef struct _kvpair_t {
    unsigned k;
    unsigned v;
} kvpair_t;

static kvpair_t bindmap_js[] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,        0x01 },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,      0x02 },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,      0x04 },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,     0x08 },
    { RETRO_DEVICE_ID_JOYPAD_B,         0x10 }, // A
    { RETRO_DEVICE_ID_JOYPAD_A,         0x20 }, // B
    { RETRO_DEVICE_ID_JOYPAD_Y,         0x40 }, // C
    { RETRO_DEVICE_ID_JOYPAD_X,         0x80 }, // D
    { RETRO_DEVICE_ID_JOYPAD_L,         0xc0 }, // CD
    { RETRO_DEVICE_ID_JOYPAD_R,         0x30 }, // AB
    { RETRO_DEVICE_ID_JOYPAD_L2,        0xe0 }, // BCD
    { RETRO_DEVICE_ID_JOYPAD_R2,        0x70 }  // ABC
};

static kvpair_t bindmap_stat_a[] = {
    { RETRO_DEVICE_ID_JOYPAD_SELECT,    0x06 }, // Coin 1
    { RETRO_DEVICE_ID_JOYPAD_SELECT,    0x05 }, // Coin 2
    { RETRO_DEVICE_ID_JOYPAD_R3,        0x03 }  // Service
};

static kvpair_t bindmap_stat_b[] = {
    { RETRO_DEVICE_ID_JOYPAD_SELECT,    0x0d }, // P1 Select
    { RETRO_DEVICE_ID_JOYPAD_START,     0x0e }, // P1 Start
    { RETRO_DEVICE_ID_JOYPAD_SELECT,    0x07 }, // P2 Select
    { RETRO_DEVICE_ID_JOYPAD_START,     0x0b }  // P2 Start
};

static kvpair_t bindmap_sys[] = {
    { RETRO_DEVICE_ID_JOYPAD_L3,        0x40 }  // Test
};

static kvpair_t bindmap_vliner[] = {
    { RETRO_DEVICE_ID_JOYPAD_L,         0x01 }, // Coin 1
    { RETRO_DEVICE_ID_JOYPAD_R,         0x02 }, // Coin 2
    { RETRO_DEVICE_ID_JOYPAD_SELECT,    0x10 }, // Operator Menu
    { RETRO_DEVICE_ID_JOYPAD_START,     0x20 }, // Clear Credit
    { RETRO_DEVICE_ID_JOYPAD_R3,        0x80 }, // Hopper Out
};

static void geo_retro_log(int level, const char *fmt, ...) {
    if (log_cb == NULL)
        return;

    char outbuf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(outbuf, sizeof(outbuf), fmt, va);
    va_end(va);
    log_cb(level, outbuf);
}

static void geo_retro_gamename(const char *path) {
    char *s = strrchr(path, pss);
    snprintf(gamename, sizeof(gamename), "%s", s ? s + 1 : path);
    for (int i = strlen(gamename) - 1; i > 0; --i) {
        if (gamename[i] == '.') {
            gamename[i] = '\0';
            break;
        }
    }
}

// Coin Slots, Service Button
static unsigned geo_input_poll_stat_a(void) {
    input_poll_cb();
    unsigned c = 0x07;

    if (systype == SYSTEM_MVS) { // Coins
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_a[0].k))
            c &= bindmap_stat_a[0].v;
        if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_a[1].k))
            c &= bindmap_stat_a[1].v;
    }

    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_a[2].k))
        c &= bindmap_stat_a[2].k;

    return c;
}

static unsigned geo_input_poll_stat_a_vliner(void) {
    return 0x07;
}

// P1/P2 Select/Start, Memcard status, AES/MVS setting
static unsigned geo_input_poll_stat_b(void) {
    input_poll_cb();
    unsigned s = 0x0f;

    /* Select button inserts coins on Universe BIOS in arcade mode. For MVS,
       it selects the cartridge slot, so do not read these buttons in MVS mode.
    */
    if (systype != SYSTEM_MVS) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[0].k))
            s &= bindmap_stat_b[0].v;
        if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[2].k))
            s &= bindmap_stat_b[2].v;
    }

    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[1].k))
        s &= bindmap_stat_b[1].v;
    if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[3].k))
        s &= bindmap_stat_b[3].v;

    return s;
}

// P1/P2 Start, P3/P4 Start, Memcard status, AES/MVS setting
static unsigned geo_input_poll_stat_b_ftc1b(void) {
    input_poll_cb();
    unsigned s = 0x0f;

    if (geo_m68k_reg_poutput() & 0x01) { // Players 2/4 (P1-B, P2-B)
        if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[1].k))
            s &= bindmap_stat_b[1].v;
        if (input_state_cb(3, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[1].k))
            s &= bindmap_stat_b[3].k;
    }
    else { // Players 1/3 (P1-A, P2-A)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[1].k))
            s &= bindmap_stat_b[1].v;
        if (input_state_cb(2, RETRO_DEVICE_JOYPAD, 0, bindmap_stat_b[1].k))
            s &= bindmap_stat_b[3].k;
    }

    return s;
}

static unsigned geo_input_poll_stat_b_vliner(void) {
    return 0x0f;
}

// Test button, slot type
static unsigned geo_input_poll_systype(void) {
    input_poll_cb();
    unsigned t = 0xc0;

    // Test Button
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_sys[0].k))
        t &= bindmap_sys[0].v;

    return t;
}

// Poll DIP Switches
static unsigned geo_input_poll_dipsw(void) {
    unsigned d = 0xff;

    if (settingmode)
        d &= ~0x01;
    if (fourplayer)
        d &= ~0x02;
    if (freeplay)
        d &= ~0x40;

    return d;
}

// V-Liner System Buttons
static unsigned geo_input_poll_sys_vliner(void) {
    input_poll_cb();
    unsigned b = 0xff;

    if (bitmasks) {
        int16_t ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
            RETRO_DEVICE_ID_JOYPAD_MASK);

        for (unsigned i = 0; i < 5; ++i) {
            if (ret & (1 << bindmap_vliner[i].k))
                b &= ~bindmap_vliner[i].v;
        }

        return b;
    }

    // Coins
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_vliner[0].k))
        b &= ~bindmap_vliner[0].v;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_vliner[1].k))
        b &= ~bindmap_vliner[1].v;

    // Operator, Clear Credit, Hopper Out
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_vliner[2].k))
        b &= ~bindmap_vliner[2].v;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_vliner[3].k))
        b &= ~bindmap_vliner[3].v;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, bindmap_vliner[4].k))
        b &= ~bindmap_vliner[4].v;

    return b;
}

// V-Liner Play Buttons
static unsigned geo_input_poll_vliner(unsigned port) {
    input_poll_cb();
    unsigned b = 0xff;

    if (bitmasks) {
        int16_t ret = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0,
            RETRO_DEVICE_ID_JOYPAD_MASK);

        for (unsigned i = 0; i < 8; ++i) {
            if (ret & (1 << bindmap_js[i].k))
                b &= ~bindmap_js[i].v;
        }
    }
    else {
        for (unsigned i = 0; i < 8; ++i) {
            if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, bindmap_js[i].k))
                b &= ~bindmap_js[i].v;
        }
    }

    return b;
}

// Port Unconnected
static unsigned geo_input_poll_none(unsigned port) {
    (void)port;
    return 0xff;
}

// Neo Geo Joystick
static unsigned geo_input_poll_js(unsigned port) {
    input_poll_cb();
    unsigned b = 0xff;

    if (bitmasks) {
        int16_t ret = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0,
            RETRO_DEVICE_ID_JOYPAD_MASK);

        for (unsigned i = 0; i < 12; ++i) {
            if (ret & (1 << bindmap_js[i].k))
                b &= ~bindmap_js[i].v;
        }
    }
    else {
        for (unsigned i = 0; i < 12; ++i) {
            if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, bindmap_js[i].k))
                b &= ~bindmap_js[i].v;
        }
    }

    // Prevent opposing directions from being pressed simultaneously
    if (!(b & 0x01) && !(b & 0x02))
        b |= 0x03;
    if (!(b & 0x04) && !(b & 0x08))
        b |= 0x0c;

    return b;
}

// Neo Geo Joystick (NEO-FTC1B 4-Player Extension Board for 2 MVS cabinets)
static unsigned geo_input_poll_js_ftc1b(unsigned port) {
    input_poll_cb();
    unsigned b = 0xff;

    uint8_t output = geo_m68k_reg_poutput();
    port = (port << 1) + (output & 0x01);

    if (output & 0x04)
        b &= ~(1 << (4 | (output & 0x01)));

    if (bitmasks) {
        int16_t ret = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0,
            RETRO_DEVICE_ID_JOYPAD_MASK);

        for (unsigned i = 0; i < 12; ++i) {
            if (ret & (1 << bindmap_js[i].k))
                b &= ~bindmap_js[i].v;
        }
    }
    else {
        for (unsigned i = 0; i < 12; ++i) {
            if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, bindmap_js[i].k))
                b &= ~bindmap_js[i].v;
        }
    }

    // Prevent opposing directions from being pressed simultaneously
    if (!(b & 0x01) && !(b & 0x02))
        b |= 0x03;
    if (!(b & 0x04) && !(b & 0x08))
        b |= 0x0c;

    return b;
}

static void geo_cb_audio(size_t samps) {
    audio_batch_cb(abuf, samps >> 1);
}

static void geo_geom_refresh(void) {
    struct retro_system_av_info avinfo;
    retro_get_system_av_info(&avinfo);
    avinfo.geometry.base_width = video_width_visible;
    avinfo.geometry.base_height = video_height_visible;
    avinfo.geometry.aspect_ratio = video_aspect;
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &avinfo);
}

static void check_variables(bool first_run) {
    struct retro_variable var = {0};

    if (first_run) {
        // System Type
        var.key   = "geolith_system_type";
        var.value = NULL;

        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            if (!strcmp(var.value, "aes"))
                systype = SYSTEM_AES;
            else if (!strcmp(var.value, "mvs"))
                systype = SYSTEM_MVS;
            else if (!strcmp(var.value, "uni"))
                systype = SYSTEM_UNI;
        }

        // Region
        var.key   = "geolith_region";
        var.value = NULL;

        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            if (!strcmp(var.value, "us"))
                region = REGION_US;
            else if (!strcmp(var.value, "jp"))
                region = REGION_JP;
            else if (!strcmp(var.value, "as"))
                region = REGION_AS;
            else if (!strcmp(var.value, "eu"))
                region = REGION_EU;
        }

        // Four Player
        var.key   = "geolith_4player";
        var.value = NULL;

        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            if (!strcmp(var.value, "on")) {
                if (systype == SYSTEM_MVS &&
                    (region == REGION_AS || region == REGION_JP))
                fourplayer = 1;
            }
            else {
                fourplayer = 0;
            }
        }

        // Setting Mode
        var.key   = "geolith_settingmode";
        var.value = NULL;

        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            if (!strcmp(var.value, "on"))
                settingmode = 1;
            else
                settingmode = 0;
        }
    }

    // Freeplay
    var.key   = "geolith_freeplay";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "on"))
            freeplay = 1;
        else
            freeplay = 0;
    }

    // Overscan
    var.key   = "geolith_overscan_t";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "16"))
            video_crop_t = 16;
        else if (!strcmp(var.value, "12"))
            video_crop_t = 12;
        else if (!strcmp(var.value, "8"))
            video_crop_t = 8;
        else if (!strcmp(var.value, "4"))
            video_crop_t = 4;
        else if (!strcmp(var.value, "0"))
            video_crop_t = 0;
    }

    var.key   = "geolith_overscan_b";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "16"))
            video_crop_b = 16;
        else if (!strcmp(var.value, "12"))
            video_crop_b = 12;
        else if (!strcmp(var.value, "8"))
            video_crop_b = 8;
        else if (!strcmp(var.value, "4"))
            video_crop_b = 4;
        else if (!strcmp(var.value, "0"))
            video_crop_b = 0;
    }

    var.key   = "geolith_overscan_l";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "16"))
            video_crop_l = 16;
        else if (!strcmp(var.value, "12"))
            video_crop_l = 12;
        else if (!strcmp(var.value, "8"))
            video_crop_l = 8;
        else if (!strcmp(var.value, "4"))
            video_crop_l = 4;
        else if (!strcmp(var.value, "0"))
            video_crop_l = 0;
    }

    var.key   = "geolith_overscan_r";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "16"))
            video_crop_r = 16;
        else if (!strcmp(var.value, "12"))
            video_crop_r = 12;
        else if (!strcmp(var.value, "8"))
            video_crop_r = 8;
        else if (!strcmp(var.value, "4"))
            video_crop_r = 4;
        else if (!strcmp(var.value, "0"))
            video_crop_r = 0;
    }

    // Recalculate visible video dimensions as they may have changed
    video_width_visible = LSPC_WIDTH - (video_crop_l + video_crop_r);
    video_height_visible = LSPC_HEIGHT - (video_crop_t + video_crop_b + 16);

    // Aspect Ratio
    var.key   = "geolith_aspect";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "1:1"))
            video_aspect = video_width_visible / (double)video_height_visible;
        else if (!strcmp(var.value, "45:44"))
            video_aspect =
                (video_width_visible * (45.0 / 44.0)) / video_height_visible;
        else if (!strcmp(var.value, "4:3"))
            video_aspect = 4.0 / 3.0;
    }

    // Sprites-per-line limit
    var.key   = "geolith_sprlimit";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "96"))
            geo_lspc_set_sprlimit(96);
        else if (!strcmp(var.value, "192"))
            geo_lspc_set_sprlimit(192);
        else if (!strcmp(var.value, "288"))
            geo_lspc_set_sprlimit(288);
        else if (!strcmp(var.value, "381"))
            geo_lspc_set_sprlimit(381);
    }

    // Overclocking
    var.key   = "geolith_oc";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "on"))
            geo_set_div68k(0);
        else
            geo_set_div68k(1);
    }
}

void retro_init(void) {
    // Set up log callback
    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;

    // Check if frontend supports input bitmasks
    if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
        bitmasks = 1;

    // Set initial core options
    check_variables(true);

    // Set up logging and system type
    geo_log_set_callback(geo_retro_log);
    geo_set_region(region);
    geo_set_system(systype);

    // Allocate and pass the video buffer into the emulator
    vbuf = (uint32_t*)calloc(1, LSPC_WIDTH * LSPC_SCANLINES * sizeof(uint32_t));
    geo_lspc_set_buffer(vbuf);

    // Allocate and pass the audio buffer into the emulator
    abuf = (int16_t*)calloc(1, 2048 * sizeof(int16_t));
    geo_mixer_set_buffer(abuf);

    // Set up audio callback
    geo_mixer_set_callback(geo_cb_audio);
    geo_mixer_set_raw(); // Bypass the emulator's internal resampler

    // Finish initialization
    geo_init();
}

void retro_deinit(void) {
    geo_mixer_deinit();

    free(vbuf);
    vbuf = NULL;

    free(abuf);
    abuf = NULL;
}

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Geolith";
    info->library_version  = "0.2.0";
    info->need_fullpath    = false;
    info->valid_extensions = "neo";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing = (struct retro_system_timing) {
        .fps = systype ? FRAMERATE_MVS : FRAMERATE_AES,
        .sample_rate = systype ? 55555 : 55943.49
    };

    info->geometry = (struct retro_game_geometry) {
        .base_width   = video_width_visible,
        .base_height  = video_height_visible,
        .max_width    = LSPC_WIDTH,
        .max_height   = LSPC_HEIGHT,
        .aspect_ratio = video_aspect,
    };
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    libretro_set_core_options(environ_cb);
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
    audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
    input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

void retro_reset(void) {
    geo_reset(0);
}

void retro_run(void) {
    geo_exec();

    bool update = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &update) && update) {
        check_variables(false);
        geo_geom_refresh();
    }

    video_cb(vbuf + (LSPC_WIDTH * (video_crop_t + 16)) + video_crop_l,
        video_width_visible,
        video_height_visible,
        LSPC_WIDTH << 2);
}

bool retro_load_game(const struct retro_game_info *info) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log_cb(RETRO_LOG_INFO, "XRGB8888 unsupported\n");
        return false;
    }

    const char *sysdir;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) || !sysdir)
        return false;

    char biospath[256];
    snprintf(biospath, sizeof(biospath), "%s%c%s", sysdir, pss,
        systype ? "neogeo.zip" : "aes.zip");

    if (!geo_bios_load_file(biospath)) {
        log_cb(RETRO_LOG_ERROR, "Failed to load bios at: %s\n", biospath);
        return false;
    }

    /* Keep an internal copy of the ROM to avoid relying on the frontend
       keeping it persistently, and to avoid altering memory controlled by
       the frontend (portions are byteswapped or otherwise manipulated inside
       the emulator at load time)
    */
    romdata = (void*)calloc(1, info->size);
    memcpy(romdata, info->data, info->size);

    if (!geo_neo_load(romdata, info->size)) {
        log_cb(RETRO_LOG_ERROR, "Failed to load ROM\n");
        retro_unload_game();
        return false;
    }

    // Extract the game name from the path
    geo_retro_gamename(info->path);

    // Grab the libretro frontend's save directory
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &savedir) || !savedir)
        return false;

    // Load any existing saved data
    char savename[292];
    char *fext[] = { "nv", "srm", "mcr" };

    for (unsigned i = 0; i < GEO_SAVEDATA_MAX; ++i) {
        snprintf(savename, sizeof(savename), "%s%c%s.%s",
            savedir, pss, gamename, fext[i]);

        int savestat = geo_savedata_load(i, (const char*)savename);

        if (savestat == 1)
            log_cb(RETRO_LOG_DEBUG, "Loaded: %s\n", savename);
        else if (savestat != 2)
            log_cb(RETRO_LOG_DEBUG, "Load Failed: %s\n", savename);
    }

    geo_input_set_callback(0, &geo_input_poll_js);
    geo_input_set_callback(1, &geo_input_poll_js);
    geo_input_sys_set_callback(0, &geo_input_poll_stat_a);
    geo_input_sys_set_callback(1, &geo_input_poll_stat_b);
    geo_input_sys_set_callback(2, &geo_input_poll_systype);
    geo_input_sys_set_callback(3, &geo_input_poll_dipsw);

    if (fourplayer) {
        geo_input_set_callback(0, &geo_input_poll_js_ftc1b);
        geo_input_set_callback(1, &geo_input_poll_js_ftc1b);
        geo_input_sys_set_callback(1, &geo_input_poll_stat_b_ftc1b);
    }

    if (geo_neo_flags() & GEO_DB_VLINER) {
        geo_input_set_callback(0, &geo_input_poll_vliner);
        geo_input_set_callback(1, &geo_input_poll_none);
        geo_input_sys_set_callback(0, &geo_input_poll_stat_a_vliner);
        geo_input_sys_set_callback(1, &geo_input_poll_stat_b_vliner);
        geo_input_sys_set_callback(4, &geo_input_poll_sys_vliner);
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_desc_vliner);
    }
    else {
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_desc_js);
    }

    geo_reset(1);
    return true;
}

void retro_unload_game(void) {
    // Save NVRAM, Cartridge RAM, and Memory Card
    char savename[292];
    char *fext[] = { "nv", "srm", "mcr" };

    for (unsigned i = 0; i < GEO_SAVEDATA_MAX; ++i) {
        snprintf(savename, sizeof(savename), "%s%c%s.%s",
            savedir, pss, gamename, fext[i]);

        int savestat = geo_savedata_save(i, (const char*)savename);

        if (savestat == 1)
            log_cb(RETRO_LOG_DEBUG, "Saved: %s\n", savename);
        else if (savestat != 2)
            log_cb(RETRO_LOG_DEBUG, "Save Failed: %s\n", savename);
    }

    if (romdata)
        free(romdata);

    geo_bios_unload();
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
                             size_t num) {
    (void)type;
    (void)info;
    (void)num;
    return false;
}

size_t retro_serialize_size(void) {
    return geo_state_size();
}

bool retro_serialize(void *data, size_t size) {
    memcpy(data, geo_state_save_raw(), size);
    return true;
}

bool retro_unserialize(const void *data, size_t size) {
    (void)size;
    return geo_state_load_raw(data);
}

void *retro_get_memory_data(unsigned id) {
    switch (id) {
        case RETRO_MEMORY_SYSTEM_RAM: {
            return (void*)geo_mem_ptr(GEO_MEMTYPE_MAINRAM, NULL);
        }
        case RETRO_MEMORY_VIDEO_RAM: {
            return (void*)geo_mem_ptr(GEO_MEMTYPE_VRAM, NULL);
        }
        default: {
            return NULL;
        }
    }
}

size_t retro_get_memory_size(unsigned id) {
    void *mem = NULL;
    size_t sz = 0;
    switch (id) {
        case RETRO_MEMORY_SYSTEM_RAM: {
            mem = (void*)geo_mem_ptr(GEO_MEMTYPE_MAINRAM, &sz);
            break;
        }
        case RETRO_MEMORY_VIDEO_RAM: {
            mem = (void*)geo_mem_ptr(GEO_MEMTYPE_VRAM, &sz);
            break;
        }
    }

    (void)mem;
    return sz;
}

void retro_cheat_reset(void) {
}

void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index;
    (void)enabled;
    (void)code;
}
