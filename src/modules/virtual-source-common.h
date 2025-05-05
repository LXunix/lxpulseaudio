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

#include <pulsecore/source.h>
#include <pulsecore/modargs.h>

/* Callbacks for virtual sources. */
int pa_virtual_source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk);

int pa_virtual_source_set_state_in_main_thread(pa_source *s, pa_source_state_t state, pa_suspend_cause_t suspend_cause);
int pa_virtual_source_set_state_in_io_thread(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause);

void pa_virtual_source_update_requested_latency(pa_source *s);
void pa_virtual_source_set_volume(pa_source *s);
void pa_virtual_source_set_mute(pa_source *s);

void pa_virtual_source_output_push(pa_source_output *o, const pa_memchunk *chunk);

void pa_virtual_source_output_update_source_latency_range(pa_source_output *o);
void pa_virtual_source_output_update_source_fixed_latency(pa_source_output *o);

void pa_virtual_source_output_process_rewind(pa_source_output *o, size_t nbytes);
void pa_virtual_source_output_update_max_rewind(pa_source_output *o, size_t nbytes);

void pa_virtual_source_output_detach(pa_source_output *o);
void pa_virtual_source_output_attach(pa_source_output *o);
void pa_virtual_source_output_kill(pa_source_output *o);
void pa_virtual_source_output_moving(pa_source_output *o, pa_source *dest);
bool pa_virtual_source_output_may_move_to(pa_source_output *o, pa_source *dest);

void pa_virtual_source_output_volume_changed(pa_source_output *o);
void pa_virtual_source_output_mute_changed(pa_source_output *o);

void pa_virtual_source_output_suspend(pa_source_output *o, pa_source_state_t old_state, pa_suspend_cause_t old_suspend_cause);

/* Set callbacks for virtual source and source output. */
void pa_virtual_source_set_callbacks(pa_source *s, bool use_volume_sharing);
void pa_virtual_source_output_set_callbacks(pa_source_output *o, bool use_volume_sharing);

/* Create a new virtual source. Returns a filled vsource structure or NULL on failure. */
pa_vsource *pa_virtual_source_create(pa_source *master, const char *source_type, const char *desc_prefix,
                                 pa_sample_spec *source_ss, pa_channel_map *source_map,
                                 pa_sample_spec *source_output_ss, pa_channel_map *source_output_map,
                                 pa_module *m, void *userdata, pa_modargs *ma,
                                 bool use_volume_sharing, bool create_memblockq);

/* Activate the new virtual source. */
int pa_virtual_source_activate(pa_vsource *vs);

/* Destroys the objects associated with the virtual source. */
void pa_virtual_source_destroy(pa_vsource *vs);

/* Create vsource structure */
pa_vsource* pa_virtual_source_vsource_new(pa_source *s);

/* Update filter parameters */
void pa_virtual_source_request_parameter_update(pa_vsource *vs, void *parameters);

/* Post data, mix in uplink sink */
void pa_virtual_source_post(pa_source *s, const pa_memchunk *chunk);
