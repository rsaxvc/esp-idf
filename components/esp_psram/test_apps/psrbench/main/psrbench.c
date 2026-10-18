/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * PSRAM 16B SIMD store microbenchmark.
 *
 * Determines whether a cacheline-sized (16B) SIMD store that misses the D-cache
 * allocates the line directly, or fetches it from PSRAM before overwriting it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_random.h"
#include "esp32s3/rom/cache.h"

#define LINE_SIZE   16
#define BUF_SIZE    (4 * 1024 * 1024)
#define NUM_TRIALS  1000

typedef void (*probe_fn_t)(void *line, uint32_t out[2], const void *pattern);

void probe_empty(void *line, uint32_t out[2], const void *pattern);
void probe_vst(void *line, uint32_t out[2], const void *pattern);
void probe_vst_memw(void *line, uint32_t out[2], const void *pattern);
void probe_vld(void *line, uint32_t out[2], const void *pattern);
void probe_s32i(void *line, uint32_t out[2], const void *pattern);

typedef struct {
    const char *name;
    probe_fn_t fn;
    bool warm;  // line is resident (clean) in cache before the probe
} probe_t;

static const probe_t s_probes[] = {
    { "empty",         probe_empty,    false },
    { "vst_cold",      probe_vst,      false },
    { "vst_memw_cold", probe_vst_memw, false },
    { "vld_cold",      probe_vld,      false },
    { "vst_warm",      probe_vst,      true  },
    { "s32i_cold",     probe_s32i,     false },
};

#define NUM_PROBES (sizeof(s_probes) / sizeof(s_probes[0]))

static const uint8_t s_pattern[LINE_SIZE] __attribute__((aligned(LINE_SIZE))) = {
    0xa5, 0x5a, 0xc3, 0x3c, 0x0f, 0xf0, 0x96, 0x69,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};

// Internal RAM, so recording results does not disturb the PSRAM cache
static uint32_t s_op1[NUM_TRIALS];
static uint32_t s_op2[NUM_TRIALS];

typedef struct {
    uint32_t min;
    uint32_t med;
    uint32_t max;
} stats_t;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static stats_t calc_stats(uint32_t *v, size_t n)
{
    qsort(v, n, sizeof(v[0]), cmp_u32);
    return (stats_t) {
        .min = v[0], .med = v[n / 2], .max = v[n - 1]
    };
}

static void run_probe(const probe_t *p, uint8_t *buf, stats_t *op1, stats_t *op2)
{
    const size_t nlines = BUF_SIZE / LINE_SIZE;
    uint32_t out[2];

    for (int i = 0; i < NUM_TRIALS; i++) {
        uint8_t *line = buf + (esp_random() % nlines) * LINE_SIZE;

        // Write back all dirty lines in the buffer and invalidate it, so the target
        // line is not resident and a miss never has to evict a dirty victim
        ESP_ERROR_CHECK(esp_cache_msync(buf, BUF_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
        if (p->warm) {
            (void)*(volatile uint32_t *)line;
        }

        portDISABLE_INTERRUPTS();
        p->fn(line, out, s_pattern);
        portENABLE_INTERRUPTS();

        s_op1[i] = out[0];
        s_op2[i] = out[1];
    }

    *op1 = calc_stats(s_op1, NUM_TRIALS);
    *op2 = calc_stats(s_op2, NUM_TRIALS);
}

void app_main(void)
{
    uint32_t line_size = Cache_Get_DCache_Line_Size();
    if (line_size != LINE_SIZE) {
        printf("D-cache line size is %" PRIu32 "B, expected %dB (set CONFIG_ESP32S3_DATA_CACHE_LINE_16B)\n", line_size, LINE_SIZE);
        abort();
    }

    uint8_t *buf = heap_caps_aligned_alloc(LINE_SIZE, BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        printf("Failed to allocate %d bytes of PSRAM\n", BUF_SIZE);
        abort();
    }
    memset(buf, 0xee, BUF_SIZE);
    ESP_ERROR_CHECK(esp_cache_msync(buf, BUF_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));

    printf("PSRAM SIMD store benchmark: %d trials/probe, %dB lines, %dKB buffer @ %p\n",
           NUM_TRIALS, LINE_SIZE, BUF_SIZE / 1024, buf);

    stats_t op1[NUM_PROBES];
    stats_t op2[NUM_PROBES];
    for (int i = 0; i < NUM_PROBES; i++) {
        run_probe(&s_probes[i], buf, &op1[i], &op2[i]);
    }

    // s_probes[0] is the empty probe: its median is the CCOUNT read overhead
    const uint32_t overhead = op1[0].med;

    printf("\n%-14s %22s %22s %18s\n", "probe", "op1 min/med/max", "op2 min/med/max", "med - overhead");
    for (int i = 0; i < NUM_PROBES; i++) {
        printf("%-14s %6" PRIu32 " /%6" PRIu32 " /%6" PRIu32 " %6" PRIu32 " /%6" PRIu32 " /%6" PRIu32 " %8" PRId32 " /%8" PRId32 "\n",
               s_probes[i].name,
               op1[i].min, op1[i].med, op1[i].max,
               op2[i].min, op2[i].med, op2[i].max,
               (int32_t)(op1[i].med - overhead), (int32_t)(op2[i].med - overhead));
    }
    printf("(cycles)\n");

    heap_caps_free(buf);
}
