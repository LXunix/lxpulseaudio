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

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>

#if HAVE_SYS_AUXV_H
#include <sys/auxv.h>
#define HWCAP_RV(letter) (1ul << ((letter) - 'A'))
#endif

#include <pulsecore/log.h>

#include "cpu-riscv.h"

void pa_cpu_get_riscv_flags(pa_cpu_riscv_flag_t *flags) {
#if HAVE_SYS_AUXV_H
    const unsigned long hwcap = getauxval(AT_HWCAP);

    if (hwcap & HWCAP_RV('V'))
        *flags |= PA_CPU_RISCV_V;

    pa_log_info("CPU flags: %s", (*flags & PA_CPU_RISCV_V) ? "V" : "");
#endif
}

bool pa_cpu_init_riscv(pa_cpu_riscv_flag_t *flags) {
    pa_cpu_get_riscv_flags(flags);

#if HAVE_RVV
    if (*flags & PA_CPU_RISCV_V) {
        pa_convert_func_init_rvv(*flags);
    }
    return true;
#else
    return false;
#endif
}
