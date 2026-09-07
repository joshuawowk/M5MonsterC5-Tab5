/* Sub-GHz (CC1101) jamming detector screen. Sends subghz_jamdet and paints
 * [SUBGHZ_JAMDET_START] / [SUBGHZ_JAMDET] state into a big colour-coded state
 * label (clear/activity/jammed) with a duty/avg/floor readout. Reachable from
 * the Radios submenu. */
#include "subghz_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "subghz_jamdet";
#define RXBUF 1024
#define LINEBUF 1024

static lv_obj_t   *s_page = NULL;
static lv_obj_t   *s_freq_lbl = NULL;
static lv_obj_t   *s_state_lbl = NULL;
static lv_obj_t   *s_info_lbl = NULL;
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_alive = false;
static int s_tab_id = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Shared state (written by reader task, read by UI tick) */
static char          s_state[16] = {0};
static int           s_duty = 0;
static int           s_avg = 0;
static int           s_floor = 0;
static float         s_freq = 0.0f;
static volatile bool s_dirty = false;

static void process_line(const char *line)
{
    const char *p;
    if ((p = strstr(line, "[SUBGHZ_JAMDET_START] "))) {
        float freq = 0.0f;
        sscanf(p, "[SUBGHZ_JAMDET_START] freq=%f", &freq);
        portENTER_CRITICAL(&s_lock);
        s_freq = freq; s_dirty = true;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if ((p = strstr(line, "[SUBGHZ_JAMDET] "))) {
        char state[16] = {0};
        int duty = 0, avg = 0, floor = 0;
        sscanf(p, "[SUBGHZ_JAMDET] state=%15s duty=%d avg=%d floor=%d",
               state, &duty, &avg, &floor);
        portENTER_CRITICAL(&s_lock);
        strncpy(s_state, state, sizeof(s_state) - 1);
        s_state[sizeof(s_state) - 1] = '\0';
        s_duty = duty; s_avg = avg; s_floor = floor;
        s_dirty = true;
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
    char state[16];
    int duty, avg, floor;
    float freq;
    portENTER_CRITICAL(&s_lock);
    memcpy(state, s_state, sizeof(state));
    duty = s_duty; avg = s_avg; floor = s_floor; freq = s_freq;
    s_dirty = false;
    portEXIT_CRITICAL(&s_lock);

    if (s_freq_lbl) {
        lv_label_set_text_fmt(s_freq_lbl, "%.2f MHz", freq);
    }
    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, state[0] ? state : "--");
        lv_color_t col;
        if (strcmp(state, "clear") == 0)         col = subghz_host_color_green();
        else if (strcmp(state, "activity") == 0) col = subghz_host_color_amber();
        else if (strcmp(state, "jammed") == 0)   col = subghz_host_color_red();
        else                                     col = subghz_host_ui_muted();
        lv_obj_set_style_text_color(s_state_lbl, col, 0);
    }
    if (s_info_lbl) {
        lv_label_set_text_fmt(s_info_lbl, "duty %d%%  avg %d%%  floor %d dBm",
                              duty, avg, floor);
    }
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("subghz_stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_freq_lbl = NULL;
    s_state_lbl = NULL;
    s_info_lbl = NULL;
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }

void show_subghz_jamdet_page(void)
{
    lv_obj_t *container = subghz_host_current_container();
    if (!container) return;
    subghz_host_hide_all_pages();
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }

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
    lv_label_set_text(title, "  Sub-GHz Jamming Detector");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_cyan(), 0);

    s_freq_lbl = lv_label_create(s_page);
    lv_label_set_text(s_freq_lbl, "-- MHz");
    lv_obj_set_style_text_font(s_freq_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_freq_lbl, subghz_host_ui_muted(), 0);

    s_state_lbl = lv_label_create(s_page);
    lv_label_set_text(s_state_lbl, "--");
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_state_lbl, subghz_host_ui_muted(), 0);

    s_info_lbl = lv_label_create(s_page);
    lv_label_set_text(s_info_lbl, "duty --%  avg --%  floor -- dBm");
    lv_obj_set_style_text_font(s_info_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info_lbl, subghz_host_ui_text(), 0);

    s_tab_id = subghz_host_current_tab();
    s_alive = true;
    s_dirty = false;
    s_state[0] = '\0'; s_duty = 0; s_avg = 0; s_floor = 0; s_freq = 0.0f;
    xTaskCreate(reader_task, "subghz_jamdet_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 120, NULL);

    subghz_host_uart_flush_input(s_tab_id);
    subghz_host_uart_send("subghz_jamdet 433.92");
    ESP_LOGI(TAG, "Sub-GHz jamming detector page ready");
}
