#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"

void app_main(void)
{
    haptic_init();

    // Test sequence: cycle through all patterns to verify physical wiring.
    // Remove this block once motors are confirmed working.
    haptic_set_pattern(HAPTIC_PATTERN_PULSE_LEFT);
    vTaskDelay(pdMS_TO_TICKS(3000));

    haptic_set_pattern(HAPTIC_PATTERN_PULSE_RIGHT);
    vTaskDelay(pdMS_TO_TICKS(3000));

    haptic_set_pattern(HAPTIC_PATTERN_ALERT_BOTH);
    vTaskDelay(pdMS_TO_TICKS(3000));

    haptic_set_pattern(HAPTIC_PATTERN_OFF);
}
