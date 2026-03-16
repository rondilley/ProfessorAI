/*
 * recommend.c -- Hardware detection and model recommendations
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "recommend.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Helpers --- */

static int64_t read_sysfs_int64(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char buf[64];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return strtoll(buf, NULL, 10);
}

static int read_sysfs_str(const char *path, char *out, size_t out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(out, (int)out_len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    /* Strip trailing newline */
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '\n') {
        out[len - 1] = '\0';
    }
    return 0;
}

static int read_proc_field(const char *path, const char *key,
                           char *out, size_t out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, klen) == 0) {
            char *val = line + klen;
            while (*val == ' ' || *val == ':' || *val == '\t') val++;
            /* Strip trailing newline */
            size_t vlen = strlen(val);
            if (vlen > 0 && val[vlen - 1] == '\n') val[vlen - 1] = '\0';
            snprintf(out, out_len, "%s", val);
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int64_t read_proc_meminfo_kb(const char *key)
{
    char val[64];
    if (read_proc_field("/proc/meminfo", key, val, sizeof(val)) != 0) {
        return -1;
    }
    /* Value is like "131163160 kB" */
    return strtoll(val, NULL, 10) * 1024;
}

/* --- GPU detection via sysfs --- */

static int find_gpu_drm_card(char *card_path, size_t path_len)
{
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* Look for card0, card1, etc. (not card0-DP-1 etc.) */
        if (strncmp(ent->d_name, "card", 4) != 0) continue;
        if (strchr(ent->d_name + 4, '-') != NULL) continue;

        char vendor_path[512];
        snprintf(vendor_path, sizeof(vendor_path),
                 "/sys/class/drm/%s/device/vendor", ent->d_name);

        char vendor[16];
        if (read_sysfs_str(vendor_path, vendor, sizeof(vendor)) == 0) {
            /* AMD vendor ID: 0x1002 */
            if (strcmp(vendor, "0x1002") == 0) {
                snprintf(card_path, path_len,
                         "/sys/class/drm/%s/device", ent->d_name);
                closedir(dir);
                return 0;
            }
        }
    }

    closedir(dir);
    return -1;
}

static void detect_gpu(hw_info_t *hw)
{
    char card_path[512];
    if (find_gpu_drm_card(card_path, sizeof(card_path)) != 0) {
        snprintf(hw->gpu_name, sizeof(hw->gpu_name), "Not detected");
        return;
    }

    /* VRAM */
    char path[600];
    snprintf(path, sizeof(path), "%s/mem_info_vram_total", card_path);
    hw->vram_bytes = read_sysfs_int64(path);

    /* GTT */
    snprintf(path, sizeof(path), "%s/mem_info_gtt_total", card_path);
    hw->gtt_bytes = read_sysfs_int64(path);

    /* GPU name from product name or marketing name via rocminfo-like approach */
    /* Try uevent for a quick name */
    snprintf(path, sizeof(path), "%s/uevent", card_path);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
                /* Not the name we want, but indicates AMD GPU is present */
            }
        }
        fclose(fp);
    }

    /* Read GPU revision/architecture from pp_dpm_sclk or similar */
    /* For now, parse /proc/cpuinfo model name which on APUs includes GPU info */

    /* Determine if UMA (APU): dedicated VRAM < 4 GB and GTT is large */
    if (hw->vram_bytes > 0 && hw->vram_bytes < (int64_t)4 * 1024 * 1024 * 1024 &&
        hw->gtt_bytes > (int64_t)4 * 1024 * 1024 * 1024) {
        hw->is_uma = 1;
        hw->effective_gpu_mem = hw->gtt_bytes;
    } else if (hw->vram_bytes > 0) {
        hw->is_uma = 0;
        hw->effective_gpu_mem = hw->vram_bytes;
    }
}

/* --- CPU detection --- */

static void detect_cpu(hw_info_t *hw)
{
    read_proc_field("/proc/cpuinfo", "model name",
                    hw->cpu_model, sizeof(hw->cpu_model));

    char val[32];
    /* Count cores from /proc/cpuinfo by counting "processor" lines */
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        int count = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "processor", 9) == 0) {
                count++;
            }
        }
        fclose(fp);
        hw->cpu_threads = count;
    }

    /* Try to get physical cores from lscpu-style info */
    if (read_proc_field("/proc/cpuinfo", "cpu cores", val, sizeof(val)) == 0) {
        hw->cpu_cores = (int)strtol(val, NULL, 10);
    } else {
        hw->cpu_cores = hw->cpu_threads / 2;
        if (hw->cpu_cores < 1) hw->cpu_cores = 1;
    }
}

/* --- NPU detection --- */

static void detect_npu(hw_info_t *hw)
{
    struct stat st;
    if (stat("/dev/accel0", &st) == 0) {
        hw->has_npu = 1;
        /* Try to read NPU name from sysfs */
        DIR *dir = opendir("/sys/class/accel");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strncmp(ent->d_name, "accel", 5) != 0) continue;
                char npu_path[512];
                snprintf(npu_path, sizeof(npu_path),
                         "/sys/class/accel/%s/device/uevent", ent->d_name);
                FILE *fp = fopen(npu_path, "r");
                if (fp) {
                    char line[128];
                    while (fgets(line, sizeof(line), fp)) {
                        if (strncmp(line, "DRIVER=", 7) == 0) {
                            size_t ll = strlen(line);
                            if (ll > 0 && line[ll - 1] == '\n') line[ll - 1] = '\0';
                            snprintf(hw->npu_name, sizeof(hw->npu_name),
                                     "%s", line + 7);
                        }
                    }
                    fclose(fp);
                }
                break;
            }
            closedir(dir);
        }
        if (hw->npu_name[0] == '\0') {
            snprintf(hw->npu_name, sizeof(hw->npu_name), "detected");
        }
    }
}

/* --- Public API --- */

int hw_detect(hw_info_t *hw)
{
    memset(hw, 0, sizeof(*hw));

    detect_cpu(hw);

    hw->ram_total_bytes = read_proc_meminfo_kb("MemTotal");
    hw->ram_available_bytes = read_proc_meminfo_kb("MemAvailable");

    detect_gpu(hw);
    detect_npu(hw);

    /* GPU name from CPU model string (APU) or sysfs product name */
    if (hw->gpu_name[0] == '\0' || strcmp(hw->gpu_name, "Not detected") == 0) {
        char product_path[512];
        char card_path[512];
        if (find_gpu_drm_card(card_path, sizeof(card_path)) == 0) {
            snprintf(product_path, sizeof(product_path),
                     "%s/product_name", card_path);
            read_sysfs_str(product_path, hw->gpu_name, sizeof(hw->gpu_name));
        }
    }

    /* Infer arch and marketing name from CPU model for known APUs */
    if (hw->gpu_arch[0] == '\0') {
        if (strstr(hw->cpu_model, "Ryzen AI MAX") != NULL ||
            strstr(hw->cpu_model, "RYZEN AI MAX") != NULL) {
            if (hw->gpu_name[0] == '\0') {
                snprintf(hw->gpu_name, sizeof(hw->gpu_name),
                         "Integrated RDNA 3.5 APU");
            }
            snprintf(hw->gpu_arch, sizeof(hw->gpu_arch), "gfx1151");
        } else if (strstr(hw->cpu_model, "Ryzen AI") != NULL ||
                   strstr(hw->cpu_model, "RYZEN AI") != NULL) {
            if (hw->gpu_name[0] == '\0') {
                snprintf(hw->gpu_name, sizeof(hw->gpu_name),
                         "Integrated RDNA 3.5 APU");
            }
            snprintf(hw->gpu_arch, sizeof(hw->gpu_arch), "gfx1150");
        } else if (hw->vram_bytes > 0 && hw->gpu_name[0] == '\0') {
            snprintf(hw->gpu_name, sizeof(hw->gpu_name), "AMD GPU");
        }
    }

    return 0;
}

#define GB (1024LL * 1024 * 1024)

void hw_print_summary(const hw_info_t *hw)
{
    printf("=== System Hardware ===\n\n");

    printf("  CPU:          %s\n", hw->cpu_model);
    printf("  Cores:        %d cores / %d threads\n", hw->cpu_cores, hw->cpu_threads);
    printf("\n");

    if (hw->gpu_name[0]) {
        printf("  GPU:          %s\n", hw->gpu_name);
        if (hw->gpu_arch[0]) {
            printf("  GPU arch:     %s\n", hw->gpu_arch);
        }
    }

    if (hw->is_uma) {
        printf("  Architecture: UMA (APU -- GPU shares system RAM)\n");
    }

    printf("  System RAM:   %.1f GB total, %.1f GB available\n",
           (double)hw->ram_total_bytes / GB,
           (double)hw->ram_available_bytes / GB);

    if (hw->vram_bytes > 0) {
        printf("  Dedicated VRAM: %.1f GB\n", (double)hw->vram_bytes / GB);
    }
    if (hw->gtt_bytes > 0) {
        printf("  GTT (GPU-accessible RAM): %.1f GB\n", (double)hw->gtt_bytes / GB);
    }
    if (hw->effective_gpu_mem > 0) {
        printf("  Effective GPU memory: %.0f GB\n",
               (double)hw->effective_gpu_mem / GB);
    }

    if (hw->has_npu) {
        printf("  NPU:          %s (not used by llama.cpp)\n", hw->npu_name);
    }

    printf("\n");
}

/* --- Model database --- */

typedef struct {
    const char *name;
    const char *quant;
    double      size_gb;
    const char *notes;
    int         chat;      /* 1 = instruction-tuned with chat template */
} model_entry_t;

static const model_entry_t hermes_models[] = {
    { "Hermes-3-Llama-3.1-8B",  "Q4_K_M",  4.9, "Fast, good quality",          1 },
    { "Hermes-3-Llama-3.1-8B",  "Q6_K",    6.6, "Higher quality",              1 },
    { "Hermes-3-Llama-3.1-8B",  "Q8_0",    8.5, "Near-lossless 8B",            1 },
    { "Hermes-3-Llama-3.1-8B",  "F16",    16.0, "Full precision 8B",           1 },
    { "Hermes-3-Llama-3.1-70B", "Q3_K_M", 33.0, "Aggressive quant, usable",   1 },
    { "Hermes-3-Llama-3.1-70B", "Q4_K_M", 40.0, "Best balance for 70B",       1 },
    { "Hermes-3-Llama-3.1-70B", "Q5_K_M", 50.0, "High quality 70B",           1 },
    { "Hermes-3-Llama-3.1-70B", "Q6_K",   58.0, "Very high quality 70B",      1 },
    { "Hermes-3-Llama-3.1-70B", "Q8_0",   74.0, "Near-lossless 70B",          1 },
    { NULL, NULL, 0, NULL, 0 }
};

static const model_entry_t other_models[] = {
    { "Qwen2.5-72B-Instruct",     "Q4_K_M", 42.0, "Strong multilingual",       1 },
    { "Qwen2.5-32B-Instruct",     "Q6_K",   25.0, "Good mid-size option",      1 },
    { "Qwen2.5-14B-Instruct",     "Q8_0",   15.0, "Fast and capable",          1 },
    { "Llama-3.3-70B-Instruct",   "Q4_K_M", 40.0, "Meta's latest 70B",        1 },
    { "Mistral-Large-2411",        "Q4_K_M", 38.0, "Strong reasoning",          1 },
    { "DeepSeek-R1-Distill-70B",  "Q4_K_M", 40.0, "Reasoning focused",        1 },
    { "Phi-4-14B",                 "Q8_0",   15.0, "Microsoft, fast",           1 },
    { NULL, NULL, 0, NULL, 0 }
};

static void print_model_table(const model_entry_t *models, int64_t budget_gb,
                               const char *header)
{
    printf("%s\n", header);
    printf("  %-35s %-8s %6s  %-5s  %s\n",
           "Model", "Quant", "Size", "Fit", "Notes");
    printf("  %-35s %-8s %6s  %-5s  %s\n",
           "-----------------------------------", "--------", "------",
           "-----", "------------------------------");

    for (const model_entry_t *m = models; m->name != NULL; m++) {
        const char *fit;
        char fit_marker;

        if (m->size_gb <= (double)budget_gb * 0.65) {
            fit = "Easy";
            fit_marker = '*';  /* recommended */
        } else if (m->size_gb <= (double)budget_gb * 0.85) {
            fit = "Good";
            fit_marker = '*';  /* recommended */
        } else if (m->size_gb <= (double)budget_gb * 0.95) {
            fit = "Tight";
            fit_marker = ' ';
        } else if (m->size_gb <= (double)budget_gb * 1.3) {
            fit = "Spill";
            fit_marker = ' ';
        } else {
            fit = "No";
            fit_marker = ' ';
        }

        printf("  %c %-34s %-8s %5.0f GB  %-5s  %s\n",
               fit_marker, m->name, m->quant, m->size_gb, fit, m->notes);
    }
    printf("\n");
    printf("  * = recommended for your hardware\n\n");
}

void hw_print_recommendations(const hw_info_t *hw)
{
    int64_t budget = hw->effective_gpu_mem;
    if (budget <= 0) {
        budget = hw->ram_available_bytes;
    }
    int64_t budget_gb = budget / GB;

    /* Leave headroom for KV cache and OS */
    int64_t model_budget_gb = budget_gb;
    if (hw->is_uma) {
        /* On UMA, reserve ~15% for KV cache + OS overhead */
        model_budget_gb = (int64_t)((double)budget_gb * 0.85);
    }

    printf("=== Model Recommendations ===\n\n");
    printf("  GPU memory budget: %lld GB", (long long)budget_gb);
    if (hw->is_uma) {
        printf(" (UMA -- shared with system)");
    }
    printf("\n");
    printf("  Model size budget: ~%lld GB (reserving headroom for KV cache)\n\n",
           (long long)model_budget_gb);

    print_model_table(hermes_models, model_budget_gb,
                      "  Hermes Models (NousResearch):");
    print_model_table(other_models, model_budget_gb,
                      "  Other Recommended Models:");

    /* Top pick */
    const model_entry_t *best = NULL;
    for (const model_entry_t *m = hermes_models; m->name != NULL; m++) {
        if (m->size_gb <= (double)model_budget_gb * 0.85 &&
            m->size_gb > 10.0) {
            best = m;
        }
    }

    if (!best) {
        /* Fall back to smallest */
        best = &hermes_models[0];
    }

    printf("  === Top Pick ===\n\n");
    printf("  %s %s (%.0f GB)\n", best->name, best->quant, best->size_gb);
    printf("  Best balance of quality and speed for your hardware.\n\n");

    printf("  Download from Hugging Face:\n");
    printf("  huggingface-cli download NousResearch/%s-GGUF %s.%s.gguf\n\n",
           best->name, best->name, best->quant);

    if (hw->is_uma) {
        printf("  Note: This is a UMA/APU system. The GPU shares system RAM.\n");
        printf("  Models fitting in GTT (%.0f GB) get full GPU acceleration.\n",
               (double)hw->gtt_bytes / GB);
        printf("  Larger models spill to CPU RAM and run slower.\n\n");
    }

    if (hw->has_npu) {
        printf("  Note: NPU detected (%s) but llama.cpp does not currently\n",
               hw->npu_name);
        printf("  support AMD XDNA/NPU acceleration. GPU is used instead.\n\n");
    }
}
