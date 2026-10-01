/* BLE surveillance/tracker Detector screen. Runs the C5 ble_detect scan and lists
 * classified [BLE_DETECT] devices: AirTag/Find-My trackers, Flock ALPR cameras,
 * Meta glasses, Flipper Zero. Defensive anti-stalk / counter-surveillance. */
#include "subghz_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_detect";
#define RXBUF 1024
#define LINEBUF 1024
#define MAX_ROWS 20

static lv_obj_t   *s_page = NULL;
static lv_obj_t   *s_list = NULL;
static lv_obj_t   *s_status = NULL;
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_alive = false;
static int s_tab_id = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_rows[MAX_ROWS][80];
static char s_types[MAX_ROWS][12];
static volatile int  s_nrows = 0;
static volatile bool s_dirty = false;

static void process_line(const char *line)
{
    const char *p = strstr(line, "[BLE_DETECT] ");
    if (!p) return;
    char type[12] = "?", mac[20] = "?", name[40] = "-"; int rssi = 0;
    sscanf(p, "[BLE_DETECT] type=%11s mac=%19s rssi=%d name=%39[^\n]", type, mac, &rssi, name);
    char row[80];
    snprintf(row, sizeof(row), "%-7s %s  %ddBm  %s", type, mac, rssi, name);
    portENTER_CRITICAL(&s_lock);
    if (s_nrows < MAX_ROWS) {
        snprintf(s_rows[s_nrows], sizeof(s_rows[s_nrows]), "%s", row);
        snprintf(s_types[s_nrows], sizeof(s_types[s_nrows]), "%s", type);
        s_nrows++; s_dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void reader_task(void *arg)
{
    (void)arg;
    static char rx[RXBUF], line[LINEBUF];
    int lp = 0;
    while (s_alive) {
        int len = subghz_host_uart_read_bytes(s_tab_id, rx, sizeof(rx) - 1, pdMS_TO_TICKS(100));
        if (len <= 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        rx[len] = '\0';
        for (int i = 0; i < len; i++) {
            char c = rx[i];
            if (c == '\n' || c == '\r') { if (lp > 0) { line[lp] = '\0'; process_line(line); lp = 0; } }
            else if (lp < LINEBUF - 1) line[lp++] = c;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static lv_color_t type_color(const char *t)
{
    if (strcmp(t, "flock") == 0)   return subghz_host_color_red();
    if (strcmp(t, "meta") == 0)    return subghz_host_color_orange();
    if (strcmp(t, "flipper") == 0) return subghz_host_color_green();
    return subghz_host_color_amber();   /* airtag / findmy */
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_dirty || !s_list) return;
    char rows[MAX_ROWS][80]; char types[MAX_ROWS][12]; int n;
    portENTER_CRITICAL(&s_lock);
    n = s_nrows;
    memcpy(rows, s_rows, sizeof(char) * 80 * (n < MAX_ROWS ? n : MAX_ROWS));
    memcpy(types, s_types, sizeof(char) * 12 * (n < MAX_ROWS ? n : MAX_ROWS));
    s_dirty = false;
    portEXIT_CRITICAL(&s_lock);
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, rows[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(l, type_color(types[i]), 0);
    }
    if (s_status) lv_label_set_text_fmt(s_status, "Scanning... %d device(s) flagged", n);
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_list = NULL; s_status = NULL;
    portENTER_CRITICAL(&s_lock); s_nrows = 0; portEXIT_CRITICAL(&s_lock);
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }

void show_ble_detect_page(void)
{
    lv_obj_t *container = subghz_host_current_container();
    if (!container) return;
    subghz_host_hide_all_pages();
    cleanup();   /* tear down any prior instance (timer/task) on re-entry */

    s_page = lv_obj_create(container);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, subghz_host_ui_bg(), 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 10, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page, 8, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(s_page);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(header);
    lv_obj_set_style_bg_color(back, subghz_host_ui_card(), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "  BLE Detectors  (trackers / cameras)");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_red(), 0);

    s_status = lv_label_create(s_page);
    lv_label_set_text(s_status, "Scanning for AirTag / Find My / Flock / Meta / Flipper...");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status, subghz_host_ui_muted(), 0);

    s_list = lv_obj_create(s_page);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_color(s_list, subghz_host_ui_panel(), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 10, 0);
    lv_obj_set_style_pad_all(s_list, 10, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    s_tab_id = subghz_host_current_tab();
    s_alive = true; s_dirty = false;
    portENTER_CRITICAL(&s_lock); s_nrows = 0; portEXIT_CRITICAL(&s_lock);
    xTaskCreate(reader_task, "ble_det_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 200, NULL);
    subghz_host_uart_flush_input(s_tab_id);
    subghz_host_uart_send("ble_detect");
    ESP_LOGI(TAG, "BLE detector page ready");
}
