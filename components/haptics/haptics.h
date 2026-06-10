#pragma once

#include "esp_err.h"

// Vibration patterns for the two coin motors.
// PULSE_LEFT / PULSE_RIGHT: intermittent, used for directional danger alerts.
// ALERT_BOTH:               continuous at full intensity, used for frontal obstacle.
typedef enum {
    HAPTIC_PATTERN_OFF = 0,
    HAPTIC_PATTERN_PULSE_LEFT,
    HAPTIC_PATTERN_PULSE_RIGHT,
    HAPTIC_PATTERN_ALERT_BOTH,
} haptic_pattern_t;

// Initialize LEDC PWM channels and start the background pattern task.
esp_err_t haptic_init(void);

// Change the active vibration pattern. Safe to call from any task.
void haptic_set_pattern(haptic_pattern_t pattern);
