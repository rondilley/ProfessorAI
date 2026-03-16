/*
 * recommend.h -- Hardware detection and model recommendations
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_RECOMMEND_H
#define PROF_RECOMMEND_H

#include <stdint.h>

typedef struct {
    char     cpu_model[256];
    int      cpu_cores;
    int      cpu_threads;

    char     gpu_name[256];
    char     gpu_arch[32];       /* e.g., "gfx1151" */
    int      gpu_compute_units;
    int      gpu_clock_mhz;

    int64_t  ram_total_bytes;
    int64_t  ram_available_bytes;
    int64_t  vram_bytes;         /* dedicated VRAM */
    int64_t  gtt_bytes;          /* GPU-accessible system RAM */

    int      has_npu;
    char     npu_name[128];

    int      is_uma;             /* unified memory architecture */
    int64_t  effective_gpu_mem;  /* usable memory for model loading */
} hw_info_t;

int  hw_detect(hw_info_t *hw);
void hw_print_summary(const hw_info_t *hw);
void hw_print_recommendations(const hw_info_t *hw);

#endif /* PROF_RECOMMEND_H */
