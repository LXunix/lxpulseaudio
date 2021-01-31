/***
    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>

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

#include <stdio.h>

#include <pulse/xmalloc.h>

#include <modules/virtual-source-common.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/mix.h>
#include <pulsecore/rtpoll.h>

PA_MODULE_AUTHOR("Pierre-Louis Bossart");
PA_MODULE_DESCRIPTION("Virtual source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        _("source_name=<name for the source> "
          "source_properties=<properties for the source> "
          "master=<name of source to filter> "
          "uplink_sink=<name> (optional)"
          "format=<sample format> "
          "rate=<sample rate> "
          "channels=<number of channels> "
          "channel_map=<channel map> "
          "use_volume_sharing=<yes or no> "
          "force_flat_volume=<yes or no> "
        ));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)
#define BLOCK_USEC 1000 /* FIXME */

struct userdata {
    pa_module *module;

    pa_vsource *vsource;
    unsigned channels;

    /* optional fields for uplink sink */
    pa_sink *sink;
    pa_usec_t block_usec;
    pa_memblockq *sink_memblockq;
    pa_rtpoll *rtpoll;
    bool auto_desc;

};

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    "master",
    "uplink_sink",
    "format",
    "rate",
    "channels",
    "channel_map",
    "use_volume_sharing",
    "force_flat_volume",
    NULL
};

static void filter_process_chunk(uint8_t *src, uint8_t *dst, unsigned in_count, unsigned out_count, void *userdata) {
    struct userdata *u;
    size_t nbytes;

    pa_assert_se(u = userdata);
    pa_assert(in_count == out_count);

    nbytes = in_count * pa_frame_size(&u->vsource->source->sample_spec);

    /* if uplink sink exists, pull data from there; simplify by using
       same length as chunk provided by source */
    if (u->sink && (u->sink->thread_info.state == PA_SINK_RUNNING)) {
        pa_memchunk tchunk;
        pa_mix_info streams[2];
        pa_memchunk chunk;
        void *src_copy;
        int ch;
        pa_source_output *o;

        /* Hmm, process any rewind request that might be queued up */
        pa_sink_process_rewind(u->sink, 0);

        /* get data from the sink */
        while (pa_memblockq_peek(u->sink_memblockq, &tchunk) < 0) {
            pa_memchunk nchunk;

            /* make sure we get nbytes from the sink with render_full,
               otherwise we cannot mix with the uplink */
            pa_sink_render_full(u->sink, nbytes, &nchunk);
            pa_memblockq_push(u->sink_memblockq, &nchunk);
            pa_memblock_unref(nchunk.memblock);
        }
        pa_assert(tchunk.length == nbytes);

        /* move the read pointer for sink memblockq */
        pa_memblockq_drop(u->sink_memblockq, tchunk.length);

        o = u->vsource->output_from_master;

        /* allocate source chunk */
        chunk.index = 0;
        chunk.length = nbytes;
        chunk.memblock = pa_memblock_new(o->source->core->mempool, nbytes);
        pa_assert(chunk.memblock);

        /* Copy source data to chunk */
        src_copy = pa_memblock_acquire_chunk(&chunk);
        memcpy(src_copy, src, nbytes);

        /* set-up mixing structure
           volume was taken care of in sink and source already */
        streams[0].chunk = chunk;
        for(ch=0;ch<o->sample_spec.channels;ch++)
            streams[0].volume.values[ch] = PA_VOLUME_NORM; /* FIXME */
        streams[0].volume.channels = o->sample_spec.channels;

        streams[1].chunk = tchunk;
        for(ch=0;ch<o->sample_spec.channels;ch++)
            streams[1].volume.values[ch] = PA_VOLUME_NORM; /* FIXME */
        streams[1].volume.channels = o->sample_spec.channels;

        /* do mixing */
        pa_mix(streams,                /* 2 streams to be mixed */
               2,
               dst,                    /* put result in dst */
               nbytes,                 /* same length as input */
               (const pa_sample_spec *)&o->sample_spec, /* same sample spec for input and output */
               NULL,                   /* no volume information */
               false);                 /* no mute */

        pa_memblock_release(chunk.memblock);
        pa_memblock_unref(tchunk.memblock);
        pa_memblock_unref(chunk.memblock);
    } else
        /* Copy input to output */
        memcpy(dst, src, nbytes);
}

/* When the source output moves, the asyncmsgq of the uplink sink has
 * to change as well */
static void source_output_moving_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_assert(u = o->userdata);

    pa_virtual_source_output_moving(o, dest);
    if (dest && u->sink) {
        pa_sink_set_asyncmsgq(u->sink, dest->asyncmsgq);
    }
}

/* Called from I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY:

            /* there's no real latency here */
            *((int64_t*) data) = 0;

            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_in_main_thread_cb(pa_sink *s, pa_sink_state_t state, pa_suspend_cause_t suspend_cause) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(state)) {
        return 0;
    }

    if (state == PA_SINK_RUNNING) {
        /* need to wake-up source if it was suspended */
        pa_log_debug("Resuming source %s, because its uplink sink became active.", u->vsource->source->name);
        pa_source_suspend(u->vsource->source, false, PA_SUSPEND_ALL);

        /* FIXME: if there's no client connected, the source will suspend
           and playback will be stuck. You'd want to prevent the source from
           sleeping when the uplink sink is active; even if the audio is
           discarded at least the app isn't stuck */

    } else {
        /* nothing to do, if the sink becomes idle or suspended let
           module-suspend-idle handle the sources later */
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* FIXME: there's no latency support */

}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    pa_source *master=NULL;
    bool use_volume_sharing = true;

    /* optional for uplink_sink */
    pa_sink_new_data sink_data;
    size_t nbytes;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "master", NULL), PA_NAMEREG_SOURCE))) {
        pa_log("Master source not found");
        goto fail;
    }

    pa_assert(master);

    ss = master->sample_spec;
    map = master->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "use_volume_sharing", &use_volume_sharing) < 0) {
        pa_log("use_volume_sharing= expects a boolean argument");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;
    u->channels = ss.channels;

    /* The rtpoll created here is never run. It is only necessary to avoid crashes
     * when module-virtual-source is used together with module-loopback or
     * module-combine-sink. Both modules base their asyncmsq on the rtpoll provided
     * by the sink. module-loopback and combine-sink only work because they
     * call pa_asyncmsq_process_one() themselves. */
    u->rtpoll = pa_rtpoll_new();

        /* Create virtual source */
    if (!(u->vsource = pa_virtual_source_create(master, "vsource", "Virtual Source", &ss, &map,
                                   &ss, &map, m, u, ma, use_volume_sharing, true)))
        goto fail;

    /* Set callback for virtual source */
    u->vsource->process_chunk = filter_process_chunk;
    u->vsource->output_from_master->moving = source_output_moving_cb;

    /* Create optional uplink sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    if ((sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "uplink_sink", NULL)))) {
        pa_sink_new_data_set_sample_spec(&sink_data, &ss);
        pa_sink_new_data_set_channel_map(&sink_data, &map);
        pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
        pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "uplink sink");
        pa_proplist_sets(sink_data.proplist, "device.uplink_sink.name", sink_data.name);

        if ((u->auto_desc = !pa_proplist_contains(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION))) {
            const char *z;

            z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
            pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Uplink Sink %s on %s", sink_data.name, z ? z : master->name);
        }

        u->sink_memblockq = pa_memblockq_new("module-virtual-source sink_memblockq", 0, MEMBLOCKQ_MAXLENGTH, 0, &ss, 1, 1, 0, NULL);
        if (!u->sink_memblockq) {
            pa_sink_new_data_done(&sink_data);
            pa_log("Failed to create sink memblockq.");
            goto fail;
        }

        u->sink = pa_sink_new(m->core, &sink_data, 0);  /* FIXME, sink has no capabilities */
        pa_sink_new_data_done(&sink_data);

        if (!u->sink) {
            pa_log("Failed to create sink.");
            goto fail;
        }

        u->sink->parent.process_msg = sink_process_msg_cb;
        u->sink->update_requested_latency = sink_update_requested_latency_cb;
        u->sink->set_state_in_main_thread = sink_set_state_in_main_thread_cb;
        u->sink->userdata = u;

        pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);
        pa_sink_set_rtpoll(u->sink, u->rtpoll);

        /* FIXME: no idea what I am doing here */
        u->block_usec = BLOCK_USEC;
        nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);
        pa_sink_set_max_rewind(u->sink, 0);
        pa_sink_set_max_request(u->sink, nbytes);

        pa_sink_put(u->sink);
    } else {
        pa_sink_new_data_done(&sink_data);
        /* optional uplink sink not enabled */
        u->sink = NULL;
    }

    if (pa_virtual_source_activate(u->vsource) < 0)
        goto fail;

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_source_linked_by(u->vsource->source);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->vsource)
        pa_virtual_source_destroy(u->vsource);

    if (u->sink) {
        pa_sink_unlink(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->sink_memblockq)
        pa_memblockq_free(u->sink_memblockq);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    pa_xfree(u);
}
