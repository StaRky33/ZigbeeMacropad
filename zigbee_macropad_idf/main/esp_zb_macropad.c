#include <stdio.h>
#include <string.h>
#include "esp_zb_macropad.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "rgb_led.h"

#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile light (Router) source code.
#endif

#define TAG                 "MACROPAD"

/* --- PINS --------------------------------------------------------------- */
#define ROWS 4
#define COLS 4
#define BTN_COUNT (ROWS * COLS)

//Must be RST Pins (support deep sleep)
static const gpio_num_t ROW_PINS[ROWS] = {
    GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_4,
}; //D0, D1, D2, MTMS

//Any available Pins
//White macropad GPIO_NUM_18, GPIO_NUM_20, GPIO_NUM_19, GPIO_NUM_17,
//Black macropad GPIO_NUM_17, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_18,
static const gpio_num_t COL_PINS[COLS] = {
    GPIO_NUM_17, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_18,
}; //D10, D9, D8, D7

//Native physical button button on board can still be connected with external button
//White macropad GPIO_NUM_16
//Black macropad GPIO_NUM_9
#define BOOT_BUTTON_GPIO     GPIO_NUM_9

/* Seeed XIAO ESP32C6 RF switch (required for usable Zigbee/Wi-Fi RF) */
#define XIAO_RF_SWITCH_ENABLE_GPIO  GPIO_NUM_3   /* LOW = enable RF switch */
#define XIAO_RF_ANT_SELECT_GPIO     GPIO_NUM_14  /* LOW = internal, HIGH = u.FL */
#define XIAO_USE_EXTERNAL_ANTENNA   0

/* --- Deep sleep variables -------------------------------------- */
#define INACTIVITY_SLEEP_MS   (120 * 1000)        // 1 minute --> 20sec test
#define INACTIVITY_SLEEP_US   (INACTIVITY_SLEEP_MS * 1000ULL)

static uint64_t g_last_activity_us = 0;

/* --- Timing (ms) for local keypad -------------------------------------- */
#define BTN_POLL_INTERVAL_MS   10
#define DEBOUNCE_MS        30
#define DOUBLE_CLICK_MS   400
#define HOLD_PRESS_MS    1000

/* --- Endpoint and clusters -------------------------------------- */
#define MACROPAD_ENDPOINT            0x01

/* Custom cluster used to report button events to Z2M */
#define MACROPAD_CLUSTER_ID          0xFF00
#define MACROPAD_CMD_BUTTON_EVENT    0x00

#define MACROPAD_ATTR_BRIGHTNESS_ID    0x0A00
/* Use all channels or restrict as you like */
#define ESP_ZB_PRIMARY_CHANNEL_MASK  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/* --- Steering -------------------------------------- */
#define STEERING_RETRY_DELAY_MS  2000
#define STEERING_MAX_RETRIES     5

static uint8_t g_steering_retries = 0;

/* --- Button state machine ----------------------------------------------- */
typedef struct {
    bool raw;
    bool stable;
    bool prev_stable;
    uint64_t last_change_us;
    uint64_t press_start_us;
    uint64_t last_release_us;
    bool hold_fired;
} btn_state_t;

typedef struct {
    uint8_t idx;       // which button (0–15)
    uint64_t timestamp_us;
    bool level;        // 0 = pressed, 1 = released
} btn_evt_t;

static QueueHandle_t s_boot_evt_q = NULL;

static btn_state_t g_btn[BTN_COUNT];

/* --- Zigbee + LED state ------------------------------------------------- */
static bool g_is_joined     = false;
static bool g_blinking      = false;
static bool g_blink_on      = false;
static bool g_zb_ready      = false;

/* Brightness (0..100 from Zigbee); used for key feedback */
static uint8_t g_brightness = 100;

/* --- Helpers ------------------------------------------------------------ */
static inline uint64_t now_us(void) { return esp_timer_get_time(); }

typedef enum { ACT_NONE, ACT_SINGLE, ACT_DOUBLE, ACT_HOLD } action_t;

/* --- Forward declarations  ------------------------------------------------------ */
static const char* action_str(action_t a);
static void flash_action(action_t a, uint8_t brightness);
static void start_network_steering(uint8_t param);
static void zb_blink_step(void);
static void zb_reset_and_steer_cb(void);

/* Prototypes */
static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message);
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

/* Public helper to send a button event (call this from your key matrix code) */
void macropad_send_button_event(uint8_t button_id, action_t action);

/* ======================================================================= */
/*                          INIT GPIO                                      */
/* ======================================================================= */
static void matrix_gpio_init(void)
{
    // Rows: inputs with pull-up
    for (int r = 0; r < ROWS; ++r) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << ROW_PINS[r],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    // Columns: outputs, start HIGH
    for (int c = 0; c < COLS; ++c) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << COL_PINS[c],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(COL_PINS[c], 1); // idle high
    }
}

/* ======================================================================= */
/*                          SLEEP                                          */
/* ======================================================================= */
static uint64_t build_row_wakeup_mask(void)
{
    uint64_t mask = 0;

    for (int r = 0; r < ROWS; ++r) {
        gpio_num_t gpio = ROW_PINS[r];

        // Optional sanity check: make sure this pin *can* be used for wake
        if (!esp_sleep_is_valid_wakeup_gpio(gpio)) {
            ESP_LOGE(TAG, "GPIO %d is not a valid deep sleep wake pin!", gpio);
        }
        mask |= (1ULL << gpio);  // BIT(gpio)
    }

    return mask;
}

static void load_brightness_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("macropad", NVS_READONLY, &h) == ESP_OK) {
        uint8_t val;
        if (nvs_get_u8(h, "brightness", &val) == ESP_OK) {
            g_brightness = val;
        }
        nvs_close(h);
    }
}

void macropad_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Preparing to enter deep sleep...");

    // 1) Columns: outputs LOW so any pressed key can pull rows LOW via diodes
    for (int c = 0; c < COLS; ++c) {
        gpio_config_t col_conf = {
            .pin_bit_mask = 1ULL << COL_PINS[c],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&col_conf);

        gpio_set_level(COL_PINS[c], 0);   // keep column LOW in deep sleep

        // Optional: hold this level through deep sleep
        gpio_hold_en(COL_PINS[c]);
    }

    // 2) Rows: inputs (no interrupts), we'll rely on wakeup logic instead
    for (int r = 0; r < ROWS; ++r) {
        gpio_config_t row_conf = {
            .pin_bit_mask = 1ULL << ROW_PINS[r],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,    // keep them HIGH when idle
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&row_conf);
    }

    // 3) Enable deep sleep wake on rows, triggered when they go LOW
    uint64_t wake_mask = build_row_wakeup_mask();

    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(
        wake_mask,
        ESP_GPIO_WAKEUP_GPIO_LOW     // wake when any selected GPIO turns LOW
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable GPIO wakeup (err=0x%x)", err);
    }

    ESP_LOGI(TAG, "Entering deep sleep, wake mask=0x%llx", (unsigned long long)wake_mask);

    // Optional: log flush
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();
    // never returns
}

/* ======================================================================= */
/*                          BLINKER (ZB CONTEXT)                           */
/* ======================================================================= */
static void zb_blink_step(void)
{
    if (!g_blinking) {
        if (g_blink_on) {                  // ensure off
            rgb_led_off();
            g_blink_on = false;
        }
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
        return;
    }

    g_blink_on = !g_blink_on;
    if (g_blink_on) {
        rgb_led_set_rgb(255, 0, 0, g_brightness);
    } else {
        rgb_led_off();
    }

    /* Reschedule single instance */
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 300);
}

static void zb_stop_pairing_blink(void)
{
    g_blinking = false;
    if (g_blink_on) {
        rgb_led_off();
        g_blink_on = false;
    }
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
}

/* ======================================================================= */
/*                   FACTORY RESET + COMMISSION (ZB CTX)                   */
/* ======================================================================= */
static void start_network_steering(uint8_t param)
{
    (void)param;
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

static void zb_reset_and_steer_cb(void)
{
    ESP_LOGW(TAG, "Factory reset: clearing Zigbee NVS and restarting commissioning...");
    g_is_joined = false;
    g_blinking  = true;

    esp_zb_factory_reset();

    /* Start fresh blink loop (single instance) */
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 0);

    /* Start network steering shortly after */
    esp_zb_scheduler_alarm(start_network_steering, 0, 1500);
}

/* ======================================================================= */
/*                      BOOT BUTTON (ISR + QUEUE)                          */
/* ======================================================================= */
static void IRAM_ATTR boot_button_isr(void *arg) {
    uint32_t dummy = 1;
    xQueueSendFromISR(s_boot_evt_q, &dummy, NULL);
}

static void boot_button_task(void *arg) {
    uint32_t dummy;
    while (1) {
        if (xQueueReceive(s_boot_evt_q, &dummy, portMAX_DELAY)) {
            if (g_zb_ready) {
                esp_zb_scheduler_alarm((esp_zb_callback_t)zb_reset_and_steer_cb, 0, 0);
            } else {
                ESP_LOGW(TAG, "Zigbee not ready; ignoring button press");
            }
        }
    }
}

/* ======================================================================= */
/*                     LOCAL FEEDBACK (color fixed)                        */
/* ======================================================================= */
static const char* action_str(action_t a) {
    return (a==ACT_SINGLE) ? "single" : (a==ACT_DOUBLE) ? "double" : "hold";
}

// Your action_t should already exist
// typedef enum { ACT_NONE, ACT_SINGLE, ACT_DOUBLE, ACT_HOLD } action_t;

static void flash_action(action_t a, uint8_t brightness_percent)
{
    /* If brightness is 0, skip visible flash */
    if (brightness_percent == 0) return;

    uint8_t r = 0, g = 0, b = 0;

    /* Fixed colors per action; only brightness comes from Zigbee */
    switch (a) {
        case ACT_SINGLE:
            // Example: green
            r = 0;   g = 255; b = 0;
            break;
        case ACT_DOUBLE:
            // Example: blue
            r = 0;   g = 0;   b = 255;
            break;
        case ACT_HOLD:
            // Example: yellow (R+G)
            r = 255; g = 255; b = 0;
            break;
        default:
            break;
    }

    /* Apply color + brightness */
    rgb_led_set_rgb(r, g, b, brightness_percent);

    vTaskDelay(pdMS_TO_TICKS(150));

    /* Turn off after flash */
    rgb_led_off();
}


static uint8_t level_to_pwm(uint8_t lvl) {
    const uint8_t max = 255; // or your PWM max
    return (uint8_t)((lvl * max) / 100);
}

static void on_button_action(uint8_t index, action_t act) {
    ESP_LOGI(TAG, "Button %u -> %s (feedback=%u)", (unsigned)index, action_str(act), (unsigned)g_brightness);
    flash_action(act, level_to_pwm(g_brightness));
    macropad_send_button_event(index, act);
}

/* ======================================================================= */
/*                        BUTTONS TASK                                     */
/* ======================================================================= */
// Fill raw_states[BTN_COUNT] with true if pressed, false otherwise
static void matrix_scan(bool raw_states[BTN_COUNT])
{
    // Clear all
    for (int i = 0; i < BTN_COUNT; ++i) {
        raw_states[i] = false;
    }

    for (int c = 0; c < COLS; ++c) {
        // Set all columns HIGH
        for (int cc = 0; cc < COLS; ++cc) {
            gpio_set_level(COL_PINS[cc], 1);
        }

        // Drive current column LOW (active)
        gpio_set_level(COL_PINS[c], 0);

        // Small settle delay (a few microseconds is enough)
        esp_rom_delay_us(5);

        for (int r = 0; r < ROWS; ++r) {
            int level = gpio_get_level(ROW_PINS[r]); // 0 = pressed (active low)
            bool pressed = (level == 0);

            int idx = r * COLS + c;  // mapping row/col -> button index
            raw_states[idx] = pressed;
        }
    }

    // Return columns to idle HIGH (optional but nice)
    for (int c = 0; c < COLS; ++c) {
        gpio_set_level(COL_PINS[c], 1);
    }
}

static void btn_state_reset(btn_state_t *b)
{
    memset(b, 0, sizeof(*b));
}

static void button_task(void *arg)
{
    
    const uint64_t debounce_us      = DEBOUNCE_MS * 1000ULL;
    const uint64_t double_click_us  = DOUBLE_CLICK_MS * 1000ULL;
    const uint64_t long_press_us    = HOLD_PRESS_MS * 1000ULL;

    bool raw_states[BTN_COUNT];
    while (true) {
        if(!g_is_joined)
        {
            vTaskDelay(pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS));
            continue; // exit function if not connected to zigbee network.
        }
        uint64_t now = now_us();

        // 1. Scan whole matrix once -> raw_states[]
        matrix_scan(raw_states);
        
        // 2. Run your existing state machine per logical button
        for (int i = 0; i < BTN_COUNT; ++i) {
            bool raw = raw_states[i];    // <- instead of reading GPIO directly
            btn_state_t *b = &g_btn[i];

            // Debounce transition
            if (raw != b->stable && now - b->last_change_us > debounce_us) {
                b->prev_stable = b->stable;
                b->stable = raw;
                b->last_change_us = now;

                if (b->stable) {
                    // pressed
                    b->press_start_us = now;
                    b->hold_fired = false;
                } else {
                    // released
                    if (!b->hold_fired) {
                        if (b->last_release_us &&
                            (now - b->last_release_us < double_click_us)) {
                            on_button_action(i, ACT_DOUBLE);
                            g_last_activity_us = now;
                            b->last_release_us = 0;
                        } else {
                            b->last_release_us = now;
                        }
                    }
                }
            }

            // long press detection
            if (b->stable && !b->hold_fired &&
                now - b->press_start_us > long_press_us) {
                b->hold_fired = true;
                on_button_action(i, ACT_HOLD);
                g_last_activity_us = now;
                b->last_release_us = 0;
            }

            // single click confirmation (timeout expired)
            if (!b->stable && b->last_release_us &&
                now - b->last_release_us > double_click_us) {
                on_button_action(i, ACT_SINGLE);
                g_last_activity_us = now;
                b->last_release_us = 0;
            }
        }
        // 🔹 Inactivity check here
        if (now - g_last_activity_us > INACTIVITY_SLEEP_US) {
            ESP_LOGI(TAG, "No activity for %llu us, entering deep sleep",
                     (unsigned long long)(now - g_last_activity_us));
            macropad_enter_deep_sleep();
            // esp_deep_sleep_start() does not return
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS));
    }
}

/* ======================================================================= */
/*                      ZIGBEE SIGNAL HANDLER                              */
/* ======================================================================= */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *sig)
{
    uint32_t *p_sg_p     = sig->p_app_signal;
    esp_err_t err_status = sig->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        g_zb_ready = true;  // ✅ move here, stack is actually ready now
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            bool is_fn = esp_zb_bdb_is_factory_new();
            g_is_joined = !is_fn;
            g_blinking  = is_fn;
            g_steering_retries = 0;

            if (is_fn) {
                ESP_LOGI(TAG, "Factory new → starting steering");
                esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
                esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 0);
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Already paired, resuming");
            }
        } else {
            ESP_LOGE(TAG, "Device start failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            g_is_joined = true;
            g_steering_retries = 0;
            zb_stop_pairing_blink();
            ESP_LOGI(TAG, "Joined network successfully");
        } else {
            g_is_joined = false;
            g_steering_retries++;
            ESP_LOGW(TAG, "Steering failed (attempt %u/%u)",
                     g_steering_retries, STEERING_MAX_RETRIES);

            if (g_steering_retries < STEERING_MAX_RETRIES) {
                // ✅ Auto retry after delay
                ESP_LOGI(TAG, "Retrying in %d ms...", STEERING_RETRY_DELAY_MS);
                g_blinking = true;
                esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
                esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 0);
                esp_zb_scheduler_alarm(start_network_steering, 0,
                                       STEERING_RETRY_DELAY_MS);
            } else {
                // ✅ Give up after max retries, faster blink to signal error
                ESP_LOGE(TAG, "Max retries reached, waiting for manual reset");
                g_blinking = true;
                // You could change blink speed here to signal "give up" state
            }
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        g_is_joined = false;
        g_blinking  = true;
        g_steering_retries = 0;
        ESP_LOGW(TAG, "Left network → rejoining");
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
        esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 0);
        esp_zb_scheduler_alarm(start_network_steering, 0, 1500);
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x) status: %s",
                 esp_zb_zdo_signal_to_string(sig_type),
                 sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ======================================================================= */
/*                       ZIGBEE STACK TASK (manual Level Control)          */
/* ======================================================================= */
static void esp_zb_task(void *pv)
{
    /*---------------------------------------------------------------
     * Initialize Zigbee stack (router mode)
     *-------------------------------------------------------------*/
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    /*---------------------------------------------------------------
     * BASIC CLUSTER (mandatory)
     *-------------------------------------------------------------*/
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);

    static const char manufacturer_name[] = ESP_MANUFACTURER_NAME;
    static const char model_identifier[]  = ESP_MODEL_IDENTIFIER;

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_cluster,
                                            ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                            ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            (void *)manufacturer_name));

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_cluster,
                                            ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                            ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            (void *)model_identifier));

    /*---------------------------------------------------------------
     * IDENTIFY CLUSTER (mandatory)
     *-------------------------------------------------------------*/
    esp_zb_identify_cluster_cfg_t identify_cfg = {.identify_time = 0};
    esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);

    /*---------------------------------------------------------------
     * CUSTOM MACROPAD CLUSTER 
     *-------------------------------------------------------------*/
    esp_zb_attribute_list_t *macropad_cluster =
    esp_zb_zcl_attr_list_create(MACROPAD_CLUSTER_ID);

    /*---------------------------------------------------------------
     * BRIGHTNESS CLUSTER 
     *-------------------------------------------------------------*/
    // 0x0A00 – brightness color
    esp_zb_custom_cluster_add_custom_attr(
        macropad_cluster,
        MACROPAD_ATTR_BRIGHTNESS_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &g_brightness);

    /*---------------------------------------------------------------
     * CLUSTER LIST (Basic + Identify + Macropad)
     *-------------------------------------------------------------*/
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(cluster_list ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cluster_list,
                                                          basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cluster_list,
                                                             identify_cluster,
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster( cluster_list,
                                                            macropad_cluster,
                                                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /*---------------------------------------------------------------
     * ENDPOINT CONFIGURATION
     *-------------------------------------------------------------*/
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = MACROPAD_ENDPOINT,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    ESP_ERROR_CHECK(ep_list ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg));

    /*---------------------------------------------------------------
     * REGISTER DEVICE + CALLBACKS
     *-------------------------------------------------------------*/
    esp_zb_device_register(ep_list);

    /* 3. Register Zigbee core action handler (for attribute writes, custom commands, etc.) */
    esp_zb_core_action_handler_register(zb_action_handler);
    
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));

    

    g_zb_ready = true;
    esp_zb_stack_main_loop();
}

/* ======================================================================= */
/*                       ZIGBEE ATTRIUBUTE HANDLER                         */
/* ======================================================================= */
static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty ZCL set_attr message");

    if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "set_attr: error status=%d", message->info.status);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t  endpoint   = message->info.dst_endpoint;
    uint16_t cluster_id = message->info.cluster;
    uint16_t attr_id    = message->attribute.id;

    ESP_LOGI(TAG,
             "set_attr: ep=%u cluster=0x%04X attr=0x%04X size=%d",
             endpoint,
             cluster_id,
             attr_id,
             message->attribute.data.size);

    switch (cluster_id) {
    case ESP_ZB_ZCL_CLUSTER_ID_BASIC:
        /* Handle writes to Basic cluster if needed (e.g. IdentifyTime) */
        break;

    case MACROPAD_CLUSTER_ID:
        if (attr_id == MACROPAD_ATTR_BRIGHTNESS_ID &&
            message->attribute.data.size == sizeof(uint8_t)) {

            uint8_t new_val = *(uint8_t *)message->attribute.data.value;
            g_brightness = new_val;
            // Persist to NVS
            nvs_handle_t h;
            if (nvs_open("macropad", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "brightness", g_brightness);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "Brightness updated to %u", g_brightness);
        }
        break;
    
    default:
        /* Unknown or unhandled cluster */
        break;
    }

    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) 
{
    esp_err_t ret = ESP_OK;

    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler(
            (const esp_zb_zcl_set_attr_value_message_t *)message);
        break;

    /* You can add more cases later:
     *  - ESP_ZB_CORE_REPORT_ATTR_CB_ID
     *  - ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID
     *  etc.
     */
    default:
        ESP_LOGW(TAG, "Unhandled Zigbee action 0x%x", callback_id);
        break;
    }

    return ret;
}

void macropad_send_button_event(uint8_t button_id, action_t action)
{
    uint8_t payload[2] = { button_id, (uint8_t)action};

    esp_zb_zcl_custom_cluster_cmd_req_t req = {0};

    req.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;  // coordinator
    req.zcl_basic_cmd.dst_endpoint          = MACROPAD_ENDPOINT;
    req.zcl_basic_cmd.src_endpoint          = MACROPAD_ENDPOINT;

    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.cluster_id   = MACROPAD_CLUSTER_ID;
    req.profile_id   = ESP_ZB_AF_HA_PROFILE_ID;
    req.direction    = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    req.custom_cmd_id = MACROPAD_CMD_BUTTON_EVENT;

    req.data.type  = ESP_ZB_ZCL_ATTR_TYPE_SET;
    req.data.size  = sizeof(payload);
    req.data.value = payload;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_custom_cluster_cmd_req(&req);
    esp_zb_lock_release();

    //ESP_EARLY_LOGI(TAG, "Sent button event: button=%u action=%u", button_id, action);
}

/* ======================================================================= */
/*                                MAIN                                     */
/* ======================================================================= */
void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    
    // Clear holds so we can reconfigure pins
    for (int c = 0; c < COLS; ++c) {
        gpio_hold_dis(COL_PINS[c]);
    }

    matrix_gpio_init();
    // Initialise button state array
    for (int i = 0; i < BTN_COUNT; ++i) {
        btn_state_reset(&g_btn[i]);
    }
    g_last_activity_us = now_us();
    
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        // We woke because some row went LOW (button pressed).
        // Try to see which button(s) are still pressed *right now*.
        bool raw_states[BTN_COUNT];
        matrix_scan(raw_states);

        for (int i = 0; i < BTN_COUNT; ++i) {
            if (raw_states[i]) {
                btn_state_t *b = &g_btn[i];

                // Pretend this button is currently pressed
                b->stable         = true;
                b->prev_stable    = false;
                b->hold_fired     = false;
                b->press_start_us = g_last_activity_us;
                b->last_change_us = g_last_activity_us;
                b->last_release_us = 0;

                // You could log it for debugging:
                ESP_LOGI(TAG, "Woke with button %d pressed", i);
            }
        }
    }
    nvs_flash_init();
    load_brightness_from_nvs();

    ESP_LOGI(TAG, "Starting 16-button macropad");
    
    // --- Configure BOOT button input ---
    gpio_config_t btnio = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&btnio));

    // --- Create queue ---
    s_boot_evt_q = xQueueCreate(4, sizeof(uint32_t));

    // --- Install ISR service ONCE ---
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_button_isr, NULL);

    // --- Launch tasks ---
    xTaskCreate(button_task, "button_task", 4096, NULL, 1, NULL);
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 1, NULL);

    /* --- XIAO ESP32C6 antenna / RF switch -------------------------------- */
    {
        gpio_config_t rfio = {
            .pin_bit_mask = (1ULL << XIAO_RF_SWITCH_ENABLE_GPIO) |
                            (1ULL << XIAO_RF_ANT_SELECT_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rfio));
        gpio_set_level(XIAO_RF_SWITCH_ENABLE_GPIO, 0); /* enable RF switch */
        gpio_set_level(XIAO_RF_ANT_SELECT_GPIO, XIAO_USE_EXTERNAL_ANTENNA ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "XIAO RF switch enabled (%s antenna)",
                 XIAO_USE_EXTERNAL_ANTENNA ? "external" : "internal");
    }

    /* --- ZIGBEE --------------------------------------------------------- */
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    xTaskCreate(esp_zb_task, "ZB_main", 8192, NULL, 5, NULL);        
    
    ESP_LOGI(TAG, "Ready.");
}
