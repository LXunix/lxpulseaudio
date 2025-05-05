/***
    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>
    Copyright 2012 Niels Ole Salscheider <niels_ole@salscheider-online.de>
    Contributor: Alexander E. Patrakov <patrakov@gmail.com>
    Copyright 2020 Christopher Snowhill <kode54@gmail.com>

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>

#include <fftw3.h>

#include <modules/virtual-sink-common.h>

#include <pulse/gccmacro.h>
#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/sound-file.h>
#include <pulsecore/resampler.h>


PA_MODULE_AUTHOR("Christopher Snowhill");
PA_MODULE_DESCRIPTION(_("Virtual surround sink"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        _("sink_name=<name for the sink> "
          "sink_properties=<properties for the sink> "
          "master=<name of sink to filter> "
          "sink_master=<name of sink to filter> "
          "format=<sample format> "
          "rate=<sample rate> "
          "channels=<number of channels> "
          "channel_map=<channel map> "
          "use_volume_sharing=<yes or no> "
          "force_flat_volume=<yes or no> "
          "hrir=/path/to/left_hrir.wav "
          "hrir_left=/path/to/left_hrir.wav "
          "hrir_right=/path/to/optional/right_hrir.wav "
          "autoloaded=<set if this module is being loaded automatically> "
        ));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)
#define DEFAULT_AUTOLOADED false

struct userdata {
    pa_module *module;

    pa_vsink *vsink;

    size_t fftlen;
    size_t hrir_samples;
    size_t inputs;

    fftwf_plan *p_fw, p_bw;
    fftwf_complex *f_in, *f_out, **f_ir;
    float *revspace, *outspace[2], **inspace;
};

#define BLOCK_SIZE (512)

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "master",  /* Will be deprecated. */
    "sink_master",
    "format",
    "rate",
    "channels",
    "channel_map",
    "use_volume_sharing",
    "force_flat_volume",
    "autoloaded",
    "hrir",
    "hrir_left",
    "hrir_right",
    NULL
};

static void filter_process_chunk(uint8_t *src_p, uint8_t *dst_p, unsigned in_count, unsigned out_count, void *userdata) {
    struct userdata *u;
    int ear;
    unsigned c;
    size_t s, fftlen;
    float fftlen_if, *revspace;
    float *src, *dst;

    pa_assert_se(u = userdata);
    pa_assert(in_count == u->fftlen);
    pa_assert(out_count == BLOCK_SIZE);

    src = (float *)src_p;
    dst = (float *)dst_p;

    for (c = 0; c < u->inputs; c++) {
        for (s = 0, fftlen = u->fftlen; s < fftlen; s++) {
            u->inspace[c][s] = src[s * u->inputs + c];
        }
    }

    fftlen_if = 1.0f / (float)u->fftlen;
    revspace = u->revspace + u->fftlen - BLOCK_SIZE;

    pa_memzero(u->outspace[0], BLOCK_SIZE * 4);
    pa_memzero(u->outspace[1], BLOCK_SIZE * 4);

    for (c = 0; c < u->inputs; c++) {
        fftwf_complex *f_in = u->f_in;
        fftwf_complex *f_out = u->f_out;

        fftwf_execute(u->p_fw[c]);

        for (ear = 0; ear < 2; ear++) {
            fftwf_complex *f_ir = u->f_ir[c * 2 + ear];
            float *outspace = u->outspace[ear];

            for (s = 0, fftlen = u->fftlen / 2 + 1; s < fftlen; s++) {
                float re = f_ir[s][0] * f_in[s][0] - f_ir[s][1] * f_in[s][1];
                float im = f_ir[s][1] * f_in[s][0] + f_ir[s][0] * f_in[s][1];
                f_out[s][0] = re;
                f_out[s][1] = im;
            }

            fftwf_execute(u->p_bw);

            for (s = 0, fftlen = BLOCK_SIZE; s < fftlen; ++s)
                outspace[s] += revspace[s] * fftlen_if;
        }
    }

    for (s = 0, fftlen = BLOCK_SIZE; s < fftlen; s++) {
        float output;
        float *outspace = u->outspace[0];

        output = outspace[s];
        if (output < -1.0) output = -1.0;
        if (output > 1.0) output = 1.0;
        dst[s * 2 + 0] = output;

        outspace = u->outspace[1];

        output = outspace[s];
        if (output < -1.0) output = -1.0;
        if (output > 1.0) output = 1.0;
        dst[s * 2 + 1] = output;
    }
}


/* Vector size of 4 floats */
#define v_size 4
static void * alloc(size_t x, size_t s) {
    size_t f;
    float *t;

    f = PA_ROUND_UP(x*s, sizeof(float)*v_size);
    pa_assert_se(t = fftwf_malloc(f));
    pa_memzero(t, f);

    return t;
}

/* Mirror channels for symmetrical impulse */
static pa_channel_position_t mirror_channel(pa_channel_position_t channel) {
    switch (channel) {
        case PA_CHANNEL_POSITION_FRONT_LEFT:
            return PA_CHANNEL_POSITION_FRONT_RIGHT;

        case PA_CHANNEL_POSITION_FRONT_RIGHT:
            return PA_CHANNEL_POSITION_FRONT_LEFT;

        case PA_CHANNEL_POSITION_REAR_LEFT:
            return PA_CHANNEL_POSITION_REAR_RIGHT;

        case PA_CHANNEL_POSITION_REAR_RIGHT:
            return PA_CHANNEL_POSITION_REAR_LEFT;

        case PA_CHANNEL_POSITION_SIDE_LEFT:
            return PA_CHANNEL_POSITION_SIDE_RIGHT;

        case PA_CHANNEL_POSITION_SIDE_RIGHT:
            return PA_CHANNEL_POSITION_SIDE_LEFT;

        case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:
            return PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;

        case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER:
            return PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;

        case PA_CHANNEL_POSITION_TOP_FRONT_LEFT:
            return PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;

        case PA_CHANNEL_POSITION_TOP_FRONT_RIGHT:
            return PA_CHANNEL_POSITION_TOP_FRONT_LEFT;

        case PA_CHANNEL_POSITION_TOP_REAR_LEFT:
            return PA_CHANNEL_POSITION_TOP_REAR_RIGHT;

        case PA_CHANNEL_POSITION_TOP_REAR_RIGHT:
            return PA_CHANNEL_POSITION_TOP_REAR_LEFT;

        default:
            return channel;
    }
}

/* Normalize the hrir */
static void normalize_hrir(float * hrir_data, unsigned hrir_samples, unsigned hrir_channels) {
    /* normalize hrir to avoid audible clipping
     *
     * The following heuristic tries to avoid audible clipping. It cannot avoid
     * clipping in the worst case though, because the scaling factor would
     * become too large resulting in a too quiet signal.
     * The idea of the heuristic is to avoid clipping when a single click is
     * played back on all channels. The scaling factor describes the additional
     * factor that is necessary to avoid clipping for "normal" signals.
     *
     * This algorithm doesn't pretend to be perfect, it's just something that
     * appears to work (not too quiet, no audible clipping) on the material that
     * it has been tested on. If you find a real-world example where this
     * algorithm results in audible clipping, please write a patch that adjusts
     * the scaling factor constants or improves the algorithm (or if you can't
     * write a patch, at least report the problem to the PulseAudio mailing list
     * or bug tracker). */

    const float scaling_factor = 2.5;

    float hrir_sum, hrir_max;
    unsigned i, j;

    hrir_max = 0;
    for (i = 0; i < hrir_samples; i++) {
        hrir_sum = 0;
        for (j = 0; j < hrir_channels; j++)
            hrir_sum += fabs(hrir_data[i * hrir_channels + j]);

        if (hrir_sum > hrir_max)
            hrir_max = hrir_sum;
    }

    for (i = 0; i < hrir_samples; i++) {
        for (j = 0; j < hrir_channels; j++)
            hrir_data[i * hrir_channels + j] /= hrir_max * scaling_factor;
    }
}

/* Normalize a stereo hrir */
static void normalize_hrir_stereo(float * hrir_data, float * hrir_right_data, unsigned hrir_samples, unsigned hrir_channels) {
    const float scaling_factor = 2.5;

    float hrir_sum, hrir_max;
    unsigned i, j;

    hrir_max = 0;
    for (i = 0; i < hrir_samples; i++) {
        hrir_sum = 0;
        for (j = 0; j < hrir_channels; j++) {
            hrir_sum += fabs(hrir_data[i * hrir_channels + j]);
            hrir_sum += fabs(hrir_right_data[i * hrir_channels + j]);
        }

        if (hrir_sum > hrir_max)
            hrir_max = hrir_sum;
    }

    for (i = 0; i < hrir_samples; i++) {
        for (j = 0; j < hrir_channels; j++) {
            hrir_data[i * hrir_channels + j] /= hrir_max * scaling_factor;
            hrir_right_data[i * hrir_channels + j] /= hrir_max * scaling_factor;
        }
    }
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss_input, ss_output;
    pa_channel_map map_output;
    pa_modargs *ma;
    const char *master_name;
    const char *hrir_left_file;
    const char *hrir_right_file;
    pa_sink *master=NULL;
    bool use_volume_sharing = true;
    unsigned i, j, ear, found_channel_left, found_channel_right;

    pa_sample_spec ss;
    pa_channel_map map;

    float *hrir_data=NULL, *hrir_right_data=NULL;
    float *hrir_temp_data;
    size_t hrir_samples;
    size_t hrir_copied_length, hrir_total_length;
    unsigned hrir_channels;
    int fftlen;

    float *impulse_temp=NULL;

    unsigned *mapping_left=NULL;
    unsigned *mapping_right=NULL;

    fftwf_plan p;

    pa_channel_map hrir_map, hrir_right_map;

    pa_sample_spec hrir_left_temp_ss;
    pa_memchunk hrir_left_temp_chunk, hrir_left_temp_chunk_resampled;
    pa_resampler *resampler;


    pa_sample_spec hrir_right_temp_ss;
    pa_memchunk hrir_right_temp_chunk, hrir_right_temp_chunk_resampled;

    pa_assert(m);

    hrir_left_temp_chunk.memblock = NULL;
    hrir_left_temp_chunk_resampled.memblock = NULL;
    hrir_right_temp_chunk.memblock = NULL;
    hrir_right_temp_chunk_resampled.memblock = NULL;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    master_name = pa_modargs_get_value(ma, "sink_master", NULL);
    if (!master_name) {
        master_name = pa_modargs_get_value(ma, "master", NULL);
        if (master_name)
            pa_log_warn("The 'master' module argument is deprecated and may be removed in the future, "
                        "please use the 'sink_master' argument instead.");
    }

    if (!(master = pa_namereg_get(m->core, master_name, PA_NAMEREG_SINK))) {
        pa_log("Master sink not found");
        goto fail;
    }

    hrir_left_file = pa_modargs_get_value(ma, "hrir_left", NULL);
    if (!hrir_left_file) {
        hrir_left_file = pa_modargs_get_value(ma, "hrir", NULL);
        if (!hrir_left_file) {
            pa_log("Either the 'hrir' or 'hrir_left' module arguments are required.");
            goto fail;
        }
    }

    hrir_right_file = pa_modargs_get_value(ma, "hrir_right", NULL);

    pa_assert(master);

    if (pa_sound_file_load(master->core->mempool, hrir_left_file, &hrir_left_temp_ss, &hrir_map, &hrir_left_temp_chunk, NULL) < 0) {
        pa_log("Cannot load hrir file.");
        goto fail;
    }

    if (hrir_right_file) {
        if (pa_sound_file_load(master->core->mempool, hrir_right_file, &hrir_right_temp_ss, &hrir_right_map, &hrir_right_temp_chunk, NULL) < 0) {
            pa_log("Cannot load hrir_right file.");
            goto fail;
        }
        if (!pa_sample_spec_equal(&hrir_left_temp_ss, &hrir_right_temp_ss)) {
            pa_log("Both hrir_left and hrir_right must have the same sample format");
            goto fail;
        }
        if (!pa_channel_map_equal(&hrir_map, &hrir_right_map)) {
            pa_log("Both hrir_left and hrir_right must have the same channel layout");
            goto fail;
        }
    }

    ss_input.format = PA_SAMPLE_FLOAT32NE;
    ss_input.rate = master->sample_spec.rate;
    ss_input.channels = hrir_left_temp_ss.channels;

    ss = ss_input;
    map = hrir_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    ss.format = PA_SAMPLE_FLOAT32NE;
    ss_input.rate = ss.rate;
    ss_input.channels = ss.channels;

    ss_output = ss_input;
    ss_output.channels = 2;

    if (pa_modargs_get_value_boolean(ma, "use_volume_sharing", &use_volume_sharing) < 0) {
        pa_log("use_volume_sharing= expects a boolean argument");
        goto fail;
    }

    pa_channel_map_init_stereo(&map_output);

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;

    /* Create virtual sink */
    if (!(u->vsink = pa_virtual_sink_create(master, "vsurroundsink", "Virtual Surround Sink", &ss_input, &map,
                                 &ss_output, &map_output, m, u, ma, use_volume_sharing, true, 0)))
        goto fail;

    u->vsink->process_chunk = filter_process_chunk;

    resampler = pa_resampler_new(u->vsink->sink->core->mempool, &hrir_left_temp_ss, &hrir_map, &ss_input, &hrir_map, u->vsink->sink->core->lfe_crossover_freq,
                                 PA_RESAMPLER_SRC_SINC_BEST_QUALITY, PA_RESAMPLER_NO_REMAP);

    hrir_samples = hrir_left_temp_chunk.length / pa_frame_size(&hrir_left_temp_ss) * ss_input.rate / hrir_left_temp_ss.rate;

    hrir_total_length = hrir_samples * pa_frame_size(&ss_input);
    hrir_channels = ss_input.channels;

    hrir_data = (float *) pa_xmalloc(hrir_total_length);
    hrir_copied_length = 0;

    u->hrir_samples = hrir_samples;
    u->inputs = hrir_channels;

    /* add silence to the hrir until we get enough samples out of the resampler */
    while (hrir_copied_length < hrir_total_length) {
        pa_resampler_run(resampler, &hrir_left_temp_chunk, &hrir_left_temp_chunk_resampled);
        if (hrir_left_temp_chunk.memblock != hrir_left_temp_chunk_resampled.memblock) {
            /* Silence input block */
            pa_silence_memblock(hrir_left_temp_chunk.memblock, &hrir_left_temp_ss);
        }

        if (hrir_left_temp_chunk_resampled.memblock) {
            /* Copy hrir data */
            hrir_temp_data = (float *) pa_memblock_acquire(hrir_left_temp_chunk_resampled.memblock);

            if (hrir_total_length - hrir_copied_length >= hrir_left_temp_chunk_resampled.length) {
                memcpy(hrir_data + hrir_copied_length, hrir_temp_data, hrir_left_temp_chunk_resampled.length);
                hrir_copied_length += hrir_left_temp_chunk_resampled.length;
            } else {
                memcpy(hrir_data + hrir_copied_length, hrir_temp_data, hrir_total_length - hrir_copied_length);
                hrir_copied_length = hrir_total_length;
            }

            pa_memblock_release(hrir_left_temp_chunk_resampled.memblock);
            pa_memblock_unref(hrir_left_temp_chunk_resampled.memblock);
            hrir_left_temp_chunk_resampled.memblock = NULL;
        }
    }

    pa_memblock_unref(hrir_left_temp_chunk.memblock);
    hrir_left_temp_chunk.memblock = NULL;

    if (hrir_right_file) {
        pa_resampler_reset(resampler);

        hrir_right_data = (float *) pa_xmalloc(hrir_total_length);
        hrir_copied_length = 0;

        while (hrir_copied_length < hrir_total_length) {
            pa_resampler_run(resampler, &hrir_right_temp_chunk, &hrir_right_temp_chunk_resampled);
            if (hrir_right_temp_chunk.memblock != hrir_right_temp_chunk_resampled.memblock) {
                /* Silence input block */
                pa_silence_memblock(hrir_right_temp_chunk.memblock, &hrir_right_temp_ss);
            }

            if (hrir_right_temp_chunk_resampled.memblock) {
                /* Copy hrir data */
                hrir_temp_data = (float *) pa_memblock_acquire(hrir_right_temp_chunk_resampled.memblock);

                if (hrir_total_length - hrir_copied_length >= hrir_right_temp_chunk_resampled.length) {
                    memcpy(hrir_right_data + hrir_copied_length, hrir_temp_data, hrir_right_temp_chunk_resampled.length);
                    hrir_copied_length += hrir_right_temp_chunk_resampled.length;
                } else {
                    memcpy(hrir_right_data + hrir_copied_length, hrir_temp_data, hrir_total_length - hrir_copied_length);
                    hrir_copied_length = hrir_total_length;
                }

                pa_memblock_release(hrir_right_temp_chunk_resampled.memblock);
                pa_memblock_unref(hrir_right_temp_chunk_resampled.memblock);
                hrir_right_temp_chunk_resampled.memblock = NULL;
            }
        }

        pa_memblock_unref(hrir_right_temp_chunk.memblock);
        hrir_right_temp_chunk.memblock = NULL;
    }

    pa_resampler_free(resampler);

    if (hrir_right_data)
        normalize_hrir_stereo(hrir_data, hrir_right_data, hrir_samples, hrir_channels);
    else
        normalize_hrir(hrir_data, hrir_samples, hrir_channels);

    /* create mapping between hrir and input */
    mapping_left = (unsigned *) pa_xnew0(unsigned, hrir_channels);
    mapping_right = (unsigned *) pa_xnew0(unsigned, hrir_channels);
    for (i = 0; i < map.channels; i++) {
        found_channel_left = 0;
        found_channel_right = 0;

        for (j = 0; j < hrir_map.channels; j++) {
            if (hrir_map.map[j] == map.map[i]) {
                mapping_left[i] = j;
                found_channel_left = 1;
            }

            if (hrir_map.map[j] == mirror_channel(map.map[i])) {
                mapping_right[i] = j;
                found_channel_right = 1;
            }
        }

        if (!found_channel_left) {
            pa_log("Cannot find mapping for channel %s", pa_channel_position_to_string(map.map[i]));
            goto fail;
        }
        if (!found_channel_right) {
            pa_log("Cannot find mapping for channel %s", pa_channel_position_to_string(mirror_channel(map.map[i])));
            goto fail;
        }
    }

    fftlen = (hrir_samples + BLOCK_SIZE + 1); /* Grow a bit for overlap */
    {
        /* Round up to a power of two */
        int pow = 1;
        while (fftlen > 2) { pow++; fftlen /= 2; }
        fftlen = 2 << pow;
    }

    u->fftlen = fftlen;

    u->f_in = (fftwf_complex*) alloc(sizeof(fftwf_complex), (fftlen/2+1));
    u->f_out = (fftwf_complex*) alloc(sizeof(fftwf_complex), (fftlen/2+1));

    u->f_ir = (fftwf_complex**) alloc(sizeof(fftwf_complex*), (hrir_channels*2));
    for (i = 0, j = hrir_channels*2; i < j; i++)
        u->f_ir[i] = (fftwf_complex*) alloc(sizeof(fftwf_complex), (fftlen/2+1));

    u->revspace = (float*) alloc(sizeof(float), fftlen);

    u->outspace[0] = (float*) alloc(sizeof(float), BLOCK_SIZE);
    u->outspace[1] = (float*) alloc(sizeof(float), BLOCK_SIZE);

    u->inspace = (float**) alloc(sizeof(float*), hrir_channels);
    for (i = 0; i < hrir_channels; i++)
        u->inspace[i] = (float*) alloc(sizeof(float), fftlen);

    u->p_fw = (fftwf_plan*) alloc(sizeof(fftwf_plan), hrir_channels);
    for (i = 0; i < hrir_channels; i++)
        pa_assert_se(u->p_fw[i] = fftwf_plan_dft_r2c_1d(fftlen, u->inspace[i], u->f_in, FFTW_ESTIMATE));

    pa_assert_se(u->p_bw = fftwf_plan_dft_c2r_1d(fftlen, u->f_out, u->revspace, FFTW_ESTIMATE));

    impulse_temp = (float*) alloc(sizeof(float), fftlen);

    if (hrir_right_data) {
        for (i = 0; i < hrir_channels; i++) {
            for (ear = 0; ear < 2; ear++) {
                size_t index = i * 2 + ear;
                size_t impulse_index = mapping_left[i];
                float *impulse = (ear == 0) ? hrir_data : hrir_right_data;
                for (j = 0; j < hrir_samples; j++) {
                    impulse_temp[j] = impulse[j * hrir_channels + impulse_index];
                }

                p = fftwf_plan_dft_r2c_1d(fftlen, impulse_temp, u->f_ir[index], FFTW_ESTIMATE);
                if (p) {
                    fftwf_execute(p);
                    fftwf_destroy_plan(p);
                } else {
                    pa_log("fftw plan creation failed for %s ear speaker index %d", (ear == 0) ? "left" : "right", i);
                    goto fail;
                }
            }
        }
    } else {
        for (i = 0; i < hrir_channels; i++) {
            for (ear = 0; ear < 2; ear++) {
                size_t index = i * 2 + ear;
                size_t impulse_index = (ear == 0) ? mapping_left[i] : mapping_right[i];
                for (j = 0; j < hrir_samples; j++) {
                    impulse_temp[j] = hrir_data[j * hrir_channels + impulse_index];
                }

                p = fftwf_plan_dft_r2c_1d(fftlen, impulse_temp, u->f_ir[index], FFTW_ESTIMATE);
                if (p) {
                    fftwf_execute(p);
                    fftwf_destroy_plan(p);
                } else {
                    pa_log("fftw plan creation failed for %s ear speaker index %d", (ear == 0) ? "left" : "right", i);
                    goto fail;
                }
            }
        }
    }

    pa_xfree(impulse_temp);

    pa_xfree(hrir_data);
    if (hrir_right_data)
        pa_xfree(hrir_right_data);

    pa_xfree(mapping_left);
    pa_xfree(mapping_right);

    u->vsink->fixed_block_size = BLOCK_SIZE;
    u->vsink->overlap_frames = u->fftlen - BLOCK_SIZE;

    if (pa_virtual_sink_activate(u->vsink) < 0)
        goto fail;

    pa_modargs_free(ma);

    return 0;

fail:
    if (impulse_temp)
        pa_xfree(impulse_temp);

    if (mapping_left)
        pa_xfree(mapping_left);

    if (mapping_right)
        pa_xfree(mapping_right);

    if (hrir_data)
        pa_xfree(hrir_data);

    if (hrir_right_data)
        pa_xfree(hrir_right_data);

    if (hrir_left_temp_chunk.memblock)
        pa_memblock_unref(hrir_left_temp_chunk.memblock);

    if (hrir_left_temp_chunk_resampled.memblock)
        pa_memblock_unref(hrir_left_temp_chunk_resampled.memblock);

    if (hrir_right_temp_chunk.memblock)
        pa_memblock_unref(hrir_right_temp_chunk.memblock);

    if (hrir_right_temp_chunk_resampled.memblock)
        pa_memblock_unref(hrir_right_temp_chunk_resampled.memblock);

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->vsink->sink);
}

void pa__done(pa_module*m) {
    size_t i, j;
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->vsink)
        pa_virtual_sink_destroy(u->vsink);

    if (u->p_fw) {
        for (i = 0, j = u->inputs; i < j; i++) {
            if (u->p_fw[i])
                fftwf_destroy_plan(u->p_fw[i]);
        }
        fftwf_free(u->p_fw);
    }

    if (u->p_bw)
        fftwf_destroy_plan(u->p_bw);

    if (u->f_ir) {
        for (i = 0, j = u->inputs * 2; i < j; i++) {
            if (u->f_ir[i])
                fftwf_free(u->f_ir[i]);
        }
        fftwf_free(u->f_ir);
    }

    if (u->f_out)
        fftwf_free(u->f_out);

    if (u->f_in)
        fftwf_free(u->f_in);

    if (u->revspace)
        fftwf_free(u->revspace);

    if (u->outspace[0])
        fftwf_free(u->outspace[0]);
    if (u->outspace[1])
        fftwf_free(u->outspace[1]);

    if (u->inspace) {
        for (i = 0, j = u->inputs; i < j; i++) {
            if (u->inspace[i])
                fftwf_free(u->inspace[i]);
        }
        fftwf_free(u->inspace);
    }

    pa_xfree(u);
}
