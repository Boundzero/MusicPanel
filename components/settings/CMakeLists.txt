idf_component_register(
    SRCS "src/settings.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES logging
)
