| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# PSRAM SIMD store benchmark

Measures whether a cacheline-sized (16B) SIMD store (`ee.vst.128.ip`) to PSRAM that misses the D-cache
allocates the line directly, or reads the line from PSRAM before overwriting it.

The D-cache is configured with 16B lines (`sdkconfig.defaults`, octal PSRAM). For each trial a random,
16B-aligned line in a 4MB PSRAM buffer is chosen, the buffer is written back and invalidated with
`esp_cache_msync()`, and then with interrupts disabled the probe runs:

    rsr.ccount / OP / rsr.ccount / OP / rsr.ccount

Both OPs target the same line. `op1` is the first OP (the miss), `op2` the second (the hit, or a stall
behind the first).

| probe           | OP                          | line state before |
| --------------- | --------------------------- | ----------------- |
| `empty`         | none                        | n/a, CCOUNT read overhead |
| `vst_cold`      | `ee.vst.128.ip`             | not in cache      |
| `vst_memw_cold` | `ee.vst.128.ip` + `memw`    | not in cache      |
| `vld_cold`      | `ee.vld.128.ip`             | not in cache (line fill cost) |
| `vst_warm`      | `ee.vst.128.ip`             | in cache, clean (hit cost) |
| `s32i_cold`     | `s32i`                      | not in cache (partial store, must fetch) |

## Reading the results

- `vst_cold` costs about as much as `vld_cold` (in `op1`, `op2`, or the `memw` variant): the store **reads the line first**.
- `vst_cold` costs about as much as `vst_warm`, and `vst_memw_cold` is well below `vld_cold`: the line is allocated **without a fetch**.
- `s32i_cold` should look like the fetch case.

## Running

    idf.py set-target esp32s3
    idf.py -p PORT flash monitor
