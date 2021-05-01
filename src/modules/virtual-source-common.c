/***
    This file is part of PulseAudio.

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

#include <modules/virtual-source-common.h>

#include <pulsecore/core-util.h>
#include <pulsecore/mix.h>

#include <pulse/timeval.h>
#include <pulse/rtclock.h>

PA_DEFINE_PRIVATE_CLASS(pa_vsource, pa_msgobject);
#define PA_VSOURCE(o) (pa_vsource_cast(o))

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)

#define MIN_BLOCK_SIZE 16
#define LATENCY_MARGIN (5 * PA_USEC_PER_MSEC)

enum {
    SOURCE_MESSAGE_UPDATE_PARAMETERS = PA_SOURCE_MESSAGE_MAX
};

enum {
    VSOURCE_MESSAGE_FREE_PARAMETERS,
    VSOURCE_MESSAGE_OUTPUT_ATTACHED
};

struct uplink_data {
    pa_vsource *vsource;
    pa_memblockq *memblockq;
};

/* Helper functions */

static inline pa_source_output* get_output_from_source(pa_source *s) {

    if (!s->vsource || !s->vsource->output_from_master)
        return NULL;
    return  s->vsource->output_from_master;
}

static int check_block_sizes(size_t fixed_block_frames, size_t fixed_input_block_frames, size_t overlap_frames, pa_vsource *vs) {
    size_t max_block_frames;
    size_t max_frame_size;

    max_frame_size = PA_MAX(pa_frame_size(&vs->source->sample_spec), pa_frame_size(&vs->output_from_master->sample_spec));

    max_block_frames = pa_mempool_block_size_max(vs->core->mempool);
    max_block_frames = max_block_frames / max_frame_size;

    if (fixed_block_frames > max_block_frames || fixed_input_block_frames > max_block_frames || overlap_frames + MIN_BLOCK_SIZE > max_block_frames) {
        pa_log_warn("At least one of fixed_block_size, fixed_input_block_size or overlap_frames exceeds maximum.");
        return -1;
    }

    if (fixed_block_frames > 0 && fixed_block_frames < MIN_BLOCK_SIZE) {
        pa_log_warn("fixed_block_size too small.");
        return -1;
    }

    if (fixed_input_block_frames > 0 && fixed_input_block_frames < MIN_BLOCK_SIZE) {
        pa_log_warn("fixed_input_block_size too small.");
        return -1;
    }

    if (fixed_block_frames + overlap_frames > max_block_frames) {
        pa_log_warn("Sum of fixed_block_size and overlap_frames exceeds maximum.");
        return -1;
    }

    if (fixed_input_block_frames > max_block_frames) {
        pa_log_warn("fixed_input_block_size exceeds maximum.");
        return -1;
    }

    if (fixed_input_block_frames != 0 && fixed_block_frames > fixed_input_block_frames) {
        pa_log_warn("fixed_block_size larger than fixed_input_block_size.");
        return -1;
    }

    return 0;
}

static void set_latency_range_within_thread(pa_vsource *vsource) {
    pa_usec_t min_latency, max_latency;
    pa_source_output *o;
    pa_source *s;

    s = vsource->source;
    pa_assert(s);
    o = vsource->output_from_master;
    pa_assert(o);

    min_latency = o->source->thread_info.min_latency;
    max_latency = o->source->thread_info.max_latency;

    if (s->flags & PA_SOURCE_DYNAMIC_LATENCY) {
        if (vsource->max_latency)
            max_latency = PA_MIN(vsource->max_latency, max_latency);

        if (vsource->fixed_block_size) {
            pa_usec_t latency;

            latency = pa_bytes_to_usec(vsource->fixed_block_size * pa_frame_size(&s->sample_spec), &s->sample_spec);
            min_latency = PA_MAX(min_latency, latency);
        }

        max_latency = PA_MAX(max_latency, min_latency);
    }

    pa_source_set_latency_range_within_thread(s, min_latency, max_latency);
    if (vsource->uplink_sink)
        pa_sink_set_latency_range_within_thread(vsource->uplink_sink, min_latency, max_latency);
}

/* Called from I/O thread context */
static void set_memblockq_rewind(pa_vsource *vsource) {

    if (vsource->memblockq) {
        size_t rewind_size;
        size_t in_fs;

        in_fs = pa_frame_size(&vsource->output_from_master->sample_spec);
        rewind_size = PA_MAX(vsource->fixed_input_block_size, vsource->overlap_frames) * in_fs;
        pa_memblockq_set_maxrewind(vsource->memblockq, rewind_size);
    }
}

/* Uplink sink callbacks */

/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    pa_sink *s;
    struct uplink_data *uplink;

    s = PA_SINK(o);
    uplink = s->userdata;
    pa_assert(uplink);

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY:

            /* While the sink is not opened or if we have not received any data yet,
             * simply return 0 as latency */
            if (!PA_SINK_IS_OPENED(s->thread_info.state)) {
                *((int64_t*) data) = 0;
                return 0;
            }

            *((int64_t*) data) = pa_bytes_to_usec(pa_memblockq_get_length(uplink->memblockq), &s->sample_spec);
            *((int64_t*) data) -= pa_source_get_latency_within_thread(uplink->vsource->source, true);

            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_in_main_thread(pa_sink *s, pa_sink_state_t state, pa_suspend_cause_t suspend_cause) {
    pa_vsource *vsource;
    struct uplink_data *uplink;

    pa_sink_assert_ref(s);
    uplink = s->userdata;
    pa_assert(uplink);
    vsource = uplink->vsource;
    pa_assert(vsource);

    if (!PA_SINK_IS_LINKED(state)) {
        return 0;
    }

    /* need to wake-up source if it was suspended */
    if (!PA_SINK_IS_OPENED(s->state) && PA_SINK_IS_OPENED(state) && !PA_SOURCE_IS_OPENED(vsource->source->state) && PA_SOURCE_IS_LINKED(vsource->source->state)) {
        pa_log_debug("Resuming source %s, because its uplink sink became active.", vsource->source->name);
        pa_source_suspend(vsource->source, false, PA_SUSPEND_IDLE);
    }

    return 0;
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct uplink_data *uplink;

    pa_sink_assert_ref(s);
    uplink = s->userdata;
    pa_assert(uplink);

    if (!PA_SINK_IS_OPENED(new_state) && PA_SINK_IS_OPENED(s->thread_info.state)) {
        pa_memblockq_flush_write(uplink->memblockq, true);
        pa_sink_set_max_request_within_thread(s, 0);
        pa_sink_set_max_rewind_within_thread(s, 0);
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    struct uplink_data *uplink;
    pa_usec_t latency;
    size_t rewind_size;

    pa_sink_assert_ref(s);
    uplink = s->userdata;
    pa_assert(uplink);

    if (!PA_SINK_IS_LINKED(s->thread_info.state))
        return;

    latency = pa_sink_get_requested_latency_within_thread(s);
    if (latency == (pa_usec_t) -1)
        latency = s->thread_info.max_latency;
    rewind_size = pa_usec_to_bytes(latency, &s->sample_spec);
    pa_memblockq_set_maxrewind(uplink->memblockq, rewind_size);

    pa_sink_set_max_request_within_thread(s, rewind_size);
    pa_sink_set_max_rewind_within_thread(s, rewind_size);
}

static void sink_process_rewind(pa_sink *s) {
    struct uplink_data *uplink;
    size_t rewind_nbytes, in_buffer;

    uplink = s->userdata;
    pa_assert(uplink);

    rewind_nbytes = s->thread_info.rewind_nbytes;

    if (!PA_SINK_IS_OPENED(s->thread_info.state) || rewind_nbytes <= 0)
        goto finish;

    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    in_buffer = pa_memblockq_get_length(uplink->memblockq);
    if (in_buffer == 0) {
        pa_log_debug("Memblockq empty, cannot rewind");
        goto finish;
    }

    if (rewind_nbytes > in_buffer)
        rewind_nbytes = in_buffer;

    pa_memblockq_seek(uplink->memblockq, -rewind_nbytes, PA_SEEK_RELATIVE, true);
    pa_sink_process_rewind(s, rewind_nbytes);

    pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
    return;

finish:
    pa_sink_process_rewind(s, 0);
}

/* Source callbacks */

/* Called from I/O thread context */
int pa_virtual_source_process_msg(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    pa_source_output *o;
    pa_vsource *vsource;

    pa_source *s = PA_SOURCE(obj);
    vsource = s->vsource;
    pa_assert(vsource);
    o = vsource->output_from_master;
    pa_assert(o);

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY:

            /* The source is _put() before the source output is, so let's
             * make sure we don't access it in that time. Also, the
             * source output is first shut down, the source second. */
            if (!PA_SOURCE_IS_LINKED(s->thread_info.state) ||
                !PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =

                /* Get the latency of the master source */
                pa_source_get_latency_within_thread(o->source, true) +

                /* Add the latency internal to our source output on top */
                pa_bytes_to_usec(pa_memblockq_get_length(o->thread_info.delay_memblockq), &o->source->sample_spec);

            /* Add latenccy caused by the local memblockq */
            if (vsource->memblockq)
                *((int64_t*) data) += pa_bytes_to_usec(pa_memblockq_get_length(vsource->memblockq), &o->sample_spec);

            /* Add resampler delay */
            *((int64_t*) data) += pa_resampler_get_delay_usec(o->thread_info.resampler);


            /* Add additional filter latency if required. */
            if (vsource->get_extra_latency)
                *((int64_t*) data) += vsource->get_extra_latency(s);

            return 0;

        case SOURCE_MESSAGE_UPDATE_PARAMETERS:

        /* Let the module update the filter parameters. Because the main thread
         * is waiting, variables can be accessed freely in the callback. */
        if (vsource->update_filter_parameters) {
            void *parameters;
            size_t old_block_size, old_input_block_size, old_overlap_frames;

            /* Save old block sizes */
            old_block_size = vsource->fixed_block_size;
            old_input_block_size = vsource->fixed_input_block_size;
            old_overlap_frames = vsource->overlap_frames;

            parameters = vsource->update_filter_parameters(data, s->userdata);
            if (parameters)
                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(vsource), VSOURCE_MESSAGE_FREE_PARAMETERS, parameters, 0, NULL, NULL);

            /* Updating the parameters may have changed the block sizes, so check them again. */
            if (check_block_sizes(vsource->fixed_block_size, vsource->fixed_input_block_size, vsource->overlap_frames, vsource) < 0) {
                pa_log_warn("Invalid new block sizes, keeping old values.");
                vsource->fixed_block_size = old_block_size;
                vsource->fixed_input_block_size = old_input_block_size;
                vsource->overlap_frames = old_overlap_frames;
            }

            /* Set rewind of memblockq */
            set_memblockq_rewind(vsource);

            /* Inform the filter of the block sizes in use */
            if (vsource->update_block_sizes)
                vsource->update_block_sizes(vsource->fixed_block_size, vsource->fixed_input_block_size, vsource->overlap_frames, s->userdata);

            /* If the block sizes changed the latency range may have changed as well. */
            set_latency_range_within_thread(vsource);
        }

        return 0;

    }

    return pa_source_process_msg(obj, code, data, offset, chunk);
}

/* Called from main context */
int pa_virtual_source_set_state_in_main_thread(pa_source *s, pa_source_state_t state, pa_suspend_cause_t suspend_cause) {
    pa_source_output *o;
    pa_vsource *vsource;
    bool suspend_cause_changed;

    pa_source_assert_ref(s);
    o = get_output_from_source(s);
    pa_assert(o);
    vsource = s->vsource;
    pa_assert(vsource);

    if (!PA_SOURCE_IS_LINKED(state) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(o->state))
        return 0;

    suspend_cause_changed = (suspend_cause != s->suspend_cause);
    if (vsource->uplink_sink && PA_SINK_IS_LINKED(vsource->uplink_sink->state) && suspend_cause_changed) {
        /* If the source is suspended for other reasons than being idle, the uplink sink
         * should be suspended using the same reasons */
        if (suspend_cause != PA_SUSPEND_IDLE && state == PA_SOURCE_SUSPENDED) {
            suspend_cause = suspend_cause & ~PA_SUSPEND_IDLE;
            pa_sink_suspend(vsource->uplink_sink, true, suspend_cause);
        } else if (PA_SOURCE_IS_OPENED(state) && s->suspend_cause != PA_SUSPEND_IDLE) {
        /* If the source is resuming, the old suspend cause of the source should
         * be removed from the sink unless the old suspend cause was idle. */
            suspend_cause = s->suspend_cause & ~PA_SUSPEND_IDLE;
            pa_sink_suspend(vsource->uplink_sink, false, suspend_cause);
        }
    }

    pa_source_output_cork(o, state == PA_SOURCE_SUSPENDED);
    return 0;
}

/* Called from the IO thread. */
int pa_virtual_source_set_state_in_io_thread(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    pa_vsource *vsource;

    pa_source_assert_ref(s);
    vsource = s->vsource;
    pa_assert(vsource);

    if (PA_SOURCE_IS_OPENED(new_state) && !PA_SOURCE_IS_OPENED(s->thread_info.state))
        set_latency_range_within_thread(vsource);

    return 0;
}

/* Called from I/O thread context */
void pa_virtual_source_update_requested_latency(pa_source *s) {
    pa_vsource *vsource;
    pa_source_output *o;
    pa_usec_t latency;

    pa_source_assert_ref(s);
    vsource = s->vsource;
    pa_assert(vsource);
    o = vsource->output_from_master;
    pa_assert(o);

    if (!PA_SOURCE_IS_LINKED(s->thread_info.state) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state))
        return;

    latency = pa_source_get_requested_latency_within_thread(s);
    if (vsource->max_latency)
        latency = PA_MIN(vsource->max_latency, latency);

    /* If we are using fixed blocksize, part of the latency is implemented
     * in the virtual source. Reduce master latency by this amount. Do not set
     * the latency too small to avoid high CPU load and underruns. */
    if (vsource->fixed_block_size) {
        size_t in_fs;
        pa_usec_t fixed_block_latency, min_latency;

        in_fs = pa_frame_size(&o->sample_spec);
        fixed_block_latency = pa_bytes_to_usec(vsource->fixed_block_size * in_fs, &o->sample_spec);
        min_latency = o->source->thread_info.min_latency;
        if (min_latency < LATENCY_MARGIN)
            min_latency += LATENCY_MARGIN;

        if (latency < fixed_block_latency + min_latency)
            latency = min_latency;
        else
            latency = latency - fixed_block_latency;
    }

    /* Now hand this one over to the master source */
    pa_source_output_set_requested_latency_within_thread(o, latency);
}

/* Called from main context */
void pa_virtual_source_set_volume(pa_source *s) {
    pa_source_output *o;
    pa_cvolume vol;

    pa_source_assert_ref(s);
    o = get_output_from_source(s);
    pa_assert(o);

    if (!PA_SOURCE_IS_LINKED(s->state) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(o->state))
        return;

    /* Remap the volume, source and source output may have different
     * channel counts. */
    vol = s->real_volume;
    pa_cvolume_remap(&vol, &s->channel_map, &o->channel_map);
    pa_source_output_set_volume(o, &vol, s->save_volume, true);
}

/* Called from main context */
void pa_virtual_source_set_mute(pa_source *s) {
    pa_source_output *o;

    pa_source_assert_ref(s);
    o = get_output_from_source(s);
    pa_assert(o);

    if (!PA_SOURCE_IS_LINKED(s->state) ||
        !PA_SOURCE_OUTPUT_IS_LINKED(o->state))
        return;

    pa_source_output_set_mute(o, s->muted, s->save_muted);
}

/* Post data, mix in uplink sink */
void pa_virtual_source_post(pa_source *s, const pa_memchunk *chunk) {
    pa_vsource *vsource;

    vsource = s->vsource;
    pa_assert(vsource);

    /* if uplink sink exists, pull data from there; simplify by using
       same length as chunk provided by source */
    if (vsource->uplink_sink && PA_SINK_IS_OPENED(vsource->uplink_sink->thread_info.state)) {
        pa_memchunk tchunk;
        pa_mix_info streams[2];
        int ch;
        uint8_t *dst;
        pa_memchunk dst_chunk;
        size_t nbytes;
        struct uplink_data *uplink;

        uplink = vsource->uplink_sink->userdata;
        pa_assert(uplink);

        /* Hmm, process any rewind request that might be queued up */
        if (PA_UNLIKELY(vsource->uplink_sink->thread_info.rewind_requested))
            sink_process_rewind(vsource->uplink_sink);

        nbytes = chunk->length;

        /* get data from the sink */
        while (pa_memblockq_get_length(uplink->memblockq) < nbytes) {
            pa_memchunk nchunk;
            size_t missing;

            missing = nbytes - pa_memblockq_get_length(uplink->memblockq);
            pa_sink_render(vsource->uplink_sink, missing, &nchunk);
            pa_memblockq_push(uplink->memblockq, &nchunk);
            pa_memblock_unref(nchunk.memblock);
        }
        pa_memblockq_peek_fixed_size(uplink->memblockq, nbytes, &tchunk);
        pa_assert(tchunk.length == nbytes);

        /* move the read pointer for sink memblockq */
        pa_memblockq_drop(uplink->memblockq, tchunk.length);

        /* Prepare output chunk */
        dst_chunk.index = 0;
        dst_chunk.length = nbytes;
        dst_chunk.memblock = pa_memblock_new(vsource->core->mempool, dst_chunk.length);
        dst = pa_memblock_acquire_chunk(&dst_chunk);

        /* set-up mixing structure
           volume was taken care of in sink and source already */
        streams[0].chunk = *chunk;
        for(ch=0; ch < s->sample_spec.channels; ch++)
            streams[0].volume.values[ch] = PA_VOLUME_NORM;
        streams[0].volume.channels = s->sample_spec.channels;

        streams[1].chunk = tchunk;
        for(ch=0; ch < s->sample_spec.channels;ch++)
            streams[1].volume.values[ch] = PA_VOLUME_NORM;
        streams[1].volume.channels = s->sample_spec.channels;

        /* do mixing */
        pa_mix(streams,                /* 2 streams to be mixed */
               2,
               dst,                    /* put result in dst */
               nbytes,                 /* same length as input */
               (const pa_sample_spec *)&s->sample_spec, /* same sample spec for input and output */
               NULL,                   /* no volume information */
               false);                 /* no mute */

        pa_memblock_release(dst_chunk.memblock);

        pa_source_post(s, &dst_chunk);

        pa_memblock_unref(tchunk.memblock);
        pa_memblock_unref(dst_chunk.memblock);
    } else
        pa_source_post(s, chunk);
}

/* Source output callbacks */

/* Called from output thread context */
void pa_virtual_source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    pa_source *s;
    size_t length, in_fs, out_fs;
    pa_vsource *vsource;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);
    pa_assert(chunk);

    if (!PA_SOURCE_IS_LINKED(s->thread_info.state) || !PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state))
        return;

    if (!vsource->process_chunk || !vsource->memblockq) {
        pa_virtual_source_post(s, chunk);
        return;
    }

    out_fs = pa_frame_size(&s->sample_spec);
    in_fs = pa_frame_size(&o->sample_spec);

    pa_memblockq_push_align(vsource->memblockq, chunk);
    length = pa_memblockq_get_length(vsource->memblockq);

    while (length > vsource->fixed_block_size * in_fs || (vsource->fixed_block_size > 0 && length == vsource->fixed_block_size * in_fs)) {
        uint8_t *src, *dst;
        size_t in_count;
        size_t overlap_frames, max_block_frames;
        unsigned n;
        pa_memchunk tchunk, schunk;

        /* Determine number of output samples */
        n = length / in_fs;
        if (vsource->fixed_input_block_size && n > vsource->fixed_input_block_size)
            n = vsource->fixed_input_block_size;
        if (vsource->fixed_block_size && n > vsource->fixed_block_size)
            n = vsource->fixed_block_size;

        n = PA_MIN(n, vsource->max_chunk_size / in_fs);

        pa_assert(n > 0);

        /* Determine number of overlap frames */
        overlap_frames = vsource->overlap_frames;
        if (vsource->get_current_overlap)
            overlap_frames = PA_MIN(overlap_frames, vsource->get_current_overlap(o));

        /* For fixed input block size ignore overlap frames */
        if (vsource->fixed_input_block_size) {
            overlap_frames = 0;
            if (n > vsource->fixed_input_block_size)
                n = vsource->fixed_input_block_size;
            else
                overlap_frames = vsource->fixed_input_block_size - n;
        }

        /* In case of variable block size, it may be possible, that the sum of
         * new samples and history data exceeds pa_mempool_block_size_max().
         * Then the number of new samples must be limited. */
        max_block_frames = pa_mempool_block_size_max(o->source->core->mempool) / PA_MAX(in_fs, out_fs);
        if (n + overlap_frames > max_block_frames)
            n = max_block_frames - overlap_frames;

        /* Get input data */
        in_count = n + overlap_frames;
        if (overlap_frames)
            pa_memblockq_rewind(vsource->memblockq, overlap_frames * in_fs);
        pa_memblockq_peek_fixed_size(vsource->memblockq, in_count * in_fs, &schunk);
        pa_memblockq_drop(vsource->memblockq, in_count * in_fs);

        /* Prepare output chunk */
        tchunk.index = 0;
        tchunk.length = n * out_fs;
        tchunk.memblock = pa_memblock_new(o->source->core->mempool, tchunk.length);

        src = pa_memblock_acquire_chunk(&schunk);
        dst = pa_memblock_acquire(tchunk.memblock);

        /* Let the filter process the chunk */
        vsource->process_chunk(src, dst, in_count, n, o->userdata);

        pa_memblock_release(tchunk.memblock);
        pa_memblock_release(schunk.memblock);
        pa_memblock_unref(schunk.memblock);

        /* Post data */
        pa_virtual_source_post(s, &tchunk);

        pa_memblock_unref(tchunk.memblock);
        length = pa_memblockq_get_length(vsource->memblockq);
    }
}

 /* Called from I/O thread context */
void pa_virtual_source_output_process_rewind(pa_source_output *o, size_t nbytes) {
    pa_source *s;
    pa_vsource *vsource;
    size_t in_fs, out_fs;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    out_fs = pa_frame_size(&s->sample_spec);
    in_fs = pa_frame_size(&o->sample_spec);

    /* If the source is not yet linked, there is nothing to rewind */
    if (!PA_SOURCE_IS_LINKED(s->thread_info.state))
        return;

    /* If the source output is corked, ignore the rewind request. */
    if (o->thread_info.state == PA_SOURCE_OUTPUT_CORKED)
        return;

    /* If we have a memblockq, the source is not rewindable, else
     * pass the rewind on to the source */
    if (vsource->memblockq)
        pa_memblockq_seek(vsource->memblockq, - nbytes, PA_SEEK_RELATIVE, true);
    else {
        pa_source_process_rewind(s, nbytes * out_fs / in_fs);
        if (vsource->uplink_sink && PA_SINK_IS_OPENED(vsource->uplink_sink->thread_info.state)) {
            struct uplink_data *uplink;

            uplink = vsource->uplink_sink->userdata;
            pa_assert(uplink);
            pa_memblockq_rewind(uplink->memblockq, nbytes * out_fs / in_fs);
        }
    }
}

/* Called from source I/O thread context. */
void pa_virtual_source_output_update_max_rewind(pa_source_output *o, size_t nbytes) {
    pa_source *s;
    pa_vsource *vsource;
    size_t in_fs, out_fs;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    out_fs = pa_frame_size(&s->sample_spec);
    in_fs = pa_frame_size(&o->sample_spec);

    /* Set rewind of memblockq */
    set_memblockq_rewind(vsource);

    if (!vsource->memblockq)
        pa_source_set_max_rewind_within_thread(s, nbytes * out_fs / in_fs);
}

/* Called from I/O thread context */
void pa_virtual_source_output_update_source_latency_range(pa_source_output *o) {
    pa_source *s;
    pa_vsource *vsource;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    set_latency_range_within_thread(vsource);
}

/* Called from I/O thread context */
void pa_virtual_source_output_update_source_fixed_latency(pa_source_output *o) {
    pa_source *s;
    pa_vsource *vsource;
    pa_usec_t latency;
    size_t out_fs;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    out_fs = pa_frame_size(&s->sample_spec);

    /* For filters with fixed block size we have to add the block size minus 1 sample
     * to the fixed latency. */
    latency = o->source->thread_info.fixed_latency;
    if (vsource->fixed_block_size && !(s->flags & PA_SOURCE_DYNAMIC_LATENCY))
        latency += pa_bytes_to_usec((vsource->fixed_block_size - 1) * out_fs, &s->sample_spec);

    pa_source_set_fixed_latency_within_thread(s, latency);
}

/* Called from I/O thread context */
void pa_virtual_source_output_attach(pa_source_output *o) {
    pa_source *s;
    pa_vsource *vsource;
    size_t out_fs, master_fs;
    pa_usec_t latency;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    out_fs = pa_frame_size(&s->sample_spec);
    master_fs = pa_frame_size(&o->source->sample_spec);

    pa_source_set_rtpoll(s, o->source->thread_info.rtpoll);
    if (vsource->uplink_sink)
        pa_sink_set_rtpoll(vsource->uplink_sink, o->source->thread_info.rtpoll);

    set_latency_range_within_thread(vsource);

    /* For filters with fixed block size we have to add the block size minus 1 sample
     * to the fixed latency. */
    latency = o->source->thread_info.fixed_latency;
    if (vsource->fixed_block_size && !(s->flags & PA_SOURCE_DYNAMIC_LATENCY))
        latency += pa_bytes_to_usec((vsource->fixed_block_size - 1) * out_fs, &s->sample_spec);

    pa_source_set_fixed_latency_within_thread(s, latency);

    /* Set max_rewind, virtual sources can only rewind when there is no memblockq */
    if (vsource->memblockq)
        pa_source_set_max_rewind_within_thread(s, 0);
    else
        pa_source_set_max_rewind_within_thread(s, o->source->thread_info.max_rewind * out_fs / master_fs);

    /* Set rewind of memblockq */
    set_memblockq_rewind(vsource);

    /* This call is needed to remove the UNAVAILABLE suspend cause after
     * a move when the previous master source disappeared. */
    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(vsource), VSOURCE_MESSAGE_OUTPUT_ATTACHED, NULL, 0, NULL, NULL);

    if (PA_SOURCE_IS_LINKED(s->thread_info.state))
        pa_source_attach_within_thread(s);
}

/* Called from output thread context */
void pa_virtual_source_output_detach(pa_source_output *o) {
    pa_source *s;
    pa_vsource *vsource;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    if (PA_SOURCE_IS_LINKED(s->thread_info.state))
        pa_source_detach_within_thread(s);

    pa_source_set_rtpoll(s, NULL);
    if (vsource->uplink_sink)
        pa_sink_set_rtpoll(vsource->uplink_sink, NULL);
}

/* Called from main thread */
void pa_virtual_source_output_kill(pa_source_output *o) {
    pa_source *s;
    pa_vsource *vsource;
    pa_module *m;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    /* The order here matters! We first kill the source so that streams
     * can properly be moved away while the source output is still connected
     * to the master. It may be possible that the source output is connected
     * to a virtual source which has lost its master, so do not try to cork
     * if the source has no I/O context. */
    if (o->source && o->source->asyncmsgq)
        pa_source_output_cork(o, true);
    pa_source_unlink(s);
    pa_source_output_unlink(o);

    pa_source_output_unref(o);

    if (vsource->memblockq)
        pa_memblockq_free(vsource->memblockq);

    /* Destroy uplink sink if present */
    if (vsource->uplink_sink) {
        struct uplink_data *uplink;

        uplink = vsource->uplink_sink->userdata;
        pa_sink_unlink(vsource->uplink_sink);
        pa_sink_unref(vsource->uplink_sink);

        if (uplink) {
            if (uplink->memblockq)
                pa_memblockq_free(uplink->memblockq);

            pa_xfree(uplink);
        }
        vsource->uplink_sink = NULL;
    }

    /* Virtual sources must set the module */
    m = s->module;
    pa_assert(m);
    pa_source_unref(s);

    vsource->source = NULL;
    vsource->output_from_master = NULL;
    vsource->memblockq = NULL;

    pa_module_unload_request(m, true);
}

/* Called from main context */
bool pa_virtual_source_output_may_move_to(pa_source_output *o, pa_source *dest) {
    pa_source *s;
    pa_vsource *vsource;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    if (vsource->autoloaded)
        return false;

    if (s == dest)
        return false;

    if (vsource->uplink_sink) {
        pa_source *chain_master;

        chain_master = dest;
        while (chain_master->vsource && chain_master->vsource->output_from_master)
            chain_master = chain_master->vsource->output_from_master->source;

        if (chain_master == vsource->uplink_sink->monitor_source)
            return false;
    }

    return true;
}

/* Called from main thread */
void pa_virtual_source_output_moving(pa_source_output *o, pa_source *dest) {
    pa_source *s;
    pa_vsource *vsource;
    uint32_t idx;
    pa_source_output *output;
    pa_sink_input *input;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    if (dest) {
        pa_source_set_asyncmsgq(s, dest->asyncmsgq);
        pa_source_update_flags(s, PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY, dest->flags);
        pa_proplist_sets(s->proplist, PA_PROP_DEVICE_MASTER_DEVICE, dest->name);
        vsource->source_moving = true;
        if (vsource->uplink_sink) {
            pa_sink_flags_t flags = 0;

            if (dest->flags & PA_SOURCE_LATENCY)
                flags |= PA_SINK_LATENCY;
            if (dest->flags & PA_SOURCE_DYNAMIC_LATENCY)
                flags |= PA_SINK_DYNAMIC_LATENCY;
            pa_sink_set_asyncmsgq(vsource->uplink_sink, dest->asyncmsgq);
            pa_sink_update_flags(vsource->uplink_sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, flags);
            pa_proplist_sets(vsource->uplink_sink->proplist, PA_PROP_DEVICE_MASTER_DEVICE, dest->name);
        }
    } else {
        pa_source_set_asyncmsgq(s, NULL);
        if (vsource->uplink_sink)
            pa_sink_set_asyncmsgq(vsource->uplink_sink, NULL);
    }

    if (dest && vsource->set_description)
        vsource->set_description(o, dest);

    else {
        if (vsource->auto_desc && dest) {
            const char *z;
            pa_proplist *pl;
            char *proplist_name;

            pl = pa_proplist_new();
            proplist_name = pa_sprintf_malloc("device.%s.name", vsource->source_type);
            z = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION);
            pa_proplist_setf(pl, PA_PROP_DEVICE_DESCRIPTION, "%s %s on %s", vsource->desc_head,
                             pa_proplist_gets(s->proplist, proplist_name), z ? z : dest->name);

            pa_source_update_proplist(s, PA_UPDATE_REPLACE, pl);
            pa_proplist_free(pl);
            pa_xfree(proplist_name);
        }

        if (dest)
            pa_proplist_setf(o->proplist, PA_PROP_MEDIA_NAME, "%s Stream from %s", vsource->desc_head, pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION));
    }

    if (vsource->uplink_sink && dest) {
        const char *z;
        pa_proplist *pl;

        pl = pa_proplist_new();
        z = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(pl, PA_PROP_DEVICE_DESCRIPTION, "Uplink sink %s on %s",
                         pa_proplist_gets(vsource->uplink_sink->proplist, "device.uplink_sink.name"), z ? z : dest->name);

        pa_sink_update_proplist(vsource->uplink_sink, PA_UPDATE_REPLACE, pl);
        pa_proplist_free(pl);
    }

    /* Propagate asyncmsq change to attached virtual sources */
    PA_IDXSET_FOREACH(output, s->outputs, idx) {
        if (output->destination_source && output->moving)
            output->moving(output, s);
    }

    /* Propagate asyncmsq change to virtual sinks attached to the uplink sink */
    if (vsource->uplink_sink) {
        PA_IDXSET_FOREACH(input, vsource->uplink_sink->inputs, idx) {
            if (input->origin_sink && input->moving)
                input->moving(input, vsource->uplink_sink);
        }
    }

}

/* Called from main context */
void pa_virtual_source_output_volume_changed(pa_source_output *o) {
    pa_source *s;
    pa_vsource *vsource;
    pa_cvolume vol;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);
    vsource = s->vsource;
    pa_assert(vsource);

    /* Preserve source volume if the master source is changing */
    if (vsource->source_moving) {
        vsource->source_moving = false;
        return;
    }

    /* Remap the volume, source and source output may have different
     * channel counts. */
    vol = o->volume;
    pa_cvolume_remap(&vol, &o->channel_map, &s->channel_map);
    pa_source_volume_changed(s, &vol);
}

/* Called from main context */
void pa_virtual_source_output_mute_changed(pa_source_output *o) {
    pa_source *s;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);

    pa_source_mute_changed(s, o->muted);
}

/* Called from main context */
void pa_virtual_source_output_suspend(pa_source_output *o, pa_source_state_t old_state, pa_suspend_cause_t old_suspend_cause) {
    pa_source *s;

    pa_source_output_assert_ref(o);
    s = o->destination_source;
    pa_assert(s);

    if (!PA_SOURCE_IS_LINKED(s->state))
        return;

    if (o->source->state != PA_SOURCE_SUSPENDED || o->source->suspend_cause == PA_SUSPEND_IDLE)
        pa_source_suspend(s, false, PA_SUSPEND_UNAVAILABLE);
    else
        pa_source_suspend(s, true, PA_SUSPEND_UNAVAILABLE);
}

/* Other functions */

void pa_virtual_source_set_callbacks(pa_source *s, bool use_volume_sharing) {

    s->parent.process_msg = pa_virtual_source_process_msg;
    s->set_state_in_main_thread = pa_virtual_source_set_state_in_main_thread;
    s->set_state_in_io_thread = pa_virtual_source_set_state_in_io_thread;
    s->update_requested_latency = pa_virtual_source_update_requested_latency;
    pa_source_set_set_mute_callback(s, pa_virtual_source_set_mute);
    if (!use_volume_sharing) {
        pa_source_set_set_volume_callback(s, pa_virtual_source_set_volume);
        pa_source_enable_decibel_volume(s, true);
    }
}

void pa_virtual_source_output_set_callbacks(pa_source_output *o, bool use_volume_sharing) {

    o->push = pa_virtual_source_output_push;
    o->update_source_latency_range = pa_virtual_source_output_update_source_latency_range;
    o->update_source_fixed_latency = pa_virtual_source_output_update_source_fixed_latency;
    o->kill = pa_virtual_source_output_kill;
    o->attach = pa_virtual_source_output_attach;
    o->detach = pa_virtual_source_output_detach;
    o->may_move_to = pa_virtual_source_output_may_move_to;
    o->moving = pa_virtual_source_output_moving;
    o->volume_changed = use_volume_sharing ? NULL : pa_virtual_source_output_volume_changed;
    o->mute_changed = pa_virtual_source_output_mute_changed;
    o->suspend = pa_virtual_source_output_suspend;
    o->update_max_rewind = pa_virtual_source_output_update_max_rewind;
    o->process_rewind = pa_virtual_source_output_process_rewind;
}

static int vsource_process_msg(pa_msgobject *obj, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_vsource *vsource;
    pa_source *s;
    pa_source_output *o;

    pa_assert(obj);
    pa_assert_ctl_context();

    vsource = PA_VSOURCE(obj);

    switch (code) {

        case VSOURCE_MESSAGE_FREE_PARAMETERS:

            pa_assert(userdata);
            pa_assert(vsource->free_filter_parameters);
            vsource->free_filter_parameters(userdata);
            return 0;

        case VSOURCE_MESSAGE_OUTPUT_ATTACHED:

            s = vsource->source;
            o = vsource->output_from_master;

            /* This may happen if a message is still pending after the vsink was
             * destroyed. */
            if (!s || !o)
                return 0;

            if (PA_SOURCE_IS_LINKED(s->state)) {
                if (o->source->state != PA_SOURCE_SUSPENDED || o->source->suspend_cause == PA_SUSPEND_IDLE)
                    pa_source_suspend(s, false, PA_SUSPEND_UNAVAILABLE);
                else
                    pa_source_suspend(s, true, PA_SUSPEND_UNAVAILABLE);
            }
            return 0;
    }
    return 0;
}

int pa_virtual_source_activate(pa_vsource *vs) {

    pa_assert(vs);
    pa_assert(vs->source);
    pa_assert(vs->output_from_master);

    /* Check that block sizes are plausible */
    if (check_block_sizes(vs->fixed_block_size, vs->fixed_input_block_size, vs->overlap_frames, vs) < 0) {
        pa_log_warn("Invalid block sizes.");
        return -1;
    }

    /* Activate uplink sink */
    if (vs->uplink_sink)
        pa_sink_put(vs->uplink_sink);

    /* Set source output latency at startup to max_latency if specified. */
    if (vs->max_latency)
        pa_source_output_set_requested_latency(vs->output_from_master, vs->max_latency);

    /* The order here is important. The output must be put first,
     * otherwise streams might attach to the source before the source
     * output is attached to the master. */
    pa_source_output_put(vs->output_from_master);
    pa_source_put(vs->source);

    /* If volume sharing and flat volumes are disabled, we have to apply the source volume to the source output. */
    if (!(vs->source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER) && !pa_source_flat_volume_enabled(vs->output_from_master->source)) {
        pa_cvolume vol;

        vol = vs->source->real_volume;
        pa_cvolume_remap(&vol, &vs->source->channel_map, &vs->output_from_master->channel_map);
        pa_source_output_set_volume(vs->output_from_master, &vol, vs->source->save_volume, true);
    }

    pa_source_output_cork(vs->output_from_master, false);

    return 0;
}

void pa_virtual_source_destroy(pa_vsource *vs) {

    pa_assert(vs);

    /* See comments in source_output_kill() above regarding
     * destruction order! */
    if (vs->output_from_master && PA_SOURCE_OUTPUT_IS_LINKED(vs->output_from_master->state))
        pa_source_output_cork(vs->output_from_master, true);

    if (vs->source)
        pa_source_unlink(vs->source);

    if (vs->output_from_master) {
        pa_source_output_unlink(vs->output_from_master);
        pa_source_output_unref(vs->output_from_master);
        vs->output_from_master = NULL;
    }

    if (vs->memblockq)
        pa_memblockq_free(vs->memblockq);

    if (vs->source) {
        pa_source_unref(vs->source);
        vs->source = NULL;
    }

    /* Destroy uplink sink if present */
    if (vs->uplink_sink) {
        struct uplink_data *uplink;

        uplink = vs->uplink_sink->userdata;
        pa_sink_unlink(vs->uplink_sink);
        pa_sink_unref(vs->uplink_sink);

        if (uplink) {
            if (uplink->memblockq)
                pa_memblockq_free(uplink->memblockq);

            pa_xfree(uplink);
        }
    }

    /* We have to use pa_msgobject_unref() here because there may still be pending
     * VSOURCE_MESSAGE_OUTPUT_ATTACHED messages. */
    pa_msgobject_unref(PA_MSGOBJECT(vs));
}

/* Manually create a vsource structure. */
pa_vsource* pa_virtual_source_vsource_new(pa_source *s) {
    pa_vsource *vsource;

    pa_assert(s);

    /* Create new vource */
    vsource = pa_msgobject_new(pa_vsource);
    vsource->parent.process_msg = vsource_process_msg;

    vsource->source = s;
    vsource->core = s->core;
    s->vsource = vsource;

    /* Reset virtual source parameters */
    vsource->output_from_master = NULL;
    vsource->memblockq = NULL;
    vsource->auto_desc = false;
    vsource->source_moving = false;
    vsource->desc_head = "Unknown Sink";
    vsource->source_type = "unknown";
    vsource->autoloaded = false;
    vsource->max_chunk_size = pa_frame_align(pa_mempool_block_size_max(s->core->mempool), &s->sample_spec);
    vsource->fixed_block_size = 0;
    vsource->fixed_input_block_size = 0;
    vsource->overlap_frames = 0;
    vsource->max_latency = 0;
    vsource->process_chunk = NULL;
    vsource->get_extra_latency = NULL;
    vsource->set_description = NULL;
    vsource->update_filter_parameters = NULL;
    vsource->update_block_sizes = NULL;
    vsource->free_filter_parameters = NULL;
    vsource->uplink_sink = NULL;

    return vsource;
}

pa_vsource *pa_virtual_source_create(pa_source *master, const char *source_type, const char *desc_prefix,
                                 pa_sample_spec *source_ss, pa_channel_map *source_map,
                                 pa_sample_spec *source_output_ss, pa_channel_map *source_output_map,
                                 pa_module *m, void *userdata, pa_modargs *ma,
                                 bool use_volume_sharing, bool create_memblockq) {

    pa_source_output_new_data source_output_data;
    pa_source_new_data source_data;
    char *source_type_property;
    bool auto_desc;
    bool force_flat_volume = false;
    bool remix = true;
    pa_resample_method_t resample_method = PA_RESAMPLER_INVALID;
    pa_vsource *vsource;
    pa_source *s;
    pa_source_output *o;
    const char *uplink_sink;
    pa_sink_new_data sink_data;

    /* Make sure all necessary values are set. Only userdata and source description
     * are allowed to be NULL. */
    pa_assert(master);
    pa_assert(source_ss);
    pa_assert(source_map);
    pa_assert(source_output_ss);
    pa_assert(source_output_map);
    pa_assert(m);
    pa_assert(ma);

    /* We do not support resampling in filters */
    pa_assert(source_output_ss->rate == source_ss->rate);

    if (!source_type)
        source_type = "unknown";
    if (!desc_prefix)
        desc_prefix = "Unknown Source";

    /* Get some command line arguments. Because there is no common default
     * for use_volume_sharing, this value must be passed as argument to
     * pa_virtual_source_create(). */

    if (pa_modargs_get_value_boolean(ma, "force_flat_volume", &force_flat_volume) < 0) {
        pa_log("force_flat_volume= expects a boolean argument");
        return NULL;
    }

    if (use_volume_sharing && force_flat_volume) {
        pa_log("Flat volume can't be forced when using volume sharing.");
        return NULL;
    }

    if (pa_modargs_get_value_boolean(ma, "remix", &remix) < 0) {
        pa_log("Invalid boolean remix parameter");
        return NULL;
    }

    if (pa_modargs_get_resample_method(ma, &resample_method) < 0) {
        pa_log("Invalid resampling method");
        return NULL;
    }

    /* Create source */
    pa_source_new_data_init(&source_data);
    source_data.driver = m->name;
    source_data.module = m;
    if (!(source_data.name = pa_xstrdup(pa_modargs_get_value(ma, "source_name", NULL))))
        source_data.name = pa_sprintf_malloc("%s.%s", master->name, source_type);
    pa_source_new_data_set_sample_spec(&source_data, source_ss);
    pa_source_new_data_set_channel_map(&source_data, source_map);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_CLASS, "filter");

    if (pa_modargs_get_proplist(ma, "source_properties", source_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&source_data);
        return NULL;
    }

    s = pa_source_new(m->core, &source_data, (master->flags & (PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY))
                                                     | (use_volume_sharing ? PA_SOURCE_SHARE_VOLUME_WITH_MASTER : 0));

    pa_source_new_data_done(&source_data);

    if (!s) {
        pa_log("Failed to create source.");
        return NULL;
    }

    /* Set name and description properties after the source has been created,
     * otherwise they may be duplicate. */
    if ((auto_desc = !pa_proplist_contains(s->proplist, PA_PROP_DEVICE_DESCRIPTION))) {
        const char *z;

        z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(s->proplist, PA_PROP_DEVICE_DESCRIPTION, "%s %s on %s", desc_prefix, s->name, z ? z : master->name);
    }

    source_type_property = pa_sprintf_malloc("device.%s.name", source_type);
    pa_proplist_sets(s->proplist, source_type_property, s->name);
    pa_xfree(source_type_property);

    /* Create vsource structure. */
    vsource = pa_virtual_source_vsource_new(s);

    pa_virtual_source_set_callbacks(s, use_volume_sharing);
    vsource->auto_desc = auto_desc;
    vsource->desc_head = desc_prefix;
    vsource->source_type = source_type;

    /* Normally this flag would be enabled automatically be we can force it. */
    if (force_flat_volume)
        s->flags |= PA_SOURCE_FLAT_VOLUME;
    s->userdata = userdata;

    pa_source_set_asyncmsgq(s, master->asyncmsgq);

    /* Create source output */
    pa_source_output_new_data_init(&source_output_data);
    source_output_data.driver = __FILE__;
    source_output_data.module = m;
    pa_source_output_new_data_set_source(&source_output_data, master, false, true);
    source_output_data.destination_source = s;

    pa_proplist_setf(source_output_data.proplist, PA_PROP_MEDIA_NAME, "%s Stream of %s", desc_prefix, pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION));
    pa_proplist_sets(source_output_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_source_output_new_data_set_sample_spec(&source_output_data, source_output_ss);
    pa_source_output_new_data_set_channel_map(&source_output_data, source_output_map);
    source_output_data.resample_method = resample_method;
    source_output_data.flags = (remix ? 0 : PA_SOURCE_OUTPUT_NO_REMIX) | PA_SOURCE_OUTPUT_START_CORKED;
    if (!pa_safe_streq(master->name, m->core->default_source->name))
        source_output_data.preferred_source = pa_xstrdup(master->name);

    if (pa_modargs_get_proplist(ma, "source_output_properties", source_output_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid source output properties");
        pa_source_output_new_data_done(&source_output_data);
        pa_virtual_source_destroy(vsource);
        return NULL;
    }

    pa_source_output_new(&o, m->core, &source_output_data);
    pa_source_output_new_data_done(&source_output_data);

    if (!o) {
        pa_log("Could not create source-output");
        pa_virtual_source_destroy(vsource);
        return NULL;
    }

    pa_virtual_source_output_set_callbacks(o, use_volume_sharing);
    o->userdata = userdata;

    vsource->output_from_master = o;

    vsource->autoloaded = false;
    if (pa_modargs_get_value_boolean(ma, "autoloaded", &vsource->autoloaded) < 0) {
        pa_log("Failed to parse autoloaded value");
        pa_virtual_source_destroy(vsource);
        return NULL;
    }

    if (create_memblockq) {
        char *tmp;
        pa_memchunk silence;

        tmp = pa_sprintf_malloc("%s memblockq", desc_prefix);
        pa_silence_memchunk_get(&s->core->silence_cache, s->core->mempool, &silence, &o->sample_spec, 0);
        vsource->memblockq = pa_memblockq_new(tmp, 0, MEMBLOCKQ_MAXLENGTH, 0, source_output_ss, 1, 1, 0, &silence);
        pa_memblock_unref(silence.memblock);
        pa_xfree(tmp);
        if (!vsource->memblockq) {
            pa_log("Failed to create memblockq");
            pa_virtual_source_destroy(vsource);
            return NULL;
        }
    }

    /* Set up uplink sink */
    uplink_sink = pa_modargs_get_value(ma, "uplink_sink", NULL);
    if (uplink_sink) {
        const char *z;
        char *tmp;
        pa_memchunk silence;
        pa_sink_flags_t flags;
        struct uplink_data *uplink;

        pa_sink_new_data_init(&sink_data);
        sink_data.driver = m->name;
        sink_data.module = m;
        sink_data.name = pa_xstrdup(uplink_sink);
        pa_sink_new_data_set_sample_spec(&sink_data, source_ss);
        pa_sink_new_data_set_channel_map(&sink_data, source_map);
        pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
        pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "uplink sink");
        pa_proplist_sets(sink_data.proplist, "device.uplink_sink.name", sink_data.name);
        z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Uplink Sink %s on %s", sink_data.name, z ? z : master->name);

        flags = 0;
        if (master->flags & PA_SOURCE_LATENCY)
            flags = PA_SINK_LATENCY;
        if (master->flags & PA_SOURCE_DYNAMIC_LATENCY)
            flags |= PA_SINK_DYNAMIC_LATENCY;
        vsource->uplink_sink = pa_sink_new(m->core, &sink_data, flags);
        pa_sink_new_data_done(&sink_data);

        if (!vsource->uplink_sink) {
            pa_log("Failed to create uplink sink");
            pa_virtual_source_destroy(vsource);
            return NULL;
        }

        uplink = pa_xnew0(struct uplink_data, 1);
        vsource->uplink_sink->userdata = uplink;

        tmp = pa_sprintf_malloc("%s uplink sink memblockq", desc_prefix);
        pa_silence_memchunk_get(&s->core->silence_cache, s->core->mempool, &silence, &s->sample_spec, 0);
        uplink->memblockq = pa_memblockq_new(tmp, 0, MEMBLOCKQ_MAXLENGTH, 0, source_ss, 1, 1, 0, &silence);
        pa_memblock_unref(silence.memblock);
        pa_xfree(tmp);
        if (!uplink->memblockq) {
            pa_log("Failed to create sink memblockq");
            pa_virtual_source_destroy(vsource);
            return NULL;
        }

        vsource->uplink_sink->parent.process_msg = sink_process_msg;
        vsource->uplink_sink->update_requested_latency = sink_update_requested_latency;
        vsource->uplink_sink->set_state_in_main_thread = sink_set_state_in_main_thread;
        vsource->uplink_sink->set_state_in_io_thread = sink_set_state_in_io_thread;
        vsource->uplink_sink->uplink_of = vsource;
        uplink->vsource = vsource;

        pa_sink_set_asyncmsgq(vsource->uplink_sink, master->asyncmsgq);
    }

    return vsource;
}

/* Send request to update filter parameters to the I/O-thread. */
void pa_virtual_source_request_parameter_update(pa_vsource *vs, void *parameters) {

    pa_assert(vs);
    pa_assert(vs->source);

    /* parameters may be NULL if it is enough to have access to userdata from the
     * callback. */
    pa_asyncmsgq_send(vs->source->asyncmsgq, PA_MSGOBJECT(vs->source), SOURCE_MESSAGE_UPDATE_PARAMETERS, parameters, 0, NULL);
}
