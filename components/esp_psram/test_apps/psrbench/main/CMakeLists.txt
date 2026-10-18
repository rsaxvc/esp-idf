idf_component_register(SRCS "psrbench.c" "psrbench_probe.S"
                       PRIV_REQUIRES esp_psram esp_mm esp_rom esp_hw_support heap)
