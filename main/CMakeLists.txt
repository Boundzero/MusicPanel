idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES bsp logging settings network ma theme timesync weather
)
