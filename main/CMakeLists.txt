idf_component_register(
    SRCS "main.cc"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_event nvs_flash esp_http_client mbedtls esp_audio_codec
)
