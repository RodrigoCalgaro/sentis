idf_component_register(
    SRCS "sentis.c"
    INCLUDE_DIRS "."
    REQUIRES haptics lidar vision monitor storage audio freertos board
)
