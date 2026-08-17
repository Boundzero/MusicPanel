idf_component_register(
    SRCS "src/weather.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES settings network logging esp_http_client json mbedtls
)
