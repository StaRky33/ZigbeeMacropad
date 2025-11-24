/*
 * Zigbee HA_color_dimmable_light Example (ESP32-C6)
 * -------------------------------------------------
 * Simplified for MACROPAD:
 *  - Only Zigbee Level Control / Current Level is honored.
 *  - That brightness is used for the short LED feedback when a key is pressed.
 *  - Color & On/Off writes are ignored.
 *  - Pairing still blinks RED while not joined.
 */

#include <stdio.h>
#include <string.h>
#include "esp_zb_macropad.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_timer.h"

#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile light (Router) source code.
#endif

#define TAG                 "MACROPAD"

/* --- PINS --------------------------------------------------------------- */
#define ROWS 4
#define COLS 4
#define BTN_COUNT (ROWS * COLS)

static const gpio_num_t COL_PINS[ROWS] = {
    GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5,
};

static const gpio_num_t ROW_PINS[COLS] = {
    GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_21, GPIO_NUM_22,
};

#define BOOT_BUTTON_GPIO     GPIO_NUM_9

/* --- Timing (ms) for local keypad -------------------------------------- */
#define BTN_POLL_INTERVAL_MS   10
#define DEBOUNCE_MS        30
#define DOUBLE_CLICK_MS   400
#define HOLD_PRESS_MS    1000

/* --- Endpoint and clusters -------------------------------------- */
#define MACROPAD_ENDPOINT            0x01

/* Custom cluster used to report button events to Z2M */
#define MACROPAD_CLUSTER_ID          0xFC00
#define MACROPAD_CMD_BUTTON_EVENT    0x00

/* Use all channels or restrict as you like */
#define ESP_ZB_PRIMARY_CHANNEL_MASK  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

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
static bool g_driver_ready  = false;
static bool g_blink_on      = false;
static bool g_zb_ready      = false;

/* Brightness (0..254 from Zigbee Level Control); used for key feedback */
static uint8_t g_feedback_level = 80;

/* --- Helpers ------------------------------------------------------------ */
static inline uint64_t now_us(void) { return esp_timer_get_time(); }
static inline uint32_t us_to_ms(uint64_t us) { return (uint32_t)(us / 1000ULL); }

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
/*                          BLINKER (ZB CONTEXT)                           */
/* ======================================================================= */
static void zb_blink_step(void)
{
    if (!g_driver_ready) {
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
        esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 500);
        return;
    }

    if (!g_blinking) {
        if (g_blink_on) {                  // ensure off
            light_driver_set_level(0);
            light_driver_set_power(false);
            g_blink_on = false;
        }
        esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
        return;
    }

    g_blink_on = !g_blink_on;
    if (g_blink_on) {
        light_driver_set_color_xy(0xA3D6, 0x547B);  // red
        //green : Color X: 0x4ccd  Color Y: 0x9999
        //blue : Color X: 0x2666  Color Y: 0x0f5c
        //yellow : Color X: 0x6b58  Color Y: 0x8157
        light_driver_set_power(true);
        light_driver_set_level(100);   // adjust if you want dimmer pairing
    } else {
        light_driver_set_level(0);
    }

    /* Reschedule single instance */
    esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
    esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 300);
}

static void zb_stop_pairing_blink(void)
{
    g_blinking = false;
    if (g_blink_on) {
        light_driver_set_level(0);
        light_driver_set_power(false);
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

static void flash_action(action_t a, uint8_t brightness) {
    /* If brightness is 0, skip visible flash */
    if (brightness == 0) return;

    /* Fixed colors per action; only brightness comes from Zigbee */
    switch(a){
        case ACT_SINGLE: light_driver_set_color_xy(0x4ccd, 0x9999);break;  // green
        case ACT_DOUBLE: light_driver_set_color_xy(0x2666, 0x0f5c);break;  // blue
        case ACT_HOLD: light_driver_set_color_xy(0x6b58, 0x8157);break;  // yellow
        default: break;
    }
    light_driver_set_power(true);
    light_driver_set_level(brightness);   // adjust if you want dimmer pairing
    vTaskDelay(pdMS_TO_TICKS(150));
    light_driver_set_power(false);
}

static void on_button_action(uint8_t index, action_t act) {
    ESP_LOGI(TAG, "Button %u -> %s (feedback=%u)", (unsigned)index+1, action_str(act), (unsigned)g_feedback_level);
    flash_action(act, g_feedback_level);
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

static void button_task(void *arg)
{
    const uint64_t debounce_us      = DEBOUNCE_MS * 1000ULL;
    const uint64_t double_click_us  = DOUBLE_CLICK_MS * 1000ULL;
    const uint64_t long_press_us    = HOLD_PRESS_MS * 1000ULL;

    bool raw_states[BTN_COUNT];

    while (true) {
        uint64_t now = esp_timer_get_time();

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
                b->last_release_us = 0;
            }

            // single click confirmation (timeout expired)
            if (!b->stable && b->last_release_us &&
                now - b->last_release_us > double_click_us) {
                on_button_action(i, ACT_SINGLE);
                b->last_release_us = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS));
    }
}

/* ======================================================================= */
/*                  DEFERRED LIGHT DRIVER INITIALIZATION                   */
/* ======================================================================= */
static esp_err_t deferred_driver_init(void)
{
    static bool inited = false;
    if (!inited) {
        light_driver_init(LIGHT_DEFAULT_OFF);
        g_driver_ready = true;
        ESP_LOGI(TAG, "Light driver initialized");
        inited = true;
    }
    return inited ? ESP_OK : ESP_FAIL;
}

/* ======================================================================= */
/*                      ZIGBEE SIGNAL HANDLER                              */
/* ======================================================================= */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *sig)
{
    uint32_t *p_sg_p    = sig->p_app_signal;
    esp_err_t err_status = sig->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            deferred_driver_init();
            bool is_fn = esp_zb_bdb_is_factory_new();
            g_is_joined = !is_fn;
            g_blinking  = is_fn;
            ESP_LOGI(TAG, "Device %s factory new", is_fn ? "is" : "is not");

            if (is_fn) {
                esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
                esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 0);
                ESP_LOGI(TAG, "Starting network steering...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            g_is_joined = true;
            g_blinking  = false;  // stop blinking
            zb_stop_pairing_blink();
            ESP_LOGI(TAG, "Joined network successfully");
        } else {
            g_is_joined = false;
            g_blinking  = true;   // retry blink
            ESP_LOGI(TAG, "Steering failed → retry & blink");
            esp_zb_scheduler_alarm_cancel((esp_zb_callback_t)zb_blink_step, 0);
            esp_zb_scheduler_alarm((esp_zb_callback_t)zb_blink_step, 0, 0);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        g_is_joined = false;
        g_blinking  = true;
        ESP_LOGW(TAG, "Left network → rejoining and blinking");
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
                                                            ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));

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
        /* If you add config attributes (e.g. per-key mode) to your macropad cluster,
         * decode them here from message->attribute.data.value
         */
        break;

    default:
        /* Unknown or unhandled cluster */
        break;
    }

    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
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

    ESP_EARLY_LOGI(TAG, "Sent button event: button=%u action=%u", button_id, action);
}

/* ======================================================================= */
/*                                MAIN                                     */
/* ======================================================================= */
void app_main(void)
{
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

    matrix_gpio_init();

    // --- Launch tasks ---
    xTaskCreate(button_task, "button_task", 4096, NULL, 1, NULL);
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 1, NULL);

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
