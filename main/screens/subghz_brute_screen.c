/* Sub-GHz (CC1101) brute-force screen. Sends subghz_brute and paints
 * [SUBGHZ_BRUTE_START] / [SUBGHZ_BRUTE] / [SUBGHZ_BRUTE_DONE] progress into a
 * live progress bar with a proto/code/total readout. Reachable from the Radios
 * submenu. */
#include "subghz_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "subghz_brute";
#define RXBUF 1024
#define LINEBUF 1024

static lv_obj_t   *s_page = NULL;
static lv_obj_t   *s_info_lbl = NULL;
static lv_obj_t   *s_bar = NULL;
static lv_obj_t   *s_status_lbl = NULL;
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_alive = false;
static int s_tab_id = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Shared state (written by reader task, read by UI tick) */
static char          s_proto[32] = {0};
static int           s_bits = 0;
static unsigned long s_total = 0;
static unsigned long s_code = 0;
static float         s_freq = 0.0f;
static volatile bool s_done = false;
static volatile bool s_dirty = false;

static void process_line(const char *line)
{
    const char *p;
    if ((p = strstr(line, "[SUBGHZ_BRUTE_START] "))) {
        char proto[32] = {0};
        int bits = 0;
        unsigned long total = 0;
        float freq = 0.0f;
        sscanf(p, "[SUBGHZ_BRUTE_START] proto=%31s bits=%d total=%lu freq=%f",
               proto, &bits, &total, &freq);
        portENTER_CRITICAL(&s_lock);
        strncpy(s_proto, proto, sizeof(s_proto) - 1);
        s_proto[sizeof(s_proto) - 1] = '\0';
        s_bits = bits; s_total = total; s_freq = freq;
        s_code = 0; s_done = false; s_dirty = true;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if ((p = strstr(line, "[SUBGHZ_BRUTE] "))) {
        unsigned long code = 0, total = 0;
        sscanf(p, "[SUBGHZ_BRUTE] code=%lu total=%lu", &code, &total);
        portENTER_CRITICAL(&s_lock);
        s_code = code;
        if (total) s_total = total;
        s_dirty = true;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strstr(line, "[SUBGHZ_BRUTE_DONE]")) {
        portENTER_CRITICAL(&s_lock);
        s_done = true; s_dirty = true;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
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
            if (c == '\n' || c == '\r') {
                if (lp > 0) { line[lp] = '\0'; process_line(line); lp = 0; }
            } else if (lp < LINEBUF - 1) line[lp++] = c;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_dirty) return;
    char proto[32];
    unsigned long total, code;
    float freq;
    bool done;
    portENTER_CRITICAL(&s_lock);
    memcpy(proto, s_proto, sizeof(proto));
    total = s_total; code = s_code; freq = s_freq; done = s_done;
    s_dirty = false;
    portEXIT_CRITICAL(&s_lock);

    if (s_bar) {
        int32_t v = 0;
        if (total > 0) {
            v = (int32_t)((code * 1000UL) / total);
            if (v > 1000) v = 1000;
        }
        if (done) v = 1000;
        lv_bar_set_value(s_bar, v, LV_ANIM_OFF);
    }
    if (s_info_lbl) {
        lv_label_set_text_fmt(s_info_lbl, "%s  %lu / %lu  (%.2f MHz)",
                              proto[0] ? proto : "?", code, total, freq);
    }
    if (s_status_lbl) {
        if (done) {
            lv_label_set_text(s_status_lbl, "Done");
            lv_obj_set_style_text_color(s_status_lbl, subghz_host_color_green(), 0);
        } else {
            lv_label_set_text(s_status_lbl, "Running...");
            lv_obj_set_style_text_color(s_status_lbl, subghz_host_ui_muted(), 0);
        }
    }
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("subghz_stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_info_lbl = NULL;
    s_bar = NULL;
    s_status_lbl = NULL;
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }

void show_subghz_brute_page(void)
{
    lv_obj_t *container = subghz_host_current_container();
    if (!container) return;
    subghz_host_hide_all_pages();
    cleanup();   /* tear down any prior instance (timer/task/PSRAM) on re-entry */

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
    lv_label_set_text(title, "  Sub-GHz Brute-Force");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_pink(), 0);

    s_info_lbl = lv_label_create(s_page);
    lv_label_set_text(s_info_lbl, "Waiting...");
    lv_obj_set_style_text_font(s_info_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_info_lbl, subghz_host_ui_text(), 0);

    s_bar = lv_bar_create(s_page);
    lv_obj_set_size(s_bar, lv_pct(100), 24);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_status_lbl = lv_label_create(s_page);
    lv_label_set_text(s_status_lbl, "Running...");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status_lbl, subghz_host_ui_muted(), 0);

    s_tab_id = subghz_host_current_tab();
    s_alive = true;
    s_dirty = false; s_done = false;
    s_proto[0] = '\0'; s_bits = 0; s_total = 0; s_code = 0; s_freq = 0.0f;
    xTaskCreate(reader_task, "subghz_brute_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 120, NULL);

    subghz_host_uart_flush_input(s_tab_id);
    subghz_host_uart_send("subghz_brute nice bits=12 reps=2");
    ESP_LOGI(TAG, "Sub-GHz brute-force page ready");
}
