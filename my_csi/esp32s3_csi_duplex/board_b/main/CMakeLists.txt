idf_component_register(
    SRCS "esp32s3_csi_duplex.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_system
)
