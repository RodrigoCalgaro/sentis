idf_component_register(
    SRCS "sentis.c"
    INCLUDE_DIRS "."
    REQUIRES haptics lidar vision monitor freertos board
)
