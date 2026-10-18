/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * Timed probes for the PSRAM store benchmark.
 *
 * void probe_xxx(void *line, uint32_t out[2], const void *pattern);
 *   a2 = 16B-aligned target line, a3 = out, a4 = 16B-aligned pattern (internal RAM)
 *
 * Timed window: rsr.ccount / OP / rsr.ccount / OP / rsr.ccount
 * Both OPs hit the same line. out[0] = t1 - t0, out[1] = t2 - t1.
 */

    /* IRAM, so instruction fetch never misses inside the timed window */
    .section .iram1, "ax"

/* Preload q0 from the pattern (outside the timed window) and drain memory ops. */
.macro PROBE_PROLOGUE
    entry       a1, 32
    ee.vld.128.ip q0, a4, 0
    l32i        a7, a4, 0
    memw
.endm

.macro PROBE_EPILOGUE
    sub         a8, a5, a4
    sub         a9, a6, a5
    s32i        a8, a3, 0
    s32i        a9, a3, 4
    retw
.endm

.macro DEFINE_PROBE name, op:vararg
    .global     \name
    .type       \name, @function
    .align      4
\name:
    PROBE_PROLOGUE
    rsr.ccount  a4
    \op
    rsr.ccount  a5
    \op
    rsr.ccount  a6
    PROBE_EPILOGUE
    .size       \name, . - \name
.endm

/* No OP: CCOUNT read overhead */
DEFINE_PROBE probe_empty
/* 16B SIMD store */
DEFINE_PROBE probe_vst, ee.vst.128.ip q0, a2, 0
/* 16B SIMD load */
DEFINE_PROBE probe_vld, ee.vld.128.ip q1, a2, 0
/* 32-bit store */
DEFINE_PROBE probe_s32i, s32i a7, a2, 0

/* 16B SIMD store followed by memw, so the store must complete before CCOUNT is read */
    .global     probe_vst_memw
    .type       probe_vst_memw, @function
    .align      4
probe_vst_memw:
    PROBE_PROLOGUE
    rsr.ccount  a4
    ee.vst.128.ip q0, a2, 0
    memw
    rsr.ccount  a5
    ee.vst.128.ip q0, a2, 0
    memw
    rsr.ccount  a6
    PROBE_EPILOGUE
    .size       probe_vst_memw, . - probe_vst_memw
