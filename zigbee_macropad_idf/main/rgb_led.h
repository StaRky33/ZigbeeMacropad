#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the RGB LED driver.
 *
 * Safe to call multiple times; later calls are no-ops.
 */
esp_err_t rgb_led_init(void);

/**
 * @brief Set RGB LED color with global brightness.
 *
 * @param r   Red   component (0–255)
 * @param g   Green component (0–255)
 * @param b   Blue  component (0–255)
 * @param brightness_percent Global brightness (0–100).
 *                           0 turns the LED completely off.
 */
void rgb_led_set_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness_percent);

/**
 * @brief Turn the RGB LED completely off.
 */
void rgb_led_off(void);

#ifdef __cplusplus
}
#endif
