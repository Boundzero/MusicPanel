idf_component_register(
    SRCS "src/timesync.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES settings logging esp_netif lwip
)
