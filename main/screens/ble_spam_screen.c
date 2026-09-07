/* BLE advertising spam screen. Selects a target ecosystem and toggles the C5's
 * native-BLE spammer (ble_spam <type> / stop), showing the sent counter from
 * [BLE_SPAM] tokens. Reachable from the Radios submenu. Authorized/lab use. */
#include "subghz_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_spam";
#define RXBUF 1024
#define LINEBUF 1024

static const char *TYPES[] = {"apple","samsung","google","windows","all"};
#define NTYPES 5

static lv_obj_t   *s_page = NULL;
static lv_obj_t   *s_status = NULL;
static lv_obj_t   *s_big_btn = NULL;
static lv_obj_t   *s_big_lbl = NULL;
static lv_obj_t   *s_type_btns[NTYPES] = {NULL};
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_alive = false;
static int s_tab_id = 0;
static int s_type = 0;
static volatile bool s_running = false;
static volatile int  s_sent = 0;
static volatile bool s_dirty = false;

static void process_line(const char *line)
{
    const char *p;
    if ((p = strstr(line, "[BLE_SPAM] sent="))) { sscanf(p, "[BLE_SPAM] sent=%d", (int*)&s_sent); s_dirty = true; }
    else if (strstr(line, "[BLE_SPAM_START]")) { s_running = true; s_dirty = true; }
    else if (strstr(line, "[BLE_SPAM_STOP]"))  { s_running = false; s_dirty = true; }
    else if (strstr(line, "[BLE_SPAM_ERR]"))   { s_running = false; s_dirty = true; }
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

static void refresh_big(void)
{
    if (!s_big_btn) return;
    lv_obj_set_style_bg_color(s_big_btn, s_running ? subghz_host_color_red() : subghz_host_color_green(), 0);
    if (s_big_lbl) lv_label_set_text(s_big_lbl, s_running ? "STOP" : "START");
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_dirty) return;
    s_dirty = false;
    if (s_status) {
        if (s_running) lv_label_set_text_fmt(s_status, "Spamming %s  -  %d adv sent", TYPES[s_type], s_sent);
        else           lv_label_set_text(s_status, "Idle");
    }
    refresh_big();
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_status = NULL; s_big_btn = NULL; s_big_lbl = NULL;
    for (int i = 0; i < NTYPES; i++) s_type_btns[i] = NULL;
    s_running = false;
}

static void style_type_btn(int i)
{
    if (!s_type_btns[i]) return;
    lv_obj_set_style_bg_color(s_type_btns[i], i == s_type ? subghz_host_color_purple() : subghz_host_ui_card(), 0);
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }

static void on_type(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= NTYPES) return;
    s_type = i;
    for (int k = 0; k < NTYPES; k++) style_type_btn(k);
}

static void on_big(lv_event_t *e)
{
    (void)e;
    if (s_running) { subghz_host_uart_send("stop"); s_running = false; }
    else {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "ble_spam %s", TYPES[s_type]);
        s_sent = 0;
        subghz_host_uart_send(cmd);
        s_running = true;
    }
    s_dirty = true;
}

void show_ble_spam_page(void)
{
    lv_obj_t *container = subghz_host_current_container();
    if (!container) return;
    subghz_host_hide_all_pages();
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }

    s_page = lv_obj_create(container);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, subghz_host_ui_bg(), 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 12, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page, 12, 0);
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
    lv_label_set_text(title, "  BLE Spam");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_purple(), 0);

    lv_obj_t *trow = lv_obj_create(s_page);
    lv_obj_set_size(trow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(trow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(trow, 0, 0);
    lv_obj_set_style_pad_all(trow, 4, 0);
    lv_obj_set_style_pad_column(trow, 10, 0);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < NTYPES; i++) {
        lv_obj_t *b = lv_btn_create(trow);
        lv_obj_set_size(b, 130, 56);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_type, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, TYPES[i]); lv_obj_center(l);
        s_type_btns[i] = b;
        style_type_btn(i);
    }

    s_big_btn = lv_btn_create(s_page);
    lv_obj_set_size(s_big_btn, 320, 80);
    lv_obj_set_style_radius(s_big_btn, 16, 0);
    lv_obj_add_event_cb(s_big_btn, on_big, LV_EVENT_CLICKED, NULL);
    s_big_lbl = lv_label_create(s_big_btn);
    lv_obj_set_style_text_font(s_big_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_big_lbl);
    refresh_big();

    s_status = lv_label_create(s_page);
    lv_label_set_text(s_status, "Idle");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status, subghz_host_ui_muted(), 0);

    s_tab_id = subghz_host_current_tab();
    s_alive = true; s_running = false; s_dirty = false; s_sent = 0;
    xTaskCreate(reader_task, "ble_spam_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 200, NULL);
    subghz_host_uart_flush_input(s_tab_id);
    ESP_LOGI(TAG, "BLE spam page ready");
}
