# OtamaticClient - ESP-IDF component
#
# Uso come componente esterno:
#   set(EXTRA_COMPONENT_DIRS "<path>/OtamaticClient")

idf_component_register(
    SRCS
        "src/otamatic_client.cpp"
    INCLUDE_DIRS
        "src"
    REQUIRES
        esp_common
        app_update
        esp_http_client
        esp_hw_support
        mbedtls
        log
)
