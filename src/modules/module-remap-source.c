/***
    This file is part of PulseAudio.

    Copyright 2013 bct electronic GmbH
    Contributor: Stefan Huber <s.huber@bct-electronic.com>

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

#include <stdio.h>

#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/namereg.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/mix.h>

PA_MODULE_AUTHOR("Stefan Huber");
PA_MODULE_DESCRIPTION("Virtual channel remapping source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "source_name=<name for the source> "
        "source_properties=<properties for the source> "
        "master=<name of source to filter> "
        "master_channel_map=<channel map> "
        "uplink_sink=<name> (optional)"
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        "resample_method=<resampler> "
        "remix=<remix channels?>");

struct userdata {
    pa_module *module;

    pa_vsource *vsource;
};

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    "master",
    "master_channel_map",
    "uplink_sink",
    "format",
    "rate",
    "channels",
    "channel_map",
    "resample_method",
    "remix",
    NULL
};

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map source_map, stream_map;
    pa_modargs *ma;
    pa_source *master;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "master", NULL), PA_NAMEREG_SOURCE))) {
        pa_log("Master source not found.");
        goto fail;
    }

    ss = master->sample_spec;
    source_map = master->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &source_map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map.");
        goto fail;
    }

    stream_map = source_map;
    if (pa_modargs_get_channel_map(ma, "master_channel_map", &stream_map) < 0) {
        pa_log("Invalid master channel map.");
        goto fail;
    }

    if (stream_map.channels != ss.channels) {
        pa_log("Number of channels doesn't match.");
        goto fail;
    }

    if (pa_channel_map_equal(&stream_map, &master->channel_map))
        pa_log_warn("No remapping configured, proceeding nonetheless!");

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;

    /* Create virtual sink */
    if (!(u->vsource = pa_virtual_source_create(master, "remapped", "Remapped Source", &ss, &source_map,
                                      &ss, &stream_map, m, u, ma, false, false)))
         goto fail;

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
