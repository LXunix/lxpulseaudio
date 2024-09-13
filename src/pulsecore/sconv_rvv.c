/***
  This file is part of PulseAudio.

  Copyright (c) 2024 Institue of Software Chinese Academy of Sciences (ISCAS).

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include "cpu-riscv.h"
#include "sconv.h"

#if HAVE_RVV
static void pa_sconv_s16le_from_f32ne_rvv(unsigned n, const float *src, int16_t *dst) {
    __asm__ __volatile__ (
        ".option       arch, +v                    \n\t"
        "li            t0, 1191182336              \n\t"
        "fmv.w.x       fa5, t0                     \n\t"
        "1:                                        \n\t"
        "vsetvli       t0, a0, e32, m8, ta, ma     \n\t"
        "vle32.v       v8, (a1)                    \n\t"
        "sub           a0, a0, t0                  \n\t"
        "vfmul.vf      v8, v8, fa5                 \n\t"
        "vsetvli       zero, zero, e16, m4, ta, ma \n\t"
        "vfncvt.x.f.w  v8, v8                      \n\t"
        "slli          t0, t0, 1                   \n\t"
        "vse16.v       v8, (a2)                    \n\t"
        "add           a1, a1, t0                  \n\t"
        "add           a1, a1, t0                  \n\t"
        "add           a2, a2, t0                  \n\t"
        "bnez          a0, 1b                      \n\t"

        :
        :
        : "cc", "memory"
    );
}

void pa_convert_func_init_rvv(pa_cpu_riscv_flag_t flags) {
    pa_log_info("Initialising RVV optimized conversions.");

    pa_set_convert_from_float32ne_function(PA_SAMPLE_S16LE, (pa_convert_func_t) pa_sconv_s16le_from_f32ne_rvv);
}
#endif
