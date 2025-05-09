#ifndef footestutilhfoo
#define footestutilhfoo

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

#include <stdbool.h>

#include <pulse/pulseaudio.h>

#include <pulsecore/idxset.h>
#include <pulsecore/macro.h>

#define WAIT_FOR_OPERATION(ctx, o)                                      \
    do {                                                                \
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {     \
            pa_threaded_mainloop_wait(ctx->mainloop);                   \
        }                                                               \
                                                                        \
        pa_assert(pa_operation_get_state(o) == PA_OPERATION_DONE);      \
        pa_operation_unref(o);                                          \
    } while (false)

typedef struct pa_test_context {
    /* "Public" members */
    pa_threaded_mainloop *mainloop;
    pa_mainloop_api *mainloop_api;
    pa_context *context;

    /* "Private" bookkeeping */
    pa_idxset *modules;
    uint32_t module_idx, sink_idx; /* only used for module -> sink index lookup */
    void *data;
    size_t length;
} pa_test_context;

pa_test_context* pa_test_context_new(const char *name);
void pa_test_context_free(pa_test_context *ctx);

/* Loads a null sink with provided params to test with */
uint32_t pa_test_context_load_null_sink(pa_test_context *ctx, const char *modargs);

/* A stream is created and started. The function doesn't wait for the data to
 * be played back, playback will continue in the background. The data buffer
 * will be played only once, after which an underflow callback will call
 * pa_threaded_mainloop_signal() so pa_threaded_mainloop_wait() can be used to
 * wait for the stream to finish playing.
 */
pa_stream* pa_test_context_create_stream(pa_test_context *ctx, const char *name, uint32_t sink_idx, pa_format_info *format,
                                         pa_stream_flags_t flags, void *data, size_t length);
/* Clean up the stream */
void pa_test_context_destroy_stream(pa_test_context *ctx, pa_stream *s);

typedef bool (*pa_test_sink_info_pred_t)(const pa_sink_info *sink_info, void *userdata);

/* Test the current state of the sink by providing a predicate function which
 * can examine the sink's pa_sink_info for whatever condition is expected. */
bool pa_test_context_check_sink(pa_test_context *ctx, uint32_t idx, pa_test_sink_info_pred_t predicate, void *userdata);

#endif /* footestutilhfoo */
