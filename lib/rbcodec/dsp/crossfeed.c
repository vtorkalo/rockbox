/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 Thom Johansen
 * Copyright (C) 2012 Michael Sevakis
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
#include "config.h"
#include <stdbool.h>
#include <sys/types.h>
#include "fixedpoint.h"
#include "fracmul.h"
#include "replaygain.h"
#include "dsp_proc_entry.h"
#include "dsp_filter.h"
#include "crossfeed.h"
#include <string.h>

/* Implemented here or in target assembly code */
void crossfeed_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p);

/**
 * Applies crossfeed to the stereo signal.
 *
 * Crossfeed is a process where listening over speakers is simulated. This
 * is good for old hard panned stereo records, which might be quite fatiguing
 * to listen to on headphones with no crossfeed.
 */

/* Crossfeed */
static struct crossfeed_state
{
    int32_t gain;           /* 00h: Direct path gain */
    int32_t coefs[3];       /* 04h: Coefficients for the shelving filter */
    int32_t history[4];     /* 10h: Format is x[n - 1], y[n - 1] (L + R) */
    int32_t delay[13*2];    /* 20h: Delay line buffer (L + R interleaved) */
    int32_t *index;         /* 88h: Current pointer into the delay line */
    struct dsp_config *dsp; /* 8ch: Current DSP */
                            /* 90h */
} crossfeed_state IBSS_ATTR;

/* Discard the sample histories */
static void crossfeed_flush(struct dsp_proc_entry *this)
{
    struct crossfeed_state *state = (void *)this->data;
    memset(state->history, 0, sizeof (state->history));
    memset(state->delay, 0, sizeof (state->delay));
    state->index = state->delay;
}


/** DSP interface **/

/* Crossfeed boot/format change function */
static void crossfeed_process_new_format(struct dsp_proc_entry *this,
                                         struct dsp_buffer **buf_p)
{
    struct crossfeed_state *state = (void *)this->data;
    struct dsp_buffer *buf = *buf_p;

    DSP_PRINT_FORMAT(DSP_PROC_CROSSFEED, DSP_PROC_CROSSFEED, buf->format);

    bool active = buf->format.num_channels >= 2;
    dsp_proc_activate(state->dsp, DSP_PROC_CROSSFEED, active);

    if (!active)
    {
        /* Can't do this. Sleep until next change */
        crossfeed_flush(this);
        DEBUGF("  DSP_PROC_CROSSFEED- deactivated\n");
        return;
    }

    /* Switch to the real function and call it once */
    this->process[0] = crossfeed_process;
    dsp_proc_call(this, buf_p, (unsigned)buf->format.changed - 1);
}

/* Enable or disable the crossfeed */
void dsp_crossfeed_enable(bool enable)
{
    if (enable != !crossfeed_state.dsp)
        return;

    struct dsp_config *dsp = dsp_get_config(CODEC_IDX_AUDIO);
    dsp_proc_enable(dsp, DSP_PROC_CROSSFEED, enable);
}

/* Set the gain of the dry mix */
void dsp_set_crossfeed_direct_gain(int gain)
{
    uint32_t gain32 = get_replaygain_int(gain * 10);
    crossfeed_state.gain =
        gain32 >= (0x80000000ul >> 7) ? 0x7ffffffful: (gain32 << 7);
}

/* Both gains should be below 0 dB */
void dsp_set_crossfeed_cross_params(long lf_gain, long hf_gain, long cutoff)
{
    int32_t *c = crossfeed_state.coefs;
    long scaler = get_replaygain_int(lf_gain * 10) << 7;

    cutoff = 0xffffffff / NATIVE_FREQUENCY * cutoff;
    hf_gain -= lf_gain;
    /* Divide cutoff by sqrt(10^(hf_gain/20)) to place cutoff at the -3 dB
     * point instead of shelf midpoint. This is for compatibility with the old
     * crossfeed shelf filter and should be removed if crossfeed settings are
     * ever made incompatible for any other good reason.
     */
    cutoff = fp_div(cutoff, get_replaygain_int(hf_gain*5), 24);
    filter_shelf_coefs(cutoff, hf_gain, false, c);
    /* Scale coefs by LF gain and shift them to s0.31 format. We have no gains
     * over 1 and can do this safely
     */
    c[0] = FRACMUL_SHL(c[0], scaler, 4);
    c[1] = FRACMUL_SHL(c[1], scaler, 4);
    c[2] <<= 4;
}

#if !defined(CPU_COLDFIRE) && !defined(CPU_ARM)
/* Apply the crossfade to the buffer in place */
void crossfeed_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p)
{
    struct crossfeed_state *state = (void *)this->data;
    struct dsp_buffer *buf = *buf_p;
   
    int32_t *hist_l = &state->history[0];
    int32_t *hist_r = &state->history[2];
    int32_t *delay = state->delay;
    int32_t *coefs = &state->coefs[0];
    int32_t gain = state->gain;
    int32_t *di = state->index;

    int count = buf->remcount;

    for (int i = 0; i < count; i++)
    {
        int32_t left = buf->p32[0][i];
        int32_t right = buf->p32[1][i];

        /* Filter delayed sample from left speaker */
        int32_t acc = FRACMUL(*di, coefs[0]);
        acc += FRACMUL(hist_l[0], coefs[1]);
        acc += FRACMUL(hist_l[1], coefs[2]);
        /* Save filter history for left speaker */
        hist_l[1] = acc;
        hist_l[0] = *di;
        *di++ = left;
        /* Filter delayed sample from right speaker */
        acc = FRACMUL(*di, coefs[0]);
        acc += FRACMUL(hist_r[0], coefs[1]);
        acc += FRACMUL(hist_r[1], coefs[2]);
        /* Save filter history for right speaker */
        hist_r[1] = acc;
        hist_r[0] = *di;
        *di++ = right;
        /* Now add the attenuated direct sound and write to outputs */
        buf->p32[0][i] = FRACMUL(left, gain) + hist_r[1];
        buf->p32[1][i] = FRACMUL(right, gain) + hist_l[1];

        /* Wrap delay line index if bigger than delay line size */
        if (di >= delay + 13*2)
            di = delay;
    }

    /* Write back local copies of data we've modified */
    state->index = di;
}
#endif /* CPU */

/* DSP message hook */
static intptr_t crossfeed_configure(struct dsp_proc_entry *this,
                                    struct dsp_config *dsp,
                                    unsigned int setting,
                                    intptr_t value)
{
    switch (setting)
    {
    case DSP_PROC_INIT:
        this->data = (intptr_t)&crossfeed_state;
        this->process[0] = crossfeed_process_new_format;
        this->process[1] = crossfeed_process_new_format;
        ((struct crossfeed_state *)this->data)->dsp = dsp;
        dsp_proc_activate(dsp, DSP_PROC_CROSSFEED, true);
    case DSP_FLUSH:
        crossfeed_flush(this);
        break;

    case DSP_PROC_CLOSE:
        ((struct crossfeed_state *)this->data)->dsp = NULL;
        break;
    }

    return 1;
    (void)value;
}

/* Database entry */
DSP_PROC_DB_ENTRY(
    CROSSFEED,
    crossfeed_configure);
