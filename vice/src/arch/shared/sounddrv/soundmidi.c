/*
 * soundmidi.c - SID to Standard MIDI File (SMF) recording
 *
 * Intercepts SID register writes via sound_store() and converts them to MIDI
 * events, producing a Type-0 SMF when recording is stopped.
 *
 * Each SID voice maps to one MIDI channel (channels 0-2 in zero-based, i.e.
 * MIDI channels 1-3).  Gate-bit transitions generate Note On / Note Off
 * events.  Attack/Decay/Sustain/Release register changes generate MIDI CC
 * events for Attack Time (CC 73), Decay Time (CC 75), Sound Controller 1
 * (CC 70, used for sustain level) and Release Time (CC 72).
 *
 * Written by the VICE contributors.
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sound.h"
#include "types.h"
#include "log.h"
#include "machine.h"

/* SID register offsets within a single voice block (7 bytes per voice) */
#define REG_FREQ_LO   0   /* Frequency low byte                          */
#define REG_FREQ_HI   1   /* Frequency high byte                         */
#define REG_PW_LO     2   /* Pulse width low byte  (not used for MIDI)   */
#define REG_PW_HI     3   /* Pulse width high nibble                     */
#define REG_CTRL      4   /* Control: bits[7:4]=waveform, bit[0]=gate    */
#define REG_ATK_DCY   5   /* Attack[7:4], Decay[3:0]                     */
#define REG_STN_REL   6   /* Sustain[7:4], Release[3:0]                  */

#define SID_NUM_VOICES    3
#define SID_REGS_PER_VOICE 7

/* MIDI file timing */
#define MIDI_PPQN        480       /* Ticks per quarter note              */
#define MIDI_TEMPO_US    500000UL  /* Microseconds per beat (= 120 BPM)   */

/* MIDI CC numbers for ADSR mapping */
#define MIDI_CC_ATTACK   73  /* Sound Controller 4 – Attack Time  */
#define MIDI_CC_DECAY    75  /* Sound Controller 6 – Decay Time   */
#define MIDI_CC_SUSTAIN  70  /* Sound Controller 1 – Sustain lvl  */
#define MIDI_CC_RELEASE  72  /* Sound Controller 3 – Release Time */

/* Fixed velocity used for all Note On events */
#define MIDI_VELOCITY    100

/* --------------------------------------------------------------------------
 * Dynamic byte buffer used to accumulate the MIDI track before writing
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} byte_buf_t;

static void buf_put(byte_buf_t *b, uint8_t byte)
{
    if (b->size >= b->capacity) {
        size_t new_cap = b->capacity ? b->capacity * 2 : 8192;
        uint8_t *p = realloc(b->data, new_cap);
        if (!p) {
            return;  /* silently drop on OOM */
        }
        b->data     = p;
        b->capacity = new_cap;
    }
    b->data[b->size++] = byte;
}

/* Write a MIDI variable-length quantity (big-endian 7-bit groups). */
static void buf_put_vlq(byte_buf_t *b, uint32_t val)
{
    uint8_t tmp[4];
    int     n = 0;

    tmp[n++] = (uint8_t)(val & 0x7F);
    val >>= 7;
    while (val) {
        tmp[n++] = (uint8_t)(0x80 | (val & 0x7F));
        val >>= 7;
    }
    /* Write high group first */
    while (--n >= 0) {
        buf_put(b, tmp[n]);
    }
}

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

/* Per-voice tracking */
typedef struct {
    uint8_t freq_lo;
    uint8_t freq_hi;
    uint8_t ctrl;
    int     playing_note;  /* current MIDI note being held; -1 = silent */
} voice_state_t;

static FILE         *midi_file  = NULL;
static int           recording  = 0;
static int           start_set  = 0;   /* whether start_clk has been set */
static CLOCK         start_clk  = 0;
static CLOCK         last_event_clk = 0;
static uint32_t      last_ticks = 0;
static long          clock_hz   = 985248L; /* updated from machine on start */
static byte_buf_t    track_buf  = { NULL, 0, 0 };
static voice_state_t voices[SID_NUM_VOICES];

/* --------------------------------------------------------------------------
 * Timing helpers
 * -------------------------------------------------------------------------- */

/*
 * Convert an absolute CPU-clock value to a MIDI tick offset from the
 * recording start.
 *
 * ticks = (clk - start_clk)  *  PPQN  *  1_000_000
 *         ------------------------------------------
 *                clock_hz  *  MIDI_TEMPO_US
 *
 * Uses 64-bit arithmetic to avoid overflow during typical session lengths.
 */
static uint32_t clk_to_ticks(CLOCK clk)
{
    uint64_t delta = (clk > start_clk) ? (uint64_t)(clk - start_clk) : 0ULL;
    uint64_t numer = delta * (uint64_t)MIDI_PPQN * 1000000ULL;
    uint64_t denom = (uint64_t)clock_hz * (uint64_t)MIDI_TEMPO_US;
    return (denom > 0) ? (uint32_t)(numer / denom) : 0;
}

/* --------------------------------------------------------------------------
 * MIDI event emission into track_buf
 * -------------------------------------------------------------------------- */

static void emit(const uint8_t *bytes, int len, CLOCK clk)
{
    uint32_t ticks = clk_to_ticks(clk);
    uint32_t delta = (ticks >= last_ticks) ? (ticks - last_ticks) : 0;
    last_ticks = ticks;

    buf_put_vlq(&track_buf, delta);
    while (len-- > 0) {
        buf_put(&track_buf, *bytes++);
    }
}

static void emit_note_on(int ch, int note, CLOCK clk)
{
    uint8_t ev[3];
    ev[0] = (uint8_t)(0x90 | (ch   & 0x0F));
    ev[1] = (uint8_t)(note  & 0x7F);
    ev[2] = (uint8_t)MIDI_VELOCITY;
    emit(ev, 3, clk);
}

static void emit_note_off(int ch, int note, CLOCK clk)
{
    uint8_t ev[3];
    ev[0] = (uint8_t)(0x80 | (ch  & 0x0F));
    ev[1] = (uint8_t)(note  & 0x7F);
    ev[2] = 0x00;
    emit(ev, 3, clk);
}

static void emit_cc(int ch, int cc, int value, CLOCK clk)
{
    uint8_t ev[3];
    ev[0] = (uint8_t)(0xB0 | (ch    & 0x0F));
    ev[1] = (uint8_t)(cc    & 0x7F);
    ev[2] = (uint8_t)(value & 0x7F);
    emit(ev, 3, clk);
}

/* --------------------------------------------------------------------------
 * SID frequency register → MIDI note number
 * -------------------------------------------------------------------------- */

/*
 * Convert a 16-bit SID frequency register to the nearest MIDI note (0-127).
 * Returns -1 if the frequency is zero or maps outside the MIDI range.
 *
 * SID freq (Hz) = freq_reg * clock_hz / 2^24
 * MIDI note     = 69 + 12 * log2(freq_hz / 440)
 */
static int freq_to_note(uint16_t freq_reg)
{
    double freq_hz, note_f;
    int    note;

    if (freq_reg == 0) {
        return -1;
    }
    freq_hz = (double)freq_reg * (double)clock_hz / 16777216.0;
    if (freq_hz <= 0.0) {
        return -1;
    }
    /* log2(x) = log(x) / log(2) — avoids a log2() portability dependency */
    note_f = 69.0 + 12.0 * (log(freq_hz / 440.0) / log(2.0));
    note   = (int)(note_f + 0.5); /* round to nearest semitone */
    if (note < 0 || note > 127) {
        return -1;
    }
    return note;
}

/* --------------------------------------------------------------------------
 * Per-voice SID register processing
 * -------------------------------------------------------------------------- */

static void process_voice_reg(int voice, int reg, uint8_t val, CLOCK clk)
{
    voice_state_t *v  = &voices[voice];
    int            ch = voice; /* MIDI channel 0–2 → MIDI channels 1–3 */

    switch (reg) {
        case REG_FREQ_LO:
            v->freq_lo = val;
            break;

        case REG_FREQ_HI:
            v->freq_hi = val;
            break;

        case REG_CTRL: {
            int old_gate = v->ctrl & 0x01;
            int new_gate = val     & 0x01;
            v->ctrl      = val;

            if (!old_gate && new_gate) {
                /* Rising edge on gate: Note On */
                uint16_t freq_reg = ((uint16_t)v->freq_hi << 8) | v->freq_lo;
                int      note     = freq_to_note(freq_reg);

                if (note >= 0) {
                    /* Release any held note on this channel first */
                    if (v->playing_note >= 0) {
                        emit_note_off(ch, v->playing_note, clk);
                    }
                    emit_note_on(ch, note, clk);
                    v->playing_note = note;
                }
            } else if (old_gate && !new_gate) {
                /* Falling edge on gate: Note Off */
                if (v->playing_note >= 0) {
                    emit_note_off(ch, v->playing_note, clk);
                    v->playing_note = -1;
                }
            }
            break;
        }

        case REG_ATK_DCY:
            /* Map 4-bit values (0-15) to MIDI CC range (7-127). */
            emit_cc(ch, MIDI_CC_ATTACK,  ((val >> 4) & 0x0F) * 8 + 7, clk);
            emit_cc(ch, MIDI_CC_DECAY,    (val       & 0x0F) * 8 + 7, clk);
            break;

        case REG_STN_REL:
            emit_cc(ch, MIDI_CC_SUSTAIN, ((val >> 4) & 0x0F) * 8 + 7, clk);
            emit_cc(ch, MIDI_CC_RELEASE,  (val       & 0x0F) * 8 + 7, clk);
            break;

        default:
            /* Pulse-width registers (REG_PW_LO, REG_PW_HI) – not mapped */
            break;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/** \brief  Returns non-zero while MIDI recording is active. */
int sid_midi_is_recording(void)
{
    return recording;
}

/**
 * \brief  Start recording SID output as MIDI to \a filename.
 *
 * \param[in]  filename  Path for the output .mid file.
 *                       Uses "vice-sid.mid" when NULL.
 * \return  0 on success, -1 on failure.
 */
int sid_midi_record_start(const char *filename)
{
    int      i;
    uint8_t  tempo_ev[7];

    if (recording) {
        sid_midi_record_stop();
    }

    /* Open output file */
    midi_file = fopen(filename ? filename : "vice-sid.mid", "wb");
    if (!midi_file) {
        log_message(LOG_DEFAULT,
                    "sid-midi: ERROR cannot open '%s' for writing",
                    filename ? filename : "vice-sid.mid");
        return -1;
    }

    /* Allocate track buffer */
    track_buf.size     = 0;
    track_buf.capacity = 8192;
    track_buf.data     = malloc(track_buf.capacity);
    if (!track_buf.data) {
        fclose(midi_file);
        midi_file = NULL;
        log_message(LOG_DEFAULT, "sid-midi: ERROR out of memory");
        return -1;
    }

    /* Capture actual machine clock rate for accurate timing */
    clock_hz = machine_get_cycles_per_second();
    if (clock_hz <= 0) {
        clock_hz = 985248L;  /* PAL C64 fallback */
    }

    /* Reset voice state */
    for (i = 0; i < SID_NUM_VOICES; i++) {
        voices[i].freq_lo      = 0;
        voices[i].freq_hi      = 0;
        voices[i].ctrl         = 0;
        voices[i].playing_note = -1;
    }

    /*
     * Write the initial tempo meta-event at delta-time 0.
     * Format: 00  FF 51 03  tt tt tt
     */
    tempo_ev[0] = 0x00;   /* delta = 0 */
    tempo_ev[1] = 0xFF;   /* meta event */
    tempo_ev[2] = 0x51;   /* set tempo */
    tempo_ev[3] = 0x03;   /* 3 bytes follow */
    tempo_ev[4] = (uint8_t)((MIDI_TEMPO_US >> 16) & 0xFF);
    tempo_ev[5] = (uint8_t)((MIDI_TEMPO_US >>  8) & 0xFF);
    tempo_ev[6] = (uint8_t)((MIDI_TEMPO_US      ) & 0xFF);
    for (i = 0; i < 7; i++) {
        buf_put(&track_buf, tempo_ev[i]);
    }

    start_set       = 0;
    last_ticks      = 0;
    last_event_clk  = 0;
    recording       = 1;

    log_message(LOG_DEFAULT, "sid-midi: recording started → '%s'",
                filename ? filename : "vice-sid.mid");
    return 0;
}

/**
 * \brief  Stop MIDI recording and flush the completed SMF to disk.
 */
void sid_midi_record_stop(void)
{
    int     i;
    uint8_t eot[4];

    if (!recording) {
        return;
    }

    recording = 0;

    /* Release any notes still held at the last observed clock */
    for (i = 0; i < SID_NUM_VOICES; i++) {
        if (voices[i].playing_note >= 0) {
            emit_note_off(i, voices[i].playing_note, last_event_clk);
            voices[i].playing_note = -1;
        }
    }

    /* End-of-track meta event: 00  FF 2F 00 */
    eot[0] = 0x00;
    eot[1] = 0xFF;
    eot[2] = 0x2F;
    eot[3] = 0x00;
    for (i = 0; i < 4; i++) {
        buf_put(&track_buf, eot[i]);
    }

    if (midi_file) {
        uint32_t trk_len = (uint32_t)track_buf.size;
        /* MIDI header chunk */
        static const uint8_t midi_hdr[14] = {
            'M', 'T', 'h', 'd',        /* chunk ID          */
            0x00, 0x00, 0x00, 0x06,     /* chunk length = 6  */
            0x00, 0x00,                  /* format 0          */
            0x00, 0x01,                  /* 1 track           */
            (uint8_t)((MIDI_PPQN >> 8) & 0xFF),
            (uint8_t)((MIDI_PPQN     ) & 0xFF)  /* PPQN       */
        };
        fwrite(midi_hdr, 1, sizeof(midi_hdr), midi_file);

        /* Track chunk header */
        {
            uint8_t th[8];
            th[0] = 'M'; th[1] = 'T'; th[2] = 'r'; th[3] = 'k';
            th[4] = (uint8_t)((trk_len >> 24) & 0xFF);
            th[5] = (uint8_t)((trk_len >> 16) & 0xFF);
            th[6] = (uint8_t)((trk_len >>  8) & 0xFF);
            th[7] = (uint8_t)((trk_len      ) & 0xFF);
            fwrite(th, 1, sizeof(th), midi_file);
        }

        /* Track data */
        if (track_buf.size > 0) {
            fwrite(track_buf.data, 1, track_buf.size, midi_file);
        }

        fclose(midi_file);
        midi_file = NULL;
        log_message(LOG_DEFAULT,
                    "sid-midi: recording stopped (%zu track bytes written)",
                    track_buf.size);
    }

    free(track_buf.data);
    track_buf.data     = NULL;
    track_buf.size     = 0;
    track_buf.capacity = 0;
}

/**
 * \brief  Called from sound_store() for every SID register write.
 *
 * \param[in]  addr  SID register offset within the chip (0-24).
 * \param[in]  val   New register value.
 * \param[in]  clk   Current CPU clock value (maincpu_clk).
 */
void sid_midi_record_store(uint16_t addr, uint8_t val, CLOCK clk)
{
    int voice, reg;

    if (!recording) {
        return;
    }

    /* Latch start clock on the very first SID write after recording begins */
    if (!start_set) {
        start_clk      = clk;
        last_ticks     = 0;
        last_event_clk = clk;
        start_set      = 1;
    }
    last_event_clk = clk;

    /* Only handle voice registers 0-20; ignore filter/volume (21-24) */
    if (addr >= (uint16_t)(SID_NUM_VOICES * SID_REGS_PER_VOICE)) {
        return;
    }

    voice = (int)addr / SID_REGS_PER_VOICE;
    reg   = (int)addr % SID_REGS_PER_VOICE;

    process_voice_reg(voice, reg, val, clk);
}
