idf_component_register(
    SRCS "src/maws.cpp"
    INCLUDE_DIRS "include"
    REQUIRES settings network ma esp_websocket_client
)
