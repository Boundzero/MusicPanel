idf_component_register(
    SRCS "src/network.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif
    PRIV_REQUIRES nvs_flash esp_http_server settings logging
)
