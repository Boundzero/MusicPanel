idf_component_register(
    SRCS "src/ma.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json
    PRIV_REQUIRES settings network logging esp_timer esp_jpeg
)
