idf_component_register(
    SRCS "main.cc"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_i2c esp_wifi esp_event nvs_flash esp_http_client mbedtls esp_audio_codec
)
