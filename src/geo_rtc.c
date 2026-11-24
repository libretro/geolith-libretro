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

// NEC uPD4990A

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "geo_rtc.h"
#include "geo_serial.h"

// 68K @ 12MHz == 12,000,000Hz
#define CYCS_68K_PER_SECOND 12000000

// 68K cycles per 64Hz step of the 15 Stage Binary Divider (stage 9 output)
#define CYCS_68K_PER_64HZ (CYCS_68K_PER_SECOND / 64)

// Timing Pulse Mode
#define TPMODE_RUN  0x00
#define TPMODE_STOP 0x01

/* 52-bit shift register broken into command and data sections (4-bit, 48-bit)
   ========================================================================
   | CMD | Y10 | Y1 | MO | WD | D10 | D1 | H10 | H1 | M10 | M1 | S10 | S1 |
   ========================================================================
   CMD is 4 bits. Year, Day, Hour, Minute, and Second are separated into a 10s
   and a 1s block of 4 bits each, while Month and Weekday are single 4 bit
   blocks.
*/
static uint8_t cmdreg = 0;
static uint64_t datareg = 0;

/* 15 Stage Binary Divider - the 32.768kHz crystal is divided down to 1Hz, and
   this is the position within the second measured in 68K cycles. The 1Hz
   DATA OUT signal and the 64Hz to 4096Hz Timing Pulses are outputs of this
   divider's stages.
*/
static uint32_t cycs = 0;

/* Timing Pulse Interval - the nominal period in whole 68K cycles. Values of a
   second or more select the interval timer, anything shorter selects one of
   the divider's frequency outputs. 2048Hz and 4096Hz are not a whole number of
   cycles (5859.375 and 2929.6875), but as the level is taken from the divider
   position the truncated value only identifies the rate.
*/
static uint32_t tpinterval = 0;

/* Interval Timer Counter - independent of the time counter. Measured in 68K
   cycles, but advanced a 64Hz step at a time: the documented interval timer
   accuracy is +/-15.625ms, or one 64Hz step. The interval signal is high when
   the counter is in the upper half of the interval, so a reset counter sits at
   the halfway point rather than zero (see geo_rtc_interval_reset).
*/
static uint32_t tpcounter = 0;

// Interval Timer Run/Stop
static uint8_t tpmode = 0;

// Function Mode
static uint8_t fmode = 0;

// Time Counter - units separated into variables for ease of use with time.h
static uint32_t year = 0;
static uint32_t month = 0;
static uint32_t weekday = 0;
static uint32_t day = 0;
static uint32_t hour = 0;
static uint32_t minute = 0;
static uint32_t second = 0;

// CLK (Clock) and STB (Strobe) values from previous write operations
static uint8_t clk = 0;
static uint8_t stb = 0;

// 1Hz Interval Timer and Timing Pulse output values
static uint8_t timer_1hz = 0;
static uint8_t tp = 0;

/* The values below are derived from the serialized state, and are recovered
   by geo_rtc_derive when a state is loaded.
*/

// Position of the next 64Hz step of the divider
static uint32_t step64 = CYCS_68K_PER_64HZ;

// Timing Pulse half periods per second for the frequency outputs
static uint32_t tphalves = 0;

/* Interval Output Flag Reset has returned the interval signal to its reset
   level: TP stays high until the interval signal next goes low.
*/
static uint8_t tpflagreset = 0;

// Interval signal level for the current position of the interval timer
static inline uint8_t geo_rtc_interval_signal(void) {
    return tpcounter >= (tpinterval >> 1);
}

/* Reset the interval timer. TP is an N-channel open drain output, and the
   interval signal is a flag for use as an interrupt. The reset level is
   therefore taken to be high (output released), with the flag asserted low
   for the second half of the interval. The datasheet does not state the
   levels. Starting the counter halfway gives a high level first using the
   same relationship between counter and level that states have always held.
*/
static inline void geo_rtc_interval_reset(void) {
    tpcounter = tpinterval >> 1;
}

static inline void geo_rtc_step64_next(void) {
    step64 = ((cycs / CYCS_68K_PER_64HZ) + 1) * CYCS_68K_PER_64HZ;
}

// Select a Timing Pulse period, measured in 68K cycles
static inline void geo_rtc_tp_select(uint32_t interval) {
    tpinterval = interval ? interval : CYCS_68K_PER_SECOND;
    tphalves = (CYCS_68K_PER_SECOND / tpinterval) << 1;
}

/* TP = 64Hz, 256Hz, 2048Hz, 4096Hz: the interval timer stops, and TP outputs
   the divider at 50% duty. Nothing documents the phase being restarted, so it
   stays tied to the divider.
*/
static inline void geo_rtc_tp_frequency(uint32_t interval) {
    geo_rtc_tp_select(interval);
    tpmode = TPMODE_STOP;
    tpflagreset = 0;
}

// TP = 1s, 10s, 30s, 60s interval: the counter is reset and started
static inline void geo_rtc_tp_interval(uint32_t interval) {
    geo_rtc_tp_select(interval);
    geo_rtc_interval_reset();
    tpmode = TPMODE_RUN;
    tpflagreset = 0;
    tp = 1;
}

static void geo_rtc_derive(void) {
    geo_rtc_step64_next();
    geo_rtc_tp_select(tpinterval);
    tp &= 0x01;
    tpflagreset = (tpinterval >= CYCS_68K_PER_SECOND) &&
        !geo_rtc_interval_signal() && tp;
}

// Frontend start time callback: local time as seconds since the epoch
static int (*geo_rtc_cb)(int64_t*) = NULL;

// Set the callback supplying the start time, or NULL to use the host clock
void geo_rtc_set_callback(int (*cb)(int64_t*)) {
    geo_rtc_cb = cb;
}

void geo_rtc_init(void) {
    // Zero the 1Hz Interval Timer and Command register
    cycs = 0;
    cmdreg = 0;

    /* Seed the time data with the start time from the frontend if it gives
       one, otherwise with the current system time. The start time is local
       time counted as UTC, so gmtime yields the local fields.
    */
    struct tm *timeinfo = NULL;
    time_t rawtime;
    int64_t localsecs;

    if (geo_rtc_cb && geo_rtc_cb(&localsecs)) {
        rawtime = (time_t)localsecs;
        if ((int64_t)rawtime == localsecs) // Fits in time_t
            timeinfo = gmtime(&rawtime);
    }

    if (timeinfo == NULL) {
        time(&rawtime);
        timeinfo = localtime(&rawtime);
    }

    /* Data types are: second, minute, hour, day, weekday, month, year
       A 24-hour system and the last two digits of the Gregorian year are used
       Leap years are multiples of 4
       Data is represented in BCD notation except for months, which use hex.
       Weekdays are 0-6, with Sunday being 0 and Saturday being 6.
    */
    year = timeinfo->tm_year % 100;
    month = timeinfo->tm_mon + 1;
    weekday = timeinfo->tm_wday;
    day = timeinfo->tm_mday;
    hour = timeinfo->tm_hour;
    minute = timeinfo->tm_min;
    second = timeinfo->tm_sec;

    // Zero the CLK and STB inputs
    clk = 0;
    stb = 0;

    /* The chip is powered by the backup battery and runs from its own crystal
       while the system is off, and a mode stays latched until another in the
       same command group is set. There is therefore no power-on state to
       speak of: start with the 4096Hz Timing Pulse selected, which is what
       the MVS system ROM's calendar test leaves behind on every boot.
    */
    tp = 0;
    tpmode = TPMODE_STOP;
    tpcounter = 0;
    tpinterval = CYCS_68K_PER_SECOND / 4096;

    geo_rtc_derive();
}

uint8_t geo_rtc_rd(void) {
    /* In Register Hold Mode and Time Read Mode, the DATA OUT pin is set to the
       1Hz Interval Timer value, otherwise it is set to the lowest bit of the
       Shift Register.
    */
    uint8_t dataout =
        ((fmode == 0 || fmode == 3) ? timer_1hz : (datareg & 0x01)) << 1;

    /* Return the values of the DATA OUT and TP pins -- this is set up for easy
       use in the Neo Geo MVS' REG_STATUS_A reads.
    */
    return dataout | tp;
}

void geo_rtc_wr(uint8_t data) {
    // Check for CLK and STB going from low to high
    int clk_lowhigh = !clk && (data & 0x02);
    int stb_lowhigh = !stb && (data & 0x04);

    // Update CLK and STB
    clk = data & 0x02;
    stb = data & 0x04;

    // If STB is going low to high, process a command
    if (stb_lowhigh) {
        switch (cmdreg & 0x0f) {
            case 0x00: { // Register Hold -- DATA OUT = 1Hz
                /* The 48-bit shift register is held. The command register is
                   not held.
                */
                fmode = 0;

                /* Register Hold belongs to the register control group and
                   leaves the Timing Pulse as it was. It is only documented
                   to output 64Hz to TP when it cancels the test mode, which
                   is not emulated.
                */
                break;
            }
            case 0x01: { // Register Shift -- DATA OUT = [LSB] (0/1)
                fmode = 1;
                break;
            }
            case 0x02: { // Time Set & Counter Hold -- DATA OUT = [LSB] (0/1)
                /* Stages 10-15 of the divider are reset and the time counter
                   is held until Register Hold, Register Shift, or Time Read
                   is set. Stages 1-9 (64Hz and faster) keep running, which is
                   where the +/-15.625ms time setting accuracy comes from.
                */
                fmode = 2;
                cycs %= CYCS_68K_PER_64HZ;
                geo_rtc_step64_next();

                /* Data is transferred from the 48-bit shift register to the
                   time counter.
                */
                second =
                    (datareg & 0x0f) + (((datareg >> 4) & 0x0f) * 10);
                minute =
                    ((datareg >> 8) & 0x0f) + (((datareg >> 12) & 0x0f) * 10);
                hour =
                    ((datareg >> 16) & 0x0f) + (((datareg >> 20) & 0x0f) * 10);
                day =
                    ((datareg >> 24) & 0x0f) + (((datareg >> 28) & 0x0f) * 10);
                weekday =
                    ((datareg >> 32) & 0x0f);
                month =
                    ((datareg >> 36) & 0x0f);
                year =
                    ((datareg >> 40) & 0x0f) + (((datareg >> 44) & 0x0f) * 10);
                break;
            }
            case 0x03: { // Time Read -- DATA OUT = 1Hz
                fmode = 3;

                /* Data is transferred from the time counter to the 48-bit
                   shift register.
                */
                datareg = 0; // Reset it
                datareg |= second % 10;
                datareg |= (second / 10) << 4;
                datareg |= (minute % 10) << 8;
                datareg |= (minute / 10) << 12;
                datareg |= (hour % 10) << 16;
                datareg |= (hour / 10) << 20;
                datareg |= (day % 10) << 24;
                datareg |= (day / 10) << 28;
                datareg |= ((uint64_t)weekday) << 32;
                datareg |= ((uint64_t)month) << 36;
                datareg |= ((uint64_t)year % 10) << 40;
                datareg |= ((uint64_t)year / 10) << 44;
                break;
            }
            case 0x04: { // TP = 64Hz
                geo_rtc_tp_frequency(CYCS_68K_PER_SECOND / 64);
                break;
            }
            case 0x05: { // TP = 256Hz
                geo_rtc_tp_frequency(CYCS_68K_PER_SECOND / 256);
                break;
            }
            case 0x06: { // TP = 2048Hz
                geo_rtc_tp_frequency(CYCS_68K_PER_SECOND / 2048);
                break;
            }
            case 0x07: { // TP = 4096Hz
                geo_rtc_tp_frequency(CYCS_68K_PER_SECOND / 4096);
                break;
            }
            case 0x08: { // TP = 1s interval set (counter reset & start)
                geo_rtc_tp_interval(CYCS_68K_PER_SECOND);
                break;
            }
            case 0x09: { // TP = 10s interval set (counter reset & start)
                geo_rtc_tp_interval(CYCS_68K_PER_SECOND * 10);
                break;
            }
            case 0x0a: { // TP = 30s interval set (counter reset & start)
                geo_rtc_tp_interval(CYCS_68K_PER_SECOND * 30);
                break;
            }
            case 0x0b: { // TP = 60s interval set (counter reset & start)
                geo_rtc_tp_interval(CYCS_68K_PER_SECOND * 60);
                break;
            }
            case 0x0c: { // Interval Output Flag Reset
                /* The interval signal to the TP pin is reset. The interval
                   timer counter continues operation.
                */
                if (tpinterval >= CYCS_68K_PER_SECOND) {
                    tp = 1;
                    tpflagreset = 1;
                }
                tpmode = TPMODE_RUN;
                break;
            }
            case 0x0d: { // Interval Timer Clock Run
                /* The timer for outputting interval signals is reset, then
                   started.
                */
                geo_rtc_interval_reset();
                tpmode = TPMODE_RUN;
                break;
            }
            case 0x0e: { // Interval Timer Clock Stop
                /* The timer for outputting interval signals is stopped. The
                   output status does not change.
                */
                tpmode = TPMODE_STOP;
                break;
            }
            case 0x0f: { // TEST MODE SET
                break;
            }
        }
    }

    /* If this is not an STB write and the CLK line is going from low to high,
       shift a new bit into the shift register.
    */
    if (!stb && clk_lowhigh) {
        // Command Register
        uint64_t shift_out = cmdreg & 0x01 ? 0x800000000000 : 0x00;
        cmdreg = ((cmdreg >> 1) & 0x07) | ((data & 0x01) << 3);

        // Data Register
        if (fmode == 1) // Data Register - only in Register Shift Mode
            datareg = ((datareg >> 1) & 0x7fffffffffff) | shift_out;
    }
}

static inline void geo_rtc_clk_inc(void) {
    if (++second < 60)
        return;
    else
        second = 0;

    if (++minute < 60)
        return;
    else
        minute = 0;

    if (++hour < 24)
        return;
    else
        hour = 0;

    if (++weekday >= 7)
        weekday = 0;

    /* Thirty days hath September, April, June, and November. All the rest have
       thirty-one, except February, twenty-eight days clear, and twenty-nine in
       each leap year.
    */
    uint32_t monthdays[12] = {
        31, (year % 4 ? 28 : 29), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (++day <= monthdays[month - 1])
        return;
    else
        day = 1; // No 0th day

    if (++month <= 12)
        return;
    else
        month = 1; // No 0th month

    if (++year < 100)
        return;
    year = 0; // If you really trigger this, you win at life.
}

void geo_rtc_sync(unsigned scycs) {
    cycs += scycs;

    // Count the 64Hz steps of the divider which have passed
    uint32_t steps = 0;
    while (cycs >= step64) {
        ++steps;
        step64 += CYCS_68K_PER_64HZ;
    }

    if (fmode == 2) {
        // Time Set & Counter Hold: stages 10-15 of the divider stay reset
        if (cycs >= CYCS_68K_PER_64HZ) {
            cycs %= CYCS_68K_PER_64HZ;
            geo_rtc_step64_next();
        }
    }
    else if (cycs >= CYCS_68K_PER_SECOND) {
        // Increment the clock by one second
        geo_rtc_clk_inc();

        cycs -= CYCS_68K_PER_SECOND;
        geo_rtc_step64_next();
    }

    /* The 1Hz Interval Timer value is output at 50% duty. Set it high for the
       second half of the interval.
    */
    timer_1hz = cycs >= (CYCS_68K_PER_SECOND >> 1);

    if (tpinterval < CYCS_68K_PER_SECOND) {
        /* Frequency output at 50% duty, taken from the divider: the output
           of a divider stage is low for the first half of its period.
        */
        tp = (((uint64_t)cycs * tphalves) / CYCS_68K_PER_SECOND) & 0x01;
    }
    else if (tpmode == TPMODE_RUN) {
        // The interval timer is clocked by the divider's 64Hz output
        tpcounter += steps * CYCS_68K_PER_64HZ;

        if (tpcounter >= tpinterval)
            tpcounter -= tpinterval;

        /* The interval signal is rectangular (50% duty) if not reset: high for
           the first half of the interval, low for the second half. If reset,
           it stays high until the next time it would go low.
        */
        uint8_t signal = geo_rtc_interval_signal();

        if (signal)
            tpflagreset = 0;

        tp = signal || tpflagreset;
    }
}

void geo_rtc_state_load(uint8_t *st) {
    cmdreg = geo_serial_pop8(st);
    datareg = geo_serial_pop64(st);
    cycs = geo_serial_pop32(st);
    tpinterval = geo_serial_pop32(st);
    tpcounter = geo_serial_pop32(st);
    tpmode = geo_serial_pop8(st);
    fmode = geo_serial_pop8(st);
    year = geo_serial_pop32(st);
    month = geo_serial_pop32(st);
    weekday = geo_serial_pop32(st);
    day = geo_serial_pop32(st);
    hour = geo_serial_pop32(st);
    minute = geo_serial_pop32(st);
    second = geo_serial_pop32(st);
    clk = geo_serial_pop8(st);
    stb = geo_serial_pop8(st);
    timer_1hz = geo_serial_pop8(st);
    tp = geo_serial_pop8(st);

    geo_rtc_derive();
}

void geo_rtc_state_save(uint8_t *st) {
    geo_serial_push8(st, cmdreg);
    geo_serial_push64(st, datareg);
    geo_serial_push32(st, cycs);
    geo_serial_push32(st, tpinterval);
    geo_serial_push32(st, tpcounter);
    geo_serial_push8(st, tpmode);
    geo_serial_push8(st, fmode);
    geo_serial_push32(st, year);
    geo_serial_push32(st, month);
    geo_serial_push32(st, weekday);
    geo_serial_push32(st, day);
    geo_serial_push32(st, hour);
    geo_serial_push32(st, minute);
    geo_serial_push32(st, second);
    geo_serial_push8(st, clk);
    geo_serial_push8(st, stb);
    geo_serial_push8(st, timer_1hz);
    geo_serial_push8(st, tp);
}
