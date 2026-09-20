/*
 * M5Stack Tab5 A164 keyboard driver (I2C 0x6D, HID mode) -> LVGL KEYPAD indev.
 * See a164_keyboard.h for wiring. Protocol per Tab5_Keyboard-I2C-Protocol-EN-V1.0.
 */
#include "a164_keyboard.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "a164_kbd";

/* --- Hardware / bus --------------------------------------------------------*/
#define A164_ADDR 0x6D
#define A164_SDA GPIO_NUM_0
#define A164_SCL GPIO_NUM_1
#define A164_FREQ_HZ 400000

/* --- Register map (addr 0x6D) ---------------------------------------------*/
#define REG_SYS 0x00       /* [0]INT_CFG [1]INT_STAT [2]EVENT_NUM [3]Brightness */
#define REG_MODE 0x10      /* [0]keyboard mode (0=Normal,1=HID,2=Char) [1]RGB   */
#define REG_HID_EVENT 0x30 /* 2 bytes: modifier, key_code (0xFF if queue empty) */
#define REG_VERSION 0xF0
#define A164_MODE_HID 0x01

/* HID modifier bits (see A164 user_hid_map.h) */
#define HID_MOD_LSHIFT 0x02
#define HID_MOD_RSHIFT 0x20

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static lv_indev_t *s_indev = NULL;
static lv_group_t *s_group = NULL;
static QueueHandle_t s_keyq = NULL;
static volatile bool s_present = false;
static volatile bool s_has_field = false; /* a text field is currently routed */
static volatile bool s_nav_mode = false;  /* true = arrows move menu focus, false = cursor/typing */

/* HID usage id -> ASCII (index by keycode 0x00..0x38), [0]=unshifted [1]=shifted.
 * Same table as the USB-HID path (esp_lvgl_port_usbhid.c); the A164 emits the
 * identical standard boot-keyboard usage ids in HID mode. */
static const char s_keycode2ascii[0x39][2] = {
    {0, 0},    {0, 0},    {0, 0},    {0, 0},    {'a', 'A'},  {'b', 'B'},  {'c', 'C'},
    {'d', 'D'}, {'e', 'E'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'},  {'i', 'I'},  {'j', 'J'},
    {'k', 'K'}, {'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'o', 'O'},  {'p', 'P'},  {'q', 'Q'},
    {'r', 'R'}, {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'},  {'w', 'W'},  {'x', 'X'},
    {'y', 'Y'}, {'z', 'Z'}, {'1', '!'}, {'2', '@'}, {'3', '#'},  {'4', '$'},  {'5', '%'},
    {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},  {'\r', '\r'}, /* 0x28 Enter */
    {0, 0},                                                                    /* 0x29 Esc */
    {'\b', '\b'},                                                              /* 0x2A Backspace */
    {'\t', '\t'},                                                              /* 0x2B Tab */
    {' ', ' '}, {'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'},  {'\\', '|'}, {'\\', '|'},
    {';', ':'}, {'\'', '"'}, {'`', '~'}, {',', '<'}, {'.', '>'}, {'/', '?'}, /* ..0x38 */
};

/* Translate an A164 HID event (modifier + keycode) into an LVGL key value.
 * Returns 0 for keys we do not map (they are simply ignored). */
static uint32_t a164_hid_to_lvkey(uint8_t keycode, uint8_t modifier)
{
    const bool shift = (modifier & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;

    switch (keycode) {
        case 0x28: /* Enter        */ return LV_KEY_ENTER;
        case 0x58: /* Keypad Enter */ return LV_KEY_ENTER;
        case 0x29: /* Esc          */ return LV_KEY_ESC;
        case 0x2A: /* Backspace    */ return LV_KEY_BACKSPACE;
        case 0x4C: /* Delete fwd   */ return LV_KEY_DEL;
        case 0x2B: /* Tab          */ return shift ? LV_KEY_PREV : LV_KEY_NEXT;
        /* Arrows: in a text field they move the cursor; in a menu they move focus
         * (LVGL navigates groups with NEXT/PREV, not raw arrow keys). */
        case 0x4F: /* Right arrow  */ return s_nav_mode ? LV_KEY_NEXT : LV_KEY_RIGHT;
        case 0x50: /* Left arrow   */ return s_nav_mode ? LV_KEY_PREV : LV_KEY_LEFT;
        case 0x51: /* Down arrow   */ return s_nav_mode ? LV_KEY_NEXT : LV_KEY_DOWN;
        case 0x52: /* Up arrow     */ return s_nav_mode ? LV_KEY_PREV : LV_KEY_UP;
        case 0x4A: /* Home         */ return LV_KEY_HOME;
        case 0x4D: /* End          */ return LV_KEY_END;
        default: break;
    }
    if (keycode >= 0x04 && keycode <= 0x38) {
        char c = s_keycode2ascii[keycode][shift ? 1 : 0];
        if (c) {
            return (uint32_t)(uint8_t)c;
        }
    }
    return 0;
}

/* LVGL KEYPAD read callback. Delivers each queued key as a PRESSED read followed
 * by a RELEASED read; continue_reading drains a burst within one LVGL cycle. */
static void a164_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    static bool releasing = false;
    static uint32_t held = 0;

    if (releasing) {
        data->key = held;
        data->state = LV_INDEV_STATE_RELEASED;
        releasing = false;
        data->continue_reading = (s_keyq && uxQueueMessagesWaiting(s_keyq) > 0);
        return;
    }

    uint32_t k = 0;
    if (s_keyq && xQueueReceive(s_keyq, &k, 0) == pdTRUE) {
        held = k;
        data->key = k;
        data->state = LV_INDEV_STATE_PRESSED;
        releasing = true;
        data->continue_reading = true; /* read again to deliver the release */
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Probe the keyboard (read version) and, if present, switch it to HID mode. */
static bool a164_probe_and_config(void)
{
    uint8_t reg = REG_VERSION;
    uint8_t ver = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &ver, 1, pdMS_TO_TICKS(50)) != ESP_OK) {
        return false;
    }
    uint8_t mode[2] = {REG_MODE, A164_MODE_HID};
    i2c_master_transmit(s_dev, mode, sizeof(mode), pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "A164 keyboard detected (fw 0x%02X); HID mode enabled", ver);
    return true;
}

static void a164_task(void *arg)
{
    LV_UNUSED(arg);
    for (;;) {
        if (!s_present) {
            /* Not attached: keep probing so docking it later just works. */
            if (a164_probe_and_config()) {
                s_present = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
        }

        uint8_t reg = REG_SYS;
        uint8_t sys[4] = {0};
        if (i2c_master_transmit_receive(s_dev, &reg, 1, sys, sizeof(sys), pdMS_TO_TICKS(50)) != ESP_OK) {
            ESP_LOGW(TAG, "A164 lost (I2C error); will re-probe");
            s_present = false;
            continue;
        }

        uint8_t event_num = sys[2]; /* queue length in the current (HID) mode */
        if (event_num) {
            ESP_LOGD(TAG, "poll: %u event(s) queued", event_num);
        }
        for (uint8_t i = 0; i < event_num && i < 32; i++) {
            uint8_t hreg = REG_HID_EVENT;
            uint8_t hid[2] = {0xFF, 0xFF};
            if (i2c_master_transmit_receive(s_dev, &hreg, 1, hid, sizeof(hid), pdMS_TO_TICKS(50)) != ESP_OK) {
                break;
            }
            uint8_t modifier = hid[0];
            uint8_t keycode = hid[1];
            if (keycode == 0x00 || keycode == 0xFF) {
                continue; /* empty / no key */
            }
            uint32_t lvkey = a164_hid_to_lvkey(keycode, modifier);
            ESP_LOGD(TAG, "key: keycode=0x%02X mod=0x%02X -> lvkey=%lu field=%d", keycode, modifier,
                     (unsigned long)lvkey, (int)s_has_field);
            if (lvkey && s_keyq) {
                xQueueSend(s_keyq, &lvkey, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

esp_err_t a164_keyboard_init(lv_display_t *disp)
{
    if (s_bus) {
        return ESP_OK; /* already initialized */
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1, /* auto-select a free I2C controller */
        .sda_io_num = A164_SDA,
        .scl_io_num = A164_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus create failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = A164_ADDR,
        .scl_speed_hz = A164_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_keyq = xQueueCreate(48, sizeof(uint32_t));
    if (!s_keyq) {
        return ESP_ERR_NO_MEM;
    }

    /* Create the LVGL keypad indev and a dedicated group. The lvgl port mutex is
     * recursive, so locking here is safe even though a164_keyboard_init() runs
     * with the LVGL task already going. */
    lvgl_port_lock(0);
    s_group = lv_group_create();
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, a164_read_cb);
    lv_indev_set_display(s_indev, disp);
    lv_indev_set_group(s_indev, s_group);
    lvgl_port_unlock();

    s_present = a164_probe_and_config();

    BaseType_t ok = xTaskCreatePinnedToCore(a164_task, "a164_kbd", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start A164 poll task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "A164 keyboard init done (present=%d)", (int)s_present);
    return ESP_OK;
}

void a164_kbd_route_to(lv_obj_t *ta)
{
    if (!s_group) {
        return;
    }
    lvgl_port_lock(0);
    lv_group_remove_all_objs(s_group);
    if (ta) {
        lv_group_add_obj(s_group, ta);
        lv_group_focus_obj(ta);
    }
    lvgl_port_unlock();
    s_has_field = (ta != NULL);
}

bool a164_kbd_present(void)
{
    return s_present;
}

void a164_kbd_set_nav_mode(bool on)
{
    s_nav_mode = on;
}

lv_group_t *a164_kbd_group(void)
{
    return s_group;
}
