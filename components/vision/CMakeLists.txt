idf_component_register(
    SRCS "vision.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_cam espressif__esp_cam_sensor espressif__esp_sccb_intf esp_driver_i2c esp_hw_support esp_driver_isp bus board freertos
)
