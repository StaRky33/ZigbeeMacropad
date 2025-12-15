#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "RGB_LED";

/* GPIOs for RGB LED (common cathode, pin to GND) */
#define RGB_LED_R_GPIO   GPIO_NUM_21 // D3
#define RGB_LED_G_GPIO   GPIO_NUM_22 // D4
#define RGB_LED_B_GPIO   GPIO_NUM_23 // D5

/* LEDC config */
#define RGB_LED_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define RGB_LED_LEDC_TIMER     LEDC_TIMER_0
#define RGB_LED_LEDC_FREQ_HZ   5000
#define RGB_LED_LEDC_RES       LEDC_TIMER_8_BIT  // 0..255
#define RGB_LED_DUTY_MAX       ((1 << 8) - 1)

/* Channels for R, G, B */
#define RGB_LED_CH_R   LEDC_CHANNEL_0
#define RGB_LED_CH_G   LEDC_CHANNEL_1
#define RGB_LED_CH_B   LEDC_CHANNEL_2

static bool s_rgb_led_inited = false;

esp_err_t rgb_led_init(void)
{
    if (s_rgb_led_inited) {
        return ESP_OK;
    }

    /* Timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode       = RGB_LED_LEDC_MODE,
        .duty_resolution  = RGB_LED_LEDC_RES,
        .timer_num        = RGB_LED_LEDC_TIMER,
        .freq_hz          = RGB_LED_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* Channels */
    ledc_channel_config_t ch_cfg = {
        .speed_mode     = RGB_LED_LEDC_MODE,
        .timer_sel      = RGB_LED_LEDC_TIMER,
        .duty           = 0,
        .hpoint         = 0,
        .intr_type      = LEDC_INTR_DISABLE,
    };

    ch_cfg.channel = RGB_LED_CH_R;
    ch_cfg.gpio_num = RGB_LED_R_GPIO;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ch_cfg.channel = RGB_LED_CH_G;
    ch_cfg.gpio_num = RGB_LED_G_GPIO;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ch_cfg.channel = RGB_LED_CH_B;
    ch_cfg.gpio_num = RGB_LED_B_GPIO;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    s_rgb_led_inited = true;
    ESP_LOGI(TAG, "RGB LED initialized on R=%d G=%d B=%d",
             RGB_LED_R_GPIO, RGB_LED_G_GPIO, RGB_LED_B_GPIO);
    return ESP_OK;
}

/* brightness_percent: 0..100
 * r/g/b: 0..255
 */
void rgb_led_set_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness_percent)
{
    if (!s_rgb_led_inited) {
        rgb_led_init();
    }

    if (brightness_percent == 0) {
        r = g = b = 0;
    }

    /* Scale by brightness and PWM resolution */
    uint32_t scale = (uint32_t)brightness_percent;
    uint32_t duty_r = (uint32_t)r * scale * RGB_LED_DUTY_MAX / (255U * 100U);
    uint32_t duty_g = (uint32_t)g * scale * RGB_LED_DUTY_MAX / (255U * 100U);
    uint32_t duty_b = (uint32_t)b * scale * RGB_LED_DUTY_MAX / (255U * 100U);

    ESP_ERROR_CHECK(ledc_set_duty(RGB_LED_LEDC_MODE, RGB_LED_CH_R, duty_r));
    ESP_ERROR_CHECK(ledc_update_duty(RGB_LED_LEDC_MODE, RGB_LED_CH_R));

    ESP_ERROR_CHECK(ledc_set_duty(RGB_LED_LEDC_MODE, RGB_LED_CH_G, duty_g));
    ESP_ERROR_CHECK(ledc_update_duty(RGB_LED_LEDC_MODE, RGB_LED_CH_G));

    ESP_ERROR_CHECK(ledc_set_duty(RGB_LED_LEDC_MODE, RGB_LED_CH_B, duty_b));
    ESP_ERROR_CHECK(ledc_update_duty(RGB_LED_LEDC_MODE, RGB_LED_CH_B));
}

/* Convenience: turn LED fully off */
void rgb_led_off(void)
{
    rgb_led_set_rgb(0, 0, 0, 0);
}

