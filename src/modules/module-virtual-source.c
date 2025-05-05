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
    "autoloaded",
    NULL
};

static void filter_process_chunk(uint8_t *src, uint8_t *dst, unsigned in_count, unsigned out_count, void *userdata) {
    struct userdata *u;
    size_t nbytes;

    pa_assert_se(u = userdata);
    pa_assert(in_count == out_count);

    nbytes = in_count * pa_frame_size(&u->vsource->source->sample_spec);

    /* Copy input to output */
    memcpy(dst, src, nbytes);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    pa_source *master=NULL;
    bool use_volume_sharing = true;

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

    /* Create virtual source */
    if (!(u->vsource = pa_virtual_source_create(master, "vsource", "Virtual Source", &ss, &map,
                                   &ss, &map, m, u, ma, use_volume_sharing, true)))
        goto fail;

    /* Set callback for virtual source */
    u->vsource->process_chunk = filter_process_chunk;

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

    pa_xfree(u);
}
