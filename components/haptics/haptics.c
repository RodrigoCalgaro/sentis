#include "haptics.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_FREQ_HZ     5000
#define LEDC_RESOLUTION  LEDC_TIMER_10_BIT   // duty range: 0–1023

#define DUTY_OFF         0
#define DUTY_MAX         ((1 << 10) - 1)     // 1023 → 100% intensity

#define PULSE_ON_MS      200
#define PULSE_OFF_MS     200

static volatile haptic_pattern_t s_pattern = HAPTIC_PATTERN_OFF;

static void set_duty(ledc_channel_t ch, uint32_t duty)
{
    ledc_set_duty(LEDC_MODE, ch, duty);
    ledc_update_duty(LEDC_MODE, ch);
}

// Runs in a dedicated FreeRTOS task so patterns never block the main loop.
static void haptic_task(void *arg)
{
    while (1) {
        switch (s_pattern) {

            case HAPTIC_PATTERN_PULSE_LEFT:
                set_duty(LEDC_CHANNEL_0, DUTY_MAX);
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_ON_MS));
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_OFF_MS));
                break;

            case HAPTIC_PATTERN_PULSE_RIGHT:
                set_duty(LEDC_CHANNEL_1, DUTY_MAX);
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_ON_MS));
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_OFF_MS));
                break;

            case HAPTIC_PATTERN_ALERT_BOTH:
                set_duty(LEDC_CHANNEL_0, DUTY_MAX);
                set_duty(LEDC_CHANNEL_1, DUTY_MAX);
                vTaskDelay(pdMS_TO_TICKS(50));
                break;

            case HAPTIC_PATTERN_OFF:
            default:
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
        }
    }
}

esp_err_t haptic_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch_left = {
        .channel    = LEDC_CHANNEL_0,
        .duty       = 0,
        .hpoint     = 0,
        .gpio_num   = BOARD_HAPTIC_LEFT_GPIO,
        .speed_mode = LEDC_MODE,
        .timer_sel  = LEDC_TIMER,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_left));

    ledc_channel_config_t ch_right = {
        .channel    = LEDC_CHANNEL_1,
        .duty       = 0,
        .hpoint     = 0,
        .gpio_num   = BOARD_HAPTIC_RIGHT_GPIO,
        .speed_mode = LEDC_MODE,
        .timer_sel  = LEDC_TIMER,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_right));

    xTaskCreate(haptic_task, "haptic", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void haptic_set_pattern(haptic_pattern_t pattern)
{
    s_pattern = pattern;
}
